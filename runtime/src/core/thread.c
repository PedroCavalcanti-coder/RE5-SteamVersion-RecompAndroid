/*
 * thread.c - threads do guest, TEB/PEB e lock do guest (ver re5/thread.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/d3d9_bridge.h"
#include "re5/fast_tls.h"
#include "re5/guest_memory.h"
#include "re5/log.h"
#include "re5/runtime.h"
#include "re5/stats.h"
#include "re5/thread.h"
#include "re5/thunks.h"
#include "host_sync.h"

#if !defined(RE5_HOST_WINDOWS)
#include <pthread.h>
#endif

static uint32_t g_peb;
static uint32_t g_next_tid = 0x1004;
static re5_mutex g_tid_mu, g_susp_mu;
static re5_cond g_susp_cv;
static re5_mutex g_lk;      /* fila do lock do guest (ver re5_guest_lock) */
static re5_cond g_lk_cv;
static re5_mutex g_hle_mu;   /* lock do HLE no modo RE5_GUEST_LOCK=hle: protege g_hle_held */
static re5_cond g_hle_cv;
static int g_hle_held, g_hle_waiting;
static int g_ready;
RE5_TLS_PTR(re5_thread *, t_self)   /* re5/fast_tls.h: no MinGW, sem o __emutls_get_address */

#if defined(_WIN32) && defined(__GNUC__) && defined(__x86_64__)
uint32_t re5_tls_alloc_slow(volatile uint32_t *slot)
{
    DWORD i = TlsAlloc();
    if (i == TLS_OUT_OF_INDEXES) re5_fatal("thread", "TlsAlloc falhou: sem slot de TLS do Windows");
    uint32_t want = (uint32_t)i + 1u;
    uint32_t prev = (uint32_t)InterlockedCompareExchange((volatile LONG *)slot, (LONG)want, 0);
    if (prev) {   /* outra thread alocou primeiro */
        TlsFree(i);
        return prev;
    }
    return want;
}
#endif

/* Threads do guest vivas, para o watchdog. */
#define MAX_THREADS 256
static re5_thread *g_threads[MAX_THREADS];

int re5_stats_on;
static uint64_t g_stat_host[RE5_ST_N];      /* threads do host sem re5_thread (atômico) */
static uint64_t g_stat_retired[RE5_ST_N];   /* threads do guest que já saíram (sob g_tid_mu) */

void re5_stat_add_thread(int which, uint64_t n)
{
    re5_thread *t = t_self_get();
    if (t) t->stats[which] += n;
    else __atomic_fetch_add(&g_stat_host[which], n, __ATOMIC_RELAXED);
}

uint64_t re5_stat_total(int which)
{
    uint64_t n = __atomic_load_n(&g_stat_host[which], __ATOMIC_RELAXED) + g_stat_retired[which];
    for (int i = 0; i < MAX_THREADS; i++) {
        re5_thread *t = g_threads[i];
        if (t) n += t->stats[which];   /* leitura sem trava: ordem de grandeza */
    }
    return n;
}

uint64_t re5_stat_dispatches(void)
{
    uint64_t n = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        re5_thread *t = g_threads[i];
        if (t && t->trail_pos) n += *t->trail_pos;
    }
    return n;
}

static void ensure_init(void)
{
    if (!g_ready) {
        re5_mutex_init(&g_tid_mu);
        re5_mutex_init(&g_lk);
        re5_cond_init(&g_lk_cv);
        re5_mutex_init(&g_susp_mu);
        re5_cond_init(&g_susp_cv);
        re5_mutex_init(&g_hle_mu);
        re5_cond_init(&g_hle_cv);
        g_ready = 1;
    }
}

static void w32(uint32_t va, uint32_t v) { memcpy(re5_g2h(va), &v, 4); }
static uint32_t r32(uint32_t va) { uint32_t v; memcpy(&v, re5_g2h(va), 4); return v; }

int re5_process_init(uint32_t image_base)
{
    ensure_init();
    if (g_peb) return 0;
    g_peb = re5_mem_alloc(RE5_PEB_SIZE, RE5_PAGE_READWRITE, "peb");
    if (!g_peb) return -1;
    w32(g_peb + RE5_PEB_IMAGE_BASE, image_base);
    /* BeingDebugged = 0, NtGlobalFlag = 0: a memória já vem zerada. ProcessHeap
     * é preenchido pelo HLE do kernel32 quando o heap do processo existir. */
    return 0;
}

uint32_t re5_process_peb(void) { return g_peb; }

/* MXCSR inicial de uma thread do guest. O Windows dá 0x1F80 (sem FTZ/DAZ), mas
 * as consultas e o solver do Havok geram subnormais em contatos quase tangentes
 * (degraus, cantos) e cada operação SSE com subnormal custa ~100 ciclos no host.
 * FTZ (bit 15) + DAZ (bit 6) os zeram; cpu_ldmxcsr leva os dois ao SSE (ou ao
 * FPCR.FZ no ARM64) da thread que chama, que é a do guest. RE5_FTZ=0 volta ao
 * padrão do Windows, para comparar. */
uint32_t re5_guest_mxcsr_default(void)
{
    const char *e = getenv("RE5_FTZ");
    return e && *e == '0' ? 0x1F80u : 0x9FC0u;
}

