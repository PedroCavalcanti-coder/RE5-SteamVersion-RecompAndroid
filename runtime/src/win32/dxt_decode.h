#ifndef RE5_DXT_DECODE_H
#define RE5_DXT_DECODE_H

#include <stdint.h>
#include <string.h>

/*
 * Decodifica um bloco 4x4 de DXT1, DXT2/3 ou DXT4/5 para RGBA8 ou BGRA8.
 * mode: 1 (DXT1), 3 (DXT2/3), 5 (DXT4/5)
 * bgra: 0 para RGBA (R no byte 0, ex.: D3DFMT_A8B8G8R8), 1 para BGRA (B no byte 0, ex.: D3DFMT_A8R8G8B8)
 */
static inline void re5_dxt_decode_block(int mode, const uint8_t *block, uint32_t width, uint32_t height,
                                        uint32_t bx, uint32_t by, uint8_t *dst, uint32_t dst_pitch, int bgra)
{
    const uint8_t *colors = block + (mode == 1 ? 0 : 8);
    uint16_t endpoints[2];
    memcpy(endpoints, colors, 4);

    uint8_t pal_raw[4][4] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t r = (endpoints[i] >> 11) & 31;
        uint32_t g = (endpoints[i] >> 5) & 63;
        uint32_t b = endpoints[i] & 31;
        pal_raw[i][0] = (uint8_t)((r << 3) | (r >> 2));
        pal_raw[i][1] = (uint8_t)((g << 2) | (g >> 4));
        pal_raw[i][2] = (uint8_t)((b << 3) | (b >> 2));
        pal_raw[i][3] = 255;
    }

    if (mode != 1 || endpoints[0] > endpoints[1]) {
        for (unsigned c = 0; c < 3; ++c) {
            pal_raw[2][c] = (uint8_t)((2 * pal_raw[0][c] + pal_raw[1][c]) / 3);
            pal_raw[3][c] = (uint8_t)((pal_raw[0][c] + 2 * pal_raw[1][c]) / 3);
        }
        pal_raw[2][3] = pal_raw[3][3] = 255;
    } else {
        for (unsigned c = 0; c < 3; ++c) {
            pal_raw[2][c] = (uint8_t)((pal_raw[0][c] + pal_raw[1][c]) / 2);
        }
        pal_raw[2][3] = 255;
        pal_raw[3][0] = pal_raw[3][1] = pal_raw[3][2] = pal_raw[3][3] = 0;
    }

    /* Pré-empacota a paleta em 32 bits (RGB, alfa será mesclado por pixel se mode != 1) */
    uint32_t pal32[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t r = pal_raw[i][0], g = pal_raw[i][1], b = pal_raw[i][2], a = pal_raw[i][3];
        if (bgra) {
            pal32[i] = b | (g << 8) | (r << 16) | (a << 24);
        } else {
            pal32[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }

    uint32_t indices;
    memcpy(&indices, colors + 4, 4);

    uint8_t alpha[8] = {0};
    uint64_t alpha_indices = 0;
    if (mode == 5) {
        alpha[0] = block[0];
        alpha[1] = block[1];
        if (alpha[0] > alpha[1]) {
            for (unsigned i = 1; i <= 6; ++i)
                alpha[i + 1] = (uint8_t)(((7 - i) * alpha[0] + i * alpha[1]) / 7);
        } else {
            for (unsigned i = 1; i <= 4; ++i)
                alpha[i + 1] = (uint8_t)(((5 - i) * alpha[0] + i * alpha[1]) / 5);
            alpha[6] = 0;
            alpha[7] = 255;
        }
        for (unsigned i = 0; i < 6; ++i)
            alpha_indices |= (uint64_t)block[2 + i] << (8 * i);
    }

    /* Fast-path para blocos 4x4 completos dentro dos limites */
    if (bx * 4 + 4 <= width && by * 4 + 4 <= height) {
        for (uint32_t row = 0; row < 4; ++row) {
            uint32_t *dst_row = (uint32_t *)(dst + (size_t)(by * 4 + row) * dst_pitch + (size_t)(bx * 4) * 4);
            if (mode == 1) {
                dst_row[0] = pal32[indices & 3];
                dst_row[1] = pal32[(indices >> 2) & 3];
                dst_row[2] = pal32[(indices >> 4) & 3];
                dst_row[3] = pal32[(indices >> 6) & 3];
            } else if (mode == 3) {
                uint16_t row_alpha = (uint16_t)block[row * 2] | ((uint16_t)block[row * 2 + 1] << 8);
                uint32_t a0 = ((row_alpha) & 0xF) * 17;
                uint32_t a1 = ((row_alpha >> 4) & 0xF) * 17;
                uint32_t a2 = ((row_alpha >> 8) & 0xF) * 17;
                uint32_t a3 = ((row_alpha >> 12) & 0xF) * 17;
                dst_row[0] = (pal32[indices & 3] & 0x00FFFFFFu) | (a0 << 24);
                dst_row[1] = (pal32[(indices >> 2) & 3] & 0x00FFFFFFu) | (a1 << 24);
                dst_row[2] = (pal32[(indices >> 4) & 3] & 0x00FFFFFFu) | (a2 << 24);
                dst_row[3] = (pal32[(indices >> 6) & 3] & 0x00FFFFFFu) | (a3 << 24);
            } else { /* mode == 5 */
                uint32_t a0 = (uint32_t)alpha[alpha_indices & 7];
                uint32_t a1 = (uint32_t)alpha[(alpha_indices >> 3) & 7];
                uint32_t a2 = (uint32_t)alpha[(alpha_indices >> 6) & 7];
                uint32_t a3 = (uint32_t)alpha[(alpha_indices >> 9) & 7];
                alpha_indices >>= 12;
                dst_row[0] = (pal32[indices & 3] & 0x00FFFFFFu) | (a0 << 24);
                dst_row[1] = (pal32[(indices >> 2) & 3] & 0x00FFFFFFu) | (a1 << 24);
                dst_row[2] = (pal32[(indices >> 4) & 3] & 0x00FFFFFFu) | (a2 << 24);
                dst_row[3] = (pal32[(indices >> 6) & 3] & 0x00FFFFFFu) | (a3 << 24);
            }
            indices >>= 8;
        }
        return;
    }

    /* Fallback com recorte para bordas parciais */
    for (unsigned p = 0; p < 16; ++p) {
        uint32_t x = bx * 4 + (p & 3);
        uint32_t y = by * 4 + (p >> 2);
        if (x >= width || y >= height) continue;

        uint8_t c_idx = (uint8_t)((indices >> (p * 2)) & 3);
        uint32_t color = pal32[c_idx];

        if (mode == 3) {
            uint32_t a = (uint32_t)(((block[p / 2] >> ((p & 1) * 4)) & 15) * 17);
            color = (color & 0x00FFFFFFu) | (a << 24);
        } else if (mode == 5) {
            uint32_t a = (uint32_t)alpha[(alpha_indices >> (p * 3)) & 7];
            color = (color & 0x00FFFFFFu) | (a << 24);
        }

        uint32_t *pixel = (uint32_t *)(dst + (size_t)y * dst_pitch + (size_t)x * 4);
        *pixel = color;
    }
}

static inline void re5_dxt_decode(int mode, uint32_t width, uint32_t height,
                                  const uint8_t *source, uint32_t src_pitch,
                                  uint8_t *dst, uint32_t dst_pitch, int bgra)
{
    uint32_t blocks_wide = (width + 3) / 4;
    uint32_t blocks_high = (height + 3) / 4;
    uint32_t block_bytes = (mode == 1) ? 8 : 16;

    for (uint32_t by = 0; by < blocks_high; ++by) {
        const uint8_t *row = source + (size_t)by * src_pitch;
        for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
            re5_dxt_decode_block(mode, row + (size_t)bx * block_bytes, width, height, bx, by, dst, dst_pitch, bgra);
        }
    }
}

#endif /* RE5_DXT_DECODE_H */
