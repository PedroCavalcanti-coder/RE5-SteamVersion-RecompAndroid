/*
 * HLE do user32 sem janela de verdade (RE5_HEADLESS): geometria (área cliente
 * x retângulo externo, como o RE5 cria a janela de 1280x720), CREATESTRUCT,
 * SetWindowPos com flags, FindWindowW, o caminho de fechamento do Windows
 * (SC_CLOSE -> WM_CLOSE -> DestroyWindow -> WM_DESTROY) e WM_QUIT na fila.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/user32.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;
static uint32_t g_scratch, g_off;

static uint32_t call(const char *name, int n, ...)
{
    uint32_t args[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, re5_import_bind("user32.dll", name, 0), n, args);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static uint32_t gbuf(uint32_t n)
{
    uint32_t va = g_scratch + g_off;
    memset(re5_g2h(va), 0, n);
    g_off = (g_off + n + 15u) & ~15u;
    return va;
}

static uint32_t gstr(const char *s)
{
    uint32_t va = gbuf((uint32_t)strlen(s) + 1);
    memcpy(re5_g2h(va), s, strlen(s) + 1);
    return va;
}

static uint32_t gwstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s), va = gbuf(2 * n + 2);
    for (uint32_t i = 0; i <= n; i++) wr16(va + 2 * i, (uint8_t)s[i]);
    return va;
}

#define MAXLOG 64
static uint32_t g_msgs[MAXLOG], g_lparams[MAXLOG], g_nmsgs;
static uint32_t g_cs_cx, g_cs_cy, g_cs_class;

static int saw(uint32_t msg)
{
    for (uint32_t i = 0; i < g_nmsgs; i++)
        if (g_msgs[i] == msg) return (int)i + 1;
    return 0;
}

/* WndProc do "jogo": registra e repassa para DefWindowProcA. */
static void wndproc(CPU *c)
{
    uint32_t args[4] = {re5_arg(c, 0), re5_arg(c, 1), re5_arg(c, 2), re5_arg(c, 3)};
    if (g_nmsgs < MAXLOG) {
        g_msgs[g_nmsgs] = args[1];
        g_lparams[g_nmsgs++] = args[3];
    }
    if (args[1] == 0x0001u) {   /* WM_CREATE: lê o CREATESTRUCT */
        g_cs_cy = rd32(args[3] + 16);
        g_cs_cx = rd32(args[3] + 20);
        g_cs_class = rd32(args[3] + 40);
    }
    uint32_t esp = c->esp;
    c->eax = re5_guest_call(T, re5_import_bind("user32.dll", "DefWindowProcA", 0), 4, args);
    c->esp = esp;
}

