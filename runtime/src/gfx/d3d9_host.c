/*
 * d3d9_host.c - o Direct3D 9 do host atrás da ponte COM (etapa 4.2).
 *
 *   Windows x64: Vulkan pela DXVK mais nova que a GPU aceita (3.1.1 ou 2.6.2,
 *                em dxvk\<versão>\d3d9.dll ao lado do re5.exe, de
 *                scripts/build_dxvk.sh), o d3d9.dll do sistema sem nenhuma, ou
 *                o caminho em RE5_D3D9_DLL. A janela é o HWND nativo da janela
 *                SDL2 (SDL_GetWindowWMInfo).
 *   Linux:       a DXVK native (libdxvk_d3d9.so, ou RE5_D3D9_LIB) com o WSI
 *                do SDL2: a "HWND" que ela recebe é o próprio SDL_Window*.
 *   Android:     a DXVK native 1.9.2b de scripts/build_dxvk_android.sh, no
 *                jniLibs do app; a "HWND" é o ANativeWindow* da janela SDL2.
 *
 * Falha ao carregar aborta com o motivo (D8): sem Direct3D do host não há o
 * que o guest possa fazer com o NULL de um Direct3DCreate9.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include "re5/runtime.h"
#include "re5/d3d9_bridge.h"
#include "re5/vk_loader.h"
#include "re5/vk2.h"
#include "re5/user32.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__ANDROID__)
#include <errno.h>
#include <sys/stat.h>
#endif

typedef void *(
#if defined(RE5_HOST_WINDOWS)
    WINAPI
#endif
    *create9_fn)(unsigned sdk);

static create9_fn g_create9;

#if defined(RE5_HOST_WINDOWS)
/* Qual DXVK a GPU aguenta. Uma DXVK que recusa a GPU lança uma exceção C++ de
 * dentro do Direct3DCreate9 que nada pega, então antes de carregar uma o
 * runtime sonda o vulkan-1.dll e escolhe a mais nova que serve:
 *   3.1.1  Vulkan 1.3+ e as extensões obrigatórias da 3.x (maintenance5/6,
 *          load_store_op_none, robustness2, depth_clip_enable, transform_feedback);
 *   2.6.2  Vulkan 1.3 e robustness2 (maintenance5 ainda opcional). Medido: AMD
 *          Radeon Vega do Ryzen 7 5700U, driver 31.0.21924.61 (Vulkan 1.3.260),
 *          sem maintenance5 nem load_store_op_none - o ramo de manutenção da AMD.
 * Não é a lista inteira de recursos da DXVK (dxvk_device_info.cpp), só o que
 * separa as gerações de driver. */
typedef struct { int sType; const void *next; const char *app; uint32_t appv; const char *eng; uint32_t engv; uint32_t api; } vk_app_info;
typedef struct { int sType; const void *next; uint32_t flags; const vk_app_info *app; uint32_t nlayers; const char *const *layers; uint32_t nexts; const char *const *exts; } vk_instance_info;
typedef struct { char name[256]; uint32_t spec; } vk_ext;
typedef struct { uint32_t api, driver, vendor, device, type; char name[256]; uint8_t rest[1024]; } vk_props;   /* VkPhysicalDeviceProperties: só o começo é lido */
typedef void (*vk_fn)(void);
typedef vk_fn (WINAPI *vk_gipa_fn)(void *, const char *);
typedef int (WINAPI *vk_create_fn)(const vk_instance_info *, const void *, void **);
typedef void (WINAPI *vk_destroy_fn)(void *, const void *);
typedef int (WINAPI *vk_enum_pd_fn)(void *, uint32_t *, void **);
typedef void (WINAPI *vk_props_fn)(void *, vk_props *);
typedef int (WINAPI *vk_enum_ext_fn)(void *, const char *, uint32_t *, vk_ext *);

typedef struct { const char *version; const char *const *need; } dxvk_tier;
static const char *const k_need_3[] = {"VK_KHR_swapchain", "VK_KHR_maintenance5", "VK_KHR_maintenance6",
                                       "VK_KHR_load_store_op_none", "VK_EXT_robustness2", "VK_EXT_depth_clip_enable",
                                       "VK_EXT_transform_feedback", NULL};
