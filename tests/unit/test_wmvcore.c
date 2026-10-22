/*
 * IWMSyncReader sobre o FFmpeg com um vídeo de verdade do jogo (argv[1], por
 * exemplo nativePC_MT/movie/bh5logo.wmv; sem o arquivo o teste é pulado).
 * O IStream é um objeto do guest como o do jogo, lendo um arquivo do host.
 * Cobre a sequência do tocador do RE5: OpenStream, saídas (YV12 em
 * VideoInfo, PCM com e sem EnableDiscreteOutput), número de stream,
 * IWMHeaderInfo::Duration, GetNextSample até NS_E_NO_MORE_SAMPLES, INSSBuffer
 * e SetRange(0) voltando ao começo com descontinuidade.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

#if !defined(_WIN32)
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

static re5_thread *T;
static FILE *g_file;

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

/* ---- IStream do guest sobre um FILE* ---- */

static int g_stream_refs = 1;

static void s_AddRef(CPU *c) { c->eax = (uint32_t)++g_stream_refs; }
static void s_Release(CPU *c) { c->eax = (uint32_t)--g_stream_refs; }

static void s_Read(CPU *c)
{
    uint32_t pv = re5_arg(c, 1), cb = re5_arg(c, 2), pn = re5_arg(c, 3);
    size_t n = fread(re5_g2h(pv), 1, cb, g_file);
    if (pn) wr32(pn, (uint32_t)n);
    c->eax = n == cb ? 0 : 1 /* S_FALSE */;
}

static void s_Seek(CPU *c)
{
    int64_t off = (int64_t)((uint64_t)re5_arg(c, 1) | (uint64_t)re5_arg(c, 2) << 32);
    uint32_t origin = re5_arg(c, 3), pnew = re5_arg(c, 4);
    if (_fseeki64(g_file, off, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END) != 0) {
        c->eax = 0x80030019u;   /* STG_E_INVALIDFUNCTION */
        return;
    }
    uint64_t pos = (uint64_t)_ftelli64(g_file);
    if (pnew) {
        wr32(pnew, (uint32_t)pos);
        wr32(pnew + 4, (uint32_t)(pos >> 32));
    }
    c->eax = 0;
}

static void s_Stat(CPU *c)
{
    uint32_t st = re5_arg(c, 1);
    int64_t here = _ftelli64(g_file);
    _fseeki64(g_file, 0, SEEK_END);
    uint64_t size = (uint64_t)_ftelli64(g_file);
    _fseeki64(g_file, here, SEEK_SET);
    memset(re5_g2h(st), 0, 72);
    wr32(st + 4, 2 /* STGTY_STREAM */);
    wr32(st + 8, (uint32_t)size);
    wr32(st + 12, (uint32_t)(size >> 32));
    c->eax = 0;
}

static void s_unused(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("teste", "%s não devia ser chamado", t ? t->name : "?");
}

static uint32_t make_stream(uint32_t at)
{
    static const struct { const char *n; re5_host_fn f; uint32_t b; } m[13] = {
        {"IStream::QueryInterface", s_unused, 12}, {"IStream::AddRef", s_AddRef, 4},
        {"IStream::Release", s_Release, 4}, {"IStream::Read", s_Read, 16}, {"IStream::Write", s_unused, 16},
        {"IStream::Seek", s_Seek, 20}, {"IStream::SetSize", s_unused, 12}, {"IStream::CopyTo", s_unused, 28},
        {"IStream::Commit", s_unused, 8}, {"IStream::Revert", s_unused, 4}, {"IStream::LockRegion", s_unused, 24},
        {"IStream::UnlockRegion", s_unused, 24}, {"IStream::Stat", s_Stat, 12},
    };
    uint32_t vt = at + 4;
    wr32(at, vt);
    for (int i = 0; i < 13; i++) wr32(vt + 4u * i, re5_thunk_new(m[i].n, m[i].f, m[i].b, NULL));
    return at;
}

static void put_wide(uint32_t va, const char *s)
{
    for (; *s; s++, va += 2) wr16(va, (uint16_t)*s);
    wr16(va, 0);
}

