/*
 * DirectInput8 sobre SDL2, sem janela (RE5_HEADLESS): os eventos do SDL entram
 * por re5_user32_handle_event. Cobre DirectInput8Create, a vtable (nomes e
 * bytes stdcall), EnumDevices (controles: nenhum; tudo: teclado e mouse),
 * CreateDevice, SetDataFormat com c_dfDIKeyboard/c_dfDIMouse2, Acquire e
 * GetDeviceState (DIK_* e movimento relativo do mouse).
 *
 * No Windows os GUIDs e os formatos vêm do dinput.h/libdxguid/libdinput8 do
 * MinGW, não de constantes do teste: se a ponte tiver um byte errado, falha.
 */
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/user32.h"

#if defined(_WIN32)
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#endif

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;
static uint32_t g_scratch, g_off;

static uint32_t gbuf(uint32_t n)
{
    uint32_t va = g_scratch + g_off;
    memset(re5_g2h(va), 0, n);
    g_off = (g_off + n + 15u) & ~15u;
    return va;
}

static uint32_t gcopy(const void *p, uint32_t n)
{
    uint32_t va = gbuf(n);
    memcpy(re5_g2h(va), p, n);
    return va;
}

static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *args)
{
    uint32_t a[16];
    a[0] = obj;
    for (int i = 0; i < n; i++) a[i + 1] = args[i];
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, rd32(rd32(obj) + 4 * slot), n + 1, a);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static const re5_thunk *method(uint32_t obj, uint32_t slot) { return re5_thunk_get(rd32(rd32(obj) + 4 * slot)); }

#if defined(_WIN32)
static const GUID *g_iid_di8a = &IID_IDirectInput8A, *g_iid_did8a = &IID_IDirectInputDevice8A,
                  *g_kb = &GUID_SysKeyboard, *g_mouse = &GUID_SysMouse;

/* Formato da dinput8.lib copiado para o guest (ponteiros de GUID viram VAs). */
static uint32_t guest_format(const DIDATAFORMAT *df)
{
    uint32_t odf = gbuf(df->dwNumObjs * 16u);
    for (DWORD i = 0; i < df->dwNumObjs; i++) {
        const DIOBJECTDATAFORMAT *o = &df->rgodf[i];
        wr32(odf + 16 * i, o->pguid ? gcopy(o->pguid, 16) : 0);
        wr32(odf + 16 * i + 4, o->dwOfs);
        wr32(odf + 16 * i + 8, o->dwType);
        wr32(odf + 16 * i + 12, o->dwFlags);
    }
    uint32_t va = gbuf(24);
    wr32(va, 24);
    wr32(va + 4, 16);
    wr32(va + 8, df->dwFlags);
    wr32(va + 12, df->dwDataSize);
    wr32(va + 16, df->dwNumObjs);
    wr32(va + 20, odf);
    return va;
}
#else
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;
static const GUID k_di8a = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID k_did8a = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
static const GUID k_kb = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID k_mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID k_xaxis = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID *g_iid_di8a = &k_di8a, *g_iid_did8a = &k_did8a, *g_kb = &k_kb, *g_mouse = &k_mouse;

/* c_dfDIKeyboard (256 objetos) e c_dfDIMouse2 (X, Y, Z + 8 botões) como a dinput8.lib os monta. */
static uint32_t guest_std_format(int keyboard)
{
    uint32_t n = keyboard ? 256u : 11u, odf = gbuf(n * 16u), xa = gcopy(&k_xaxis, 16);
    for (uint32_t i = 0; i < n; i++) {
        wr32(odf + 16 * i, !keyboard && i == 0 ? xa : 0);
        wr32(odf + 16 * i + 4, keyboard ? i : i < 3 ? 4 * i : 12 + (i - 3));
    }
    uint32_t va = gbuf(24);
    wr32(va, 24);
    wr32(va + 4, 16);
    wr32(va + 8, keyboard ? 2 : 2);
    wr32(va + 12, keyboard ? 256 : 20);
    wr32(va + 16, n);
    wr32(va + 20, odf);
    return va;
}
#endif

static uint32_t g_cb_calls, g_cb_stop, g_cb_devtype[4];

static void enum_cb(CPU *c)
{
    uint32_t inst = re5_arg(c, 0);
    CHECK_EQ_U32(rd32(inst), 580);   /* sizeof(DIDEVICEINSTANCEA) no x86 */
    CHECK_EQ_U32(re5_arg(c, 1), 0x1234);
    if (g_cb_calls < 4) g_cb_devtype[g_cb_calls] = rd32(inst + 36);
    g_cb_calls++;
    c->eax = g_cb_stop ? 0 : 1;   /* DIENUM_STOP / DIENUM_CONTINUE */
}