re5_thread *re5_thread_attach(uint32_t stack_bytes)
{
    ensure_init();
    if (t_self_get()) return t_self_get();
    re5_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->stack_lo = re5_mem_alloc(stack_bytes, RE5_PAGE_READWRITE, "pilha");
    t->teb = re5_mem_alloc(RE5_TEB_SIZE, RE5_PAGE_READWRITE, "teb");
    if (!t->stack_lo || !t->teb) {
        if (t->stack_lo) re5_mem_free(t->stack_lo);
        if (t->teb) re5_mem_free(t->teb);
        free(t);
        return NULL;
    }
    t->stack_hi = t->stack_lo + ((stack_bytes + RE5_MEM_GRANULARITY - 1) & ~(RE5_MEM_GRANULARITY - 1));
    re5_mutex_lock(&g_tid_mu);
    t->tid = g_next_tid;
    g_next_tid += 4;
    re5_mutex_unlock(&g_tid_mu);

    w32(t->teb + RE5_TEB_SEH, 0xFFFFFFFFu);          /* fim da cadeia SEH */
    w32(t->teb + RE5_TEB_STACK_HI, t->stack_hi);
    w32(t->teb + RE5_TEB_STACK_LO, t->stack_lo);
    w32(t->teb + RE5_TEB_SELF, t->teb);
    w32(t->teb + RE5_TEB_PID, RE5_GUEST_PID);
    w32(t->teb + RE5_TEB_TID, t->tid);
    w32(t->teb + RE5_TEB_PEB, g_peb);

    t->cpu.esp = t->stack_hi - 16;   /* folga para quem lê acima do topo */
    t->cpu.fs_base = t->teb;
    t->cpu.fs = 0x3B;
    t->cpu.cs = 0x23;
    t->cpu.ds = t->cpu.es = t->cpu.ss = 0x2B;
    t->cpu.fpu_cw = 0x027F;          /* o do Windows numa thread nova: RC = mais próximo, 53 bits */
    cpu_ldmxcsr(&t->cpu, re5_guest_mxcsr_default());   /* exceções mascaradas, FTZ/DAZ */
    re5_dispatch_trail_ref(&t->trail_pos, &t->trail);
#if defined(RE5_HOST_WINDOWS)
    HANDLE h = NULL;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, 0))
        t->host = h;
#endif
    re5_mutex_lock(&g_tid_mu);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i]) {
            g_threads[i] = t;
            break;
        }
    }
    re5_mutex_unlock(&g_tid_mu);
    t_self_set(t);
    RE5_DEBUG("thread", "tid 0x%X: pilha 0x%08X-0x%08X, teb 0x%08X", t->tid, t->stack_lo, t->stack_hi, t->teb);
    return t;
}

void re5_thread_detach(void)
{
    re5_thread *t = t_self_get();
    if (!t) return;
    re5_mutex_lock(&g_tid_mu);
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i] == t) g_threads[i] = NULL;
    for (int w = 0; w < RE5_ST_N; w++) g_stat_retired[w] += t->stats[w];
    re5_mutex_unlock(&g_tid_mu);
#if defined(RE5_HOST_WINDOWS)
    if (t->host) CloseHandle((HANDLE)t->host);
#endif
    re5_mem_free(t->stack_lo);
    re5_mem_free(t->teb);
    free(t);
    t_self_set(NULL);
}

re5_thread *re5_thread_current(void) { return t_self_get(); }

uint32_t re5_guest_call(re5_thread *t, uint32_t va, int argc, const uint32_t *argv)
{
    CPU *c = &t->cpu;
    uint32_t saved_esp = c->esp;
    for (int i = argc - 1; i >= 0; i--) push32(c, argv[i]);
    push32(c, RE5_RETURN_TO_HOST);
    /* RE5_GUEST_LOCK=hle: código do guest chamado de dentro de um handler (WndProc,
     * IStream, início de thread...) roda sem o lock, como qualquer outro. */
    int depth = re5_guest_lock_mode_hle() && t == t_self_get() ? re5_guest_lock_release_all() : 0;
    dispatch(c, va);
    if (depth) re5_guest_lock_reacquire(depth);
    c->esp = saved_esp;
    return c->eax;
}

void re5_set_last_error(uint32_t err)
{
    if (t_self_get()) w32(t_self_get()->teb + RE5_TEB_LAST_ERROR, err);
}

uint32_t re5_get_last_error(void) { return t_self_get() ? r32(t_self_get()->teb + RE5_TEB_LAST_ERROR) : 0; }

/* ---- lock do guest ---- */

/* Suspensa (SuspendThread), a thread espera aqui sem o lock do guest. */
static void wait_resumed(re5_thread *t)
{
    re5_mutex_lock(&g_susp_mu);
    while (t->suspend > 0) re5_cond_wait_ms(&g_susp_cv, &g_susp_mu, 0xFFFFFFFFu);
    re5_mutex_unlock(&g_susp_mu);
}

/* O lock do guest é uma fila por senha (FIFO). Um CRITICAL_SECTION não é
 * justo: medido no RE5 com o watchdog, uma thread num laço de espera do jogo
 * (EnterCriticalSection/LeaveCriticalSection sem nunca bloquear) ficava com o
 * lock por minutos e todas as outras - a principal, dona da janela, inclusive -
 * paravam ("não respondendo"). Com a fila, quem solta o lock vai para o fim. */
