/*
 * kernel32.c - HLE do kernel32.dll (Fase 3).
 *
 * Padrão de todo módulo do HLE:
 *   - um handler `static void k_<Nome>(CPU *c)` por função; argumentos com
 *     re5_arg(c, i), resultado em c->eax;
 *   - a tabela no fim com o nome exato do export e os bytes de argumento do
 *     stdcall (4 x nº de argumentos; confira com tools/pcrecomp/tools/pe/
 *     stdcall_argc.py). Quem limpa a pilha é o runtime, não o handler;
 *   - handler que bloqueia solta o lock do guest (RE5_BLOCKING_BEGIN/END).
 *
 * Só o mínimo para o loader e os testes estão aqui. A lista do que falta é o
 * censo: `re5 --census --exe <clean.exe>` (docs/PLANO_DO_PROJETO.md, Fase 3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/runtime.h"
#include "kernel32_internal.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
static uint64_t host_ns(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
static void host_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <time.h>
static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void host_sleep_ms(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif

#define K32_ERROR_MOD_NOT_FOUND  126u
#define K32_ERROR_PROC_NOT_FOUND 127u
#define QPC_FREQUENCY        10000000ull   /* 10 MHz, como o Windows 10+ */

static void w32(uint32_t va, uint32_t v) { memcpy(re5_g2h(va), &v, 4); }

