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

static int read_ulong_file(const char *path, unsigned long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long v = 0;
    int ok = (fscanf(f, "%lu", &v) == 1 && v > 0);
    fclose(f);
    if (ok) *out = v;
    return ok;
}

static uint64_t mask_from_freqs(const unsigned long *freq, int n)
{
    if (n <= 0) return 0;
    unsigned long hi = 0, lo = (unsigned long)-1;
    for (int i = 0; i < n; i++) {
        if (freq[i] > hi) hi = freq[i];
        if (freq[i] < lo) lo = freq[i];
    }
    if (!hi || hi == lo) return 0;   /* todos iguais: nada a separar */
    /* Se o menor cluster já roda >= 2.6 GHz (ex.: Snapdragon 8 Gen 4 / 8 Elite com 6x Oryon
     * Performance a 3.53 GHz + 2x Prime a 4.32 GHz, sem núcleos LITTLE), prender o jogo só
     * acima de 'lo' confina main + render em apenas 2 núcleos e provoca throttling térmico. */
    if (lo >= 2600000ul) return 0;
    /* Todos acima do cluster mais lento: num Snapdragon 1+3+4 o Prime sozinho poria a thread
     * principal e a de render no mesmo núcleo; Prime + médios dão espaço às duas. */
    uint64_t m = 0;
    for (int i = 0; i < n; i++)
        if (freq[i] > lo) m |= 1ull << i;
    return m;
}

static int is_little_arm_part(unsigned long part)
{
    switch (part) {
    case 0xd03: /* Cortex-A53 */
    case 0xd05: /* Cortex-A55 */
    case 0xd46: /* Cortex-A510 */
    case 0xd80: /* Cortex-A520 */
    case 0x803: /* Kryo 385 Silver */
    case 0x805: /* Kryo 485/585/680 Silver */
        return 1;
    default:
        return 0;
    }
}

static uint64_t mask_from_proc_cpuinfo(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[256];
    int cur_cpu = -1;
    int little[64];
    int known[64];
    memset(little, 0, sizeof little);
    memset(known, 0, sizeof known);
    int max_cpu = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long val = 0;
        if (sscanf(line, "processor : %lu", &val) == 1) {
            cur_cpu = (val < 64) ? (int)val : -1;
            if (cur_cpu > max_cpu) max_cpu = cur_cpu;
        } else if (cur_cpu >= 0 && strncmp(line, "CPU part", 8) == 0) {
            const char *colon = strchr(line, ':');
            if (colon && sscanf(colon + 1, " %lx", &val) == 1) {
                known[cur_cpu] = 1;
                little[cur_cpu] = is_little_arm_part(val);
            }
        }
    }
    fclose(f);
    int n_known = 0, n_little = 0;
    uint64_t big_mask = 0;
    for (int i = 0; i <= max_cpu; i++) {
        if (!known[i]) continue;
        n_known++;
        if (little[i]) n_little++;
        else big_mask |= 1ull << i;
    }
    if (n_known == 0 || n_little == 0 || n_little == n_known) return 0;
    return big_mask;
}

uint64_t re5_perf_big_mask(const char *sysroot)
{
    unsigned long freq[64];
    int n = 0;
    for (; n < 64; n++) {
        char path[512];
        snprintf(path, sizeof path, "%s/cpu%d/cpufreq/cpuinfo_max_freq", sysroot, n);
        if (!read_ulong_file(path, &freq[n])) break;
    }
    if (n > 0) return mask_from_freqs(freq, n);

    /* Fallback para sysfs real (/sys/...) em Android 14/15 quando cpuinfo_max_freq sofre EACCES */
    if (strncmp(sysroot, "/sys", 4) != 0) return 0;

    n = 0;
    for (; n < 64; n++) {
        char path[512];
        snprintf(path, sizeof path, "%s/cpu%d/cpufreq/scaling_max_freq", sysroot, n);
        if (!read_ulong_file(path, &freq[n])) break;
    }
    if (n > 0) return mask_from_freqs(freq, n);

    uint64_t pm = mask_from_proc_cpuinfo();
    if (pm) return pm;

    unsigned long cap[64];
    n = 0;
    for (; n < 64; n++) {
        char path[512];
        snprintf(path, sizeof path, "%s/cpu%d/cpu_capacity", sysroot, n);
        if (!read_ulong_file(path, &cap[n])) break;
    }
    if (n > 0) {
        unsigned long hi = 0, lo = (unsigned long)-1;
        for (int i = 0; i < n; i++) {
            if (cap[i] > hi) hi = cap[i];
            if (cap[i] < lo) lo = cap[i];
        }
        /* Só separa quando existe cluster realmente pequeno (< 65% da capacidade máxima) */
        if (hi > 0 && lo < (hi * 65ul) / 100ul) {
            uint64_t m = 0;
            unsigned long thresh = (hi * 65ul) / 100ul;
            for (int i = 0; i < n; i++)
                if (cap[i] >= thresh) m |= 1ull << i;
            return m;
        }
    }
    return 0;
}

