/*
 * user32.c - janelas e entrada do Win32 sobre SDL2 (Fase 4, etapa 4.1).
 *
 * Modelo:
 *   - cada HWND do guest é uma janela SDL2 cujo tamanho é a ÁREA CLIENTE. O
 *     retângulo externo (o que CreateWindowEx/SetWindowPos/GetWindowRect
 *     falam) é a área cliente mais a moldura calculada do estilo, com as mesmas
 *     métricas de AdjustWindowRect - assim o jogo, que pede 1280x720 de cliente
 *     e cria a janela com 1286x748, recebe 1280x720 em GetClientRect e cria o
 *     back buffer certo;
 *   - mensagens "enviadas" (WM_CREATE, WM_SIZE, WM_ACTIVATE, WM_CLOSE...) chamam
 *     o WndProc do guest na hora, por re5_guest_call; as "postadas" (teclado,
 *     mouse, WM_CHAR) vão para uma fila lida por PeekMessage/GetMessage;
 *   - teclado: scancode do SDL -> VK + scancode set 1 no lParam, como o Windows;
 *     WM_CHAR vem do SDL_TEXTINPUT (texto já com layout e acentos);
 *   - fechar a janela segue o caminho do Windows: WM_SYSCOMMAND(SC_CLOSE) ->
 *     DefWindowProc -> WM_CLOSE -> DefWindowProc -> DestroyWindow -> WM_DESTROY,
 *     e quem posta WM_QUIT é o jogo (PostQuitMessage). O SDL_QUIT é ignorado.
 *
 * Esperas (GetMessage) soltam o lock do guest; PeekMessage com fila vazia cede
 * o lock por um instante, para as threads de trabalho da MT Framework andarem
 * enquanto o laço principal só consulta mensagens (decisão D5).
 *
 * Fora do que o jogo usa, índices e ações desconhecidos abortam (D8).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "re5/coldebug.h"
#include "re5/runtime.h"
#include "re5/user32.h"
#include "re5_cursors.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#endif

/* ---- constantes do Win32 (prefixadas: o windows.h do host define as mesmas) ---- */

#define U32_WM_CREATE           0x0001u
#define U32_WM_DESTROY          0x0002u
#define U32_WM_MOVE             0x0003u
#define U32_WM_SIZE             0x0005u
#define U32_WM_ACTIVATE         0x0006u
#define U32_WM_SETFOCUS         0x0007u
#define U32_WM_KILLFOCUS        0x0008u
#define U32_WM_GETTEXT          0x000Du
#define U32_WM_GETTEXTLENGTH    0x000Eu
#define U32_WM_PAINT            0x000Fu
#define U32_WM_CLOSE            0x0010u
#define U32_WM_QUIT             0x0012u
#define U32_WM_ERASEBKGND       0x0014u
#define U32_WM_SHOWWINDOW       0x0018u
#define U32_WM_ACTIVATEAPP      0x001Cu
#define U32_WM_SETCURSOR        0x0020u
#define U32_WM_MOUSEACTIVATE    0x0021u
#define U32_WM_WINDOWPOSCHANGED 0x0047u
#define U32_WM_NCCREATE         0x0081u
#define U32_WM_NCDESTROY        0x0082u
#define U32_WM_NCHITTEST        0x0084u
#define U32_WM_NCACTIVATE       0x0086u
#define U32_WM_KEYDOWN          0x0100u
#define U32_WM_KEYUP            0x0101u
#define U32_WM_CHAR             0x0102u
#define U32_WM_SYSKEYDOWN       0x0104u
#define U32_WM_SYSKEYUP         0x0105u
#define U32_WM_SYSCOMMAND       0x0112u
#define U32_WM_MOUSEMOVE        0x0200u
#define U32_WM_LBUTTONDOWN      0x0201u
#define U32_WM_LBUTTONUP        0x0202u
#define U32_WM_RBUTTONDOWN      0x0204u
#define U32_WM_RBUTTONUP        0x0205u
#define U32_WM_MBUTTONDOWN      0x0207u
#define U32_WM_MBUTTONUP        0x0208u
#define U32_WM_MOUSEWHEEL       0x020Au

#define U32_SC_CLOSE      0xF060u
#define U32_HTCLIENT      1u
#define U32_MA_ACTIVATE   1u
#define U32_WA_INACTIVE   0u
#define U32_WA_ACTIVE     1u
#define U32_SIZE_RESTORED 0u
#define U32_MK_LBUTTON    0x01u
#define U32_MK_RBUTTON    0x02u
#define U32_MK_SHIFT      0x04u
#define U32_MK_CONTROL    0x08u
#define U32_MK_MBUTTON    0x10u
#define U32_PM_REMOVE     0x0001u
#define U32_CW_USEDEFAULT 0x80000000u

#define U32_WS_VISIBLE    0x10000000u
#define U32_WS_BORDER     0x00800000u
#define U32_WS_DLGFRAME   0x00400000u
#define U32_WS_CAPTION    0x00C00000u
#define U32_WS_THICKFRAME 0x00040000u

#define U32_SWP_NOSIZE    0x0001u
#define U32_SWP_NOMOVE    0x0002u
#define U32_SWP_HIDEWINDOW 0x0080u
#define U32_SWP_SHOWWINDOW 0x0040u

#define U32_GWL_WNDPROC   (-4)
#define U32_GWL_HINSTANCE (-6)
#define U32_GWL_STYLE     (-16)
#define U32_GWL_EXSTYLE   (-20)
#define U32_GWL_USERDATA  (-21)

#define U32_GCL_HBRBACKGROUND (-10)
#define U32_GCL_HCURSOR       (-12)
#define U32_GCL_HICON         (-14)
#define U32_GCL_HMODULE       (-16)
#define U32_GCL_WNDPROC       (-24)
#define U32_GCL_STYLE         (-26)

#define U32_ERROR_INVALID_WINDOW_HANDLE 1400u

/* Métricas de borda do Windows 7 para um executável de subsistema 4.0 (sem o
 * SM_CXPADDEDBORDER do Aero, que só vale para executáveis 6.0+). */
#define U32_CYCAPTION   22
#define U32_CYMENU      20
#define U32_SIZEFRAME   4
#define U32_FIXEDFRAME  3
#define U32_BORDER      1

/* ---- tela ---- */

/* Resolução do monitor que o guest vê: RE5_SCREEN=LxA, senão o monitor
 * principal do host (Windows) ou 1920x1080. */
