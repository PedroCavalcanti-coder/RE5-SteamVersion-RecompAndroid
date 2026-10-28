/*
 * kernel32_env.c - o que o CRT do MSVC 2005 consulta antes do WinMain e o
 * ambiente do processo (Fase 3, etapa 3.5): startup info, linha de comando,
 * variáveis de ambiente, versão do SO, informações do sistema, tempo, pasta
 * corrente, handles padrão e o fim do processo.
 *
 * O guest vê um Windows 7 SP1 32 bits em inglês, instalado em C:\RE5 (o VFS
 * leva esse caminho para a pasta real do jogo).
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "kernel32_internal.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* Núcleos anunciados ao jogo (GetSystemInfo, NUMBER_OF_PROCESSORS); a MT Framework
 * divide os jobs por eles (workers = min(JobThread do config.ini, 8, este número)).
 * Com o lock global (D5) mais núcleos só disputam o mesmo lock: 2. Com
 * RE5_GUEST_LOCK=hle o código do guest roda em paralelo e anunciamos TODOS os processadores
 * lógicos do host (até 32, o que cabe no dwActiveProcessorMask de 32 bits); o teto que a
 * MT Framework impõe, se houver, é o do próprio jogo. RE5_CPUS força um número (1..32). */
static uint32_t guest_cpus(void)
{
    static uint32_t n;
    if (n) return n;
    const char *e = getenv("RE5_CPUS");
    long v = e ? strtol(e, NULL, 10) : 0;
    if (v >= 1 && v <= 32) return n = (uint32_t)v;
    if (!re5_guest_lock_mode_hle()) return n = 2;
#if defined(RE5_HOST_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long host = (long)si.dwNumberOfProcessors;
#else
    long host = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    n = host < 1 ? 1u : host > 32 ? 32u : (uint32_t)host;
    RE5_INFO("kernel32", "GetSystemInfo: %u núcleos para o jogo (host: %ld)", n, host);
    return n;
}

/* ---- tempo: FILETIME = 100 ns desde 1601-01-01 UTC ---- */

#define EPOCH_DIFF_100NS 116444736000000000ull

static uint64_t host_filetime(void)
{
#if defined(RE5_HOST_WINDOWS)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return EPOCH_DIFF_100NS + (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
#endif
}

static uint64_t r64(uint32_t va) { return (uint64_t)k32_r32(va + 4) << 32 | k32_r32(va); }
static void w64(uint32_t va, uint64_t v) { k32_w32(va, (uint32_t)v); k32_w32(va + 4, (uint32_t)(v >> 32)); }

static void k_GetSystemTimeAsFileTime(CPU *c) { w64(re5_arg(c, 0), host_filetime()); }

/* Dias desde 1970-01-01 <-> data civil (algoritmo de Howard Hinnant). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

/* SYSTEMTIME: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds. */
static void k_FileTimeToSystemTime(CPU *c)
{
    uint64_t ft = r64(re5_arg(c, 0));
    uint32_t st = re5_arg(c, 1);
    if (ft >> 63) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = 0; return; }
    int64_t secs = (int64_t)(ft / 10000000ull) - (int64_t)(EPOCH_DIFF_100NS / 10000000ull);
    int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    int64_t rem = secs - days * 86400, y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    k32_w16(st + 0, (uint16_t)y);
    k32_w16(st + 2, (uint16_t)m);
    k32_w16(st + 4, (uint16_t)(((days % 7) + 11) % 7));   /* 1970-01-01 foi quinta (4) */
    k32_w16(st + 6, (uint16_t)d);
    k32_w16(st + 8, (uint16_t)(rem / 3600));
    k32_w16(st + 10, (uint16_t)(rem / 60 % 60));
    k32_w16(st + 12, (uint16_t)(rem % 60));
    k32_w16(st + 14, (uint16_t)(ft / 10000u % 1000u));
    c->eax = 1;
}

