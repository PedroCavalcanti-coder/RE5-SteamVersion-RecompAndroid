/*
 * kernel32_heap.c - heaps do Win32, VirtualAlloc e Global* (Fase 3, etapa 3.1).
 *
 * Os blocos moram na memória do guest; os metadados ficam no host. Cada bloco
 * tem um cabeçalho de 8 bytes logo antes do ponteiro (tamanho pedido e uma
 * marca com o índice do heap), o que dá o alinhamento de 8 do HeapAlloc do
 * Windows x86 e permite HeapSize/HeapReAlloc sem tabela. Pequenos e médios vêm
 * de classes de tamanho dentro de arenas de 32 MiB; grandes (>= 256 KiB) ganham
 * uma região própria (re5_mem_alloc) e voltam ao sistema no HeapFree.
 *
 * Um mutex do host (g_hmu) protege os heaps: os handlers de Heap* podem rodar sem o
 * lock do HLE (RE5_HEAP_NOLOCK=1, RE5_THUNK_NOLOCK_HEAP; PLANO_MESTRE, fase P1). Liberar duas vezes ou passar um ponteiro
 * que não é de heap aborta com o endereço (falhar alto, D8).
 */
#include <stdlib.h>

#include "kernel32_internal.h"
#include "re5/stats.h"
#include "../core/host_sync.h"

#define HEAP_NO_SERIALIZE          0x01u
#define HEAP_ZERO_MEMORY           0x08u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

#define HDR          8u
#define ARENA_BYTES  (32u << 20)
#define LARGE_MIN    (256u << 10)
#define MAX_HEAPS    64u
#define MAGIC_LIVE   0x00564C48u   /* "HLV" + índice do heap no byte alto */
#define MAGIC_FREE   0x00455246u
#define MAGIC_LARGE  0x00474C48u

typedef struct {
    uint32_t *v;
    uint32_t  n, cap;
} vec;

static int vec_push(vec *a, uint32_t x)
{
    if (a->n == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 64;
        uint32_t *nv = realloc(a->v, cap * sizeof *nv);
        if (!nv) return -1;
        a->v = nv;
        a->cap = cap;
    }
    a->v[a->n++] = x;
    return 0;
}

/* Classes: 16..512 de 16 em 16, depois 4 por potência de 2 até LARGE_MIN. */
#define NCLASS 72
static uint32_t g_class[NCLASS];
static uint32_t g_nclass;

static void init_classes(void)
{
    if (g_nclass) return;
    for (uint32_t s = 16; s <= 512; s += 16) g_class[g_nclass++] = s;
    for (uint32_t p = 512; p < LARGE_MIN; p *= 2)
        for (uint32_t q = 1; q <= 4 && g_nclass < NCLASS; q++) g_class[g_nclass++] = p + q * (p / 4);
}

/* Menor classe com capacidade >= total (cabeçalho incluso); -1 = bloco grande. */
static int class_of(uint32_t total)
{
    if (total >= LARGE_MIN) return -1;
    uint32_t lo = 0, hi = g_nclass;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_class[mid] < total) lo = mid + 1; else hi = mid;
    }
    return lo < g_nclass ? (int)lo : -1;
}

typedef struct {
    uint32_t handle;            /* VA de uma página do guest: o HANDLE do heap */
    uint32_t flags;
    uint32_t bump, bump_end;    /* arena corrente */
    vec      arenas;
    vec      large;
    vec      free_list[NCLASS];
} heap_t;

static heap_t g_heaps[MAX_HEAPS];
static uint32_t g_process_heap;

/* Criado por heap_create (HeapCreate/GetProcessHeap rodam sob o lock do HLE e vêm antes
 * de qualquer HeapAlloc com um handle válido). */
static re5_mutex g_hmu;
static void hlock(void) { if (g_hmu.ready) re5_mutex_lock(&g_hmu); }
static void hunlock(void) { if (g_hmu.ready) re5_mutex_unlock(&g_hmu); }

static heap_t *heap_from_handle(uint32_t h, uint32_t *idx)
{
    for (uint32_t i = 0; i < MAX_HEAPS; i++) {
        if (g_heaps[i].handle && g_heaps[i].handle == h) {
            if (idx) *idx = i;
            return &g_heaps[i];
        }
    }
    return NULL;
}

static uint32_t heap_create_locked(uint32_t flags);

static uint32_t heap_create(uint32_t flags)
{
    if (!g_hmu.ready) re5_mutex_init(&g_hmu);
    init_classes();
    hlock();
    uint32_t r = heap_create_locked(flags);
    hunlock();
    return r;
}

