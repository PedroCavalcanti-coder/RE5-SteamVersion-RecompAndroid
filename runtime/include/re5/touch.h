/*
 * touch.h - dispositivo de toque próprio do runtime (runtime/src/win32/touch.c).
 *
 * No Android os controles na tela (TouchControlsView) não passam pelo teclado nem
 * pelo mouse do SDL: entram aqui por JNI, marcados como toque. O runtime guarda qual
 * dispositivo foi usado por último (teclado/mouse, controle físico ou toque) e o app
 * consulta isso para esconder ou mostrar os botões.
 *
 * O jogo só conhece teclado/mouse e XInput, então o toque chega a ele assim:
 *   - botões e analógicos da tela: o "controle de toque", somado ao usuário 0 do
 *     XInput (junto com o controle físico 0, se houver). O RE5 mostra os ícones do
 *     controle (A, B, X, Y...), os mesmos rótulos dos botões da tela, e navega todos
 *     os menus pelo analógico. Quando um controle físico chega ou sai, o usuário 0
 *     some por um instante (re5_xinput_slot0_gap): o jogo vê a conexão e avisa,
 *     como no PC. O teclado físico chega pelo SDL como sempre e vira o dispositivo
 *     ativo ao ser usado;
 *   - toque direto num menu: clique do mouse na posição tocada (WM_MOUSEMOVE e
 *     WM_LBUTTON*, GetCursorPos e o botão do mouse do DirectInput), aplicado pelo
 *     user32 na thread que bombeia a fila.
 */
#ifndef RE5_TOUCH_H
#define RE5_TOUCH_H

#include <stdint.h>

/* Último dispositivo usado. */
typedef enum {
    RE5_INPUT_NONE  = 0,
    RE5_INPUT_KBM   = 1,   /* teclado ou mouse físicos */
    RE5_INPUT_PAD   = 2,   /* controle físico (Bluetooth/USB) */
    RE5_INPUT_TOUCH = 3,   /* tela de toque */
} re5_input_source;

void re5_input_note(re5_input_source s);
re5_input_source re5_input_active(void);

/* Botões no formato XINPUT_GAMEPAD_* (wButtons). */
#define RE5_TOUCH_DPAD_UP    0x0001u
#define RE5_TOUCH_DPAD_DOWN  0x0002u
#define RE5_TOUCH_DPAD_LEFT  0x0004u
#define RE5_TOUCH_DPAD_RIGHT 0x0008u
#define RE5_TOUCH_START      0x0010u
#define RE5_TOUCH_BACK       0x0020u
#define RE5_TOUCH_LTHUMB     0x0040u
#define RE5_TOUCH_RTHUMB     0x0080u
#define RE5_TOUCH_LB         0x0100u
#define RE5_TOUCH_RB         0x0200u
#define RE5_TOUCH_A          0x1000u
#define RE5_TOUCH_B          0x2000u
#define RE5_TOUCH_X          0x4000u
#define RE5_TOUCH_Y          0x8000u

/* Estado do controle de toque. Eixos em -1..1 com o Y para CIMA positivo (como o
 * XInput), gatilhos em 0..1. A zona morta e a curva já vêm aplicadas pelo app.
 * A primeira chamada "liga" o controle de toque: daí em diante o usuário 0 do
 * XInput existe mesmo sem controle físico. Um estado com algo apertado marca o
 * toque como dispositivo ativo; o estado neutro (soltar tudo) não. */
void re5_touch_set_pad(uint32_t buttons, float lx, float ly, float rx, float ry, float lt, float rt);

/* 1 se o controle de toque está ligado; preenche o XINPUT_GAMEPAD (12 bytes). */
int re5_touch_pad(uint8_t out[12]);

/* Soma dois XINPUT_GAMEPAD: botões em OU, gatilhos pelo maior, cada analógico pelo
 * de maior deflexão. Assim controle físico e toque funcionam juntos no usuário 0. */
void re5_touch_merge(uint8_t dst[12], const uint8_t src[12]);

/* Toque direto na tela (menus): posição normalizada (0..1 da área do jogo). */
enum { RE5_TOUCH_PTR_DOWN = 0, RE5_TOUCH_PTR_MOVE = 1, RE5_TOUCH_PTR_UP = 2 };
void re5_touch_pointer(int action, float nx, float ny);

typedef struct {
    int   action;
    float nx, ny;
} re5_touch_ptr_event;

/* Próximo evento de ponteiro pendente (o user32 consome no bombeamento). 0 = vazio. */
int re5_touch_take_pointer(re5_touch_ptr_event *ev);

/* Desliga o controle de toque e esvazia a fila (testes; app fechando). */
void re5_touch_reset(void);

#endif /* RE5_TOUCH_H */
