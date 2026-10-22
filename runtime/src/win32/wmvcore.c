/*
 * wmvcore.c - Windows Media Format: IWMSyncReader sobre o FFmpeg (Fase 4,
 * etapa 4.5).
 *
 * Os vídeos do RE5 (nativePC_MT/movie/<nome>.wmv: bh5logo, re5logo, op_a, op_b)
 * são VC-1 (WVC1) 1280x720 com WMA Pro 5.1 a 48 kHz. O tocador do jogo faz,
 * nesta ordem:
 *   WMCreateSyncReader -> OpenStream(IStream do próprio jogo) ->
 *   SetReadStreamSamples(stream, FALSE) -> GetOutputCount -> para cada saída:
 *   GetOutputProps/GetMediaType; áudio: SetOutputSetting(EnableDiscreteOutput,
 *   SpeakerConfig) + GetOutputFormat(0) + SetOutputProps; vídeo:
 *   GetOutputFormatCount + GetOutputFormat até achar YV12 em VideoInfo +
 *   SetOutputProps + GetStreamNumberForOutput -> SetOutputSetting(
 *   VideoSampleDurations) -> QI(IWMHeaderInfo)::GetAttributeByName(Duration)
 *   -> numa thread própria, GetNextSample em laço (INSSBuffer::GetBuffer no
 *   vídeo, GetBufferAndLength no áudio) e SetRange(0) no fim do arquivo.
 *
 * Aqui: o IStream do guest é a fonte de um AVIOContext do FFmpeg (Read/Seek/
 * Stat chamados por re5_guest_call), o demuxer asf e os decodificadores vc1 e
 * wmapro fazem o resto. Vídeo sai em YV12 (o yuv420p do decodificador com os
 * planos V e U trocados), áudio em PCM 16 bits intercalado - com os canais da
 * fonte quando o jogo pede saída discreta, senão estéreo (downmix).
 *
 * A leitura do pacote (que chama o IStream do guest) roda com o lock do guest;
 * a decodificação, sem ele. Objetos COM como nas outras pontes; método sem
 * handler aborta com o nome (D8).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>

#include "kernel32_internal.h"

#define S_OK_                0x00000000u
#define E_INVALIDARG_        0x80070057u
#define E_POINTER_           0x80004003u
#define E_FAIL_              0x80004005u
#define NS_E_NO_MORE_SAMPLES 0xC00D0BCFu
#define NS_E_INVALID_OUTPUT_FORMAT 0xC00D0BE9u
#define NS_E_INVALID_REQUEST 0xC00D002Bu
#define NS_E_FILE_OPEN_FAILED 0xC00D001Du

#define WM_SF_CLEANPOINT    0x1u
#define WM_SF_DISCONTINUITY 0x2u

#define WM_MAGIC 0x31564D57u   /* 'WMV1' */
#define MT_HEADER 72u          /* WM_MEDIA_TYPE no x86; o formato vem logo depois */

