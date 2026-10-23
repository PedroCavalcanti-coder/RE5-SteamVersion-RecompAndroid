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

/* Nome de função do guest (carregado de work/names.tsv), ou NULL. */
void re5_names_init(void);
const char *re5_name_lookup(uint32_t va);
/* Cobertura de funções (trace.c): F8 zera, F9 grava (RE5_COV=1). */
void re5_cov_key(int fkey);
/* Despejo da memória do guest (memdump.c): F10 e RE5_DUMP_AT=<Present,...> com RE5_DUMP=1. */
void re5_dump_key(int fkey);
void re5_dump_at_present(uint32_t n);
/* Despejo com nome dado (<RE5_DUMP_DIR ou work/dump>/<nome>.bin/.map), sem depender de RE5_DUMP. */
void re5_dump_named(const char *name);
/* Captura de uma chamada (trace.c): F11 arma; a próxima chamada de RE5_CAPTURE=<VA> (um tracepoint)
 * grava registradores e memória na entrada e na saída, para o scripts/snap_diff.py. */
void re5_capture_key(void);
/* RE5_CAPTURE_AT=<n>[,<n>...]: arma a captura (como o F11) no n-ésimo Present, sem tecla
 * (validação automática com o scripts/bench_re5.py). */
void re5_capture_at_present(uint32_t present);

/* Deterministic Replay & Trace Diff (estilo TAS / Lockstep) */
void     re5_det_init(void);
int      re5_det_is_enabled(void);
uint64_t re5_det_qpc(void);
uint32_t re5_det_tick(void);
void     re5_det_on_present(void);
void     re5_det_on_trace(CPU *c, uint32_t va, uint32_t hook_site);
int      re5_det_get_tas_input(uint32_t user, int16_t *stickX, int16_t *stickY);
void     re5_det_key(int fkey);

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