int main(void)
{
#if defined(_WIN32)
    _putenv("RE5_HEADLESS=1");
    _putenv("RE5_SCREEN=1920x1080");
    _putenv("RE5_INPUT_SCRIPT=10:Return,+5:Down");
#else
    setenv("RE5_HEADLESS", "1", 1);
    setenv("RE5_SCREEN", "1920x1080", 1);
    setenv("RE5_INPUT_SCRIPT", "10:Return,+5:Down", 1);
#endif
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");

    CHECK_EQ_U32(call("GetSystemMetrics", 1, 0u), 1920);
    CHECK_EQ_U32(call("GetSystemMetrics", 1, 1u), 1080);

    /* Teclas de aderência: o RE5 lê, desliga o atalho e restaura na saída. */
    uint32_t sk = gbuf(8);
    wr32(sk, 8);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x3Au, 8u, sk, 0u), 1);   /* SPI_GETSTICKYKEYS */
    CHECK_EQ_U32(rd32(sk + 4), 0x1FE);
    wr32(sk + 4, 0x1FE & ~4u);                                             /* sem SKF_HOTKEYACTIVE */
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x3Bu, 8u, sk, 0u), 1);
    wr32(sk + 4, 0);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x3Au, 8u, sk, 0u), 1);
    CHECK_EQ_U32(rd32(sk + 4), 0x1FA);
    wr32(sk, 12);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x3Au, 8u, sk, 0u), 0);   /* cbSize errado */
    uint32_t fk = gbuf(24);
    wr32(fk, 24);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x32u, 24u, fk, 0u), 1);  /* SPI_GETFILTERKEYS */
    CHECK_EQ_U32(rd32(fk + 8), 1000);
    uint32_t trails = gbuf(4);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x5Eu, 0u, trails, 0u), 1);   /* SPI_GETMOUSETRAILS */
    CHECK_EQ_U32(rd32(trails), 0);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x5Du, 3u, 0u, 0u), 1);
    CHECK_EQ_U32(call("SystemParametersInfoA", 4, 0x5Eu, 0u, trails, 0u), 1);
    CHECK_EQ_U32(rd32(trails), 3);

    /* O RE5: WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, cliente 1280x720 -> 1286x748. */
    const uint32_t style = 0x00CA0000u;
    uint32_t rc = gbuf(16);
    wr32(rc + 8, 1280);
    wr32(rc + 12, 720);
    call("AdjustWindowRect", 3, rc, style, 0u);
    int32_t ow = (int32_t)(rd32(rc + 8) - rd32(rc)), oh = (int32_t)(rd32(rc + 12) - rd32(rc + 4));
    CHECK_EQ_U32(ow, 1286);
    CHECK_EQ_U32(oh, 748);

    uint32_t proc = re5_thunk_new("teste!WndProc", wndproc, 16, NULL);
    uint32_t cls = gstr("MTFramework");
    uint32_t wc = gbuf(48);
    wr32(wc, 48);
    wr32(wc + 8, proc);
    wr32(wc + 40, cls);
    CHECK(call("RegisterClassExA", 1, wc) != 0);
    CHECK_EQ_U32(call("RegisterClassExA", 1, wc), 0);   /* já existe */

    uint32_t hwnd = call("CreateWindowExA", 12, 0u, cls, gstr("RESIDENT EVIL 5"), style, 100u, 50u,
                         (uint32_t)ow, (uint32_t)oh, 0u, 0u, 0x00400000u, 0u);
    CHECK(hwnd != 0);
    CHECK(saw(0x0081u) && saw(0x0001u) && saw(0x0081u) < saw(0x0001u));   /* NCCREATE antes de CREATE */
    CHECK_EQ_U32(g_cs_cx, 1286);
    CHECK_EQ_U32(g_cs_cy, 748);
    CHECK_EQ_U32(g_cs_class, cls);

    uint32_t r = gbuf(16);
    CHECK_EQ_U32(call("GetClientRect", 2, hwnd, r), 1);
    CHECK_EQ_U32(rd32(r + 8), 1280);
    CHECK_EQ_U32(rd32(r + 12), 720);
    CHECK_EQ_U32(call("GetWindowRect", 2, hwnd, r), 1);
    CHECK_EQ_U32(rd32(r), 100);
    CHECK_EQ_U32(rd32(r + 8), 1386);
    uint32_t pt = gbuf(8);
    CHECK_EQ_U32(call("ClientToScreen", 2, hwnd, pt), 1);
    CHECK_EQ_U32(rd32(pt), 103);        /* moldura de 3 */
    CHECK_EQ_U32(rd32(pt + 4), 75);     /* 3 + barra de título de 22 */
    CHECK_EQ_U32(call("ScreenToClient", 2, hwnd, pt), 1);
    CHECK_EQ_U32(rd32(pt), 0);

    /* SWP_NOMOVE|SWP_NOSIZE não mexe; só tamanho manda WM_SIZE com o cliente. */
    g_nmsgs = 0;
    call("SetWindowPos", 7, hwnd, 0u, 0u, 0u, 0u, 0u, 0x0003u);
    CHECK_EQ_U32(call("GetClientRect", 2, hwnd, r), 1);
    CHECK_EQ_U32(rd32(r + 8), 1280);
    CHECK(!saw(0x0005u));
    call("SetWindowPos", 7, hwnd, 0u, 0u, 0u, 806u, 628u, 0x0002u);
    int i = saw(0x0005u);
    CHECK(i != 0);
    if (i) CHECK_EQ_U32(g_lparams[i - 1], (600u << 16) | 800u);
    CHECK(!saw(0x0003u));   /* sem WM_MOVE */

    CHECK_EQ_U32(call("FindWindowW", 2, gwstr("mtframework"), 0u), hwnd);
    CHECK_EQ_U32(call("FindWindowW", 2, gwstr("OutraClasse"), 0u), 0);
    CHECK_EQ_U32(call("GetClassLongA", 2, hwnd, (uint32_t)-24), proc);   /* GCL_WNDPROC */
    uint32_t pid = gbuf(4);
    CHECK_EQ_U32(call("GetWindowThreadProcessId", 2, hwnd, pid), T->tid);

    /* ShowWindow ativa e devolve a visibilidade anterior. */
    g_nmsgs = 0;
    CHECK_EQ_U32(call("ShowWindow", 2, hwnd, 1u), 0);
    CHECK(saw(0x0018u) && saw(0x0006u));   /* WM_SHOWWINDOW, WM_ACTIVATE */
    CHECK_EQ_U32(call("GetForegroundWindow", 0), hwnd);
    CHECK_EQ_U32(call("ShowWindow", 2, hwnd, 1u), 1);

    /* Fila: WM_QUIT só depois de PostQuitMessage, e uma vez só. */
    uint32_t msg = gbuf(28);
    CHECK_EQ_U32(call("PeekMessageW", 5, msg, 0u, 0u, 0u, 1u), 0);
    call("PostQuitMessage", 1, 7u);
    CHECK_EQ_U32(call("PeekMessageW", 5, msg, 0u, 0u, 0u, 0u), 1);   /* PM_NOREMOVE */
    CHECK_EQ_U32(call("GetMessageW", 4, msg, 0u, 0u, 0u), 0);
    CHECK_EQ_U32(rd32(msg + 4), 0x0012u);
    CHECK_EQ_U32(rd32(msg + 8), 7);
    CHECK_EQ_U32(call("PeekMessageW", 5, msg, 0u, 0u, 0u, 1u), 0);

    /* Fechar: SC_CLOSE -> WM_CLOSE -> DestroyWindow (WM_DESTROY, WM_NCDESTROY). */
    g_nmsgs = 0;
    call("SendMessageA", 4, hwnd, 0x0112u, 0xF060u, 0u);
    CHECK(saw(0x0010u) && saw(0x0002u) && saw(0x0082u));
    CHECK(saw(0x0010u) < saw(0x0002u));
    CHECK_EQ_U32(call("GetClientRect", 2, hwnd, r), 0);
    CHECK_EQ_U32(call("FindWindowW", 2, gwstr("MTFramework"), 0u), 0);

    /* RE5_INPUT_SCRIPT: Return no Present 10 por 6 Presents, Down 5 depois (espera o Return soltar). */
    uint8_t dik[256];
    re5_user32_script_tick(9);
    re5_user32_dik_state(dik);
    CHECK_EQ_U32(dik[0x1C], 0);
    re5_user32_script_tick(10);
    re5_user32_dik_state(dik);
    CHECK_EQ_U32(dik[0x1C], 0x80);   /* DIK_RETURN */
    re5_user32_script_tick(15);
    re5_user32_dik_state(dik);
    CHECK_EQ_U32(dik[0xD0], 0);      /* DIK_DOWN: ainda segurando o Return */
    re5_user32_script_tick(16);
    re5_user32_dik_state(dik);
    CHECK_EQ_U32(dik[0x1C], 0);
    CHECK_EQ_U32(dik[0xD0], 0x80);
    re5_user32_script_tick(22);
    re5_user32_dik_state(dik);
    CHECK_EQ_U32(dik[0xD0], 0);

    /* Cursos e Mouse: LoadCursorA (101, 102, 104), SetCursor, ShowCursor, ClipCursor, GetCursorPos */
    uint32_t c101 = call("LoadCursorA", 2, 0x00400000u, 101u);
    uint32_t c102 = call("LoadCursorA", 2, 0x00400000u, 102u);
    uint32_t c104 = call("LoadCursorA", 2, 0x00400000u, 104u);
    CHECK_EQ_U32(c101, 0x1D000065u);
    CHECK_EQ_U32(c102, 0x1D000066u);
    CHECK_EQ_U32(c104, 0x1D000068u);

    /* SetCursor troca e devolve o anterior */
    uint32_t prev_cur = call("SetCursor", 1, c101);
    CHECK(prev_cur != 0);
    CHECK_EQ_U32(call("SetCursor", 1, c102), c101);
    CHECK_EQ_U32(call("SetCursor", 1, c104), c102);

    /* ShowCursor incrementa/decrementa contador */
    int32_t sc = (int32_t)call("ShowCursor", 1, 0u);   /* decrementa */
    CHECK(sc < 0);
    sc = (int32_t)call("ShowCursor", 1, 1u);          /* incrementa */
    CHECK(sc >= 0);

    /* SetCursorPos e GetCursorPos */
    call("SetCursorPos", 2, 250u, 150u);
    uint32_t pt_cur = gbuf(8);
    CHECK_EQ_U32(call("GetCursorPos", 1, pt_cur), 1);
    CHECK_EQ_U32(rd32(pt_cur), 250);
    CHECK_EQ_U32(rd32(pt_cur + 4), 150);

    /* Teste de tela cheia e escala de cursor (ex: monitor 1920x1080 com render 1280x720) */
    uint32_t hwnd_fs = call("CreateWindowExA", 12, 0u, cls, gstr("RE5 FS"), 0x80880000u, 0u, 0u,
                            1920u, 1080u, 0u, 0u, 0x00400000u, 0u);
    CHECK(hwnd_fs != 0);
    re5_user32_set_fullscreen(hwnd_fs, 1, 1280u, 720u);
    call("SetCursorPos", 2, 640u, 360u); /* ponto central do render */
    CHECK_EQ_U32(call("GetCursorPos", 1, pt_cur), 1);
    CHECK_EQ_U32(rd32(pt_cur), 640);
    CHECK_EQ_U32(rd32(pt_cur + 4), 360);
    re5_user32_set_fullscreen(hwnd_fs, 0, 1280u, 720u);
    call("DestroyWindow", 1, hwnd_fs);

    /* ClipCursor: NULL solta, rect prende */
    CHECK_EQ_U32(call("ClipCursor", 1, 0u), 1);
    uint32_t clip_rc = gbuf(16);
    wr32(clip_rc + 8, 100);
    wr32(clip_rc + 12, 100);
    CHECK_EQ_U32(call("ClipCursor", 1, clip_rc), 1);
    CHECK_EQ_U32(call("ClipCursor", 1, 0u), 1);

    CHECK_DONE();
}