static int stricmp_ascii(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

/* ---- módulos: HMODULEs falsos para as DLLs que o HLE conhece ---- */

#define MAX_MODULES 64
#define MODULE_HANDLE_BASE 0x7F000000u
static char g_modules[MAX_MODULES][40];
static uint32_t g_nmodules;

/* "C:/Windows/System32/USER32" -> "user32.dll": sem pasta, minúsculas, com extensão. */
static void module_norm(const char *in, char *out, size_t cap)
{
    const char *base = in;
    for (const char *p = in; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    size_t k = 0;
    for (; base[k] && k + 1 < cap; k++) out[k] = (char)(base[k] >= 'A' && base[k] <= 'Z' ? base[k] + 32 : base[k]);
    out[k] = 0;
    if (!strchr(out, '.') && k + 4 < cap) memcpy(out + k, ".dll", 5);
}

/* GetModuleHandle acha as DLLs da tabela de imports (o loader do Windows já as
 * carregou) e as que um LoadLibrary anterior abriu; LoadLibrary abre as que o
 * HLE conhece. */
static uint32_t module_handle(const char *name, int load)
{
    const re5_image *img = re5_image_main();
    if (!name) return img ? img->base : 0;
    char norm[40];
    module_norm(name, norm, sizeof norm);
    for (uint32_t i = 0; i < g_nmodules; i++)
        if (stricmp_ascii(g_modules[i], norm) == 0) return MODULE_HANDLE_BASE + i * 0x10000u;
    int present = re5_import_dll_bound(norm) || (load && re5_import_module_known(norm));
    if (!present || g_nmodules >= MAX_MODULES) return 0;
    snprintf(g_modules[g_nmodules], sizeof g_modules[0], "%s", norm);
    return MODULE_HANDLE_BASE + g_nmodules++ * 0x10000u;
}

static const char *module_name(uint32_t h)
{
    if (h < MODULE_HANDLE_BASE || (h - MODULE_HANDLE_BASE) % 0x10000u) return NULL;
    uint32_t i = (h - MODULE_HANDLE_BASE) / 0x10000u;
    return i < g_nmodules ? g_modules[i] : NULL;
}

/* ---- handlers ---- */

uint32_t k32_tick_ms(void) { return (uint32_t)(host_ns() / 1000000ull); }

static void k_GetTickCount(CPU *c) { c->eax = re5_det_is_enabled() ? re5_det_tick() : k32_tick_ms(); }
static void k_GetCurrentProcessId(CPU *c) { c->eax = RE5_GUEST_PID; }
static void k_GetCurrentProcess(CPU *c) { c->eax = RE5_PSEUDO_PROCESS; }
static void k_GetCurrentThread(CPU *c) { c->eax = RE5_PSEUDO_THREAD; }
static void k_GetLastError(CPU *c) { c->eax = re5_get_last_error(); }
static void k_SetLastError(CPU *c) { re5_set_last_error(re5_arg(c, 0)); }

static void k_GetCurrentThreadId(CPU *c)
{
    re5_thread *t = re5_thread_current();
    c->eax = t ? t->tid : 0;
}

static void k_Sleep(CPU *c)
{
    uint32_t ms = re5_arg(c, 0);
    RE5_BLOCKING_BEGIN();
    host_sleep_ms(ms);
    RE5_BLOCKING_END();
}

static void k_ExitProcess(CPU *c)
{
    uint32_t code = re5_arg(c, 0);
    uint32_t ret = rd32(c->esp);
    const char *nm = re5_name_lookup(ret);
    RE5_INFO("kernel32", "ExitProcess(%u) chamado de 0x%08X%s%s", code, ret, nm ? " " : "", nm ? nm : "");
    re5_log_process_cpu();
    re5_dispatch_trail_print();
    fflush(NULL);
    exit((int)code);
}

static void k_QueryPerformanceFrequency(CPU *c)
{
    uint32_t p = re5_arg(c, 0);
    w32(p, (uint32_t)QPC_FREQUENCY);
    w32(p + 4, 0);
    c->eax = 1;
}

static void k_QueryPerformanceCounter(CPU *c)
{
    uint64_t t = re5_det_is_enabled() ? re5_det_qpc() : (host_ns() / (1000000000ull / QPC_FREQUENCY));
    uint32_t p = re5_arg(c, 0);
    w32(p, (uint32_t)t);
    w32(p + 4, (uint32_t)(t >> 32));
    c->eax = 1;
}

static void k_OutputDebugStringA(CPU *c)
{
    const char *s = re5_gstr(re5_arg(c, 0));
    if (s) RE5_DEBUG("guest", "%s", s);
}

static void k_GetModuleHandleA(CPU *c)
{
    c->eax = module_handle(re5_gstr(re5_arg(c, 0)), 0);
    if (!c->eax) re5_set_last_error(K32_ERROR_MOD_NOT_FOUND);
}

static void k_LoadLibraryA(CPU *c)
{
    const char *name = re5_gstr(re5_arg(c, 0));
    c->eax = name ? module_handle(name, 1) : 0;
    if (!c->eax) {
        RE5_WARN("kernel32", "LoadLibraryA(\"%s\"): DLL sem HLE", name ? name : "(null)");
        re5_set_last_error(K32_ERROR_MOD_NOT_FOUND);
    }
}

static void k_GetProcAddress(CPU *c)
{
    const char *dll = module_name(re5_arg(c, 0));
    uint32_t p = re5_arg(c, 1);
    c->eax = 0;
    if (dll) c->eax = p < 0x10000u ? re5_import_lookup(dll, NULL, (uint16_t)p) : re5_import_lookup(dll, re5_gstr(p), 0);
    if (!c->eax) {
        /* O CRT pergunta por EncodePointer a cada ponteiro que guarda: só a
         * primeira vez de cada nome vai para o log em nível INFO. */
        static char seen[128][48];
        static uint32_t nseen;
        char what[48];
        if (p < 0x10000u) snprintf(what, sizeof what, "#%u", p);
        else snprintf(what, sizeof what, "%s", re5_gstr(p));
        int first = 1;
        for (uint32_t i = 0; i < nseen && first; i++) first = strcmp(seen[i], what) != 0;
        if (first && nseen < 128) snprintf(seen[nseen++], sizeof seen[0], "%s", what);
        if (first) RE5_INFO("kernel32", "GetProcAddress(%s, \"%s\") -> NULL", dll ? dll : "?", what);
        else RE5_DEBUG("kernel32", "GetProcAddress(%s, \"%s\") -> NULL", dll ? dll : "?", what);
        re5_set_last_error(K32_ERROR_PROC_NOT_FOUND);
    }
}

static void k_FreeLibrary(CPU *c) { c->eax = module_name(re5_arg(c, 0)) ? 1u : 0u; }

/* Vista+: trunca, termina com NUL e devolve nSize com ERROR_INSUFFICIENT_BUFFER. */
static void k_GetModuleFileNameA(CPU *c)
{
    const re5_image *img = re5_image_main();
    uint32_t h = re5_arg(c, 0), buf = re5_arg(c, 1), n = re5_arg(c, 2);
    char path[300];
    const char *dll = module_name(h);
    if (!h || (img && h == img->base)) snprintf(path, sizeof path, "%s\\re5dx9.exe", RE5_VFS_INSTALL_DIR);
    else if (dll) snprintf(path, sizeof path, "C:\\Windows\\system32\\%s", dll);
    else {
        re5_set_last_error(K32_ERROR_MOD_NOT_FOUND);
        c->eax = 0;
        return;
    }
    uint32_t len = (uint32_t)strlen(path);
    if (!n) { re5_set_last_error(122u); c->eax = 0; return; }
    if (len >= n) {
        memcpy(re5_g2h(buf), path, n - 1);
        *(char *)re5_g2h(buf + n - 1) = 0;
        re5_set_last_error(122u);   /* ERROR_INSUFFICIENT_BUFFER */
        c->eax = n;
        return;
    }
    memcpy(re5_g2h(buf), path, len + 1);
    c->eax = len;
}

static void k_CloseHandle(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    if (h == RE5_PSEUDO_PROCESS || h == RE5_PSEUDO_THREAD) { c->eax = 1; return; }
    c->eax = re5_handle_close(h) == 0;
    if (!c->eax) re5_set_last_error(6u);   /* ERROR_INVALID_HANDLE */
}

static const re5_import_def k_defs[] = {
    {"CloseHandle",               k_CloseHandle,               4, 0},
    {"ExitProcess",               k_ExitProcess,               4, 0},
    {"FreeLibrary",               k_FreeLibrary,               4, 0},
    {"GetCurrentProcess",         k_GetCurrentProcess,         0, 0, RE5_THUNK_NOLOCK},
    {"GetCurrentProcessId",       k_GetCurrentProcessId,       0, 0, RE5_THUNK_NOLOCK},
    {"GetCurrentThread",          k_GetCurrentThread,          0, 0, RE5_THUNK_NOLOCK},
    {"GetCurrentThreadId",        k_GetCurrentThreadId,        0, 0, RE5_THUNK_NOLOCK},
    {"GetLastError",              k_GetLastError,              0, 0, RE5_THUNK_NOLOCK},
    {"GetModuleFileNameA",        k_GetModuleFileNameA,       12, 0},
    {"GetModuleHandleA",          k_GetModuleHandleA,          4, 0},
    {"GetProcAddress",            k_GetProcAddress,            8, 0},
    {"GetTickCount",              k_GetTickCount,              0, 0, RE5_THUNK_NOLOCK},
    {"LoadLibraryA",              k_LoadLibraryA,              4, 0},
    {"OutputDebugStringA",        k_OutputDebugStringA,        4, 0},
    {"QueryPerformanceCounter",   k_QueryPerformanceCounter,   4, 0, RE5_THUNK_NOLOCK},
    {"QueryPerformanceFrequency", k_QueryPerformanceFrequency, 4, 0, RE5_THUNK_NOLOCK},
    {"SetLastError",              k_SetLastError,              4, 0, RE5_THUNK_NOLOCK},
    {"Sleep",                     k_Sleep,                     4, 0, RE5_THUNK_NOLOCK},   /* só dorme, sem o lock */
    {0},
};

const re5_import_module re5_mod_kernel32 = {"kernel32.dll", k_defs};