/* IWMSyncReader / IWMOutputMediaProps / INSSBuffer / IWMHeaderInfo */
enum { R_QI = 0, R_RELEASE = 2, R_SETRANGE = 5, R_GETNEXT = 7, R_SETREADSS = 10, R_SETOUTSET = 13, R_OUTCOUNT = 14,
       R_OUTPROPS = 15, R_SETOUTPROPS = 16, R_FMTCOUNT = 17, R_FMT = 18, R_STREAMFOROUT = 20, R_OPENSTREAM = 23 };
enum { P_RELEASE = 2, P_GETTYPE = 3, P_GETMT = 4 };
enum { B_RELEASE = 2, B_GETLEN = 3, B_GETBUF = 6, B_GETBUFLEN = 7 };
enum { H_RELEASE = 2, H_GETBYNAME = 5 };

int main(int argc, char **argv)
{
    if (argc < 2 || !(g_file = fopen(argv[1], "rb"))) {
        printf("SKIP: vídeo do jogo não encontrado (%s)\n", argc > 1 ? argv[1] : "sem argumento");
        return 77;
    }
#if defined(_WIN32)
    _putenv("RE5_HEADLESS=1");
#else
    setenv("RE5_HEADLESS", "1", 1);
#endif
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    uint32_t m = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    uint32_t pr = m, cnt = m + 4, pp = m + 8, mt = m + 0x100, cb = m + 0xC, guid = m + 0x10, val = m + 0x20;
    uint32_t ps = m + 0x30, pt = m + 0x38, pd = m + 0x40, pf = m + 0x48, po = m + 0x4C, pn = m + 0x50;
    uint32_t name = m + 0x300, ph = m + 0x54, iid = m + 0x60, pbuf = m + 0x70, plen = m + 0x74, stream = m + 0x400;
    make_stream(stream);
    /* argv[2] (opcional): grava o PCM de 6 canais/16 bits que sai do leitor, para análise. */
    FILE *pcm_out = argc > 2 ? fopen(argv[2], "wb") : NULL;

    uint32_t create = re5_import_bind("wmvcore.dll", "WMCreateSyncReader", 0);
    CHECK_EQ_U32(re5_thunk_get(create)->arg_bytes, 12);
    const uint32_t cr[3] = {0, 0, pr};
    CHECK_EQ_U32(re5_guest_call(T, create, 3, cr), 0);
    uint32_t r = rd32(pr);
    CHECK(r != 0);
    CHECK_EQ_U32(vcall(r, R_OPENSTREAM, 1, &stream), 0);
    CHECK_EQ_U32(g_stream_refs, 2);   /* o leitor segura o IStream */
    CHECK_EQ_U32(vcall(r, R_OUTCOUNT, 1, &cnt), 0);
    CHECK_EQ_U32(rd32(cnt), 2);

    /* Saídas: acha a de vídeo e a de áudio pelo GetType. */
    int vo = -1, ao = -1;
    for (uint32_t o = 0; o < 2; o++) {
        const uint32_t a[2] = {o, pp};
        CHECK_EQ_U32(vcall(r, R_OUTPROPS, 2, a), 0);
        uint32_t p = rd32(pp);
        CHECK_EQ_U32(vcall(p, P_GETTYPE, 1, &guid), 0);
        if (rd32(guid) == 0x73646976u) vo = (int)o;   /* 'vids' */
        if (rd32(guid) == 0x73647561u) ao = (int)o;   /* 'auds' */
        CHECK_EQ_U32(vcall(p, P_RELEASE, 0, NULL), 0);
    }
    CHECK(vo >= 0 && ao >= 0);

    /* Vídeo: um formato, YV12 em VideoInfo, 1280x720, 12 bits. */
    uint32_t a2[3] = {(uint32_t)vo, cnt};
    CHECK_EQ_U32(vcall(r, R_FMTCOUNT, 2, a2), 0);
    CHECK_EQ_U32(rd32(cnt), 1);
    uint32_t a3[3] = {(uint32_t)vo, 0, pp};
    CHECK_EQ_U32(vcall(r, R_FMT, 3, a3), 0);
    uint32_t vp = rd32(pp);
    uint32_t q[2] = {0, cb};
    CHECK_EQ_U32(vcall(vp, P_GETMT, 2, q), 0);
    CHECK_EQ_U32(rd32(cb), 72 + 88);
    q[0] = mt;
    CHECK_EQ_U32(vcall(vp, P_GETMT, 2, q), 0);
    CHECK_EQ_U32(rd32(mt + 16), 0x32315659u);    /* subtype YV12 */
    CHECK_EQ_U32(rd32(mt + 44), 0x05589F80u);    /* FORMAT_VideoInfo */
    CHECK_EQ_U32(rd32(mt + 68), mt + 72);         /* pbFormat logo depois */
    uint32_t bi = rd32(mt + 68) + 48;
    CHECK_EQ_U32(rd32(bi + 4), 1280);
    CHECK_EQ_U32(rd32(bi + 8), 720);
    CHECK_EQ_U32(rd16(bi + 14), 12);
    CHECK_EQ_U32(rd32(bi + 16), 0x32315659u);
    const uint32_t sp[2] = {(uint32_t)vo, vp};
    CHECK_EQ_U32(vcall(r, R_SETOUTPROPS, 2, sp), 0);
    CHECK_EQ_U32(vcall(vp, P_RELEASE, 0, NULL), 0);
    const uint32_t sn[2] = {(uint32_t)vo, pn};
    CHECK_EQ_U32(vcall(r, R_STREAMFOROUT, 2, sn), 0);
    uint32_t vnum = rd16(pn);
    CHECK(vnum != 0);
    const uint32_t rs[2] = {vnum, 0};
    CHECK_EQ_U32(vcall(r, R_SETREADSS, 2, rs), 0);

    /* Áudio: estéreo PCM por padrão; com EnableDiscreteOutput, os 6 canais em WAVEFORMATEXTENSIBLE. */
    uint32_t a4[3] = {(uint32_t)ao, 0, pp};
    CHECK_EQ_U32(vcall(r, R_FMT, 3, a4), 0);
    uint32_t ap = rd32(pp);
    wr32(cb, 0x200);
    q[0] = mt;
    CHECK_EQ_U32(vcall(ap, P_GETMT, 2, q), 0);
    CHECK_EQ_U32(rd32(mt + 16), 1);               /* MEDIASUBTYPE_PCM */
    uint32_t wfx = rd32(mt + 68);
    CHECK_EQ_U32(rd16(wfx), 1);
    CHECK_EQ_U32(rd16(wfx + 2), 2);
    CHECK_EQ_U32(rd32(wfx + 4), 48000);
    CHECK_EQ_U32(rd16(wfx + 14), 16);
    put_wide(name, "EnableDiscreteOutput");
    wr32(val, 1);
    const uint32_t so[5] = {(uint32_t)ao, name, 2 /* WMT_TYPE_BOOL */, val, 4};
    CHECK_EQ_U32(vcall(r, R_SETOUTSET, 5, so), 0);
    put_wide(name, "SpeakerConfig");
    wr32(val, 0x3F);
    CHECK_EQ_U32(vcall(r, R_SETOUTSET, 5, so), 0);
    wr32(cb, 0x200);
    CHECK_EQ_U32(vcall(ap, P_GETMT, 2, q), 0);
    CHECK_EQ_U32(rd16(wfx), 0xFFFE);
    CHECK_EQ_U32(rd16(wfx + 2), 6);
    CHECK_EQ_U32(rd16(wfx + 12), 12);
    CHECK_EQ_U32(rd32(wfx + 20), 0x3F);
    CHECK_EQ_U32(vcall(ap, P_RELEASE, 0, NULL), 0);
    put_wide(name, "VideoSampleDurations");
    const uint32_t sv[5] = {(uint32_t)vo, name, 2, val, 4};
    CHECK_EQ_U32(vcall(r, R_SETOUTSET, 5, sv), 0);

    /* IWMHeaderInfo::GetAttributeByName("Duration"): QWORD em 100 ns. */
    const uint8_t iid_hi[16] = {0xDA, 0x6B, 0x40, 0x96, 0x2B, 0x2B, 0xD3, 0x11,
                                0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF};
    memcpy(re5_g2h(iid), iid_hi, 16);
    const uint32_t qi[2] = {iid, ph};
    CHECK_EQ_U32(vcall(r, R_QI, 2, qi), 0);
    uint32_t h = rd32(ph);
    put_wide(name, "Duration");
    wr16(pn, 0);
    wr16(plen, 8);
    const uint32_t ga[5] = {pn, name, cnt, val, plen};
    CHECK_EQ_U32(vcall(h, H_GETBYNAME, 5, ga), 0);
    CHECK_EQ_U32(rd32(cnt), 4);   /* WMT_TYPE_QWORD */
    uint64_t dur = (uint64_t)rd32(val) | (uint64_t)rd32(val + 4) << 32;
    CHECK(dur > 10000000u && dur < 3000000000u);   /* entre 1 s e 5 min */
    CHECK_EQ_U32(vcall(h, H_RELEASE, 0, NULL), 0);

    /* Lê tudo: vídeo com o tamanho de um quadro YV12, áudio em blocos de 12 bytes, tempos subindo. */
    uint32_t nvid = 0, naud = 0, maxluma = 0, hr;
    uint64_t last_v = 0, last_a = 0, last_t = 0;
    const uint32_t gn[7] = {0, ps, pt, pd, pf, po, pn};
    while ((hr = vcall(r, R_GETNEXT, 7, gn)) == 0) {
        uint32_t s = rd32(ps), o = rd32(po);
        uint64_t t = (uint64_t)rd32(pt) | (uint64_t)rd32(pt + 4) << 32;
        CHECK(rd32(pd) > 0);
        if ((int)o == vo) {
            CHECK_EQ_U32(rd16(pn), vnum);
            const uint32_t gb[1] = {pbuf};
            CHECK_EQ_U32(vcall(s, B_GETBUF, 1, gb), 0);
            CHECK_EQ_U32(vcall(s, B_GETLEN, 1, &plen), 0);
            CHECK_EQ_U32(rd32(plen), 1280 * 720 * 3 / 2);
            if (nvid) CHECK(t > last_v);
            if (nvid == 0) CHECK(rd32(pf) & 1u);   /* o primeiro quadro é chave (cleanpoint) */
            const uint8_t *y = re5_g2h(rd32(pbuf));
            for (uint32_t i = 0; i < 1280 * 720; i += 97) if (y[i] > maxluma) maxluma = y[i];
            last_v = t;
            nvid++;
        } else {
            CHECK_EQ_U32((int)o, ao);
            const uint32_t gb[2] = {pbuf, plen};
            CHECK_EQ_U32(vcall(s, B_GETBUFLEN, 2, gb), 0);
            CHECK(rd32(plen) > 0 && rd32(plen) % 12 == 0);
            if (pcm_out) fwrite(re5_g2h(rd32(pbuf)), 1, rd32(plen), pcm_out);
            if (naud) CHECK(t >= last_a);
            last_a = t;
            naud++;
        }
        if (t > last_t) last_t = t;
        CHECK_EQ_U32(vcall(s, B_RELEASE, 0, NULL), 0);
    }
    CHECK_EQ_U32(hr, 0xC00D0BCFu);   /* NS_E_NO_MORE_SAMPLES */
    printf("wmvcore: %u quadros de vídeo, %u blocos de áudio, último tempo %.2f s, duração %.2f s, luma máx %u\n", nvid,
           naud, last_t / 1e7, dur / 1e7, maxluma);
    CHECK(nvid > 100 && naud > 50);
    CHECK(maxluma > 100);                        /* o logo aparece: não é tudo preto */
    CHECK(last_t + 20000000u >= dur);            /* chegou a menos de 2 s do fim */

    /* SetRange(0, 0): volta ao começo, com descontinuidade. */
    const uint32_t sr[4] = {0, 0, 0, 0};
    CHECK_EQ_U32(vcall(r, R_SETRANGE, 4, sr), 0);
    const uint32_t gv[7] = {vnum, ps, pt, pd, pf, po, pn};
    CHECK_EQ_U32(vcall(r, R_GETNEXT, 7, gv), 0);
    CHECK_EQ_U32(rd32(po), (uint32_t)vo);
    CHECK(rd32(pt + 4) == 0 && rd32(pt) < 10000000u);
    CHECK(rd32(pf) & 2u);   /* WM_SF_DISCONTINUITY */
    CHECK_EQ_U32(vcall(rd32(ps), B_RELEASE, 0, NULL), 0);

    CHECK_EQ_U32(vcall(r, R_RELEASE, 0, NULL), 0);
    CHECK_EQ_U32(g_stream_refs, 1);   /* soltou o IStream */
    fclose(g_file);
    if (pcm_out) fclose(pcm_out);
    CHECK_DONE();
}
