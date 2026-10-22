/*
 * d3d9_bridge.h - ponte COM do Direct3D 9 (etapa 4.2, decisão D4).
 *
 * O guest recebe, para cada objeto do Direct3D do host (IDirect3D9,
 * IDirect3DDevice9, texturas, buffers...), um objeto-sombra de 16 bytes na
 * memória do guest:
 *   +0  vtable (uma por interface, de thunks stdcall: COM conta o this)
 *   +4  handle (re5_handle_new, tipo RE5_H_COM) -> objeto do host
 *   +8  tipo (d9_kind)
 *   +12 'D3D9'
 * Ponteiros do host nunca entram na memória do guest. A tabela de métodos vem
 * do d3d9.h (scripts/gen_d3d9_bridge.py); método sem handler aborta com
 * "Interface::Método sem implementação" (D8).
 *
 * O backend é o Direct3D 9 do host: no Windows o d3d9.dll x64 (o do sistema ou
 * o do DXVK, via RE5_D3D9_DLL); no Linux/Android a DXVK native
 * (libdxvk_d3d9.so, via RE5_D3D9_LIB). Os testes trocam o backend por um falso.
 */
#ifndef RE5_D3D9_BRIDGE_H
#define RE5_D3D9_BRIDGE_H

#include <stdint.h>

typedef struct {
    const char *name;
    /* Direct3DCreate9 do host: devolve um IDirect3D9* do host, ou NULL. */
    void *(*create9)(uint32_t sdk_version);
    /* HWND do guest -> janela que o Direct3D do host aceita (HWND nativo no
     * Windows, SDL_Window* na DXVK native). NULL se a janela não existe no host. */
    void *(*window)(uint32_t guest_hwnd);
} re5_d3d9_backend;

/* Backend do host (runtime/src/gfx/d3d9_host.c). */
const re5_d3d9_backend *re5_d3d9_host_backend(void);

/* Troca o backend (testes). NULL volta ao do host. */
void re5_d3d9_set_backend(const re5_d3d9_backend *b);

/* D3DPRESENT_PARAMETERS do guest (56 bytes, HWND de 32 bits) <-> do host
 * (`host` aponta um D3DPRESENT_PARAMETERS do host). from_guest traduz
 * hDeviceWindow pelo backend; -1 se a janela não existe no host. to_guest
 * devolve o que o Direct3D preencheu (BackBufferCount 0 -> 1...), menos o
 * hDeviceWindow, que continua o do guest. */
int  re5_d3d9_pp_from_guest(uint32_t va, void *host);
void re5_d3d9_pp_to_guest(const void *host, uint32_t va);

/* Objeto do host atrás de um objeto do guest (NULL se não for da ponte). */
void *re5_d3d9_host_object(uint32_t guest_obj);

/* Diagnóstico: Present já feitos e objetos do guest vivos. */
uint32_t re5_d3d9_present_count(void);
uint32_t re5_d3d9_live_objects(void);

#endif /* RE5_D3D9_BRIDGE_H */
