/*
 * dsound.c - DirectSound 8 sobre o áudio do SDL2 (Fase 4, etapa 4.3).
 *
 * O DirectSound é o motor de som inteiro do RE5: um buffer primário (o jogo
 * escolhe 48 kHz e 2 ou 6 canais pela configuração de alto-falantes) e
 * buffers secundários que ele mesmo enche e toca (IDirectSoundBuffer8).
 *
 * Modelo:
 *   - objetos COM no guest como nas outras pontes: [vtable][handle]['DSND'][tipo],
 *     vtables de thunks stdcall; método sem handler aborta com o nome (D8);
 *   - a memória de cada buffer secundário mora no guest: Lock devolve ponteiros
 *     direto para ela (até dois trechos, quando a região dá a volta), o jogo
 *     escreve, e o mixer lê dali - como o hardware lê a memória do buffer;
 *   - o mixer roda na thread de áudio do SDL (48 kHz, estéreo, float): para cada
 *     buffer tocando, reamostragem linear da frequência do buffer para 48 kHz,
 *     volume e pan em centésimos de dB como o DirectSound, downmix de 5.1 para
 *     estéreo; o cursor de reprodução anda com o que foi consumido, e o de
 *     escrita fica 10 ms à frente;
 *   - DuplicateSoundBuffer compartilha os dados (cursor, volume e pan próprios);
 *   - o host não tem saída de áudio (ou RE5_HEADLESS): DirectSoundCreate8 abre
 *     sem dispositivo e o mixer só anda quando alguém o chama (testes). Sem
 *     subsistema de áudio no SDL: DSERR_NODRIVER, como um PC sem placa.
 *
 * O estado que o mixer lê é protegido por um mutex do host; os handlers rodam
 * com o lock do guest.
 *
 * Diagnóstico: RE5_DSOUND_DUMP=<pasta> grava a mistura final (mix_f32.raw,
 * float estéreo a 48 kHz) e, de cada buffer secundário, os bytes que o mixer
 * consumiu, na ordem tocada (buf_<n>_<Hz>hz_<canais>ch_<bits>b.raw), por até
 * RE5_DSOUND_DUMP_SECS segundos de mistura (padrão 60). Separa "o jogo entrega
 * o som estragado" (tradução) de "o mixer estraga".
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "re5/dsound.h"
#include "kernel32_internal.h"
#include "../core/host_sync.h"

#define DS_OK                  0x00000000u
#define DSERR_ALLOCATED        0x8878000Au
#define DSERR_CONTROLUNAVAIL   0x8878001Eu
#define DSERR_INVALIDPARAM     0x80070057u
#define DSERR_INVALIDCALL      0x88780032u
#define DSERR_BADFORMAT        0x88780064u
#define DSERR_NODRIVER         0x88780078u
#define DSERR_ALREADYINITIALIZED 0x88780082u
#define DSERR_OUTOFMEMORY      0x8007000Eu
#define E_POINTER_             0x80004003u

#define DSBCAPS_PRIMARYBUFFER      0x00000001u
#define DSBCAPS_CTRL3D             0x00000010u
#define DSBCAPS_CTRLFREQUENCY      0x00000020u
#define DSBCAPS_CTRLPAN            0x00000040u
#define DSBCAPS_CTRLVOLUME         0x00000080u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100u
#define DSBCAPS_CTRLFX             0x00000200u
#define DSBCAPS_LOCSOFTWARE        0x00000008u

#define DSBPLAY_LOOPING      0x1u
#define DSBSTATUS_PLAYING    0x1u
#define DSBSTATUS_LOOPING    0x4u
#define DSBLOCK_FROMWRITECURSOR 0x1u
#define DSBLOCK_ENTIREBUFFER    0x2u

#define DS_MAGIC 0x444E5344u   /* 'DSND' */

/* ---- objetos ---- */

typedef struct {
    uint32_t va, size, refs;   /* memória do buffer no guest, compartilhada pelos duplicados */
} ds_data;

typedef struct ds_buf {
    int       primary;
    uint32_t  flags;
    ds_data  *data;
    uint16_t  tag, channels, block, bits;
    uint32_t  rate, freq;      /* freq: a atual (SetFrequency); rate: a do formato */
    int32_t   volume, pan;
    double    pos;             /* em quadros, com fração da reamostragem */
    int       playing, looping;
    uint32_t  fmt_cb;
    uint8_t   fmt[40];         /* WAVEFORMATEX(TENSIBLE) como o guest deu (GetFormat) */
    uint32_t  guest, handle, refs;
    uint32_t  id;              /* ordem de criação (RE5_DSOUND_DUMP) */
    FILE     *dump;
    struct ds_buf *next;       /* lista do mixer */
} ds_buf;

typedef struct {
    uint32_t guest, handle, refs, coop, speaker;
    ds_buf  *primary;
} ds_dev;