static uint64_t g_next_ticket, g_serving;
static volatile int g_waiting;          /* threads na fila, sem a vez */
static volatile uint64_t g_turn_ns;     /* quando a dona atual pegou a vez */
RE5_TLS_PTR(int *, t_anon_depth)   /* threads do host sem re5_thread */
static re5_thread *volatile g_holder;   /* quem tem o lock agora (RE5_PROFILE: quem faz os outros esperarem) */
typedef void (*pump_fn)(void);
RE5_TLS_PTR(pump_fn, t_pump)   /* re5_thread_set_lock_wait_pump */

#define QUANTUM_NS 2000000ull   /* 2 ms de código do guest antes de dar a vez a quem espera */

/* Modo do lock (RE5_GUEST_LOCK, config runtime.guest_lock):
 *   global  o lock serializa todo o código do guest (fila justa + preempção);
 *   hle     o código do guest roda em paralelo e o lock protege só os handlers
 *           do HLE: re5_thunk_invoke o pega, re5_guest_call o solta. Seguro num
 *           host x86-64 (TSO no hardware) porque o RE5 não tem atômicos em linha:
 *           toda a sincronização dele passa por imports do kernel32.
 * Padrão: hle no x86-64; global nos outros (ARM64 tem memória fraca: etapa 6.2). */
static int g_mode = -1;

int re5_guest_lock_mode_hle(void)
{
    if (g_mode < 0) {
        ensure_init();
        const char *e = getenv("RE5_GUEST_LOCK");
#if defined(__x86_64__) || defined(_M_X64)
        int hle = !e || strcmp(e, "hle") == 0;
#else
        int hle = e && strcmp(e, "hle") == 0;
#endif
        if (e && !hle && strcmp(e, "global") != 0) re5_fatal("thread", "RE5_GUEST_LOCK=%s: use global ou hle", e);
        g_mode = hle;
        RE5_INFO("thread", "lock do guest: %s", hle ? "hle (código do guest em paralelo)" : "global (serializado)");
    }
    return g_mode;
}

/* Modo hle: uma flag com mutex e variável de condição do host (as posses são
 * curtas). Quem espera acorda quando o lock é solto; a thread da janela não
 * pode parar sem tratar mensagens e acorda também a cada 10 ms para bombear.
 * Antes ela tentava, cedia e dormia 1 ms em laço: medido no demo do jogo
 * (RE5_PROFILE), 7% das amostras eram threads esperando com o lock livre. */
static void hle_take(void)
{
    re5_mutex_lock(&g_hle_mu);
    if (g_hle_held) {
        g_hle_waiting++;
        while (g_hle_held) {
            if (!t_pump_get()) {
                re5_cond_wait_ms(&g_hle_cv, &g_hle_mu, 0xFFFFFFFFu);
            } else if (re5_cond_wait_ms(&g_hle_cv, &g_hle_mu, 10) && g_hle_held) {
                re5_mutex_unlock(&g_hle_mu);   /* o bombeamento roda sem o mutex */
                t_pump_get()();
                re5_mutex_lock(&g_hle_mu);
            }
        }
        g_hle_waiting--;
    }
    g_hle_held = 1;
    re5_mutex_unlock(&g_hle_mu);
}

static void hle_give(void)
{
    re5_mutex_lock(&g_hle_mu);
    g_hle_held = 0;
    if (g_hle_waiting) re5_cond_signal(&g_hle_cv);
    re5_mutex_unlock(&g_hle_mu);
}

#if defined(RE5_HOST_WINDOWS)
static uint64_t now_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static RE5_NOINLINE int *anon_depth_new(void)
{
    int *p = calloc(1, sizeof *p);
    if (!p) re5_fatal("thread", "sem memória");
    t_anon_depth_set(p);
    return p;
}

static int *depth_of(re5_thread *t)
{
    if (t) return &t->lock_depth;
    int *p = t_anon_depth_get();
    return p ? p : anon_depth_new();
}

static void take_turn(void)
{
    re5_mutex_lock(&g_lk);
    uint64_t my = g_next_ticket++;
    g_waiting++;
    while (g_serving != my) {
        if (!t_pump_get()) {
            re5_cond_wait_ms(&g_lk_cv, &g_lk, 0xFFFFFFFFu);
            continue;
        }
        re5_cond_wait_ms(&g_lk_cv, &g_lk, 10);
        if (g_serving == my) break;
        re5_mutex_unlock(&g_lk);   /* a fila não espera o bombeamento */
        t_pump_get()();
        re5_mutex_lock(&g_lk);
    }
    g_waiting--;
    g_turn_ns = now_ns();
    re5_mutex_unlock(&g_lk);
}

static void give_turn(void)
{
    re5_mutex_lock(&g_lk);
    g_serving++;
    re5_cond_broadcast(&g_lk_cv);
    re5_mutex_unlock(&g_lk);
}

