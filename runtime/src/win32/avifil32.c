/*
 * avifil32.c - gravação de vídeo em AVI (avifil32 + msvfw32).
 *
 * O RE5 grava a tela num AVI (0x006645xx): ICCompressorChoose ->
 * AVIFileOpenA(OF_CREATE|OF_WRITE) -> AVIFileCreateStreamA ->
 * AVIMakeCompressedStream -> AVIStreamSetFormat(BITMAPINFOHEADER) ->
 * AVIStreamWrite por quadro -> AVIStreamRelease/AVIFileRelease -> AVIFileExit.
 *
 * Como no Windows, PAVIFILE e PAVISTREAM são objetos COM (IAVIFile, IAVIStream)
 * e várias funções do avifil32 só chamam um slot da vtable do objeto - tanto
 * que o IAT do jogo tem pares com o mesmo endereço (AVIFileReadData =
 * AVIStreamSetFormat: slot 7; AVIFileRelease = AVIStreamRelease: slot 2).
 * Aqui é igual: os objetos têm vtables de thunks stdcall, e essas funções
 * despacham pela vtable.
 *
 * O runtime tem um só compressor, o "Full Frames (Uncompressed)" ('DIB '),
 * que é o que o jogo pré-seleciona no COMPVARS; ICCompressorChoose não tem
 * diálogo para mostrar e fica com essa escolha. O arquivo é um AVI 1.0 de
 * verdade (hdrl/strl/movi/idx1), escrito pelo VFS; os tamanhos e o índice
 * entram quando o arquivo é solto.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel32_internal.h"

#define AVIERR_OK           0x00000000u
#define AVIERR_FILEOPEN     0x80044067u
#define AVIERR_NOCOMPRESSOR 0x80044071u
#define AVIERR_BADFORMAT    0x80044066u
#define AVIERR_READONLY     0x80044072u

#define FCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FCC_DIB  FCC('D', 'I', 'B', ' ')
#define AVI_MAGIC 0x31495641u   /* 'AVI1' */

/* ---- objetos ---- */

typedef struct avi_stream avi_stream;

typedef struct {
    FILE       *f;
    char        path[1024];
    uint32_t    refs, guest, handle;
    avi_stream *stream;   /* um só: o jogo grava só vídeo */
} avi_file;

typedef struct { uint32_t offset, size; } avi_index;

struct avi_stream {
    avi_file   *file;     /* NULL num stream comprimido: ele repassa ao `src` */
    avi_stream *src;
    uint32_t    refs, guest, handle;
    uint32_t    info[35];   /* AVISTREAMINFOA (140 bytes) como o guest passou */
    uint8_t    *format;
    uint32_t    cb_format;
    uint32_t    fcc_handler;
    long        hdr_avih, hdr_strh, movi_list, movi_fcc;
    int         header_written;
    avi_index  *idx;
    uint32_t    nidx, capidx, max_chunk;
};

static uint32_t g_vt_file, g_vt_stream;
static int g_init_count;

static uint32_t vtable_of(int stream);

static uint32_t obj_new(void *host, int stream, uint32_t *handle)
{
    uint32_t g = k32_heap_alloc(k32_process_heap(), 0, 16);
    if (!g) re5_fatal("avifil32", "sem memória do guest");
    *handle = re5_handle_new(RE5_H_COM, host, NULL);
    k32_w32(g, vtable_of(stream));
    k32_w32(g + 4, *handle);
    k32_w32(g + 8, AVI_MAGIC);
    k32_w32(g + 12, (uint32_t)stream);
    return g;
}

static void *obj_of(uint32_t va, int stream)
{
    if (!va || !re5_mem_is_mapped(va, 16) || k32_r32(va + 8) != AVI_MAGIC || k32_r32(va + 12) != (uint32_t)stream)
        return NULL;
    return re5_handle_get(k32_r32(va + 4), RE5_H_COM);
}

static void obj_free(uint32_t guest, uint32_t handle)
{
    re5_handle_close(handle);
    k32_w32(guest + 8, 0);
    k32_heap_free(k32_process_heap(), guest);
}