static re5_mutex g_mu;
static int g_mu_ready;
static ds_buf *g_bufs;
static double g_master = 1.0;
static SDL_AudioDeviceID g_audio;
static uint32_t g_vt[2];   /* IDirectSound8, IDirectSoundBuffer8 */

/* ---- RE5_DSOUND_DUMP ---- */
static int g_dump_on = -1;
static char g_dump_dir[512];
static FILE *g_dump_mix;
static uint64_t g_dump_left;   /* quadros de mistura que ainda cabem */
static uint32_t g_next_id;

static int dump_on(void)
{
    if (g_dump_on < 0) {
        const char *d = getenv("RE5_DSOUND_DUMP"), *s = getenv("RE5_DSOUND_DUMP_SECS");
        g_dump_on = d && *d;
        if (g_dump_on) {
            snprintf(g_dump_dir, sizeof g_dump_dir, "%s", d);
            long secs = s ? strtol(s, NULL, 10) : 60;
            g_dump_left = (uint64_t)(secs > 0 ? secs : 60) * RE5_DSOUND_RATE;
            char path[640];
            snprintf(path, sizeof path, "%s/mix_f32.raw", g_dump_dir);
            g_dump_mix = fopen(path, "wb");
            if (!g_dump_mix) re5_fatal("dsound", "RE5_DSOUND_DUMP: não consegui criar %s", path);
            RE5_INFO("dsound", "RE5_DSOUND_DUMP: gravando o som em %s", g_dump_dir);
        }
    }
    return g_dump_on;
}

/* Linha de eventos (events.txt): tempo em µs do host e o que o guest pediu. */
static FILE *g_dump_ev;
static re5_mutex g_ev_mu;

static void ev(const char *fmt, ...) RE5_PRINTF(1, 2);
static void ev(const char *fmt, ...)
{
    if (!dump_on() || !g_dump_left) return;
    static int ready;
    if (!ready) {
        re5_mutex_init(&g_ev_mu);
        ready = 1;
    }
    re5_mutex_lock(&g_ev_mu);
    if (!g_dump_ev) {
        char path[640];
        snprintf(path, sizeof path, "%s/events.txt", g_dump_dir);
        g_dump_ev = fopen(path, "w");
    }
    if (g_dump_ev) {
        static uint64_t t0;
        uint64_t now = SDL_GetPerformanceCounter();
        if (!t0) t0 = now;
        fprintf(g_dump_ev, "%llu ", (unsigned long long)((now - t0) * 1000000ull / SDL_GetPerformanceFrequency()));
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_dump_ev, fmt, ap);
        va_end(ap);
        fputc('\n', g_dump_ev);
    }
    re5_mutex_unlock(&g_ev_mu);
}

/* Bytes do buffer consumidos pelo mixer de `from` até `to` quadros (com a volta). */
static void dump_buf(ds_buf *b, const uint8_t *mem, uint32_t from, uint32_t to, uint32_t total)
{
    if (!b->dump) {
        char path[640];
        snprintf(path, sizeof path, "%s/buf_%u_%uhz_%uch_%ub.raw", g_dump_dir, b->id, b->rate, b->channels, b->bits);
        b->dump = fopen(path, "wb");
        if (!b->dump) return;
    }
    if (to >= from) {
        fwrite(mem + (size_t)from * b->block, b->block, to - from, b->dump);
    } else {
        fwrite(mem + (size_t)from * b->block, b->block, total - from, b->dump);
        fwrite(mem, b->block, to, b->dump);
    }
}

static void mix_lock(void)
{
    if (!g_mu_ready) {
        re5_mutex_init(&g_mu);
        g_mu_ready = 1;
    }
    re5_mutex_lock(&g_mu);
}

static void mix_unlock(void) { re5_mutex_unlock(&g_mu); }

static uint32_t vtable_of(int buffer);

static uint32_t obj_new(void *host, int buffer, uint32_t *handle)
{
    uint32_t g = k32_heap_alloc(k32_process_heap(), 0, 16);
    if (!g) re5_fatal("dsound", "sem memória do guest");
    *handle = re5_handle_new(RE5_H_COM, host, NULL);
    k32_w32(g, vtable_of(buffer));
    k32_w32(g + 4, *handle);
    k32_w32(g + 8, DS_MAGIC);
    k32_w32(g + 12, (uint32_t)buffer);
    return g;
}

static void *obj_of(uint32_t va, int buffer)
{
    if (!va || !re5_mem_is_mapped(va, 16) || k32_r32(va + 8) != DS_MAGIC || k32_r32(va + 12) != (uint32_t)buffer)
        return NULL;
    return re5_handle_get(k32_r32(va + 4), RE5_H_COM);
}

static void obj_free(uint32_t guest, uint32_t handle)
{
    re5_handle_close(handle);
    k32_w32(guest + 8, 0);
    k32_heap_free(k32_process_heap(), guest);
}

static RE5_NORETURN void not_ours(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("dsound", "%s: this=0x%08X não é um objeto do DirectSound da ponte", t ? t->name : "?", re5_arg(c, 0));
}