int re5_perf_pin_render(uint64_t big_mask, const char *env)
{
    if (env && *env == '0') return 0;
    if (env && *env == '1') return big_mask != 0;
    return __builtin_popcountll(big_mask) >= 3;
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

void re5_perf_split(int64_t interval_ns, uint64_t idle_ns, uint64_t present_ns, int64_t *total, int64_t *cpu,
                    int64_t *gpu)
{
    int64_t t = re5_perf_work_ns(interval_ns, idle_ns);
    int64_t g = present_ns > (uint64_t)t ? t : (int64_t)present_ns;
    int64_t c = t - g;
    *total = t;
    *gpu = g;
    *cpu = c > 0 ? c : 1;
}

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <stdbool.h>
#include "re5/d3d9_bridge.h"
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
/* Android 15+ (API 35): duração com CPU e GPU separadas. Todas por dlsym; faltando uma, fica o
 * relato antigo (só o total). */
typedef struct AWorkDuration AWorkDuration;
typedef int (*fn_report2)(APerformanceHintSession *, AWorkDuration *);
typedef AWorkDuration *(*fn_wd_create)(void);
typedef void (*fn_wd_set)(AWorkDuration *, int64_t);

static fn_report2 g_report2;
static fn_wd_set g_wd_start, g_wd_total, g_wd_cpu, g_wd_gpu;
static AWorkDuration *g_wd;

static int g_on = -1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_present_tid, g_render_tid;
static int g_setup_done;
static APerformanceHintSession *g_session;
static fn_report g_report;
static int64_t g_target_ns;
/* Android 16 (API 36): pedido de boost quando o quadro estoura o alvo (RE5_PERF_BOOST=0 desliga). */
typedef int (*fn_boost)(APerformanceHintSession *, bool, bool, const char *);
static fn_boost g_boost;
static int64_t g_last_boost;
static uint32_t g_boosts;
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
        if (g_render_tid && g_render_tid != g_present_tid) {
            if (re5_perf_pin_render(big, getenv("RE5_PIN_RENDER"))) pin_big(g_render_tid, big, "de render");
            else RE5_INFO("perf", "thread de render (tid %d) livre: só %d núcleos grandes, ficam para a principal",
                          g_render_tid, __builtin_popcountll(big));
        }
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
    {
        const char *e = getenv("RE5_PERF_GPU");
        fn_wd_create wd_create = (fn_wd_create)dlsym(lib, "AWorkDuration_create");
        g_report2 = (fn_report2)dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
        g_wd_start = (fn_wd_set)dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
        g_wd_total = (fn_wd_set)dlsym(lib, "AWorkDuration_setActualTotalDurationNanos");
        g_wd_cpu = (fn_wd_set)dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
        g_wd_gpu = (fn_wd_set)dlsym(lib, "AWorkDuration_setActualGpuDurationNanos");
        int off = e && *e == '0';
        if (!off && wd_create && g_report2 && g_wd_start && g_wd_total && g_wd_cpu && g_wd_gpu) g_wd = wd_create();
        if (!g_wd) g_report2 = NULL;
        RE5_INFO("perf", "ADPF com duração de GPU: %s", g_wd ? "sim (Android 15+)"
                                                          : off ? "desligado (RE5_PERF_GPU=0)" : "indisponível");
    }
    g_target_ns = re5_perf_target_ns(getenv("RE5_FPS_CAP"));
    int32_t tids[2] = {g_present_tid, g_render_tid};
    size_t nt = g_render_tid && g_render_tid != g_present_tid ? 2 : 1;
    APerformanceHintManager *m = get();
    const char *how = "createSession";
    /* API 36: sessão marcada como pipeline gráfico, ligada à janela do jogo e sem preferência por
     * economia; o PowerHAL trata essas como jogo/render (CPU e GPU). Falhando, a sessão antiga. */
    {
        typedef void *(*fn_cfg_create)(void);
        typedef void (*fn_cfg_release)(void *);
        typedef void (*fn_cfg_tids)(void *, const int32_t *, size_t);
        typedef void (*fn_cfg_target)(void *, int64_t);
        typedef void (*fn_cfg_bool)(void *, bool);
        typedef void (*fn_cfg_surf)(void *, void **, size_t, void **, size_t);
        typedef int (*fn_create_cfg)(APerformanceHintManager *, void *, APerformanceHintSession **);
        fn_cfg_create cc = (fn_cfg_create)dlsym(lib, "ASessionCreationConfig_create");
        fn_cfg_release cr = (fn_cfg_release)dlsym(lib, "ASessionCreationConfig_release");
        fn_cfg_tids ct = (fn_cfg_tids)dlsym(lib, "ASessionCreationConfig_setTids");
        fn_cfg_target cg = (fn_cfg_target)dlsym(lib, "ASessionCreationConfig_setTargetWorkDurationNanos");
        fn_cfg_bool cp = (fn_cfg_bool)dlsym(lib, "ASessionCreationConfig_setGraphicsPipeline");
        fn_cfg_bool ce = (fn_cfg_bool)dlsym(lib, "ASessionCreationConfig_setPreferPowerEfficiency");
        fn_cfg_surf cs = (fn_cfg_surf)dlsym(lib, "ASessionCreationConfig_setNativeSurfaces");
        fn_create_cfg ccfg = (fn_create_cfg)dlsym(lib, "APerformanceHint_createSessionUsingConfig");
        const char *e = getenv("RE5_PERF_GFX");
        if (m && cc && ct && cg && ccfg && !(e && *e == '0')) {
            void *cfg = cc();
            if (cfg) {
                ct(cfg, tids, nt);
                cg(cfg, g_target_ns);
                if (cp) cp(cfg, true);
                if (ce) ce(cfg, false);
                void *win = re5_android_current_window();
                if (cs && win) cs(cfg, &win, 1, NULL, 0);
                APerformanceHintSession *s = NULL;
                int r = ccfg(m, cfg, &s);
                if (r == 0 && s) {
                    g_session = s;
                    how = win ? "pipeline gráfico + janela" : "pipeline gráfico";
                } else {
                    RE5_WARN("perf", "createSessionUsingConfig recusou (%d): sessão comum", r);
                }
                if (cr) cr(cfg);
            }
        }
    }
    if (!g_session && m) g_session = create(m, tids, nt, g_target_ns);
    if (g_session) {
        typedef int (*fn_eff)(APerformanceHintSession *, bool);
        fn_eff eff = (fn_eff)dlsym(lib, "APerformanceHint_setPreferPowerEfficiency");
        if (eff) eff(g_session, false);
        const char *b = getenv("RE5_PERF_BOOST");
        if (!(b && *b == '0')) g_boost = (fn_boost)dlsym(lib, "APerformanceHint_notifyWorkloadIncrease");
    }
    RE5_INFO("perf", "ADPF: %s (%s, alvo %.1f ms, %zu thread%s, boost %s)", g_session ? "sessão criada" : "recusada",
             how, g_target_ns / 1e6, nt, nt > 1 ? "s" : "", g_boost ? "sim" : "não");
}