static void screen_size(int32_t *w, int32_t *h)
{
    static int32_t cw, ch;
    if (!cw) {
        const char *env = getenv("RE5_SCREEN");
        int a = 0, b = 0;
        if (env && sscanf(env, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) {
            cw = a;
            ch = b;
        } else {
#if defined(RE5_HOST_WINDOWS)
            cw = GetSystemMetrics(SM_CXSCREEN);
            ch = GetSystemMetrics(SM_CYSCREEN);
#endif
            if (cw <= 0 || ch <= 0) { cw = 1920; ch = 1080; }
        }
    }
    *w = cw;
    *h = ch;
}

static void u_GetSystemMetrics(CPU *c)
{
    uint32_t idx = re5_arg(c, 0);
    int32_t w, h;
    screen_size(&w, &h);
    switch (idx) {
    case 0: case 16: case 78: c->eax = (uint32_t)w; break;   /* CXSCREEN, CXFULLSCREEN, CXVIRTUALSCREEN */
    case 1: c->eax = (uint32_t)h; break;                       /* CYSCREEN */
    case 17: c->eax = (uint32_t)(h - U32_CYCAPTION); break;   /* CYFULLSCREEN: sem a barra de título */
    case 79: c->eax = (uint32_t)h; break;                      /* CYVIRTUALSCREEN */
    case 76: case 77: c->eax = 0; break;                       /* X/YVIRTUALSCREEN */
    case 4: c->eax = U32_CYCAPTION; break;                     /* CYCAPTION */
    case 5: case 6: c->eax = U32_BORDER; break;                /* CX/CYBORDER */
    case 7: case 8: c->eax = U32_FIXEDFRAME; break;            /* CX/CYDLGFRAME */
    case 32: case 33: c->eax = U32_SIZEFRAME; break;           /* CX/CYFRAME */
    case 15: c->eax = U32_CYMENU; break;                       /* CYMENU */
    case 11: case 12: c->eax = 32; break;                      /* CX/CYICON */
    case 13: case 14: c->eax = 32; break;                      /* CX/CYCURSOR */
    case 19: c->eax = 1; break;                                /* MOUSEPRESENT */
    case 23: c->eax = 0; break;                                /* SWAPBUTTON */
    case 43: c->eax = 3; break;                                /* CMOUSEBUTTONS */
    case 75: c->eax = 1; break;                                /* MOUSEWHEELPRESENT */
    case 80: c->eax = 1; break;                                /* CMONITORS */
    case 81: c->eax = 1; break;                                /* SAMEDISPLAYFORMAT */
    case 4096: c->eax = 0; break;                              /* REMOTESESSION */
    default: re5_fatal("user32", "GetSystemMetrics(%u) sem implementação", idx);
    }
}

/* Moldura (esquerda, topo, direita, baixo) que o estilo acrescenta à área cliente. */
static void frame_of(uint32_t style, int menu, int32_t f[4])
{
    int32_t b = 0;
    if (style & U32_WS_THICKFRAME) b = U32_SIZEFRAME;
    else if (style & U32_WS_DLGFRAME) b = U32_FIXEDFRAME;
    else if (style & U32_WS_BORDER) b = U32_BORDER;
    f[0] = f[1] = f[2] = f[3] = b;
    if ((style & U32_WS_CAPTION) == U32_WS_CAPTION) f[1] += U32_CYCAPTION;
    if (menu) f[1] += U32_CYMENU;
}

static void u_AdjustWindowRect(CPU *c)
{
    uint32_t rc = re5_arg(c, 0), style = re5_arg(c, 1), menu = re5_arg(c, 2);
    int32_t r[4], f[4];
    memcpy(r, re5_g2h(rc), sizeof r);   /* left, top, right, bottom */
    frame_of(style, menu != 0, f);
    r[0] -= f[0];
    r[1] -= f[1];
    r[2] += f[2];
    r[3] += f[3];
    memcpy(re5_g2h(rc), r, sizeof r);
    c->eax = 1;
}

/* Configurações do sistema que o jogo liga e desliga enquanto roda. O estado
 * fica no runtime (o guest lê de volta o que escreveu); o desktop do host não
 * muda - o SDL já suspende o protetor de tela por conta própria. */
#define U32_SPI_GETSCREENSAVEACTIVE 0x0010u
#define U32_SPI_SETSCREENSAVEACTIVE 0x0011u
#define U32_SPI_GETSHOWIMEUI        0x006Eu
#define U32_SPI_SETSHOWIMEUI        0x006Fu
#define U32_SPI_GETFILTERKEYS       0x0032u
#define U32_SPI_SETFILTERKEYS       0x0033u
#define U32_SPI_GETTOGGLEKEYS       0x0034u
#define U32_SPI_SETTOGGLEKEYS       0x0035u
#define U32_SPI_GETSTICKYKEYS       0x003Au
#define U32_SPI_SETSTICKYKEYS       0x003Bu
#define U32_SPI_SETMOUSETRAILS      0x005Du
#define U32_SPI_GETMOUSETRAILS      0x005Eu

static uint32_t g_screensaver = 1, g_show_ime_ui = 1, g_mouse_trails;   /* rastro do mouse: desligado */

/* Teclas de aderência/alternância/filtro (o jogo desliga o atalho enquanto
 * roda e restaura na saída). Valores padrão do Windows 7: disponíveis, com
 * atalho ativo. STICKYKEYS e TOGGLEKEYS: {cbSize, dwFlags}; FILTERKEYS:
 * {cbSize, dwFlags, iWaitMSec, iDelayMSec, iRepeatMSec, iBounceMSec}. */
static uint32_t g_stickykeys[2] = {8, 0x1FE}, g_togglekeys[2] = {8, 0x1E};
static uint32_t g_filterkeys[6] = {24, 0x7E, 1000, 1000, 500, 0};

static int spi_struct(uint32_t pv, uint32_t ui, uint32_t *st, uint32_t n, int set)
{
    if (!pv || ui != 4u * n || rd32(pv) != 4u * n) return 0;   /* cbSize errado: o Windows recusa */
    for (uint32_t i = 1; i < n; i++) {
        if (set) st[i] = rd32(pv + 4u * i);
        else wr32(pv + 4u * i, st[i]);
    }
    return 1;
}

static void u_SystemParametersInfoA(CPU *c)
{
    uint32_t action = re5_arg(c, 0), ui = re5_arg(c, 1), pv = re5_arg(c, 2);
    switch (action) {
    case U32_SPI_GETSCREENSAVEACTIVE: if (pv) wr32(pv, g_screensaver); break;
    case U32_SPI_SETSCREENSAVEACTIVE: g_screensaver = ui != 0; break;
    case U32_SPI_GETSHOWIMEUI: if (pv) wr32(pv, g_show_ime_ui); break;
    case U32_SPI_SETSHOWIMEUI: g_show_ime_ui = ui != 0; break;
    case U32_SPI_GETMOUSETRAILS: if (pv) wr32(pv, g_mouse_trails); break;
    case U32_SPI_SETMOUSETRAILS: g_mouse_trails = ui; break;
    case U32_SPI_GETSTICKYKEYS: case U32_SPI_SETSTICKYKEYS:
        c->eax = (uint32_t)spi_struct(pv, ui, g_stickykeys, 2, action == U32_SPI_SETSTICKYKEYS);
        return;
    case U32_SPI_GETTOGGLEKEYS: case U32_SPI_SETTOGGLEKEYS:
        c->eax = (uint32_t)spi_struct(pv, ui, g_togglekeys, 2, action == U32_SPI_SETTOGGLEKEYS);
        return;
    case U32_SPI_GETFILTERKEYS: case U32_SPI_SETFILTERKEYS:
        c->eax = (uint32_t)spi_struct(pv, ui, g_filterkeys, 6, action == U32_SPI_SETFILTERKEYS);
        return;
    default: re5_fatal("user32", "SystemParametersInfoA(0x%X, %u, 0x%08X) sem implementação", action, ui, pv);
    }
    c->eax = 1;
}

/* ---- strings do guest ---- */

static void read_guest_str(uint32_t va, int wide, char *dst, size_t cap)
{
    dst[0] = 0;
    if (!va) return;
    if (va < 0x10000u) {   /* átomo (MAKEINTATOM) */
        snprintf(dst, cap, "#%u", va);
        return;
    }
    size_t d = 0;
    if (!wide) {
        snprintf(dst, cap, "%s", re5_gstr(va));
        return;
    }
    for (uint32_t p = va;; p += 2) {
        uint32_t cp = rd16(p);
        if (!cp || d + 4 >= cap) break;
        if (cp < 0x80) dst[d++] = (char)cp;
        else if (cp < 0x800) {
            dst[d++] = (char)(0xC0 | (cp >> 6));
            dst[d++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[d++] = (char)(0xE0 | (cp >> 12));
            dst[d++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[d++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[d] = 0;
}

static int streq_ci(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Espaço temporário na pilha do guest para estruturas que o WndProc recebe por
 * ponteiro (CREATESTRUCT, WINDOWPOS). Baixa o esp da própria CPU, então as
 * chamadas aninhadas ao guest empilham abaixo dele. */
typedef struct { CPU *c; uint32_t saved_esp; } scratch;

static uint32_t scratch_begin(scratch *s, uint32_t size)
{
    re5_thread *t = re5_thread_current();
    if (!t) re5_fatal("user32", "chamada de janela fora de uma thread do guest");
    s->c = &t->cpu;
    s->saved_esp = s->c->esp;
    s->c->esp = (s->c->esp - size - 16u) & ~15u;
    return s->c->esp;
}

static void scratch_end(scratch *s) { s->c->esp = s->saved_esp; }

/* ---- classes ---- */

typedef struct {
    char     name[64];
    uint32_t proc, style, cls_extra, wnd_extra, hinst, icon, cursor, brush;
} wnd_class;

#define MAX_CLASSES 32
static wnd_class g_classes[MAX_CLASSES];
static int g_nclasses;

static wnd_class *find_class(const char *name)
{
    for (int i = 0; i < g_nclasses; i++)
        if (streq_ci(g_classes[i].name, name)) return &g_classes[i];
    return NULL;
}

static void register_class(CPU *c, int wide)
{
    /* WNDCLASSEX: cbSize, style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance,
     * hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName, hIconSm. */
    uint32_t wc = re5_arg(c, 0);
    char name[64];
    read_guest_str(rd32(wc + 40), wide, name, sizeof name);
    c->eax = 0;
    if (find_class(name)) {
        re5_set_last_error(1410u);   /* ERROR_CLASS_ALREADY_EXISTS */
        return;
    }
    if (g_nclasses >= MAX_CLASSES) re5_fatal("user32", "mais de %d classes de janela", MAX_CLASSES);
    wnd_class *k = &g_classes[g_nclasses];
    snprintf(k->name, sizeof k->name, "%s", name);
    k->style = rd32(wc + 4);
    k->proc = rd32(wc + 8);
    k->cls_extra = rd32(wc + 12);
    k->wnd_extra = rd32(wc + 16);
    k->hinst = rd32(wc + 20);
    k->icon = rd32(wc + 24);
    k->cursor = rd32(wc + 28);
    k->brush = rd32(wc + 32);
    RE5_INFO("user32", "classe registrada: '%s' (WndProc=0x%08X)", name, k->proc);
    c->eax = 0xC000u + (uint32_t)g_nclasses++;   /* ATOM */
}

static void u_RegisterClassExW(CPU *c) { register_class(c, 1); }
static void u_RegisterClassExA(CPU *c) { register_class(c, 0); }

/* ---- janelas ---- */

typedef struct {
    int         used;
    uint32_t    hwnd;
    int         cls;
    uint32_t    proc, style, exstyle, hinst, parent, menu, user_data, owner_tid;
    int32_t     x, y, w, h;      /* retângulo externo, coordenadas de tela */
    int         visible;
    char        title[256];
    SDL_Window *sdl;
} window;

#define MAX_WINDOWS 16
#define HWND_BASE   0x00010010u
static window g_windows[MAX_WINDOWS];
static uint32_t g_active, g_focus;
static int g_sdl_ready;

static window *find_window(uint32_t hwnd)
{
    if (hwnd < HWND_BASE || (hwnd - HWND_BASE) % 4u) return NULL;
    uint32_t i = (hwnd - HWND_BASE) / 4u;
    return i < MAX_WINDOWS && g_windows[i].used ? &g_windows[i] : NULL;
}

static window *find_sdl(Uint32 id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].used && g_windows[i].sdl && SDL_GetWindowID(g_windows[i].sdl) == id) return &g_windows[i];
    return NULL;
}

static void client_size(const window *w, int32_t *cw, int32_t *ch)
{
    int32_t f[4];
    frame_of(w->style, w->menu != 0, f);
    *cw = w->w - f[0] - f[2];
    *ch = w->h - f[1] - f[3];
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

static void client_origin(const window *w, int32_t *ox, int32_t *oy)
{
    int32_t f[4];
    frame_of(w->style, w->menu != 0, f);
    *ox = w->x + f[0];
    *oy = w->y + f[1];
}

static uint32_t send_msg(window *w, uint32_t msg, uint32_t wparam, uint32_t lparam);
static void activate(window *w, int on);
static void update_clip_grab(void);
static int cursor_hidden(void);
static void free_cursors(void);

static uint32_t pack16(int32_t lo, int32_t hi) { return ((uint32_t)(hi & 0xFFFF) << 16) | (uint32_t)(lo & 0xFFFF); }

static void sync_sdl_geometry(window *w)
{
    if (!w->sdl) return;
    int32_t cw, ch, ox, oy;
    client_size(w, &cw, &ch);
    client_origin(w, &ox, &oy);
    SDL_SetWindowSize(w->sdl, cw, ch);
    SDL_SetWindowPosition(w->sdl, ox, oy);
}

/* O que DefWindowProc faz com WM_WINDOWPOSCHANGED: WM_MOVE e WM_SIZE. */
static void notify_geometry(window *w, int moved, int sized)
{
    int32_t cw, ch, ox, oy;
    client_size(w, &cw, &ch);
    client_origin(w, &ox, &oy);
    if (moved) send_msg(w, U32_WM_MOVE, 0, pack16(ox, oy));
    if (sized) send_msg(w, U32_WM_SIZE, U32_SIZE_RESTORED, pack16(cw, ch));
}

static void destroy_window(window *w)
{
    uint32_t hwnd = w->hwnd;
    send_msg(w, U32_WM_DESTROY, 0, 0);
    send_msg(w, U32_WM_NCDESTROY, 0, 0);
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    memset(w, 0, sizeof *w);
    if (g_active == hwnd) g_active = 0;
    if (g_focus == hwnd) g_focus = 0;
    int any_used = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_windows[i].used) any_used = 1;
    if (!any_used) free_cursors();
}

static uint32_t default_proc(window *w, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    switch (msg) {
    case U32_WM_NCCREATE: return 1;
    case U32_WM_ERASEBKGND: return 1;
    case U32_WM_NCHITTEST: return U32_HTCLIENT;
    case U32_WM_MOUSEACTIVATE: return U32_MA_ACTIVATE;
    case U32_WM_NCACTIVATE: return 1;
    case U32_WM_SETCURSOR: return 0;
    case U32_WM_GETTEXTLENGTH: return w ? (uint32_t)strlen(w->title) : 0;
    case U32_WM_GETTEXT:
        if (w && lparam && wparam) {
            uint32_t n = (uint32_t)strlen(w->title);
            if (n >= wparam) n = wparam - 1;
            memcpy(re5_g2h(lparam), w->title, n);
            wr8(lparam + n, 0);
            return n;
        }
        return 0;
    case U32_WM_SYSCOMMAND:
        if ((wparam & 0xFFF0u) == U32_SC_CLOSE && w) send_msg(w, U32_WM_CLOSE, 0, 0);
        return 0;
    case U32_WM_CLOSE:
        if (w) destroy_window(w);
        return 0;
    case U32_WM_WINDOWPOSCHANGED:
        if (w) notify_geometry(w, 1, 1);
        return 0;
    default:
        (void)hwnd;
        return 0;
    }
}

static uint32_t send_msg(window *w, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    if (!w->proc) return default_proc(w, w->hwnd, msg, wparam, lparam);
    re5_thread *t = re5_thread_current();
    if (!t) re5_fatal("user32", "mensagem 0x%X para 0x%X fora de uma thread do guest", msg, w->hwnd);
    uint32_t args[4] = {w->hwnd, msg, wparam, lparam};
    return re5_guest_call(t, w->proc, 4, args);
}

/* Só trata as mensagens do Windows das janelas SDL (os eventos ficam na fila
 * do SDL até o PeekMessage do jogo): não roda código do guest. */
static void lock_wait_pump(void) { SDL_PumpEvents(); }

static void sdl_init(void)
{
    if (g_sdl_ready || getenv("RE5_HEADLESS")) return;
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   /* o handler de crash do runtime fica */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        re5_fatal("user32", "SDL_InitSubSystem(VIDEO) falhou: %s", SDL_GetError());
    SDL_StartTextInput();
    g_sdl_ready = 1;
    /* A thread que cria a janela é a dona dela no Windows: tem de tratar as
     * mensagens mesmo enquanto espera a vez no lock do guest. */
    re5_thread_set_lock_wait_pump(lock_wait_pump);
}

static uint32_t create_window(CPU *c, int wide)
{
    uint32_t exstyle = re5_arg(c, 0), cls_va = re5_arg(c, 1), name_va = re5_arg(c, 2), style = re5_arg(c, 3);
    int32_t x = (int32_t)re5_arg(c, 4), y = (int32_t)re5_arg(c, 5);
    int32_t w = (int32_t)re5_arg(c, 6), h = (int32_t)re5_arg(c, 7);
    uint32_t parent = re5_arg(c, 8), menu = re5_arg(c, 9), hinst = re5_arg(c, 10), param = re5_arg(c, 11);
    char clsname[64];
    read_guest_str(cls_va, wide, clsname, sizeof clsname);
    wnd_class *k = find_class(clsname);
    if (!k) {
        re5_set_last_error(1407u);   /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    if (parent) re5_fatal("user32", "CreateWindowEx de janela filha (pai 0x%X) sem implementação", parent);
    int slot = 0;
    while (slot < MAX_WINDOWS && g_windows[slot].used) slot++;
    if (slot == MAX_WINDOWS) re5_fatal("user32", "mais de %d janelas", MAX_WINDOWS);

    window *win = &g_windows[slot];
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->hwnd = HWND_BASE + 4u * (uint32_t)slot;
    win->cls = (int)(k - g_classes);
    win->proc = k->proc;
    win->style = style;
    win->exstyle = exstyle;
    win->hinst = hinst;
    win->menu = menu;
    re5_thread *t = re5_thread_current();
    win->owner_tid = t ? t->tid : 0;
    read_guest_str(name_va, wide, win->title, sizeof win->title);

    int32_t sw, sh;
    screen_size(&sw, &sh);
    if ((uint32_t)w == U32_CW_USEDEFAULT) { w = sw * 3 / 4; h = sh * 3 / 4; }
    if ((uint32_t)x == U32_CW_USEDEFAULT) { x = (sw - w) / 2; y = (sh - h) / 2; }
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->visible = (style & U32_WS_VISIBLE) != 0;

    sdl_init();
    if (g_sdl_ready) {
        int32_t cw, ch, ox, oy;
        client_size(win, &cw, &ch);
        client_origin(win, &ox, &oy);
        Uint32 flags = win->visible ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN;
        if (style & U32_WS_THICKFRAME) flags |= SDL_WINDOW_RESIZABLE;
        if ((style & U32_WS_CAPTION) != U32_WS_CAPTION) flags |= SDL_WINDOW_BORDERLESS;
        win->sdl = SDL_CreateWindow(win->title[0] ? win->title : "Resident Evil 5", ox, oy, cw, ch, flags);
        if (!win->sdl) re5_fatal("user32", "SDL_CreateWindow(%dx%d) falhou: %s", cw, ch, SDL_GetError());
        RE5_INFO("user32", "janela 0x%X '%s': externa %dx%d em (%d,%d), cliente %dx%d (SDL)", win->hwnd,
                 win->title, w, h, x, y, cw, ch);
    }

    /* CREATESTRUCT: lpCreateParams, hInstance, hMenu, hwndParent, cy, cx, y, x,
     * style, lpszName, lpszClass, dwExStyle. */
    scratch s;
    uint32_t cs = scratch_begin(&s, 48);
    const uint32_t v[12] = {param, hinst, menu, parent, (uint32_t)h, (uint32_t)w, (uint32_t)y, (uint32_t)x,
                            style, name_va, cls_va, exstyle};
    memcpy(re5_g2h(cs), v, sizeof v);
    uint32_t ok = send_msg(win, U32_WM_NCCREATE, 0, cs);
    int32_t created = ok ? (int32_t)send_msg(win, U32_WM_CREATE, 0, cs) : -1;
    scratch_end(&s);
    if (created == -1) {
        if (win->used) destroy_window(win);
        return 0;
    }
    if (win->visible) {
        notify_geometry(win, 1, 1);
        activate(win, 1);
    }
    return win->hwnd;
}

static void u_CreateWindowExW(CPU *c) { c->eax = create_window(c, 1); }
static void u_CreateWindowExA(CPU *c) { c->eax = create_window(c, 0); }

static window *arg_window(CPU *c, int i)
{
    window *w = find_window(re5_arg(c, i));
    if (!w) re5_set_last_error(U32_ERROR_INVALID_WINDOW_HANDLE);
    return w;
}

static void u_DestroyWindow(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = 0;
    if (!w) return;
    destroy_window(w);
    c->eax = 1;
}

static void activate(window *w, int on)
{
    if (on) {
        g_active = g_focus = w->hwnd;
        send_msg(w, U32_WM_ACTIVATEAPP, 1, 0);
        send_msg(w, U32_WM_NCACTIVATE, 1, 0);
        send_msg(w, U32_WM_ACTIVATE, U32_WA_ACTIVE, 0);
        send_msg(w, U32_WM_SETFOCUS, 0, 0);
    } else {
        send_msg(w, U32_WM_NCACTIVATE, 0, 0);
        send_msg(w, U32_WM_ACTIVATE, U32_WA_INACTIVE, 0);
        send_msg(w, U32_WM_ACTIVATEAPP, 0, 0);
        send_msg(w, U32_WM_KILLFOCUS, 0, 0);
        if (g_active == w->hwnd) g_active = 0;
        if (g_focus == w->hwnd) g_focus = 0;
    }
    update_clip_grab();
}

static void u_ShowWindow(CPU *c)
{
    window *w = arg_window(c, 0);
    uint32_t cmd = re5_arg(c, 1);   /* SW_HIDE = 0, o resto mostra */
    c->eax = 0;
    if (!w) return;
    int was = w->visible, show = cmd != 0;
    c->eax = (uint32_t)was;
    if (was == show) return;
    w->visible = show;
    if (w->sdl) {
        if (show) SDL_ShowWindow(w->sdl);
        else SDL_HideWindow(w->sdl);
    }
    send_msg(w, U32_WM_SHOWWINDOW, (uint32_t)show, 0);
    if (show) {
        notify_geometry(w, 1, 1);
        activate(w, 1);
    } else if (g_active == w->hwnd) {
        activate(w, 0);
    }
}

static void u_UpdateWindow(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = w != NULL;
    if (w && w->visible) send_msg(w, U32_WM_PAINT, 0, 0);
}

static void move_window(window *w, int32_t x, int32_t y, int32_t cx, int32_t cy, int move, int size)
{
    int moved = move && (x != w->x || y != w->y), sized = size && (cx != w->w || cy != w->h);
    if (move) { w->x = x; w->y = y; }
    if (size) { w->w = cx; w->h = cy; }
    if (moved || sized) sync_sdl_geometry(w);
    notify_geometry(w, moved, sized);
}

static void u_MoveWindow(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = w != NULL;
    if (w) move_window(w, (int32_t)re5_arg(c, 1), (int32_t)re5_arg(c, 2), (int32_t)re5_arg(c, 3),
                       (int32_t)re5_arg(c, 4), 1, 1);
}

static void u_SetWindowPos(CPU *c)
{
    window *w = arg_window(c, 0);
    uint32_t flags = re5_arg(c, 6);
    c->eax = w != NULL;
    if (!w) return;
    /* hWndInsertAfter (ordem Z, HWND_TOPMOST) não tem efeito com uma janela só. */
    move_window(w, (int32_t)re5_arg(c, 2), (int32_t)re5_arg(c, 3), (int32_t)re5_arg(c, 4), (int32_t)re5_arg(c, 5),
                !(flags & U32_SWP_NOMOVE), !(flags & U32_SWP_NOSIZE));
    if ((flags & U32_SWP_SHOWWINDOW) && !w->visible) { w->visible = 1; if (w->sdl) SDL_ShowWindow(w->sdl); }
    if ((flags & U32_SWP_HIDEWINDOW) && w->visible) { w->visible = 0; if (w->sdl) SDL_HideWindow(w->sdl); }
}

static void u_GetClientRect(CPU *c)
{
    window *w = arg_window(c, 0);
    uint32_t rc = re5_arg(c, 1);
    c->eax = 0;
    if (!w) return;
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    const int32_t r[4] = {0, 0, cw, ch};
    memcpy(re5_g2h(rc), r, sizeof r);
    c->eax = 1;
}

static void u_GetWindowRect(CPU *c)
{
    window *w = arg_window(c, 0);
    uint32_t rc = re5_arg(c, 1);
    c->eax = 0;
    if (!w) return;
    const int32_t r[4] = {w->x, w->y, w->x + w->w, w->y + w->h};
    memcpy(re5_g2h(rc), r, sizeof r);
    c->eax = 1;
}

static void map_point(CPU *c, int sign)
{
    window *w = arg_window(c, 0);
    uint32_t pt = re5_arg(c, 1);
    c->eax = 0;
    if (!w) return;
    int32_t ox, oy;
    client_origin(w, &ox, &oy);
    wr32(pt, (uint32_t)((int32_t)rd32(pt) + sign * ox));
    wr32(pt + 4, (uint32_t)((int32_t)rd32(pt + 4) + sign * oy));
    c->eax = 1;
}

static void u_ScreenToClient(CPU *c) { map_point(c, -1); }
static void u_ClientToScreen(CPU *c) { map_point(c, 1); }

static void u_SetWindowTextW(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = 0;
    if (!w) return;
    read_guest_str(re5_arg(c, 1), 1, w->title, sizeof w->title);
    if (w->sdl) SDL_SetWindowTitle(w->sdl, w->title);
    c->eax = 1;
}

/* Procura por classe e/ou título - é como o jogo detecta outra instância aberta. */
static void u_FindWindowW(CPU *c)
{
    char cls[64], title[256];
    uint32_t cls_va = re5_arg(c, 0), title_va = re5_arg(c, 1);
    read_guest_str(cls_va, 1, cls, sizeof cls);
    read_guest_str(title_va, 1, title, sizeof title);
    c->eax = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window *w = &g_windows[i];
        if (!w->used) continue;
        if (cls_va && !streq_ci(g_classes[w->cls].name, cls)) continue;
        if (title_va && strcmp(w->title, title) != 0) continue;
        c->eax = w->hwnd;
        return;
    }
}

static void u_GetForegroundWindow(CPU *c) { c->eax = g_active; }
static void u_GetFocus(CPU *c) { c->eax = g_focus; }

static void u_SetForegroundWindow(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = 0;
    if (!w) return;
    if (w->sdl) SDL_RaiseWindow(w->sdl);
    if (g_active != w->hwnd) activate(w, 1);
    c->eax = 1;
}

static void u_SetActiveWindow(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = g_active;
    if (w && g_active != w->hwnd) activate(w, 1);
}

static void u_IsZoomed(CPU *c) { c->eax = 0; }   /* nada maximiza a janela */

static void u_IsIconic(CPU *c)
{
    window *w = find_window(re5_arg(c, 0));
    c->eax = w && w->sdl && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED) ? 1u : 0u;
}

static void u_GetWindowThreadProcessId(CPU *c)
{
    window *w = arg_window(c, 0);
    uint32_t pid = re5_arg(c, 1);
    if (pid) wr32(pid, w ? RE5_GUEST_PID : 0);
    c->eax = w ? w->owner_tid : 0;
}

/* Todas as janelas e threads do guest compartilham um estado de entrada só. */
static void u_AttachThreadInput(CPU *c) { c->eax = 1; }

static void u_SetWindowLongA(CPU *c)
{
    window *w = arg_window(c, 0);
    int32_t idx = (int32_t)re5_arg(c, 1);
    uint32_t val = re5_arg(c, 2), *field;
    c->eax = 0;
    if (!w) return;
    switch (idx) {
    case U32_GWL_WNDPROC: field = &w->proc; break;
    case U32_GWL_USERDATA: field = &w->user_data; break;
    case U32_GWL_STYLE: field = &w->style; break;
    case U32_GWL_EXSTYLE: field = &w->exstyle; break;
    case U32_GWL_HINSTANCE: field = &w->hinst; break;
    default: re5_fatal("user32", "SetWindowLongA(índice %d) sem implementação", idx);
    }
    c->eax = *field;
    *field = val;
    if (idx == U32_GWL_STYLE && w->sdl) {
        SDL_SetWindowBordered(w->sdl, (val & U32_WS_CAPTION) == U32_WS_CAPTION ? SDL_TRUE : SDL_FALSE);
        sync_sdl_geometry(w);
    }
}

static wnd_class *arg_class(CPU *c)
{
    window *w = arg_window(c, 0);
    return w ? &g_classes[w->cls] : NULL;
}

static uint32_t *class_field(wnd_class *k, int32_t idx)
{
    switch (idx) {
    case U32_GCL_HICON: return &k->icon;
    case U32_GCL_HCURSOR: return &k->cursor;
    case U32_GCL_HBRBACKGROUND: return &k->brush;
    case U32_GCL_STYLE: return &k->style;
    case U32_GCL_WNDPROC: return &k->proc;
    case U32_GCL_HMODULE: return &k->hinst;
    default: re5_fatal("user32", "Get/SetClassLongA(índice %d) sem implementação", idx);
    }
}

static void u_GetClassLongA(CPU *c)
{
    wnd_class *k = arg_class(c);
    c->eax = k ? *class_field(k, (int32_t)re5_arg(c, 1)) : 0;
}

static void u_SetClassLongA(CPU *c)
{
    wnd_class *k = arg_class(c);
    c->eax = 0;
    if (!k) return;
    uint32_t *f = class_field(k, (int32_t)re5_arg(c, 1));
    c->eax = *f;
    *f = re5_arg(c, 2);
}

/* ---- teclado: scancode do SDL -> VK e scancode set 1 ---- */

typedef struct { uint16_t vk, scan; } keymap;
static keymap g_keymap[SDL_NUM_SCANCODES];

static void keymap_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    for (int i = 0; i < 26; i++) g_keymap[SDL_SCANCODE_A + i] = (keymap){(uint16_t)('A' + i), letters[i]};
    for (int i = 0; i < 9; i++) g_keymap[SDL_SCANCODE_1 + i] = (keymap){(uint16_t)('1' + i), (uint16_t)(0x02 + i)};
    g_keymap[SDL_SCANCODE_0] = (keymap){'0', 0x0B};
    for (int i = 0; i < 10; i++) g_keymap[SDL_SCANCODE_F1 + i] = (keymap){(uint16_t)(0x70 + i), (uint16_t)(0x3B + i)};
    g_keymap[SDL_SCANCODE_F11] = (keymap){0x7A, 0x57};
    g_keymap[SDL_SCANCODE_F12] = (keymap){0x7B, 0x58};
    for (int i = 0; i < 9; i++) g_keymap[SDL_SCANCODE_KP_1 + i] = (keymap){(uint16_t)(0x61 + i), 0};
    static const uint8_t kp_scan[9] = {0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};
    for (int i = 0; i < 9; i++) g_keymap[SDL_SCANCODE_KP_1 + i].scan = kp_scan[i];
    static const struct { int sdl; uint16_t vk, scan; } rest[] = {
        {SDL_SCANCODE_RETURN, 0x0D, 0x1C}, {SDL_SCANCODE_ESCAPE, 0x1B, 0x01}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E},
        {SDL_SCANCODE_TAB, 0x09, 0x0F}, {SDL_SCANCODE_SPACE, 0x20, 0x39}, {SDL_SCANCODE_MINUS, 0xBD, 0x0C},
        {SDL_SCANCODE_EQUALS, 0xBB, 0x0D}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A},
        {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B}, {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B},
        {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28}, {SDL_SCANCODE_GRAVE, 0xC0, 0x29},
        {SDL_SCANCODE_COMMA, 0xBC, 0x33}, {SDL_SCANCODE_PERIOD, 0xBE, 0x34}, {SDL_SCANCODE_SLASH, 0xBF, 0x35},
        {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A}, {SDL_SCANCODE_PRINTSCREEN, 0x2C, 0x137},
        {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46}, {SDL_SCANCODE_PAUSE, 0x13, 0x45},
        {SDL_SCANCODE_INSERT, 0x2D, 0x152}, {SDL_SCANCODE_HOME, 0x24, 0x147}, {SDL_SCANCODE_PAGEUP, 0x21, 0x149},
        {SDL_SCANCODE_DELETE, 0x2E, 0x153}, {SDL_SCANCODE_END, 0x23, 0x14F}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x151},
        {SDL_SCANCODE_RIGHT, 0x27, 0x14D}, {SDL_SCANCODE_LEFT, 0x25, 0x14B}, {SDL_SCANCODE_DOWN, 0x28, 0x150},
        {SDL_SCANCODE_UP, 0x26, 0x148}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x145},
        {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0x135}, {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37},
        {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E}, {SDL_SCANCODE_KP_ENTER, 0x0D, 0x11C},
        {SDL_SCANCODE_KP_0, 0x60, 0x52}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53},
        {SDL_SCANCODE_NONUSBACKSLASH, 0xE2, 0x56},
        /* Modificadores: WM_KEYDOWN leva o VK genérico; o estado guarda os dois. */
        {SDL_SCANCODE_LCTRL, 0x11, 0x1D}, {SDL_SCANCODE_RCTRL, 0x11, 0x11D}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2A},
        {SDL_SCANCODE_RSHIFT, 0x10, 0x36}, {SDL_SCANCODE_LALT, 0x12, 0x38}, {SDL_SCANCODE_RALT, 0x12, 0x138},
        {SDL_SCANCODE_LGUI, 0x5B, 0x15B}, {SDL_SCANCODE_RGUI, 0x5C, 0x15C},
    };
    for (size_t i = 0; i < sizeof rest / sizeof rest[0]; i++) g_keymap[rest[i].sdl] = (keymap){rest[i].vk, rest[i].scan};
}

