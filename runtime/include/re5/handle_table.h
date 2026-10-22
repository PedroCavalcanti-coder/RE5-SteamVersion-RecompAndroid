/*
 * handle_table.h - HANDLEs de 32 bits para objetos do host.
 *
 * O guest guarda HANDLE, HKEY, HMODULE, HWND e ponteiros COM em campos de 32
 * bits; os objetos reais do host (FILE*, pthread_mutex_t*, objetos da DXVK de
 * 64 bits) não cabem neles. A tabela entrega um valor de 32 bits estável por
 * objeto e o converte de volta, verificando o tipo.
 *
 * Valores: múltiplos de 4 a partir de 0x100, como no Windows. Nunca 0 nem os
 * pseudo-handles (0xFFFFFFFF processo atual, 0xFFFFFFFE thread atual).
 */
#ifndef RE5_HANDLE_TABLE_H
#define RE5_HANDLE_TABLE_H

#include <stdint.h>

typedef enum {
    RE5_H_FREE = 0,
    RE5_H_FILE,
    RE5_H_FIND,
    RE5_H_EVENT,
    RE5_H_MUTEX,
    RE5_H_SEMAPHORE,
    RE5_H_THREAD,
    RE5_H_HKEY,
    RE5_H_MODULE,
    RE5_H_MAPPING,
    RE5_H_COM,          /* objeto COM do host atrás de um objeto-sombra do guest (D4) */
    RE5_H_SOCKET,       /* SOCKET do Winsock -> socket do host */
    RE5_H_OTHER,
    RE5_H_ANY = 0xFF,   /* para re5_handle_get: aceita qualquer tipo */
} re5_handle_type;

#define RE5_PSEUDO_PROCESS 0xFFFFFFFFu
#define RE5_PSEUDO_THREAD  0xFFFFFFFEu

typedef void (*re5_handle_destroy_fn)(void *obj);

uint32_t re5_handle_new(re5_handle_type type, void *obj, re5_handle_destroy_fn destroy);
void    *re5_handle_get(uint32_t h, re5_handle_type expect);
re5_handle_type re5_handle_type_of(uint32_t h);
uint32_t re5_handle_dup(uint32_t h);   /* DuplicateHandle: mesmo objeto, contagem +1 */
int      re5_handle_close(uint32_t h); /* 0 = ok; destrói o objeto na última referência */
uint32_t re5_handle_live_count(void);

#endif /* RE5_HANDLE_TABLE_H */