static void k_SystemTimeToFileTime(CPU *c)
{
    uint32_t st = re5_arg(c, 0);
    unsigned y = k32_r16(st), m = k32_r16(st + 2), d = k32_r16(st + 6);
    unsigned hh = k32_r16(st + 8), mm = k32_r16(st + 10), ss = k32_r16(st + 12), ms = k32_r16(st + 14);
    if (y < 1601 || y > 30827 || m < 1 || m > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 59 || ms > 999) {
        re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
        c->eax = 0;
        return;
    }
    int64_t secs = days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
    w64(re5_arg(c, 1), (uint64_t)(secs * 10000000 + (int64_t)EPOCH_DIFF_100NS) + ms * 10000ull);
    c->eax = 1;
}

/* O guest vive em UTC: GetTimeZoneInformation diz bias 0, então local == UTC. */
static void k_FileTimeToLocalFileTime(CPU *c) { w64(re5_arg(c, 1), r64(re5_arg(c, 0))); c->eax = 1; }
static void k_LocalFileTimeToFileTime(CPU *c) { w64(re5_arg(c, 1), r64(re5_arg(c, 0))); c->eax = 1; }

static void k_GetTimeZoneInformation(CPU *c)
{
    uint32_t tzi = re5_arg(c, 0);
    memset(re5_g2h(tzi), 0, 172);
    static const char utc[] = "Coordinated Universal Time";
    for (uint32_t i = 0; utc[i]; i++) {
        k32_w16(tzi + 4 + 2 * i, (uint16_t)utc[i]);    /* StandardName */
        k32_w16(tzi + 88 + 2 * i, (uint16_t)utc[i]);   /* DaylightName */
    }
    c->eax = 0;   /* TIME_ZONE_ID_UNKNOWN: sem horário de verão */
}

/* ---- startup e linha de comando ---- */

static void k_GetStartupInfoA(CPU *c)
{
    uint32_t si = re5_arg(c, 0);
    memset(re5_g2h(si), 0, 68);
    k32_w32(si, 68);   /* cb; dwFlags = 0 -> WinMain recebe SW_SHOWDEFAULT */
}

static uint32_t guest_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, va = k32_heap_alloc(k32_process_heap(), 0, n);
    if (!va) re5_fatal("kernel32", "sem memória do guest para \"%s\"", s);
    memcpy(re5_g2h(va), s, n);
    return va;
}

static void k_GetCommandLineA(CPU *c)
{
    static uint32_t cmdline;
    if (!cmdline) {
        const char *extra = getenv("RE5_GUEST_ARGS");   /* argumentos para o jogo, se precisar */
        char buf[1024];
        snprintf(buf, sizeof buf, "\"%s\"%s%s", K32_EXE_PATH, extra && *extra ? " " : "", extra ? extra : "");
        cmdline = guest_strdup(buf);
    }
    c->eax = cmdline;
}

/* ---- variáveis de ambiente: lista no host, blocos montados sob demanda ---- */

#define MAX_ENV 64
static char *g_env[MAX_ENV];
static uint32_t g_nenv;

static void env_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    static const char *const k_initial[] = {
        "ALLUSERSPROFILE=C:\\ProgramData",
        "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
        "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local",
        "OS=Windows_NT",
        "PATH=C:\\Windows\\system32;C:\\Windows",
        "PROCESSOR_ARCHITECTURE=x86",
        "SystemDrive=C:",
        "SystemRoot=C:\\Windows",
        "TEMP=C:\\Users\\Player\\AppData\\Local\\Temp",
        "TMP=C:\\Users\\Player\\AppData\\Local\\Temp",
        "USERNAME=Player",
        "USERPROFILE=C:\\Users\\Player",
        "windir=C:\\Windows",
    };
    for (size_t i = 0; i < sizeof k_initial / sizeof k_initial[0]; i++) g_env[g_nenv++] = strdup(k_initial[i]);
    char cpus[40];
    snprintf(cpus, sizeof cpus, "NUMBER_OF_PROCESSORS=%u", guest_cpus());   /* o mesmo que GetSystemInfo */
    g_env[g_nenv++] = strdup(cpus);
}

static int env_find(const char *name, size_t len)
{
    for (uint32_t i = 0; i < g_nenv; i++) {
        const char *e = g_env[i];
        size_t k = 0;
        while (k < len && e[k] && (e[k] | 32) == (name[k] | 32)) k++;
        if (k == len && e[k] == '=') return (int)i;
    }
    return -1;
}