static uint8_t g_keystate[256];

/* O mesmo teclado e mouse vistos pelo DirectInput8 (dinput8.c): estado por
 * DIK_* e movimento relativo acumulado até alguém tirar. */
static uint8_t g_dik[256];
static int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;

/* DIK_* = scancode set 1, com 0x80 nas teclas estendidas (E0). Pause e Num
 * Lock são as exceções: no set 1 o Pause é 0x45 (sem E0) e o Num Lock E0 45. */
static uint8_t dik_of(uint16_t scan)
{
    if (scan == 0x45) return 0xC5;    /* DIK_PAUSE */
    if (scan == 0x145) return 0x45;   /* DIK_NUMLOCK */
    return (uint8_t)((scan & 0x7Fu) | (scan & 0x100u ? 0x80u : 0u));
}

static uint16_t lr_vk(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LALT: return 0xA4;
    case SDL_SCANCODE_RALT: return 0xA5;
    default: return 0;
    }
}

/* ---- fila de mensagens ---- */

typedef struct {
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t  pt_x, pt_y;
} queued_msg;

#define MAX_QUEUE 512
static queued_msg g_queue[MAX_QUEUE];
static int g_qhead, g_qcount;
static int g_quit_pending;
static uint32_t g_quit_code;
static int32_t g_cursor_x, g_cursor_y;   /* tela */

