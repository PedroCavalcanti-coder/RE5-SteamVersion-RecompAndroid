/*
 * re5_dxt_to_etc2.h - Transcodificador ultrarrápido de blocos DXT (BC1/BC2/BC3) para ETC2 / EAC.
 *
 * Utilizado como alternativa de alto desempenho em GPUs mobile ARM Mali (onde textureCompressionBC
 * é VK_FALSE, mas textureCompressionETC2 é VK_TRUE obrigatório pela especificação Vulkan).
 *
 * Benefícios sobre descompressão para RGBA8:
 *  - DXT1 -> ETC2 RGB8: exatamente o mesmo tamanho (4 bits/pixel, 8 bytes por bloco 4x4).
 *  - DXT3/5 -> ETC2 RGBA8 (EAC): exatamente o mesmo tamanho (8 bits/pixel, 16 bytes por bloco 4x4).
 *  - Economia de 75% a 87.5% de memória VRAM e largura de banda de amostragem na GPU Mali.
 *  - Descompressão em silício nativo pelas TPUs do Mali, sem sobrecarga de Fragment Shaders.
 */
#ifndef RE5_DXT_TO_ETC2_H
#define RE5_DXT_TO_ETC2_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tabela padrão de modificadores de intensidade ETC1/ETC2 */
static const int16_t g_etc2_mod_table[8][4] = {
    {  2,   8,  -2,   -8 },
    {  5,  17,  -5,  -17 },
    {  9,  29,  -9,  -29 },
    { 13,  42, -13,  -42 },
    { 18,  60, -18,  -60 },
    { 24,  80, -24,  -80 },
    { 33, 106, -33, -106 },
    { 47, 183, -47, -183 }
};

/* Tabela padrão de modificadores EAC para o canal Alpha do ETC2 RGBA8 */
static const int8_t g_eac_mod_table[16][8] = {
    { -3, -6,  -9, -15, 2, 5,  8, 14 },
    { -3, -7, -10, -13, 2, 6,  9, 12 },
    { -2, -5,  -8, -13, 1, 4,  7, 12 },
    { -2, -4,  -6, -13, 1, 3,  5, 12 },
    { -3, -6,  -8, -12, 2, 5,  7, 11 },
    { -3, -7,  -9, -11, 2, 6,  8, 10 },
    { -4, -7,  -8, -11, 3, 6,  7, 10 },
    { -3, -5,  -8, -11, 2, 4,  7, 10 },
    { -2, -6,  -8, -10, 1, 5,  7,  9 },
    { -2, -5,  -8, -10, 1, 4,  7,  9 },
    { -2, -4,  -8, -10, 1, 3,  7,  9 },
    { -2, -5,  -7, -10, 1, 4,  6,  9 },
    { -3, -4,  -7, -10, 2, 3,  6,  9 },
    { -1, -2,  -3, -10, 0, 1,  2,  9 },
    { -4, -6,  -8,  -9, 3, 5,  7,  8 },
    { -3, -5,  -7,  -9, 2, 4,  6,  8 }
};

