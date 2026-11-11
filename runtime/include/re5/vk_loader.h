/*
 * vk_loader.h - carregamento centralizado do driver Vulkan (sistema ou AdrenoTools).
 */
#ifndef RE5_VK_LOADER_H
#define RE5_VK_LOADER_H

#include <stddef.h>

typedef struct re5_vk_driver_cfg {
    int  custom_requested;
    int  turbo;
    char driver_dir[512];
    char driver_lib[128];
    char hook_dir[512];
    char tmp_dir[512];
    char redirect_dir[512];
} re5_vk_driver_cfg;

/* Copia src para dst garantindo '/' final para concatenação segura no adrenotools.
 * Retorna 0 em sucesso, -1 se vazio/nulo ou maior que cap. */
int re5_vk_format_dir_slash(char *dst, size_t cap, const char *src);

/* Valida e normaliza as variáveis de ambiente de driver customizado e modo Turbo GPU. */
void re5_vk_parse_driver_env(const char *driver_path, const char *driver_lib,
                             const char *hook_dir, const char *tmp_dir,
                             const char *redirect_dir, const char *turbo,
                             re5_vk_driver_cfg *out);

/* Devolve PFN_vkGetInstanceProcAddr do driver Vulkan ativo (sistema ou customizado). */
void *re5_vk_get_instance_proc_addr(void);

/* 1 se o driver ativo foi carregado por libadrenotools como driver customizado. */
int re5_vk_is_custom_driver(void);

#endif /* RE5_VK_LOADER_H */