static void post_msg(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    if (g_qcount == MAX_QUEUE) {
        RE5_WARN("user32", "fila de mensagens cheia: 0x%X descartada (o jogo não está lendo a fila)", msg);
        return;
    }
    queued_msg *m = &g_queue[(g_qhead + g_qcount++) % MAX_QUEUE];
    *m = (queued_msg){hwnd, msg, wparam, lparam, SDL_GetTicks(), g_cursor_x, g_cursor_y};
}

void re5_user32_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    post_msg(hwnd, msg, wparam, lparam);
}

static uint32_t mouse_keys(void)
{
    uint32_t mk = 0, b = g_sdl_ready ? SDL_GetMouseState(NULL, NULL) : 0;
    if (b & SDL_BUTTON_LMASK) mk |= U32_MK_LBUTTON;
    if (b & SDL_BUTTON_RMASK) mk |= U32_MK_RBUTTON;
    if (b & SDL_BUTTON_MMASK) mk |= U32_MK_MBUTTON;
    if (g_keystate[0x10] & 0x80) mk |= U32_MK_SHIFT;
    if (g_keystate[0x11] & 0x80) mk |= U32_MK_CONTROL;
    return mk;
}

static void on_key(const SDL_KeyboardEvent *k, window *w)
{
    keymap_init();
    keymap m = g_keymap[k->keysym.scancode];
    if (!m.vk) return;
    int down = k->type == SDL_KEYDOWN;
    uint8_t v = down ? 0x80 : 0;
    g_keystate[m.vk] = (uint8_t)((g_keystate[m.vk] & 1) ^ (down && !k->repeat && m.vk == 0x14 ? 1 : 0)) | v;
    uint16_t lr = lr_vk(k->keysym.scancode);
    if (lr) g_keystate[lr] = v;
    if (m.scan) g_dik[dik_of(m.scan)] = v;
    int alt = (g_keystate[0x12] & 0x80) != 0;
    uint32_t lparam = 1u | (uint32_t)(m.scan & 0xFF) << 16 | (m.scan & 0x100 ? 1u << 24 : 0) | (alt ? 1u << 29 : 0);
    if (!down) lparam |= 0xC0000000u;
    else if (k->repeat) lparam |= 0x40000000u;
    /* Alt e F10 viram WM_SYS*, como no Windows. */
    int sys = alt || m.vk == 0x12 || m.vk == 0x79;
    uint32_t msg = down ? (sys ? U32_WM_SYSKEYDOWN : U32_WM_KEYDOWN) : (sys ? U32_WM_SYSKEYUP : U32_WM_KEYUP);
    post_msg(w ? w->hwnd : g_focus, msg, m.vk, lparam);
}

