/*
 * thread.h - threads do guest, TEB/PEB simulados e o lock do guest.
 *
 * Cada thread do guest é uma thread real do host com:
 *   - seu próprio CPU (lift32_cpu é reentrante: uma CPU por thread);
 *   - uma pilha na memória do guest (o jogo endereça a pilha com esp);
 *   - um TEB na memória do guest, apontado por c->fs_base (fs:[0] = cadeia
 *     SEH, fs:[0x18] = self, fs:[0x30] = PEB, fs:[0x34] = LastError,
 *     fs:[0xE10] = slots de TLS).
 *
 * Lock do guest (RE5_GUEST_LOCK, ver thread.c e a decisão D5):
 *   global  só uma thread executa código do guest por vez (padrão no ARM64);
 *   hle     o código do guest roda em paralelo e o lock protege só os handlers
 *           do HLE (padrão no x86-64, onde o hardware já é TSO).
 * Nos dois, handlers que bloqueiam (Sleep, WaitForSingleObject...) DEVEM soltar o
 * lock enquanto esperam: use RE5_BLOCKING_BEGIN/END.
 */
#ifndef RE5_THREAD_H
#define RE5_THREAD_H

#include <stddef.h>
#include "cpu.h"

/* Offsets do TEB x86 que o runtime preenche. */
#define RE5_TEB_SEH        0x000u
#define RE5_TEB_STACK_HI   0x004u
#define RE5_TEB_STACK_LO   0x008u
#define RE5_TEB_SELF       0x018u
#define RE5_TEB_PID        0x020u
#define RE5_TEB_TID        0x024u
#define RE5_TEB_TLS_PTR    0x02Cu
#define RE5_TEB_PEB        0x030u
#define RE5_TEB_LAST_ERROR 0x034u
#define RE5_TEB_TLS_SLOTS  0xE10u
#define RE5_TEB_SIZE       0x2000u
#define RE5_TLS_SLOTS      64u

/* Offsets do PEB x86. */
#define RE5_PEB_BEING_DEBUGGED 0x002u
#define RE5_PEB_IMAGE_BASE     0x008u
#define RE5_PEB_PROCESS_HEAP   0x018u
#define RE5_PEB_SIZE           0x1000u

#define RE5_GUEST_PID 0x1000u

typedef struct re5_thread {
    CPU      cpu;
    uint32_t teb;
    uint32_t stack_lo, stack_hi;
    uint32_t tid;
    int      lock_depth;   /* reentrância guest -> host -> guest */
    int      suspend;      /* SuspendThread: > 0 = parada no próximo lock do guest */
    /* Diagnóstico (RE5_WATCHDOG): lidos por outra thread sem lock, só para relatório. */
    volatile int         lock_wait;    /* 1 enquanto espera o lock do guest */
    const char *volatile thunk;        /* handler do host em execução (NULL = código do guest) */
    const volatile uint32_t *trail_pos; /* contador de dispatches desta thread (dispatch.c) */
    const uint32_t      *trail;        /* últimos endereços despachados */
    void                *host;         /* Windows: HANDLE da thread do host (RE5_PROFILE) */
} re5_thread;

/* Cria o PEB (uma vez por processo). image_base vai para PEB.ImageBaseAddress. */
int re5_process_init(uint32_t image_base);
uint32_t re5_process_peb(void);
/* MXCSR com que toda thread do guest nasce: 0x9FC0 (FTZ+DAZ), ou 0x1F80 com RE5_FTZ=0. */
uint32_t re5_guest_mxcsr_default(void);

/* Dá à thread do host que chama um contexto de guest: pilha + TEB. */
re5_thread *re5_thread_attach(uint32_t stack_bytes);
void        re5_thread_detach(void);
re5_thread *re5_thread_current(void);

/* Chama a função do guest em `va` com argumentos de 32 bits (empilhados da
 * direita para a esquerda) e devolve eax. Serve para stdcall e cdecl: a pilha
 * é restaurada depois, seja quem for que limpou os argumentos. */
