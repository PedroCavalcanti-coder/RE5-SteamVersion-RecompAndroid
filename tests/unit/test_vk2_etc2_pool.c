/* Recodificação ETC2 em paralelo (runtime/src/gfx/vk2/vk_etc2_pool.c): o resultado tem que ser byte a
 * byte o da versão em uma thread, em tamanhos que não dividem certo entre os trechos, com várias
 * threads pedindo ao mesmo tempo. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/thread.h"
#include "../../runtime/src/gfx/vk2/re5_dxt_to_etc2.h"
#include "../../runtime/src/gfx/vk2/vk2_etc2_pool.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

static uint32_t g_seed = 12345;
static uint8_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (uint8_t)(g_seed >> 24);
}

static int one(int mode, uint32_t w, uint32_t h)
{
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, sb = mode == 1 ? 8 : 16, db = mode == 1 ? 8 : 16;
    uint32_t sp = bw * sb, dp = bw * db;
    uint8_t *src = malloc((size_t)sp * bh), *a = malloc((size_t)dp * bh), *b = malloc((size_t)dp * bh);
    for (size_t i = 0; i < (size_t)sp * bh; i++) src[i] = rnd();
    memset(a, 0xAA, (size_t)dp * bh);
    memset(b, 0x55, (size_t)dp * bh);
    re5_dxt_to_etc2_surface(mode, w, h, src, sp, a, dp);
    vk2_etc2_surface(mode, w, h, src, sp, b, dp);
    int same = memcmp(a, b, (size_t)dp * bh) == 0;
    free(src);
    free(a);
    free(b);
    return same;
}

static volatile int g_bad, g_finished;
static void concurrent(void *arg)
{
    int k = (int)(intptr_t)arg;
    for (int i = 0; i < 4; i++)
        if (!one(i & 1 ? 5 : 1, 256 + 12 * k, 132 + 4 * i)) g_bad = 1;
    __atomic_add_fetch(&g_finished, 1, __ATOMIC_RELEASE);
}

int main(void)
{
    printf("auxiliares: %d\n", vk2_etc2_threads());
    CHECK(one(1, 512, 512));
    CHECK(one(5, 512, 256));
    CHECK(one(3, 300, 172));   /* trechos desiguais */
    CHECK(one(5, 4, 4));       /* pequena: caminho direto */
    CHECK(one(1, 2048, 8));    /* poucas linhas de blocos */
    for (int k = 0; k < 4; k++) CHECK(re5_host_thread_start(concurrent, (void *)(intptr_t)k, 256 * 1024) == 0);
    concurrent((void *)(intptr_t)9);
    while (__atomic_load_n(&g_finished, __ATOMIC_ACQUIRE) < 5) {}   /* as 4 threads + esta */
    CHECK(!g_bad);
    CHECK_DONE();
}