static void unknown_method(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("avifil32", "%s sem implementação (this=0x%08X)", t ? t->name : "?", re5_arg(c, 0));
}

/* ---- escrita do AVI ---- */

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static void patch32(FILE *f, long at, uint32_t v)
{
    long here = ftell(f);
    fseek(f, at, SEEK_SET);
    put32(f, v);
    fseek(f, here, SEEK_SET);
}

static uint32_t info(const avi_stream *s, int i) { return s->info[i]; }   /* índice em DWORDs */

/* Cabeçalho com os campos que só o fim conhece em zero (corrigidos em finish). */
static void write_header(avi_stream *s)
{
    FILE *f = s->file->f;
    uint32_t strf = (s->cb_format + 1u) & ~1u;
    put32(f, FCC('R', 'I', 'F', 'F'));
    put32(f, 0);
    put32(f, FCC('A', 'V', 'I', ' '));
    put32(f, FCC('L', 'I', 'S', 'T'));
    put32(f, 4 + 8 + 56 + 12 + 8 + 56 + 8 + strf);
    put32(f, FCC('h', 'd', 'r', 'l'));
    put32(f, FCC('a', 'v', 'i', 'h'));
    put32(f, 56);
    s->hdr_avih = ftell(f);
    uint32_t scale = info(s, 5) ? info(s, 5) : 1, rate = info(s, 6) ? info(s, 6) : 30;
    put32(f, (uint32_t)(1000000ull * scale / rate));   /* dwMicroSecPerFrame */
    put32(f, 0);                                      /* dwMaxBytesPerSec (no fim) */
    put32(f, 0);
    put32(f, 0x10);                                   /* AVIF_HASINDEX */
    put32(f, 0);                                      /* dwTotalFrames (no fim) */
    put32(f, 0);
    put32(f, 1);                                      /* dwStreams */
    put32(f, 0);                                      /* dwSuggestedBufferSize (no fim) */
    uint32_t w = s->cb_format >= 12 ? (uint32_t)s->format[4] | (uint32_t)s->format[5] << 8 |
                                      (uint32_t)s->format[6] << 16 | (uint32_t)s->format[7] << 24 : 0;
    int32_t h = s->cb_format >= 12 ? (int32_t)((uint32_t)s->format[8] | (uint32_t)s->format[9] << 8 |
                                               (uint32_t)s->format[10] << 16 | (uint32_t)s->format[11] << 24) : 0;
    put32(f, w);
    put32(f, (uint32_t)(h < 0 ? -h : h));
    for (int i = 0; i < 4; i++) put32(f, 0);
    put32(f, FCC('L', 'I', 'S', 'T'));
    put32(f, 4 + 8 + 56 + 8 + strf);
    put32(f, FCC('s', 't', 'r', 'l'));
    put32(f, FCC('s', 't', 'r', 'h'));
    put32(f, 56);
    s->hdr_strh = ftell(f);
    put32(f, info(s, 0) ? info(s, 0) : FCC('v', 'i', 'd', 's'));
    put32(f, s->fcc_handler);
    put32(f, info(s, 2));                             /* dwFlags */
    put16(f, (uint16_t)info(s, 4));                   /* wPriority */
    put16(f, (uint16_t)(info(s, 4) >> 16));           /* wLanguage */
    put32(f, info(s, 9));                             /* dwInitialFrames */
    put32(f, scale);
    put32(f, rate);
    put32(f, info(s, 7));                             /* dwStart */
    put32(f, 0);                                      /* dwLength (no fim) */
    put32(f, 0);                                      /* dwSuggestedBufferSize (no fim) */
    put32(f, info(s, 11));                            /* dwQuality */
    put32(f, info(s, 12));                            /* dwSampleSize */
    for (int i = 0; i < 4; i++) put16(f, (uint16_t)info(s, 13 + i));   /* rcFrame em shorts */
    put32(f, FCC('s', 't', 'r', 'f'));
    put32(f, s->cb_format);
    fwrite(s->format, 1, s->cb_format, f);
    if (s->cb_format & 1) fputc(0, f);
    put32(f, FCC('L', 'I', 'S', 'T'));
    s->movi_list = ftell(f);
    put32(f, 0);
    s->movi_fcc = ftell(f);
    put32(f, FCC('m', 'o', 'v', 'i'));
    s->header_written = 1;
}