static uint32_t heap_create_locked(uint32_t flags)
{
    for (uint32_t i = 0; i < MAX_HEAPS; i++) {
        if (g_heaps[i].handle) continue;
        uint32_t page = re5_mem_alloc(RE5_MEM_PAGE, RE5_PAGE_READWRITE, "heap");
        if (!page) return 0;
        memset(&g_heaps[i], 0, sizeof g_heaps[i]);
        g_heaps[i].handle = page;
        g_heaps[i].flags = flags;
        return page;
    }
    re5_fatal("kernel32", "HeapCreate: mais de %u heaps", MAX_HEAPS);
}

uint32_t k32_process_heap(void)
{
    if (!g_process_heap) {
        g_process_heap = heap_create(0);
        if (!g_process_heap) re5_fatal("kernel32", "sem memória do guest para o heap do processo");
        uint32_t peb = re5_process_peb();
        if (peb) k32_w32(peb + RE5_PEB_PROCESS_HEAP, g_process_heap);
    }
    return g_process_heap;
}

static uint32_t hdr_magic(uint32_t ptr) { return k32_r32(ptr - 4) & 0x00FFFFFFu; }
static uint32_t hdr_heap(uint32_t ptr) { return k32_r32(ptr - 4) >> 24; }
static uint32_t hdr_size(uint32_t ptr) { return k32_r32(ptr - 8); }

static uint32_t capacity(uint32_t ptr)
{
    if (hdr_magic(ptr) == MAGIC_LARGE) {
        re5_mem_region r;
        return re5_mem_query(ptr, &r) == 0 ? r.base + r.size - ptr : hdr_size(ptr);
    }
    int k = class_of(hdr_size(ptr) + HDR);
    return g_class[k] - HDR;
}

/* Valida um ponteiro vindo do guest; aborta se não for um bloco vivo deste heap. */
static void check_block(uint32_t hidx, uint32_t ptr, const char *who)
{
    if (ptr < RE5_MEM_DYN_LO || ptr % 8 || !re5_mem_is_mapped(ptr - HDR, HDR))
        re5_fatal("kernel32", "%s(0x%08X): ponteiro que não é de heap", who, ptr);
    uint32_t m = hdr_magic(ptr);
    if (m == MAGIC_FREE) re5_fatal("kernel32", "%s(0x%08X): bloco já liberado", who, ptr);
    if ((m != MAGIC_LIVE && m != MAGIC_LARGE) || hdr_heap(ptr) != hidx)
        re5_fatal("kernel32", "%s(0x%08X): cabeçalho inválido (0x%08X) ou heap errado", who, ptr, k32_r32(ptr - 4));
}

static uint32_t heap_alloc_idx(uint32_t hidx, uint32_t flags, uint32_t size)
{
    heap_t *h = &g_heaps[hidx];
    if (size > 0x7FFF0000u) return 0;
    uint32_t total = (size + HDR + 15u) & ~15u;
    int k = class_of(total);
    uint32_t blk;
    if (k < 0) {
        blk = re5_mem_alloc(total, RE5_PAGE_READWRITE, "heap-grande");
        if (!blk) return 0;
        vec_push(&h->large, blk);
        k32_w32(blk + 4, MAGIC_LARGE | hidx << 24);
    } else {
        vec *fl = &h->free_list[k];
        if (fl->n) {
            blk = fl->v[--fl->n] - HDR;
            if (flags & HEAP_ZERO_MEMORY) memset(re5_g2h(blk + HDR), 0, g_class[k] - HDR);
        } else {
            uint32_t cap = g_class[k];
            if (h->bump + cap > h->bump_end || !h->bump) {
                uint32_t arena = re5_mem_alloc(ARENA_BYTES, RE5_PAGE_READWRITE, "heap-arena");
                if (!arena) return 0;
                vec_push(&h->arenas, arena);
                h->bump = arena;
                h->bump_end = arena + ARENA_BYTES;
            }
            blk = h->bump;          /* memória nova de arena já vem zerada */
            h->bump += cap;
        }
        k32_w32(blk + 4, MAGIC_LIVE | hidx << 24);
    }
    k32_w32(blk, size);
    return blk + HDR;
}

static void heap_free_idx(uint32_t hidx, uint32_t ptr)
{
    heap_t *h = &g_heaps[hidx];
    if (hdr_magic(ptr) == MAGIC_LARGE) {
        uint32_t blk = ptr - HDR;
        for (uint32_t i = 0; i < h->large.n; i++) {
            if (h->large.v[i] == blk) {
                h->large.v[i] = h->large.v[--h->large.n];
                break;
            }
        }
        re5_mem_free(blk);
        return;
    }
    int k = class_of(hdr_size(ptr) + HDR);
    k32_w32(ptr - 4, MAGIC_FREE | hidx << 24);
    if (vec_push(&h->free_list[k], ptr) != 0) re5_fatal("kernel32", "HeapFree: sem memória no host");
}

