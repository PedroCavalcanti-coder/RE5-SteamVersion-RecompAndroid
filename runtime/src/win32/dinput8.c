/*
 * dinput8.c - DirectInput 8 sobre o teclado e o mouse do SDL2 (Fase 4,
 * etapa 4.4; decisão D4).
 *
 * O RE5 cria três IDirectInput8A (versão 0x800): um só para enumerar
 * controles (DI8DEVCLASS_GAMECTRL - os de XInput ele lê pelo xinput1_3), e um
 * para cada dispositivo do sistema, GUID_SysKeyboard e GUID_SysMouse, ambos
 * DISCL_NONEXCLUSIVE | DISCL_BACKGROUND, com c_dfDIKeyboard e c_dfDIMouse2.
 *
 * Modelo:
 *   - objetos COM no guest como na ponte do D3D9: [vtable][handle][magic], a
 *     vtable de thunks stdcall (COM conta o this); índice sem handler aborta
 *     com "IDirectInputDevice8A::Nome" (D8);
 *   - o sistema tem um teclado e um mouse e nenhum controle DirectInput (o
 *     mesmo que um PC sem joystick que não seja XInput): EnumDevices de
 *     controles não chama o callback;
 *   - o estado vem do user32 (o único leitor da fila do SDL): teclas por DIK_*
 *     e o movimento relativo do mouse acumulado entre duas leituras.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/user32.h"
#include "kernel32_internal.h"

#define DI_OK               0x00000000u
#define DI_NOEFFECT         0x00000001u   /* S_FALSE */
#define DIERR_NOTACQUIRED   0x8007000Cu
#define DIERR_ACQUIRED      0x800700AAu
#define DIERR_INVALIDPARAM  0x80070057u
#define DIERR_NOINTERFACE   0x80004002u
#define DIERR_DEVICENOTREG  0x80040154u
#define E_POINTER_          0x80004003u

#define DI_MAGIC 0x38494E44u   /* 'DIN8' */

/* GUIDs como ficam na memória (bytes_le). */
static const uint8_t k_iid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t k_iid_di8a[16] = {0x30, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D,
                                       0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00};
static const uint8_t k_iid_did8a[16] = {0x80, 0x10, 0xD4, 0x54, 0x15, 0xDC, 0x33, 0x48,
                                        0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79};
static const uint8_t k_guid_mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                         0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t k_guid_keyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                            0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t k_guid_xaxis[16] = {0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11,
                                         0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};

static int guid_at(uint32_t va, const uint8_t g[16]) { return va && memcmp(re5_g2h(va), g, 16) == 0; }

