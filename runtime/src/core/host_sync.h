/*
 * host_sync.h - mutex do host (interno ao runtime).
 *
 * Recursivo nas duas plataformas: CRITICAL_SECTION já é; no POSIX pedimos
 * PTHREAD_MUTEX_RECURSIVE. Usado pelas tabelas do runtime e pelo lock do guest.
 */
#ifndef RE5_HOST_SYNC_H
#define RE5_HOST_SYNC_H

#include <stdint.h>

#include "re5/platform.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
typedef struct { CRITICAL_SECTION cs; int ready; } re5_mutex;
static inline void re5_mutex_init(re5_mutex *m) { InitializeCriticalSection(&m->cs); m->ready = 1; }
static inline void re5_mutex_lock(re5_mutex *m) { EnterCriticalSection(&m->cs); }
static inline void re5_mutex_unlock(re5_mutex *m) { LeaveCriticalSection(&m->cs); }
static inline int re5_mutex_trylock(re5_mutex *m) { return TryEnterCriticalSection(&m->cs) != 0; }

/* Variável de condição sobre um re5_mutex travado UMA vez. ms = 0xFFFFFFFF espera
 * sem prazo. Devolve 0 se acordou, 1 se o prazo venceu. */
typedef struct { CONDITION_VARIABLE cv; } re5_cond;
static inline void re5_cond_init(re5_cond *c) { InitializeConditionVariable(&c->cv); }
static inline void re5_cond_broadcast(re5_cond *c) { WakeAllConditionVariable(&c->cv); }
static inline void re5_cond_signal(re5_cond *c) { WakeConditionVariable(&c->cv); }
static inline int re5_cond_wait_ms(re5_cond *c, re5_mutex *m, uint32_t ms)
{
    return SleepConditionVariableCS(&c->cv, &m->cs, ms == 0xFFFFFFFFu ? INFINITE : ms) ? 0 : 1;
}
static inline void re5_host_yield(void) { SwitchToThread(); }
static inline void re5_host_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
typedef struct { pthread_mutex_t mu; int ready; } re5_mutex;
static inline void re5_mutex_init(re5_mutex *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mu, &a);
    pthread_mutexattr_destroy(&a);
    m->ready = 1;
}
static inline void re5_mutex_lock(re5_mutex *m) { pthread_mutex_lock(&m->mu); }
static inline void re5_mutex_unlock(re5_mutex *m) { pthread_mutex_unlock(&m->mu); }
static inline int re5_mutex_trylock(re5_mutex *m) { return pthread_mutex_trylock(&m->mu) == 0; }

typedef struct { pthread_cond_t cv; } re5_cond;
static inline void re5_cond_init(re5_cond *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->cv, &a);
    pthread_condattr_destroy(&a);
}
static inline void re5_cond_broadcast(re5_cond *c) { pthread_cond_broadcast(&c->cv); }
static inline void re5_cond_signal(re5_cond *c) { pthread_cond_signal(&c->cv); }
static inline int re5_cond_wait_ms(re5_cond *c, re5_mutex *m, uint32_t ms)
{
    if (ms == 0xFFFFFFFFu) return pthread_cond_wait(&c->cv, &m->mu) ? 1 : 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c->cv, &m->mu, &ts) == ETIMEDOUT ? 1 : 0;
}
static inline void re5_host_yield(void) { sched_yield(); }
static inline void re5_host_sleep_ms(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif

#endif /* RE5_HOST_SYNC_H */