static ds_dev *dev_self(CPU *c)
{
    ds_dev *d = obj_of(re5_arg(c, 0), 0);
    if (!d) not_ours(c);
    return d;
}

static ds_buf *buf_self(CPU *c)
{
    ds_buf *b = obj_of(re5_arg(c, 0), 1);
    if (!b) not_ours(c);
    return b;
}

static void unknown_method(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("dsound", "%s sem implementação (this=0x%08X)", t ? t->name : "?", re5_arg(c, 0));
}

/* ---- mixer ---- */

static double db_gain(int32_t hundredths) { return hundredths <= -10000 ? 0.0 : pow(10.0, hundredths / 2000.0); }

/* Amostra do quadro `f`, canal `ch`, em [-1, 1). */
static float sample_at(const ds_buf *b, const uint8_t *mem, uint32_t f, uint32_t ch)
{
    const uint8_t *p = mem + (size_t)f * b->block + (size_t)ch * (b->bits / 8u);
    if (b->bits == 8) return ((int)p[0] - 128) / 128.0f;
    return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
}

/* Quadro `f` em estéreo (downmix da ordem de canais do WAVE: FL FR FC LFE BL BR...). */
static void frame_lr(const ds_buf *b, const uint8_t *mem, uint32_t f, float *l, float *r)
{
    if (b->channels == 1) {
        *l = *r = sample_at(b, mem, f, 0);
        return;
    }
    float fl = sample_at(b, mem, f, 0), fr = sample_at(b, mem, f, 1);
    if (b->channels >= 3) {
        float fc = sample_at(b, mem, f, 2) * 0.7071f;
        fl += fc;
        fr += fc;
    }
    if (b->channels >= 6) {
        fl += sample_at(b, mem, f, 4) * 0.7071f;
        fr += sample_at(b, mem, f, 5) * 0.7071f;
    }
    *l = fl;
    *r = fr;
}

void re5_dsound_mix(float *out, uint32_t frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    int dumping = dump_on() && g_dump_left > 0;
    mix_lock();
    for (ds_buf *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->primary || !b->data) continue;
        const uint8_t *mem = re5_g2h(b->data->va);
        uint32_t total = b->data->size / b->block;
        if (!total) continue;
        uint32_t dump_from = (uint32_t)b->pos, ev_from = dump_from;
        double step = (double)(b->freq ? b->freq : b->rate) / RE5_DSOUND_RATE;
        double g = db_gain(b->volume) * g_master;
        double gl = g * (b->pan > 0 ? db_gain(-b->pan) : 1.0), gr = g * (b->pan < 0 ? db_gain(b->pan) : 1.0);
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f0 = (uint32_t)b->pos;
            double t = b->pos - f0;
            uint32_t f1 = f0 + 1 < total ? f0 + 1 : (b->looping ? 0 : f0);
            float l0, r0, l1, r1;
            frame_lr(b, mem, f0, &l0, &r0);
            frame_lr(b, mem, f1, &l1, &r1);
            out[2 * i] += (float)((l0 + (l1 - l0) * t) * gl);
            out[2 * i + 1] += (float)((r0 + (r1 - r0) * t) * gr);
            b->pos += step;
            if (b->pos >= total) {
                if (b->looping) {
                    b->pos -= total;
                } else {   /* fim de um buffer sem laço: para, cursor no começo */
                    if (dumping) dump_buf(b, mem, dump_from, total, total);
                    dump_from = 0;
                    b->pos = 0;
                    b->playing = 0;
                    break;
                }
            }
        }
        if (dumping && b->playing) dump_buf(b, mem, dump_from, (uint32_t)b->pos, total);
        if (dumping) ev("M %u %u %u", b->id, ev_from, (uint32_t)b->pos);
    }
    mix_unlock();
    for (uint32_t i = 0; i < 2 * frames; i++) out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
    if (dumping) {
        fwrite(out, 2 * sizeof *out, frames, g_dump_mix);
        g_dump_left = g_dump_left > frames ? g_dump_left - frames : 0;
        if (!g_dump_left) {
            fflush(NULL);
            RE5_INFO("dsound", "RE5_DSOUND_DUMP: fim da gravação");
        }
    }
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    re5_dsound_mix((float *)stream, (uint32_t)len / (2u * sizeof(float)));
}

/* ---- IDirectSoundBuffer8 ---- */

static void buf_link(ds_buf *b)
{
    mix_lock();
    b->next = g_bufs;
    g_bufs = b;
    mix_unlock();
}

static void buf_destroy(ds_buf *b)
{
    mix_lock();
    for (ds_buf **p = &g_bufs; *p; p = &(*p)->next) {
        if (*p == b) {
            *p = b->next;
            break;
        }
    }
    if (b->dump) fclose(b->dump);
    mix_unlock();
    if (b->data && --b->data->refs == 0) {
        k32_heap_free(k32_process_heap(), b->data->va);
        free(b->data);
    }
    obj_free(b->guest, b->handle);
    free(b);
}

