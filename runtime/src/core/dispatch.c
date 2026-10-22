/*
 * dispatch.c - endereço do guest -> código (ver re5/cpu_epilogue.h).
 *
 * O lift32_cpu traduz todo call/jmp para fora da função corrente em
 * dispatch(c, va). Aqui va vira: um thunk do host, uma função gerada (tabela
 * hash montada da tabela ordenada no primeiro uso) ou um erro fatal que diz o
 * endereço. Endereço
 * sem função gerada é o defeito mais comum do bring-up: a trilha impressa no
 * erro alimenta a próxima rodada do catálogo (Fase 2, etapa 2.4).
 */
#include <stdio.h>
#include <stdlib.h>

#include "re5/image.h"
#include "re5/log.h"
#include "re5/runtime.h"

uint32_t g_image_delta;   /* cpu.h: GVA(). Sempre 0: a imagem não tem relocações. */

#define TRAIL RE5_TRAIL
/* Um bloco só: no MinGW o TLS é emulado (__emutls_get_address por variável),
 * e o dispatch roda a cada call/jmp do jogo. */
typedef struct { uint32_t pos; uint32_t va[TRAIL]; } trail_t;
static RE5_THREAD_LOCAL trail_t t_trail;

void re5_dispatch_trail_ref(const volatile uint32_t **pos, const uint32_t **trail)
{
    *pos = &t_trail.pos;
    *trail = t_trail.va;
}

/* VA -> função gerada. A busca binária nas ~70 mil entradas eram ~17 passos
 * com faltas de cache a cada call/jmp (RE5_PROFILE no demo do jogo: dispatch
 * era a função do host mais amostrada). Endereçamento aberto, carga <= 50%. */
typedef struct { uint32_t va; re5_lifted_fn fn; } slot_t;
static slot_t *g_slots;
static uint32_t g_mask, g_shift;

static inline uint32_t slot_of(uint32_t va) { return (va * 0x9E3779B1u) >> g_shift; }

static slot_t *build(void)
{
    uint32_t bits = 4;
    while ((1u << bits) < 2u * re5_lifted_count) bits++;
    slot_t *t = calloc((size_t)1 << bits, sizeof *t);
    if (!t) re5_fatal("dispatch", "sem memória para a tabela de funções");
    g_mask = (1u << bits) - 1;
    g_shift = 32 - bits;
    for (uint32_t i = 0; i < re5_lifted_count; i++) {
        uint32_t h = slot_of(re5_lifted_table[i].va);
        while (t[h].fn) h = (h + 1) & g_mask;
        t[h] = (slot_t){re5_lifted_table[i].va, re5_lifted_table[i].fn};
    }
    return t;
}

re5_lifted_fn re5_lookup(uint32_t va)
{
    slot_t *t = g_slots;
    if (!t) t = g_slots = build();   /* re5_runtime_init já montou, com uma thread só */
    for (uint32_t h = slot_of(va);; h = (h + 1) & g_mask) {
        if (t[h].va == va && t[h].fn) return t[h].fn;
        if (!t[h].fn) return NULL;
    }
}

void re5_dispatch_trail_print(void)
{
    trail_t *tr = &t_trail;
    if (!tr->pos) return;
    RE5_ERROR("dispatch", "trilha de dispatch desta thread (mais recente primeiro):");
    uint32_t n = tr->pos < TRAIL ? tr->pos : TRAIL;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t va = tr->va[(tr->pos - 1 - i) % TRAIL];
        const re5_thunk *t = re5_thunk_get(va);
        RE5_ERROR("dispatch", "  0x%08X%s%s", va, t ? "  " : "", t ? t->name : "");
    }
}

static RE5_NORETURN void unresolved(CPU *c, uint32_t va, const char *how)
{
    const re5_image *img = re5_image_main();
    const char *hint = "fora da imagem: ponteiro de função corrompido ou objeto do host sem vtable de thunks";
    if (img && va >= img->base && va < img->base + img->size) {
        hint = "dentro da imagem: função que o catálogo não encontrou (etapa 2.6: run_lift.py catalog "
               "--add-seeds <RE5_SEED_LOG>, lift e build)";
        /* Etapa 2.6: o endereço vira semente do próximo catálogo. */
        const char *seed_log = getenv("RE5_SEED_LOG");
        FILE *f = seed_log ? fopen(seed_log, "a") : NULL;
        if (f) {
            fprintf(f, "0x%08X\n", va);
            fclose(f);
        }
    }
    re5_fatal("dispatch", "%s para 0x%08X sem função gerada (esp=0x%08X eax=0x%08X ecx=0x%08X) - %s",
              how, va, c->esp, c->eax, c->ecx, hint);
}

static inline void go(CPU *c, uint32_t va, const char *how)
{
    trail_t *tr = &t_trail;
    uint32_t n = ++tr->pos;
    tr->va[(n - 1) % TRAIL] = va;
    if ((n & 1023u) == 0) re5_guest_preempt();
    if (re5_is_thunk(va)) {
        re5_thunk_invoke(c, va);
        return;
    }
    re5_lifted_fn fn = re5_lookup(va);
    if (!fn) unresolved(c, va, how);
    fn(c);
}

void dispatch(CPU *c, uint32_t va) { go(c, va, "call/jmp"); }
void dispatch_jmp(CPU *c, uint32_t va) { go(c, va, "jmp indireto"); }