void re5_guest_lock(void)
{
    ensure_init();
    re5_thread *t = t_self_get();
    int *depth = depth_of(t);
    if (*depth > 0) {   /* reentrante: guest -> host -> guest */
        (*depth)++;
        return;
    }
    int hle = re5_guest_lock_mode_hle();
    uint64_t wait0 = re5_stats_on ? now_ns() : 0;
    if (t) t->lock_wait = 1;
    for (;;) {
        if (t) wait_resumed(t);
        if (hle) hle_take();
        else take_turn();
        /* Ponto de suspensão: suspensa enquanto esperava (quem suspendeu tinha o
         * lock), passa a vez adiante e espera o ResumeThread. */
        if (!t || t->suspend == 0) break;
        if (hle) hle_give();
        else give_turn();
    }
    if (t) t->lock_wait = 0;
    if (wait0) RE5_STAT_ADD(RE5_ST_LOCKWAIT_NS, now_ns() - wait0);
    *depth = 1;
    g_holder = t;
}

/* RE5_THREAD_PRIO=1 (fase P7): SetThreadPriority do guest vale na thread do host, para a de
 * render/principal passar à frente dos workers. Só Windows; nos outros hosts não faz nada. */
void re5_thread_apply_priority(uint32_t tid, int prio)
{
#if defined(RE5_HOST_WINDOWS)
    static int on = -1;
    if (on < 0) on = getenv("RE5_THREAD_PRIO") && *getenv("RE5_THREAD_PRIO") && *getenv("RE5_THREAD_PRIO") != '0';
    if (!on) return;
    if (prio < THREAD_PRIORITY_LOWEST && prio != THREAD_PRIORITY_IDLE) prio = THREAD_PRIORITY_LOWEST;
    if (prio > THREAD_PRIORITY_HIGHEST && prio != THREAD_PRIORITY_TIME_CRITICAL) prio = THREAD_PRIORITY_HIGHEST;
    if (prio == THREAD_PRIORITY_TIME_CRITICAL) prio = THREAD_PRIORITY_HIGHEST;   /* nunca pior que a UI do host */
    re5_mutex_lock(&g_tid_mu);
    for (int i = 0; i < MAX_THREADS; i++) {
        re5_thread *t = g_threads[i];
        if (t && t->tid == tid && t->host) {
            SetThreadPriority((HANDLE)t->host, prio);
            break;
        }
    }
    re5_mutex_unlock(&g_tid_mu);
#else
    (void)tid;
    (void)prio;
#endif
}

#if defined(RE5_HOST_WINDOWS)
#include <tlhelp32.h>
typedef LONG (NTAPI *nt_query_thread_fn)(HANDLE, int, PVOID, ULONG, PULONG);
typedef HRESULT (WINAPI *get_thread_desc_fn)(HANDLE, PWSTR *);

static uint64_t ft_100ns(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

/* RE5_FPS_LOG: no ExitProcess, o tempo de CPU de TODAS as threads do processo (inclusive as da
 * DXVK e do driver, que o RE5_PROFILE não amostra), por módulo da função inicial da thread. */
void re5_log_process_cpu(void)
{
    if (!re5_stats_on) return;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    nt_query_thread_fn query = nt ? (nt_query_thread_fn)(void *)GetProcAddress(nt, "NtQueryInformationThread") : NULL;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    get_thread_desc_fn desc = k32 ? (get_thread_desc_fn)(void *)GetProcAddress(k32, "GetThreadDescription") : NULL;
    FILETIME cr, ex, kn, us;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kn, &us)) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    double wall = (double)(ft_100ns(now) - ft_100ns(cr)) / 1e7;
    double total = (double)(ft_100ns(kn) + ft_100ns(us)) / 1e7;
    RE5_INFO("cpu", "processo: %.1f s de parede, %.1f s de CPU em todas as threads (%.2f núcleos em média)", wall, total,
             wall > 0 ? total / wall : 0.0);
    static uint32_t prev_tid[256];
    static double prev_sec[256], prev_wall_t;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    struct row { double sec; char mod[40]; char name[48]; DWORD tid; } rows[256];
    struct { char mod[40]; double sec; } mods[16];
    int nr = 0, nm = 0;
    THREADENTRY32 te = {sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || nr >= 256) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME tc, te2, tk, tu;
        if (GetThreadTimes(h, &tc, &te2, &tk, &tu)) {
            struct row *r = &rows[nr++];
            r->sec = (double)(ft_100ns(tk) + ft_100ns(tu)) / 1e7;
            r->tid = te.th32ThreadID;
            snprintf(r->mod, sizeof r->mod, "?");
            r->name[0] = 0;
            PVOID start = NULL;
            if (query && query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) == 0 && start) {
                HMODULE m = NULL;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)start, &m) && m) {
                    char path[MAX_PATH] = "?";
                    GetModuleFileNameA(m, path, sizeof path);
                    const char *b = strrchr(path, '\\');
                    snprintf(r->mod, sizeof r->mod, "%s", b ? b + 1 : path);
                }
            }
            PWSTR w = NULL;
            if (desc && SUCCEEDED(desc(h, &w)) && w) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, r->name, sizeof r->name, NULL, NULL);
                LocalFree(w);
            }
            int k = 0;
            while (k < nm && strcmp(mods[k].mod, r->mod)) k++;
            if (k == nm && nm < 16) { snprintf(mods[nm].mod, sizeof mods[nm].mod, "%s", r->mod); mods[nm++].sec = 0; }
            if (k < nm) mods[k].sec += r->sec;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    for (int i = 0; i < nm; i++)
        RE5_INFO("cpu", "por módulo da thread: %-28s %7.1f s  (%4.1f%% da CPU do processo)", mods[i].mod, mods[i].sec,
                 total > 0 ? 100.0 * mods[i].sec / total : 0.0);
    /* Uso no intervalo desde a chamada anterior (% de um núcleo): quem está saturado e quem espera. */
    double dt = wall - prev_wall_t;
    for (int k = 0; k < 16; k++) {
        int bi = -1;
        for (int i = 0; i < nr; i++) if (rows[i].sec > 0 && (bi < 0 || rows[i].sec > rows[bi].sec)) bi = i;
        if (bi < 0 || rows[bi].sec < 0.05) break;
        double before = 0;
        for (int j = 0; j < 256; j++) if (prev_tid[j] == rows[bi].tid) before = prev_sec[j];
        double util = dt > 0.5 ? 100.0 * (rows[bi].sec - before) / dt : -1.0;
        if (util < 0)
            RE5_INFO("cpu", "  thread %5lu  %7.1f s total  %-24s %s", (unsigned long)rows[bi].tid, rows[bi].sec, rows[bi].mod,
                     rows[bi].name);
        else
            RE5_INFO("cpu", "  thread %5lu  %7.1f s total, %5.1f%% de um núcleo agora  %-24s %s", (unsigned long)rows[bi].tid,
                     rows[bi].sec, util, rows[bi].mod, rows[bi].name);
        rows[bi].sec = -rows[bi].sec;   /* marca como impressa; salvo abaixo */
    }
    for (int i = 0; i < nr && i < 256; i++) { prev_tid[i] = rows[i].tid; prev_sec[i] = rows[i].sec < 0 ? -rows[i].sec : rows[i].sec; }
    prev_wall_t = wall;
}
#else
void re5_log_process_cpu(void) {}
#endif