static void on_text(const char *utf8, window *w)
{
    const uint8_t *s = (const uint8_t *)utf8;
    while (*s) {
        uint32_t cp;
        int n = *s < 0x80 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 : 4;
        cp = n == 1 ? *s : (uint32_t)(*s & (0x7F >> n));
        for (int i = 1; i < n && s[i]; i++) cp = cp << 6 | (s[i] & 0x3F);
        s += n;
        if (cp >= 0x10000) {   /* fora do BMP: par substituto, um WM_CHAR por unidade */
            cp -= 0x10000;
            post_msg(w ? w->hwnd : g_focus, U32_WM_CHAR, 0xD800 + (cp >> 10), 1);
            post_msg(w ? w->hwnd : g_focus, U32_WM_CHAR, 0xDC00 + (cp & 0x3FF), 1);
        } else {
            post_msg(w ? w->hwnd : g_focus, U32_WM_CHAR, cp, 1);
        }
    }
}

static void on_window_event(const SDL_WindowEvent *e, window *w)
{
    if (!w) return;
    int32_t f[4];
    switch (e->event) {
    case SDL_WINDOWEVENT_CLOSE:
        RE5_INFO("user32", "janela 0x%X: o host pediu para fechar (SDL_WINDOWEVENT_CLOSE)", w->hwnd);
        send_msg(w, U32_WM_SYSCOMMAND, U32_SC_CLOSE, 0);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED: if (g_active != w->hwnd) activate(w, 1); break;
    case SDL_WINDOWEVENT_FOCUS_LOST: if (g_active == w->hwnd) activate(w, 0); break;
    case SDL_WINDOWEVENT_MOVED:
        frame_of(w->style, w->menu != 0, f);
        w->x = e->data1 - f[0];
        w->y = e->data2 - f[1];
        notify_geometry(w, 1, 0);
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        frame_of(w->style, w->menu != 0, f);
        w->w = e->data1 + f[0] + f[2];
        w->h = e->data2 + f[1] + f[3];
        notify_geometry(w, 0, 1);
        break;
    default: break;
    }
}