static const uint8_t k_video[16] = {0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
static const uint8_t k_audio[16] = {0x61, 0x75, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
static const uint8_t k_yv12[16] = {0x59, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
static const uint8_t k_pcm[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
static const uint8_t k_fmt_videoinfo[16] = {0x80, 0x9F, 0x58, 0x05, 0x56, 0xC3, 0xCE, 0x11, 0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A};
static const uint8_t k_fmt_waveformatex[16] = {0x81, 0x9F, 0x58, 0x05, 0x56, 0xC3, 0xCE, 0x11, 0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A};
static const uint8_t k_iid_headerinfo[16] = {0xDA, 0x6B, 0x40, 0x96, 0x2B, 0x2B, 0xD3, 0x11, 0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF};
static const uint8_t k_iid_syncreader[16] = {0x21, 0xF1, 0x97, 0x93, 0x05, 0x77, 0xC9, 0x4D, 0xB0, 0x49, 0x98, 0xB6, 0x98, 0x18, 0x84, 0x14};
static const uint8_t k_iid_nssbuffer[16] = {0x24, 0x35, 0xCD, 0xE1, 0xD7, 0x03, 0xD2, 0x11, 0x9E, 0xED, 0x00, 0x60, 0x97, 0xD2, 0xD7, 0xCF};
static const uint8_t k_iid_outputprops[16] = {0xD7, 0x6B, 0x40, 0x96, 0x2B, 0x2B, 0xD3, 0x11, 0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF};
static const uint8_t k_iid_mediaprops[16] = {0xCE, 0x6B, 0x40, 0x96, 0x2B, 0x2B, 0xD3, 0x11, 0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF};
static const uint8_t k_iid_unknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

enum { K_READER, K_PROPS, K_SAMPLE, K_HEADER, K_COUNT };
static const char *const k_kind_name[K_COUNT] = {"IWMSyncReader", "IWMOutputMediaProps", "INSSBuffer", "IWMHeaderInfo"};

/* ---- objetos do guest ---- */

static uint32_t vtable_of(int kind);

typedef struct {
    int      kind;
    uint32_t guest, handle, refs;
} wm_obj;

static uint32_t obj_init(wm_obj *o, int kind)
{
    o->kind = kind;
    o->refs = 1;
    o->guest = k32_heap_alloc(k32_process_heap(), 0, 16);
    if (!o->guest) re5_fatal("wmvcore", "sem memória do guest");
    o->handle = re5_handle_new(RE5_H_COM, o, NULL);
    k32_w32(o->guest, vtable_of(kind));
    k32_w32(o->guest + 4, o->handle);
    k32_w32(o->guest + 8, WM_MAGIC);
    k32_w32(o->guest + 12, (uint32_t)kind);
    return o->guest;
}

static void obj_drop(wm_obj *o)
{
    re5_handle_close(o->handle);
    k32_w32(o->guest + 8, 0);
    k32_heap_free(k32_process_heap(), o->guest);
}

static wm_obj *obj_of(uint32_t va, int kind)
{
    if (!va || !re5_mem_is_mapped(va, 16) || k32_r32(va + 8) != WM_MAGIC || k32_r32(va + 12) != (uint32_t)kind)
        return NULL;
    return re5_handle_get(k32_r32(va + 4), RE5_H_COM);
}

static void *self(CPU *c, int kind)
{
    wm_obj *o = obj_of(re5_arg(c, 0), kind);
    if (!o) {
        const re5_thunk *t = re5_thunk_current();
        re5_fatal("wmvcore", "%s: this=0x%08X não é um %s da ponte", t ? t->name : "?", re5_arg(c, 0), k_kind_name[kind]);
    }
    return o;
}

static void unknown_method(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("wmvcore", "%s sem implementação (this=0x%08X)", t ? t->name : "?", re5_arg(c, 0));
}

static int guid_at(uint32_t va, const uint8_t g[16]) { return va && memcmp(re5_g2h(va), g, 16) == 0; }

static RE5_NORETURN void iid_fatal(uint32_t va, const char *who)
{
    const uint8_t *g = re5_g2h(va);
    re5_fatal("wmvcore", "%s::QueryInterface({%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}) "
              "sem implementação", who, g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11],
              g[12], g[13], g[14], g[15]);
}

/* ---- leitor ---- */

#define MAX_OUT 4
#define IO_SIZE 0x10000u

typedef struct frame_node {
    AVFrame *f;
    int out;
    struct frame_node *next;
} frame_node;

typedef struct {
    int st;                 /* índice no AVFormatContext */
    AVCodecContext *dec;
    int video;
    uint16_t num;           /* número do stream no ASF */
    int discontinuity;
    uint64_t next_t;        /* menor tempo da próxima amostra (100 ns) */
    uint64_t pred_t;        /* tempo de quem não traz tempo: o fim da anterior */
} wm_out;

typedef struct {
    wm_obj obj;
    uint32_t stream;        /* IStream do guest */
    uint32_t io;            /* memória do guest para Read/Seek/Stat */
    AVFormatContext *fmt;
    AVIOContext *avio;
    AVPacket *pkt;
    wm_out out[MAX_OUT];
    int nout, eof, discrete;
    uint32_t speaker;
    frame_node *head, *tail;
    uint32_t free_va[8], free_size[8];   /* memória de amostras para reaproveitar */
} wm_reader;

typedef struct {
    wm_obj obj;
    wm_reader *r;
    int out;
} wm_props;

typedef struct {
    wm_obj obj;
    wm_reader *r;
    uint32_t va, len, max;
} wm_sample;

typedef struct {
    wm_obj obj;
    wm_reader *r;
} wm_header;

/* IStream do guest: Read = 3, Seek = 5, Stat = 12. */
static uint32_t stream_call(wm_reader *r, uint32_t slot, int n, const uint32_t *args)
{
    uint32_t a[8];
    a[0] = r->stream;
    for (int i = 0; i < n; i++) a[i + 1] = args[i];
    return re5_guest_call(re5_thread_current(), k32_r32(k32_r32(r->stream) + 4u * slot), n + 1, a);
}

static int io_read(void *opaque, uint8_t *buf, int size)
{
    wm_reader *r = opaque;
    uint32_t want = (uint32_t)size < IO_SIZE ? (uint32_t)size : IO_SIZE, got = r->io + IO_SIZE;
    k32_w32(got, 0);
    const uint32_t a[3] = {r->io, want, got};
    uint32_t hr = stream_call(r, 3, 3, a);
    uint32_t n = k32_r32(got);
    if ((int32_t)hr < 0) return AVERROR(EIO);
    if (!n) return AVERROR_EOF;
    memcpy(buf, re5_g2h(r->io), n);
    return (int)n;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
    wm_reader *r = opaque;
    uint32_t out = r->io + IO_SIZE + 8;
    if (whence & AVSEEK_SIZE) {   /* STATSTG.cbSize em +8 */
        const uint32_t a[2] = {r->io, 1 /* STATFLAG_NONAME */};
        if ((int32_t)stream_call(r, 12, 2, a) < 0) return -1;
        return (int64_t)((uint64_t)k32_r32(r->io + 8) | (uint64_t)k32_r32(r->io + 12) << 32);
    }
    uint32_t origin = (whence & 3) == SEEK_SET ? 0 : (whence & 3) == SEEK_CUR ? 1 : 2;
    const uint32_t a[4] = {(uint32_t)off, (uint32_t)((uint64_t)off >> 32), origin, out};
    if ((int32_t)stream_call(r, 5, 4, a) < 0) return -1;
    return (int64_t)((uint64_t)k32_r32(out) | (uint64_t)k32_r32(out + 4) << 32);
}

static void queue_clear(wm_reader *r)
{
    while (r->head) {
        frame_node *n = r->head;
        r->head = n->next;
        av_frame_free(&n->f);
        free(n);
    }
    r->tail = NULL;
}

static void queue_push(wm_reader *r, AVFrame *f, int out)
{
    frame_node *n = malloc(sizeof *n);
    if (!n) re5_fatal("wmvcore", "sem memória");
    *n = (frame_node){f, out, NULL};
    if (r->tail) r->tail->next = n;
    else r->head = n;
    r->tail = n;
}

static void reader_close(wm_reader *r)
{
    queue_clear(r);
    for (int i = 0; i < r->nout; i++) avcodec_free_context(&r->out[i].dec);
    r->nout = 0;
    if (r->fmt) avformat_close_input(&r->fmt);
    if (r->avio) {
        av_freep(&r->avio->buffer);
        avio_context_free(&r->avio);
    }
    av_packet_free(&r->pkt);
    for (int i = 0; i < 8; i++) {
        if (r->free_va[i]) k32_heap_free(k32_process_heap(), r->free_va[i]);
        r->free_va[i] = 0;
    }
    if (r->stream) {
        stream_call(r, 2, 0, NULL);   /* IStream::Release */
        r->stream = 0;
    }
}

static int out_of_stream(const wm_reader *r, uint32_t num)
{
    for (int i = 0; i < r->nout; i++)
        if (r->out[i].num == num) return i;
    return -1;
}

/* ---- tipos de mídia ---- */

static uint32_t audio_channels(const wm_reader *r, const AVCodecContext *d)
{
    uint32_t ch = (uint32_t)d->ch_layout.nb_channels;
    return r->discrete ? ch : ch < 2 ? ch : 2;
}

/* WM_MEDIA_TYPE + formato no buffer host `b` (tamanho em *len). pbFormat = `at` + 72. */
static void media_type(const wm_reader *r, int o, uint32_t at, uint8_t *b, uint32_t *len)
{
    const wm_out *w = &r->out[o];
    const AVCodecContext *d = w->dec;
    memset(b, 0, 160);
    uint32_t cb;
    if (w->video) {
        memcpy(b, k_video, 16);
        memcpy(b + 16, k_yv12, 16);
        uint32_t wdt = (uint32_t)d->width, hgt = (uint32_t)d->height, img = wdt * hgt * 3u / 2u;
        memcpy(b + 32, &(uint32_t){1}, 4);          /* bFixedSizeSamples */
        memcpy(b + 40, &img, 4);                      /* lSampleSize */
        memcpy(b + 44, k_fmt_videoinfo, 16);
        cb = 88;                                      /* VIDEOINFOHEADER */
        uint8_t *v = b + MT_HEADER;
        AVRational fr = r->fmt->streams[w->st]->avg_frame_rate;
        uint64_t avg = fr.num > 0 && fr.den > 0 ? (uint64_t)10000000 * (uint64_t)fr.den / (uint64_t)fr.num : 333667;
        memcpy(v + 8, &wdt, 4);                       /* rcSource.right */
        memcpy(v + 12, &hgt, 4);
        memcpy(v + 24, &wdt, 4);                      /* rcTarget.right */
        memcpy(v + 28, &hgt, 4);
        uint32_t rate = (uint32_t)r->fmt->streams[w->st]->codecpar->bit_rate;
        memcpy(v + 32, &rate, 4);                     /* dwBitRate */
        memcpy(v + 40, &avg, 8);                      /* AvgTimePerFrame */
        uint32_t bi[10] = {40, wdt, hgt, 1u | 12u << 16, 0x32315659u /* 'YV12' */, img, 0, 0, 0, 0};
        memcpy(v + 48, bi, 40);                       /* BITMAPINFOHEADER */
    } else {
        memcpy(b, k_audio, 16);
        memcpy(b + 16, k_pcm, 16);
        uint32_t ch = audio_channels(r, d), rate = (uint32_t)d->sample_rate, block = ch * 2u, bps = rate * block;
        memcpy(b + 32, &(uint32_t){1}, 4);
        memcpy(b + 40, &block, 4);
        memcpy(b + 44, k_fmt_waveformatex, 16);
        uint8_t *x = b + MT_HEADER;
        uint16_t tag = ch > 2 ? 0xFFFEu : 1u, chs = (uint16_t)ch, bl = (uint16_t)block, bits = 16, ext = ch > 2 ? 22 : 0;
        memcpy(x, &tag, 2);
        memcpy(x + 2, &chs, 2);
        memcpy(x + 4, &rate, 4);
        memcpy(x + 8, &bps, 4);
        memcpy(x + 12, &bl, 2);
        memcpy(x + 14, &bits, 2);
        memcpy(x + 16, &ext, 2);
        cb = 18;
        if (ch > 2) {   /* WAVEFORMATEXTENSIBLE: 16 bits válidos, máscara da ordem de canais, subformato PCM */
            uint32_t mask = ch == 6 ? 0x3Fu : (1u << ch) - 1u;
            memcpy(x + 18, &bits, 2);
            memcpy(x + 20, &mask, 4);
            memcpy(x + 24, k_pcm, 16);
            cb = 40;
        }
    }
    memcpy(b + 64, &cb, 4);
    uint32_t pb = at + MT_HEADER;
    memcpy(b + 68, &pb, 4);
    *len = MT_HEADER + cb;
}

/* ---- IWMSyncReader ---- */

static wm_reader *reader(CPU *c) { return self(c, K_READER); }

static void r_QueryInterface(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t iid = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if (guid_at(iid, k_iid_unknown) || guid_at(iid, k_iid_syncreader)) {
        r->obj.refs++;
        k32_w32(out, r->obj.guest);
        c->eax = S_OK_;
        return;
    }
    if (guid_at(iid, k_iid_headerinfo)) {
        wm_header *h = calloc(1, sizeof *h);
        if (!h) re5_fatal("wmvcore", "sem memória");
        h->r = r;
        r->obj.refs++;   /* o header segura o leitor */
        k32_w32(out, obj_init(&h->obj, K_HEADER));
        c->eax = S_OK_;
        return;
    }
    iid_fatal(iid, "IWMSyncReader");
}

static void obj_AddRef(CPU *c)
{
    uint32_t va = re5_arg(c, 0);
    wm_obj *o = va && re5_mem_is_mapped(va, 16) ? obj_of(va, (int)k32_r32(va + 12)) : NULL;
    if (!o) unknown_method(c);
    c->eax = ++o->refs;
}

static void reader_release(wm_reader *r)
{
    if (--r->obj.refs) return;
    reader_close(r);
    obj_drop(&r->obj);
    free(r);
}

static void r_Release(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t left = r->obj.refs - 1;
    reader_release(r);   /* pode chamar IStream::Release do guest, que mexe no eax */
    c->eax = left;
}

static void r_OpenStream(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t st = re5_arg(c, 1);
    if (!st) { c->eax = E_INVALIDARG_; return; }
    if (r->fmt) { c->eax = NS_E_INVALID_REQUEST; return; }
    r->stream = st;
    stream_call(r, 1, 0, NULL);   /* IStream::AddRef */
    if (!r->io) r->io = k32_heap_alloc(k32_process_heap(), 0, IO_SIZE + 64);
    uint8_t *buf = av_malloc(IO_SIZE);
    r->avio = avio_alloc_context(buf, (int)IO_SIZE, 0, r, io_read, NULL, io_seek);
    r->fmt = avformat_alloc_context();
    r->pkt = av_packet_alloc();
    if (!r->io || !buf || !r->avio || !r->fmt || !r->pkt) re5_fatal("wmvcore", "sem memória");
    r->fmt->pb = r->avio;
    const AVInputFormat *asf = av_find_input_format("asf");
    int e = avformat_open_input(&r->fmt, NULL, asf, NULL);
    if (e >= 0) e = avformat_find_stream_info(r->fmt, NULL);
    if (e < 0) {
        RE5_WARN("wmvcore", "OpenStream: o FFmpeg não abriu o ASF (%d)", e);
        reader_close(r);
        c->eax = NS_E_FILE_OPEN_FAILED;
        return;
    }
    for (unsigned i = 0; i < r->fmt->nb_streams && r->nout < MAX_OUT; i++) {
        AVStream *s = r->fmt->streams[i];
        int video = s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        if (!video && s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
        if (!codec) re5_fatal("wmvcore", "stream %d (%s) sem decodificador no FFmpeg do runtime", s->id,
                              avcodec_get_name(s->codecpar->codec_id));
        wm_out *w = &r->out[r->nout];
        w->dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(w->dec, s->codecpar);
        w->dec->thread_count = 4;
        if (avcodec_open2(w->dec, codec, NULL) < 0) re5_fatal("wmvcore", "avcodec_open2(%s) falhou", codec->name);
        w->st = (int)i;
        w->video = video;
        w->num = (uint16_t)s->id;
        r->nout++;
        RE5_INFO("wmvcore", "saída %d: stream %u, %s %s", r->nout - 1, w->num, video ? "vídeo" : "áudio", codec->name);
    }
    c->eax = S_OK_;
}

static void r_Close(CPU *c)
{
    reader_close(reader(c));
    c->eax = S_OK_;
}

/* SetRange(this, QWORD cnsStartTime, LONGLONG cnsDuration): o jogo só volta ao começo. */
static void r_SetRange(CPU *c)
{
    wm_reader *r = reader(c);
    uint64_t start = (uint64_t)re5_arg(c, 1) | (uint64_t)re5_arg(c, 2) << 32;
    uint64_t dur = (uint64_t)re5_arg(c, 3) | (uint64_t)re5_arg(c, 4) << 32;
    if (!r->fmt) { c->eax = NS_E_INVALID_REQUEST; return; }
    if (dur) re5_fatal("wmvcore", "SetRange com duração (%llu) sem implementação", (unsigned long long)dur);
    int64_t ts = (int64_t)(start / 10);   /* 100 ns -> AV_TIME_BASE (us) */
    if (av_seek_frame(r->fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
        re5_fatal("wmvcore", "SetRange(%llu): o FFmpeg não conseguiu voltar no ASF", (unsigned long long)start);
    queue_clear(r);
    for (int i = 0; i < r->nout; i++) {
        avcodec_flush_buffers(r->out[i].dec);
        r->out[i].discontinuity = 1;
        r->out[i].next_t = r->out[i].pred_t = start;
    }
    r->eof = 0;
    c->eax = S_OK_;
}

static uint32_t sample_memory(wm_reader *r, uint32_t size)
{
    for (int i = 0; i < 8; i++) {
        if (r->free_va[i] && r->free_size[i] >= size) {
            uint32_t va = r->free_va[i];
            r->free_va[i] = 0;
            return va;
        }
    }
    uint32_t va = k32_heap_alloc(k32_process_heap(), 0, size);
    if (!va) re5_fatal("wmvcore", "sem memória do guest para uma amostra de %u bytes", size);
    return va;
}

static int16_t to_s16(float v)
{
    v *= 32768.0f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

/* Amostra de saída a partir de um quadro decodificado. */
static uint32_t make_sample(wm_reader *r, frame_node *n, uint32_t *len)
{
    const wm_out *w = &r->out[n->out];
    AVFrame *f = n->f;
    if (w->video) {
        if (f->format != AV_PIX_FMT_YUV420P)
            re5_fatal("wmvcore", "vídeo em %s: só yuv420p vira YV12", av_get_pix_fmt_name(f->format));
        uint32_t wd = (uint32_t)f->width, ht = (uint32_t)f->height, size = wd * ht * 3u / 2u;
        uint32_t va = sample_memory(r, size);
        uint8_t *dst = re5_g2h(va);
        for (uint32_t y = 0; y < ht; y++) memcpy(dst + (size_t)y * wd, f->data[0] + (size_t)y * f->linesize[0], wd);
        uint8_t *vp = dst + (size_t)wd * ht, *up = vp + (size_t)(wd / 2) * (ht / 2);   /* YV12: Y, V, U */
        for (uint32_t y = 0; y < ht / 2; y++) {
            memcpy(vp + (size_t)y * (wd / 2), f->data[2] + (size_t)y * f->linesize[2], wd / 2);
            memcpy(up + (size_t)y * (wd / 2), f->data[1] + (size_t)y * f->linesize[1], wd / 2);
        }
        *len = size;
        return va;
    }
    uint32_t src_ch = (uint32_t)f->ch_layout.nb_channels, ch = audio_channels(r, w->dec), ns = (uint32_t)f->nb_samples;
    uint32_t size = ns * ch * 2u, va = sample_memory(r, size ? size : 2);
    int16_t *dst = re5_g2h(va);
    int planar = av_sample_fmt_is_planar(f->format);
    enum AVSampleFormat base = av_get_packed_sample_fmt(f->format);
    for (uint32_t i = 0; i < ns; i++) {
        float s[8] = {0};
        for (uint32_t k = 0; k < src_ch && k < 8; k++) {
            const uint8_t *p = planar ? f->extended_data[k] + (size_t)i * av_get_bytes_per_sample(base)
                                      : f->extended_data[0] + ((size_t)i * src_ch + k) * av_get_bytes_per_sample(base);
            switch (base) {
            case AV_SAMPLE_FMT_FLT: memcpy(&s[k], p, 4); break;
            case AV_SAMPLE_FMT_S16: s[k] = *(const int16_t *)p / 32768.0f; break;
            case AV_SAMPLE_FMT_S32: s[k] = (float)(*(const int32_t *)p / 2147483648.0); break;
            default: re5_fatal("wmvcore", "áudio em %s sem conversão", av_get_sample_fmt_name(f->format));
            }
        }
        if (ch == src_ch) {
            for (uint32_t k = 0; k < ch; k++) dst[i * ch + k] = to_s16(s[k]);
        } else if (ch == 2) {   /* downmix: FL FR FC LFE BL BR */
            float l = s[0], rr = src_ch > 1 ? s[1] : s[0];
            if (src_ch >= 3) { l += 0.7071f * s[2]; rr += 0.7071f * s[2]; }
            if (src_ch >= 6) { l += 0.7071f * s[4]; rr += 0.7071f * s[5]; }
            dst[2 * i] = to_s16(l);
            dst[2 * i + 1] = to_s16(rr);
        } else {
            dst[i] = to_s16(s[0]);
        }
    }
    *len = size;
    return va;
}

/* Decodifica um pacote (sem o lock do guest: só trabalho do host) e enfileira os quadros. */
static void decode(wm_reader *r, int o, const AVPacket *pkt)
{
    AVCodecContext *d = r->out[o].dec;
    AVFrame *f = NULL;
    RE5_BLOCKING_BEGIN();
    avcodec_send_packet(d, pkt);
    for (;;) {
        if (!f) f = av_frame_alloc();
        if (avcodec_receive_frame(d, f) != 0) break;
        queue_push(r, f, o);
        f = NULL;
    }
    RE5_BLOCKING_END();
    av_frame_free(&f);
}

/* GetNextSample(this, WORD wStreamNum, INSSBuffer **, QWORD *time, QWORD *dur, DWORD *flags, DWORD *out, WORD *num) */
static void r_GetNextSample(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t want = re5_arg(c, 1) & 0xFFFFu, pps = re5_arg(c, 2), pt = re5_arg(c, 3), pd = re5_arg(c, 4);
    uint32_t pf = re5_arg(c, 5), po = re5_arg(c, 6), pn = re5_arg(c, 7);
    if (!r->fmt) { c->eax = NS_E_INVALID_REQUEST; return; }
    if (!pps) { c->eax = E_POINTER_; return; }
    int want_out = want ? out_of_stream(r, want) : -1;
    if (want && want_out < 0) { c->eax = E_INVALIDARG_; return; }
    for (;;) {
        frame_node **pp = &r->head, *prev = NULL;
        while (*pp && want_out >= 0 && (*pp)->out != want_out) {
            prev = *pp;
            pp = &(*pp)->next;
        }
        if (*pp) {
            frame_node *n = *pp;
            *pp = n->next;
            if (r->tail == n) r->tail = prev;
            wm_out *w = &r->out[n->out];
            AVStream *s = r->fmt->streams[w->st];
            int64_t pts = n->f->best_effort_timestamp != AV_NOPTS_VALUE ? n->f->best_effort_timestamp : n->f->pts;
            /* Só o 1º quadro de um pacote traz tempo (o wmapro tira vários de um pacote, e o último quadro
             * do vc1 sai no flush sem tempo): os outros seguem o relógio da saída, que nunca anda para trás. */
            uint64_t t100 = pts == AV_NOPTS_VALUE ? w->pred_t
                                                  : (uint64_t)av_rescale_q(pts, s->time_base, (AVRational){1, 10000000});
            if (t100 < w->next_t && !w->discontinuity) t100 = w->next_t;
            uint64_t d100 = w->video ? (n->f->duration > 0 ? (uint64_t)av_rescale_q(n->f->duration, s->time_base,
                                                                                    (AVRational){1, 10000000})
                                                            : 333667u)
                                     : (uint64_t)n->f->nb_samples * 10000000u / (uint64_t)w->dec->sample_rate;
            uint32_t flags = (w->video ? ((n->f->flags & AV_FRAME_FLAG_KEY) ? WM_SF_CLEANPOINT : 0) : WM_SF_CLEANPOINT) |
                             (w->discontinuity ? WM_SF_DISCONTINUITY : 0);
            w->discontinuity = 0;
            w->next_t = t100 + (w->video ? 1 : d100);
            w->pred_t = t100 + d100;
            wm_sample *sm = calloc(1, sizeof *sm);
            if (!sm) re5_fatal("wmvcore", "sem memória");
            sm->r = r;
            r->obj.refs++;   /* a amostra segura o leitor (a memória dela é dele) */
            sm->va = make_sample(r, n, &sm->len);
            sm->max = sm->len;
            k32_w32(pps, obj_init(&sm->obj, K_SAMPLE));
            if (pt) { k32_w32(pt, (uint32_t)t100); k32_w32(pt + 4, (uint32_t)(t100 >> 32)); }
            if (pd) { k32_w32(pd, (uint32_t)d100); k32_w32(pd + 4, (uint32_t)(d100 >> 32)); }
            if (pf) k32_w32(pf, flags);
            if (po) k32_w32(po, (uint32_t)n->out);
            if (pn) k32_w16(pn, w->num);
            av_frame_free(&n->f);
            free(n);
            c->eax = S_OK_;
            return;
        }
        if (r->eof) {
            k32_w32(pps, 0);
            c->eax = NS_E_NO_MORE_SAMPLES;
            return;
        }
        int e = av_read_frame(r->fmt, r->pkt);   /* com o lock: o IStream é do guest */
        if (e == AVERROR_EOF) {
            for (int o = 0; o < r->nout; o++) decode(r, o, NULL);
            r->eof = 1;
            continue;
        }
        if (e < 0) re5_fatal("wmvcore", "av_read_frame falhou (%d) no meio do ASF", e);
        int o = -1;
        for (int i = 0; i < r->nout; i++)
            if (r->out[i].st == r->pkt->stream_index) o = i;
        if (o >= 0) decode(r, o, r->pkt);
        av_packet_unref(r->pkt);
    }
}

static void r_SetReadStreamSamples(CPU *c)
{
    wm_reader *r = reader(c);
    if (re5_arg(c, 2)) re5_fatal("wmvcore", "SetReadStreamSamples(%u, comprimido) sem implementação", re5_arg(c, 1) & 0xFFFF);
    c->eax = out_of_stream(r, re5_arg(c, 1) & 0xFFFFu) >= 0 || !r->fmt ? S_OK_ : E_INVALIDARG_;
}

static const char *wide(uint32_t va, char *out, size_t n)
{
    size_t k = 0;
    for (; va && k + 1 < n; k++) {
        uint16_t ch = (uint16_t)k32_r16(va + 2 * (uint32_t)k);
        if (!ch) break;
        out[k] = ch < 0x80 ? (char)ch : '?';
    }
    out[k] = 0;
    return out;
}

/* SetOutputSetting(this, dwOutputNum, pszName, Type, pValue, WORD cbLength) */
static void r_SetOutputSetting(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1), val = re5_arg(c, 4);
    char name[64];
    wide(re5_arg(c, 2), name, sizeof name);
    if ((int)o >= r->nout || !val) { c->eax = E_INVALIDARG_; return; }
    if (!strcmp(name, "EnableDiscreteOutput")) r->discrete = k32_r32(val) != 0;
    else if (!strcmp(name, "SpeakerConfig")) r->speaker = k32_r32(val);
    else if (!strcmp(name, "VideoSampleDurations")) { /* as durações já saem em toda amostra */ }
    else re5_fatal("wmvcore", "SetOutputSetting(%u, \"%s\") sem implementação", o, name);
    c->eax = S_OK_;
}

static void r_GetOutputCount(CPU *c)
{
    wm_reader *r = reader(c);
    if (!re5_arg(c, 1)) { c->eax = E_POINTER_; return; }
    k32_w32(re5_arg(c, 1), (uint32_t)r->nout);
    c->eax = S_OK_;
}

static uint32_t new_props(wm_reader *r, int o)
{
    wm_props *p = calloc(1, sizeof *p);
    if (!p) re5_fatal("wmvcore", "sem memória");
    p->r = r;
    p->out = o;
    r->obj.refs++;
    return obj_init(&p->obj, K_PROPS);
}

/* GetOutputProps(this, dwOutputNum, IWMOutputMediaProps **) e GetOutputFormat(this, out, fmt, props**):
 * cada saída tem um formato só (YV12 ou PCM). */
static void r_GetOutputProps(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if ((int)o >= r->nout) { c->eax = E_INVALIDARG_; return; }
    k32_w32(out, new_props(r, (int)o));
    c->eax = S_OK_;
}

static void r_GetOutputFormatCount(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if ((int)o >= r->nout) { c->eax = E_INVALIDARG_; return; }
    k32_w32(out, 1);
    c->eax = S_OK_;
}

static void r_GetOutputFormat(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1), fmt = re5_arg(c, 2), out = re5_arg(c, 3);
    if (!out) { c->eax = E_POINTER_; return; }
    if ((int)o >= r->nout || fmt != 0) { c->eax = E_INVALIDARG_; return; }
    k32_w32(out, new_props(r, (int)o));
    c->eax = S_OK_;
}

static void r_SetOutputProps(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1);
    wm_props *p = (wm_props *)obj_of(re5_arg(c, 2), K_PROPS);
    if ((int)o >= r->nout || !p) { c->eax = E_INVALIDARG_; return; }
    c->eax = p->out == (int)o ? S_OK_ : NS_E_INVALID_OUTPUT_FORMAT;
}

static void r_GetOutputNumberForStream(CPU *c)
{
    wm_reader *r = reader(c);
    int o = out_of_stream(r, re5_arg(c, 1) & 0xFFFFu);
    if (!re5_arg(c, 2)) { c->eax = E_POINTER_; return; }
    if (o < 0) { c->eax = E_INVALIDARG_; return; }
    k32_w32(re5_arg(c, 2), (uint32_t)o);
    c->eax = S_OK_;
}

static void r_GetStreamNumberForOutput(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1);
    if (!re5_arg(c, 2)) { c->eax = E_POINTER_; return; }
    if ((int)o >= r->nout) { c->eax = E_INVALIDARG_; return; }
    k32_w16(re5_arg(c, 2), r->out[o].num);
    c->eax = S_OK_;
}

static void r_GetMaxOutputSampleSize(CPU *c)
{
    wm_reader *r = reader(c);
    uint32_t o = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if ((int)o >= r->nout) { c->eax = E_INVALIDARG_; return; }
    const AVCodecContext *d = r->out[o].dec;
    k32_w32(out, r->out[o].video ? (uint32_t)(d->width * d->height * 3 / 2) : 65536u * audio_channels(r, d) * 2u);
    c->eax = S_OK_;
}

/* ---- IWMOutputMediaProps ---- */

static void p_QueryInterface(CPU *c)
{
    wm_props *p = self(c, K_PROPS);
    uint32_t iid = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if (guid_at(iid, k_iid_unknown) || guid_at(iid, k_iid_outputprops) || guid_at(iid, k_iid_mediaprops)) {
        p->obj.refs++;
        k32_w32(out, p->obj.guest);
        c->eax = S_OK_;
        return;
    }
    iid_fatal(iid, "IWMOutputMediaProps");
}

static void p_Release(CPU *c)
{
    wm_props *p = self(c, K_PROPS);
    c->eax = --p->obj.refs;
    if (p->obj.refs) return;
    wm_reader *r = p->r;
    obj_drop(&p->obj);
    free(p);
    reader_release(r);
    c->eax = 0;
}

static void p_GetType(CPU *c)
{
    wm_props *p = self(c, K_PROPS);
    if (!re5_arg(c, 1)) { c->eax = E_POINTER_; return; }
    memcpy(re5_g2h(re5_arg(c, 1)), p->r->out[p->out].video ? k_video : k_audio, 16);
    c->eax = S_OK_;
}

/* GetMediaType(this, WM_MEDIA_TYPE *pType, DWORD *pcbType): NULL pergunta o tamanho. */
static void p_GetMediaType(CPU *c)
{
    wm_props *p = self(c, K_PROPS);
    uint32_t at = re5_arg(c, 1), pcb = re5_arg(c, 2);
    if (!pcb) { c->eax = E_POINTER_; return; }
    uint8_t b[160];
    uint32_t len;
    media_type(p->r, p->out, at, b, &len);
    if (!at) {
        k32_w32(pcb, len);
        c->eax = S_OK_;
        return;
    }
    if (k32_r32(pcb) < len) {
        k32_w32(pcb, len);
        c->eax = 0x8007007Au;   /* ASF_E_BUFFERTOOSMALL / ERROR_INSUFFICIENT_BUFFER */
        return;
    }
    memcpy(re5_g2h(at), b, len);
    k32_w32(pcb, len);
    c->eax = S_OK_;
}

static void p_SetMediaType(CPU *c)
{
    wm_props *p = self(c, K_PROPS);
    uint32_t mt = re5_arg(c, 1);
    if (!mt) { c->eax = E_POINTER_; return; }
    int video = p->r->out[p->out].video;
    c->eax = guid_at(mt + 16, video ? k_yv12 : k_pcm) ? S_OK_ : NS_E_INVALID_OUTPUT_FORMAT;
}

/* ---- INSSBuffer ---- */

static void b_QueryInterface(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    uint32_t iid = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if (guid_at(iid, k_iid_unknown) || guid_at(iid, k_iid_nssbuffer)) {
        s->obj.refs++;
        k32_w32(out, s->obj.guest);
        c->eax = S_OK_;
        return;
    }
    iid_fatal(iid, "INSSBuffer");
}

static void b_Release(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    c->eax = --s->obj.refs;
    if (s->obj.refs) return;
    wm_reader *r = s->r;
    int kept = 0;
    for (int i = 0; i < 8 && !kept; i++) {
        if (!r->free_va[i]) {
            r->free_va[i] = s->va;
            r->free_size[i] = s->max;
            kept = 1;
        }
    }
    if (!kept) k32_heap_free(k32_process_heap(), s->va);
    obj_drop(&s->obj);
    free(s);
    reader_release(r);
    c->eax = 0;
}

static void b_GetLength(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    if (!re5_arg(c, 1)) { c->eax = E_POINTER_; return; }
    k32_w32(re5_arg(c, 1), s->len);
    c->eax = S_OK_;
}

static void b_SetLength(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    if (re5_arg(c, 1) > s->max) { c->eax = E_INVALIDARG_; return; }
    s->len = re5_arg(c, 1);
    c->eax = S_OK_;
}

static void b_GetMaxLength(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    if (!re5_arg(c, 1)) { c->eax = E_POINTER_; return; }
    k32_w32(re5_arg(c, 1), s->max);
    c->eax = S_OK_;
}

static void b_GetBuffer(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    if (!re5_arg(c, 1)) { c->eax = E_POINTER_; return; }
    k32_w32(re5_arg(c, 1), s->va);
    c->eax = S_OK_;
}

static void b_GetBufferAndLength(CPU *c)
{
    wm_sample *s = self(c, K_SAMPLE);
    if (!re5_arg(c, 1) || !re5_arg(c, 2)) { c->eax = E_POINTER_; return; }
    k32_w32(re5_arg(c, 1), s->va);
    k32_w32(re5_arg(c, 2), s->len);
    c->eax = S_OK_;
}

/* ---- IWMHeaderInfo ---- */

static void h_QueryInterface(CPU *c)
{
    wm_header *h = self(c, K_HEADER);
    uint32_t iid = re5_arg(c, 1), out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    if (guid_at(iid, k_iid_unknown) || guid_at(iid, k_iid_headerinfo)) {
        h->obj.refs++;
        k32_w32(out, h->obj.guest);
        c->eax = S_OK_;
        return;
    }
    iid_fatal(iid, "IWMHeaderInfo");
}

static void h_Release(CPU *c)
{
    wm_header *h = self(c, K_HEADER);
    c->eax = --h->obj.refs;
    if (h->obj.refs) return;
    wm_reader *r = h->r;
    obj_drop(&h->obj);
    free(h);
    reader_release(r);
    c->eax = 0;
}

/* GetAttributeByName(this, WORD *pwStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE *pType, BYTE *pValue, WORD *pcbLength) */
static void h_GetAttributeByName(CPU *c)
{
    wm_header *h = self(c, K_HEADER);
    uint32_t pt = re5_arg(c, 3), val = re5_arg(c, 4), plen = re5_arg(c, 5);
    char name[64];
    wide(re5_arg(c, 2), name, sizeof name);
    if (strcmp(name, "Duration") != 0) re5_fatal("wmvcore", "IWMHeaderInfo::GetAttributeByName(\"%s\") sem implementação", name);
    if (!pt || !plen || !h->r->fmt) { c->eax = E_INVALIDARG_; return; }
    k32_w32(pt, 4);   /* WMT_TYPE_QWORD */
    if (!val) {
        k32_w16(plen, 8);
        c->eax = S_OK_;
        return;
    }
    if ((k32_r16(plen) & 0xFFFFu) < 8) { c->eax = 0x8007007Au; return; }
    uint64_t d = h->r->fmt->duration > 0 ? (uint64_t)h->r->fmt->duration * 10u : 0;   /* us -> 100 ns */
    k32_w32(val, (uint32_t)d);
    k32_w32(val + 4, (uint32_t)(d >> 32));
    k32_w16(plen, 8);
    c->eax = S_OK_;
}

/* ---- vtables ---- */

typedef struct { const char *name; uint32_t bytes; re5_host_fn fn; } wm_method;

static const wm_method k_reader[] = {
    {"QueryInterface", 12, r_QueryInterface}, {"AddRef", 4, obj_AddRef}, {"Release", 4, r_Release},
    {"Open", 8, NULL}, {"Close", 4, r_Close}, {"SetRange", 20, r_SetRange}, {"SetRangeByFrame", 24, NULL},
    {"GetNextSample", 32, r_GetNextSample}, {"SetStreamsSelected", 16, NULL}, {"GetStreamSelected", 12, NULL},
    {"SetReadStreamSamples", 12, r_SetReadStreamSamples}, {"GetReadStreamSamples", 12, NULL},
    {"GetOutputSetting", 24, NULL}, {"SetOutputSetting", 24, r_SetOutputSetting},
    {"GetOutputCount", 8, r_GetOutputCount}, {"GetOutputProps", 12, r_GetOutputProps},
    {"SetOutputProps", 12, r_SetOutputProps}, {"GetOutputFormatCount", 12, r_GetOutputFormatCount},
    {"GetOutputFormat", 16, r_GetOutputFormat}, {"GetOutputNumberForStream", 12, r_GetOutputNumberForStream},
    {"GetStreamNumberForOutput", 12, r_GetStreamNumberForOutput},
    {"GetMaxOutputSampleSize", 12, r_GetMaxOutputSampleSize}, {"GetMaxStreamSampleSize", 12, NULL},
    {"OpenStream", 8, r_OpenStream},
};

static const wm_method k_props[] = {
    {"QueryInterface", 12, p_QueryInterface}, {"AddRef", 4, obj_AddRef}, {"Release", 4, p_Release},
    {"GetType", 8, p_GetType}, {"GetMediaType", 12, p_GetMediaType}, {"SetMediaType", 8, p_SetMediaType},
    {"GetStreamGroupName", 12, NULL}, {"GetConnectionName", 12, NULL},
};

static const wm_method k_sample[] = {
    {"QueryInterface", 12, b_QueryInterface}, {"AddRef", 4, obj_AddRef}, {"Release", 4, b_Release},
    {"GetLength", 8, b_GetLength}, {"SetLength", 8, b_SetLength}, {"GetMaxLength", 8, b_GetMaxLength},
    {"GetBuffer", 8, b_GetBuffer}, {"GetBufferAndLength", 12, b_GetBufferAndLength},
};

static const wm_method k_header[] = {
    {"QueryInterface", 12, h_QueryInterface}, {"AddRef", 4, obj_AddRef}, {"Release", 4, h_Release},
    {"GetAttributeCount", 12, NULL}, {"GetAttributeByIndex", 28, NULL},
    {"GetAttributeByName", 24, h_GetAttributeByName}, {"SetAttribute", 24, NULL}, {"GetMarkerCount", 8, NULL},
    {"GetMarker", 20, NULL}, {"AddMarker", 16, NULL}, {"RemoveMarker", 8, NULL}, {"GetScriptCount", 8, NULL},
    {"GetScript", 28, NULL}, {"AddScript", 20, NULL}, {"RemoveScript", 8, NULL},
};

static uint32_t g_vt[K_COUNT];

static uint32_t vtable_of(int kind)
{
    if (g_vt[kind]) return g_vt[kind];
    static const wm_method *const tables[K_COUNT] = {k_reader, k_props, k_sample, k_header};
    static const uint32_t counts[K_COUNT] = {sizeof k_reader / sizeof k_reader[0], sizeof k_props / sizeof k_props[0],
                                             sizeof k_sample / sizeof k_sample[0], sizeof k_header / sizeof k_header[0]};
    uint32_t va = k32_heap_alloc(k32_process_heap(), 0, 4u * counts[kind]);
    if (!va) re5_fatal("wmvcore", "sem memória do guest");
    for (uint32_t i = 0; i < counts[kind]; i++) {
        char *name = malloc(64);
        if (!name) re5_fatal("wmvcore", "sem memória");
        snprintf(name, 64, "%s::%s", k_kind_name[kind], tables[kind][i].name);
        k32_w32(va + 4u * i, re5_thunk_new(name, tables[kind][i].fn ? tables[kind][i].fn : unknown_method,
                                           tables[kind][i].bytes, NULL));
    }
    return g_vt[kind] = va;
}

/* ---- WMCreateSyncReader(pUnkCert, dwRights, IWMSyncReader **) ---- */

static void w_WMCreateSyncReader(CPU *c)
{
    uint32_t out = re5_arg(c, 2);
    if (!out) { c->eax = E_POINTER_; return; }
    wm_reader *r = calloc(1, sizeof *r);
    if (!r) re5_fatal("wmvcore", "sem memória");
    k32_w32(out, obj_init(&r->obj, K_READER));
    c->eax = S_OK_;
}

static const re5_import_def k_defs[] = {
    {"WMCreateSyncReader", w_WMCreateSyncReader, 12, 0},
    {0},
};

const re5_import_module re5_mod_wmvcore = {"wmvcore.dll", k_defs};