static const char *const k_need_2[] = {"VK_KHR_swapchain", "VK_EXT_robustness2", NULL};
static const dxvk_tier k_tiers[] = {{"3.1.1", k_need_3}, {"2.6.2", k_need_2}};
#define N_TIERS (sizeof k_tiers / sizeof k_tiers[0])

static int has_ext(const vk_ext *ext, uint32_t n, const char *name)
{
    for (uint32_t j = 0; ext && j < n; j++)
        if (strcmp(ext[j].name, name) == 0) return 1;
    return 0;
}

/* Índice em k_tiers da DXVK mais nova que alguma GPU aceita, ou -1. */
static int best_dxvk_tier(void)
{
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { RE5_WARN("d3d9", "sem vulkan-1.dll: a DXVK não tem como rodar"); return -1; }
    vk_gipa_fn gipa = (vk_gipa_fn)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr");
    vk_create_fn create = gipa ? (vk_create_fn)(void *)gipa(NULL, "vkCreateInstance") : NULL;
    if (!create) { RE5_WARN("d3d9", "vulkan-1.dll sem vkCreateInstance"); return -1; }
    vk_app_info app = {0 /* VK_STRUCTURE_TYPE_APPLICATION_INFO */, NULL, "re5", 1, "re5", 1, (1u << 22) | (3u << 12)};
    vk_instance_info ci = {1 /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */, NULL, 0, &app, 0, NULL, 0, NULL};
    void *inst = NULL;
    if (create(&ci, NULL, &inst) != 0 || !inst) { RE5_WARN("d3d9", "vkCreateInstance falhou"); return -1; }
    vk_destroy_fn destroy = (vk_destroy_fn)(void *)gipa(inst, "vkDestroyInstance");
    vk_enum_pd_fn enum_pd = (vk_enum_pd_fn)(void *)gipa(inst, "vkEnumeratePhysicalDevices");
    vk_props_fn props = (vk_props_fn)(void *)gipa(inst, "vkGetPhysicalDeviceProperties");
    vk_enum_ext_fn enum_ext = (vk_enum_ext_fn)(void *)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    void *pd[8];
    uint32_t npd = 8;
    int best = -1;
    if (enum_pd && props && enum_ext && enum_pd(inst, &npd, pd) >= 0) {
        for (uint32_t i = 0; i < npd; i++) {
            vk_props p;
            props(pd[i], &p);
            uint32_t next = 0;
            enum_ext(pd[i], NULL, &next, NULL);
            vk_ext *ext = calloc(next ? next : 1, sizeof *ext);
            if (ext) enum_ext(pd[i], NULL, &next, ext);
            int v13 = (p.api >> 22) > 1 || ((p.api >> 22) == 1 && ((p.api >> 12) & 0x3FF) >= 3);
            for (int t = 0; t < (int)N_TIERS; t++) {
                const char *missing = v13 ? NULL : "Vulkan 1.3";
                for (const char *const *n = k_tiers[t].need; !missing && *n; n++)
                    if (!has_ext(ext, next, *n)) missing = *n;
                if (!missing) {
                    RE5_INFO("d3d9", "GPU '%s' (Vulkan %u.%u.%u, driver 0x%X): serve à DXVK %s", p.name, p.api >> 22,
                             (p.api >> 12) & 0x3FF, p.api & 0xFFF, p.driver, k_tiers[t].version);
                    if (best < 0 || t < best) best = t;
                    break;
                }
                RE5_INFO("d3d9", "GPU '%s': sem %s, não serve à DXVK %s", p.name, missing, k_tiers[t].version);
            }
            free(ext);
        }
    }
    if (destroy) destroy(inst, NULL);
    return best;
}
#endif

