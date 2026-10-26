/*
 * test_dxt_decode.c - Teste unitário da decodificação de texturas DXT1/3/5 para RGBA8/BGRA8.
 */
#include "check.h"
#include "../../runtime/src/win32/dxt_decode.h"

int main(void)
{
    /* 1. DXT1 com 4 cores (c0 > c1): Vermelho e Azul */
    {
        uint8_t block[8];
        uint16_t c0 = 0xF800;   /* Vermelho puro: r=31, g=0, b=0 */
        uint16_t c1 = 0x001F;   /* Azul puro: r=0, g=0, b=31 */
        memcpy(block, &c0, 2);
        memcpy(block + 2, &c1, 2);
        /* 16 índices de 2 bits:
         * p0 = 0 (c0), p1 = 1 (c1), p2 = 2 (2/3 c0 + 1/3 c1), p3 = 3 (1/3 c0 + 2/3 c1) */
        uint32_t indices = 0xE4;   /* binário: 11 10 01 00 */
        memcpy(block + 4, &indices, 4);

        uint8_t rgba[4 * 4 * 4];
        re5_dxt_decode(1, 4, 4, block, 8, rgba, 16, 0);

        /* Pixel 0: Vermelho puro */
        CHECK_EQ_U32(rgba[0], 255);
        CHECK_EQ_U32(rgba[1], 0);
        CHECK_EQ_U32(rgba[2], 0);
        CHECK_EQ_U32(rgba[3], 255);

        /* Pixel 1: Azul puro */
        CHECK_EQ_U32(rgba[4], 0);
        CHECK_EQ_U32(rgba[5], 0);
        CHECK_EQ_U32(rgba[6], 255);
        CHECK_EQ_U32(rgba[7], 255);

        /* Pixel 2: 2/3 c0 + 1/3 c1 -> R=170, G=0, B=85 */
        CHECK_EQ_U32(rgba[8], 170);
        CHECK_EQ_U32(rgba[9], 0);
        CHECK_EQ_U32(rgba[10], 85);
        CHECK_EQ_U32(rgba[11], 255);

        /* Pixel 3: 1/3 c0 + 2/3 c1 -> R=85, G=0, B=170 */
        CHECK_EQ_U32(rgba[12], 85);
        CHECK_EQ_U32(rgba[13], 0);
        CHECK_EQ_U32(rgba[14], 170);
        CHECK_EQ_U32(rgba[15], 255);

        /* Formato BGRA (D3DFMT_A8R8G8B8) */
        uint8_t bgra[4 * 4 * 4];
        re5_dxt_decode(1, 4, 4, block, 8, bgra, 16, 1);
        CHECK_EQ_U32(bgra[0], 0);     /* B */
        CHECK_EQ_U32(bgra[1], 0);     /* G */
        CHECK_EQ_U32(bgra[2], 255);   /* R */
        CHECK_EQ_U32(bgra[3], 255);   /* A */
    }

    /* 2. DXT1 com transparência de 1 bit (c0 <= c1) */
    {
        uint8_t block[8];
        uint16_t c0 = 0x001F;   /* Azul */
        uint16_t c1 = 0xF800;   /* Vermelho */
        memcpy(block, &c0, 2);
        memcpy(block + 2, &c1, 2);
        uint32_t indices = 0xC0;   /* p0 = 0, p1 = 0, p2 = 0, p3 = 3 (transparente) */
        memcpy(block + 4, &indices, 4);

        uint8_t rgba[4 * 4 * 4];
        re5_dxt_decode(1, 4, 4, block, 8, rgba, 16, 0);

        /* Pixel 3: Transparente (alfa 0) */
        CHECK_EQ_U32(rgba[15], 0);
    }

    /* 3. DXT3 com alfa explícito de 4 bits */
    {
        uint8_t block[16];
        /* 8 bytes de alfa: cada byte tem 2 pixels (nibbles) */
        memset(block, 0, 8);
        block[0] = 0xFA;   /* p0 = 0xA (10*17 = 170), p1 = 0xF (15*17 = 255) */
        block[1] = 0x05;   /* p2 = 0x5 (5*17 = 85), p3 = 0x0 (0) */

        uint16_t c0 = 0xF800, c1 = 0x001F;
        memcpy(block + 8, &c0, 2);
        memcpy(block + 10, &c1, 2);
        uint32_t indices = 0;
        memcpy(block + 12, &indices, 4);

        uint8_t rgba[4 * 4 * 4];
        re5_dxt_decode(3, 4, 4, block, 16, rgba, 16, 0);

        CHECK_EQ_U32(rgba[3], 170);
        CHECK_EQ_U32(rgba[7], 255);
        CHECK_EQ_U32(rgba[11], 85);
        CHECK_EQ_U32(rgba[15], 0);
    }

    /* 4. DXT5 com alfa interpolado */
    {
        uint8_t block[16];
        block[0] = 255;   /* alpha_0 */
        block[1] = 0;     /* alpha_1 (alpha_0 > alpha_1: 8 valores interpolados) */
        /* Índices de 3 bits: p0=0 (255), p1=1 (0), p2=2 ((6*255)/7 = 218), p3=7 ((1*255)/7 = 36) */
        /* p0=0 (000), p1=1 (001), p2=2 (010), p3=7 (111) -> 111 010 001 000 = 0xE88 */
        uint64_t a_idx = 0xE88;
        memcpy(block + 2, &a_idx, 6);

        uint16_t c0 = 0xF800, c1 = 0x001F;
        memcpy(block + 8, &c0, 2);
        memcpy(block + 10, &c1, 2);
        uint32_t indices = 0;
        memcpy(block + 12, &indices, 4);

        uint8_t rgba[4 * 4 * 4];
        re5_dxt_decode(5, 4, 4, block, 16, rgba, 16, 0);

        CHECK_EQ_U32(rgba[3], 255);
        CHECK_EQ_U32(rgba[7], 0);
        CHECK_EQ_U32(rgba[11], 218);
        CHECK_EQ_U32(rgba[15], 36);
    }

    /* 5. Dimensões menores que o bloco 4x4 (ex.: mip level 2x2) */
    {
        uint8_t block[16];
        block[0] = 200;
        block[1] = 100;
        memset(block + 2, 0, 6);
        uint16_t c0 = 0x07E0, c1 = 0x07E0;   /* Verde */
        memcpy(block + 8, &c0, 2);
        memcpy(block + 10, &c1, 2);
        memset(block + 12, 0, 4);

        uint8_t rgba[2 * 2 * 4];
        memset(rgba, 0xCC, sizeof rgba);
        re5_dxt_decode(5, 2, 2, block, 16, rgba, 8, 0);

        /* (0,0), (1,0), (0,1), (1,1) são escritos */
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                uint8_t *px = rgba + y * 8 + x * 4;
                CHECK_EQ_U32(px[0], 0);     /* R */
                CHECK_EQ_U32(px[1], 255);   /* G */
                CHECK_EQ_U32(px[2], 0);     /* B */
                CHECK_EQ_U32(px[3], 200);   /* A */
            }
        }
    }

    CHECK_DONE();
}
