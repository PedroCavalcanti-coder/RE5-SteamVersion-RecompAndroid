/*
 * perf_hint.c - núcleos grandes e ADPF (ver re5/perf_hint.h).
 */
#if defined(__ANDROID__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* cpu_set_t, CPU_SET, sched_setaffinity */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/log.h"
#include "re5/perf_hint.h"

uint64_t re5_perf_big_mask(const char *sysroot)
{
    unsigned long freq[64];
    int n = 0;
    for (; n < 64; n++) {
        char path[512];
        snprintf(path, sizeof path, "%s/cpu%d/cpufreq/cpuinfo_max_freq", sysroot, n);
        FILE *f = fopen(path, "r");
        if (!f) break;
        unsigned long v = 0;
        if (fscanf(f, "%lu", &v) != 1) v = 0;
        fclose(f);
        freq[n] = v;
    }
    if (n == 0) return 0;
    unsigned long hi = 0, lo = (unsigned long)-1;
    for (int i = 0; i < n; i++) {
        if (freq[i] > hi) hi = freq[i];
        if (freq[i] < lo) lo = freq[i];
    }
    if (!hi || hi == lo) return 0;   /* todos iguais: nada a separar */
    /* Todos acima do cluster mais lento: num Snapdragon 1+3+4 o Prime sozinho poria a thread
     * principal e a de render no mesmo núcleo; Prime + médios dão espaço às duas. */
    uint64_t m = 0;
    for (int i = 0; i < n; i++)
        if (freq[i] > lo) m |= 1ull << i;
    return m;
}

int64_t re5_perf_target_ns(const char *fps_cap)
{
    long fps = fps_cap ? strtol(fps_cap, NULL, 10) : 0;
    if (fps < 30 || fps > 240) fps = 60;   /* sem limite: pede ao sistema o máximo útil */
    return 1000000000LL / fps;
}

int64_t re5_perf_work_ns(int64_t interval_ns, uint64_t idle_ns)
{
    int64_t w = interval_ns - (int64_t)idle_ns;
    return w > 0 ? w : 1;
}

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef APerformanceHintManager *(*fn_get_manager)(void);
typedef APerformanceHintSession *(*fn_create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
typedef int (*fn_report)(APerformanceHintSession *, int64_t);

static int g_on = -1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_present_tid, g_render_tid;
static int g_setup_done;
static APerformanceHintSession *g_session;
static fn_report g_report;
static int64_t g_target_ns;
static __thread int t_present_noted, t_render_noted;

static int enabled(void)
{
    if (g_on < 0) {
        const char *e = getenv("RE5_PERF_HINT");
        g_on = !(e && *e == '0');
    }
    return g_on;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void pin_big(int32_t tid, uint64_t mask, const char *who)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    int r = sched_setaffinity(tid, sizeof set, &set);
    RE5_INFO("perf", "thread %s (tid %d) nos núcleos grandes 0x%llx: %s", who, tid, (unsigned long long)mask,
             r == 0 ? "ok" : "recusado");
}

/* Com as duas threads conhecidas: afinidade e sessão ADPF. Sob g_mu. */
static void setup(void)
{
    g_setup_done = 1;
    uint64_t big = re5_perf_big_mask("/sys/devices/system/cpu");
    if (big) {
        pin_big(g_present_tid, big, "principal");
        if (g_render_tid && g_render_tid != g_present_tid) pin_big(g_render_tid, big, "de render");
    } else {
        RE5_INFO("perf", "núcleos todos iguais (ou cpufreq ilegível): sem afinidade");
    }
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    fn_get_manager get = lib ? (fn_get_manager)dlsym(lib, "APerformanceHint_getManager") : NULL;
    fn_create create = lib ? (fn_create)dlsym(lib, "APerformanceHint_createSession") : NULL;
    g_report = lib ? (fn_report)dlsym(lib, "APerformanceHint_reportActualWorkDuration") : NULL;
    if (!get || !create || !g_report) {
        RE5_INFO("perf", "ADPF indisponível (Android 13+): só a afinidade");
        return;
    }
    g_target_ns = re5_perf_target_ns(getenv("RE5_FPS_CAP"));
    int32_t tids[2] = {g_present_tid, g_render_tid};
    size_t nt = g_render_tid && g_render_tid != g_present_tid ? 2 : 1;
    APerformanceHintManager *m = get();
    g_session = m ? create(m, tids, nt, g_target_ns) : NULL;
    RE5_INFO("perf", "ADPF: %s (alvo %.1f ms, %zu thread%s)", g_session ? "sessão criada" : "recusada",
             g_target_ns / 1e6, nt, nt > 1 ? "s" : "");
}

void re5_perf_note_render(void)
{
    if (t_render_noted || !enabled()) return;
    t_render_noted = 1;
    pthread_mutex_lock(&g_mu);
    if (!g_render_tid) g_render_tid = (int32_t)syscall(SYS_gettid);
    pthread_mutex_unlock(&g_mu);
}

void re5_perf_note_present(uint64_t idle_ns)
{
    if (!enabled()) return;
    static int64_t last;
    static uint32_t presents;
    if (!t_present_noted) {
        t_present_noted = 1;
        pthread_mutex_lock(&g_mu);
        if (!g_present_tid) g_present_tid = (int32_t)syscall(SYS_gettid);
        pthread_mutex_unlock(&g_mu);
    }
    presents++;
    /* Espera a thread de render aparecer (ou 600 quadros, se o jogo desenha na principal). */
    if (!g_setup_done && (g_render_tid || presents > 600)) {
        pthread_mutex_lock(&g_mu);
        if (!g_setup_done) setup();
        pthread_mutex_unlock(&g_mu);
    }
    int64_t now = now_ns();
    if (g_session && last) {
        int64_t d = now - last;
        if (d > 0 && d < 1000000000LL) g_report(g_session, re5_perf_work_ns(d, idle_ns));   /* fora: carregamento/pausa */
    }
    last = now;
}

#else

void re5_perf_note_present(uint64_t idle_ns) { (void)idle_ns; }
void re5_perf_note_render(void) {}

#endif