void re5_thread_set_lock_wait_pump(void (*fn)(void)) { t_pump_set(fn); }
int re5_thread_has_pump(void) { return t_pump_get() != NULL; }
void re5_thread_pump(void)
{
    if (t_pump_get()) t_pump_get()();
}

void re5_guest_preempt(void)
{
    re5_thread *t = t_self_get();
    if (g_mode == 1) {   /* hle: nada a ceder; só o ponto de suspensão do código do guest */
        if (t && t->suspend > 0 && t->lock_depth == 0) wait_resumed(t);
        return;
    }
    if (!g_waiting || !t || t->lock_depth != 1 || t->thunk) return;
    if (now_ns() - g_turn_ns < QUANTUM_NS) return;
    re5_guest_unlock();
    re5_guest_lock();
}

uint32_t re5_thread_suspend(re5_thread *t)
{
    ensure_init();
    re5_mutex_lock(&g_susp_mu);
    uint32_t prev = (uint32_t)t->suspend++;
    re5_mutex_unlock(&g_susp_mu);
    if (t == t_self_get()) {   /* a própria thread: para já, sem o lock */
        int depth = re5_guest_lock_release_all();
        wait_resumed(t);
        re5_guest_lock_reacquire(depth);
    }
    return prev;
}

uint32_t re5_thread_resume(re5_thread *t)
{
    ensure_init();
    re5_mutex_lock(&g_susp_mu);
    uint32_t prev = (uint32_t)t->suspend;
    if (t->suspend > 0 && --t->suspend == 0) re5_cond_broadcast(&g_susp_cv);
    re5_mutex_unlock(&g_susp_mu);
    return prev;
}

void re5_guest_unlock(void)
{
    int *depth = depth_of(t_self_get());
    if (*depth <= 0) re5_fatal("thread", "re5_guest_unlock sem o lock do guest");
    if (--*depth == 0) {
        g_holder = NULL;
        if (g_mode == 1) hle_give();
        else give_turn();
    }
}

int re5_guest_lock_release_all(void)
{
    int depth = t_self_get() ? t_self_get()->lock_depth : 0;
    for (int i = 0; i < depth; i++) re5_guest_unlock();
    return depth;
}

void re5_thread_yield(void) { re5_host_yield(); }

void re5_guest_lock_reacquire(int depth)
{
    for (int i = 0; i < depth; i++) re5_guest_lock();
}

/* ---- watchdog (RE5_WATCHDOG=<segundos>) ---- */

static void watchdog_main(void *arg)
{
    uint32_t secs = (uint32_t)(uintptr_t)arg;
    uint32_t last_tid[MAX_THREADS] = {0}, last_pos[MAX_THREADS] = {0};
    for (;;) {
        re5_host_sleep_ms(secs * 1000u);
        re5_mutex_lock(&g_tid_mu);
        for (int i = 0; i < MAX_THREADS; i++) {
            re5_thread *t = g_threads[i];
            if (!t) continue;
            uint32_t pos = *t->trail_pos;
            uint32_t delta = last_tid[i] == t->tid ? pos - last_pos[i] : pos;
            last_tid[i] = t->tid;
            last_pos[i] = pos;
            const char *th = t->thunk;
            /* Os últimos destinos da trilha (thunks pelo nome): numa thread que gira, mostram o laço. */
            char tail[256];
            size_t k = 0;
            tail[0] = 0;
            for (uint32_t j = 0; j < 8 && j < pos && j < RE5_TRAIL && k < sizeof tail - 48; j++) {
                uint32_t va = t->trail[(pos - 1 - j) % RE5_TRAIL];
                const re5_thunk *tk = re5_thunk_get(va);
                const char *nm = tk ? tk->name : re5_name_lookup(va);
                if (nm) k += (size_t)snprintf(tail + k, sizeof tail - k, " %.40s", nm);
                else k += (size_t)snprintf(tail + k, sizeof tail - k, " 0x%08X", va);
            }
            RE5_INFO("watchdog", "tid 0x%X: %s, +%u dispatches, %s%s; trilha:%s", t->tid,
                     t->lock_wait ? "esperando o lock" : t->lock_depth > 0 ? "com o lock" : "sem o lock", delta,
                     th ? "no host: " : "no guest", th ? th : "", tail);
        }
        re5_mutex_unlock(&g_tid_mu);
    }
}

