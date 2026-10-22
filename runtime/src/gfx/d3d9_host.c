/*
 * d3d9_host.c - o Direct3D 9 do host atrás da ponte COM (etapa 4.2).
 *
 *   Windows x64: Vulkan pela DXVK mais nova que a GPU aceita (3.1.1 ou 2.6.2,
 *                em dxvk\<versão>\d3d9.dll ao lado do re5.exe, de
 *                scripts/build_dxvk.sh), o d3d9.dll do sistema sem nenhuma, ou
 *                o caminho em RE5_D3D9_DLL. A janela é o HWND nativo da janela
 *                SDL2 (SDL_GetWindowWMInfo).
 *   Linux/Android: a DXVK native (libdxvk_d3d9.so, ou RE5_D3D9_LIB) com o WSI
 *                do SDL2: a "HWND" que ela recebe é o próprio SDL_Window*.
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
#include "re5/user32.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#else
#include <dlfcn.h>
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
#if defined(RE5_HOST_WINDOWS)
    /* Padrão: Vulkan pela DXVK mais nova que a GPU aceita, em
     * dxvk\<versão>\d3d9.dll ao lado do re5.exe (o build copia de
     * scripts/build_dxvk.sh); sem nenhuma, o d3d9.dll do sistema.
     * RE5_DXVK=<versão> escolhe a versão; RE5_D3D9_DLL, a DLL inteira. */
    const char *path = getenv("RE5_D3D9_DLL");
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
    if (!path || !*path) path = "libdxvk_d3d9.so";
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);   /* a janela que a ponte entrega é um SDL_Window* */
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) re5_fatal("d3d9", "dlopen(\"%s\") falhou: %s", path, dlerror());
    g_create9 = (create9_fn)dlsym(lib, "Direct3DCreate9");
    if (!g_create9) re5_fatal("d3d9", "%s não exporta Direct3DCreate9", path);
    RE5_INFO("d3d9", "Direct3D do host: %s (DXVK native)", path);
#endif
}

static void *host_create9(uint32_t sdk)
{
    load();
    return g_create9(sdk);
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