static void load(void)
{
    if (g_create9) return;
    re5_gfx_backend gfx = re5_gfx_mode();
#if defined(RE5_HOST_WINDOWS)
    /* Padrão: Vulkan pela DXVK mais nova que a GPU aceita, em
     * dxvk\<versão>\d3d9.dll ao lado do re5.exe (o build copia de
     * scripts/build_dxvk.sh); sem nenhuma, o d3d9.dll do sistema.
     * RE5_DXVK=<versão> escolhe a versão; RE5_D3D9_DLL, a DLL inteira. */
    const char *path = getenv("RE5_D3D9_DLL");
    if (gfx == RE5_GFX_GL && (!path || !*path)) {
        re5_fatal("d3d9",
                  "RE5_GFX=gl requer um tradutor D3D9->OpenGL externo em RE5_D3D9_DLL. "
                  "O runtime embutido suporta a DXVK.");
    }
    char dxvk[MAX_PATH];
    if (!path || !*path) {
        path = "d3d9.dll";
        const char *want = getenv("RE5_DXVK");
        int t = -1;
        if (want && *want) {
            for (int i = 0; i < (int)N_TIERS; i++)
                if (strcmp(want, k_tiers[i].version) == 0) t = i;
            if (t < 0) re5_fatal("d3d9", "RE5_DXVK=%s: versões compiladas: 3.1.1, 2.6.2", want);
        } else {
            t = best_dxvk_tier();
        }
        DWORD n = GetModuleFileNameA(NULL, dxvk, sizeof dxvk);
        char *slash = n && n < sizeof dxvk ? strrchr(dxvk, '\\') : NULL;
        if (t < 0) {
            RE5_WARN("d3d9", "nenhuma GPU serve a uma DXVK compilada: usando o d3d9.dll do sistema (sem Vulkan)");
        } else if (slash && (size_t)(slash - dxvk) + 32 < sizeof dxvk) {
            snprintf(slash, sizeof dxvk - (size_t)(slash - dxvk), "\\dxvk\\%s\\d3d9.dll", k_tiers[t].version);
            if (GetFileAttributesA(dxvk) == INVALID_FILE_ATTRIBUTES)
                RE5_WARN("d3d9", "%s não existe (scripts/build_dxvk.sh %s): usando o d3d9.dll do sistema", dxvk,
                         k_tiers[t].version);
            else
                path = dxvk;
        }
    }
    HMODULE dll = LoadLibraryA(path);
    if (!dll) re5_fatal("d3d9", "LoadLibrary(\"%s\") falhou (erro %lu)", path, (unsigned long)GetLastError());
    char full[MAX_PATH] = "?";
    GetModuleFileNameA(dll, full, sizeof full);
    g_create9 = (create9_fn)(void *)GetProcAddress(dll, "Direct3DCreate9");
    if (!g_create9) re5_fatal("d3d9", "%s não exporta Direct3DCreate9", full);
    RE5_INFO("d3d9", "Direct3D do host: %s", full);
#else
    const char *path = getenv("RE5_D3D9_LIB");
    if (gfx == RE5_GFX_GL) {
        if (!path || !*path) {
            re5_fatal("d3d9",
                      "RE5_GFX=gl requer um tradutor D3D9->OpenGL ES externo em RE5_D3D9_LIB "
                      "(ex.: libwined3d/Nine). O runtime embutido suporta dxvk, vkadreno e vkmali.");
        }
        void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!lib) re5_fatal("d3d9", "dlopen(\"%s\") falhou: %s", path, dlerror());
        g_create9 = (create9_fn)dlsym(lib, "Direct3DCreate9");
        if (!g_create9) re5_fatal("d3d9", "%s não exporta Direct3DCreate9", path);
        RE5_INFO("d3d9", "Direct3D do host (RE5_GFX=gl externo): %s", path);
        return;
    }
    void *lib = NULL;
    if (!path || !*path) path = "libdxvk_d3d9.so";
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);   /* a janela que a ponte entrega é um SDL_Window* */
#if defined(__ANDROID__)
    /* Sem DXVK_STATE_CACHE_PATH a DXVK tenta criar o diretório "" ao lado do
     * "executável" (app_process64) e aborta no CreateDevice com um
     * filesystem_error. O cache (estados e VkPipelineCache) vai para a pasta
     * interna do app. */
    if (!getenv("DXVK_STATE_CACHE_PATH")) {
        const char *base = SDL_AndroidGetInternalStoragePath();
        if (!base) re5_fatal("d3d9", "SDL_AndroidGetInternalStoragePath falhou: %s", SDL_GetError());
        char cache[1024];
        snprintf(cache, sizeof cache, "%s/dxvk", base);
        if (mkdir(cache, 0700) != 0 && errno != EEXIST) re5_fatal("d3d9", "mkdir(\"%s\") falhou: %s", cache, strerror(errno));
        setenv("DXVK_STATE_CACHE_PATH", cache, 1);
        RE5_INFO("d3d9", "cache da DXVK em %s", cache);
    }
