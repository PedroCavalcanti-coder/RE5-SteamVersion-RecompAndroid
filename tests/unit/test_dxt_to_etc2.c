/*
 * test_dxt_to_etc2.c - transcodificação DXT1/3/5 -> ETC2/EAC (runtime/src/gfx/vk/re5_dxt_to_etc2.h),
 * usada no Mali (sem textureCompressionBC).
 *
 * Cada bloco gerado é decodificado por um decodificador de referência escrito aqui a partir da
 * especificação (Khronos Data Format, ETC1/ETC2 e EAC) e comparado com a decodificação exata do
 * DXT (runtime/src/win32/dxt_decode.h):
 *  - alpha de 1 bit do DXT1 (folhas, cercas, cabelos): tem que sair exato (0 ou 255);
 *  - alpha do DXT3/DXT5: erro pequeno e os extremos 0/255 exatos;
 *  - cor: erro médio pequeno (o sinal dos modificadores trocado dava erro grande e imagem suja).
 * E o cache em disco: grava e lê de volta.
 */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "../../runtime/src/gfx/vk/re5_dxt_to_etc2.h"
#include "../../runtime/src/gfx/vk/re5_tex_cache.h"
#include "../../runtime/src/win32/dxt_decode.h"

/* ---- decodificador de referência (especificação, independente do codificador) ---- */

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ETC1/ETC2 RGB, modos individual e diferencial. Devolve 0 se o bloco cair em T/H/planar
 * (o codificador nunca deve gerá-los). out[y*4+x][c]. */
static int ref_etc_rgb(const uint8_t *b, uint8_t out[16][3])
{
    static const int tab[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    int diff = (b[3] >> 1) & 1, flip = b[3] & 1;
    int base[2][3];
    for (int c = 0; c < 3; c++) {
        if (diff) {
            int v = b[c] >> 3, d = b[c] & 7;
            if (d >= 4) d -= 8;
            int v1 = v + d;
            if (v1 < 0 || v1 > 31) return 0;
            base[0][c] = (v << 3) | (v >> 2);
            base[1][c] = (v1 << 3) | (v1 >> 2);
        } else {
            base[0][c] = (b[c] >> 4) * 17;
            base[1][c] = (b[c] & 15) * 17;
        }
    }
    int t[2] = {b[3] >> 5, (b[3] >> 2) & 7};
    unsigned msb = ((unsigned)b[4] << 8) | b[5], lsb = ((unsigned)b[6] << 8) | b[7];
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y, sub = flip ? (y >= 2) : (x >= 2);
            int sel = (int)(((msb >> i) & 1) << 1 | ((lsb >> i) & 1));
            /* 00 -> +a, 01 -> +b, 10 -> -a, 11 -> -b */
            int a = tab[t[sub]][0], bb = tab[t[sub]][1];
            int mod = sel == 0 ? a : sel == 1 ? bb : sel == 2 ? -a : -bb;
            for (int c = 0; c < 3; c++) out[y * 4 + x][c] = (uint8_t)clamp255(base[sub][c] + mod);
        }
    return 1;
}

