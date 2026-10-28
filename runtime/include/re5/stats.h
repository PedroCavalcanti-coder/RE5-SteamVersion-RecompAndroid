/*
 * stats.h - contadores por segundo do RE5_FPS_LOG (PLANO_MESTRE_OTIMIZACAO, fase 0.3).
 *
 * Só contam com RE5_FPS_LOG ligado (re5_stats_on); desligados, cada ponto custa uma
 * leitura de inteiro. Cada thread do guest soma nos próprios contadores (re5_thread.stats):
 * um contador global atômico, disputado pelas 8 threads a cada thunk, distorcia a medida.
 */
#ifndef RE5_STATS_H
#define RE5_STATS_H

#include <stdint.h>

enum {
    RE5_ST_THUNKS,        /* handlers do HLE chamados */
    RE5_ST_D3D9,          /* ... dos quais métodos do Direct3D 9 */
    RE5_ST_DRAWS,         /* ... e dos quais Draw* */
    RE5_ST_HEAP,          /* HeapAlloc + HeapFree + HeapReAlloc */
    RE5_ST_LOCKWAIT_NS,   /* tempo esperando o lock do guest/HLE, somado das threads */
    RE5_ST_N
};

extern int re5_stats_on;

void     re5_stat_add_thread(int which, uint64_t n);   /* na thread atual (ou num global, se do host) */
uint64_t re5_stat_total(int which);                     /* soma das threads vivas, das que saíram e do host */

#define RE5_STAT_ADD(which, n) do { if (re5_stats_on) re5_stat_add_thread((which), (uint64_t)(n)); } while (0)

/* Soma dos dispatches (call/jmp indiretos) das threads vivas. */
uint64_t re5_stat_dispatches(void);

#endif