static inline uint8_t re5_clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Transcodifica os 8 bytes de cor DXT1 de um bloco 4x4 para 8 bytes de ETC2 RGB8 */
static inline void re5_dxt1_to_etc2_block(const uint8_t *dxt, uint8_t *etc2)
{
    uint16_t c0 = (uint16_t)dxt[0] | ((uint16_t)dxt[1] << 8);
    uint16_t c1 = (uint16_t)dxt[2] | ((uint16_t)dxt[3] << 8);
    uint32_t indices = (uint32_t)dxt[4] | ((uint32_t)dxt[5] << 8) |
                       ((uint32_t)dxt[6] << 16) | ((uint32_t)dxt[7] << 24);

    /* Descompacta as cores da paleta DXT1 em RGB8 */
    uint8_t pal[4][3];
    uint32_t r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
    uint32_t r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    pal[0][0] = (uint8_t)((r0 << 3) | (r0 >> 2));
    pal[0][1] = (uint8_t)((g0 << 2) | (g0 >> 4));
    pal[0][2] = (uint8_t)((b0 << 3) | (b0 >> 2));
    pal[1][0] = (uint8_t)((r1 << 3) | (r1 >> 2));
    pal[1][1] = (uint8_t)((g1 << 2) | (g1 >> 4));
    pal[1][2] = (uint8_t)((b1 << 3) | (b1 >> 2));

    if (c0 > c1) {
        for (int c = 0; c < 3; c++) {
            pal[2][c] = (uint8_t)((2 * pal[0][c] + pal[1][c]) / 3);
            pal[3][c] = (uint8_t)((pal[0][c] + 2 * pal[1][c]) / 3);
        }
    } else {
        for (int c = 0; c < 3; c++) {
            pal[2][c] = (uint8_t)((pal[0][c] + pal[1][c]) / 2);
        }
        /* Em DXT1 com c0 <= c1, texels transparentes usam pal[2] na reconstrução de cor
         * para não contaminar a média dos sub-blocos com preto, eliminando bordas pretas (halos). */
        pal[3][0] = pal[2][0];
        pal[3][1] = pal[2][1];
        pal[3][2] = pal[2][2];
    }

    /* Reconstrói os 16 pixels do bloco 4x4 em RGB */
    uint8_t rgb[16][3];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int p = y * 4 + x;
            int idx = (indices >> (p * 2)) & 3;
            rgb[p][0] = pal[idx][0];
            rgb[p][1] = pal[idx][1];
            rgb[p][2] = pal[idx][2];
        }
    }

    /* Otimização rápida: testa partição horizontal (flip=1) e vertical (flip=0) */
    int best_flip = 0;
    int best_diff = 1;
    uint8_t best_r0 = 0, best_g0 = 0, best_b0 = 0;
    int8_t best_dr = 0, best_dg = 0, best_db = 0;
    uint8_t best_r1 = 0, best_g1 = 0, best_b1 = 0;
    int best_t0 = 0, best_t1 = 0;
    uint16_t best_msb = 0, best_lsb = 0;
    int32_t best_err = 0x7FFFFFFF;

    for (int flip = 0; flip < 2; flip++) {
        /* Sub-bloco 0 e 1 */
        int sum_rgb[2][3] = {{0}};
        int count[2] = {8, 8};

        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int p = y * 4 + x;
                int sub = flip ? (y >= 2) : (x >= 2);
                sum_rgb[sub][0] += rgb[p][0];
                sum_rgb[sub][1] += rgb[p][1];
                sum_rgb[sub][2] += rgb[p][2];
            }
        }

        uint8_t avg[2][3];
        for (int s = 0; s < 2; s++) {
            avg[s][0] = (uint8_t)(sum_rgb[s][0] / count[s]);
            avg[s][1] = (uint8_t)(sum_rgb[s][1] / count[s]);
            avg[s][2] = (uint8_t)(sum_rgb[s][2] / count[s]);
        }

        /* Testa modo diferencial (5 bits de base + 3 bits de delta: -4..+3) */
        /* quantização arredondada (o >> 3 truncava e puxava tudo para baixo) */
        int r0_5 = (avg[0][0] * 31 + 127) / 255;
        int g0_5 = (avg[0][1] * 31 + 127) / 255;
        int b0_5 = (avg[0][2] * 31 + 127) / 255;
        int r1_5 = (avg[1][0] * 31 + 127) / 255;
        int g1_5 = (avg[1][1] * 31 + 127) / 255;
        int b1_5 = (avg[1][2] * 31 + 127) / 255;

        int dr = r1_5 - r0_5;
        int dg = g1_5 - g0_5;
        int db = b1_5 - b0_5;

        int diff = (dr >= -4 && dr <= 3 && dg >= -4 && dg <= 3 && db >= -4 && db <= 3);

        uint8_t base_c[2][3];
        int q4[2][3] = {{0}};
        if (diff) {
            base_c[0][0] = (uint8_t)((r0_5 << 3) | (r0_5 >> 2));
            base_c[0][1] = (uint8_t)((g0_5 << 3) | (g0_5 >> 2));
            base_c[0][2] = (uint8_t)((b0_5 << 3) | (b0_5 >> 2));

            int r1_adj = r0_5 + dr;
            int g1_adj = g0_5 + dg;
            int b1_adj = b0_5 + db;
            base_c[1][0] = (uint8_t)((r1_adj << 3) | (r1_adj >> 2));
            base_c[1][1] = (uint8_t)((g1_adj << 3) | (g1_adj >> 2));
            base_c[1][2] = (uint8_t)((b1_adj << 3) | (b1_adj >> 2));
        } else {
            /* Modo individual (4 bits para cada sub-bloco) */
            int r0_4 = (avg[0][0] * 15 + 127) / 255;
            int g0_4 = (avg[0][1] * 15 + 127) / 255;
            int b0_4 = (avg[0][2] * 15 + 127) / 255;
            int r1_4 = (avg[1][0] * 15 + 127) / 255;
            int g1_4 = (avg[1][1] * 15 + 127) / 255;
            int b1_4 = (avg[1][2] * 15 + 127) / 255;
            q4[0][0] = r0_4; q4[0][1] = g0_4; q4[0][2] = b0_4;
            q4[1][0] = r1_4; q4[1][1] = g1_4; q4[1][2] = b1_4;

            base_c[0][0] = (uint8_t)((r0_4 << 4) | r0_4);
            base_c[0][1] = (uint8_t)((g0_4 << 4) | g0_4);
            base_c[0][2] = (uint8_t)((b0_4 << 4) | b0_4);
            base_c[1][0] = (uint8_t)((r1_4 << 4) | r1_4);
            base_c[1][1] = (uint8_t)((g1_4 << 4) | g1_4);
            base_c[1][2] = (uint8_t)((b1_4 << 4) | b1_4);
        }

        /* Seleção da melhor tabela de modificador para cada sub-bloco */
        int cur_t[2] = {0, 0};
        uint16_t cur_msb = 0, cur_lsb = 0;
        int32_t total_err = 0;

        for (int s = 0; s < 2; s++) {
            int best_sub_t = 0;
            int32_t best_sub_err = 0x7FFFFFFF;
            uint8_t best_mod_idx[8];

            for (int t = 0; t < 8; t++) {
                int32_t t_err = 0;
                uint8_t tmp_mod_idx[8];
                int px_count = 0;

                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) {
                        int p = y * 4 + x;
                        int sub = flip ? (y >= 2) : (x >= 2);
                        if (sub != s) continue;

                        int pr = rgb[p][0], pg = rgb[p][1], pb = rgb[p][2];
                        int min_mod_err = 0x7FFFFFFF;
                        int best_m = 0;

                        /* 4 modificadores da tabela t: +a, +b, -a, -b */
                        for (int m = 0; m < 4; m++) {
                            int mod = g_etc2_mod_table[t][m];
                            int cr = re5_clamp_u8(base_c[s][0] + mod);
                            int cg = re5_clamp_u8(base_c[s][1] + mod);
                            int cb = re5_clamp_u8(base_c[s][2] + mod);
                            int dr_p = pr - cr, dg_p = pg - cg, db_p = pb - cb;
                            int err = dr_p * dr_p + dg_p * dg_p + db_p * db_p;
                            if (err < min_mod_err) {
                                min_mod_err = err;
                                best_m = m;
                            }
                        }
                        tmp_mod_idx[px_count++] = (uint8_t)best_m;
                        t_err += min_mod_err;
                    }
                }

                if (t_err < best_sub_err) {
                    best_sub_err = t_err;
                    best_sub_t = t;
                    memcpy(best_mod_idx, tmp_mod_idx, 8);
                }
            }

            cur_t[s] = best_sub_t;
            total_err += best_sub_err;

            /* Atribui os bits MSB/LSB nos índices column-major do ETC */
            int px_count = 0;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int sub = flip ? (y >= 2) : (x >= 2);
                    if (sub != s) continue;

                    /* No ETC, índice do pixel = x * 4 + y */
                    int etc_p = x * 4 + y;
                    int m = best_mod_idx[px_count++];

                    /* Índice do pixel na especificação (MSB, LSB): 00 -> +a, 01 -> +b, 10 -> -a,
                     * 11 -> -b; g_etc2_mod_table[t] = {+a, +b, -a, -b}, então MSB = negativo.
                     * (Estava invertido: cada pixel saía espelhado em volta da cor base.) */
                    int msb = (m >= 2) ? 1 : 0;
                    int lsb = (m == 1 || m == 3) ? 1 : 0;

                    if (msb) cur_msb |= (uint16_t)(1u << etc_p);
                    if (lsb) cur_lsb |= (uint16_t)(1u << etc_p);
                }
            }
        }

        if (total_err < best_err) {
            best_err = total_err;
            best_flip = flip;
            best_diff = diff;
            if (diff) {
                best_r0 = (uint8_t)r0_5; best_g0 = (uint8_t)g0_5; best_b0 = (uint8_t)b0_5;
            } else {
                best_r0 = (uint8_t)q4[0][0]; best_g0 = (uint8_t)q4[0][1]; best_b0 = (uint8_t)q4[0][2];
            }
            best_dr = (int8_t)dr;    best_dg = (int8_t)dg;    best_db = (int8_t)db;
            best_r1 = (uint8_t)q4[1][0];
            best_g1 = (uint8_t)q4[1][1];
            best_b1 = (uint8_t)q4[1][2];
            best_t0 = cur_t[0];
            best_t1 = cur_t[1];
            best_msb = cur_msb;
            best_lsb = cur_lsb;
        }
    }

    /* Empacota os 8 bytes do bloco ETC2 */
    if (best_diff) {
        etc2[0] = (uint8_t)((best_r0 << 3) | (best_dr & 7));
        etc2[1] = (uint8_t)((best_g0 << 3) | (best_dg & 7));
        etc2[2] = (uint8_t)((best_b0 << 3) | (best_db & 7));
    } else {
        etc2[0] = (uint8_t)((best_r0 << 4) | (best_r1 & 15));
        etc2[1] = (uint8_t)((best_g0 << 4) | (best_g1 & 15));
        etc2[2] = (uint8_t)((best_b0 << 4) | (best_b1 & 15));
    }

    etc2[3] = (uint8_t)((best_t0 << 5) | (best_t1 << 2) | (best_diff ? 2 : 0) | (best_flip ? 1 : 0));
    etc2[4] = (uint8_t)(best_msb >> 8);
    etc2[5] = (uint8_t)(best_msb & 0xFF);
    etc2[6] = (uint8_t)(best_lsb >> 8);
    etc2[7] = (uint8_t)(best_lsb & 0xFF);
}

