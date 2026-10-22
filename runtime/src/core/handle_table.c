/*
 * handle_table.c - HANDLEs de 32 bits para objetos do host (ver re5/handle_table.h).
 */
#include <stdlib.h>

#include "re5/handle_table.h"
#include "re5/log.h"
#include "host_sync.h"

#define FIRST_HANDLE 0x100u

typedef struct {
    re5_handle_type       type;
    void                 *obj;
    re5_handle_destroy_fn destroy;
    uint32_t              refs;
    uint32_t              next_free;   /* índice + 1; 0 = fim da lista */
} slot;

static slot *g_slots;
static uint32_t g_n, g_cap, g_free_head, g_live;
static re5_mutex g_mu;
static int g_ready;

static void ensure_init(void)
{
    if (!g_ready) { re5_mutex_init(&g_mu); g_ready = 1; }
}

static slot *lookup(uint32_t h)
{
    if (h < FIRST_HANDLE || (h & 3u)) return NULL;
    uint32_t idx = (h - FIRST_HANDLE) / 4u;
    if (idx >= g_n || g_slots[idx].type == RE5_H_FREE) return NULL;
    return &g_slots[idx];
}

uint32_t re5_handle_new(re5_handle_type type, void *obj, re5_handle_destroy_fn destroy)
{
    ensure_init();
    re5_mutex_lock(&g_mu);
    uint32_t idx;
    if (g_free_head) {
        idx = g_free_head - 1;
        g_free_head = g_slots[idx].next_free;
    } else {
        if (g_n == g_cap) {
            uint32_t cap = g_cap ? g_cap * 2 : 256;
            slot *s = realloc(g_slots, cap * sizeof *s);
            if (!s) re5_fatal("handle", "sem memória para a tabela de handles");
            g_slots = s;
            g_cap = cap;
        }
        idx = g_n++;
    }
    g_slots[idx] = (slot){type, obj, destroy, 1, 0};
    g_live++;
    re5_mutex_unlock(&g_mu);
    return FIRST_HANDLE + idx * 4u;
}

void *re5_handle_get(uint32_t h, re5_handle_type expect)
{
    ensure_init();
    re5_mutex_lock(&g_mu);
    slot *s = lookup(h);
    void *obj = (s && (expect == RE5_H_ANY || s->type == expect)) ? s->obj : NULL;
    re5_mutex_unlock(&g_mu);
    return obj;
}

re5_handle_type re5_handle_type_of(uint32_t h)
{
    ensure_init();
    re5_mutex_lock(&g_mu);
    slot *s = lookup(h);
    re5_handle_type t = s ? s->type : RE5_H_FREE;
    re5_mutex_unlock(&g_mu);
    return t;
}

uint32_t re5_handle_dup(uint32_t h)
{
    ensure_init();
    re5_mutex_lock(&g_mu);
    slot *s = lookup(h);
    if (s) s->refs++;
    re5_mutex_unlock(&g_mu);
    return s ? h : 0;
}

int re5_handle_close(uint32_t h)
{
    ensure_init();
    re5_mutex_lock(&g_mu);
    slot *s = lookup(h);
    if (!s) { re5_mutex_unlock(&g_mu); return -1; }
    void *obj = NULL;
    re5_handle_destroy_fn destroy = NULL;
    if (--s->refs == 0) {
        obj = s->obj;
        destroy = s->destroy;
        uint32_t idx = (uint32_t)(s - g_slots);
        *s = (slot){RE5_H_FREE, NULL, NULL, 0, g_free_head};
        g_free_head = idx + 1;
        g_live--;
    }
    re5_mutex_unlock(&g_mu);
    if (destroy) destroy(obj);   /* fora do lock: o destrutor pode fechar outros handles */
    return 0;
}

uint32_t re5_handle_live_count(void) { return g_live; }
