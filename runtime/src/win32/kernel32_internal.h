/*
 * kernel32_internal.h - o que os arquivos do HLE do kernel32 compartilham.
 *
 * O kernel32 é grande demais para um arquivo: kernel32.c (módulos, tempo,
 * processo), kernel32_env.c (CRT: linha de comando, ambiente, versão, startup),
 * kernel32_heap.c (heaps, VirtualAlloc, Global*), kernel32_nls.c (code pages e
 * locale), kernel32_sync.c (seções críticas, TLS, Interlocked). Cada um exporta
 * a sua tabela re5_mod_kernel32_<parte>, todas com o nome "kernel32.dll".
 */
#ifndef RE5_KERNEL32_INTERNAL_H
#define RE5_KERNEL32_INTERNAL_H
#include <string.h>

#include "re5/runtime.h"

#define K32_ERROR_SUCCESS             0u
#define K32_ERROR_FILE_NOT_FOUND      2u
#define K32_ERROR_NOT_ENOUGH_MEMORY   8u
#define K32_ERROR_INVALID_PARAMETER   87u
#define K32_ERROR_INSUFFICIENT_BUFFER 122u
#define K32_ERROR_INVALID_ADDRESS     487u
#define K32_ERROR_NO_UNICODE_TRANSLATION 1113u
#define K32_ERROR_OLD_WIN_VERSION     1150u
#define K32_ERROR_ENVVAR_NOT_FOUND    203u

static inline void     k32_w8(uint32_t va, uint8_t v) { *(uint8_t *)re5_g2h(va) = v; }
static inline void     k32_w16(uint32_t va, uint16_t v) { memcpy(re5_g2h(va), &v, 2); }
static inline void     k32_w32(uint32_t va, uint32_t v) { memcpy(re5_g2h(va), &v, 4); }
static inline uint8_t  k32_r8(uint32_t va) { return *(const uint8_t *)re5_g2h(va); }
static inline uint16_t k32_r16(uint32_t va) { uint16_t v; memcpy(&v, re5_g2h(va), 2); return v; }
static inline uint32_t k32_r32(uint32_t va) { uint32_t v; memcpy(&v, re5_g2h(va), 4); return v; }

/* Comprimento de uma string UTF-16 do guest, em unidades. */
static inline uint32_t k32_wcslen(uint32_t va)
{
    uint32_t n = 0;
    while (k32_r16(va + 2 * n)) n++;
    return n;
}

/* Heap do processo (GetProcessHeap): criado na primeira chamada, que também
 * grava PEB.ProcessHeap. Para o HLE alocar memória que o guest vai ler. */
uint32_t k32_process_heap(void);
uint32_t k32_heap_alloc(uint32_t heap, uint32_t flags, uint32_t size);
int      k32_heap_free(uint32_t heap, uint32_t ptr);

/* Milissegundos do relógio monotônico do host (GetTickCount, timeGetTime). */
uint32_t k32_tick_ms(void);

/* Sinaliza um evento do guest (OVERLAPPED.hEvent). 0 se o handle não é evento. */
int k32_event_signal(uint32_t h);

/* APCs da thread atual (ReadFileEx): rodam na próxima espera alertável
 * (SleepEx com bAlertable). fn é um VA do guest chamado como stdcall(a0, a1, a2). */
void k32_apc_queue(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
int  k32_apc_run(void);   /* roda as pendentes; devolve quantas */

/* Versão do Windows que o guest enxerga (GetVersionExA, VerifyVersionInfoA). */
#define K32_OS_MAJOR 6u
#define K32_OS_MINOR 1u
#define K32_OS_BUILD 7601u
#define K32_OS_SP_MAJOR 1u
#define K32_OS_SP_NAME "Service Pack 1"

/* Pasta e caminho do executável como o guest os vê. */
#define K32_EXE_PATH RE5_VFS_INSTALL_DIR "\\re5dx9.exe"

#endif /* RE5_KERNEL32_INTERNAL_H */
