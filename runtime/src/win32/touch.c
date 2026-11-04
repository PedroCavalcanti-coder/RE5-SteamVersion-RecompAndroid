/*
 * touch.c - dispositivo de toque próprio (ver re5/touch.h).
 *
 * Quem escreve é a thread de UI do app (JNI); quem lê é o jogo (XInputGetState) e
 * a thread que bombeia a fila do user32. O estado é pequeno: um spinlock do SDL
 * basta e não depende de SDL_Init (os testes rodam sem janela).
 */
#include <math.h>
#include <string.h>

#include <SDL2/SDL_atomic.h>

#include "re5/log.h"
#include "re5/touch.h"

static SDL_SpinLock g_lock;
static SDL_atomic_t g_active;           /* re5_input_source */

static int      g_pad_on;
static uint32_t g_buttons;
static float    g_axis[4], g_trig[2];   /* lx, ly, rx, ry; lt, rt */

#define PTR_MAX 64
static re5_touch_ptr_event g_ptr[PTR_MAX];
static int g_ptr_head, g_ptr_count;

static const char *const k_source_name[] = {"nenhum", "teclado/mouse", "controle", "toque"};

void re5_input_note(re5_input_source s)
{
    int old = SDL_AtomicSet(&g_active, (int)s);
    if (old != (int)s) RE5_INFO("input", "dispositivo ativo: %s", k_source_name[s]);
}

re5_input_source re5_input_active(void) { return (re5_input_source)SDL_AtomicGet(&g_active); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void re5_touch_set_pad(uint32_t buttons, float lx, float ly, float rx, float ry, float lt, float rt)
{
    SDL_AtomicLock(&g_lock);
    int first = !g_pad_on;
    g_pad_on = 1;
    g_buttons = buttons & 0xF3FFu;
    g_axis[0] = clampf(lx, -1.f, 1.f);
    g_axis[1] = clampf(ly, -1.f, 1.f);
    g_axis[2] = clampf(rx, -1.f, 1.f);
    g_axis[3] = clampf(ry, -1.f, 1.f);
    g_trig[0] = clampf(lt, 0.f, 1.f);
    g_trig[1] = clampf(rt, 0.f, 1.f);
    SDL_AtomicUnlock(&g_lock);
    if (first) RE5_INFO("input", "controle de toque ligado (usuário 0 do XInput, somado ao controle físico)");
    /* Soltar tudo (o app soltando os botões porque um controle físico entrou) não
     * conta como uso do toque. */
    if (buttons || lx || ly || rx || ry || lt || rt) re5_input_note(RE5_INPUT_TOUCH);
}

static void wr16le(uint8_t *p, int v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int rd16le(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int re5_touch_pad(uint8_t out[12])
{
    /* Sem controle de toque ligado (todo PC): nem o spinlock. */
    if (!__atomic_load_n(&g_pad_on, __ATOMIC_ACQUIRE)) return 0;
    SDL_AtomicLock(&g_lock);
    int on = g_pad_on;
    uint32_t b = g_buttons;
    float ax[4], tr[2];
    memcpy(ax, g_axis, sizeof ax);
    memcpy(tr, g_trig, sizeof tr);
    SDL_AtomicUnlock(&g_lock);
    if (!on) return 0;
    wr16le(out, (int)b);
    out[2] = (uint8_t)lrintf(tr[0] * 255.f);
    out[3] = (uint8_t)lrintf(tr[1] * 255.f);
    for (int i = 0; i < 4; i++) wr16le(out + 4 + 2 * i, (int)lrintf(ax[i] * 32767.f));
    return 1;
}

void re5_touch_merge(uint8_t dst[12], const uint8_t src[12])
{
    dst[0] |= src[0];
    dst[1] |= src[1];
    for (int t = 2; t < 4; t++)
        if (src[t] > dst[t]) dst[t] = src[t];
    for (int s = 0; s < 2; s++) {   /* cada analógico inteiro (X e Y juntos), pelo de maior deflexão */
        uint8_t *d = dst + 4 + 4 * s;
        const uint8_t *o = src + 4 + 4 * s;
        long dx = rd16le(d), dy = rd16le(d + 2), ox = rd16le(o), oy = rd16le(o + 2);
        if (ox * ox + oy * oy > dx * dx + dy * dy) memcpy(d, o, 4);
    }
}

void re5_touch_pointer(int action, float nx, float ny)
{
    re5_touch_ptr_event ev = {action, clampf(nx, 0.f, 1.f), clampf(ny, 0.f, 1.f)};
    SDL_AtomicLock(&g_lock);
    int last = (g_ptr_head + g_ptr_count - 1) % PTR_MAX;
    if (action == RE5_TOUCH_PTR_MOVE && g_ptr_count && g_ptr[last].action == RE5_TOUCH_PTR_MOVE) {
        g_ptr[last] = ev;   /* movimentos seguidos: só o último interessa */
    } else if (g_ptr_count < PTR_MAX) {
        g_ptr[(g_ptr_head + g_ptr_count) % PTR_MAX] = ev;
        g_ptr_count++;
    }
    SDL_AtomicUnlock(&g_lock);
    re5_input_note(RE5_INPUT_TOUCH);
}

int re5_touch_take_pointer(re5_touch_ptr_event *ev)
{
    SDL_AtomicLock(&g_lock);
    int got = g_ptr_count > 0;
    if (got) {
        *ev = g_ptr[g_ptr_head];
        g_ptr_head = (g_ptr_head + 1) % PTR_MAX;
        g_ptr_count--;
    }
    SDL_AtomicUnlock(&g_lock);
    return got;
}

void re5_touch_reset(void)
{
    SDL_AtomicLock(&g_lock);
    g_pad_on = 0;
    g_buttons = 0;
    memset(g_axis, 0, sizeof g_axis);
    memset(g_trig, 0, sizeof g_trig);
    g_ptr_head = g_ptr_count = 0;
    SDL_AtomicUnlock(&g_lock);
    SDL_AtomicSet(&g_active, RE5_INPUT_NONE);
}

#if defined(__ANDROID__)
#include <jni.h>

/* TouchControlsView -> runtime. Declarados em org.libsdl.app.SDLActivity, como os
 * outros nativos do app (nativeIsCursorHidden, nativeGetPresents). */
JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeTouchPad(JNIEnv *env, jclass cls, jint buttons,
                                                                     jfloat lx, jfloat ly, jfloat rx, jfloat ry,
                                                                     jfloat lt, jfloat rt)
{
    (void)env; (void)cls;
    re5_touch_set_pad((uint32_t)buttons, lx, ly, rx, ry, lt, rt);
}

JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeTouchPointer(JNIEnv *env, jclass cls, jint action,
                                                                         jfloat nx, jfloat ny)
{
    (void)env; (void)cls;
    re5_touch_pointer(action, nx, ny);
}

/* Qualquer dedo na tela, mesmo sem apertar nada: o toque volta a ser o dispositivo ativo. */
JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeTouchNote(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    re5_input_note(RE5_INPUT_TOUCH);
}

JNIEXPORT jint JNICALL Java_org_libsdl_app_SDLActivity_nativeActiveInput(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)re5_input_active();
}
#endif
