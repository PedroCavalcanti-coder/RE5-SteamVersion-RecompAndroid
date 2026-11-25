/*
 * re5_tex_cache.h - Sistema de cache persistente de texturas transcodificadas em disco.
 *
 * Armazena texturas DXT já transcodificadas para formatos nativos móveis (ETC2/EAC).
 * Na primeira execução ou carregamento, a textura é transcodificada e salva no cache;
 * em carregamentos subsequentes, a textura convertida é lida diretamente do armazenamento
 * secundário em velocidade máxima de E/S, eliminando 100% do custo de CPU e GPU no loading
 * e prevenindo engasgos (stuttering) durante o streaming de recursos.
 */
#ifndef RE5_TEX_CACHE_H
#define RE5_TEX_CACHE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define re5_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define re5_mkdir(p) mkdir(p, 0755)
#endif

#if defined(__ANDROID__)
#include <SDL2/SDL.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RE5_TEX_CACHE_MAGIC 0x32435445u /* "ETC2" little-endian */
#define RE5_TEX_CACHE_VERSION 5u /* 5: hash por palavras (o de bytes era ~10x mais lento); 4: sinal do ETC2 e EAC refeitos */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    uint32_t d3d_format;
    uint32_t data_size;
    uint32_t checksum;
} re5_tex_cache_header;

/* Hash FNV-1a de 64 bits */
static inline uint64_t re5_fnv1a64(const void *data, size_t size, uint64_t h)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Hash de 64 bits por palavras, 4 acumuladores independentes (estilo xxHash64). O FNV-1a byte a byte tem a multiplicação
 * encadeada (~5 ciclos por byte: ~0,4 GB/s num Cortex-A78) e hashear as texturas DXT do carregamento custava segundos;
 * aqui são 32 bytes por volta sem dependência entre as 4 pistas. Só identifica o conteúdo (chave do cache em disco). */
static inline uint64_t re5_rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t re5_hash_words64(const void *data, size_t n, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
    uint64_t a0 = seed + P1 + P2, a1 = seed + P2, a2 = seed, a3 = seed - P1, h;
    size_t len = n;
    for (; n >= 32; p += 32, n -= 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p, 8);
        memcpy(&w1, p + 8, 8);
        memcpy(&w2, p + 16, 8);
        memcpy(&w3, p + 24, 8);
        a0 = re5_rotl64(a0 + w0 * P2, 31) * P1;
        a1 = re5_rotl64(a1 + w1 * P2, 31) * P1;
        a2 = re5_rotl64(a2 + w2 * P2, 31) * P1;
        a3 = re5_rotl64(a3 + w3 * P2, 31) * P1;
    }
    h = re5_rotl64(a0, 1) + re5_rotl64(a1, 7) + re5_rotl64(a2, 12) + re5_rotl64(a3, 18);
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = re5_rotl64(h ^ (re5_rotl64(w * P2, 31) * P1), 27) * P1 + P3;
    }
    for (; n; p++, n--) h = re5_rotl64(h ^ ((uint64_t)*p * P3), 11) * P1;
    h ^= (uint64_t)len;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

/* Calcula o hash único da textura a partir das suas dimensões e dados DXT */
static inline uint64_t re5_tex_calc_hash(uint32_t w, uint32_t height, uint32_t levels,
                                         uint32_t d3d_fmt, const uint8_t *data, size_t size)
{
    uint64_t hval = 14695981039346656037ull;
    uint32_t meta[4] = {w, height, levels, d3d_fmt};
    hval = re5_fnv1a64(meta, sizeof meta, hval);
    /* Faz o hash dos blocos DXT de entrada */
    if (data && size) {
        hval = re5_hash_words64(data, size, hval);
    }
    return hval;
}

static inline int re5_tex_cache_enabled(void)
{
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char *e = getenv("RE5VK2_TEX_CACHE");
        s_enabled = !(e && *e == '0');
    }
    return s_enabled;
}

/* Retorna o diretório base do cache de texturas */
static inline const char *re5_tex_cache_dir(void)
{
    static char s_dir[1024];
    static int s_init = 0;
    if (s_init) return s_dir;
    s_init = 1;

    const char *e = getenv("RE5VK2_TEX_CACHE_DIR");
    if (!e || !*e) e = getenv("RE5VK_TEX_CACHE_DIR");   /* nome do launcher Android */
    if (e && *e) {
        snprintf(s_dir, sizeof s_dir, "%s", e);
        re5_mkdir(s_dir);
        return s_dir;
    }

#if defined(__ANDROID__)
    const char *base = SDL_AndroidGetInternalStoragePath();
    if (base && *base) {
        snprintf(s_dir, sizeof s_dir, "%.900s/texcache", base);
        re5_mkdir(s_dir);
        return s_dir;
    }
#endif

    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        char base[1024];
        snprintf(base, sizeof base, "%.900s/re5", xdg);
        re5_mkdir(base);
        snprintf(s_dir, sizeof s_dir, "%.900s/texcache", base);
        re5_mkdir(s_dir);
        return s_dir;
    }

    snprintf(s_dir, sizeof s_dir, "re5_texcache");
    re5_mkdir(s_dir);
    return s_dir;
}

/* Carrega textura comprimida do cache persistente se existir e for válida */
static inline int re5_tex_cache_load(uint64_t hash, uint32_t expected_size, uint8_t *dst)
{
    if (!re5_tex_cache_enabled()) return 0;
    const char *dir = re5_tex_cache_dir();
    char path[2048];
    snprintf(path, sizeof path, "%s/%016llx.etc2", dir, (unsigned long long)hash);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    re5_tex_cache_header hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1) {
        fclose(f);
        return 0;
    }

    if (hdr.magic != RE5_TEX_CACHE_MAGIC || hdr.version != RE5_TEX_CACHE_VERSION ||
        hdr.hash != hash || hdr.data_size != expected_size) {
        fclose(f);
        return 0;
    }

    if (fread(dst, 1, expected_size, f) != expected_size) {
        fclose(f);
        return 0;
    }

    fclose(f);
    return 1;
}

/* Grava textura convertida no cache persistente */
static inline void re5_tex_cache_save(uint64_t hash, uint32_t w, uint32_t h, uint32_t levels,
                                      uint32_t d3d_fmt, const uint8_t *data, uint32_t size)
{
    if (!re5_tex_cache_enabled()) return;
    const char *dir = re5_tex_cache_dir();
    char tmp_path[2048], path[2048];
    snprintf(tmp_path, sizeof tmp_path, "%s/%016llx.tmp", dir, (unsigned long long)hash);
    snprintf(path, sizeof path, "%s/%016llx.etc2", dir, (unsigned long long)hash);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;

    re5_tex_cache_header hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = RE5_TEX_CACHE_MAGIC;
    hdr.version = RE5_TEX_CACHE_VERSION;
    hdr.hash = hash;
    hdr.width = w;
    hdr.height = h;
    hdr.levels = levels;
    hdr.d3d_format = d3d_fmt;
    hdr.data_size = size;

    if (fwrite(&hdr, sizeof hdr, 1, f) == 1) {
        if (fwrite(data, 1, size, f) == size) {
            fclose(f);
            /* Substituição atômica via rename */
#if defined(_WIN32)
            remove(path);
#endif
            rename(tmp_path, path);
            return;
        }
    }

    fclose(f);
    remove(tmp_path);
}

#ifdef __cplusplus
}
#endif

#endif /* RE5_TEX_CACHE_H */