/* Codifica 16 valores de alpha (ordem y*4+x) num bloco EAC de 8 bytes (alpha do ETC2 RGBA8).
 * Para cada uma das 16 tabelas, ajusta multiplicador e base para cobrir [mín, máx] ancorando
 * num dos extremos, e fica com a de menor erro. 0 e 255 erram com peso alto: são o recorte das
 * folhas, cercas e cabelos (alpha test) e o "totalmente opaco". */
static inline void re5_eac_encode(const uint8_t a[16], uint8_t *eac)
{
    int mn = 255, mx = 0;
    for (int p = 0; p < 16; p++) {
        if (a[p] < mn) mn = a[p];
        if (a[p] > mx) mx = a[p];
    }
    uint64_t bits48 = 0;
    if (mn == mx) {
        /* tabela 13, seletor 4 = modificador 0: exato sem depender do multiplicador 0 */
        eac[0] = (uint8_t)mn;
        eac[1] = (uint8_t)((1 << 4) | 13);
        for (int i = 0; i < 16; i++) bits48 |= (uint64_t)4 << (45 - 3 * i);
    } else {
        int best_base = 0, best_mult = 1, best_t = 0;
        int64_t best_err = INT64_MAX;
        uint8_t best_sel[16] = {0};
        for (int t = 0; t < 16; t++) {
            int tmin = g_eac_mod_table[t][3], tmax = g_eac_mod_table[t][7];
            int range = tmax - tmin;
            int mult = (mx - mn + range - 1) / range;
            if (mult < 1) mult = 1;
            if (mult > 15) mult = 15;
            int bases[2] = {mn - mult * tmin, mx - mult * tmax};
            for (int bi = 0; bi < 2; bi++) {
                int base = re5_clamp_u8(bases[bi]);
                int64_t err = 0;
                uint8_t sel[16];
                for (int p = 0; p < 16 && err < best_err; p++) {
                    int be = INT32_MAX, bs = 0;
                    for (int s8 = 0; s8 < 8; s8++) {
                        int v = re5_clamp_u8(base + mult * g_eac_mod_table[t][s8]);
                        int d = a[p] - v;
                        int e = d * d;
                        if ((a[p] == 0 || a[p] == 255) && d) e += 1 << 20;
                        if (e < be) { be = e; bs = s8; }
                    }
                    sel[p] = (uint8_t)bs;
                    err += be;
                }
                if (err < best_err) {
                    best_err = err;
                    best_base = base;
                    best_mult = mult;
                    best_t = t;
                    memcpy(best_sel, sel, 16);
                }
            }
        }
        eac[0] = (uint8_t)best_base;
        eac[1] = (uint8_t)((best_mult << 4) | best_t);
        /* 48 bits de seletores de 3 bits, pixels em ordem de coluna (x * 4 + y), o primeiro no topo */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                bits48 |= (uint64_t)(best_sel[y * 4 + x] & 7) << (45 - 3 * (x * 4 + y));
    }
    for (int i = 0; i < 6; i++) eac[2 + i] = (uint8_t)(bits48 >> (40 - 8 * i));
}