static void k_SetEnvironmentVariableA(CPU *c)
{
    env_init();
    const char *name = re5_gstr(re5_arg(c, 0)), *value = re5_gstr(re5_arg(c, 1));
    if (!name || !*name || strchr(name, '=')) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = 0; return; }
    int i = env_find(name, strlen(name));
    if (i >= 0) {
        free(g_env[i]);
        g_env[i] = g_env[--g_nenv];
    }
    if (value) {
        if (g_nenv == MAX_ENV) re5_fatal("kernel32", "SetEnvironmentVariableA: mais de %u variáveis", MAX_ENV);
        size_t n = strlen(name) + strlen(value) + 2;
        g_env[g_nenv] = malloc(n);
        snprintf(g_env[g_nenv++], n, "%s=%s", name, value);
    }
    c->eax = 1;
}

/* Bloco "A=1\0B=2\0\0" no heap do processo; Free* devolve ao heap. */
static uint32_t env_block(int wide)
{
    env_init();
    uint32_t units = 1;
    for (uint32_t i = 0; i < g_nenv; i++) units += (uint32_t)strlen(g_env[i]) + 1;
    uint32_t va = k32_heap_alloc(k32_process_heap(), 0, units * (wide ? 2 : 1));
    if (!va) return 0;
    uint32_t p = va;
    for (uint32_t i = 0; i < g_nenv; i++) {
        for (const char *s = g_env[i];; s++) {
            if (wide) { k32_w16(p, (uint8_t)*s); p += 2; } else { k32_w8(p, (uint8_t)*s); p++; }
            if (!*s) break;
        }
    }
    if (wide) k32_w16(p, 0); else k32_w8(p, 0);
    return va;
}

static void k_GetEnvironmentStrings(CPU *c) { c->eax = env_block(0); }
static void k_GetEnvironmentStringsW(CPU *c) { c->eax = env_block(1); }
static void k_FreeEnvironmentStrings(CPU *c) { c->eax = (uint32_t)k32_heap_free(k32_process_heap(), re5_arg(c, 0)); }

/* ---- versão do SO ---- */

static void k_GetVersionExA(CPU *c)
{
    uint32_t v = re5_arg(c, 0), size = k32_r32(v);
    if (size != 148 && size != 156) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = 0; return; }
    memset(re5_g2h(v + 4), 0, size - 4);
    k32_w32(v + 4, K32_OS_MAJOR);
    k32_w32(v + 8, K32_OS_MINOR);
    k32_w32(v + 12, K32_OS_BUILD);
    k32_w32(v + 16, 2);   /* VER_PLATFORM_WIN32_NT */
    memcpy(re5_g2h(v + 20), K32_OS_SP_NAME, sizeof K32_OS_SP_NAME);
    if (size == 156) {
        k32_w16(v + 148, K32_OS_SP_MAJOR);
        k32_w16(v + 152, 0x0100);   /* VER_SUITE_SINGLEUSERTS */
        k32_w8(v + 154, 1);         /* VER_NT_WORKSTATION */
    }
    c->eax = 1;
}

#define K32_VER_EQUAL 1u
#define K32_VER_GREATER 2u
#define K32_VER_GREATER_EQUAL 3u
#define K32_VER_LESS 4u
#define K32_VER_LESS_EQUAL 5u
#define K32_VER_AND 6u
#define K32_VER_OR 7u

static void k_VerSetConditionMask(CPU *c)
{
    uint64_t mask = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    uint32_t type = re5_arg(c, 2), cond = re5_arg(c, 3) & 7u;
    for (uint32_t bit = 0; bit < 8; bit++)
        if (type & (1u << bit)) mask |= (uint64_t)cond << (bit * 3);
    c->eax = (uint32_t)mask;
    c->edx = (uint32_t)(mask >> 32);
}

static int cmp_cond(uint32_t cond, int64_t sys, int64_t req)
{
    switch (cond) {
    case K32_VER_EQUAL: return sys == req;
    case K32_VER_GREATER: return sys > req;
    case K32_VER_GREATER_EQUAL: return sys >= req;
    case K32_VER_LESS: return sys < req;
    case K32_VER_LESS_EQUAL: return sys <= req;
    case K32_VER_AND: return (sys & req) == req;
    case K32_VER_OR: return (sys & req) != 0;
    default: return 0;
    }
}