/* Um evento do SDL: estado de teclado/mouse e mensagens para a fila. */
void re5_user32_handle_event(const SDL_Event *e)
{
    const SDL_Event ev = *e;
    window *w = NULL;
    switch (ev.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            if (ev.key.keysym.scancode == SDL_SCANCODE_F7) re5_det_key(7);
            if (ev.key.keysym.scancode == SDL_SCANCODE_F8) re5_cov_key(8);
            if (ev.key.keysym.scancode == SDL_SCANCODE_F9) re5_cov_key(9);
            if (ev.key.keysym.scancode == SDL_SCANCODE_F10) { re5_dump_key(10); re5_det_key(10); }
            if (ev.key.keysym.scancode == SDL_SCANCODE_F6) re5_coldebug_key();
            if (ev.key.keysym.scancode == SDL_SCANCODE_F11) re5_capture_key();
        }
        on_key(&ev.key, find_sdl(ev.key.windowID));
        break;
    case SDL_TEXTINPUT:
        on_text(ev.text.text, find_sdl(ev.text.windowID));
        break;
    case SDL_MOUSEMOTION:
        g_mouse_dx += ev.motion.xrel;
        g_mouse_dy += ev.motion.yrel;
        if ((w = find_sdl(ev.motion.windowID)) != NULL) {
            int32_t ox, oy;
            client_origin(w, &ox, &oy);
            g_cursor_x = ox + ev.motion.x;
            g_cursor_y = oy + ev.motion.y;
            post_msg(w->hwnd, U32_WM_MOUSEMOVE, mouse_keys(), pack16(ev.motion.x, ev.motion.y));
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if ((w = find_sdl(ev.button.windowID)) != NULL) {
            int down = ev.type == SDL_MOUSEBUTTONDOWN;
            uint32_t msg = ev.button.button == SDL_BUTTON_LEFT ? (down ? U32_WM_LBUTTONDOWN : U32_WM_LBUTTONUP)
                         : ev.button.button == SDL_BUTTON_RIGHT ? (down ? U32_WM_RBUTTONDOWN : U32_WM_RBUTTONUP)
                         : ev.button.button == SDL_BUTTON_MIDDLE ? (down ? U32_WM_MBUTTONDOWN : U32_WM_MBUTTONUP)
                         : 0;
            if (msg) post_msg(w->hwnd, msg, mouse_keys(), pack16(ev.button.x, ev.button.y));
        }
        break;
    case SDL_MOUSEWHEEL:
        g_mouse_dz += (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y) * 120;
        if ((w = find_sdl(ev.wheel.windowID)) != NULL)
            post_msg(w->hwnd, U32_WM_MOUSEWHEEL, mouse_keys() | (uint32_t)(ev.wheel.y * 120) << 16,
                     pack16(g_cursor_x, g_cursor_y));   /* WM_MOUSEWHEEL leva coordenadas de tela */
        break;
    case SDL_WINDOWEVENT:
        on_window_event(&ev.window, find_sdl(ev.window.windowID));
        break;
    default:
        break;   /* SDL_QUIT: quem decide sair é o jogo (WM_CLOSE -> WM_DESTROY -> PostQuitMessage) */
    }
}

/* ---- tela cheia sem bordas e mouse relativo ----
 * Pedidos de qualquer thread; aplicados por quem bombeia a fila (a thread dona
 * da janela), porque mudar a janela de outra thread pode travar no Windows. */
static volatile uint32_t g_fs_hwnd;
static volatile int g_fs_want = -1, g_mouse_excl, g_rel_on;

void re5_user32_set_fullscreen(uint32_t hwnd, int on)
{
    g_fs_hwnd = hwnd;
    g_fs_want = on != 0;
}

void re5_user32_set_mouse_exclusive(int on) { g_mouse_excl = on != 0; }

static int cursor_hidden(void);

static void apply_display_requests(void)
{
    int fs = g_fs_want;
    if (fs >= 0) {
        g_fs_want = -1;
        window *w = find_window(g_fs_hwnd);
        if (w && w->sdl) {
            SDL_SetWindowFullscreen(w->sdl, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            RE5_INFO("user32", "janela 0x%X: %s", w->hwnd, fs ? "tela cheia sem bordas" : "janela");
        }
    }
    int rel = g_active && (g_mouse_excl || cursor_hidden());
    if (rel != g_rel_on) {
        g_rel_on = rel;
        SDL_SetRelativeMouseMode(rel ? SDL_TRUE : SDL_FALSE);
    }
    update_clip_grab();
}

static void pump(void)
{
    if (!g_sdl_ready) return;
    apply_display_requests();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) re5_user32_handle_event(&ev);
}

void re5_user32_pump(void) { pump(); }

/* ---- entrada roteirizada (RE5_INPUT_SCRIPT) ----
 * Lista "N:TECLA,+M:TECLA,...": aperta a tecla (nome do SDL: Return, Down,
 * Escape...) no Present N, ou M Presents depois do item anterior, e solta
 * RE5_INPUT_HOLD Presents depois (padrão 6). Para levar o jogo a um ponto
 * (menu, benchmark) sem ninguém no teclado. Nome que o SDL não conhece aborta.
 * RE5_INPUT_SYNC=<trecho>: os números contam a partir do Present em que o jogo
 * abre um arquivo cujo caminho contém o trecho (ex.: titlelogo, a música do
 * título) - a contagem até lá varia de uma execução para outra. */

#define SCRIPT_MAX 256
typedef struct { uint32_t at; SDL_Scancode sc; } script_item;
static script_item g_script[SCRIPT_MAX];
static int g_script_n = -1, g_script_next, g_script_held = -1;
static uint32_t g_script_up, g_script_hold = 6;
static uint32_t g_script_base, g_script_present;   /* RE5_INPUT_SYNC: Present da sincronia; o último visto */
static int g_script_wait = -1;                     /* 1 enquanto espera o arquivo da sincronia */

void re5_user32_script_file(const char *guest_path)
{
    if (g_script_wait != 1 || !guest_path) return;
    const char *want = getenv("RE5_INPUT_SYNC");
    char low[512];
    size_t n = strlen(guest_path) < sizeof low - 1 ? strlen(guest_path) : sizeof low - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)guest_path[i]);
    low[n] = 0;
    if (!want || !strstr(low, want)) return;
    g_script_base = g_script_present;
    g_script_wait = 0;
    RE5_INFO("user32", "RE5_INPUT_SYNC: \"%s\" no Present %u: o roteiro conta daqui", guest_path, g_script_base);
}

static void script_parse(void)
{
    g_script_n = 0;
    const char *e = getenv("RE5_INPUT_SCRIPT"), *h = getenv("RE5_INPUT_HOLD");
    if (h && atoi(h) > 0) g_script_hold = (uint32_t)atoi(h);
    uint32_t prev = 0;
    for (const char *p = e; p && *p && g_script_n < SCRIPT_MAX;) {
        int rel = *p == '+';
        char *end;
        unsigned long n = strtoul(p + rel, &end, 10);
        if (*end != ':') re5_fatal("user32", "RE5_INPUT_SCRIPT: esperava N:TECLA em \"%s\"", p);
        const char *k = end + 1, *stop = strchr(k, ',');
        size_t len = stop ? (size_t)(stop - k) : strlen(k);
        char name[32];
        if (len >= sizeof name) len = sizeof name - 1;
        memcpy(name, k, len);
        name[len] = 0;
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc == SDL_SCANCODE_UNKNOWN) re5_fatal("user32", "RE5_INPUT_SCRIPT: tecla \"%s\" desconhecida do SDL", name);
        prev = rel ? prev + (uint32_t)n : (uint32_t)n;
        g_script[g_script_n++] = (script_item){prev, sc};
        p = stop ? stop + 1 : k + len;
    }
    if (g_script_n) RE5_INFO("user32", "RE5_INPUT_SCRIPT: %d teclas roteirizadas", g_script_n);
}

static void script_key(SDL_Scancode sc, int down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type = ev.type;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    re5_user32_handle_event(&ev);
}

void re5_user32_script_tick(uint32_t present)
{
    if (g_script_n < 0) script_parse();
    g_script_present = present;
    if (g_script_wait < 0) {
        const char *e = getenv("RE5_INPUT_SYNC");
        g_script_wait = e && *e && g_script_n > 0;
    }
    if (g_script_wait) return;
    present -= g_script_base;
    if (g_script_held >= 0 && present >= g_script_up) {
        script_key(g_script[g_script_held].sc, 0);
        g_script_held = -1;
    }
    if (g_script_held < 0 && g_script_next < g_script_n && present >= g_script[g_script_next].at) {
        RE5_INFO("user32", "RE5_INPUT_SCRIPT: Present %u (+%u), tecla %s", present + g_script_base, present,
                 SDL_GetScancodeName(g_script[g_script_next].sc));
        script_key(g_script[g_script_next].sc, 1);
        g_script_held = g_script_next++;
        g_script_up = present + g_script_hold;
    }
}