static void b_AddRef(CPU *c) { c->eax = ++buf_self(c)->refs; }

static void b_Release(CPU *c)
{
    ds_buf *b = buf_self(c);
    c->eax = --b->refs;
    if (!b->refs && !b->primary) buf_destroy(b);   /* o primário é do device */
}

static const uint8_t k_iid_dsb[16] = {0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                      0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
static const uint8_t k_iid_dsb8[16] = {0x49, 0xA4, 0x25, 0x68, 0x24, 0x75, 0x82, 0x4D,
                                       0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E};
static const uint8_t k_iid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t k_iid_ds[16] = {0x83, 0x4F, 0x9D, 0x27, 0x60, 0x49, 0xCE, 0x11,
                                     0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
static const uint8_t k_iid_ds8[16] = {0x93, 0x7E, 0x0A, 0xC5, 0x95, 0xF3, 0x34, 0x48,
                                      0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66};

static int iid_is(uint32_t va, const uint8_t g[16]) { return va && memcmp(re5_g2h(va), g, 16) == 0; }

static void iid_fatal(uint32_t va, const char *who)
{
    const uint8_t *g = re5_g2h(va);
    re5_fatal("dsound", "%s::QueryInterface({%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}) "
              "sem implementação", who, g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11],
              g[12], g[13], g[14], g[15]);
}

static void b_QueryInterface(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t iid = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if (iid_is(iid, k_iid_unknown) || iid_is(iid, k_iid_dsb) || iid_is(iid, k_iid_dsb8)) {
        b->refs++;
        k32_w32(out, b->guest);
        c->eax = DS_OK;
        return;
    }
    iid_fatal(iid, "IDirectSoundBuffer8");
}

/* DSBCAPS: dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead */
static void b_GetCaps(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t p = re5_arg(c, 1);
    if (!p || k32_r32(p) < 20) { c->eax = DSERR_INVALIDPARAM; return; }
    k32_w32(p + 4, b->flags | (b->primary ? 0 : DSBCAPS_LOCSOFTWARE));
    k32_w32(p + 8, b->data ? b->data->size : 0);
    k32_w32(p + 12, 0);
    k32_w32(p + 16, 0);
    c->eax = DS_OK;
}

static uint32_t lead_bytes(const ds_buf *b)
{
    uint32_t r = b->freq ? b->freq : b->rate;
    return (r / 100u) * b->block;   /* 10 ms */
}

static void b_GetCurrentPosition(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t pp = re5_arg(c, 1), pw = re5_arg(c, 2);
    if (b->primary) {   /* o primário não tem memória visível: cursores em 0 */
        if (pp) k32_w32(pp, 0);
        if (pw) k32_w32(pw, 0);
        c->eax = DS_OK;
        return;
    }
    mix_lock();
    uint32_t play = (uint32_t)b->pos * b->block, size = b->data->size;
    uint32_t write = b->playing ? (play + lead_bytes(b)) % size : play;
    mix_unlock();
    ev("P %u %u %u ret=%08X", b->id, play, write, rd32(c->esp));
    if (pp) k32_w32(pp, play);
    if (pw) k32_w32(pw, write);
    c->eax = DS_OK;
}

static void b_GetFormat(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t p = re5_arg(c, 1), cb = re5_arg(c, 2), pw = re5_arg(c, 3);
    if (!p) {
        if (pw) k32_w32(pw, b->fmt_cb);
        c->eax = pw ? DS_OK : DSERR_INVALIDPARAM;
        return;
    }
    uint32_t n = cb < b->fmt_cb ? cb : b->fmt_cb;
    memcpy(re5_g2h(p), b->fmt, n);
    if (pw) k32_w32(pw, n);
    c->eax = DS_OK;
}

static void b_GetVolume(CPU *c)
{
    ds_buf *b = buf_self(c);
    if (re5_arg(c, 1)) k32_w32(re5_arg(c, 1), (uint32_t)b->volume);
    c->eax = re5_arg(c, 1) ? DS_OK : DSERR_INVALIDPARAM;
}

static void b_GetPan(CPU *c)
{
    ds_buf *b = buf_self(c);
    if (re5_arg(c, 1)) k32_w32(re5_arg(c, 1), (uint32_t)b->pan);
    c->eax = re5_arg(c, 1) ? DS_OK : DSERR_INVALIDPARAM;
}

static void b_GetFrequency(CPU *c)
{
    ds_buf *b = buf_self(c);
    if (re5_arg(c, 1)) k32_w32(re5_arg(c, 1), b->freq ? b->freq : b->rate);
    c->eax = re5_arg(c, 1) ? DS_OK : DSERR_INVALIDPARAM;
}

static void b_GetStatus(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t p = re5_arg(c, 1);
    if (!p) { c->eax = DSERR_INVALIDPARAM; return; }
    mix_lock();
    uint32_t s = b->playing ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0u) : 0u;
    mix_unlock();
    k32_w32(p, s);
    c->eax = DS_OK;
}