static void k_VerifyVersionInfoA(CPU *c)
{
    uint32_t v = re5_arg(c, 0), type = re5_arg(c, 1);
    uint64_t mask = (uint64_t)re5_arg(c, 3) << 32 | re5_arg(c, 2);
#define COND(bit) ((uint32_t)(mask >> ((bit) * 3)) & 7u)
    int ok = 1;
    /* major, minor, SP major, SP minor comparam como uma versão só, com a
     * condição do campo mais significativo presente (como o Windows faz). */
    const uint32_t vbits[4] = {1, 0, 5, 4};   /* posições no mask */
    const int64_t sys[4] = {K32_OS_MAJOR, K32_OS_MINOR, K32_OS_SP_MAJOR, 0};
    const int64_t req[4] = {k32_r32(v + 4), k32_r32(v + 8), k32_r16(v + 148), k32_r16(v + 150)};
    int first = -1;
    for (int i = 0; i < 4; i++) if (type & (1u << vbits[i])) { first = i; break; }
    if (first >= 0) {
        uint32_t cond = COND(vbits[first]);
        int order = 0;
        for (int i = first; i < 4 && !order; i++)
            if (type & (1u << vbits[i])) order = sys[i] < req[i] ? -1 : sys[i] > req[i] ? 1 : 0;
        ok &= cmp_cond(cond, order, 0);
    }
    if (type & 0x04u) ok &= cmp_cond(COND(2), K32_OS_BUILD, k32_r32(v + 12));
    if (type & 0x08u) ok &= cmp_cond(COND(3), 2, k32_r32(v + 16));
    if (type & 0x40u) ok &= cmp_cond(COND(6), 0x0100, k32_r16(v + 152));
    if (type & 0x80u) ok &= cmp_cond(COND(7), 1, k32_r8(v + 154));
#undef COND
    c->eax = (uint32_t)ok;
    if (!ok) re5_set_last_error(K32_ERROR_OLD_WIN_VERSION);
}

/* ---- sistema ---- */

static void k_GetSystemInfo(CPU *c)
{
    uint32_t si = re5_arg(c, 0);
    memset(re5_g2h(si), 0, 36);
    k32_w16(si + 0, 0);                         /* PROCESSOR_ARCHITECTURE_INTEL */
    k32_w32(si + 4, RE5_MEM_PAGE);
    k32_w32(si + 8, RE5_MEM_GRANULARITY);       /* lpMinimumApplicationAddress */
    k32_w32(si + 12, 0x7FFEFFFFu);              /* sem LARGE_ADDRESS_AWARE */
    uint32_t cpus = guest_cpus();
    k32_w32(si + 16, cpus >= 32 ? 0xFFFFFFFFu : (1u << cpus) - 1);   /* dwActiveProcessorMask */
    k32_w32(si + 20, cpus);
    k32_w32(si + 24, 586);                      /* PROCESSOR_INTEL_PENTIUM */
    k32_w32(si + 28, RE5_MEM_GRANULARITY);
    k32_w16(si + 32, 6);                        /* wProcessorLevel */
    k32_w16(si + 34, 0x0F0B);                   /* wProcessorRevision */
}

static void k_IsDebuggerPresent(CPU *c) { c->eax = 0; }

static uint32_t g_unhandled_filter;

static void k_SetUnhandledExceptionFilter(CPU *c)
{
    c->eax = g_unhandled_filter;
    g_unhandled_filter = re5_arg(c, 0);
}

static void k_UnhandledExceptionFilter(CPU *c)
{
    uint32_t ptrs = re5_arg(c, 0), rec = ptrs ? k32_r32(ptrs) : 0;
    re5_fatal("kernel32", "exceção não tratada no guest: código 0x%08X em 0x%08X",
              rec ? k32_r32(rec) : 0, rec ? k32_r32(rec + 12) : 0);
}

/* Exceções do guest (SEH, throw do C++ = 0xE06D7363) são a etapa 3.6. Até lá,
 * qualquer RaiseException aborta dizendo o código e quem lançou. */
static void k_RaiseException(CPU *c)
{
    re5_fatal("kernel32", "RaiseException(0x%08X, flags 0x%X, %u args) chamada de 0x%08X: SEH do guest ainda não "
              "implementado (etapa 3.6)", re5_arg(c, 0), re5_arg(c, 1), re5_arg(c, 2), k32_r32(c->esp));
}

