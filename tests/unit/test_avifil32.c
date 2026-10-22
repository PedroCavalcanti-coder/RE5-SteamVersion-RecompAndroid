/*
 * Gravação de AVI como o RE5 faz (0x006645xx): ICCompressorChoose ->
 * AVIFileOpenA -> AVIFileCreateStreamA -> AVIMakeCompressedStream ->
 * AVIStreamSetFormat (o slot que o IAT chama de AVIFileReadData) ->
 * AVIStreamWrite -> Release -> AVIFileExit. O arquivo no disco é conferido:
 * RIFF 'AVI ', avih/strh com os totais, movi com os quadros e idx1.
 * Argumento: pasta nova que faz de C:\RE5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;

static uint32_t api(const char *dll, const char *name, int n, const uint32_t *a)
{
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, re5_import_bind(dll, name, 0), n, a);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

#define FCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
#if defined(_WIN32)
    _mkdir(argv[1]);
#else
    mkdir(argv[1], 0755);
#endif
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    CHECK(re5_vfs_init(argv[1], NULL) == 0);
    uint32_t m = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    uint32_t pc = m, pf = m + 0x40, ps = m + 0x44, pz = m + 0x48, si = m + 0x100, opt = m + 0x200, bih = m + 0x280;
    uint32_t frame = m + 0x300, ns = m + 0x50, nb = m + 0x54, path = m + 0x400;

    api("avifil32.dll", "AVIFileInit", 0, NULL);
    /* COMPVARS como o jogo prepara: cbSize 64, fccHandler 'DIB ' já escolhido. */
    wr32(pc, 64);
    wr32(pc + 16, FCC('D', 'I', 'B', ' '));
    const uint32_t ch[6] = {0x10010u, 3, 0, 0, pc, 0};
    CHECK_EQ_U32(api("msvfw32.dll", "ICCompressorChoose", 6, ch), 1);
    CHECK_EQ_U32(rd32(pc + 4) & 1, 1);                    /* ICMF_COMPVARS_VALID */
    CHECK_EQ_U32(rd32(pc + 16), FCC('D', 'I', 'B', ' '));

    memcpy(re5_g2h(path), "C:\\RE5\\captura.avi", 19);
    const uint32_t op[4] = {pf, path, 0x1041u /* OF_CREATE|OF_SHARE_DENY_WRITE|OF_WRITE */, 0};
    CHECK_EQ_U32(api("avifil32.dll", "AVIFileOpenA", 4, op), 0);
    uint32_t file = rd32(pf);
    CHECK(file != 0);

    /* AVISTREAMINFOA: 'vids', 30 quadros/s, rcFrame 4x2 */
    memset(re5_g2h(si), 0, 140);
    wr32(si, FCC('v', 'i', 'd', 's'));
    wr32(si + 20, 1);
    wr32(si + 24, 30);
    wr32(si + 60, 4);
    wr32(si + 64, 2);
    const uint32_t cs[3] = {file, ps, si};
    CHECK_EQ_U32(api("avifil32.dll", "AVIFileCreateStreamA", 3, cs), 0);
    uint32_t raw = rd32(ps);

    memset(re5_g2h(opt), 0, 44);
    wr32(opt, FCC('v', 'i', 'd', 's'));
    wr32(opt + 4, FCC('X', 'V', 'I', 'D'));
    const uint32_t mc[4] = {pz, raw, opt, 0};
    CHECK_EQ_U32(api("avifil32.dll", "AVIMakeCompressedStream", 4, mc), 0x80044071u);   /* AVIERR_NOCOMPRESSOR */
    wr32(opt + 4, FCC('D', 'I', 'B', ' '));
    CHECK_EQ_U32(api("avifil32.dll", "AVIMakeCompressedStream", 4, mc), 0);
    uint32_t comp = rd32(pz);
    CHECK(comp != 0 && comp != raw);

    /* BITMAPINFOHEADER 4x2, 32 bits */
    memset(re5_g2h(bih), 0, 40);
    wr32(bih, 40);
    wr32(bih + 4, 4);
    wr32(bih + 8, 2);
    wr16(bih + 12, 1);
    wr16(bih + 14, 32);
    wr32(bih + 20, 32);
    const uint32_t sf[4] = {comp, 0, bih, 40};
    CHECK_EQ_U32(api("avifil32.dll", "AVIFileReadData", 4, sf), 0);   /* = AVIStreamSetFormat: slot 7 */

    for (uint32_t k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < 32; i++) wr8(frame + i, (uint8_t)(k * 100 + i));
        const uint32_t w[8] = {comp, k, 1, frame, 32, 0x10, ns, nb};
        CHECK_EQ_U32(api("avifil32.dll", "AVIStreamWrite", 8, w), 0);
        CHECK_EQ_U32(rd32(ns), 1);
        CHECK_EQ_U32(rd32(nb), 32);
    }
    CHECK_EQ_U32(api("avifil32.dll", "AVIStreamRelease", 1, &comp), 0);
    CHECK_EQ_U32(api("avifil32.dll", "AVIStreamRelease", 1, &raw), 0);
    CHECK_EQ_U32(api("avifil32.dll", "AVIFileRelease", 1, &file), 0);   /* grava índice e tamanhos */
    api("avifil32.dll", "AVIFileExit", 0, NULL);
    uint32_t pcv = pc;
    api("msvfw32.dll", "ICCompressorFree", 1, &pcv);

    char host[1024];
    CHECK(re5_vfs_resolve("C:\\RE5\\captura.avi", host, sizeof host) == 0);
    FILE *f = fopen(host, "rb");
    CHECK(f != NULL);
    if (f) {
        static uint8_t b[4096];
        size_t n = fread(b, 1, sizeof b, f);
        fclose(f);
        CHECK_EQ_U32(le32(b), FCC('R', 'I', 'F', 'F'));
        CHECK_EQ_U32(le32(b + 4), n - 8);
        CHECK_EQ_U32(le32(b + 8), FCC('A', 'V', 'I', ' '));
        CHECK_EQ_U32(le32(b + 20), FCC('h', 'd', 'r', 'l'));
        CHECK_EQ_U32(le32(b + 24), FCC('a', 'v', 'i', 'h'));
        CHECK_EQ_U32(le32(b + 32), 33333);         /* microssegundos por quadro a 30/s */
        CHECK_EQ_U32(le32(b + 32 + 16), 2);        /* dwTotalFrames */
        CHECK_EQ_U32(le32(b + 32 + 32), 4);        /* dwWidth */
        CHECK_EQ_U32(le32(b + 32 + 36), 2);        /* dwHeight */
        uint32_t movi = 0, idx = 0;
        for (size_t i = 0; i + 4 <= n; i++) {
            if (!movi && le32(b + i) == FCC('m', 'o', 'v', 'i')) movi = (uint32_t)i;
            if (le32(b + i) == FCC('i', 'd', 'x', '1')) idx = (uint32_t)i;
        }
        CHECK(movi && idx);
        CHECK_EQ_U32(le32(b + movi + 4), FCC('0', '0', 'd', 'b'));
        CHECK_EQ_U32(le32(b + movi + 8), 32);
        CHECK_EQ_U32(b[movi + 12 + 5], 5);                                   /* quadro 0 */
        CHECK_EQ_U32(le32(b + idx + 4), 32);                                 /* 2 entradas de 16 bytes */
        CHECK_EQ_U32(le32(b + idx + 8 + 16 + 8), 4 + 40);                    /* offset do 2º relativo a 'movi' */
        CHECK_EQ_U32(b[movi + le32(b + idx + 8 + 16 + 8) + 8 + 1], 101);     /* quadro 1 */
    }
    CHECK_DONE();
}