static void b_Initialize(CPU *c) { buf_self(c); c->eax = DSERR_ALREADYINITIALIZED; }

/* Lock(this, dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags) */
static void b_Lock(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t off = re5_arg(c, 1), n = re5_arg(c, 2), pp1 = re5_arg(c, 3), pn1 = re5_arg(c, 4);
    uint32_t pp2 = re5_arg(c, 5), pn2 = re5_arg(c, 6), flags = re5_arg(c, 7);
    if (b->primary) re5_fatal("dsound", "Lock no buffer primário (DSSCL_WRITEPRIMARY) sem implementação");
    uint32_t size = b->data->size;
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        mix_lock();
        off = b->playing ? ((uint32_t)b->pos * b->block + lead_bytes(b)) % size : (uint32_t)b->pos * b->block;
        mix_unlock();
    }
    if (flags & DSBLOCK_ENTIREBUFFER) n = size;
    if (!pp1 || !pn1 || off >= size || !n || n > size) { c->eax = DSERR_INVALIDPARAM; return; }
    uint32_t n1 = n < size - off ? n : size - off, n2 = n - n1;
    if (!pp2) n2 = 0;   /* sem segundo ponteiro a trava fica no primeiro trecho */
    k32_w32(pp1, b->data->va + off);
    k32_w32(pn1, n1);
    if (pp2) k32_w32(pp2, n2 ? b->data->va : 0);
    if (pn2) k32_w32(pn2, n2);
    ev("L %u %u %u fl=%u %u %u ret=%08X", b->id, off, n, flags, n1, n2, rd32(c->esp));
    c->eax = DS_OK;
}

static void b_Unlock(CPU *c)   /* a memória já é a do buffer */
{
    ds_buf *b = buf_self(c);
    if (b->data)
        ev("U %u %u %u %u %u", b->id, re5_arg(c, 1) - b->data->va, re5_arg(c, 2),
           re5_arg(c, 3) ? re5_arg(c, 3) - b->data->va : 0u, re5_arg(c, 4));
    c->eax = DS_OK;
}

static void b_Play(CPU *c)
{
    ds_buf *b = buf_self(c);
    mix_lock();
    b->playing = 1;
    b->looping = (re5_arg(c, 3) & DSBPLAY_LOOPING) != 0;
    mix_unlock();
    ev("PLAY %u %u", b->id, re5_arg(c, 3));
    c->eax = DS_OK;
}

static void b_SetCurrentPosition(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t p = re5_arg(c, 1);
    if (b->primary || p >= b->data->size) { c->eax = DSERR_INVALIDPARAM; return; }
    mix_lock();
    b->pos = p / b->block;
    mix_unlock();
    ev("SETPOS %u %u", b->id, p);
    c->eax = DS_OK;
}

static int read_format(uint32_t va, ds_buf *b)
{
    /* WAVEFORMATEX: wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec, nBlockAlign, wBitsPerSample, cbSize */
    uint16_t tag = (uint16_t)rd16(va), ch = (uint16_t)rd16(va + 2), block = (uint16_t)rd16(va + 12);
    uint16_t bits = (uint16_t)rd16(va + 14), extra = tag == 1 ? 0 : (uint16_t)rd16(va + 16);
    uint32_t rate = k32_r32(va + 4);
    if (tag == 0xFFFEu) {   /* WAVEFORMATEXTENSIBLE: só com subformato PCM */
        if (extra < 22 || k32_r32(va + 24) != 1u) return -1;
    } else if (tag != 1) {
        return -1;
    }
    if (!ch || ch > 8 || (bits != 8 && bits != 16) || block != ch * bits / 8 || rate < 100 || rate > 200000) return -1;
    b->tag = tag;
    b->channels = ch;
    b->block = block;
    b->bits = bits;
    b->rate = rate;
    b->fmt_cb = 18u + extra > sizeof b->fmt ? (uint32_t)sizeof b->fmt : 18u + extra;
    memcpy(b->fmt, re5_g2h(va), b->fmt_cb);
    return 0;
}

static void b_SetFormat(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t fmt = re5_arg(c, 1);
    if (!b->primary) { c->eax = DSERR_INVALIDCALL; return; }
    if (!fmt || read_format(fmt, b) != 0) { c->eax = DSERR_BADFORMAT; return; }
    RE5_INFO("dsound", "buffer primário: %u Hz, %u canais, %u bits (saída do host: %u Hz estéreo)", b->rate,
             b->channels, b->bits, RE5_DSOUND_RATE);
    c->eax = DS_OK;
}

static void b_SetVolume(CPU *c)
{
    ds_buf *b = buf_self(c);
    int32_t v = (int32_t)re5_arg(c, 1);
    if (v > 0 || v < -10000) { c->eax = DSERR_INVALIDPARAM; return; }
    if (!(b->flags & DSBCAPS_CTRLVOLUME) && !b->primary) { c->eax = DSERR_CONTROLUNAVAIL; return; }
    mix_lock();
    b->volume = v;
    if (b->primary) g_master = db_gain(v);
    mix_unlock();
    c->eax = DS_OK;
}

