/*
 * vk_loader.c - carregador compartilhado entre re5vk e DXVK no Android.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/log.h"
#include "re5/platform.h"
#include "re5/vk_loader.h"

#if defined(RE5_HOST_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#if defined(RE5_HAS_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif

int re5_vk_format_dir_slash(char *dst, size_t cap, const char *src)
{
    if (!dst || cap < 2) return -1;
    dst[0] = '\0';
    if (!src || !*src) return -1;
    size_t len = strlen(src);
    int need_slash = (src[len - 1] != '/' && src[len - 1] != '\\');
    if (len + (size_t)need_slash + 1 > cap) return -1;
    memcpy(dst, src, len);
    if (need_slash) dst[len++] = '/';
    dst[len] = '\0';
    return 0;
}

static int valid_lib_name(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (*p == '/' || *p == '\\') return 0;
    return 1;
}

void re5_vk_parse_driver_env(const char *driver_path, const char *driver_lib,
                             const char *hook_dir, const char *tmp_dir,
                             const char *redirect_dir, const char *turbo,
                             re5_vk_driver_cfg *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->turbo = (turbo && turbo[0] == '1');

    if (tmp_dir && *tmp_dir)
        re5_vk_format_dir_slash(out->tmp_dir, sizeof out->tmp_dir, tmp_dir);
    if (redirect_dir && *redirect_dir)
        re5_vk_format_dir_slash(out->redirect_dir, sizeof out->redirect_dir, redirect_dir);

    if (!driver_path || !*driver_path || !driver_lib || !*driver_lib || !hook_dir || !*hook_dir)
        return;
    if (!valid_lib_name(driver_lib) || strlen(driver_lib) >= sizeof out->driver_lib)
        return;
    if (re5_vk_format_dir_slash(out->driver_dir, sizeof out->driver_dir, driver_path) != 0)
        return;
    if (strlen(hook_dir) >= sizeof out->hook_dir)
        return;

    snprintf(out->driver_lib, sizeof out->driver_lib, "%s", driver_lib);
    snprintf(out->hook_dir, sizeof out->hook_dir, "%s", hook_dir);
    out->custom_requested = 1;
}

static void *g_gipa;
static int   g_custom;

static void init_loader_impl(void)
{
    re5_vk_driver_cfg cfg;
    const char *env_path = getenv("RE5VK_DRIVER_PATH");
    const char *env_lib  = getenv("RE5VK_DRIVER_LIB");
    re5_vk_parse_driver_env(env_path, env_lib, getenv("RE5VK_HOOK_DIR"),
                            getenv("RE5VK_DRIVER_TMP"), getenv("RE5VK_DRIVER_REDIRECT"),
                            getenv("RE5VK_ADRENO_TURBO"), &cfg);

#if defined(RE5_HAS_ADRENOTOOLS)
    if (cfg.turbo) {
        adrenotools_set_turbo(true);
        RE5_INFO("vk", "Adreno Turbo GPU ativado (KGSL_PROP_PWRCTRL=0)");
    }

    if (cfg.custom_requested) {
        int flags = ADRENOTOOLS_DRIVER_CUSTOM;
        const char *redir = NULL;
        if (cfg.redirect_dir[0]) {
            flags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;
            redir = cfg.redirect_dir;
        }
        const char *tmp = cfg.tmp_dir[0] ? cfg.tmp_dir : NULL;
        void *lib = adrenotools_open_libvulkan(RTLD_NOW, flags, tmp, cfg.hook_dir,
                                               cfg.driver_dir, cfg.driver_lib, redir, NULL);
        if (lib) {
            void *gipa = dlsym(lib, "vkGetInstanceProcAddr");
            if (gipa) {
                g_gipa = gipa;
                g_custom = 1;
                RE5_INFO("vk", "driver Vulkan customizado carregado: %s%s", cfg.driver_dir, cfg.driver_lib);
                return;
            }
            RE5_WARN("vk", "driver customizado '%s%s' não expôs vkGetInstanceProcAddr; voltando ao sistema",
                     cfg.driver_dir, cfg.driver_lib);
            dlclose(lib);
        } else {
            RE5_WARN("vk", "adrenotools falhou ao carregar '%s%s' (hookDir=%s); voltando ao sistema",
                     cfg.driver_dir, cfg.driver_lib, cfg.hook_dir);
        }
    } else if ((env_path && *env_path) || (env_lib && *env_lib)) {
        RE5_WARN("vk", "variáveis de driver customizado incompletas/inválidas; usando driver do sistema");
    }
#endif

#if defined(RE5_HOST_WINDOWS)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (lib) g_gipa = (void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
#elif defined(RE5_HOST_ANDROID)
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (lib) g_gipa = dlsym(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) g_gipa = dlsym(lib, "vkGetInstanceProcAddr");
#endif
}

#if defined(RE5_HOST_WINDOWS)
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK win_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    init_loader_impl();
    return TRUE;
}
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
#endif

void *re5_vk_get_instance_proc_addr(void)
{
#if defined(RE5_HOST_WINDOWS)
    InitOnceExecuteOnce(&g_once, win_once_cb, NULL, NULL);
#else
    pthread_once(&g_once, init_loader_impl);
#endif
    return g_gipa;
}

int re5_vk_is_custom_driver(void)
{
    (void)re5_vk_get_instance_proc_addr();
    return g_custom;
}
