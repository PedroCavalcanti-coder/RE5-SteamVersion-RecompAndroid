/*
 * test_vk_loader.c - testes unitários do parser/formatador de driver Vulkan customizado.
 */
#include <string.h>

#include "check.h"
#include "re5/vk_loader.h"

int main(void)
{
    char buf[16];
    re5_vk_driver_cfg cfg;

    /* 1. Barra final adicionada e idempotente (inclusive barra invertida). */
    CHECK(re5_vk_format_dir_slash(buf, sizeof buf, "/data/turnip") == 0);
    CHECK(strcmp(buf, "/data/turnip/") == 0);

    CHECK(re5_vk_format_dir_slash(buf, sizeof buf, "/data/turnip/") == 0);
    CHECK(strcmp(buf, "/data/turnip/") == 0);

    CHECK(re5_vk_format_dir_slash(buf, sizeof buf, "C:\\drv\\") == 0);
    CHECK(strcmp(buf, "C:\\drv\\") == 0);

    /* 2. Entradas inválidas e overflow no buffer de destino. */
    CHECK(re5_vk_format_dir_slash(NULL, sizeof buf, "/a") == -1);
    CHECK(re5_vk_format_dir_slash(buf, 1, "/a") == -1);
    CHECK(re5_vk_format_dir_slash(buf, sizeof buf, NULL) == -1);
    CHECK(buf[0] == '\0');
    CHECK(re5_vk_format_dir_slash(buf, sizeof buf, "") == -1);
    CHECK(buf[0] == '\0');
    /* "1234" -> "1234/" precisa de 6 bytes; cap=5 deve falhar com segurança. */
    CHECK(re5_vk_format_dir_slash(buf, 5, "1234") == -1);
    CHECK(re5_vk_format_dir_slash(buf, 6, "1234") == 0);
    CHECK(strcmp(buf, "1234/") == 0);

    /* 3. Configuração completa válida + modo turbo + redirect/tmp normalizados. */
    re5_vk_parse_driver_env("/data/user/0/com.re5/files/gpu_drivers/turnip",
                            "libvulkan_freedreno.so",
                            "/data/app/com.re5/lib/arm64",
                            "/data/user/0/com.re5/cache/drv_tmp",
                            "/sdcard/redirect",
                            "1", &cfg);
    CHECK(cfg.custom_requested == 1);
    CHECK(cfg.turbo == 1);
    CHECK(strcmp(cfg.driver_dir, "/data/user/0/com.re5/files/gpu_drivers/turnip/") == 0);
    CHECK(strcmp(cfg.driver_lib, "libvulkan_freedreno.so") == 0);
    CHECK(strcmp(cfg.hook_dir, "/data/app/com.re5/lib/arm64") == 0);
    CHECK(strcmp(cfg.tmp_dir, "/data/user/0/com.re5/cache/drv_tmp/") == 0);
    CHECK(strcmp(cfg.redirect_dir, "/sdcard/redirect/") == 0);

    /* 4. Campos ausentes não ativam driver customizado; turbo independe de driver customizado. */
    re5_vk_parse_driver_env(NULL, "libvulkan.so", "/hooks", NULL, NULL, "1", &cfg);
    CHECK(cfg.custom_requested == 0);
    CHECK(cfg.turbo == 1);

    re5_vk_parse_driver_env("/drv", NULL, "/hooks", NULL, NULL, "0", &cfg);
    CHECK(cfg.custom_requested == 0);
    CHECK(cfg.turbo == 0);

    re5_vk_parse_driver_env("/drv", "libvulkan.so", "", NULL, NULL, NULL, &cfg);
    CHECK(cfg.custom_requested == 0);
    CHECK(cfg.turbo == 0);

    /* 5. Path traversal ou subdiretório no nome da biblioteca é rejeitado. */
    re5_vk_parse_driver_env("/drv", "sub/libvulkan.so", "/hooks", NULL, NULL, NULL, &cfg);
    CHECK(cfg.custom_requested == 0);
    re5_vk_parse_driver_env("/drv", "..\\libvulkan.so", "/hooks", NULL, NULL, NULL, &cfg);
    CHECK(cfg.custom_requested == 0);

    /* 6. Overflow de path longo rejeita sem ativar. */
    char long_path[600];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    re5_vk_parse_driver_env(long_path, "libvulkan_freedreno.so", "/hooks", NULL, NULL, NULL, &cfg);
    CHECK(cfg.custom_requested == 0);

    CHECK_DONE();
}
