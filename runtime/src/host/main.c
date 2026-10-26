/*
 * main.c - executável host `re5`: carrega o re5dx9.exe e roda o código gerado.
 *
 *   re5 [--exe PATH] [--game-dir DIR] [--user-dir DIR] [--census] [--missing]
 *
 *   --exe       executável limpo do jogo (padrão: bin_input/re5dx9.clean.exe)
 *   --game-dir  pasta de instalação com nativePC/ (padrão: pasta do --exe)
 *   --census    só carrega a imagem e imprime quantos imports têm HLE
 *   --missing   com --census, lista os imports que faltam
 *
 * Sequência: memória do guest -> thunks -> imagem em 0x00400000 (IAT ligada
 * a thunks) -> PEB -> thread principal do guest numa thread do host com pilha
 * grande -> entry point (WinMainCRTStartup) pelo dispatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "re5/runtime.h"

#define HOST_STACK_BYTES ((size_t)256 << 20)   /* o código gerado aninha uma chamada C por chamada do guest */
#define GUEST_MAIN_STACK (64u << 20)

typedef struct {
    uint32_t entry;
    uint32_t result;
    int      ok;
} main_args;

static void guest_main(void *p)
{
    main_args *a = p;
    re5_thread *t = re5_thread_attach(GUEST_MAIN_STACK);
    if (!t) {
        RE5_ERROR("host", "sem memória do guest para a thread principal");
        return;
    }
    re5_guest_lock();
    a->result = re5_guest_call(t, a->entry, 0, NULL);
    re5_guest_unlock();
    a->ok = 1;
}

static void usage(void)
{
    fprintf(stderr, "uso: re5 [--exe PATH] [--game-dir DIR] [--user-dir DIR] [--census [--missing]]\n");
}

static int re5_main_entry(int argc, char **argv)
{
    const char *exe = "bin_input/re5dx9.clean.exe", *game_dir = NULL, *user_dir = NULL;
    int census = 0, missing = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) game_dir = argv[++i];
        else if (!strcmp(argv[i], "--user-dir") && i + 1 < argc) user_dir = argv[++i];
        else if (!strcmp(argv[i], "--census")) census = 1;
        else if (!strcmp(argv[i], "--missing")) missing = 1;
        else { usage(); return 2; }
    }

    SDL_SetMainReady();
    if (re5_runtime_init() != 0) return 1;

    char dir[1024];
    if (!game_dir) {
        snprintf(dir, sizeof dir, "%s", exe);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
        game_dir = dir;
    }
    re5_vfs_init(game_dir, user_dir);

    re5_image img;
    const char *err = NULL;
    if (re5_image_load(exe, &img, &err) != 0) {
        RE5_ERROR("host", "%s: %s", exe, err);
        return 1;
    }
    if (census) {
        re5_import_census_print(stdout, missing);
        return 0;
    }
    if (re5_lifted_count == 0) {
        RE5_ERROR("host", "nenhum código gerado foi linkado (generated/src vazio). "
                          "Rode a Fase 2: scripts/run_lift.py catalog && scripts/run_lift.py lift, e recompile.");
        return 3;
    }
    if (re5_process_init(img.base) != 0) return 1;

    main_args a = {img.entry, 0, 0};
    if (re5_host_thread_run(guest_main, &a, HOST_STACK_BYTES) != 0 || !a.ok) {
        RE5_ERROR("host", "a thread principal do guest não terminou normalmente");
        return 1;
    }
    RE5_INFO("host", "entry point retornou 0x%08X", a.result);
    return (int)a.result;
}

int main(int argc, char **argv)
{
    return re5_main_entry(argc, argv);
}

int SDL_main(int argc, char **argv)
{
    return re5_main_entry(argc, argv);
}
