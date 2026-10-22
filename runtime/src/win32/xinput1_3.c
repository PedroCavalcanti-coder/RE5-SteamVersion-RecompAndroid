/*
 * xinput1_3.c - XInput 1.3 sobre SDL_GameController (Fase 4, etapa 4.4).
 *
 * Os quatro "usuários" do XInput são os controles que o SDL reconhece como
 * gamepad, na ordem em que aparecem. Um índice vazio procura controle novo no
 * máximo uma vez por segundo (o jogo sonda os quatro a cada quadro). Sem
 * janela (RE5_HEADLESS) o SDL não é iniciado e não há controle: o mesmo que um
 * PC sem nenhum ligado.
 *
 * XInputGetState/SetState/GetCapabilities: stdcall, devolvem códigos do Win32
 * (ERROR_SUCCESS, ERROR_DEVICE_NOT_CONNECTED, ERROR_BAD_ARGUMENTS).
 */
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "re5/runtime.h"
#include "re5/xinput.h"

#define XI_ERROR_SUCCESS              0u
#define XI_ERROR_BAD_ARGUMENTS        160u
#define XI_ERROR_DEVICE_NOT_CONNECTED 1167u
#define XI_USERS 4

typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID      id;
    uint32_t            packet;
    uint8_t             last[12];
} pad;

static pad g_pads[XI_USERS];
static int g_ready;     /* 1 = SDL iniciado; -1 = sem SDL (sem janela ou falhou) */
static uint32_t g_last_scan;

static int sdl_ready(void)
{
    if (g_ready) return g_ready > 0;
    g_ready = -1;
    if (getenv("RE5_HEADLESS")) return 0;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* como o XInput: lê sem foco */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        RE5_WARN("xinput", "SDL_INIT_GAMECONTROLLER falhou (%s): nenhum controle", SDL_GetError());
        return 0;
    }
    g_ready = 1;
    return 1;
}

static int slot_of(SDL_JoystickID id)
{
    for (int i = 0; i < XI_USERS; i++)
        if (g_pads[i].gc && g_pads[i].id == id) return i;
    return -1;
}

static void scan(void)
{
    uint32_t now = SDL_GetTicks();
    if (g_last_scan && now - g_last_scan < 1000u) return;
    g_last_scan = now;
    for (int i = 0; i < XI_USERS; i++) {
        if (g_pads[i].gc && !SDL_GameControllerGetAttached(g_pads[i].gc)) {
            RE5_INFO("xinput", "controle %d desligado", i);
            SDL_GameControllerClose(g_pads[i].gc);
            memset(&g_pads[i], 0, sizeof g_pads[i]);
        }
    }
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j) || slot_of(SDL_JoystickGetDeviceInstanceID(j)) >= 0) continue;
        int i = 0;
        while (i < XI_USERS && g_pads[i].gc) i++;
        if (i == XI_USERS) break;
        SDL_GameController *gc = SDL_GameControllerOpen(j);
        if (!gc) continue;
        g_pads[i].gc = gc;
        g_pads[i].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        RE5_INFO("xinput", "controle %d: %s", i, SDL_GameControllerName(gc));
    }
}

/* Controle do usuário `i`, ou NULL. */
static SDL_GameController *pad_of(uint32_t i)
{
    if (!sdl_ready()) return NULL;
    SDL_GameControllerUpdate();
    if (!g_pads[i].gc || !SDL_GameControllerGetAttached(g_pads[i].gc)) scan();
    return g_pads[i].gc && SDL_GameControllerGetAttached(g_pads[i].gc) ? g_pads[i].gc : NULL;
}

static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int16_t flip(int16_t v) { return v == INT16_MIN ? INT16_MAX : (int16_t)-v; }