static void finish(avi_stream *s)
{
    FILE *f = s->file->f;
    if (!s->header_written) {
        if (!s->format) return;   /* nenhum formato: nada a gravar além do arquivo vazio */
        write_header(s);
    }
    long movi_end = ftell(f);
    patch32(f, s->movi_list, (uint32_t)(movi_end - s->movi_fcc));
    put32(f, FCC('i', 'd', 'x', '1'));
    put32(f, 16u * s->nidx);
    for (uint32_t i = 0; i < s->nidx; i++) {
        put32(f, FCC('0', '0', 'd', 'b'));
        put32(f, 0x10);   /* AVIIF_KEYFRAME: quadros sem compressão */
        put32(f, s->idx[i].offset);
        put32(f, s->idx[i].size);
    }
    long end = ftell(f);
    patch32(f, 4, (uint32_t)(end - 8));
    uint32_t scale = info(s, 5) ? info(s, 5) : 1, rate = info(s, 6) ? info(s, 6) : 30;
    patch32(f, s->hdr_avih + 4, (uint32_t)((uint64_t)s->max_chunk * rate / scale));
    patch32(f, s->hdr_avih + 16, s->nidx);
    patch32(f, s->hdr_avih + 28, s->max_chunk);
    patch32(f, s->hdr_strh + 32, s->nidx);
    patch32(f, s->hdr_strh + 36, s->max_chunk);
}

/* ---- IAVIFile ---- */

static void f_AddRef(CPU *c)
{
    avi_file *f = obj_of(re5_arg(c, 0), 0);
    if (!f) unknown_method(c);
    c->eax = ++f->refs;
}

static void f_Release(CPU *c)
{
    avi_file *f = obj_of(re5_arg(c, 0), 0);
    if (!f) unknown_method(c);
    c->eax = --f->refs;
    if (f->refs) return;
    if (f->stream) {
        if (f->stream->refs) RE5_WARN("avifil32", "%s solto com o stream ainda aberto", f->path);
        finish(f->stream);
        f->stream->file = NULL;
    }
    fclose(f->f);
    RE5_INFO("avifil32", "AVI gravado: %s (%u quadros)", f->path, f->stream ? f->stream->nidx : 0);
    if (f->stream && !f->stream->refs) {
        free(f->stream->format);
        free(f->stream->idx);
        free(f->stream);
    }
    obj_free(f->guest, f->handle);
    free(f);
}

/* ---- IAVIStream ---- */

static avi_stream *stream_self(CPU *c)
{
    avi_stream *s = obj_of(re5_arg(c, 0), 1);
    if (!s) unknown_method(c);
    return s;
}

static void s_AddRef(CPU *c) { c->eax = ++stream_self(c)->refs; }

/* Última referência: o objeto do guest some; a estrutura fica enquanto o arquivo
 * (que grava o índice no fim) ainda existir. */
static void stream_drop(avi_stream *s)
{
    obj_free(s->guest, s->handle);
    s->guest = 0;
    avi_stream *src = s->src;
    if (!s->file) {
        free(s->format);
        free(s->idx);
        free(s);
    }
    if (src && --src->refs == 0) stream_drop(src);   /* o comprimido segurava o original */
}

static void s_Release(CPU *c)
{
    avi_stream *s = stream_self(c);
    c->eax = --s->refs;
    if (!s->refs) stream_drop(s);
}

static avi_stream *target(avi_stream *s) { return s->src ? s->src : s; }

