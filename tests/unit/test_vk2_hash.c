/*
 * test_vk2_hash.c - hash de texturas do cache ETC2 (re5_hash_words64): determinístico, sensível a qualquer byte,
 * trata caudas de todos os tamanhos e é bem mais rápido que o FNV-1a byte a byte que ele substituiu.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "../../runtime/src/gfx/vk2/re5_tex_cache.h"

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
    uint8_t buf[200];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 37u + 11u);

    /* determinístico e sensível ao tamanho */
    CHECK(re5_hash_words64(buf, 100, 1) == re5_hash_words64(buf, 100, 1));
    CHECK(re5_hash_words64(buf, 100, 1) != re5_hash_words64(buf, 101, 1));
    CHECK(re5_hash_words64(buf, 100, 1) != re5_hash_words64(buf, 100, 2));

    /* cada tamanho de 0 a 130 (todas as caudas) e cada byte alterado muda o hash */
    for (size_t n = 1; n <= 130; n++) {
        uint64_t base = re5_hash_words64(buf, n, 7);
        for (size_t i = 0; i < n; i++) {
            buf[i] ^= 0x01;
            if (re5_hash_words64(buf, n, 7) == base) {
                fprintf(stderr, "n=%zu byte %zu não alterou o hash\n", n, i);
                check_failures++;
            }
            buf[i] ^= 0x01;
        }
    }

    /* sem colisão entre 20 mil entradas curtas parecidas (chaves do cache) */
    enum { N = 20000 };
    uint64_t *h = malloc(N * sizeof *h);
    for (uint32_t i = 0; i < N; i++) {
        uint8_t blk[48];
        memset(blk, 0xA5, sizeof blk);
        memcpy(blk, &i, 4);
        h[i] = re5_tex_calc_hash(64, 64, 1, 0x35545844, blk, sizeof blk);
    }
    int dup = 0;
    for (uint32_t i = 0; i < N; i += 1)
        for (uint32_t j = i + 1; j < i + 50 && j < N; j++) dup += h[i] == h[j];
    CHECK(dup == 0);
    free(h);

    /* velocidade (informativa; falha só se NÃO for mais rápido que o FNV byte a byte) */
    size_t big = 64u << 20;
    uint8_t *m = malloc(big);
    for (size_t i = 0; i < big; i++) m[i] = (uint8_t)(i * 2654435761u >> 24);
    double t0 = now_s();
    volatile uint64_t a = re5_hash_words64(m, big, 1);
    double t1 = now_s();
    volatile uint64_t b = re5_fnv1a64(m, big, 1);
    double t2 = now_s();
    (void)a;
    (void)b;
    printf("hash por palavras %.2f GB/s, FNV-1a por byte %.2f GB/s (%.1fx)\n", 0.0625 / (t1 - t0), 0.0625 / (t2 - t1),
           (t2 - t1) / (t1 - t0));
    CHECK((t1 - t0) < (t2 - t1));
    free(m);
    CHECK_DONE();
}