uint32_t k32_heap_alloc(uint32_t heap, uint32_t flags, uint32_t size)
{
    uint32_t idx;
    hlock();
    if (!heap_from_handle(heap, &idx)) re5_fatal("kernel32", "HeapAlloc: heap inválido 0x%08X", heap);
    uint32_t r = heap_alloc_idx(idx, flags, size);
    hunlock();
    return r;
}

int k32_heap_free(uint32_t heap, uint32_t ptr)
{
    uint32_t idx;
    if (!ptr) return 1;
    hlock();
    if (!heap_from_handle(heap, &idx)) re5_fatal("kernel32", "HeapFree: heap inválido 0x%08X", heap);
    check_block(idx, ptr, "HeapFree");
    heap_free_idx(idx, ptr);
    hunlock();
    return 1;
}

/* ---- handlers ---- */

static void k_GetProcessHeap(CPU *c) { c->eax = k32_process_heap(); }

static void k_HeapCreate(CPU *c)
{
    c->eax = heap_create(re5_arg(c, 0));
    if (!c->eax) re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY);
}

static void k_HeapDestroy(CPU *c)
{
    uint32_t idx;
    hlock();
    heap_t *h = heap_from_handle(re5_arg(c, 0), &idx);
    if (!h || h->handle == g_process_heap) {
        hunlock();
        re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
        c->eax = 0;
        return;
    }
    for (uint32_t i = 0; i < h->arenas.n; i++) re5_mem_free(h->arenas.v[i]);
    for (uint32_t i = 0; i < h->large.n; i++) re5_mem_free(h->large.v[i]);
    for (uint32_t k = 0; k < NCLASS; k++) free(h->free_list[k].v);
    free(h->arenas.v);
    free(h->large.v);
    re5_mem_free(h->handle);
    memset(h, 0, sizeof *h);
    hunlock();
    c->eax = 1;
}

static void k_HeapAlloc(CPU *c)
{
    uint32_t idx, heap = re5_arg(c, 0), flags = re5_arg(c, 1), size = re5_arg(c, 2);
    RE5_STAT_ADD(RE5_ST_HEAP, 1);
    hlock();
    if (!heap_from_handle(heap, &idx)) re5_fatal("kernel32", "HeapAlloc: heap inválido 0x%08X", heap);
    c->eax = heap_alloc_idx(idx, flags, size);
    hunlock();
    if (!c->eax) RE5_WARN("kernel32", "HeapAlloc(0x%08X, %u bytes): sem memória do guest", heap, size);
}

static void k_HeapFree(CPU *c)
{
    RE5_STAT_ADD(RE5_ST_HEAP, 1);
    c->eax = (uint32_t)k32_heap_free(re5_arg(c, 0), re5_arg(c, 2));
}

static void k_HeapSize(CPU *c)
{
    uint32_t idx, heap = re5_arg(c, 0), ptr = re5_arg(c, 2);
    hlock();
    if (!heap_from_handle(heap, &idx)) re5_fatal("kernel32", "HeapSize: heap inválido 0x%08X", heap);
    check_block(idx, ptr, "HeapSize");
    c->eax = hdr_size(ptr);
    hunlock();
}

static void k_HeapReAlloc(CPU *c)
{
    uint32_t idx, heap = re5_arg(c, 0), flags = re5_arg(c, 1), ptr = re5_arg(c, 2), size = re5_arg(c, 3);
    RE5_STAT_ADD(RE5_ST_HEAP, 1);
    hlock();
    if (!heap_from_handle(heap, &idx)) re5_fatal("kernel32", "HeapReAlloc: heap inválido 0x%08X", heap);
    check_block(idx, ptr, "HeapReAlloc");
    uint32_t old = hdr_size(ptr);
    int in_place = hdr_magic(ptr) == MAGIC_LARGE ? size <= capacity(ptr)
                                                 : size <= 0x7FFF0000u && class_of(size + HDR) == class_of(old + HDR);
    if (in_place) {
        if (size > old && (flags & HEAP_ZERO_MEMORY)) memset(re5_g2h(ptr + old), 0, size - old);
        k32_w32(ptr - HDR, size);
        c->eax = ptr;
        hunlock();
        return;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        c->eax = 0;
        hunlock();
        return;
    }
    uint32_t np = heap_alloc_idx(idx, flags & HEAP_ZERO_MEMORY, size);
    if (!np) {
        c->eax = 0;
        hunlock();
        return;
    }
    memcpy(re5_g2h(np), re5_g2h(ptr), old < size ? old : size);
    heap_free_idx(idx, ptr);
    c->eax = np;
    hunlock();
}

/* ---- VirtualAlloc / VirtualFree ---- */

#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u