/* SetFormat(this, lPos, lpFormat, cbFormat) */
static void s_SetFormat(CPU *c)
{
    avi_stream *s = target(stream_self(c));
    uint32_t pos = re5_arg(c, 1), fmt = re5_arg(c, 2), cb = re5_arg(c, 3);
    if (!s->file) { c->eax = AVIERR_READONLY; return; }
    if (s->header_written || pos != 0 || !fmt || cb < 40 || cb > 4096) { c->eax = AVIERR_BADFORMAT; return; }
    free(s->format);
    s->format = malloc(cb);
    if (!s->format) re5_fatal("avifil32", "sem memória");
    memcpy(s->format, re5_g2h(fmt), cb);
    s->cb_format = cb;
    c->eax = AVIERR_OK;
}

/* Write(this, lStart, lSamples, lpBuffer, cbBuffer, dwFlags, plSampWritten, plBytesWritten) */
static void s_Write(CPU *c)
{
    avi_stream *s = target(stream_self(c));
    uint32_t n = re5_arg(c, 2), buf = re5_arg(c, 3), cb = re5_arg(c, 4);
    uint32_t psamp = re5_arg(c, 6), pbytes = re5_arg(c, 7);
    if (!s->file) { c->eax = AVIERR_READONLY; return; }
    if (!s->format) { c->eax = AVIERR_BADFORMAT; return; }
    if (!s->header_written) write_header(s);
    FILE *f = s->file->f;
    if (s->nidx == s->capidx) {
        uint32_t cap = s->capidx ? s->capidx * 2 : 256;
        avi_index *ni = realloc(s->idx, cap * sizeof *ni);
        if (!ni) re5_fatal("avifil32", "sem memória");
        s->idx = ni;
        s->capidx = cap;
    }
    long at = ftell(f);
    s->idx[s->nidx++] = (avi_index){(uint32_t)(at - s->movi_fcc), cb};
    put32(f, FCC('0', '0', 'd', 'b'));
    put32(f, cb);
    if (cb && fwrite(re5_g2h(buf), 1, cb, f) != cb) re5_fatal("avifil32", "falha ao gravar %s", s->file->path);
    if (cb & 1) fputc(0, f);
    if (cb > s->max_chunk) s->max_chunk = cb;
    if (psamp) k32_w32(psamp, n);
    if (pbytes) k32_w32(pbytes, cb);
    c->eax = AVIERR_OK;
}

static void i_QueryInterface(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("avifil32", "%s sem implementação", t ? t->name : "?");
}

/* ---- vtables ---- */

typedef struct { const char *name; uint32_t bytes; re5_host_fn fn; } avi_method;

static const avi_method k_file[] = {
    {"QueryInterface", 12, i_QueryInterface}, {"AddRef", 4, f_AddRef}, {"Release", 4, f_Release},
    {"Info", 12, NULL}, {"GetStream", 16, NULL}, {"CreateStream", 12, NULL}, {"WriteData", 16, NULL},
    {"ReadData", 16, NULL}, {"EndRecord", 4, NULL}, {"DeleteStream", 12, NULL},
};

static const avi_method k_stream[] = {
    {"QueryInterface", 12, i_QueryInterface}, {"AddRef", 4, s_AddRef}, {"Release", 4, s_Release},
    {"Create", 12, NULL}, {"Info", 12, NULL}, {"FindSample", 12, NULL}, {"ReadFormat", 16, NULL},
    {"SetFormat", 16, s_SetFormat}, {"Read", 28, NULL}, {"Write", 32, s_Write}, {"Delete", 12, NULL},
    {"ReadData", 16, NULL}, {"WriteData", 16, NULL}, {"SetInfo", 12, NULL},
};