static void b_SetPan(CPU *c)
{
    ds_buf *b = buf_self(c);
    int32_t v = (int32_t)re5_arg(c, 1);
    if (v > 10000 || v < -10000) { c->eax = DSERR_INVALIDPARAM; return; }
    if (!(b->flags & DSBCAPS_CTRLPAN)) { c->eax = DSERR_CONTROLUNAVAIL; return; }
    mix_lock();
    b->pan = v;
    mix_unlock();
    c->eax = DS_OK;
}

static void b_SetFrequency(CPU *c)
{
    ds_buf *b = buf_self(c);
    uint32_t f = re5_arg(c, 1);
    if (f && (f < 100 || f > 200000)) { c->eax = DSERR_INVALIDPARAM; return; }
    if (!(b->flags & DSBCAPS_CTRLFREQUENCY) || b->primary) { c->eax = DSERR_CONTROLUNAVAIL; return; }
    mix_lock();
    b->freq = f;   /* 0: a frequência do formato */
    mix_unlock();
    c->eax = DS_OK;
}

static void b_Stop(CPU *c)
{
    ds_buf *b = buf_self(c);
    mix_lock();
    b->playing = 0;
    mix_unlock();
    ev("STOP %u", b->id);
    c->eax = DS_OK;
}

static void b_Restore(CPU *c) { buf_self(c); c->eax = DS_OK; }   /* a memória nunca se perde */

static void b_SetFX(CPU *c)
{
    buf_self(c);
    if (re5_arg(c, 1)) re5_fatal("dsound", "IDirectSoundBuffer8::SetFX com efeitos sem implementação");
    c->eax = DS_OK;   /* nenhum efeito: é o que "tirar os efeitos" faz */
}

static void b_AcquireResources(CPU *c) { buf_self(c); c->eax = DS_OK; }   /* tudo é software */

/* ---- IDirectSound8 ---- */

static void d_QueryInterface(CPU *c)
{
    ds_dev *d = dev_self(c);
    uint32_t iid = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if (iid_is(iid, k_iid_unknown) || iid_is(iid, k_iid_ds) || iid_is(iid, k_iid_ds8)) {
        d->refs++;
        k32_w32(out, d->guest);
        c->eax = DS_OK;
        return;
    }
    iid_fatal(iid, "IDirectSound8");
}

static void d_AddRef(CPU *c) { c->eax = ++dev_self(c)->refs; }

static void d_Release(CPU *c)
{
    ds_dev *d = dev_self(c);
    c->eax = --d->refs;
    if (d->refs) return;
    if (d->primary) {
        mix_lock();
        for (ds_buf **p = &g_bufs; *p; p = &(*p)->next) {
            if (*p == d->primary) {
                *p = d->primary->next;
                break;
            }
        }
        mix_unlock();
        obj_free(d->primary->guest, d->primary->handle);
        free(d->primary);
    }
    obj_free(d->guest, d->handle);
    free(d);
}

/* CreateSoundBuffer(this, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN) */
static void d_CreateSoundBuffer(CPU *c)
{
    ds_dev *d = dev_self(c);
    uint32_t desc = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!desc || !out) { c->eax = DSERR_INVALIDPARAM; return; }
    if (re5_arg(c, 3)) re5_fatal("dsound", "CreateSoundBuffer com agregação sem implementação");
    k32_w32(out, 0);
    /* DSBUFFERDESC: dwSize (0x24, ou 0x14 do DirectX 7), dwFlags, dwBufferBytes, dwReserved, lpwfxFormat */
    uint32_t size = k32_r32(desc), flags = k32_r32(desc + 4), bytes = k32_r32(desc + 8), fmt = k32_r32(desc + 16);
    if (size != 0x24 && size != 0x14) { c->eax = DSERR_INVALIDPARAM; return; }
    if (flags & (DSBCAPS_CTRL3D | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLFX))
        re5_fatal("dsound", "CreateSoundBuffer(flags 0x%X): 3D/notificação/efeitos sem implementação", flags);
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (bytes || fmt) { c->eax = DSERR_INVALIDPARAM; return; }
        if (!d->primary) {
            ds_buf *p = calloc(1, sizeof *p);
            if (!p) re5_fatal("dsound", "sem memória");
            p->primary = 1;
            p->flags = flags;
            p->tag = 1;
            p->channels = 2;
            p->bits = 16;
            p->block = 4;
            p->rate = 22050;   /* o padrão do DirectSound até o SetFormat */
            p->guest = obj_new(p, 1, &p->handle);
            d->primary = p;
            buf_link(p);
        }
        d->primary->refs++;
        k32_w32(out, d->primary->guest);
        c->eax = DS_OK;
        return;
    }
    ds_buf *b = calloc(1, sizeof *b);
    if (!b) re5_fatal("dsound", "sem memória");
    if (!fmt || read_format(fmt, b) != 0) {
        free(b);
        c->eax = DSERR_BADFORMAT;
        return;
    }
    if (bytes < 4 || bytes > 0x0FFFFFFFu) {
        free(b);
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    bytes -= bytes % b->block;
    b->data = calloc(1, sizeof *b->data);
    if (!b->data) re5_fatal("dsound", "sem memória");
    b->data->size = bytes;
    b->data->refs = 1;
    b->data->va = k32_heap_alloc(k32_process_heap(), 0, bytes);
    if (!b->data->va) {
        free(b->data);
        free(b);
        c->eax = DSERR_OUTOFMEMORY;
        return;
    }
    memset(re5_g2h(b->data->va), b->bits == 8 ? 0x80 : 0, bytes);   /* silêncio */
    b->flags = flags;
    b->refs = 1;
    b->guest = obj_new(b, 1, &b->handle);
    buf_link(b);
    k32_w32(out, b->guest);
    b->id = g_next_id++;
    if (dump_on())
        RE5_INFO("dsound", "buffer %u (0x%08X): %u bytes, %u Hz, %u canais, %u bits, flags 0x%X", b->id, b->guest, bytes,
                 b->rate, b->channels, b->bits, flags);
    else
        RE5_DEBUG("dsound", "buffer 0x%08X: %u bytes, %u Hz, %u canais, %u bits, flags 0x%X", b->guest, bytes, b->rate,
                  b->channels, b->bits, flags);
    c->eax = DS_OK;
}