uint32_t re5_guest_call(re5_thread *t, uint32_t va, int argc, const uint32_t *argv);

/* Roda fn(arg) numa thread nova do host com pilha de `stack_bytes` e espera.
 * O código gerado aninha uma chamada C por chamada do guest, então a pilha do
 * host precisa ser grande. 0 = ok. */
int re5_host_thread_run(void (*fn)(void *), void *arg, size_t stack_bytes);

/* Como re5_host_thread_run, mas não espera: a thread roda solta (CreateThread
 * do guest). 0 = ok. */
int re5_host_thread_start(void (*fn)(void *), void *arg, size_t stack_bytes);

void re5_guest_lock(void);
void re5_guest_unlock(void);
/* Para handlers que bloqueiam: solta o lock (inteiro, mesmo reentrante) e o retoma. */
int  re5_guest_lock_release_all(void);
/* 1 se RE5_GUEST_LOCK=hle (o lock protege só o HLE; ver thread.c). */
int  re5_guest_lock_mode_hle(void);
void re5_guest_lock_reacquire(int depth);

/* Ponto de preempção (dispatch.c, a cada 1024 dispatches): se há thread
 * esperando o lock e esta o segura há mais de 2 ms rodando código do guest
 * (fora de handler do host), vai para o fim da fila. */
void re5_guest_preempt(void);

/* Para a thread que é dona de janelas do host: enquanto ela espera o lock do
 * guest, fn é chamada a cada 10 ms (o user32 passa SDL_PumpEvents), para o
 * Windows não marcar a janela "não respondendo" enquanto as outras threads do
 * guest rodam. fn não pode rodar código do guest nem pegar o lock. */
void re5_thread_set_lock_wait_pump(void (*fn)(void));

/* SetThreadPriority do guest na thread do host que tem esse TID (RE5_THREAD_PRIO=1, Windows). */
void re5_thread_apply_priority(uint32_t tid, int prio);

/* O mesmo bombeamento para as esperas do HLE (WaitFor*, Sleep, seção crítica
 * ocupada) na thread dona da janela: 1 se esta thread tem um; re5_thread_pump
 * o chama (sem o lock do guest e sem locks do HLE). */
int  re5_thread_has_pump(void);
void re5_thread_pump(void);

/* Cede o processador do host (use dentro de RE5_BLOCKING_BEGIN/END para dar a
 * vez a outra thread do guest que espera o lock). */
void re5_thread_yield(void);
#define RE5_BLOCKING_BEGIN() int re5__lock_depth = re5_guest_lock_release_all()
#define RE5_BLOCKING_END()   re5_guest_lock_reacquire(re5__lock_depth)

/* SuspendThread/ResumeThread de uma thread em execução. Ela só roda código do
 * guest com o lock do guest, então a suspensão vale no próximo ponto em que
 * ela o pegaria (inclusive no fim de uma espera bloqueante). Suspender a
 * própria thread solta o lock e espera ali mesmo. Devolvem a contagem
 * anterior, como o Win32. */
uint32_t re5_thread_suspend(re5_thread *t);
uint32_t re5_thread_resume(re5_thread *t);

/* RE5_WATCHDOG=<segundos>: uma thread do host relata, a cada intervalo, onde
 * cada thread do guest está (lock, thunk, dispatches andados, último VA) - para
 * travamentos sem crash ("não respondendo"). Chamado por re5_runtime_init. */
void re5_watchdog_start(void);

/* RE5_PROFILE=<segundos>: amostra as threads do guest a cada ~1 ms (no Windows:
 * suspende a thread do host e lê o RIP) e, a cada intervalo, escreve no log a
 * divisão do tempo (código gerado, handlers do host, esperando o lock) e as
 * funções do guest mais quentes. */
void re5_profile_start(void);

/* LastError no TEB da thread atual. */
void     re5_set_last_error(uint32_t err);
uint32_t re5_get_last_error(void);

#endif /* RE5_THREAD_H */