static uint32_t vtable_of(int stream)
{
    uint32_t *vt = stream ? &g_vt_stream : &g_vt_file;
    if (*vt) return *vt;
    const avi_method *m = stream ? k_stream : k_file;
    uint32_t n = stream ? (uint32_t)(sizeof k_stream / sizeof k_stream[0]) : (uint32_t)(sizeof k_file / sizeof k_file[0]);
    uint32_t va = k32_heap_alloc(k32_process_heap(), 0, 4u * n);
    if (!va) re5_fatal("avifil32", "sem memória do guest");
    for (uint32_t i = 0; i < n; i++) {
        char *name = malloc(48);
        if (!name) re5_fatal("avifil32", "sem memória");
        snprintf(name, 48, "%s::%s", stream ? "IAVIStream" : "IAVIFile", m[i].name);
        k32_w32(va + 4u * i, re5_thunk_new(name, m[i].fn ? m[i].fn : unknown_method, m[i].bytes, NULL));
    }
    return *vt = va;
}

/* Chama o slot `slot` da vtable do objeto em arg 0 com os `n` argumentos seguintes. */
static void via_vtable(CPU *c, uint32_t slot, int n)
{
    uint32_t obj = re5_arg(c, 0), a[8];
    if (!obj) { c->eax = 0x80004003u; return; }   /* E_POINTER */
    a[0] = obj;
    for (int i = 1; i <= n; i++) a[i] = re5_arg(c, i);
    c->eax = re5_guest_call(re5_thread_current(), k32_r32(k32_r32(obj) + 4u * slot), n + 1, a);
}

/* ---- exports do avifil32 ---- */

static void a_AVIFileInit(CPU *c) { (void)c; g_init_count++; }
static void a_AVIFileExit(CPU *c)
{
    (void)c;
    if (g_init_count) g_init_count--;
}

#define OF_WRITE_     0x0001u
#define OF_READWRITE_ 0x0002u
#define OF_CREATE_    0x1000u

static void a_AVIFileOpenA(CPU *c)
{
    uint32_t pp = re5_arg(c, 0), name = re5_arg(c, 1), mode = re5_arg(c, 2), handler = re5_arg(c, 3);
    if (!pp || !name) { c->eax = AVIERR_FILEOPEN; return; }
    k32_w32(pp, 0);
    if (!(mode & OF_CREATE_) || !(mode & (OF_WRITE_ | OF_READWRITE_)) || handler)
        re5_fatal("avifil32", "AVIFileOpenA(\"%s\", modo 0x%X): só a gravação de arquivo novo existe", re5_gstr(name),
                  mode);
    avi_file *f = calloc(1, sizeof *f);
    if (!f) re5_fatal("avifil32", "sem memória");
    if (re5_vfs_resolve(re5_gstr(name), f->path, sizeof f->path) != 0 || !(f->f = fopen(f->path, "wb"))) {
        RE5_WARN("avifil32", "AVIFileOpenA(\"%s\"): não abriu para gravar", re5_gstr(name));
        free(f);
        c->eax = AVIERR_FILEOPEN;
        return;
    }
    f->refs = 1;
    f->guest = obj_new(f, 0, &f->handle);
    k32_w32(pp, f->guest);
    RE5_INFO("avifil32", "gravando AVI em %s", f->path);
    c->eax = AVIERR_OK;
}

static void a_AVIFileCreateStreamA(CPU *c)
{
    avi_file *f = obj_of(re5_arg(c, 0), 0);
    uint32_t pp = re5_arg(c, 1), si = re5_arg(c, 2);
    if (!f) re5_fatal("avifil32", "AVIFileCreateStreamA num PAVIFILE que não é da ponte (0x%08X)", re5_arg(c, 0));
    if (!pp || !si) { c->eax = 0x80004003u; return; }
    if (f->stream) re5_fatal("avifil32", "AVIFileCreateStreamA: mais de um stream por arquivo sem implementação");
    avi_stream *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("avifil32", "sem memória");
    s->file = f;
    s->refs = 1;
    memcpy(s->info, re5_g2h(si), sizeof s->info);
    s->fcc_handler = s->info[1];
    s->guest = obj_new(s, 1, &s->handle);
    f->stream = s;
    k32_w32(pp, s->guest);
    c->eax = AVIERR_OK;
}