void re5_watchdog_start(void)
{
    const char *e = getenv("RE5_WATCHDOG");
    long secs = e ? strtol(e, NULL, 10) : 0;
    if (secs <= 0) return;
    ensure_init();
    if (re5_host_thread_start(watchdog_main, (void *)(uintptr_t)secs, 1u << 20) != 0)
        RE5_WARN("watchdog", "não consegui criar a thread do watchdog");
    else
        RE5_INFO("watchdog", "relatório das threads do guest a cada %ld s", secs);
}

/* ---- perfil por amostragem (RE5_PROFILE=<segundos>) ---- */

#if defined(RE5_HOST_WINDOWS)
typedef struct { uintptr_t host; uint32_t va; } prof_fn;
static prof_fn *g_pf;          /* funções geradas por endereço do host */
static uint32_t g_npf, *g_pf_hits;
#define PROF_THUNKS 512
static const char *g_pt_name[PROF_THUNKS];
static uint32_t g_pt_hits[PROF_THUNKS];
/* Amostras de espera pelo lock: em que thunk a thread quer entrar, e o que a
 * dona do lock está rodando naquele instante (handler, ou "código do guest"
 * num guest_call de dentro de handler). */
static const char *g_pw_name[PROF_THUNKS], *g_ph_name[PROF_THUNKS];
static uint32_t g_pw_hits[PROF_THUNKS], g_ph_hits[PROF_THUNKS];
/* Fora do código gerado e fora de handler: RIP (em blocos de 16 bytes) para o nm. */
#define PROF_OTHER 4096
static uintptr_t g_po_rip[PROF_OTHER];
static uint32_t g_po_hits[PROF_OTHER];

/* RE5_PROFILE_RIPS=<arquivo>: cada endereço exato do host amostrado no código gerado, para o
 * scripts/prof_lines.py levar até a linha do C gerado e à instrução x86 de origem. */
#define PROF_RIPS 262144
static uintptr_t g_pr_rip[PROF_RIPS];
static uint32_t g_pr_hits[PROF_RIPS];

static void pr_hit(uintptr_t rip)
{
    uint32_t h = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 46) % PROF_RIPS;
    for (uint32_t i = 0; i < 256; i++, h = (h + 1) % PROF_RIPS) {
        if (g_pr_rip[h] == rip || !g_pr_rip[h]) {
            g_pr_rip[h] = rip;
            g_pr_hits[h]++;
            return;
        }
    }
}

static void pr_dump(void)
{
    const char *path = getenv("RE5_PROFILE_RIPS");
    if (!path || !*path) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    uintptr_t mod = (uintptr_t)GetModuleHandleA(NULL);
    for (uint32_t i = 0; i < PROF_RIPS; i++)
        if (g_pr_hits[i] && g_pr_rip[i] >= mod)
            fprintf(f, "0x%llx %u\n", (unsigned long long)(g_pr_rip[i] - mod + 0x140000000ull), g_pr_hits[i]);
    fclose(f);
    memset(g_pr_rip, 0, sizeof g_pr_rip);
    memset(g_pr_hits, 0, sizeof g_pr_hits);
}

static void po_hit(uintptr_t rip)
{
    rip &= ~(uintptr_t)15;
    uint32_t h = (uint32_t)(rip >> 4) % PROF_OTHER;
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) % PROF_OTHER) {
        if (g_po_rip[h] == rip || !g_po_rip[h]) {
            g_po_rip[h] = rip;
            g_po_hits[h]++;
            return;
        }
    }
}

static int pf_cmp(const void *a, const void *b)
{
    uintptr_t x = ((const prof_fn *)a)->host, y = ((const prof_fn *)b)->host;
    return x < y ? -1 : x > y;
}

/* Função gerada que contém `rip` (a de endereço logo abaixo), ou -1. */
static int pf_find(uintptr_t rip)
{
    if (!g_npf || rip < g_pf[0].host || rip > g_pf[g_npf - 1].host + 0x100000u) return -1;
    uint32_t lo = 0, hi = g_npf;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_pf[mid].host <= rip) lo = mid; else hi = mid;
    }
    return (int)lo;
}

static void tab_hit(const char **names, uint32_t *hits, const char *name)
{
    uint32_t h = (uint32_t)((uintptr_t)name >> 3) % PROF_THUNKS;
    for (uint32_t i = 0; i < PROF_THUNKS; i++, h = (h + 1) % PROF_THUNKS) {
        if (names[h] == name || !names[h]) {
            names[h] = name;
            hits[h]++;
            return;
        }
    }
}

static void pt_hit(const char *name) { tab_hit(g_pt_name, g_pt_hits, name); }

