/*
 * stats.h - contadores por segundo do RE5_FPS_LOG (PLANO_MESTRE_OTIMIZACAO, fase 0.3).
 *
 * Só contam com RE5_FPS_LOG ligado (re5_stats_on); desligados, cada ponto custa uma
 * leitura de inteiro. Contadores relaxados: servem para ordem de grandeza, não para
 * sincronizar nada.
 */
#ifndef RE5_STATS_H
#define RE5_STATS_H

#include <stdint.h>

extern int re5_stats_on;
extern uint64_t re5_stat_thunks;        /* handlers do HLE chamados */
extern uint64_t re5_stat_d3d9;          /* ... dos quais métodos do Direct3D 9 */
extern uint64_t re5_stat_draws;         /* ... e dos quais Draw* */
extern uint64_t re5_stat_heap;          /* HeapAlloc + HeapFree + HeapReAlloc */
extern uint64_t re5_stat_lockwait_ns;   /* tempo esperando o lock do guest/HLE, somado das threads */

#define RE5_STAT_ADD(v, n) do { if (re5_stats_on) __atomic_fetch_add(&(v), (uint64_t)(n), __ATOMIC_RELAXED); } while (0)

/* Soma dos dispatches (call/jmp indiretos) das threads vivas. */
uint64_t re5_stat_dispatches(void);

#endif
