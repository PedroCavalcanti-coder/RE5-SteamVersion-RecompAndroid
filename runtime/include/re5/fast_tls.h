/*
 * fast_tls.h - variável por thread barata nos caminhos quentes do runtime.
 *
 * No MinGW (GCC sem TLS nativo) todo `_Thread_local` vira uma chamada a
 * __emutls_get_address, que passa por pthread_getspecific da libwinpthread: o
 * dispatch, cada thunk do HLE, o lock do guest e o Get/SetLastError pagavam isso
 * milhões de vezes por segundo (RE5_PROFILE em jogo, 28/10: libwinpthread e o
 * __emutls_get_address entre as rotinas do host mais amostradas). Aqui, no
 * Windows x64, a variável é um slot de TlsAlloc lido direto do TEB
 * (gs:[0x1480 + 8*índice], os 64 slots TlsSlots): duas instruções. Nos outros
 * hosts o `_Thread_local` já é nativo e fica como está.
 *
 * Só para valores do tamanho de um ponteiro, começando em zero em cada thread:
 *
 *     RE5_TLS_PTR(re5_thread *, t_self)        -> t_self_get(), t_self_set(v)
 */
#ifndef RE5_FAST_TLS_H
#define RE5_FAST_TLS_H

#include <stdint.h>

#include "re5/platform.h"

#if defined(_WIN32) && defined(__GNUC__) && defined(__x86_64__)
#include <windows.h>

/* Aloca o índice do slot na primeira vez (com corrida resolvida por CAS) e devolve índice+1. */
uint32_t re5_tls_alloc_slow(volatile uint32_t *slot);

static inline uint32_t re5_tls_index(volatile uint32_t *slot)
{
    uint32_t s = *slot;
    if (__builtin_expect(s == 0, 0)) s = re5_tls_alloc_slow(slot);
    return s - 1;
}

static inline void *re5_tls_read(volatile uint32_t *slot)
{
    uint32_t i = re5_tls_index(slot);
    if (__builtin_expect(i < 64, 1)) return (void *)__readgsqword(0x1480 + i * 8);
    return TlsGetValue(i);
}

static inline void re5_tls_write(volatile uint32_t *slot, void *v)
{
    TlsSetValue(re5_tls_index(slot), v);
}

#define RE5_TLS_PTR(type, name)                                                          \
    static volatile uint32_t name##_slot;                                                \
    static inline type name##_get(void) { return (type)(uintptr_t)re5_tls_read(&name##_slot); } \
    static inline void name##_set(type v) { re5_tls_write(&name##_slot, (void *)(uintptr_t)v); }

#else

#define RE5_TLS_PTR(type, name)                                    \
    static RE5_THREAD_LOCAL type name##_tls;                       \
    static inline type name##_get(void) { return name##_tls; }     \
    static inline void name##_set(type v) { name##_tls = v; }

#endif

#endif /* RE5_FAST_TLS_H */
