/*
 * XInput 1.3 sobre SDL_GameController: sem janela (RE5_HEADLESS) não há
 * controle, e os erros são os do Win32; a montagem do XINPUT_GAMEPAD a partir
 * do estado do SDL (botões, gatilhos, eixo Y invertido) é testada direto.
 */
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/touch.h"
#include "re5/xinput.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;

static uint32_t call(const char *name, int n, const uint32_t *args)
{
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, re5_import_bind("xinput1_3.dll", name, 0), n, args);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static int16_t s16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int main(void)
{
#if defined(_WIN32)
    _putenv("RE5_HEADLESS=1");
#else
    setenv("RE5_HEADLESS", "1", 1);
#endif
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    uint32_t buf = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");

    CHECK_EQ_U32(re5_thunk_get(re5_import_bind("xinput1_3.dll", "XInputGetState", 0))->arg_bytes, 8);
    CHECK_EQ_U32(re5_thunk_get(re5_import_bind("xinput1_3.dll", "XInputGetCapabilities", 0))->arg_bytes, 12);
    for (uint32_t u = 0; u < 4; u++) {
        const uint32_t a[2] = {u, buf};
        CHECK_EQ_U32(call("XInputGetState", 2, a), 1167);   /* ERROR_DEVICE_NOT_CONNECTED */
    }
    const uint32_t bad[2] = {4, buf};
    CHECK_EQ_U32(call("XInputGetState", 2, bad), 160);     /* ERROR_BAD_ARGUMENTS */
    const uint32_t caps[3] = {0, 1, buf};
    CHECK_EQ_U32(call("XInputGetCapabilities", 3, caps), 1167);
    const uint32_t vib[2] = {0, buf};
    CHECK_EQ_U32(call("XInputSetState", 2, vib), 1167);

    int16_t axis[6] = {9000, INT16_MIN, -9000, INT16_MAX, INT16_MAX, 0};
    uint32_t b = 1u << SDL_CONTROLLER_BUTTON_A | 1u << SDL_CONTROLLER_BUTTON_DPAD_UP |
                 1u << SDL_CONTROLLER_BUTTON_START | 1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    uint8_t g[12];
    re5_xinput_gamepad(axis, b, g);
    CHECK_EQ_U32(g[0] | g[1] << 8, 0x1211);    /* A | RIGHT_SHOULDER | START | DPAD_UP */
    CHECK_EQ_U32(g[2], 255);                   /* gatilho esquerdo no fim */
    CHECK_EQ_U32(g[3], 0);
    CHECK_EQ_U32(s16(g + 4), 9000);
    CHECK_EQ_U32((uint16_t)s16(g + 6), (uint16_t)32767);   /* Y do SDL para cima no máximo -> +32767 */
    CHECK_EQ_U32((uint16_t)s16(g + 8), (uint16_t)-9000);
    CHECK_EQ_U32((uint16_t)s16(g + 10), (uint16_t)-32767);

    /* Zona morta (7849 esquerdo, 8689 direito) e limiar dos gatilhos (30 de 255). */
    int16_t small[6] = {1000, -7848, -5, 8688, 3800, 0};    /* 3800 -> 29 no gatilho */
    re5_xinput_gamepad(small, 0, g);
    CHECK_EQ_U32(s16(g + 4), 0);
    CHECK_EQ_U32(s16(g + 6), 0);
    CHECK_EQ_U32(s16(g + 8), 0);
    CHECK_EQ_U32(s16(g + 10), 0);
    CHECK_EQ_U32(g[2], 0);
    int16_t edge[6] = {7849, 0, -8689, 0, 4000, 0};          /* no limite: passa; 4000 -> 31 */
    re5_xinput_gamepad(edge, 0, g);
    CHECK_EQ_U32(s16(g + 4), 7849);
    CHECK_EQ_U32((uint16_t)s16(g + 8), (uint16_t)-8689);
    CHECK_EQ_U32(g[2], 31);

    /* Controle de toque (re5/touch.h): liga o usuário 0 mesmo sem controle físico; os
     * outros continuam desligados. Y para cima positivo, gatilhos 0..1 -> 0..255. */
    CHECK_EQ_U32(re5_input_active(), RE5_INPUT_NONE);
    re5_touch_set_pad(RE5_TOUCH_A | RE5_TOUCH_LB, 0.5f, 1.0f, -1.0f, 0.f, 1.0f, 0.f);
    CHECK_EQ_U32(re5_input_active(), RE5_INPUT_TOUCH);
    const uint32_t a0[2] = {0, buf};
    CHECK_EQ_U32(call("XInputGetState", 2, a0), 0);
    uint32_t pkt = rd32(buf);
    const uint8_t *st = re5_g2h(buf + 4);
    CHECK_EQ_U32(st[0] | st[1] << 8, 0x1100);   /* A | LEFT_SHOULDER */
    CHECK_EQ_U32(st[2], 255);
    CHECK_EQ_U32(st[3], 0);
    CHECK_EQ_U32(s16(st + 4), 16384);           /* 0,5 * 32767 arredondado */
    CHECK_EQ_U32(s16(st + 6), 32767);
    CHECK_EQ_U32((uint16_t)s16(st + 8), (uint16_t)-32767);
    CHECK_EQ_U32(call("XInputGetState", 2, a0), 0);
    CHECK_EQ_U32(rd32(buf), pkt);               /* nada mudou: mesmo dwPacketNumber */
    re5_input_note(RE5_INPUT_PAD);
    re5_touch_set_pad(0, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);       /* soltar tudo não tira o controle físico */
    CHECK_EQ_U32(re5_input_active(), RE5_INPUT_PAD);
    CHECK_EQ_U32(call("XInputGetState", 2, a0), 0);
    CHECK(rd32(buf) != pkt);
    CHECK_EQ_U32(call("XInputGetCapabilities", 3, caps), 0);
    CHECK_EQ_U32(call("XInputSetState", 2, vib), 0);
    const uint32_t a1[2] = {1, buf};
    CHECK_EQ_U32(call("XInputGetState", 2, a1), 1167);

    /* Soma com o controle físico: botões em OU, gatilho maior, analógico mais inclinado. */
    uint8_t phys[12] = {0x01, 0x20, 10, 200, 0x10, 0x27, 0, 0, 0x00, 0x10, 0, 0};   /* LX 10000, RX 4096 */
    uint8_t tch[12] = {0x00, 0x10, 90, 50, 0x00, 0x08, 0x00, 0x08, 0x00, 0x80, 0, 0}; /* LX=LY 2048, RX -32768 */
    re5_touch_merge(phys, tch);
    CHECK_EQ_U32(phys[0] | phys[1] << 8, 0x3001);
    CHECK_EQ_U32(phys[2], 90);
    CHECK_EQ_U32(phys[3], 200);
    CHECK_EQ_U32(s16(phys + 4), 10000);         /* o físico estava mais inclinado */
    CHECK_EQ_U32(s16(phys + 6), 0);
    CHECK_EQ_U32((uint16_t)s16(phys + 8), (uint16_t)-32768);

    /* Troca toque <-> controle físico no usuário 0: o usuário 0 some por 500 ms para o
     * jogo ver o controle entrar (e avisar, como no PC). */
    CHECK_EQ_U32(re5_xinput_slot0_gap(0, 1, 1000), 0);        /* só o toque */
    re5_input_note(RE5_INPUT_TOUCH);
    CHECK_EQ_U32(re5_xinput_slot0_gap(1, 1, 2000), 1);        /* controle físico chegou */
    CHECK_EQ_U32(re5_input_active(), RE5_INPUT_PAD);
    CHECK_EQ_U32(re5_xinput_slot0_gap(1, 1, 2499), 1);
    CHECK_EQ_U32(re5_xinput_slot0_gap(1, 1, 2500), 0);
    CHECK_EQ_U32(re5_xinput_slot0_gap(0, 1, 9000), 1);        /* saiu: volta o toque, com a mesma lacuna */
    CHECK_EQ_U32(re5_input_active(), RE5_INPUT_TOUCH);
    CHECK_EQ_U32(re5_xinput_slot0_gap(0, 1, 9500), 0);
    CHECK_EQ_U32(re5_xinput_slot0_gap(1, 0, 9600), 0);        /* sem toque (PC): sem lacuna */
    CHECK_EQ_U32(re5_input_active(), RE5_INPUT_PAD);
    CHECK_EQ_U32(re5_xinput_slot0_gap(0, 0, 9700), 0);

    re5_touch_reset();
    CHECK_EQ_U32(call("XInputGetState", 2, a0), 1167);
    CHECK_DONE();
}