static void k_VirtualAlloc(CPU *c)
{
    uint32_t addr = re5_arg(c, 0), size = re5_arg(c, 1), type = re5_arg(c, 2), prot = re5_arg(c, 3);
    c->eax = 0;
    if (!size) {
        re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
        return;
    }
    if (!addr) {
        /* Reserva = commit no host: as páginas só ocupam RAM quando tocadas. */
        c->eax = re5_mem_alloc(size, prot, (type & MEM_COMMIT) ? "VirtualAlloc" : "VirtualAlloc-reserva");
        if (!c->eax) re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    if (type & MEM_RESERVE) {
        uint32_t base = addr & ~(RE5_MEM_GRANULARITY - 1);
        uint32_t len = (addr - base + size + RE5_MEM_PAGE - 1) & ~(RE5_MEM_PAGE - 1);
        if (re5_mem_map_fixed(base, len, prot, "VirtualAlloc-fixo") == 0) c->eax = base;
        else re5_set_last_error(K32_ERROR_INVALID_ADDRESS);
        return;
    }
    /* MEM_COMMIT dentro de uma reserva já feita. */
    uint32_t lo = addr & ~(RE5_MEM_PAGE - 1);
    uint32_t len = (addr - lo + size + RE5_MEM_PAGE - 1) & ~(RE5_MEM_PAGE - 1);
    if (re5_mem_is_mapped(lo, len) && re5_mem_protect(lo, len, prot, NULL) == 0) c->eax = lo;
    else re5_set_last_error(K32_ERROR_INVALID_ADDRESS);
}

static void k_VirtualFree(CPU *c)
{
    uint32_t addr = re5_arg(c, 0), size = re5_arg(c, 1), type = re5_arg(c, 2);
    c->eax = 0;
    if (type & MEM_RELEASE) {
        if (size == 0 && re5_mem_free(addr) == 0) c->eax = 1;
        else re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
        return;
    }
    if (type & MEM_DECOMMIT) {
        /* A região continua do guest; o que importa é o recommit voltar zerado. */
        uint32_t lo = addr & ~(RE5_MEM_PAGE - 1);
        uint32_t len = (addr - lo + size + RE5_MEM_PAGE - 1) & ~(RE5_MEM_PAGE - 1);
        if (!size) {
            re5_mem_region r;
            if (re5_mem_query(addr, &r) != 0) { re5_set_last_error(K32_ERROR_INVALID_ADDRESS); return; }
            lo = r.base;
            len = r.size;
        }
        if (!re5_mem_is_mapped(lo, len)) { re5_set_last_error(K32_ERROR_INVALID_ADDRESS); return; }
        if (re5_mem_zero(lo, len) != 0) memset(re5_g2h(lo), 0, len);   /* atravessa regiões: devagar */
        c->eax = 1;
        return;
    }
    re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
}

/* ---- Global* (clipboard e código antigo): memória fixa do heap do processo ---- */

#define GMEM_ZEROINIT 0x40u

static void k_GlobalAlloc(CPU *c)
{
    uint32_t flags = re5_arg(c, 0), size = re5_arg(c, 1);
    c->eax = k32_heap_alloc(k32_process_heap(), (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, size);
    if (!c->eax) re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY);
}

/* HGLOBAL == ponteiro: Lock devolve o próprio bloco, Unlock não tem contagem. */
static void k_GlobalLock(CPU *c) { c->eax = re5_arg(c, 0); }

static void k_GlobalUnlock(CPU *c)
{
    re5_set_last_error(K32_ERROR_SUCCESS);
    c->eax = 0;
}

static const re5_import_def k_defs[] = {
    {"GetProcessHeap", k_GetProcessHeap, 0, 0},
    {"GlobalAlloc",    k_GlobalAlloc,    8, 0},
    {"GlobalLock",     k_GlobalLock,     4, 0},
    {"GlobalUnlock",   k_GlobalUnlock,   4, 0},
    {"HeapAlloc",      k_HeapAlloc,     12, 0, RE5_THUNK_NOLOCK_HEAP},
    {"HeapCreate",     k_HeapCreate,    12, 0},
    {"HeapDestroy",    k_HeapDestroy,    4, 0},
    {"HeapFree",       k_HeapFree,      12, 0, RE5_THUNK_NOLOCK_HEAP},
    {"HeapReAlloc",    k_HeapReAlloc,   16, 0, RE5_THUNK_NOLOCK_HEAP},
    {"HeapSize",       k_HeapSize,      12, 0, RE5_THUNK_NOLOCK_HEAP},
    {"VirtualAlloc",   k_VirtualAlloc,  16, 0},
    {"VirtualFree",    k_VirtualFree,   12, 0, RE5_THUNK_NOLOCK},   /* a memória do guest tem mutex próprio */
    {0},
};

const re5_import_module re5_mod_kernel32_heap = {"kernel32.dll", k_defs};