#endif
    if (!lib) lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) re5_fatal("d3d9", "dlopen(\"%s\") falhou: %s", path, dlerror());
    g_create9 = (create9_fn)dlsym(lib, "Direct3DCreate9");
    if (!g_create9) re5_fatal("d3d9", "%s não exporta Direct3DCreate9", path);
#if defined(__ANDROID__)
    void *vk_gipa = re5_vk_get_instance_proc_addr();
    if (re5_vk_is_custom_driver() && vk_gipa) {
        void (*set_gipa)(void *) = (void (*)(void *))dlsym(lib, "dxvkAndroidSetInstanceProcAddr");
        if (set_gipa) {
            set_gipa(vk_gipa);
            RE5_INFO("d3d9", "DXVK usando driver Vulkan customizado via adrenotools");
        } else {
            RE5_WARN("d3d9", "%s não exporta dxvkAndroidSetInstanceProcAddr; DXVK usará driver do sistema", path);
        }
    }
    /* O WSI do Android (android/dxvk/android_wsi.cpp) lista os modos do
     * adaptador a partir do tamanho da tela, antes de existir device. */
    void (*set_window_fn)(void *(*)(void)) = (void (*)(void *(*)(void)))dlsym(lib, "re5_dxvk_set_window_fn");
    if (set_window_fn) set_window_fn(re5_android_current_window);
    else RE5_WARN("d3d9", "%s sem re5_dxvk_set_window_fn: voltar do segundo plano pode derrubar o jogo", path);
    void (*set_screen)(uint32_t, uint32_t) = (void (*)(uint32_t, uint32_t))dlsym(lib, "re5_dxvk_set_screen");
    if (!set_screen) re5_fatal("d3d9", "%s não exporta re5_dxvk_set_screen (não é a DXVK do Android)", path);
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        uint32_t sw = (uint32_t)dm.w, sh = (uint32_t)dm.h;
        if (sw < sh) { uint32_t t = sw; sw = sh; sh = t; }
        set_screen(sw, sh);
        RE5_INFO("d3d9", "tela do Android: %dx%d (landscape)", sw, sh);
    }
#endif
    RE5_INFO("d3d9", "Direct3D do host: %s (DXVK native)", path);
#endif
}

/* RE5_GFX: "" ou "dxvk" = DXVK (padrão); "vk2" = Vulkan 1.1 nativo (runtime/src/gfx/vk2, o device é nosso e a DXVK
 * só responde as consultas do adaptador); "gl" = tradutor D3D9->GL externo (RE5_D3D9_LIB / RE5_D3D9_DLL).
 * Os backends Vulkan nativos (vkmali, vkadreno, re5vk) saíram em 23/11/2026: o código está na tag
 * base-lean-2026-11-23. Valor desconhecido aborta (D8: falhar alto). */
int re5_parse_gfx_mode(const char *env, re5_gfx_backend *out)
{
    if (!env || !*env || strcmp(env, "dxvk") == 0) {
        if (out) *out = RE5_GFX_DXVK;
        return 0;
    }
    if (strcmp(env, "gl") == 0) {
        if (out) *out = RE5_GFX_GL;
        return 0;
    }
    if (strcmp(env, "vk2") == 0) {
        if (out) *out = RE5_GFX_VK2;
        return 0;
    }
    return -1;
}

re5_gfx_backend re5_gfx_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RE5_GFX");
        re5_gfx_backend m;
        if (re5_parse_gfx_mode(e, &m) != 0)
            re5_fatal("d3d9", "RE5_GFX=%s: use dxvk, vk2 ou gl (vkmali, vkadreno e re5vk foram removidos)", e);
        mode = (int)m;
    }
    return (re5_gfx_backend)mode;
}

static void *host_create9(uint32_t sdk)
{
    load();
    void *d3d = g_create9(sdk);
    if (re5_gfx_mode() == RE5_GFX_VK2) return vk2_wrap_d3d9(d3d);
    return d3d;
}

/* ---- janela do Android em segundo plano ----
 * Ao sair do primeiro plano o Android destrói a superfície (SDLSurface.surfaceDestroyed) e o SDL
 * solta o ANativeWindow; ao voltar dá OUTRO ANativeWindow. A DXVK guardava o da criação
 * do device e o processo caía na volta (uso de janela liberada / swapchain sem superfície). Agora:
 *  - o Java fecha o portão antes de a superfície morrer (espera o Present em curso acabar) e o abre
 *    quando a nova existe (SDLActivity.nativeSurfaceGate);
 *  - o Present do jogo espera com o portão fechado (o jogo fica parado em segundo plano);
 *  - quem cria superfície pede a janela atual (re5_android_current_window), nunca a guardada. */
