/*
 * thunks.c - faixa de endereços do guest atendida pelo host (ver re5/thunks.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/fast_tls.h"
#include "re5/guest_memory.h"
#include "re5/log.h"
#include "re5/stats.h"
#include "re5/thunks.h"
#include "re5/thread.h"
#include "host_sync.h"

static re5_thunk *g_thunks;
static uint32_t g_next;
static re5_mutex g_mu;
RE5_TLS_PTR(const re5_thunk *, t_current)

static void return_to_host(CPU *c)
{
    re5_fatal("thunk", "dispatch para o endereço de retorno do host (esp=0x%08X): pilha do guest corrompida "
                       "ou um `ret` sem chamada correspondente", c->esp);
}

static uint32_t g_nolock_mask;   /* RE5_THUNK_NOLOCK_* ligados por RE5_*_NOLOCK */

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int re5_thunks_init(void)
{
    if (g_thunks) return 0;
    const char *hn = getenv("RE5_HEAP_NOLOCK");   /* padrão ligado desde o A/B de 28/10; =0 desliga */
    if (!hn || !*hn || env_on("RE5_HEAP_NOLOCK")) g_nolock_mask |= RE5_THUNK_NOLOCK_HEAP;
    re5_mutex_init(&g_mu);
    g_thunks = calloc(RE5_THUNK_MAX, sizeof *g_thunks);
    if (!g_thunks) return -1;
    /* A faixa existe na memória do guest para que ler um ponteiro de função
     * (comparar, copiar para uma vtable) funcione. Os bytes são int3. */
    uint32_t size = RE5_THUNK_LIMIT - RE5_THUNK_BASE;
    if (re5_mem_map_fixed(RE5_THUNK_BASE, size, RE5_PAGE_READWRITE, "thunks") != 0) return -1;
    memset(re5_g2h(RE5_THUNK_BASE), 0xCC, size);
    re5_mem_protect(RE5_THUNK_BASE, size, RE5_PAGE_EXECUTE_READ, NULL);
    uint32_t va = re5_thunk_new("<retorno ao host>", return_to_host, 0, NULL);
    return va == RE5_RETURN_TO_HOST ? 0 : -1;
}

uint32_t re5_thunk_new(const char *name, re5_host_fn fn, uint32_t arg_bytes, void *user)
{
    re5_mutex_lock(&g_mu);
    if (g_next >= RE5_THUNK_MAX) {
        re5_mutex_unlock(&g_mu);
        re5_fatal("thunk", "faixa de thunks esgotada (%u)", (unsigned)RE5_THUNK_MAX);
    }
    uint32_t idx = g_next++;
    g_thunks[idx] = (re5_thunk){fn, name, arg_bytes, user, 0};
    re5_mutex_unlock(&g_mu);
    return RE5_THUNK_BASE + idx * RE5_THUNK_STRIDE;
}