void re5_perf_note_render(void)
{
    if (t_render_noted || !enabled()) return;
    t_render_noted = 1;
    pthread_mutex_lock(&g_mu);
    if (!g_render_tid) g_render_tid = (int32_t)syscall(SYS_gettid);
    pthread_mutex_unlock(&g_mu);
}

void re5_perf_note_present(uint64_t idle_ns, uint64_t present_ns)
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
        if (d > 0 && d < 1000000000LL) {   /* fora: carregamento/pausa */
            if (g_report2) {
                int64_t total, cpu, gpu;
                re5_perf_split(d, idle_ns, present_ns, &total, &cpu, &gpu);
                g_wd_start(g_wd, last);
                g_wd_total(g_wd, total);
                g_wd_cpu(g_wd, cpu);
                g_wd_gpu(g_wd, gpu);
                if (g_report2(g_session, g_wd) != 0) {   /* recusou: volta ao relato antigo de vez */
                    RE5_WARN("perf", "reportActualWorkDuration2 recusado: só a duração total daqui em diante");
                    g_report2 = NULL;
                    g_report(g_session, total);
                }
            } else {
                g_report(g_session, re5_perf_work_ns(d, idle_ns));
            }
            /* quadro acima do alvo: pede mais CPU e GPU, no máximo a cada 300 ms */
            if (g_boost && re5_perf_work_ns(d, idle_ns) > g_target_ns && now - g_last_boost > 300000000LL) {
                g_last_boost = now;
                if (g_boost(g_session, true, true, "re5_frame") == 0 && ++g_boosts == 1)
                    RE5_INFO("perf", "ADPF: primeiro pedido de boost aceito (quadro acima do alvo)");
            }
        }
    }
    last = now;
}

void re5_perf_thread_start(void)
{
    static int keep = -1;
    if (keep < 0) {
        const char *e = getenv("RE5_WORKER_BIG");
        keep = e && *e == '1';
    }
    if (keep) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < n && i < CPU_SETSIZE; i++) CPU_SET((int)i, &set);
    sched_setaffinity(0, sizeof set, &set);
}

#else

void re5_perf_note_present(uint64_t idle_ns, uint64_t present_ns) { (void)idle_ns; (void)present_ns; }
void re5_perf_note_render(void) {}
void re5_perf_thread_start(void) {}

#endif
