/*
 * runtime.h - inicialização do runtime como um todo.
 */
#ifndef RE5_RUNTIME_H
#define RE5_RUNTIME_H

#include "cpu.h"
#include "re5/guest_memory.h"
#include "re5/handle_table.h"
#include "re5/image.h"
#include "re5/imports.h"
#include "re5/log.h"
#include "re5/thread.h"
#include "re5/thunks.h"
#include "re5/vfs.h"

/* Memória, thunks e log. Chamado uma vez, antes de carregar a imagem. 0 = ok. */
int re5_runtime_init(void);

/* Endereço do guest -> função gerada (busca binária em re5_lifted_table). */
re5_lifted_fn re5_lookup(uint32_t va);

/* Imprime os últimos endereços despachados pela thread atual (crash trail). */
void re5_dispatch_trail_print(void);

/* Contador e anel de endereços despachados pela thread atual (para o watchdog). */
#define RE5_TRAIL 32
void re5_dispatch_trail_ref(const volatile uint32_t **pos, const uint32_t **trail);

/* Falha do host causada pelo guest -> relatório com VA do guest, registradores
 * e trilha de dispatch (runtime/src/core/crash.c). Chamado por re5_runtime_init. */
void re5_crash_handler_install(void);

/* Corpos escritos à mão para endereços em config/overrides.txt. */
typedef struct {
    uint32_t      va;
    re5_lifted_fn fn;
    const char   *reason;
} re5_override_def;

re5_lifted_fn re5_override_get(uint32_t va);

#endif /* RE5_RUNTIME_H */