void re5_xinput_gamepad(const int16_t axis[6], uint32_t buttons, uint8_t out[12])
{
    /* XINPUT_GAMEPAD_*: o SDL usa a mesma disposição do controle do Xbox. */
    static const struct { int sdl; uint16_t xi; } k_buttons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_CONTROLLER_BUTTON_START, 0x0010}, {SDL_CONTROLLER_BUTTON_BACK, 0x0020},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200},
        {SDL_CONTROLLER_BUTTON_A, 0x1000}, {SDL_CONTROLLER_BUTTON_B, 0x2000},
        {SDL_CONTROLLER_BUTTON_X, 0x4000}, {SDL_CONTROLLER_BUTTON_Y, 0x8000},
    };
    uint16_t b = 0;
    for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
        if (buttons & (1u << k_buttons[i].sdl)) b |= k_buttons[i].xi;
    wr16le(out, b);
    for (int t = 0; t < 2; t++) {   /* gatilhos: 0..32767 -> 0..255 */
        int32_t v = axis[SDL_CONTROLLER_AXIS_TRIGGERLEFT + t];
        out[2 + t] = (uint8_t)(v <= 0 ? 0 : v * 255 / 32767);
    }
    wr16le(out + 4, (uint16_t)axis[SDL_CONTROLLER_AXIS_LEFTX]);
    wr16le(out + 6, (uint16_t)flip(axis[SDL_CONTROLLER_AXIS_LEFTY]));
    wr16le(out + 8, (uint16_t)axis[SDL_CONTROLLER_AXIS_RIGHTX]);
    wr16le(out + 10, (uint16_t)flip(axis[SDL_CONTROLLER_AXIS_RIGHTY]));
}

static void x_GetState(CPU *c)
{
    uint32_t user = re5_arg(c, 0), out = re5_arg(c, 1);
    if (user >= XI_USERS || !out) {
        c->eax = XI_ERROR_BAD_ARGUMENTS;
        return;
    }
    SDL_GameController *gc = pad_of(user);
    if (!gc) {
        c->eax = XI_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    int16_t axis[6];
    uint32_t buttons = 0;
    for (int a = 0; a < 6; a++) axis[a] = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)a);
    for (int k = 0; k < SDL_CONTROLLER_BUTTON_MAX && k < 32; k++)
        if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)k)) buttons |= 1u << k;
    pad *p = &g_pads[user];
    uint8_t g[12];
    re5_xinput_gamepad(axis, buttons, g);
    if (memcmp(g, p->last, sizeof g) != 0) {   /* dwPacketNumber muda quando o estado muda */
        memcpy(p->last, g, sizeof g);
        p->packet++;
    }
    wr32(out, p->packet);
    memcpy(re5_g2h(out + 4), g, sizeof g);
    c->eax = XI_ERROR_SUCCESS;
}

static void x_SetState(CPU *c)
{
    uint32_t user = re5_arg(c, 0), vib = re5_arg(c, 1);
    if (user >= XI_USERS || !vib) {
        c->eax = XI_ERROR_BAD_ARGUMENTS;
        return;
    }
    SDL_GameController *gc = pad_of(user);
    if (!gc) {
        c->eax = XI_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    /* XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed. O XInput mantém até
     * mudar; o SDL pede duração: o jogo renova a vibração enquanto ela dura. */
    SDL_GameControllerRumble(gc, rd16(vib), rd16(vib + 2), 5000);
    c->eax = XI_ERROR_SUCCESS;
}

static void x_GetCapabilities(CPU *c)
{
    uint32_t user = re5_arg(c, 0), out = re5_arg(c, 2);
    if (user >= XI_USERS || !out) {
        c->eax = XI_ERROR_BAD_ARGUMENTS;
        return;
    }
    if (!pad_of(user)) {
        c->eax = XI_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    /* XINPUT_CAPABILITIES: Type, SubType, Flags, Gamepad (o que existe), Vibration. */
    memset(re5_g2h(out), 0, 20);
    wr8(out, 1);          /* XINPUT_DEVTYPE_GAMEPAD */
    wr8(out + 1, 1);      /* XINPUT_DEVSUBTYPE_GAMEPAD */
    wr16(out + 4, 0xF3FF);
    wr8(out + 6, 0xFF);
    wr8(out + 7, 0xFF);
    for (uint32_t i = 0; i < 4; i++) wr16(out + 8 + 2 * i, 0xFFC0);
    wr16(out + 16, 0xFFFF);
    wr16(out + 18, 0xFFFF);
    c->eax = XI_ERROR_SUCCESS;
}

static const re5_import_def k_defs[] = {
    {"XInputGetCapabilities", x_GetCapabilities, 12, 0},
    {"XInputGetState",        x_GetState,         8, 0},
    {"XInputSetState",        x_SetState,         8, 0},
    {0},
};

const re5_import_module re5_mod_xinput1_3 = {"xinput1_3.dll", k_defs};