/* Alpha interpolado do DXT5 (8 bytes) -> EAC */
static inline void re5_dxt5_alpha_to_eac(const uint8_t *dxt_a, uint8_t *eac)
{
    uint8_t a0 = dxt_a[0], a1 = dxt_a[1];
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= (uint64_t)dxt_a[2 + i] << (8 * i);
    uint8_t pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++) pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i <= 4; i++) pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint8_t a[16];
    for (int p = 0; p < 16; p++) a[p] = pal[(idx >> (p * 3)) & 7];
    re5_eac_encode(a, eac);
}

/* Alpha explícito de 4 bits do DXT3 (8 bytes) -> EAC */
static inline void re5_dxt3_alpha_to_eac(const uint8_t *dxt_a, uint8_t *eac)
{
    uint8_t a[16];
    for (int y = 0; y < 4; y++) {
        uint16_t row = (uint16_t)dxt_a[y * 2] | ((uint16_t)dxt_a[y * 2 + 1] << 8);
        for (int x = 0; x < 4; x++) a[y * 4 + x] = (uint8_t)(((row >> (x * 4)) & 0xF) * 17);
    }
    re5_eac_encode(a, eac);
}

/* Transcodifica um bloco de 8 bytes de DXT1 para 16 bytes de ETC2 RGBA8 (EAC Alpha + ETC2 RGB).
 * Suporta o canal Alpha de 1 bit (punch-through) do DXT1 usado em folhagens de árvores, arbustos, cercas e cabelos. */