void re5_thunk_log_top(int n, double secs)
{
    enum { MAXN = 12 };
    if (n > MAXN) n = MAXN;
    const re5_thunk *best[MAXN] = {0};
    uint64_t delta[MAXN] = {0};
    for (uint32_t i = 0; i < g_next; i++) {
        re5_thunk *t = &g_thunks[i];
        uint64_t d = t->calls - t->calls_prev;
        t->calls_prev = t->calls;
        for (int k = 0; k < n; k++) {
            if (d > delta[k]) {
                for (int j = n - 1; j > k; j--) { best[j] = best[j - 1]; delta[j] = delta[j - 1]; }
                best[k] = t;
                delta[k] = d;
                break;
            }
        }
    }
    char line[900];
    int len = snprintf(line, sizeof line, "thunks/s:");
    for (int k = 0; k < n && best[k] && len < (int)sizeof line - 80; k++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %s=%.0f", best[k]->name, (double)delta[k] / secs);
    RE5_INFO("thunk", "%s", line);

    /* Os métodos do D3D9 que mais tempo tomam (ms por segundo): onde o d3d9_ms vai. */
    const re5_thunk *slow[MAXN] = {0};
    uint64_t dns[MAXN] = {0};
    for (uint32_t i = 0; i < g_next; i++) {
        re5_thunk *t = &g_thunks[i];
        uint64_t d = t->ns - t->ns_prev;
        t->ns_prev = t->ns;
        for (int k = 0; k < n; k++) {
            if (d > dns[k]) {
                for (int j = n - 1; j > k; j--) { slow[j] = slow[j - 1]; dns[j] = dns[j - 1]; }
                slow[k] = t;
                dns[k] = d;
                break;
            }
        }
    }
    if (!slow[0]) return;
    len = snprintf(line, sizeof line, "d3d9 ms/s:");
    for (int k = 0; k < n && slow[k] && len < (int)sizeof line - 80; k++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %s=%.2f", slow[k]->name, (double)dns[k] / 1e6 / secs);
    RE5_INFO("thunk", "%s", line);
}

void re5_thunk_set_flags(uint32_t va, uint32_t flags)
{
    if (!re5_is_thunk(va) || (va - RE5_THUNK_BASE) % RE5_THUNK_STRIDE) return;
    uint32_t idx = (va - RE5_THUNK_BASE) / RE5_THUNK_STRIDE;
    if (idx < g_next) g_thunks[idx].flags = flags;
}

const re5_thunk *re5_thunk_get(uint32_t va)
{
    if (!re5_is_thunk(va) || (va - RE5_THUNK_BASE) % RE5_THUNK_STRIDE) return NULL;
    uint32_t idx = (va - RE5_THUNK_BASE) / RE5_THUNK_STRIDE;
    return idx < g_next ? &g_thunks[idx] : NULL;
}

uint32_t re5_thunk_count(void) { return g_next; }
const re5_thunk *re5_thunk_current(void)
{
    re5_thread *th = re5_thread_current();
    return th ? (const re5_thunk *)th->cur_thunk : t_current_get();
}

void re5_thunk_invoke(CPU *c, uint32_t va)
{
    const re5_thunk *t = re5_thunk_get(va);
    if (!t) re5_fatal("thunk", "dispatch para 0x%08X na faixa de thunks, mas nenhum thunk mora aí", va);
    /* O thunk corrente mora na re5_thread (o TLS só nas threads do host sem ela): uma leitura
     * de TLS por chamada em vez de cinco (RE5_PROFILE, 2026-11-04: re5_thread_current 1,4% da
     * thread de render). */
    re5_thread *th = re5_thread_current();
    const re5_thunk *prev;
    if (th) {
        prev = th->cur_thunk;
        th->cur_thunk = t;
    } else {
        prev = t_current_get();
        t_current_set(t);
    }
    const char *prev_name = th ? th->thunk : NULL;
    if (th) th->thunk = t->name;
    RE5_TRACE("thunk", "-> %s", t->name);
    /* RE5_GUEST_LOCK=hle: o código do guest roda solto; o lock protege só o HLE. */
    static int census = -1;
    if (census < 0) census = env_on("RE5_D3D9_CENSUS");
    if (census && !(t->flags & RE5_THUNK_SEEN) && !strncmp(t->name, "IDirect3D", 9)) {
        ((re5_thunk *)t)->flags |= RE5_THUNK_SEEN;   /* corrida inofensiva: no pior caso, duas linhas */
        RE5_INFO("censo", "método %s", t->name);
    }
    int d3d9_timed = 0;
    if (re5_stats_on) {
        RE5_STAT_ADD(RE5_ST_THUNKS, 1);
        /* Contagem por thunk amostrada 1 em 16: um store por chamada na tabela global disputava
         * as linhas de cache entre as threads. */
        if (th && (++th->stat_sample & 15u) == 0) ((re5_thunk *)t)->calls += 16;
        if (!strncmp(t->name, "IDirect3D", 9)) {
            RE5_STAT_ADD(RE5_ST_D3D9, 1);
            const char *m = strstr(t->name, "::");
            if (m && !strncmp(m + 2, "Draw", 4)) RE5_STAT_ADD(RE5_ST_DRAWS, 1);
            /* O Present mede a si mesmo (d3d9.c), sem a espera para retomar o lock. */
            d3d9_timed = !(m && !strcmp(m + 2, "Present"));
        }
    }
    int lk = re5_guest_lock_mode_hle() && !(t->flags & (RE5_THUNK_NOLOCK | g_nolock_mask));
    if (lk) re5_guest_lock_as(th);
    uint64_t t0 = d3d9_timed ? re5_stat_now_ns() : 0;
    t->fn(c);
    if (d3d9_timed) {
        uint64_t dt = re5_stat_now_ns() - t0;
        RE5_STAT_ADD(RE5_ST_D3D9_NS, dt);
        ((re5_thunk *)t)->ns += dt;   /* sem trava: ordem de grandeza, como calls */
    }
    if (lk) re5_guest_unlock_as(th);
    if (th) {
        th->thunk = prev_name;
        th->cur_thunk = prev;
    } else {
        t_current_set(prev);
    }
    c->esp += 4u + t->arg_bytes;   /* ret N: endereço de retorno + argumentos stdcall */
}