static void key(SDL_Scancode sc, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.scancode = sc;
    re5_user32_handle_event(&e);
}

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
    g_scratch = re5_mem_alloc(0x40000u, RE5_PAGE_READWRITE, "teste");

#if defined(_WIN32)
    /* As constantes de erro que a ponte usa, conferidas com o dinput.h. */
    CHECK_EQ_U32((uint32_t)DIERR_NOTACQUIRED, 0x8007000Cu);
    CHECK_EQ_U32((uint32_t)DIERR_ACQUIRED, 0x800700AAu);
    CHECK_EQ_U32((uint32_t)DIERR_DEVICENOTREG, 0x80040154u);
    CHECK_EQ_U32(sizeof(DIDEVICEINSTANCEA), 580);
#endif

    uint32_t create = re5_import_bind("dinput8.dll", "DirectInput8Create", 0);
    CHECK_EQ_U32(re5_thunk_get(create)->arg_bytes, 20);
    uint32_t iid = gcopy(g_iid_di8a, 16), pdi = gbuf(4);
    const uint32_t ca[5] = {0x00400000u, 0x800, iid, pdi, 0};
    CHECK_EQ_U32(re5_guest_call(T, create, 5, ca), 0);
    uint32_t di = rd32(pdi);
    CHECK(di != 0);
    CHECK(strcmp(method(di, 4)->name, "IDirectInput8A::EnumDevices") == 0);
    CHECK_EQ_U32(method(di, 4)->arg_bytes, 20);
    CHECK(strcmp(method(di, 10)->name, "IDirectInput8A::ConfigureDevices") == 0);

    /* EnumDevices: o RE5 pede controles (DI8DEVCLASS_GAMECTRL) - não há nenhum. */
    uint32_t cb = re5_thunk_new("teste!EnumDevicesCallback", enum_cb, 8, NULL);
    const uint32_t eg[4] = {4 /* GAMECTRL */, cb, 0x1234, 0x101};
    CHECK_EQ_U32(vcall(di, 4, 4, eg), 0);
    CHECK_EQ_U32(g_cb_calls, 0);
    const uint32_t ea[4] = {0 /* ALL */, cb, 0x1234, 1};
    CHECK_EQ_U32(vcall(di, 4, 4, ea), 0);
    CHECK_EQ_U32(g_cb_calls, 2);
    CHECK_EQ_U32(g_cb_devtype[0] & 0xFF, 0x13);   /* DI8DEVTYPE_KEYBOARD */
    CHECK_EQ_U32(g_cb_devtype[1] & 0xFF, 0x12);   /* DI8DEVTYPE_MOUSE */
    g_cb_stop = 1;
    CHECK_EQ_U32(vcall(di, 4, 4, ea), 0);
    CHECK_EQ_U32(g_cb_calls, 3);                  /* DIENUM_STOP para no primeiro */

    /* Teclado */
    uint32_t gkb = gcopy(g_kb, 16), pkb = gbuf(4);
    const uint32_t cd[3] = {gkb, pkb, 0};
    CHECK_EQ_U32(vcall(di, 3, 3, cd), 0);
    uint32_t kb = rd32(pkb);
    CHECK(kb != 0);
    CHECK(strcmp(method(kb, 9)->name, "IDirectInputDevice8A::GetDeviceState") == 0);
    CHECK_EQ_U32(method(kb, 9)->arg_bytes, 12);
    CHECK(strcmp(method(kb, 31)->name, "IDirectInputDevice8A::GetImageInfo") == 0);
    uint32_t qiid = gcopy(g_iid_did8a, 16), pq = gbuf(4);
    const uint32_t qi[2] = {qiid, pq};
    CHECK_EQ_U32(vcall(kb, 0, 2, qi), 0);
    CHECK_EQ_U32(rd32(pq), kb);
    CHECK_EQ_U32(vcall(kb, 2, 0, NULL), 1);       /* Release do QI */

    const uint32_t coop[2] = {0x00010010u, 0xA};   /* DISCL_NONEXCLUSIVE | DISCL_BACKGROUND */
    CHECK_EQ_U32(vcall(kb, 13, 2, coop), 0);
    uint32_t state = gbuf(256);
    const uint32_t gs[2] = {256, state};
    CHECK_EQ_U32(vcall(kb, 9, 2, gs), 0x8007000Cu);   /* DIERR_NOTACQUIRED */
