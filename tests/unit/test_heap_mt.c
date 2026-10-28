/* Heap do guest com várias threads do host ao mesmo tempo (fase P1: Heap* sem o lock do HLE).
 * Cada thread aloca, grava um padrão próprio, confere e libera; qualquer sobreposição ou
 * corrupção de lista livre aparece como padrão trocado ou como abort do heap. */
#include <pthread.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"
#include "../../runtime/src/win32/kernel32_internal.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

#define NTHREADS 6
#define ROUNDS   4000
#define LIVE     64

static uint32_t g_heap;
static int g_bad[NTHREADS];

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint32_t ptr[LIVE] = {0}, size[LIVE] = {0};
    uint32_t seed = 0x1234u + (uint32_t)id * 7919u;
    for (int r = 0; r < ROUNDS; r++) {
        seed = seed * 1664525u + 1013904223u;
        int i = (int)((seed >> 16) % LIVE);
        if (ptr[i]) {
            for (uint32_t b = 0; b < size[i]; b++)
                if (rd8(ptr[i] + b) != (uint8_t)(id * 16 + 1)) g_bad[id]++;
            k32_heap_free(g_heap, ptr[i]);
            ptr[i] = 0;
        } else {
            size[i] = 8u + (seed >> 8) % 3000u;
            ptr[i] = k32_heap_alloc(g_heap, 0, size[i]);
            if (!ptr[i]) { g_bad[id]++; continue; }
            memset(re5_g2h(ptr[i]), id * 16 + 1, size[i]);
        }
    }
    for (int i = 0; i < LIVE; i++) if (ptr[i]) k32_heap_free(g_heap, ptr[i]);
    return NULL;
}

int main(void)
{
    CHECK(re5_mem_init() == 0);
    g_heap = k32_process_heap();
    CHECK(g_heap != 0);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < NTHREADS; i++) CHECK_EQ_U32((uint32_t)g_bad[i], 0u);
    return 0;
}