static void tab_report(const char *title, const char **names, uint32_t *hits, uint64_t total, int n)
{
    for (int k = 0; k < n; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < PROF_THUNKS; i++)
            if (hits[i] > best) best = hits[i], bi = i;
        if (!best) break;
        RE5_INFO("profile", "  %5.1f%%  %s %s", 100.0 * best / (double)total, title, names[bi]);
        hits[bi] = 0;
    }
    memset(hits, 0, PROF_THUNKS * sizeof *hits);
    memset((void *)names, 0, PROF_THUNKS * sizeof *names);
}

static void prof_report(uint64_t total, uint64_t gen, uint64_t host, uint64_t other, uint64_t lockw, uint64_t busy,
                        uint64_t ticks)
{
    if (!total) return;
    RE5_INFO("profile", "%llu amostras em %llu ticks: alguma thread rodando em %.1f%% dos ticks",
             (unsigned long long)total, (unsigned long long)ticks, ticks ? 100.0 * (double)busy / (double)ticks : 0.0);
    RE5_INFO("profile", "  código gerado %.1f%%, handlers do host %.1f%%, runtime/outros %.1f%%, esperando o lock %.1f%%",
             100.0 * (double)gen / (double)total, 100.0 * (double)host / (double)total,
             100.0 * (double)other / (double)total, 100.0 * (double)lockw / (double)total);
    for (int k = 0; k < 25; k++) {   /* as 25 funções do guest mais amostradas */
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < g_npf; i++)
            if (g_pf_hits[i] > best) best = g_pf_hits[i], bi = i;
        if (!best) break;
        const char *nm = re5_name_lookup(g_pf[bi].va);
        RE5_INFO("profile", "  %5.1f%%  0x%08X%s%s", 100.0 * best / (double)total, g_pf[bi].va,
                 nm ? "  " : "", nm ? nm : "");
        g_pf_hits[bi] = 0;
    }
    for (int k = 0; k < 12; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < PROF_THUNKS; i++)
            if (g_pt_hits[i] > best) best = g_pt_hits[i], bi = i;
        if (!best) break;
        RE5_INFO("profile", "  %5.1f%%  %s", 100.0 * best / (double)total, g_pt_name[bi]);
        g_pt_hits[bi] = 0;
    }
    /* Endereços como no `nm re5.exe` (base preferida 0x140000000, sem o ASLR). */
    uintptr_t mod = (uintptr_t)GetModuleHandleA(NULL);
    for (int k = 0; k < 12; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < PROF_OTHER; i++)
            if (g_po_hits[i] > best) best = g_po_hits[i], bi = i;
        if (!best) break;
        uintptr_t r = g_po_rip[bi];
        if (r >= mod && r - mod < 0x40000000u)
            RE5_INFO("profile", "  %5.1f%%  re5.exe 0x%llx", 100.0 * best / (double)total,
                     (unsigned long long)(r - mod + 0x140000000ull));
        else {
            HMODULE m = NULL;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)r, &m) && m) {
                GetModuleFileNameA(m, name, sizeof name);
                const char *b = strrchr(name, '\\');
                if (b) memmove(name, b + 1, strlen(b));
            }
            RE5_INFO("profile", "  %5.1f%%  fora do re5.exe: %s+0x%llx", 100.0 * best / (double)total, name,
                     (unsigned long long)(r - (uintptr_t)m));
        }
        g_po_hits[bi] = 0;
    }
    tab_report("esperando para entrar em", g_pw_name, g_pw_hits, total, 10);
    tab_report("enquanto o lock está com", g_ph_name, g_ph_hits, total, 10);
    pr_dump();
    memset(g_po_rip, 0, sizeof g_po_rip);
    memset(g_po_hits, 0, sizeof g_po_hits);
    memset(g_pf_hits, 0, g_npf * sizeof *g_pf_hits);
    memset(g_pt_hits, 0, sizeof g_pt_hits);
    memset((void *)g_pt_name, 0, sizeof g_pt_name);
}

