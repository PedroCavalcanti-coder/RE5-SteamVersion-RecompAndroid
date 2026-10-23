/*
 * guest_memory.c - reserva de 4 GiB e regiões comprometidas (ver re5/guest_memory.h).
 *
 * As regiões ficam num vetor ordenado por base. É O(n) por alocação, o que
 * basta: o jogo faz poucas centenas de VirtualAlloc; os heaps do guest
 * (HeapAlloc/malloc do CRT) sub-alocam dentro de regiões grandes.
 */
#include <stdlib.h>
#include <string.h>

#include "re5/guest_base.h"
#include "re5/guest_memory.h"
#include "re5/log.h"
#include "host_sync.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define GUEST_SPACE ((uint64_t)1 << 32)

uintptr_t re5_guest_base;

static re5_mutex g_mu;
static re5_mem_region *g_regions;
static uint32_t g_count, g_cap;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

#if defined(RE5_HOST_WINDOWS)
static DWORD host_prot(uint32_t p)
{
    switch (p) {
    case RE5_PAGE_NOACCESS: return PAGE_NOACCESS;
    case RE5_PAGE_READONLY: case RE5_PAGE_EXECUTE: case RE5_PAGE_EXECUTE_READ: return PAGE_READONLY;
    default: return PAGE_READWRITE;
    }
}
static int host_reserve(void)
{
    void *p = VirtualAlloc(NULL, (SIZE_T)GUEST_SPACE, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return -1;
    re5_guest_base = (uintptr_t)p;
    return 0;
}
static int host_commit(uint32_t va, uint32_t size, uint32_t prot)
{
    return VirtualAlloc((void *)RE5_G2H(va), size, MEM_COMMIT, host_prot(prot)) ? 0 : -1;
}
static int host_decommit(uint32_t va, uint32_t size)
{
    return VirtualFree((void *)RE5_G2H(va), size, MEM_DECOMMIT) ? 0 : -1;
}
static int host_protect(uint32_t va, uint32_t size, uint32_t prot)
{
    DWORD old;
    return VirtualProtect((void *)RE5_G2H(va), size, host_prot(prot), &old) ? 0 : -1;
}
static void host_release(void) { VirtualFree((void *)re5_guest_base, 0, MEM_RELEASE); }
#else
static int host_prot(uint32_t p)
{
    switch (p) {
    case RE5_PAGE_NOACCESS: return PROT_NONE;
    case RE5_PAGE_READONLY: case RE5_PAGE_EXECUTE: case RE5_PAGE_EXECUTE_READ: return PROT_READ;
    default: return PROT_READ | PROT_WRITE;
    }
}
static int host_reserve(void)
{
    void *p = mmap(NULL, (size_t)GUEST_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    re5_guest_base = (uintptr_t)p;
    return 0;
}
static int host_commit(uint32_t va, uint32_t size, uint32_t prot)
{
    return mprotect((void *)RE5_G2H(va), size, host_prot(prot));
}
static int host_decommit(uint32_t va, uint32_t size)
{
    /* Devolve as páginas (a próxima alocação volta zerada) e fecha o acesso. */
    madvise((void *)RE5_G2H(va), size, MADV_DONTNEED);
    return mprotect((void *)RE5_G2H(va), size, PROT_NONE);
}
static int host_protect(uint32_t va, uint32_t size, uint32_t prot)
{
    return mprotect((void *)RE5_G2H(va), size, host_prot(prot));
}
static void host_release(void) { munmap((void *)re5_guest_base, (size_t)GUEST_SPACE); }
#endif

int re5_mem_init(void)
{
    if (re5_guest_base) return 0;
    re5_mutex_init(&g_mu);
    if (host_reserve() != 0) {
        RE5_ERROR("mem", "não foi possível reservar 4 GiB para o espaço do guest");
        return -1;
    }
    RE5_DEBUG("mem", "espaço do guest em host %p", (void *)re5_guest_base);
    return 0;
}

void re5_mem_shutdown(void)
{
    if (!re5_guest_base) return;
    host_release();
    re5_guest_base = 0;
    free(g_regions);
    g_regions = NULL;
    g_count = g_cap = 0;
}

/* Índice da primeira região com base >= va. */
static uint32_t lower_bound(uint32_t va)
{
    uint32_t lo = 0, hi = g_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_regions[mid].base < va) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int overlaps(uint32_t va, uint64_t end)
{
    uint32_t i = lower_bound(va);
    if (i > 0 && (uint64_t)g_regions[i - 1].base + g_regions[i - 1].size > va) return 1;
    return i < g_count && g_regions[i].base < end;
}

static int insert(uint32_t va, uint32_t size, uint32_t prot, const char *tag)
{
    if (g_count == g_cap) {
        uint32_t cap = g_cap ? g_cap * 2 : 64;
        re5_mem_region *r = realloc(g_regions, cap * sizeof *r);
        if (!r) return -1;
        g_regions = r;
        g_cap = cap;
    }
    uint32_t i = lower_bound(va);
    memmove(&g_regions[i + 1], &g_regions[i], (g_count - i) * sizeof *g_regions);
    g_regions[i] = (re5_mem_region){va, size, prot, tag ? tag : "?"};
    g_count++;
    return 0;
}

int re5_mem_map_fixed(uint32_t va, uint32_t size, uint32_t prot, const char *tag)
{
    if (va % RE5_MEM_PAGE || !size) return -1;
    size = align_up(size, RE5_MEM_PAGE);
    uint64_t end = (uint64_t)va + size;
    if (va < RE5_MEM_GRANULARITY || end > GUEST_SPACE) return -1;
    re5_mutex_lock(&g_mu);
    int rc = -1;
    if (!overlaps(va, end) && host_commit(va, size, prot) == 0)
        rc = insert(va, size, prot, tag);
    re5_mutex_unlock(&g_mu);
    if (rc == 0) RE5_DEBUG("mem", "map  0x%08X-0x%08X %-12s prot=0x%02X", va, (uint32_t)(end - 1), tag, prot);
    return rc;
}

uint32_t re5_mem_alloc(uint32_t size, uint32_t prot, const char *tag)
{
    if (!size) return 0;
    uint32_t want = align_up(size, RE5_MEM_GRANULARITY);
    re5_mutex_lock(&g_mu);
    uint32_t va = 0, cand = RE5_MEM_DYN_LO;
    uint32_t first = lower_bound(RE5_MEM_DYN_LO);
    if (first > 0) {   /* uma região que começa antes da faixa e entra nela */
        uint64_t end = (uint64_t)g_regions[first - 1].base + g_regions[first - 1].size;
        end = (end + RE5_MEM_GRANULARITY - 1) & ~(uint64_t)(RE5_MEM_GRANULARITY - 1);
        if (end > cand) cand = end >= RE5_MEM_DYN_HI ? RE5_MEM_DYN_HI : (uint32_t)end;
    }
    for (uint32_t i = first; ; i++) {
        uint64_t gap_end = i < g_count ? g_regions[i].base : RE5_MEM_DYN_HI;
        if (gap_end > RE5_MEM_DYN_HI) gap_end = RE5_MEM_DYN_HI;
        if ((uint64_t)cand + want <= gap_end) { va = cand; break; }
        if (i >= g_count) break;
        uint64_t next = (uint64_t)g_regions[i].base + g_regions[i].size;
        next = (next + RE5_MEM_GRANULARITY - 1) & ~(uint64_t)(RE5_MEM_GRANULARITY - 1);
        if (next >= RE5_MEM_DYN_HI) break;
        if (next > cand) cand = (uint32_t)next;
    }
    if (va && (host_commit(va, want, prot) != 0 || insert(va, want, prot, tag) != 0)) va = 0;
    re5_mutex_unlock(&g_mu);
    if (va) RE5_DEBUG("mem", "alloc 0x%08X +0x%X %s", va, want, tag);
    else RE5_WARN("mem", "sem espaço para 0x%X bytes (%s)", want, tag);
    return va;
}

int re5_mem_free(uint32_t va)
{
    re5_mutex_lock(&g_mu);
    uint32_t i = lower_bound(va);
    int rc = -1;
    if (i < g_count && g_regions[i].base == va) {
        host_decommit(va, g_regions[i].size);
        memmove(&g_regions[i], &g_regions[i + 1], (g_count - i - 1) * sizeof *g_regions);
        g_count--;
        rc = 0;
    }
    re5_mutex_unlock(&g_mu);
    return rc;
}

static re5_mem_region *find(uint32_t va)
{
    uint32_t i = lower_bound(va + 1);
    if (i == 0) return NULL;
    re5_mem_region *r = &g_regions[i - 1];
    return (uint64_t)va < (uint64_t)r->base + r->size ? r : NULL;
}

int re5_mem_protect(uint32_t va, uint32_t size, uint32_t prot, uint32_t *old)
{
    uint32_t lo = va & ~(RE5_MEM_PAGE - 1);
    uint32_t len = align_up(va - lo + size, RE5_MEM_PAGE);
    re5_mutex_lock(&g_mu);
    re5_mem_region *r = find(lo);
    int rc = -1;
    if (r && (uint64_t)lo + len <= (uint64_t)r->base + r->size && host_protect(lo, len, prot) == 0) {
        if (old) *old = r->prot;
        /* Proteção por página não é rastreada: a região guarda a última. */
        r->prot = prot;
        rc = 0;
    }
    re5_mutex_unlock(&g_mu);
    return rc;
}

/* Zera [va, va+size) dentro de uma região. As páginas inteiras voltam para o
 * host (zeradas de novo só quando tocadas: decommit + commit no Windows,
 * MADV_DONTNEED no POSIX), as pontas com memset. É o MEM_DECOMMIT do guest:
 * um memset de MBs custava 18% das esperas do carregamento (RE5_PROFILE). */
int re5_mem_zero(uint32_t va, uint32_t size)
{
    re5_mutex_lock(&g_mu);
    re5_mem_region *r = find(va);
    int ok = r && (uint64_t)va + size <= (uint64_t)r->base + r->size;
    uint32_t prot = ok ? r->prot : 0;
    re5_mutex_unlock(&g_mu);
    if (!ok) return -1;
    uint32_t lo = align_up(va, RE5_MEM_PAGE), hi = (va + size) & ~(RE5_MEM_PAGE - 1);
    if (size < 16u * RE5_MEM_PAGE || hi <= lo) {
        memset(re5_g2h(va), 0, size);
        return 0;
    }
    memset(re5_g2h(va), 0, lo - va);
    memset(re5_g2h(hi), 0, va + size - hi);
#if defined(RE5_HOST_WINDOWS)
    if (!VirtualFree((void *)RE5_G2H(lo), hi - lo, MEM_DECOMMIT) || host_commit(lo, hi - lo, prot) != 0)
        re5_fatal("memory", "re5_mem_zero(0x%08X, %u): decommit/commit do host falhou", lo, hi - lo);
#else
    (void)prot;
    if (madvise((void *)RE5_G2H(lo), hi - lo, MADV_DONTNEED) != 0) memset(re5_g2h(lo), 0, hi - lo);
#endif
    return 0;
}

int re5_mem_is_mapped(uint32_t va, uint32_t size)
{
    re5_mutex_lock(&g_mu);
    uint64_t cur = va, end = (uint64_t)va + size;
    int ok = 1;
    while (ok && cur < end) {
        re5_mem_region *r = find((uint32_t)cur);
        if (!r) ok = 0;
        else cur = (uint64_t)r->base + r->size;
    }
    re5_mutex_unlock(&g_mu);
    return ok;
}

int re5_mem_query(uint32_t va, re5_mem_region *out)
{
    re5_mutex_lock(&g_mu);
    re5_mem_region *r = find(va);
    if (r && out) *out = *r;
    re5_mutex_unlock(&g_mu);
    return r ? 0 : -1;
}

uint32_t re5_mem_regions(re5_mem_region *out, uint32_t max)
{
    re5_mutex_lock(&g_mu);
    uint32_t n = g_count < max ? g_count : max;
    for (uint32_t i = 0; i < n; i++) out[i] = g_regions[i];
    re5_mutex_unlock(&g_mu);
    return n;
}

void re5_mem_dump(void)
{
    re5_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_count; i++)
        RE5_INFO("mem", "0x%08X-0x%08X prot=0x%02X %s", g_regions[i].base,
                 g_regions[i].base + g_regions[i].size - 1, g_regions[i].prot, g_regions[i].tag);
    re5_mutex_unlock(&g_mu);
}