void re5_user32_dik_state(uint8_t out[256]) { memcpy(out, g_dik, sizeof g_dik); }

void re5_user32_mouse_take(int32_t *dx, int32_t *dy, int32_t *dz)
{
    *dx = g_mouse_dx;
    *dy = g_mouse_dy;
    *dz = g_mouse_dz;
    g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;
}

uint32_t re5_user32_mouse_buttons(void)
{
    uint32_t b = g_sdl_ready ? SDL_GetMouseState(NULL, NULL) : 0, out = 0;
    if (b & SDL_BUTTON_LMASK) out |= 1u;
    if (b & SDL_BUTTON_RMASK) out |= 2u;
    if (b & SDL_BUTTON_MMASK) out |= 4u;
    if (b & SDL_BUTTON_X1MASK) out |= 8u;
    if (b & SDL_BUTTON_X2MASK) out |= 16u;
    return out;
}

static int msg_matches(const queued_msg *m, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (hwnd && m->hwnd != hwnd) return 0;
    if (!lo && !hi) return 1;
    return m->message >= lo && m->message <= hi;
}

/* Tira (ou só olha) a primeira mensagem que casa. WM_QUIT vem depois da fila,
 * como no Windows. */
static int take_msg(queued_msg *out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    for (int i = 0; i < g_qcount; i++) {
        queued_msg *m = &g_queue[(g_qhead + i) % MAX_QUEUE];
        if (!msg_matches(m, hwnd, lo, hi)) continue;
        *out = *m;
        if (remove) {
            for (int j = i; j > 0; j--)
                g_queue[(g_qhead + j) % MAX_QUEUE] = g_queue[(g_qhead + j - 1) % MAX_QUEUE];
            g_qhead = (g_qhead + 1) % MAX_QUEUE;
            g_qcount--;
        }
        return 1;
    }
    if (g_quit_pending && (!lo && !hi ? 1 : (U32_WM_QUIT >= lo && U32_WM_QUIT <= hi))) {
        *out = (queued_msg){0, U32_WM_QUIT, g_quit_code, 0, SDL_GetTicks(), g_cursor_x, g_cursor_y};
        if (remove) g_quit_pending = 0;
        return 1;
    }
    return 0;
}

static void write_msg(uint32_t va, const queued_msg *m)
{
    const uint32_t d[7] = {m->hwnd, m->message, m->wparam, m->lparam, m->time, (uint32_t)m->pt_x, (uint32_t)m->pt_y};
    memcpy(re5_g2h(va), d, sizeof d);
}

static void u_PeekMessageW(CPU *c)
{
    uint32_t lp = re5_arg(c, 0), hwnd = re5_arg(c, 1), lo = re5_arg(c, 2), hi = re5_arg(c, 3);
    uint32_t remove = re5_arg(c, 4) & U32_PM_REMOVE;
    queued_msg m;
    pump();
    c->eax = (uint32_t)take_msg(&m, hwnd, lo, hi, (int)remove);
    if (c->eax) {
        write_msg(lp, &m);
        return;
    }
    /* Fila vazia: o laço principal do jogo gira aqui entre quadros. Cede o lock
     * do guest um instante para as threads de trabalho andarem. */
    RE5_BLOCKING_BEGIN();
    re5_thread_yield();
    RE5_BLOCKING_END();
}

static void u_GetMessageW(CPU *c)
{
    uint32_t lp = re5_arg(c, 0), hwnd = re5_arg(c, 1), lo = re5_arg(c, 2), hi = re5_arg(c, 3);
    queued_msg m;
    for (;;) {
        pump();
        if (take_msg(&m, hwnd, lo, hi, 1)) break;
        RE5_BLOCKING_BEGIN();
        if (g_sdl_ready) SDL_WaitEventTimeout(NULL, 10);
        else re5_thread_yield();
        RE5_BLOCKING_END();
    }
    write_msg(lp, &m);
    c->eax = m.message == U32_WM_QUIT ? 0u : 1u;
}

/* O WM_CHAR vem do SDL_TEXTINPUT (com layout e acentos); TranslateMessage só
 * diz se a mensagem era de tecla. */
static void u_TranslateMessage(CPU *c)
{
    uint32_t msg = rd32(re5_arg(c, 0) + 4);
    c->eax = msg == U32_WM_KEYDOWN || msg == U32_WM_SYSKEYDOWN;
}

static void u_DispatchMessageW(CPU *c)
{
    uint32_t lp = re5_arg(c, 0);
    window *w = find_window(rd32(lp));
    c->eax = w ? send_msg(w, rd32(lp + 4), rd32(lp + 8), rd32(lp + 12)) : 0;
}

static void u_PostQuitMessage(CPU *c)
{
    g_quit_pending = 1;
    g_quit_code = re5_arg(c, 0);
    RE5_INFO("user32", "PostQuitMessage(%u) chamado de 0x%08X", g_quit_code, rd32(c->esp));
}

static void u_SendMessageA(CPU *c)
{
    window *w = arg_window(c, 0);
    c->eax = w ? send_msg(w, re5_arg(c, 1), re5_arg(c, 2), re5_arg(c, 3)) : 0;
}

static void u_DefWindowProc(CPU *c)
{
    uint32_t hwnd = re5_arg(c, 0);
    c->eax = default_proc(find_window(hwnd), hwnd, re5_arg(c, 1), re5_arg(c, 2), re5_arg(c, 3));
}

/* PAINTSTRUCT: hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[32]. A
 * imagem é da Direct3D; não existe DC de GDI para pintar. */
#define U32_FAKE_HDC 0x20000001u

static void u_BeginPaint(CPU *c)
{
    window *w = arg_window(c, 0);
    uint32_t ps = re5_arg(c, 1);
    c->eax = 0;
    if (!w) return;
    int32_t cw, ch;
    client_size(w, &cw, &ch);
    memset(re5_g2h(ps), 0, 64);
    wr32(ps, U32_FAKE_HDC);
    wr32(ps + 16, (uint32_t)cw);
    wr32(ps + 20, (uint32_t)ch);
    c->eax = U32_FAKE_HDC;
}

static void u_EndPaint(CPU *c) { c->eax = 1; }

/* ---- cursor ---- */

#define U32_ICON_BASE   0x1C000000u
#define U32_CURSOR_BASE 0x1D000000u

/* Ícones e cursores: carregamos as formas estilizadas do RE5 (101 normal, 102 hover verde)
 * sobrepondo como SDL_Cursor com hotspot em (0, 0). */
static void u_LoadIcon(CPU *c) { c->eax = U32_ICON_BASE | (re5_arg(c, 1) & 0xFFFFu); }
static void u_LoadCursorA(CPU *c) { c->eax = U32_CURSOR_BASE | (re5_arg(c, 1) & 0xFFFFu); }
static void u_DestroyIcon(CPU *c) { c->eax = 1; }

static uint32_t g_cursor = U32_CURSOR_BASE | 32512u;   /* IDC_ARROW */
static int32_t g_cursor_count;                          /* contador do ShowCursor: começa em 0 com mouse */
static int g_clip_wanted;
static SDL_Cursor *g_cursor_sdl_sys_arrow;
static SDL_Cursor *g_cursor_sdl_101;
static SDL_Cursor *g_cursor_sdl_102;

static SDL_Cursor *create_color_cursor(const uint32_t *pixels, int w, int h, int hot_x, int hot_y)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        (void *)pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return NULL;
    SDL_Cursor *cur = SDL_CreateColorCursor(surf, hot_x, hot_y);
    SDL_FreeSurface(surf);
    return cur;
}

static void ensure_cursors(void)
{
    if (!g_sdl_ready || g_cursor_sdl_101) return;
    g_cursor_sdl_sys_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    g_cursor_sdl_101 = create_color_cursor(k_cursor_re5_arrow, 32, 32, 0, 0);
    g_cursor_sdl_102 = create_color_cursor(k_cursor_re5_hover, 32, 32, 0, 0);
}

static void free_cursors(void)
{
    if (g_cursor_sdl_101) { SDL_FreeCursor(g_cursor_sdl_101); g_cursor_sdl_101 = NULL; }
    if (g_cursor_sdl_102) { SDL_FreeCursor(g_cursor_sdl_102); g_cursor_sdl_102 = NULL; }
    if (g_cursor_sdl_sys_arrow) { SDL_FreeCursor(g_cursor_sdl_sys_arrow); g_cursor_sdl_sys_arrow = NULL; }
}

static int cursor_hidden(void)
{
    return !g_cursor || g_cursor == (U32_CURSOR_BASE | 104u) || g_cursor_count < 0;
}

static void update_clip_grab(void)
{
    window *w = find_window(g_active);
    if (!w) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_windows[i].used && g_windows[i].sdl) { w = &g_windows[i]; break; }
        }
    }
    if (!w || !w->sdl) return;

    /* No jogo original, quando o mouse está visível (menus, pause, inventário),
     * ele consegue sair dos limites da tela para clicar em outros programas.
     * Só confina à janela quando o jogo pediu ClipCursor E o cursor está escondido (gameplay). */
    int should_grab = g_clip_wanted && cursor_hidden();
    SDL_SetWindowGrab(w->sdl, should_grab ? SDL_TRUE : SDL_FALSE);
}