static void profile_main(void *arg)
{
    uint32_t secs = (uint32_t)(uintptr_t)arg;
    uint64_t total = 0, gen = 0, host = 0, other = 0, lockw = 0, busy = 0, ticks = 0, running = 0;
    DWORD last = GetTickCount();
    uint32_t last_pc = 0;
    for (;;) {
        Sleep(1);
        ticks++;
        int any = 0;   /* threads rodando neste tick */
        re5_mutex_lock(&g_tid_mu);
        for (int i = 0; i < MAX_THREADS; i++) {
            re5_thread *t = g_threads[i];
            if (!t || !t->host) continue;
            if (t->lock_wait) {   /* parada na fila do lock: custo da serialização */
                total++;
                lockw++;
                const char *want = t->thunk;
                re5_thread *h = g_holder;
                const char *has = h ? h->thunk : NULL;
                tab_hit(g_pw_name, g_pw_hits, want ? want : "(código do guest)");
                tab_hit(g_ph_name, g_ph_hits, has ? has : h ? "(código do guest)" : "(ninguém: passando a vez)");
                continue;
            }
            const char *th = t->thunk;
            /* Sem o lock dentro de um handler: bloqueada numa espera (ou um handler
             * NOLOCK curto), não conta. No modo global, sem o lock = parada. */
            if (t->lock_depth <= 0 && (th || g_mode != 1)) continue;
            any++;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread((HANDLE)t->host) == (DWORD)-1) continue;
            BOOL ok = GetThreadContext((HANDLE)t->host, &ctx);
            /* Relido com a thread parada: entre a 1ª leitura e o SuspendThread ela
             * pode ter entrado num handler sem lock (Sleep, esperas), e o RIP no
             * ntdll contava como "runtime/outros". */
            th = t->thunk;
            int idle = t->lock_depth <= 0 && th && g_mode == 1;
            ResumeThread((HANDLE)t->host);
            if (!ok || idle) continue;
            total++;
            int f = pf_find((uintptr_t)ctx.Rip);
            if (th) {
                host++;
                pt_hit(th);
            } else if (f >= 0) {
                gen++;
                g_pf_hits[f]++;
                pr_hit((uintptr_t)ctx.Rip);
            } else {
                other++;
                po_hit((uintptr_t)ctx.Rip);
            }
        }
        re5_mutex_unlock(&g_tid_mu);
        busy += any > 0;
        running += (uint64_t)any;
        if (GetTickCount() - last >= secs * 1000u) {
            prof_report(total, gen, host, other, lockw, busy, ticks);
            uint32_t pc = re5_d3d9_present_count();
            if (ticks) RE5_INFO("profile", "  threads rodando (código do guest ou handler) por tick: %.2f; %.1f Present/s",
                                (double)running / (double)ticks, (double)(pc - last_pc) * 1000.0 / (double)(GetTickCount() - last));
            last_pc = pc;
            total = gen = host = other = lockw = busy = ticks = running = 0;
            last = GetTickCount();
        }
    }
}
#endif

void re5_profile_start(void)
{
    const char *e = getenv("RE5_PROFILE");
    long secs = e ? strtol(e, NULL, 10) : 0;
    if (secs <= 0) return;
#if defined(RE5_HOST_WINDOWS)
    ensure_init();
    g_npf = re5_lifted_count;
    g_pf = calloc(g_npf ? g_npf : 1, sizeof *g_pf);
    g_pf_hits = calloc(g_npf ? g_npf : 1, sizeof *g_pf_hits);
    if (!g_pf || !g_pf_hits) re5_fatal("profile", "sem memória");
    for (uint32_t i = 0; i < g_npf; i++) g_pf[i] = (prof_fn){(uintptr_t)re5_lifted_table[i].fn, re5_lifted_table[i].va};
    qsort(g_pf, g_npf, sizeof *g_pf, pf_cmp);
    if (re5_host_thread_start(profile_main, (void *)(uintptr_t)secs, 1u << 20) != 0)
        RE5_WARN("profile", "não consegui criar a thread do perfil");
    else
        RE5_INFO("profile", "amostragem a cada 1 ms, relatório a cada %ld s (%u funções geradas)", secs, g_npf);
#else
    RE5_WARN("profile", "RE5_PROFILE só existe no Windows por enquanto");
#endif
}

/* ---- threads do host com pilha grande ---- */

typedef struct { void (*fn)(void *); void *arg; } trampoline;

#if defined(RE5_HOST_WINDOWS)
/* Num estouro da pilha do host o filtro do crash.c roda na própria pilha estourada:
 * sem reserva ele falha no __chkstk e o processo some sem log (visto no RE5,
 * 2026-10-24: 0xC0000005 em ntdll!__chkstk). */
static void stack_guarantee(void)
{
    ULONG g = 256u << 10;
    SetThreadStackGuarantee(&g);
}

static DWORD WINAPI host_entry(LPVOID p)
{
    trampoline *tr = p;
    stack_guarantee();
    tr->fn(tr->arg);
    return 0;
}

int re5_host_thread_run(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    trampoline tr = {fn, arg};
    HANDLE h = CreateThread(NULL, stack_bytes, host_entry, &tr, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return -1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return 0;
}

static DWORD WINAPI host_entry_detached(LPVOID p)
{
    trampoline tr = *(trampoline *)p;
    free(p);
    stack_guarantee();
    tr.fn(tr.arg);
    return 0;
}

int re5_host_thread_start(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    trampoline *tr = malloc(sizeof *tr);
    if (!tr) return -1;
    *tr = (trampoline){fn, arg};
    HANDLE h = CreateThread(NULL, stack_bytes, host_entry_detached, tr, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(tr); return -1; }
    CloseHandle(h);
    return 0;
}
#else
static void *host_entry(void *p)
{
    trampoline *tr = p;
    tr->fn(tr->arg);
    return NULL;
}

int re5_host_thread_run(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    trampoline tr = {fn, arg};
    pthread_attr_t a;
    pthread_t th;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_bytes);
    int rc = pthread_create(&th, &a, host_entry, &tr);
    pthread_attr_destroy(&a);
    if (rc != 0) return -1;
    pthread_join(th, NULL);
    return 0;
}

static void *host_entry_detached(void *p)
{
    trampoline tr = *(trampoline *)p;
    free(p);
    tr.fn(tr.arg);
    return NULL;
}

int re5_host_thread_start(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    trampoline *tr = malloc(sizeof *tr);
    if (!tr) return -1;
    *tr = (trampoline){fn, arg};
    pthread_attr_t a;
    pthread_t th;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_bytes);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &a, host_entry_detached, tr);
    pthread_attr_destroy(&a);
    if (rc != 0) { free(tr); return -1; }
    return 0;
}
#endif