static void a_AVIMakeCompressedStream(CPU *c)
{
    uint32_t pp = re5_arg(c, 0), src_va = re5_arg(c, 1), opt = re5_arg(c, 2);
    avi_stream *src = obj_of(src_va, 1);
    if (!pp || !src) { c->eax = 0x80004003u; return; }
    k32_w32(pp, 0);
    uint32_t fcc = opt ? k32_r32(opt + 4) : 0;   /* AVICOMPRESSOPTIONS.fccHandler */
    if (fcc && fcc != FCC_DIB) {
        RE5_WARN("avifil32", "AVIMakeCompressedStream: compressor 0x%08X não existe (só 'DIB ')", fcc);
        c->eax = AVIERR_NOCOMPRESSOR;
        return;
    }
    avi_stream *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("avifil32", "sem memória");
    s->src = src;
    src->refs++;
    src->fcc_handler = FCC_DIB;
    s->refs = 1;
    s->guest = obj_new(s, 1, &s->handle);
    k32_w32(pp, s->guest);
    c->eax = AVIERR_OK;
}

/* Mesmo endereço no Windows: slot 7 (IAVIFile::ReadData / IAVIStream::SetFormat). */
static void a_slot7(CPU *c) { via_vtable(c, 7, 3); }
/* Mesmo endereço no Windows: IUnknown::Release. */
static void a_Release(CPU *c) { via_vtable(c, 2, 0); }
static void a_AVIStreamWrite(CPU *c) { via_vtable(c, 9, 7); }

static const re5_import_def k_avifil32[] = {
    {"AVIFileCreateStreamA",    a_AVIFileCreateStreamA,    12, 0},
    {"AVIFileExit",             a_AVIFileExit,              0, 0},
    {"AVIFileInit",             a_AVIFileInit,              0, 0},
    {"AVIFileOpenA",            a_AVIFileOpenA,            16, 0},
    {"AVIFileReadData",         a_slot7,                   16, 0},
    {"AVIStreamSetFormat",      a_slot7,                   16, 0},
    {"AVIFileRelease",          a_Release,                  4, 0},
    {"AVIStreamRelease",        a_Release,                  4, 0},
    {"AVIMakeCompressedStream", a_AVIMakeCompressedStream, 16, 0},
    {"AVIStreamWrite",          a_AVIStreamWrite,          32, 0},
    {0},
};

const re5_import_module re5_mod_avifil32 = {"avifil32.dll", k_avifil32};

/* ---- msvfw32: escolha do compressor ---- */

#define ICMF_COMPVARS_VALID 0x00000001u

/* ICCompressorChoose(hwnd, uiFlags, pvIn, lpData, PCOMPVARS pc, lpszTitle). O
 * diálogo do Windows lista os compressores; o runtime só tem "Full Frames",
 * e é ela que volta (como um OK no diálogo com a seleção padrão). */
static void m_ICCompressorChoose(CPU *c)
{
    uint32_t pc = re5_arg(c, 4);
    if (!pc || k32_r32(pc) < 64) { c->eax = 0; return; }
    k32_w32(pc + 8, 0);                                        /* hic: nenhum compressor aberto */
    k32_w32(pc + 12, FCC('v', 'i', 'd', 'c'));                 /* fccType */
    k32_w32(pc + 16, FCC_DIB);                                 /* fccHandler */
    k32_w32(pc + 4, k32_r32(pc + 4) | ICMF_COMPVARS_VALID);
    RE5_INFO("msvfw32", "ICCompressorChoose: Full Frames (sem compressão), o único compressor do runtime");
    c->eax = 1;
}

static void m_ICCompressorFree(CPU *c)
{
    uint32_t pc = re5_arg(c, 0);
    if (pc && k32_r32(pc + 4) & ICMF_COMPVARS_VALID) k32_w32(pc + 8, 0);
}

static const re5_import_def k_msvfw32[] = {
    {"ICCompressorChoose", m_ICCompressorChoose, 24, 0},
    {"ICCompressorFree",   m_ICCompressorFree,    4, 0},
    {0},
};

const re5_import_module re5_mod_msvfw32 = {"msvfw32.dll", k_msvfw32};