#if defined(_WIN32)
    uint32_t kfmt = guest_format(&c_dfDIKeyboard), mfmt = guest_format(&c_dfDIMouse2);
#else
    uint32_t kfmt = guest_std_format(1), mfmt = guest_std_format(0);
#endif
    CHECK_EQ_U32(vcall(kb, 11, 1, &kfmt), 0);         /* SetDataFormat */
    CHECK_EQ_U32(vcall(kb, 7, 0, NULL), 0);           /* Acquire */
    CHECK_EQ_U32(vcall(kb, 7, 0, NULL), 1);           /* de novo: DI_NOEFFECT */
    CHECK_EQ_U32(vcall(kb, 11, 1, &kfmt), 0x800700AAu);   /* adquirido: DIERR_ACQUIRED */
    key(SDL_SCANCODE_LEFT, 1);
    key(SDL_SCANCODE_A, 1);
    key(SDL_SCANCODE_PAUSE, 1);
    key(SDL_SCANCODE_NUMLOCKCLEAR, 1);
    CHECK_EQ_U32(vcall(kb, 9, 2, gs), 0);
    CHECK_EQ_U32(rd8(state + 0xCB), 0x80);   /* DIK_LEFT */
    CHECK_EQ_U32(rd8(state + 0x1E), 0x80);   /* DIK_A */
    CHECK_EQ_U32(rd8(state + 0xC5), 0x80);   /* DIK_PAUSE */
    CHECK_EQ_U32(rd8(state + 0x45), 0x80);   /* DIK_NUMLOCK */
    CHECK_EQ_U32(rd8(state + 0x4B), 0);      /* DIK_NUMPAD4 não (seta = E0 4B) */
    key(SDL_SCANCODE_A, 0);
    CHECK_EQ_U32(vcall(kb, 9, 2, gs), 0);
    CHECK_EQ_U32(rd8(state + 0x1E), 0);
    const uint32_t bad[2] = {16, state};
    CHECK_EQ_U32(vcall(kb, 9, 2, bad), 0x80070057u);   /* tamanho errado */

    /* Mouse */
    uint32_t gm = gcopy(g_mouse, 16), pm = gbuf(4);
    const uint32_t cm[3] = {gm, pm, 0};
    CHECK_EQ_U32(vcall(di, 3, 3, cm), 0);
    uint32_t mouse = rd32(pm);
    CHECK_EQ_U32(vcall(mouse, 13, 2, coop), 0);
    CHECK_EQ_U32(vcall(mouse, 11, 1, &mfmt), 0);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION;
    e.motion.xrel = 7;
    re5_user32_handle_event(&e);          /* antes do Acquire: não conta */
    CHECK_EQ_U32(vcall(mouse, 7, 0, NULL), 0);
    e.motion.xrel = 5;
    e.motion.yrel = -3;
    re5_user32_handle_event(&e);
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEWHEEL;
    e.wheel.y = 1;
    re5_user32_handle_event(&e);
    uint32_t ms = gbuf(20);
    const uint32_t gms[2] = {20, ms};
    CHECK_EQ_U32(vcall(mouse, 9, 2, gms), 0);
    CHECK_EQ_U32(rd32(ms), 5);
    CHECK_EQ_U32(rd32(ms + 4), (uint32_t)-3);
    CHECK_EQ_U32(rd32(ms + 8), 120);
    CHECK_EQ_U32(vcall(mouse, 9, 2, gms), 0);   /* relativo: zerou */
    CHECK_EQ_U32(rd32(ms), 0);
    CHECK_EQ_U32(rd32(ms + 8), 0);
    CHECK_EQ_U32(vcall(mouse, 25, 0, NULL), 1);  /* Poll: DI_NOEFFECT */

    /* Dispositivo que o sistema não tem (um joystick). */
    const uint8_t joy[16] = {1, 2, 3, 4};
    uint32_t gj = gcopy(joy, 16), pj = gbuf(4);
    const uint32_t cj[3] = {gj, pj, 0};
    CHECK_EQ_U32(vcall(di, 3, 3, cj), 0x80040154u);   /* DIERR_DEVICENOTREG */
    CHECK_EQ_U32(rd32(pj), 0);

    CHECK_EQ_U32(vcall(mouse, 2, 0, NULL), 0);
    CHECK_EQ_U32(vcall(kb, 2, 0, NULL), 0);
    CHECK_EQ_U32(vcall(di, 2, 0, NULL), 0);
    CHECK_DONE();
}
