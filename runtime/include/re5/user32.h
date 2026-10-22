/*
 * user32.h - o que o HLE do user32 (runtime/src/win32/user32.c) oferece às
 * outras pontes: a janela do host atrás de um HWND do guest.
 */
#ifndef RE5_USER32_H
#define RE5_USER32_H

#include <stdint.h>

struct SDL_Window;

/* Janela SDL2 do HWND do guest; NULL se o HWND não existe ou se não há
 * janela de verdade (RE5_HEADLESS). */
struct SDL_Window *re5_user32_sdl_window(uint32_t hwnd);

/* Área cliente do HWND do guest. 0 = ok, -1 se o HWND não existe. */
int re5_user32_client_size(uint32_t hwnd, int32_t *cw, int32_t *ch);

/* Entrada para o DirectInput8 (dinput8.c). O user32 é o único leitor da fila
 * de eventos do SDL; estes dão a mesma entrada no formato do DirectInput. */
union SDL_Event;
void re5_user32_handle_event(const union SDL_Event *ev);   /* um evento (o laço e os testes) */
void re5_user32_pump(void);                                /* lê a fila do SDL agora */
void re5_user32_dik_state(uint8_t out[256]);               /* 0x80 = apertada, índice DIK_* */
/* Movimento do mouse desde a última chamada (dz em múltiplos de 120 por clique). */
void re5_user32_mouse_take(int32_t *dx, int32_t *dy, int32_t *dz);
uint32_t re5_user32_mouse_buttons(void);                   /* bit 0 esquerdo, 1 direito, 2 meio, 3-4 X1/X2 */

/* Entrada roteirizada (RE5_INPUT_SCRIPT, ver user32.c): o d3d9 chama a cada
 * Present, com o número dele; aperta/solta as teclas da vez. */
void re5_user32_script_tick(uint32_t present);
/* O kernel32 avisa cada arquivo aberto (RE5_INPUT_SYNC). */
void re5_user32_script_file(const char *guest_path);

/* PostMessage para a fila do guest (WSAAsyncSelect...). Quem chama segura o lock
 * do guest: a fila só é mexida com ele. */
void re5_user32_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

/* Tela cheia do guest apresentada como janela sem bordas do tamanho do monitor
 * (SDL_WINDOW_FULLSCREEN_DESKTOP; o modo do monitor não muda). O d3d9 pede no
 * CreateDevice/Reset; a thread dona da janela aplica no próximo bombeamento. */
void re5_user32_set_fullscreen(uint32_t hwnd, int on);
/* Mouse exclusivo do DirectInput adquirido (1) ou solto (0): com ele, ou com o
 * cursor escondido pelo jogo, o mouse do host fica em modo relativo (movimento
 * sem o limite da borda da tela). */
void re5_user32_set_mouse_exclusive(int on);

#endif /* RE5_USER32_H */