static void guid_text(uint32_t va, char *out, size_t n)
{
    const uint8_t *g = re5_g2h(va);
    snprintf(out, n, "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g[3], g[2], g[1],
             g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* ---- objetos ---- */

typedef enum { DK_DINPUT, DK_KEYBOARD, DK_MOUSE } di_kind;
typedef enum { FMT_NONE, FMT_KEYBOARD, FMT_MOUSE, FMT_MOUSE2 } di_format;

typedef struct {
    di_kind   kind;
    uint32_t  guest, handle, refs;
    int       acquired;
    di_format format;
    uint32_t  coop_hwnd, coop_flags, buffer_size;
} di_obj;

static uint32_t g_vt[3];
static const char *const k_kind_name[3] = {"IDirectInput8A", "IDirectInputDevice8A", "IDirectInputDevice8A"};

static uint32_t heap(void) { return k32_process_heap(); }

static uint32_t vtable_of(di_kind k);

static uint32_t obj_new(di_kind k)
{
    di_obj *o = calloc(1, sizeof *o);
    if (!o) re5_fatal("dinput8", "sem memória");
    o->kind = k;
    o->refs = 1;
    o->guest = k32_heap_alloc(heap(), 0, 16);
    if (!o->guest) re5_fatal("dinput8", "sem memória do guest");
    o->handle = re5_handle_new(RE5_H_COM, o, NULL);
    wr32(o->guest, vtable_of(k));
    wr32(o->guest + 4, o->handle);
    wr32(o->guest + 8, DI_MAGIC);
    wr32(o->guest + 12, (uint32_t)k);
    return o->guest;
}

static di_obj *obj_of(uint32_t va)
{
    if (!va || !re5_mem_is_mapped(va, 16) || rd32(va + 8) != DI_MAGIC) return NULL;
    di_obj *o = re5_handle_get(rd32(va + 4), RE5_H_COM);
    return o && o->guest == va ? o : NULL;
}

static di_obj *self(CPU *c, int device)
{
    di_obj *o = obj_of(re5_arg(c, 0));
    if (!o || (device ? o->kind == DK_DINPUT : o->kind != DK_DINPUT)) {
        const re5_thunk *t = re5_thunk_current();
        re5_fatal("dinput8", "%s: this=0x%08X não é um objeto do DirectInput da ponte", t ? t->name : "?",
                  re5_arg(c, 0));
    }
    return o;
}

static di_obj *any(CPU *c)
{
    di_obj *o = obj_of(re5_arg(c, 0));
    if (!o) {
        const re5_thunk *t = re5_thunk_current();
        re5_fatal("dinput8", "%s: this=0x%08X não é um objeto do DirectInput da ponte", t ? t->name : "?",
                  re5_arg(c, 0));
    }
    return o;
}

static void unknown_method(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("dinput8", "%s sem implementação (this=0x%08X, retorno 0x%08X)", t ? t->name : "?", re5_arg(c, 0),
              rd32(c->esp));
}

/* ---- IUnknown ---- */

static void i_QueryInterface(CPU *c)
{
    di_obj *o = any(c);
    uint32_t riid = re5_arg(c, 1), ppv = re5_arg(c, 2);
    if (!ppv) {
        c->eax = E_POINTER_;
        return;
    }
    const uint8_t *own = o->kind == DK_DINPUT ? k_iid_di8a : k_iid_did8a;
    if (guid_at(riid, k_iid_unknown) || guid_at(riid, own)) {
        o->refs++;
        wr32(ppv, o->guest);
        c->eax = DI_OK;
        return;
    }
    char s[64];
    guid_text(riid, s, sizeof s);
    re5_fatal("dinput8", "%s::QueryInterface(%s) sem implementação", k_kind_name[o->kind], s);
}

static void i_AddRef(CPU *c)
{
    di_obj *o = any(c);
    c->eax = ++o->refs;
}

static void i_Release(CPU *c)
{
    di_obj *o = any(c);
    if (!o->refs) re5_fatal("dinput8", "%s::Release em 0x%08X sem referência", k_kind_name[o->kind], o->guest);
    c->eax = --o->refs;
    if (o->refs) return;
    re5_handle_close(o->handle);
    wr32(o->guest + 8, 0);
    k32_heap_free(heap(), o->guest);
    free(o);
}

/* ---- IDirectInput8A ---- */

#define DIDEVICEINSTANCEA_SIZE 580u
#define DI8DEVTYPE_MOUSE    0x12u
#define DI8DEVTYPE_KEYBOARD 0x13u
#define DIEDFL_FORCEFEEDBACK 0x100u

/* DIDEVICEINSTANCEA: dwSize, guidInstance, guidProduct, dwDevType,
 * tszInstanceName[260], tszProductName[260], guidFFDriver, wUsagePage, wUsage. */
static void fill_instance(uint32_t va, int keyboard)
{
    memset(re5_g2h(va), 0, DIDEVICEINSTANCEA_SIZE);
    const uint8_t *g = keyboard ? k_guid_keyboard : k_guid_mouse;
    const char *name = keyboard ? "Keyboard" : "Mouse";
    wr32(va, DIDEVICEINSTANCEA_SIZE);
    memcpy(re5_g2h(va + 4), g, 16);
    memcpy(re5_g2h(va + 20), g, 16);
    /* DI8DEVTYPEKEYBOARD_PCENH (4), DI8DEVTYPEMOUSE_UNKNOWN (1) */
    wr32(va + 36, keyboard ? DI8DEVTYPE_KEYBOARD | 4u << 8 : DI8DEVTYPE_MOUSE | 1u << 8);
    memcpy(re5_g2h(va + 40), name, strlen(name) + 1);
    memcpy(re5_g2h(va + 300), name, strlen(name) + 1);
    wr16(va + 576, 1);                    /* HID: Generic Desktop */
    wr16(va + 578, keyboard ? 6 : 2);     /* Keyboard / Mouse */
}

/* O tipo pedido em EnumDevices (classe DI8DEVCLASS_* ou DI8DEVTYPE_*) inclui o dispositivo? */
static int enum_matches(uint32_t type, int keyboard)
{
    switch (type) {
    case 0: return 1;                                     /* DI8DEVCLASS_ALL */
    case 2: case DI8DEVTYPE_MOUSE: return !keyboard;      /* DI8DEVCLASS_POINTER */
    case 3: case DI8DEVTYPE_KEYBOARD: return keyboard;    /* DI8DEVCLASS_KEYBOARD */
    default: return 0;   /* DI8DEVCLASS_DEVICE, GAMECTRL, joysticks...: o sistema não tem nenhum */
    }
}

static void d_EnumDevices(CPU *c)
{
    self(c, 0);
    uint32_t type = re5_arg(c, 1), cb = re5_arg(c, 2), ref = re5_arg(c, 3), flags = re5_arg(c, 4);
    RE5_INFO("dinput8", "EnumDevices(tipo 0x%X, flags 0x%X)", type, flags);
    c->eax = DI_OK;
    if (!cb) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    if (flags & DIEDFL_FORCEFEEDBACK) return;   /* teclado e mouse não têm force feedback */
    uint32_t inst = 0;
    for (int kb = 1; kb >= 0; kb--) {
        if (!enum_matches(type, kb)) continue;
        if (!inst) inst = k32_heap_alloc(heap(), 0, DIDEVICEINSTANCEA_SIZE);
        fill_instance(inst, kb);
        const uint32_t a[2] = {inst, ref};
        if (re5_guest_call(re5_thread_current(), cb, 2, a) == 0) break;   /* DIENUM_STOP */
    }
    if (inst) k32_heap_free(heap(), inst);
    c->eax = DI_OK;
}

static void d_CreateDevice(CPU *c)
{
    self(c, 0);
    uint32_t guid = re5_arg(c, 1), out = re5_arg(c, 2), outer = re5_arg(c, 3);
    if (!out || !guid) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    if (outer) re5_fatal("dinput8", "IDirectInput8A::CreateDevice com agregação (pUnkOuter) sem implementação");
    di_kind k;
    if (guid_at(guid, k_guid_keyboard)) k = DK_KEYBOARD;
    else if (guid_at(guid, k_guid_mouse)) k = DK_MOUSE;
    else {
        char s[64];
        guid_text(guid, s, sizeof s);
        RE5_WARN("dinput8", "CreateDevice(%s): dispositivo que o sistema não tem", s);
        wr32(out, 0);
        c->eax = DIERR_DEVICENOTREG;
        return;
    }
    wr32(out, obj_new(k));
    RE5_INFO("dinput8", "CreateDevice(%s) -> 0x%08X", k == DK_KEYBOARD ? "GUID_SysKeyboard" : "GUID_SysMouse",
             rd32(out));
    c->eax = DI_OK;
}

static void d_GetDeviceStatus(CPU *c)
{
    self(c, 0);
    uint32_t guid = re5_arg(c, 1);
    c->eax = guid_at(guid, k_guid_keyboard) || guid_at(guid, k_guid_mouse) ? DI_OK : DIERR_DEVICENOTREG;
}

/* ---- IDirectInputDevice8A ---- */

static void v_Acquire(CPU *c)
{
    di_obj *o = self(c, 1);
    if (o->format == FMT_NONE) {
        c->eax = DIERR_INVALIDPARAM;   /* sem SetDataFormat */
        return;
    }
    c->eax = o->acquired ? DI_NOEFFECT : DI_OK;
    if (!o->acquired && o->kind == DK_MOUSE) {   /* movimento de antes da aquisição não conta */
        int32_t dx, dy, dz;
        re5_user32_mouse_take(&dx, &dy, &dz);
    }
    o->acquired = 1;
    if (o->kind == DK_MOUSE && (o->coop_flags & 1u)) re5_user32_set_mouse_exclusive(1);
}

static void v_Unacquire(CPU *c)
{
    di_obj *o = self(c, 1);
    c->eax = o->acquired ? DI_OK : DI_NOEFFECT;
    if (o->acquired && o->kind == DK_MOUSE && (o->coop_flags & 1u)) re5_user32_set_mouse_exclusive(0);
    o->acquired = 0;
}

/* Só os formatos padrão da dinput8.lib: c_dfDIKeyboard, c_dfDIMouse, c_dfDIMouse2. */
static void v_SetDataFormat(CPU *c)
{
    di_obj *o = self(c, 1);
    uint32_t df = re5_arg(c, 1);
    if (!df) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    if (o->acquired) {
        c->eax = DIERR_ACQUIRED;
        return;
    }
    /* DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf */
    uint32_t size = rd32(df), objsize = rd32(df + 4), datasize = rd32(df + 12), nobjs = rd32(df + 16);
    uint32_t odf = rd32(df + 20);
    di_format f = FMT_NONE;
    if (size == 24 && objsize == 16) {
        if (o->kind == DK_KEYBOARD && datasize == 256 && nobjs == 256) f = FMT_KEYBOARD;
        if (o->kind == DK_MOUSE && odf && guid_at(rd32(odf), k_guid_xaxis) && rd32(odf + 4) == 0) {
            if (datasize == 16 && nobjs == 7) f = FMT_MOUSE;
            if (datasize == 20 && nobjs == 11) f = FMT_MOUSE2;
        }
    }
    if (f == FMT_NONE)
        re5_fatal("dinput8", "%s::SetDataFormat(dwDataSize %u, %u objetos) fora dos formatos padrão sem implementação",
                  o->kind == DK_KEYBOARD ? "teclado" : "mouse", datasize, nobjs);
    o->format = f;
    c->eax = DI_OK;
}

static void v_SetCooperativeLevel(CPU *c)
{
    di_obj *o = self(c, 1);
    o->coop_hwnd = re5_arg(c, 1);
    o->coop_flags = re5_arg(c, 2);
    /* DISCL_EXCLUSIVE no mouse: adquirido, o mouse do host vai para o modo
     * relativo (re5_user32_set_mouse_exclusive), como o cursor preso do Windows. */
    c->eax = DI_OK;
}

static void v_GetDeviceState(CPU *c)
{
    di_obj *o = self(c, 1);
    uint32_t cb = re5_arg(c, 1), data = re5_arg(c, 2);
    if (!o->acquired) {
        c->eax = DIERR_NOTACQUIRED;
        return;
    }
    uint32_t want = o->format == FMT_KEYBOARD ? 256u : o->format == FMT_MOUSE ? 16u : 20u;
    if (!data || cb != want) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    re5_user32_pump();
    if (o->kind == DK_KEYBOARD) {
        re5_user32_dik_state(re5_g2h(data));
    } else {
        /* DIMOUSESTATE(2): LONG lX, lY, lZ; BYTE rgbButtons[4 ou 8] (eixos relativos) */
        int32_t dx, dy, dz;
        re5_user32_mouse_take(&dx, &dy, &dz);
        uint32_t b = re5_user32_mouse_buttons();
        wr32(data, (uint32_t)dx);
        wr32(data + 4, (uint32_t)dy);
        wr32(data + 8, (uint32_t)dz);
        for (uint32_t i = 0; i < want - 12; i++) wr8(data + 12 + i, (b >> i) & 1u ? 0x80 : 0);
    }
    c->eax = DI_OK;
}

/* DIPROP_* são GUIDs "falsos": o REFGUID é um inteiro pequeno. */
#define DIPROP_BUFFERSIZE 1u
#define DIPROP_AXISMODE   2u

static void v_SetProperty(CPU *c)
{
    di_obj *o = self(c, 1);
    uint32_t prop = re5_arg(c, 1), ph = re5_arg(c, 2);
    if (!ph) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    uint32_t value = rd32(ph + 16);   /* DIPROPDWORD: DIPROPHEADER (16 bytes) + dwData */
    if (prop == DIPROP_BUFFERSIZE) {
        o->buffer_size = value;
        RE5_INFO("dinput8", "SetProperty(DIPROP_BUFFERSIZE, %u)", value);
    } else if (prop == DIPROP_AXISMODE && value == 1u) {   /* DIPROPAXISMODE_REL: o que já fazemos */
    } else {
        re5_fatal("dinput8", "IDirectInputDevice8A::SetProperty(0x%X, %u) sem implementação", prop, value);
    }
    c->eax = DI_OK;
}

static void v_Poll(CPU *c)
{
    self(c, 1);
    c->eax = DI_NOEFFECT;   /* teclado e mouse não são sondados */
}

static void v_GetDeviceInfo(CPU *c)
{
    di_obj *o = self(c, 1);
    uint32_t out = re5_arg(c, 1);
    if (!out || rd32(out) != DIDEVICEINSTANCEA_SIZE) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    fill_instance(out, o->kind == DK_KEYBOARD);
    c->eax = DI_OK;
}

/* ---- vtables ---- */

typedef struct { const char *name; uint32_t arg_bytes; re5_host_fn fn; } di_method;

static const di_method k_dinput[] = {
    {"QueryInterface", 12, i_QueryInterface}, {"AddRef", 4, i_AddRef}, {"Release", 4, i_Release},
    {"CreateDevice", 16, d_CreateDevice}, {"EnumDevices", 20, d_EnumDevices},
    {"GetDeviceStatus", 8, d_GetDeviceStatus}, {"RunControlPanel", 12, NULL}, {"Initialize", 12, NULL},
    {"FindDevice", 16, NULL}, {"EnumDevicesBySemantics", 24, NULL}, {"ConfigureDevices", 20, NULL},
};

static const di_method k_device[] = {
    {"QueryInterface", 12, i_QueryInterface}, {"AddRef", 4, i_AddRef}, {"Release", 4, i_Release},
    {"GetCapabilities", 8, NULL}, {"EnumObjects", 16, NULL}, {"GetProperty", 12, NULL},
    {"SetProperty", 12, v_SetProperty}, {"Acquire", 4, v_Acquire}, {"Unacquire", 4, v_Unacquire},
    {"GetDeviceState", 12, v_GetDeviceState}, {"GetDeviceData", 20, NULL}, {"SetDataFormat", 8, v_SetDataFormat},
    {"SetEventNotification", 8, NULL}, {"SetCooperativeLevel", 12, v_SetCooperativeLevel},
    {"GetObjectInfo", 16, NULL}, {"GetDeviceInfo", 8, v_GetDeviceInfo}, {"RunControlPanel", 12, NULL},
    {"Initialize", 16, NULL}, {"CreateEffect", 20, NULL}, {"EnumEffects", 16, NULL}, {"GetEffectInfo", 12, NULL},
    {"GetForceFeedbackState", 8, NULL}, {"SendForceFeedbackCommand", 8, NULL},
    {"EnumCreatedEffectObjects", 16, NULL}, {"Escape", 8, NULL}, {"Poll", 4, v_Poll},
    {"SendDeviceData", 20, NULL}, {"EnumEffectsInFile", 20, NULL}, {"WriteEffectToFile", 20, NULL},
    {"BuildActionMap", 16, NULL}, {"SetActionMap", 16, NULL}, {"GetImageInfo", 8, NULL},
};

static uint32_t vtable_of(di_kind k)
{
    uint32_t slot = k == DK_DINPUT ? 0u : 1u;
    if (g_vt[slot]) return g_vt[slot];
    const di_method *m = slot ? k_device : k_dinput;
    uint32_t n = slot ? (uint32_t)(sizeof k_device / sizeof k_device[0]) : (uint32_t)(sizeof k_dinput / sizeof k_dinput[0]);
    uint32_t vt = k32_heap_alloc(heap(), 0, 4u * n);
    if (!vt) re5_fatal("dinput8", "sem memória do guest");
    for (uint32_t i = 0; i < n; i++) {
        char *name = malloc(64);
        if (!name) re5_fatal("dinput8", "sem memória");
        snprintf(name, 64, "%s::%s", slot ? "IDirectInputDevice8A" : "IDirectInput8A", m[i].name);
        wr32(vt + 4u * i, re5_thunk_new(name, m[i].fn ? m[i].fn : unknown_method, m[i].arg_bytes, NULL));
    }
    return g_vt[slot] = vt;
}

/* ---- DirectInput8Create ---- */

static void d_DirectInput8Create(CPU *c)
{
    uint32_t version = re5_arg(c, 1), riid = re5_arg(c, 2), out = re5_arg(c, 3), outer = re5_arg(c, 4);
    if (!out) {
        c->eax = DIERR_INVALIDPARAM;
        return;
    }
    if (outer) re5_fatal("dinput8", "DirectInput8Create com agregação (punkOuter) sem implementação");
    if (!guid_at(riid, k_iid_di8a)) {
        char s[64];
        guid_text(riid, s, sizeof s);
        re5_fatal("dinput8", "DirectInput8Create(%s) sem implementação: só IDirectInput8A", s);
    }
    wr32(out, obj_new(DK_DINPUT));
    RE5_INFO("dinput8", "DirectInput8Create(versão 0x%X) -> 0x%08X", version, rd32(out));
    c->eax = DI_OK;
}

static const re5_import_def k_defs[] = {
    {"DirectInput8Create", d_DirectInput8Create, 20, 0},
    {0},
};

const re5_import_module re5_mod_dinput8 = {"dinput8.dll", k_defs};