static void apply_cursor(void)
{
    if (!g_sdl_ready) return;
    ensure_cursors();
    int hidden = cursor_hidden();
    SDL_ShowCursor(hidden ? SDL_DISABLE : SDL_ENABLE);
    if (!hidden) {
        uint32_t id = g_cursor & 0xFFFFu;
        if (id == 102u && g_cursor_sdl_102) {
            SDL_SetCursor(g_cursor_sdl_102);
        } else if ((id == 101u || id == 32512u) && g_cursor_sdl_101) {
            SDL_SetCursor(g_cursor_sdl_101);
        } else if (g_cursor_sdl_sys_arrow) {
            SDL_SetCursor(g_cursor_sdl_sys_arrow);
        }
    }
    update_clip_grab();
}

static void u_SetCursor(CPU *c)
{
    c->eax = g_cursor;
    g_cursor = re5_arg(c, 0);
    apply_cursor();
}

static void u_ShowCursor(CPU *c)
{
    g_cursor_count += re5_arg(c, 0) ? 1 : -1;
    apply_cursor();
    c->eax = (uint32_t)g_cursor_count;
}

static void u_GetCursorPos(CPU *c)
{
    uint32_t pt = re5_arg(c, 0);
    int x = g_cursor_x, y = g_cursor_y;
    if (g_sdl_ready && !g_rel_on) {
        SDL_GetGlobalMouseState(&x, &y);
    }
    wr32(pt, (uint32_t)x);
    wr32(pt + 4, (uint32_t)y);
    c->eax = 1;
}

static void u_SetCursorPos(CPU *c)
{
    g_cursor_x = (int32_t)re5_arg(c, 0);
    g_cursor_y = (int32_t)re5_arg(c, 1);
    window *w = find_window(g_active);
    if (!w) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_windows[i].used && g_windows[i].sdl) { w = &g_windows[i]; break; }
        }
    }
    if (w && w->sdl) {
        int32_t ox, oy;
        client_origin(w, &ox, &oy);
        SDL_WarpMouseInWindow(w->sdl, g_cursor_x - ox, g_cursor_y - oy);
    }
    c->eax = 1;
}

/* Prender o cursor num retângulo vira prendê-lo na janela ativa (o jogo usa a
 * área cliente inteira); NULL solta. */
static void u_ClipCursor(CPU *c)
{
    g_clip_wanted = re5_arg(c, 0) != 0;
    update_clip_grab();
    c->eax = 1;
}

static void u_GetKeyboardState(CPU *c)
{
    memcpy(re5_g2h(re5_arg(c, 0)), g_keystate, sizeof g_keystate);
    c->eax = 1;
}

/* ---- miscelânea ---- */

static void u_GetSystemMenu(CPU *c) { c->eax = 0x1E000001u; }   /* menu de sistema: handle sem itens */
static void u_LoadAcceleratorsA(CPU *c) { c->eax = 0x1F000001u; }

/* WH_KEYBOARD_LL: o jogo só o usa para engolir a tecla Windows. No runtime não
 * existe teclado do sistema passando por hooks, então o hook nunca é chamado -
 * o mesmo que um Windows em que ninguém aperta a tecla. */
static void u_SetWindowsHookExA(CPU *c)
{
    RE5_INFO("user32", "SetWindowsHookExA(tipo %d, proc 0x%08X): registrado, nunca chamado",
             (int32_t)re5_arg(c, 0), re5_arg(c, 1));
    c->eax = 0x1E100000u | (re5_arg(c, 0) & 0xFFu);
}

static void u_UnhookWindowsHookEx(CPU *c) { c->eax = 1; }
static void u_CallNextHookEx(CPU *c) { c->eax = 0; }

/* Área de transferência: CF_TEXT vai para o clipboard do host. */
static void u_EmptyClipboard(CPU *c) { c->eax = 1; }
static void u_CloseClipboard(CPU *c) { c->eax = 1; }

static void u_SetClipboardData(CPU *c)
{
    uint32_t fmt = re5_arg(c, 0), mem = re5_arg(c, 1);
    if (fmt == 1u && mem && g_sdl_ready) SDL_SetClipboardText(re5_gstr(mem));   /* CF_TEXT; HGLOBAL == ponteiro */
    else if (fmt != 1u) RE5_WARN("user32", "SetClipboardData(formato %u) ignorado: só CF_TEXT chega ao host", fmt);
    c->eax = mem;
}

/* Para pontes que precisam da janela do host (D3D9 -> DXVK/Direct3D do host). */
SDL_Window *re5_user32_sdl_window(uint32_t hwnd)
{
    window *w = find_window(hwnd);
    return w ? w->sdl : NULL;
}

int re5_user32_client_size(uint32_t hwnd, int32_t *cw, int32_t *ch)
{
    window *w = find_window(hwnd);
    if (!w) return -1;
    client_size(w, cw, ch);
    return 0;
}

static const re5_import_def k_defs[] = {
    {"AdjustWindowRect",         u_AdjustWindowRect,         12, 0},
    {"AttachThreadInput",        u_AttachThreadInput,        12, 0},
    {"BeginPaint",               u_BeginPaint,                8, 0},
    {"CallNextHookEx",           u_CallNextHookEx,           16, 0},
    {"ClientToScreen",           u_ClientToScreen,            8, 0},
    {"ClipCursor",               u_ClipCursor,                4, 0},
    {"CloseClipboard",           u_CloseClipboard,            0, 0},
    {"CreateWindowExA",          u_CreateWindowExA,          48, 0},
    {"CreateWindowExW",          u_CreateWindowExW,          48, 0},
    {"DefWindowProcA",           u_DefWindowProc,            16, 0},
    {"DefWindowProcW",           u_DefWindowProc,            16, 0},
    {"DestroyIcon",              u_DestroyIcon,               4, 0},
    {"DestroyWindow",            u_DestroyWindow,             4, 0},
    {"DispatchMessageW",         u_DispatchMessageW,          4, 0},
    {"EmptyClipboard",           u_EmptyClipboard,            0, 0},
    {"EndPaint",                 u_EndPaint,                  8, 0},
    {"FindWindowW",              u_FindWindowW,               8, 0},
    {"GetClassLongA",            u_GetClassLongA,             8, 0},
    {"GetClientRect",            u_GetClientRect,             8, 0},
    {"GetCursorPos",             u_GetCursorPos,              4, 0},
    {"GetFocus",                 u_GetFocus,                  0, 0},
    {"GetForegroundWindow",      u_GetForegroundWindow,       0, 0},
    {"GetKeyboardState",         u_GetKeyboardState,          4, 0},
    {"GetMessageW",              u_GetMessageW,              16, 0},
    {"GetSystemMenu",            u_GetSystemMenu,             8, 0},
    {"GetSystemMetrics",         u_GetSystemMetrics,          4, 0},
    {"GetWindowRect",            u_GetWindowRect,             8, 0},
    {"GetWindowThreadProcessId", u_GetWindowThreadProcessId,  8, 0},
    {"IsIconic",                 u_IsIconic,                  4, 0},
    {"IsZoomed",                 u_IsZoomed,                  4, 0},
    {"LoadAcceleratorsA",        u_LoadAcceleratorsA,         8, 0},
    {"LoadCursorA",              u_LoadCursorA,               8, 0},
    {"LoadIconA",                u_LoadIcon,                  8, 0},
    {"LoadIconW",                u_LoadIcon,                  8, 0},
    {"MoveWindow",               u_MoveWindow,               24, 0},
    {"PeekMessageW",             u_PeekMessageW,             20, 0},
    {"PostQuitMessage",          u_PostQuitMessage,           4, 0},
    {"RegisterClassExA",         u_RegisterClassExA,          4, 0},
    {"RegisterClassExW",         u_RegisterClassExW,          4, 0},
    {"ScreenToClient",           u_ScreenToClient,            8, 0},
    {"SendMessageA",             u_SendMessageA,             16, 0},
    {"SetActiveWindow",          u_SetActiveWindow,           4, 0},
    {"SetClassLongA",            u_SetClassLongA,            12, 0},
    {"SetClipboardData",         u_SetClipboardData,          8, 0},
    {"SetCursor",                u_SetCursor,                 4, 0},
    {"SetCursorPos",             u_SetCursorPos,              8, 0},
    {"SetForegroundWindow",      u_SetForegroundWindow,       4, 0},
    {"SetWindowLongA",           u_SetWindowLongA,           12, 0},
    {"SetWindowPos",             u_SetWindowPos,             28, 0},
    {"SetWindowTextW",           u_SetWindowTextW,            8, 0},
    {"SetWindowsHookExA",        u_SetWindowsHookExA,        16, 0},
    {"ShowCursor",               u_ShowCursor,                4, 0},
    {"ShowWindow",               u_ShowWindow,                8, 0},
    {"SystemParametersInfoA",    u_SystemParametersInfoA,    16, 0},
    {"TranslateMessage",         u_TranslateMessage,          4, 0},
    {"UnhookWindowsHookEx",      u_UnhookWindowsHookEx,       4, 0},
    {"UpdateWindow",             u_UpdateWindow,              4, 0},
    {0},
};

const re5_import_module re5_mod_user32 = {"user32.dll", k_defs};
