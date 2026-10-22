/*
 * DirectSound 8 sem dispositivo (RE5_HEADLESS): o teste chama o mixer à mão.
 * Cobre DirectSoundCreate8, GetCaps (os bits que o RE5 exige), GetSpeakerConfig,
 * o primário (SetFormat/GetFormat), um secundário (Lock com volta, Play em laço,
 * reamostragem 22050 -> 48000, volume e pan em dB, GetCurrentPosition andando
 * com o mixer), fim de um buffer sem laço, DuplicateSoundBuffer e QI do
 * IDirectSoundBuffer8.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/dsound.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;

static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *args)
{
    uint32_t a[16];
    a[0] = obj;
    for (int i = 0; i < n; i++) a[i + 1] = args[i];
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, rd32(rd32(obj) + 4 * slot), n + 1, a);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

/* IDirectSoundBuffer8 */
enum { B_QI, B_ADDREF, B_RELEASE, B_GETCAPS, B_GETPOS, B_GETFMT, B_GETVOL, B_GETPAN, B_GETFREQ, B_GETSTATUS,
       B_INIT, B_LOCK, B_PLAY, B_SETPOS, B_SETFMT, B_SETVOL, B_SETPAN, B_SETFREQ, B_STOP, B_UNLOCK };

int main(void)
{
#if defined(_WIN32)
    _putenv("RE5_HEADLESS=1");
#else
    setenv("RE5_HEADLESS", "1", 1);
#endif
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    uint32_t m = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    uint32_t pds = m, desc = m + 0x10, wfx = m + 0x40, pbuf = m + 0x80, caps = m + 0x100, v = m + 0x180;
    uint32_t p1 = m + 0x190, n1 = m + 0x194, p2 = m + 0x198, n2 = m + 0x19C, prim = m + 0x1A0, dup = m + 0x1A4;
    uint32_t iid = m + 0x1C0, pq = m + 0x1D0;

    CHECK_EQ_U32(re5_thunk_get(re5_import_bind("dsound.dll", "DirectSoundCreate8", 0))->arg_bytes, 12);
    const uint32_t cr[3] = {0, pds, 0};
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("dsound.dll", "DirectSoundCreate8", 0), 3, cr), 0);
    uint32_t ds = rd32(pds);
    CHECK(ds != 0);
    const uint32_t coop[2] = {0x10010u, 2 /* DSSCL_PRIORITY */};
    CHECK_EQ_U32(vcall(ds, 6, 2, coop), 0);
    wr32(caps, 0x60);
    CHECK_EQ_U32(vcall(ds, 4, 1, &caps), 0);
    CHECK_EQ_U32(rd32(caps + 4) & 0xB0Au, 0xB0Au);   /* o RE5 solta o DirectSound sem estes */
    CHECK_EQ_U32(vcall(ds, 8, 1, &v), 0);
    CHECK_EQ_U32(rd32(v), 4);                         /* DSSPEAKER_STEREO: o jogo usa 2 canais */

    /* Primário: 48 kHz estéreo 16 bits */
    memset(re5_g2h(desc), 0, 0x24);
    wr32(desc, 0x24);
    wr32(desc + 4, 1);   /* DSBCAPS_PRIMARYBUFFER */
    const uint32_t cb1[3] = {desc, prim, 0};
    CHECK_EQ_U32(vcall(ds, 3, 3, cb1), 0);
    uint32_t pb = rd32(prim);
    memset(re5_g2h(wfx), 0, 18);
    wr16(wfx, 1);
    wr16(wfx + 2, 2);
    wr32(wfx + 4, 48000);
    wr32(wfx + 8, 192000);
    wr16(wfx + 12, 4);
    wr16(wfx + 14, 16);
    CHECK_EQ_U32(vcall(pb, B_SETFMT, 1, &wfx), 0);

    /* Secundário: 22050 Hz mono 16 bits, 0,1 s (4410 bytes), volume/pan/frequência */
    wr16(wfx + 2, 1);
    wr32(wfx + 4, 22050);
    wr32(wfx + 8, 44100);
    wr16(wfx + 12, 2);
    wr32(desc + 4, 0x80u | 0x40u | 0x20u | 0x10000u);   /* CTRLVOLUME|CTRLPAN|CTRLFREQUENCY|GETCURRENTPOSITION2 */
    wr32(desc + 8, 4410);
    wr32(desc + 16, wfx);
    const uint32_t cb2[3] = {desc, pbuf, 0};
    CHECK_EQ_U32(vcall(ds, 3, 3, cb2), 0);
    uint32_t sb = rd32(pbuf);
    CHECK(sb != 0);

    const uint32_t lk[7] = {0, 0, p1, n1, p2, n2, 2 /* DSBLOCK_ENTIREBUFFER */};
    CHECK_EQ_U32(vcall(sb, B_LOCK, 7, lk), 0);
    CHECK_EQ_U32(rd32(n1), 4410);
    CHECK_EQ_U32(rd32(n2), 0);
    for (uint32_t i = 0; i < 2205; i++) wr16(rd32(p1) + 2 * i, 16384);   /* 0,5 constante */
    const uint32_t ul[4] = {rd32(p1), 4410, 0, 0};
    CHECK_EQ_U32(vcall(sb, B_UNLOCK, 4, ul), 0);
    const uint32_t lk2[7] = {4000, 1000, p1, n1, p2, n2, 0};   /* dá a volta: 410 + 590 */
    CHECK_EQ_U32(vcall(sb, B_LOCK, 7, lk2), 0);
    CHECK_EQ_U32(rd32(n1), 410);
    CHECK_EQ_U32(rd32(n2), 590);
    CHECK_EQ_U32(rd32(p2) + 4000, rd32(p1));

    const uint32_t play[3] = {0, 0, 1 /* DSBPLAY_LOOPING */};
    CHECK_EQ_U32(vcall(sb, B_PLAY, 3, play), 0);
    CHECK_EQ_U32(vcall(sb, B_GETSTATUS, 1, &v), 0);
    CHECK_EQ_U32(rd32(v), 5);   /* PLAYING | LOOPING */
    static float out[2 * 480];
    re5_dsound_mix(out, 480);   /* 10 ms a 48 kHz = 220,5 quadros do buffer */
    CHECK(near(out[0], 0.5f) && near(out[1], 0.5f) && near(out[2 * 479], 0.5f));
    const uint32_t gp[2] = {v, v + 4};
    CHECK_EQ_U32(vcall(sb, B_GETPOS, 2, gp), 0);
    CHECK_EQ_U32(rd32(v), 220 * 2);
    CHECK_EQ_U32(rd32(v + 4), 440 + 220 * 2);   /* escrita 10 ms à frente */

    uint32_t arg = (uint32_t)-600;               /* -6 dB */
    CHECK_EQ_U32(vcall(sb, B_SETVOL, 1, &arg), 0);
    arg = 10000;                                 /* tudo à direita */
    CHECK_EQ_U32(vcall(sb, B_SETPAN, 1, &arg), 0);
    re5_dsound_mix(out, 480);
    CHECK(near(out[0], 0.0f));
    CHECK(near(out[1], 0.5f * powf(10.0f, -0.3f)));
    arg = (uint32_t)-10001;
    CHECK_EQ_U32(vcall(sb, B_SETVOL, 1, &arg), 0x80070057u);   /* fora da faixa */

    /* Sem laço: termina, para e volta o cursor para o começo. */
    const uint32_t once[3] = {0, 0, 0};
    arg = 0;
    vcall(sb, B_SETPAN, 1, &arg);
    arg = 44100;   /* o dobro: o buffer de 0,1 s acaba em 50 ms */
    CHECK_EQ_U32(vcall(sb, B_SETFREQ, 1, &arg), 0);
    CHECK_EQ_U32(vcall(sb, B_PLAY, 3, once), 0);
    for (int i = 0; i < 6; i++) re5_dsound_mix(out, 480);
    CHECK_EQ_U32(vcall(sb, B_GETSTATUS, 1, &v), 0);
    CHECK_EQ_U32(rd32(v), 0);
    CHECK_EQ_U32(vcall(sb, B_GETFREQ, 1, &v), 0);
    CHECK_EQ_U32(rd32(v), 44100);

    /* DuplicateSoundBuffer: mesmos dados e parâmetros iniciais (o original está em -6 dB), cursor próprio. */
    const uint32_t du[2] = {sb, dup};
    CHECK_EQ_U32(vcall(ds, 5, 2, du), 0);
    uint32_t db = rd32(dup);
    CHECK(db != sb);
    CHECK_EQ_U32(vcall(db, B_PLAY, 3, play), 0);
    re5_dsound_mix(out, 480);
    CHECK(near(out[0], 0.5f * powf(10.0f, -0.3f)));
    CHECK_EQ_U32(vcall(db, B_STOP, 0, NULL), 0);

    /* QI do IDirectSoundBuffer8: o mesmo objeto. */
    const uint8_t iid8[16] = {0x49, 0xA4, 0x25, 0x68, 0x24, 0x75, 0x82, 0x4D,
                              0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E};
    memcpy(re5_g2h(iid), iid8, 16);
    const uint32_t qi[2] = {iid, pq};
    CHECK_EQ_U32(vcall(sb, B_QI, 2, qi), 0);
    CHECK_EQ_U32(rd32(pq), sb);
    CHECK_EQ_U32(vcall(sb, B_RELEASE, 0, NULL), 1);

    /* Formato que o DirectSound não aceita (float de 32 bits). */
    wr16(wfx, 3);
    CHECK_EQ_U32(vcall(ds, 3, 3, cb2), 0x88780064u);   /* DSERR_BADFORMAT */

    CHECK_EQ_U32(vcall(db, B_RELEASE, 0, NULL), 0);
    CHECK_EQ_U32(vcall(sb, B_RELEASE, 0, NULL), 0);
    CHECK_EQ_U32(vcall(pb, B_RELEASE, 0, NULL), 0);
    CHECK_EQ_U32(vcall(ds, 2, 0, NULL), 0);
    CHECK_DONE();
}