#if defined(__ANDROID__)
#include <pthread.h>
#include <jni.h>
#include <time.h>

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_closed, g_in_present;
static uint32_t g_gate_gen;
static SDL_Window *g_game_window;

void *re5_android_current_window(void)
{
    SDL_Window *w = g_game_window;
    if (!w) return NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_ANDROID) return NULL;
    return info.info.android.window;
}

uint32_t re5_android_surface_gen(void) { return __atomic_load_n(&g_gate_gen, __ATOMIC_ACQUIRE); }

void re5_android_present_enter(void)
{
    pthread_mutex_lock(&g_gate_mu);
    if (g_gate_closed) {
        RE5_INFO("d3d9", "segundo plano: Present parado até a janela voltar");
        while (g_gate_closed) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
        RE5_INFO("d3d9", "janela de volta: Present segue (superfície %u)", g_gate_gen);
    }
    g_in_present = 1;
    pthread_mutex_unlock(&g_gate_mu);
}

void re5_android_present_leave(void)
{
    pthread_mutex_lock(&g_gate_mu);
    g_in_present = 0;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeSurfaceGate(JNIEnv *env, jclass cls, jint open)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g_gate_mu);
    if (!open) {
        if (!g_gate_closed) {
            re5_dsound_pause(1);
            re5_k32_clock_pause(1);
        }
        g_gate_closed = 1;
        /* espera o Present em curso (no máximo 3 s: um driver preso não segura a UI até o ANR) */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        while (g_in_present)
            if (pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &ts) != 0) {
                RE5_WARN("d3d9", "superfície destruída com um Present ainda em curso");
                break;
            }
    } else if (g_gate_closed) {
        re5_k32_clock_pause(0);
        re5_dsound_pause(0);
        g_gate_closed = 0;
        __atomic_add_fetch(&g_gate_gen, 1, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&g_gate_cv);
    }
    pthread_mutex_unlock(&g_gate_mu);
}
#endif

void re5_android_frame_rate(void *anw)
{
#if defined(__ANDROID__)
    static void *last;
    static float rate = -1.0f;
    static int (*set_rate)(void *, float, int8_t);
    if (rate < 0.0f) {
        const char *e = getenv("RE5_FRAME_RATE");
        rate = e ? (float)atof(e) : 60.0f;
        if (rate < 0.0f) rate = 0.0f;
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        set_rate = lib ? (int (*)(void *, float, int8_t))dlsym(lib, "ANativeWindow_setFrameRate") : NULL;
    }
    if (!anw || anw == last || rate == 0.0f || !set_rate) return;
    last = anw;
    int r = set_rate(anw, rate, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
    RE5_INFO("d3d9", "ANativeWindow_setFrameRate(%.0f, FIXED_SOURCE): %s (RE5_FRAME_RATE=0 desliga)", rate,
             r == 0 ? "ok" : "recusado");
#else
    (void)anw;
#endif
}

static void *host_window(uint32_t hwnd)
{
    SDL_Window *w = re5_user32_sdl_window(hwnd);
    if (!w) return NULL;
#if defined(RE5_HOST_WINDOWS)
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        re5_fatal("d3d9", "SDL_GetWindowWMInfo da janela 0x%X falhou: %s", hwnd, SDL_GetError());
    return info.info.win.window;
#elif defined(__ANDROID__)
    /* A DXVK do Android não usa o SDL (que está estático na libre5.so): o WSI
     * dela cria a superfície Vulkan direto do ANativeWindow da janela. */
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    g_game_window = w;
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_ANDROID || !info.info.android.window)
        re5_fatal("d3d9", "janela 0x%X sem ANativeWindow (SDL_GetWindowWMInfo: %s)", hwnd, SDL_GetError());
    re5_android_frame_rate(info.info.android.window);
    return info.info.android.window;
#else
    return w;
#endif
}

static const re5_d3d9_backend k_host = {
#if defined(RE5_HOST_WINDOWS)
    "d3d9.dll do host",
#else
    "DXVK native",
#endif
    host_create9,
    host_window,
};

const re5_d3d9_backend *re5_d3d9_host_backend(void) { return &k_host; }