/* DSCAPS (0x60 bytes) de uma placa "por software" que faz tudo. */
static void d_GetCaps(CPU *c)
{
    dev_self(c);
    uint32_t p = re5_arg(c, 1);
    if (!p || k32_r32(p) < 0x60) { c->eax = DSERR_INVALIDPARAM; return; }
    memset(re5_g2h(p + 4), 0, 0x60 - 4);
    k32_w32(p + 4, 0x00000F1Fu);   /* PRIMARY/SECONDARY MONO/STEREO/8BIT/16BIT, CONTINUOUSRATE */
    k32_w32(p + 8, 100);           /* dwMinSecondarySampleRate */
    k32_w32(p + 12, 200000);       /* dwMaxSecondarySampleRate */
    k32_w32(p + 16, 1);            /* dwPrimaryBuffers */
    c->eax = DS_OK;
}

static void d_DuplicateSoundBuffer(CPU *c)
{
    dev_self(c);
    ds_buf *o = obj_of(re5_arg(c, 1), 1);
    uint32_t out = re5_arg(c, 2);
    if (!o || !out || o->primary) { c->eax = DSERR_INVALIDPARAM; return; }
    ds_buf *b = malloc(sizeof *b);
    if (!b) re5_fatal("dsound", "sem memória");
    mix_lock();
    *b = *o;
    mix_unlock();
    b->data->refs++;
    b->pos = 0;
    b->playing = b->looping = 0;
    b->dump = NULL;
    b->id = g_next_id++;
    b->refs = 1;
    b->guest = obj_new(b, 1, &b->handle);
    buf_link(b);
    k32_w32(out, b->guest);
    c->eax = DS_OK;
}

static void d_SetCooperativeLevel(CPU *c)
{
    ds_dev *d = dev_self(c);
    d->coop = re5_arg(c, 2);
    c->eax = DS_OK;
}

static void d_Compact(CPU *c) { dev_self(c); c->eax = DS_OK; }

/* A saída do host é estéreo: o jogo escolhe 2 canais no primário. */
#define DSSPEAKER_STEREO 0x00000004u

static void d_GetSpeakerConfig(CPU *c)
{
    ds_dev *d = dev_self(c);
    if (!re5_arg(c, 1)) { c->eax = DSERR_INVALIDPARAM; return; }
    k32_w32(re5_arg(c, 1), d->speaker);
    c->eax = DS_OK;
}

static void d_SetSpeakerConfig(CPU *c)
{
    dev_self(c)->speaker = re5_arg(c, 1);
    c->eax = DS_OK;
}

static void d_Initialize(CPU *c) { dev_self(c); c->eax = DSERR_ALREADYINITIALIZED; }

static void d_VerifyCertification(CPU *c)
{
    dev_self(c);
    if (re5_arg(c, 1)) k32_w32(re5_arg(c, 1), 0);   /* DS_CERTIFIED */
    c->eax = re5_arg(c, 1) ? DS_OK : DSERR_INVALIDPARAM;
}

/* ---- vtables ---- */

/* nolock: sem o lock do HLE - só lêem ou mudam estado sob o mix_lock (a thread
 * de som do jogo chama GetStatus/GetCurrentPosition/Lock/Unlock sem parar e
 * esperava o lock global por isso; RE5_PROFILE, Fase 4). Os que mexem no heap
 * do guest ou em contagens de referência continuam com ele. */