static void k_RtlUnwind(CPU *c)
{
    re5_fatal("kernel32", "RtlUnwind(frame 0x%08X, alvo 0x%08X) chamada de 0x%08X: SEH do guest ainda não "
              "implementado (etapa 3.6)", re5_arg(c, 0), re5_arg(c, 1), k32_r32(c->esp));
}

static void k_TerminateProcess(CPU *c)
{
    uint32_t h = re5_arg(c, 0), code = re5_arg(c, 1);
    if (h != RE5_PSEUDO_PROCESS) re5_fatal("kernel32", "TerminateProcess(0x%X): só o próprio processo existe", h);
    RE5_INFO("kernel32", "TerminateProcess(%u)", code);
    fflush(NULL);
    exit((int)code);
}

static void k_FatalAppExitA(CPU *c)
{
    const char *msg = re5_gstr(re5_arg(c, 1));
    re5_fatal("kernel32", "FatalAppExitA: %s", msg ? msg : "(null)");
}

static void k_DebugBreak(CPU *c) { re5_fatal("kernel32", "DebugBreak chamada de 0x%08X", k32_r32(c->esp)); }

/* ---- handles padrão: processo de GUI, sem console ---- */

static uint32_t g_std[3];

static void k_GetStdHandle(CPU *c)
{
    uint32_t n = re5_arg(c, 0);   /* -10, -11, -12 */
    if (n < 0xFFFFFFF4u || n > 0xFFFFFFF6u) { re5_set_last_error(6u); c->eax = 0xFFFFFFFFu; return; }
    c->eax = g_std[0xFFFFFFF6u - n];
}

static void k_SetStdHandle(CPU *c)
{
    uint32_t n = re5_arg(c, 0);
    if (n < 0xFFFFFFF4u || n > 0xFFFFFFF6u) { re5_set_last_error(6u); c->eax = 0; return; }
    g_std[0xFFFFFFF6u - n] = re5_arg(c, 1);
    c->eax = 1;
}

static void k_GetFileType(CPU *c)
{
    switch (re5_handle_type_of(re5_arg(c, 0))) {
    case RE5_H_FILE: c->eax = 1; break;   /* FILE_TYPE_DISK */
    default:
        re5_set_last_error(6u);
        c->eax = 0;                        /* FILE_TYPE_UNKNOWN */
    }
}

static void k_SetHandleCount(CPU *c) { c->eax = re5_arg(c, 0); }

/* ---- console: um processo de janela (subsistema GUI) não tem um ----
 * O CRT pergunta antes de escrever em stderr; a resposta do Windows sem console
 * é falhar com ERROR_INVALID_HANDLE, e o CRT segue pelo caminho de arquivo. */

static void k_NoConsole(CPU *c)
{
    re5_set_last_error(6u);   /* ERROR_INVALID_HANDLE */
    c->eax = 0;
}

static void k_WriteConsole(CPU *c)
{
    uint32_t written = re5_arg(c, 3);
    if (written) k32_w32(written, 0);
    k_NoConsole(c);
}

/* ---- lstr* (sem SEH: ponteiro inválido do guest falha alto no host) ---- */

static void k_lstrlenA(CPU *c)
{
    uint32_t s = re5_arg(c, 0);
    c->eax = s ? (uint32_t)strlen(re5_gstr(s)) : 0;
}

static void k_lstrcpyA(CPU *c)
{
    uint32_t dst = re5_arg(c, 0), src = re5_arg(c, 1);
    if (!dst || !src) { c->eax = 0; return; }
    const char *s = re5_gstr(src);
    memmove(re5_g2h(dst), s, strlen(s) + 1);
    c->eax = dst;
}

/* ---- pasta corrente (caminho do guest) ---- */

static char g_cwd[260] = RE5_VFS_INSTALL_DIR;

static void k_GetCurrentDirectoryA(CPU *c)
{
    uint32_t n = re5_arg(c, 0), buf = re5_arg(c, 1), len = (uint32_t)strlen(g_cwd);
    if (n <= len) { c->eax = len + 1; return; }
    memcpy(re5_g2h(buf), g_cwd, len + 1);
    c->eax = len;
}