/* EAC de 8 bits (alpha do ETC2 RGBA8). out[y*4+x]. */
static void ref_eac(const uint8_t *b, uint8_t out[16])
{
    static const int tab[16][8] = {
        {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
        {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
        {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
        {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
        {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
        {-3, -5, -7, -9, 2, 4, 6, 8}};
    int base = b[0], mult = b[1] >> 4, t = b[1] & 15;
    uint64_t bits = 0;
    for (int i = 2; i < 8; i++) bits = (bits << 8) | b[i];
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y;
            int sel = (int)((bits >> (45 - 3 * i)) & 7);
            out[y * 4 + x] = (uint8_t)clamp255(base + tab[t][sel] * mult);
        }
}

/* DXT exato (decodificador da CPU), um bloco, RGBA. */
static void ref_dxt(int mode, const uint8_t *blk, uint8_t out[16][4])
{
    re5_dxt_decode_block(mode, blk, 4, 4, 0, 0, &out[0][0], 16, 0);
}

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* Bloco de cor DXT "de textura": dois extremos próximos (como em fotos/pinturas), índices livres. */
static void rand_color_block(uint8_t *blk, int punch)
{
    int r = (int)(rnd() % 32), g = (int)(rnd() % 64), bl = (int)(rnd() % 32);
    int r1 = clamp255(r + (int)(rnd() % 9) - 4) & 31, g1 = clamp255(g + (int)(rnd() % 17) - 8) & 63,
        b1 = clamp255(bl + (int)(rnd() % 9) - 4) & 31;
    uint16_t c0 = (uint16_t)(r << 11 | g << 5 | bl), c1 = (uint16_t)(r1 << 11 | g1 << 5 | b1);
    if ((c0 > c1) == !!punch) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (!punch && c0 == c1) c0 = (uint16_t)(c1 + 1);
    memcpy(blk, &c0, 2);
    memcpy(blk + 2, &c1, 2);
    uint32_t idx = rnd();
    memcpy(blk + 4, &idx, 4);
}

int main(void)
{
    /* 1. DXT1 com alpha de 1 bit -> ETC2 RGBA8: alpha exato, cor próxima nos opacos */
    {
        double err = 0;
        int n = 0, rgb_ok = 1, alpha_bad = 0;
        for (int k = 0; k < 4000; k++) {
            uint8_t dxt[8], etc[16], a[16], rgb[16][3], ref[16][4];
            rand_color_block(dxt, 1);
            re5_dxt1_to_etc2_rgba8_block(dxt, etc);
            ref_eac(etc, a);
            rgb_ok &= ref_etc_rgb(etc + 8, rgb);
            ref_dxt(1, dxt, ref);
            for (int p = 0; p < 16; p++) {
                if (a[p] != ref[p][3]) alpha_bad++;
                if (ref[p][3])
                    for (int c = 0; c < 3; c++, n++) err += abs((int)rgb[p][c] - (int)ref[p][c]);
            }
        }
        CHECK(rgb_ok);
        CHECK_EQ_U32(alpha_bad, 0);
        printf("DXT1 1-bit: erro médio de cor %.2f\n", err / n);
        CHECK(err / n < 8.0); /* ruído de matiz por pixel: o ETC1 só desloca o brilho (sinal trocado dava 13) */
    }

    /* 2. DXT1 opaco -> ETC2 RGB8 e RGBA8 (alpha 255 em todo pixel) */
    {
        double err = 0;
        int n = 0, ok = 1, alpha_bad = 0;
        for (int k = 0; k < 4000; k++) {
            uint8_t dxt[8], etc[8], etca[16], a[16], rgb[16][3], ref[16][4];
            rand_color_block(dxt, 0);
            re5_dxt1_to_etc2_block(dxt, etc);
            ok &= ref_etc_rgb(etc, rgb);
            ref_dxt(1, dxt, ref);
            for (int p = 0; p < 16; p++)
                for (int c = 0; c < 3; c++, n++) err += abs((int)rgb[p][c] - (int)ref[p][c]);
            re5_dxt1_to_etc2_rgba8_block(dxt, etca);
            ref_eac(etca, a);
            for (int p = 0; p < 16; p++) alpha_bad += a[p] != 255;
        }
        CHECK(ok);
        CHECK_EQ_U32(alpha_bad, 0);
        printf("DXT1 opaco: erro médio de cor %.2f\n", err / n);
        CHECK(err / n < 8.0); /* ruído de matiz por pixel: o ETC1 só desloca o brilho (sinal trocado dava 13) */
    }

    /* 3. Cor uniforme: tem que sair quase exata (o caso mais comum em blocos de fundo) */
    {
        int worst = 0;
        for (int k = 0; k < 2000; k++) {
            uint8_t dxt[8], etc[8], rgb[16][3], ref[16][4];
            uint16_t c = (uint16_t)rnd(), c1 = 0;
            memcpy(dxt, &c, 2);
            memcpy(dxt + 2, &c1, 2);
            memset(dxt + 4, 0, 4); /* todos no índice 0 */
            re5_dxt1_to_etc2_block(dxt, etc);
            CHECK(ref_etc_rgb(etc, rgb));
            ref_dxt(1, dxt, ref);
            for (int p = 0; p < 16; p++)
                for (int ch = 0; ch < 3; ch++) {
                    int e = abs((int)rgb[p][ch] - (int)ref[p][ch]);
                    if (e > worst) worst = e;
                }
        }
        printf("cor uniforme: pior erro %d\n", worst);
        CHECK(worst <= 6);
    }

    /* 3b. Gradiente só de brilho (cinza): é o que o ETC1 representa bem */
    {
        double err = 0;
        int n = 0;
        for (int k = 0; k < 2000; k++) {
            uint8_t dxt[8], etc[8], rgb[16][3], ref[16][4];
            int v = (int)(rnd() % 26), d = 1 + (int)(rnd() % 6);
            uint16_t c0 = (uint16_t)((v + d) << 11 | (2 * (v + d)) << 5 | (v + d)), c1 = (uint16_t)(v << 11 | (2 * v) << 5 | v);
            uint32_t idx = rnd();
            memcpy(dxt, &c0, 2);
            memcpy(dxt + 2, &c1, 2);
            memcpy(dxt + 4, &idx, 4);
            re5_dxt1_to_etc2_block(dxt, etc);
            CHECK(ref_etc_rgb(etc, rgb));
            ref_dxt(1, dxt, ref);
            for (int p = 0; p < 16; p++)
                for (int c = 0; c < 3; c++, n++) err += abs((int)rgb[p][c] - (int)ref[p][c]);
        }
        printf("gradiente de brilho: erro médio %.2f\n", err / n);
        CHECK(err / n < 4.0);
    }

    /* 4. DXT5 -> EAC: erro de alpha pequeno, extremos exatos, e o recorte em 0.5 (alpha test)
     *    concorda com o DXT fora de uma faixa estreita em volta do corte */
    {
        double err = 0;
        int n = 0, ext_bad = 0, cut_bad = 0;
        for (int k = 0; k < 4000; k++) {
            uint8_t dxt[16], etc[16], a[16], ref[16][4];
            dxt[0] = (uint8_t)rnd();
            dxt[1] = (uint8_t)rnd();
            if (k % 4 == 0) { dxt[0] = 255; dxt[1] = 0; }
            if (k % 4 == 1) { dxt[0] = 0; dxt[1] = 255; } /* modo de 6 níveis com 0 e 255 explícitos */
            for (int i = 2; i < 8; i++) dxt[i] = (uint8_t)rnd();
            rand_color_block(dxt + 8, 0);
            re5_dxt5_to_etc2_block(dxt, etc);
            ref_eac(etc, a);
            ref_dxt(5, dxt, ref);
            for (int p = 0; p < 16; p++, n++) {
                int r = ref[p][3], e = abs((int)a[p] - r);
                err += e;
                if ((r == 0 || r == 255) && e) ext_bad++;
                if ((r < 112 && a[p] >= 128) || (r > 144 && a[p] < 128)) cut_bad++;
            }
        }
        printf("DXT5: erro médio de alpha %.2f, extremos errados %d, recorte errado %d\n", err / n, ext_bad, cut_bad);
        CHECK(err / n < 4.0);
        CHECK_EQ_U32(ext_bad, 0);
        CHECK_EQ_U32(cut_bad, 0);
    }

    /* 5. DXT3 -> EAC (alpha explícito de 4 bits) */
    {
        double err = 0;
        int n = 0, ext_bad = 0;
        for (int k = 0; k < 4000; k++) {
            uint8_t dxt[16], etc[16], a[16], ref[16][4];
            for (int i = 0; i < 8; i++) dxt[i] = (uint8_t)rnd();
            rand_color_block(dxt + 8, 0);
            re5_dxt3_to_etc2_block(dxt, etc);
            ref_eac(etc, a);
            ref_dxt(3, dxt, ref);
            for (int p = 0; p < 16; p++, n++) {
                int r = ref[p][3], e = abs((int)a[p] - r);
                err += e;
                if ((r == 0 || r == 255) && e) ext_bad++;
            }
        }
        printf("DXT3: erro médio de alpha %.2f, extremos errados %d\n", err / n, ext_bad);
        CHECK(err / n < 8.0);
        CHECK_EQ_U32(ext_bad, 0);
    }

    /* 6. Superfície 8x8 em DXT1 com alpha (passo de 16 bytes por bloco no destino = RGBA8) e
     *    mip 2x2 (bloco parcial) */
    {
        uint8_t src[4 * 8], dst[4 * 16];
        for (int i = 0; i < 4; i++) rand_color_block(src + i * 8, 1);
        re5_dxt_to_etc2_surface(1, 8, 8, src, 16, dst, 32);
        for (int i = 0; i < 4; i++) {
            uint8_t want[16];
            uint8_t a[16];
            re5_dxt1_to_etc2_rgba8_block(src + i * 8, want);
            CHECK(memcmp(dst + i * 16, want, 16) == 0);
            ref_eac(dst + i * 16, a);
        }
        uint8_t s2[8], d2[16];
        rand_color_block(s2, 1);
        re5_dxt_to_etc2_surface(1, 2, 2, s2, 8, d2, 16);
        uint8_t want[16];
        re5_dxt1_to_etc2_rgba8_block(s2, want);
        CHECK(memcmp(d2, want, 16) == 0);
    }

    /* 7. Cache persistente (pasta em RE5VK_TEX_CACHE_DIR, definida pelo CTest) */
    {
        uint8_t dxt[64], conv[64], back[64];
        for (int i = 0; i < 64; i++) dxt[i] = (uint8_t)(i * 7), conv[i] = (uint8_t)(i ^ 0xAA);
        uint64_t hash = re5_tex_calc_hash(16, 16, 1, 0x31545844, dxt, sizeof dxt);
        CHECK(hash != 0);
        re5_tex_cache_save(hash, 16, 16, 1, 0x31545844, conv, sizeof conv);
        memset(back, 0, sizeof back);
        CHECK_EQ_U32(re5_tex_cache_load(hash, sizeof back, back), 1);
        CHECK(memcmp(conv, back, sizeof conv) == 0);
        CHECK_EQ_U32(re5_tex_cache_load(hash, sizeof back - 8, back), 0); /* tamanho diferente: recusa */
    }

    CHECK_DONE();
}
