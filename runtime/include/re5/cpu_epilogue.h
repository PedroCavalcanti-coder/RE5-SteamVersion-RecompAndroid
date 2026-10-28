/*
 * cpu_epilogue.h - anexado ao FIM do cpu.h gerado: a ABI entre o código
 * gerado (generated/src, saída do scripts/run_lift.py) e o runtime.
 *
 * O código gerado só chama estas coisas do runtime:
 *   dispatch(c, va)      call/jmp para outro endereço do guest
 *   RE5_CALL(c, L_xxx)   o mesmo, direto, quando o alvo é constante e gerado
 *   dispatch_jmp(c, va)  jmp indireto (mesmo efeito; separado para diagnóstico)
 *   re5_override(c, va)  corpo escrito à mão (config/overrides.txt)
 *   recomp_cpuid(c)      a CPU que o jogo enxerga (runtime/src/core/cpu_ext.c)
 *   recomp_rdtsc(c)      contador de ciclos virtual
 * e o runtime só chama do código gerado a tabela re5_lifted_table.
 */
#ifndef RE5_CPU_EPILOGUE_H
#define RE5_CPU_EPILOGUE_H

typedef void (*re5_lifted_fn)(CPU *c);

/* As funções geradas recebem `CPU *RE5_RESTRICT c`: a CPU fica no heap do host
 * e a memória do guest nunca a apelida (run_lift.py). */
#if defined(_MSC_VER)
#define RE5_RESTRICT __restrict
#else
#define RE5_RESTRICT restrict
#endif

/* Na cabeça de cada laço do código gerado (run_lift.py): a memória do guest é
 * relida a cada volta, como no x86. Uma espera ocupada do jogo numa flag que
 * outra thread escreve não pode virar laço infinito por otimização. */
#if defined(_MSC_VER)
#include <intrin.h>
#define RE5_LOOP_BARRIER() _ReadWriteBarrier()
#else
#define RE5_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

typedef struct {
    uint32_t      va;
    re5_lifted_fn fn;
} re5_func_entry;

/* Ordenada por va. Definida por generated/src/re5_dispatch_table.c, ou pelo
 * stub generated/stub/re5_generated_stub.c antes da Fase 2. */
extern const re5_func_entry re5_lifted_table[];
extern const uint32_t       re5_lifted_count;

void dispatch(CPU *c, uint32_t va);
void dispatch_jmp(CPU *c, uint32_t va);
void re5_override(CPU *c, uint32_t va);
void recomp_cpuid(CPU *c);
void recomp_rdtsc(CPU *c);
void re5_guest_preempt(void);

/* call/jmp com alvo constante que existe no catálogo: o run_lift.py troca o
 * dispatch(c, va) por uma chamada direta (lift.direct_calls). Fica o ponto de
 * preempção a cada 1024 chamadas, como no dispatch (o modo global do lock
 * depende dele para revezar as threads), contado na CPU da thread. Não entra
 * na trilha de diagnóstico do dispatch. */
#define RE5_CALL(c, fn) do { if ((++(c)->ncalls & 1023u) == 0) re5_guest_preempt(); fn(c); } while (0)

/* jmp com alvo constante para outra função (lift.tail_calls): o `return` faz parte do macro.
 * dispatch_tail(c, va) cai no dispatch; RE5_TAILCALL(c, fn) é a chamada direta em posição de
 * cauda - o compilador a faz virar um jmp (-foptimize-sibling-calls, ou RE5_USE_MUSTTAIL com
 * Clang/GCC 15 para garantir), então a pilha do host não cresce a cada jmp entre funções. */
#define dispatch_tail(c, va) do { dispatch(c, va); return; } while (0)
#ifdef RE5_USE_MUSTTAIL
#define RE5_MUSTTAIL __attribute__((musttail))
#else
#define RE5_MUSTTAIL
#endif
#define RE5_TAILCALL(c, fn) do { if ((++(c)->ncalls & 1023u) == 0) re5_guest_preempt(); RE5_MUSTTAIL return fn(c); } while (0)

#endif /* RE5_CPU_EPILOGUE_H */