static void k_SetCurrentDirectoryA(CPU *c)
{
    const char *p = re5_gstr(re5_arg(c, 0));
    char host[1024];
    struct stat st;
    c->eax = 0;
    if (!p || re5_vfs_resolve(p, host, sizeof host) != 0 || stat(host, &st) != 0 || !S_ISDIR(st.st_mode)) {
        re5_set_last_error(K32_ERROR_FILE_NOT_FOUND);
        return;
    }
    if (p[0] && p[1] == ':') snprintf(g_cwd, sizeof g_cwd, "%s", p);
    else snprintf(g_cwd, sizeof g_cwd, "%s\\%s", RE5_VFS_INSTALL_DIR, p);
    size_t n = strlen(g_cwd);
    if (n > 3 && (g_cwd[n - 1] == '\\' || g_cwd[n - 1] == '/')) g_cwd[n - 1] = 0;
    c->eax = 1;
}

static const re5_import_def k_defs[] = {
    {"DebugBreak",                  k_DebugBreak,                   0, 0},
    {"FatalAppExitA",               k_FatalAppExitA,                8, 0},
    {"FileTimeToLocalFileTime",     k_FileTimeToLocalFileTime,      8, 0},
    {"FileTimeToSystemTime",        k_FileTimeToSystemTime,         8, 0},
    {"FreeEnvironmentStringsA",     k_FreeEnvironmentStrings,       4, 0},
    {"FreeEnvironmentStringsW",     k_FreeEnvironmentStrings,       4, 0},
    {"GetCommandLineA",             k_GetCommandLineA,              0, 0},
    {"GetCurrentDirectoryA",        k_GetCurrentDirectoryA,         8, 0},
    {"GetEnvironmentStrings",       k_GetEnvironmentStrings,        0, 0},
    {"GetEnvironmentStringsW",      k_GetEnvironmentStringsW,       0, 0},
    {"GetConsoleCP",                k_NoConsole,                    0, 0},
    {"GetConsoleMode",              k_NoConsole,                    8, 0},
    {"GetConsoleOutputCP",          k_NoConsole,                    0, 0},
    {"GetFileType",                 k_GetFileType,                  4, 0},
    {"GetStartupInfoA",             k_GetStartupInfoA,              4, 0},
    {"GetStdHandle",                k_GetStdHandle,                 4, 0},
    {"GetSystemInfo",               k_GetSystemInfo,                4, 0},
    {"GetSystemTimeAsFileTime",     k_GetSystemTimeAsFileTime,      4, 0},
    {"GetTimeZoneInformation",      k_GetTimeZoneInformation,       4, 0},
    {"GetVersionExA",               k_GetVersionExA,                4, 0},
    {"IsDebuggerPresent",           k_IsDebuggerPresent,            0, 0},
    {"LocalFileTimeToFileTime",     k_LocalFileTimeToFileTime,      8, 0},
    {"RaiseException",              k_RaiseException,              16, 0},
    {"RtlUnwind",                   k_RtlUnwind,                   16, 0},
    {"SetCurrentDirectoryA",        k_SetCurrentDirectoryA,         4, 0},
    {"SetEnvironmentVariableA",     k_SetEnvironmentVariableA,      8, 0},
    {"SetHandleCount",              k_SetHandleCount,               4, 0},
    {"SetStdHandle",                k_SetStdHandle,                 8, 0},
    {"SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter,  4, 0},
    {"SystemTimeToFileTime",        k_SystemTimeToFileTime,         8, 0},
    {"TerminateProcess",            k_TerminateProcess,             8, 0},
    {"UnhandledExceptionFilter",    k_UnhandledExceptionFilter,     4, 0},
    {"VerifyVersionInfoA",          k_VerifyVersionInfoA,          16, 0},
    {"VerSetConditionMask",         k_VerSetConditionMask,         16, 0},
    {"WriteConsoleA",               k_WriteConsole,                20, 0},
    {"WriteConsoleW",               k_WriteConsole,                20, 0},
    {"lstrcpyA",                    k_lstrcpyA,                     8, 0},
    {"lstrlenA",                    k_lstrlenA,                     4, 0},
    {0},
};

const re5_import_module re5_mod_kernel32_env = {"kernel32.dll", k_defs};
