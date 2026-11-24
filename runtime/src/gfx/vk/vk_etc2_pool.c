/*
 * vk_etc2_pool.c - recodificação DXT -> ETC2/EAC em paralelo (re5vk no Mali, sem BC).
 *
 * Medido no A54 (22/11/2026): no carregamento uma thread fica a 99% recodificando as texturas
 * numa só thread enquanto o resto da CPU fica ocioso (o processo inteiro abaixo de 25%), e com
 * texturas em qualidade alta o carregamento fica muito mais lento (4x os blocos por textura).
 * Aqui as linhas de blocos de uma superfície são divididas entre threads do host fixas; quem
 * pediu também trabalha e só volta com a superfície inteira pronta. Várias threads podem pedir ao
 * mesmo tempo (cada pedido é um trabalho na lista). O resultado é byte a byte o mesmo da versão
 * em uma thread (os blocos são independentes).
 *
 * RE5VK_ETC2_THREADS=<n>: threads auxiliares (padrão: núcleos - 2, de 1 a 6; 0 = tudo na thread
 * que pediu, como antes).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "re5/log.h"
#include "re5/thread.h"
#include "../../core/host_sync.h"
#include "re5_dxt_to_etc2.h"
#include "vk_etc2_pool.h"

typedef struct etc2_job {
    int mode;
    uint32_t w, h, bh, rows_per_chunk, nchunks;
    const uint8_t *src;
    uint32_t src_pitch;
    uint8_t *dst;
    uint32_t dst_pitch;
    uint32_t next, done;   /* trechos entregues / terminados (com g_mu) */
    struct etc2_job *nx;
} etc2_job;

static re5_mutex g_mu;
static re5_cond g_work, g_done;
static etc2_job *g_head;
static int g_threads = -1;

/* Pega o próximo trecho de algum trabalho da lista. Com g_mu. */
static etc2_job *take(etc2_job *only, uint32_t *chunk)
{
    for (etc2_job **pp = &g_head; *pp; pp = &(*pp)->nx) {
        etc2_job *j = *pp;
        if (only && j != only) continue;
        if (j->next < j->nchunks) {
            *chunk = j->next++;
            if (j->next == j->nchunks) *pp = j->nx;   /* tudo entregue: sai da lista */
            return j;
        }
    }
    return NULL;
}

static void run_chunk(const etc2_job *j, uint32_t c)
{
    uint32_t r0 = c * j->rows_per_chunk, r1 = r0 + j->rows_per_chunk;
    if (r1 > j->bh) r1 = j->bh;
    uint32_t y0 = r0 * 4, h = r1 * 4 <= j->h ? (r1 - r0) * 4 : j->h - y0;
    re5_dxt_to_etc2_surface(j->mode, j->w, h, j->src + (size_t)r0 * j->src_pitch, j->src_pitch,
                            j->dst + (size_t)r0 * j->dst_pitch, j->dst_pitch);
}

static void finish_chunk(etc2_job *j)
{
    re5_mutex_lock(&g_mu);
    if (++j->done == j->nchunks) re5_cond_broadcast(&g_done);
    re5_mutex_unlock(&g_mu);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t c;
        re5_mutex_lock(&g_mu);
        etc2_job *j;
        while (!(j = take(NULL, &c))) re5_cond_wait_ms(&g_work, &g_mu, 0xFFFFFFFFu);
        re5_mutex_unlock(&g_mu);
        run_chunk(j, c);
        finish_chunk(j);
    }
}

static long cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwNumberOfProcessors;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

int re5vk_etc2_threads(void)
{
    if (g_threads >= 0) return g_threads;
    re5_mutex_init(&g_mu);
    re5_cond_init(&g_work);
    re5_cond_init(&g_done);
    const char *e = getenv("RE5VK_ETC2_THREADS");
    long n = e && *e ? strtol(e, NULL, 10) : cpu_count() - 2;
    if (n < 0) n = 0;
    if (n > 6 && !(e && *e)) n = 6;
    if (n > 16) n = 16;
    int started = 0;
    for (long i = 0; i < n; i++)
        if (re5_host_thread_start(worker, NULL, 256 * 1024) == 0) started++;
    g_threads = started;
    RE5_INFO("re5vk", "recodificação ETC2 com %d thread(s) auxiliar(es) (RE5VK_ETC2_THREADS)", started);
    return g_threads;
}

void re5vk_etc2_surface(int mode, uint32_t w, uint32_t h, const uint8_t *src, uint32_t src_pitch, uint8_t *dst,
                        uint32_t dst_pitch)
{
    static volatile int inited;   /* o primeiro pedido pode vir de duas threads */
    if (!inited) {
        static int once;
        if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) {
            re5vk_etc2_threads();
            __atomic_store_n(&inited, 1, __ATOMIC_RELEASE);
        } else {
            while (!__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) {}
        }
    }
    uint32_t bh = (h + 3) / 4, bw = (w + 3) / 4;
    /* pequena (menos de 1024 blocos) ou sem auxiliares: direto, sem custo de sincronização */
    if (!g_threads || (uint64_t)bw * bh < 1024u || bh < 2) {
        re5_dxt_to_etc2_surface(mode, w, h, src, src_pitch, dst, dst_pitch);
        return;
    }
    etc2_job j;
    memset(&j, 0, sizeof j);
    j.mode = mode;
    j.w = w;
    j.h = h;
    j.bh = bh;
    /* ~4 trechos por thread para equilibrar; pelo menos 256 blocos por trecho */
    uint32_t parts = (uint32_t)(g_threads + 1) * 4;
    uint32_t min_rows = (256u + bw - 1) / bw;
    j.rows_per_chunk = (bh + parts - 1) / parts;
    if (j.rows_per_chunk < min_rows) j.rows_per_chunk = min_rows;
    j.nchunks = (bh + j.rows_per_chunk - 1) / j.rows_per_chunk;
    j.src = src;
    j.src_pitch = src_pitch;
    j.dst = dst;
    j.dst_pitch = dst_pitch;
    re5_mutex_lock(&g_mu);
    j.nx = g_head;
    g_head = &j;
    re5_cond_broadcast(&g_work);
    for (;;) {   /* quem pediu também trabalha, só no próprio pedido */
        uint32_t c;
        if (!take(&j, &c)) break;
        re5_mutex_unlock(&g_mu);
        run_chunk(&j, c);
        re5_mutex_lock(&g_mu);
        j.done++;
    }
    while (j.done < j.nchunks) re5_cond_wait_ms(&g_done, &g_mu, 0xFFFFFFFFu);
    re5_mutex_unlock(&g_mu);
}