static inline void re5_dxt1_to_etc2_rgba8_block(const uint8_t *dxt, uint8_t *etc2)
{
    uint16_t c0 = (uint16_t)dxt[0] | ((uint16_t)dxt[1] << 8);
    uint16_t c1 = (uint16_t)dxt[2] | ((uint16_t)dxt[3] << 8);
    uint32_t indices = (uint32_t)dxt[4] | ((uint32_t)dxt[5] << 8) |
                       ((uint32_t)dxt[6] << 16) | ((uint32_t)dxt[7] << 24);

    /* 1. Canal alpha (bytes 0..7): com c0 <= c1, o índice 3 é transparente (0); o resto é 255 */
    uint8_t a[16];
    for (int p = 0; p < 16; p++) a[p] = (c0 <= c1 && ((indices >> (p * 2)) & 3) == 3) ? 0 : 255;
    re5_eac_encode(a, etc2);

    /* 2. Canal RGB (bytes 8..15 de etc2) */
    re5_dxt1_to_etc2_block(dxt, etc2 + 8);
}

/* Transcodifica um bloco de 16 bytes de DXT5 para 16 bytes de ETC2 RGBA8 */
static inline void re5_dxt5_to_etc2_block(const uint8_t *dxt, uint8_t *etc2)
{
    /* Bytes 0..7: EAC Alpha */
    re5_dxt5_alpha_to_eac(dxt, etc2);
    /* Bytes 8..15: ETC2 RGB */
    re5_dxt1_to_etc2_block(dxt + 8, etc2 + 8);
}

/* Transcodifica um bloco de 16 bytes de DXT3 para 16 bytes de ETC2 RGBA8 */
static inline void re5_dxt3_to_etc2_block(const uint8_t *dxt, uint8_t *etc2)
{
    /* Bytes 0..7: EAC Alpha */
    re5_dxt3_alpha_to_eac(dxt, etc2);
    /* Bytes 8..15: ETC2 RGB */
    re5_dxt1_to_etc2_block(dxt + 8, etc2 + 8);
}

/* Transcodifica uma superfície inteira de DXT1/3/5 para ETC2 RGB8 ou RGBA8 */
static inline void re5_dxt_to_etc2_surface(int dxt_mode, uint32_t w, uint32_t h,
                                           const uint8_t *src, uint32_t src_pitch,
                                           uint8_t *dst, uint32_t dst_pitch)
{
    uint32_t bw = (w + 3) / 4;
    uint32_t bh = (h + 3) / 4;
    uint32_t src_block_bytes = (dxt_mode == 1) ? 8 : 16;
    uint32_t dst_block_bytes = (bw > 0 && dst_pitch / bw >= 16) ? 16 : (dxt_mode == 1 ? 8 : 16);

    for (uint32_t by = 0; by < bh; by++) {
        const uint8_t *s_row = src + (size_t)by * src_pitch;
        uint8_t *d_row = dst + (size_t)by * dst_pitch;

        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *s_blk = s_row + (size_t)bx * src_block_bytes;
            uint8_t *d_blk = d_row + (size_t)bx * dst_block_bytes;

            if (dxt_mode == 1) {
                if (dst_block_bytes == 16) {
                    re5_dxt1_to_etc2_rgba8_block(s_blk, d_blk);
                } else {
                    re5_dxt1_to_etc2_block(s_blk, d_blk);
                }
            } else if (dxt_mode == 3) {
                re5_dxt3_to_etc2_block(s_blk, d_blk);
            } else { /* mode == 5 */
                re5_dxt5_to_etc2_block(s_blk, d_blk);
            }
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* RE5_DXT_TO_ETC2_H */
