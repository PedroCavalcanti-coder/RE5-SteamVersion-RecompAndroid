/*
 * handle_table.c - HANDLEs de 32 bits para objetos do host (ver re5/handle_table.h).
 *
 * Leitura sem mutex (re5_handle_get / re5_handle_type_of): toda chamada D3D9 resolve o objeto
 * `this` e os argumentos por aqui (~1 milhão por segundo na thread de render), e o SetEvent e as
 * esperas da thread principal também. Com um mutex global as duas threads disputavam a mesma
 * linha (RE5_PROFILE, 2026-11-03: a principal "parada em SetEvent" em 15% das esperas).
 *
 * Os slots moram em blocos fixos que nunca mudam de lugar nem são liberados (antes um realloc
 * movia o array, e por isso a leitura precisava do mutex). Criar, duplicar e fechar continuam
 * sob o mutex. O slot é publicado com o objeto primeiro e o tipo por último (release); o leitor
 * lê o tipo (acquire), o objeto e o tipo de novo, e só aceita se o tipo não mudou. Ler um handle
 * que outra thread fecha ao mesmo tempo continua sendo um uso depois de fechar do próprio jogo,
 * como antes: o mutex nunca impediu que o objeto sumisse logo depois do get.
 */
#include <stdlib.h>

#include "re5/handle_table.h"
#include "re5/log.h"
#include "host_sync.h"

#define FIRST_HANDLE 0x100u
#define CHUNK_BITS   10u
#define CHUNK        (1u << CHUNK_BITS)   /* slots por bloco */
#define MAX_CHUNKS   4096u                /* 4M handles */

typedef struct {
    re5_handle_type       type;        /* lido sem mutex: atômico */
    void                 *obj;         /* lido sem mutex: atômico */
    re5_handle_destroy_fn destroy;
    uint32_t              refs;
    uint32_t              next_free;   /* índice + 1; 0 = fim da lista */
} slot;

static slot *g_chunks[MAX_CHUNKS];     /* blocos publicados com release */
static uint32_t g_n;                   /* slots já usados alguma vez (publicado com release) */
static uint32_t g_free_head, g_live;
static re5_mutex g_mu;
static int g_ready;

static void ensure_init(void)
{
    if (!g_ready) { re5_mutex_init(&g_mu); g_ready = 1; }
}

static slot *slot_at(uint32_t idx)
{
    slot *c = __atomic_load_n(&g_chunks[idx >> CHUNK_BITS], __ATOMIC_ACQUIRE);
    return c ? &c[idx & (CHUNK - 1u)] : NULL;
}

/* Sob o mutex. */
static slot *lookup(uint32_t h)
{
    if (h < FIRST_HANDLE || (h & 3u)) return NULL;
    uint32_t idx = (h - FIRST_HANDLE) / 4u;
    if (idx >= g_n) return NULL;
    slot *s = slot_at(idx);
    return s && s->type != RE5_H_FREE ? s : NULL;
}

/* Sem o mutex: tipo e objeto de um mesmo estado do slot. */
static int read_slot(uint32_t h, re5_handle_type *type, void **obj)
{
    if (h < FIRST_HANDLE || (h & 3u)) return 0;
    uint32_t idx = (h - FIRST_HANDLE) / 4u;
    if (idx >= __atomic_load_n(&g_n, __ATOMIC_ACQUIRE)) return 0;
    slot *s = slot_at(idx);
    if (!s) return 0;
    for (;;) {
        re5_handle_type t = __atomic_load_n(&s->type, __ATOMIC_ACQUIRE);
        void *o = __atomic_load_n(&s->obj, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->type, __ATOMIC_ACQUIRE) != t) continue;
        if (t == RE5_H_FREE) return 0;
        *type = t;
        *obj = o;
        return 1;
    }
}

uint32_t re5_handle_new(re5_handle_type type, void *obj, re5_handle_destroy_fn destroy)
{
    ensure_init();
    re5_mutex_lock(&g_mu);
    uint32_t idx;
    if (g_free_head) {
        idx = g_free_head - 1;
        g_free_head = slot_at(idx)->next_free;
    } else {
        idx = g_n;
        if ((idx >> CHUNK_BITS) >= MAX_CHUNKS) re5_fatal("handle", "tabela de handles cheia (%u)", idx);
        if (!g_chunks[idx >> CHUNK_BITS]) {
            slot *c = calloc(CHUNK, sizeof *c);
            if (!c) re5_fatal("handle", "sem memória para a tabela de handles");
            __atomic_store_n(&g_chunks[idx >> CHUNK_BITS], c, __ATOMIC_RELEASE);
        }
    }
    slot *s = slot_at(idx);
    s->destroy = destroy;
    s->refs = 1;
    s->next_free = 0;
    __atomic_store_n(&s->obj, obj, __ATOMIC_RELEASE);
    __atomic_store_n(&s->type, type, __ATOMIC_RELEASE);   /* por último: o slot passa a valer */
    if (idx == g_n) __atomic_store_n(&g_n, idx + 1, __ATOMIC_RELEASE);
    g_live++;
    re5_mutex_unlock(&g_mu);
    return FIRST_HANDLE + idx * 4u;
}

void *re5_handle_get(uint32_t h, re5_handle_type expect)
{
    re5_handle_type t;
    void *obj;
    if (!read_slot(h, &t, &obj)) return NULL;
    return expect == RE5_H_ANY || t == expect ? obj : NULL;
}

re5_handle_type re5_handle_type_of(uint32_t h)
{
    re5_handle_type t;
    void *obj;
    return read_slot(h, &t, &obj) ? t : RE5_H_FREE;
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
        uint32_t idx = (h - FIRST_HANDLE) / 4u;
        __atomic_store_n(&s->type, RE5_H_FREE, __ATOMIC_RELEASE);   /* primeiro: deixa de valer */
        __atomic_store_n(&s->obj, NULL, __ATOMIC_RELEASE);
        s->destroy = NULL;
        s->next_free = g_free_head;
        g_free_head = idx + 1;
        g_live--;
    }
    re5_mutex_unlock(&g_mu);
    if (destroy) destroy(obj);   /* fora do lock: o destrutor pode fechar outros handles */
    return 0;
}

uint32_t re5_handle_live_count(void) { return g_live; }