typedef struct { const char *name; uint32_t bytes; re5_host_fn fn; int nolock; } ds_method;

static const ds_method k_dev[] = {
    {"QueryInterface", 12, d_QueryInterface}, {"AddRef", 4, d_AddRef}, {"Release", 4, d_Release},
    {"CreateSoundBuffer", 16, d_CreateSoundBuffer}, {"GetCaps", 8, d_GetCaps},
    {"DuplicateSoundBuffer", 12, d_DuplicateSoundBuffer}, {"SetCooperativeLevel", 12, d_SetCooperativeLevel},
    {"Compact", 4, d_Compact}, {"GetSpeakerConfig", 8, d_GetSpeakerConfig},
    {"SetSpeakerConfig", 8, d_SetSpeakerConfig}, {"Initialize", 8, d_Initialize},
    {"VerifyCertification", 8, d_VerifyCertification},
};

static const ds_method k_buf[] = {
    {"QueryInterface", 12, b_QueryInterface}, {"AddRef", 4, b_AddRef}, {"Release", 4, b_Release},
    {"GetCaps", 8, b_GetCaps, 1}, {"GetCurrentPosition", 12, b_GetCurrentPosition, 1}, {"GetFormat", 16, b_GetFormat, 1},
    {"GetVolume", 8, b_GetVolume, 1}, {"GetPan", 8, b_GetPan, 1}, {"GetFrequency", 8, b_GetFrequency, 1},
    {"GetStatus", 8, b_GetStatus, 1}, {"Initialize", 12, b_Initialize}, {"Lock", 32, b_Lock, 1}, {"Play", 16, b_Play, 1},
    {"SetCurrentPosition", 8, b_SetCurrentPosition, 1}, {"SetFormat", 8, b_SetFormat}, {"SetVolume", 8, b_SetVolume, 1},
    {"SetPan", 8, b_SetPan, 1}, {"SetFrequency", 8, b_SetFrequency, 1}, {"Stop", 4, b_Stop, 1}, {"Unlock", 20, b_Unlock, 1},
    {"Restore", 4, b_Restore}, {"SetFX", 16, b_SetFX}, {"AcquireResources", 16, b_AcquireResources},
    {"GetObjectInPath", 24, NULL},
};

static uint32_t vtable_of(int buffer)
{
    if (g_vt[buffer]) return g_vt[buffer];
    const ds_method *m = buffer ? k_buf : k_dev;
    uint32_t n = buffer ? (uint32_t)(sizeof k_buf / sizeof k_buf[0]) : (uint32_t)(sizeof k_dev / sizeof k_dev[0]);
    uint32_t va = k32_heap_alloc(k32_process_heap(), 0, 4u * n);
    if (!va) re5_fatal("dsound", "sem memória do guest");
    for (uint32_t i = 0; i < n; i++) {
        char *name = malloc(64);
        if (!name) re5_fatal("dsound", "sem memória");
        snprintf(name, 64, "%s::%s", buffer ? "IDirectSoundBuffer8" : "IDirectSound8", m[i].name);
        uint32_t th = re5_thunk_new(name, m[i].fn ? m[i].fn : unknown_method, m[i].bytes, NULL);
        if (m[i].nolock) re5_thunk_set_flags(th, RE5_THUNK_NOLOCK);
        k32_w32(va + 4u * i, th);
    }
    return g_vt[buffer] = va;
}

/* ---- DirectSoundCreate8 ---- */

static int open_output(void)
{
    if (g_audio || getenv("RE5_HEADLESS")) return 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        RE5_WARN("dsound", "SDL_INIT_AUDIO falhou (%s): sem placa de som", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = RE5_DSOUND_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_cb;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_audio) {
        RE5_WARN("dsound", "nenhuma saída de áudio no host (%s): sem placa de som", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(g_audio, 0);
    RE5_INFO("dsound", "saída de áudio do host: %d Hz, %d canais, %d quadros por bloco", have.freq, have.channels,
             have.samples);
    return 0;
}

static void s_DirectSoundCreate8(CPU *c)
{
    uint32_t out = re5_arg(c, 1);
    if (!out) { c->eax = DSERR_INVALIDPARAM; return; }
    if (re5_arg(c, 2)) re5_fatal("dsound", "DirectSoundCreate8 com agregação sem implementação");
    k32_w32(out, 0);
    if (open_output() != 0) { c->eax = DSERR_NODRIVER; return; }
    ds_dev *d = calloc(1, sizeof *d);
    if (!d) re5_fatal("dsound", "sem memória");
    d->refs = 1;
    d->speaker = DSSPEAKER_STEREO;
    d->guest = obj_new(d, 0, &d->handle);
    k32_w32(out, d->guest);
    c->eax = DS_OK;
}

static const re5_import_def k_defs[] = {
    {"DirectSoundCreate8", s_DirectSoundCreate8, 12, 0},
    {0},
};

const re5_import_module re5_mod_dsound = {"dsound.dll", k_defs};
