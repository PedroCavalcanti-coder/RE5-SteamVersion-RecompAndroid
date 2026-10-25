/*
 * test_lan_menu.c - menu de perfil LAN (runtime/src/win32/lan_menu.c) sem GPU:
 * eventos do SDL montados à mão e a geometria que o menu desenharia.
 *
 * argv[1]: arquivo de identidade (também em RE5_LAN_CONFIG).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "check.h"
#include "re5/lan_menu.h"
#include "re5/runtime.h"
#include "re5/steam_lan.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;

static uint32_t flat(const char *name, int n, const uint32_t *args)
{
    return re5_guest_call(T, re5_import_bind("steam_api.dll", name, 0), n, args);
}

static int key(SDL_Scancode sc, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.scancode = sc;
    return re5_lan_menu_event(&e);
}

static int press(SDL_Scancode sc) { return key(sc, 1); }

static int type(const char *s)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", s);
    return re5_lan_menu_event(&e);
}

static int finger(int down, int id, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_FINGERDOWN : SDL_FINGERUP;
    e.tfinger.fingerId = id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    return re5_lan_menu_event(&e);
}

static void clear_field(void)
{
    for (int i = 0; i < 40; i++) press(SDL_SCANCODE_BACKSPACE);
}

static void read_file(const char *path, char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    out[fread(out, 1, cap - 1, f)] = 0;
    fclose(f);
}

int main(int argc, char **argv)
{
    CHECK(argc > 1);
    if (argc < 2) return 1;
    remove(argv[1]);
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);

    /* Fechado: o jogo recebe tudo, menos o F12. */
    CHECK(!re5_lan_menu_is_open());
    CHECK(!press(SDL_SCANCODE_W));
    CHECK(!type("w"));
    CHECK(press(SDL_SCANCODE_F12));
    CHECK(re5_lan_menu_is_open());
    CHECK(key(SDL_SCANCODE_F12, 0));

    /* Aberto: teclas e texto são do menu; soltar tecla continua indo ao jogo. */
    CHECK(press(SDL_SCANCODE_W));
    CHECK(!key(SDL_SCANCODE_W, 0));

    /* Nome: '#' (separador do ID) e não-ASCII ficam de fora. */
    clear_field();
    CHECK(type("Ada#Wong"));
    CHECK(type("\xC3\xA9"));
    press(SDL_SCANCODE_TAB);   /* -> país */
    press(SDL_SCANCODE_RIGHT);
    type("p");
    type("t");                 /* código digitado: PT */
    press(SDL_SCANCODE_TAB);   /* -> IP do host */
    type("192.168.43.1x");     /* 'x' não entra */
    CHECK(press(SDL_SCANCODE_RETURN));
    CHECK(!re5_lan_menu_is_open());

    CHECK(strcmp(steam_lan_get_local_name(), "AdaWong") == 0);
    char cc[4];
    steam_lan_get_local_country(cc);
    CHECK(strcmp(cc, "PT") == 0);
    char hosts[64];
    steam_lan_get_manual_hosts(hosts, sizeof hosts);
    CHECK(strcmp(hosts, "192.168.43.1") == 0);
    uint32_t utils = flat("SteamUtils", 0, NULL);
    T->cpu.ecx = utils;
    CHECK(strcmp(re5_gstr(re5_guest_call(T, rd32(rd32(utils) + 4 * 4), 0, NULL)), "PT") == 0);
    char text[1024];
    read_file(argv[1], text, sizeof text);
    CHECK(strstr(text, "\nnome=AdaWong\n") != NULL);
    CHECK(strstr(text, "\npais=PT\n") != NULL);
    CHECK(strstr(text, "\nhosts=192.168.43.1\n") != NULL);

    /* Nome vazio não salva: o menu fica aberto com o aviso. */
    press(SDL_SCANCODE_F12);
    CHECK(re5_lan_menu_is_open());
    clear_field();
    press(SDL_SCANCODE_RETURN);
    CHECK(re5_lan_menu_is_open());
    CHECK(strcmp(steam_lan_get_local_name(), "AdaWong") == 0);
    /* Esc fecha sem salvar. */
    type("Outro");
    press(SDL_SCANCODE_ESCAPE);
    CHECK(!re5_lan_menu_is_open());
    CHECK(strcmp(steam_lan_get_local_name(), "AdaWong") == 0);

    /* IP inválido também segura o menu. */
    press(SDL_SCANCODE_F12);
    press(SDL_SCANCODE_UP);    /* nome -> (volta) Fechar */
    press(SDL_SCANCODE_UP);    /* -> Salvar */
    press(SDL_SCANCODE_UP);    /* -> IP do host */
    clear_field();
    type("999.");
    press(SDL_SCANCODE_RETURN);
    CHECK(re5_lan_menu_is_open());
    clear_field();
    press(SDL_SCANCODE_RETURN);   /* vazio = procurar na rede */
    CHECK(!re5_lan_menu_is_open());
    steam_lan_get_manual_hosts(hosts, sizeof hosts);
    CHECK(hosts[0] == 0);

    /* Celular: três dedos abrem; os eventos do gesto não vão ao jogo. */
    CHECK(!finger(1, 1, 0.2f, 0.5f));
    CHECK(!finger(1, 2, 0.5f, 0.5f));
    CHECK(finger(1, 3, 0.8f, 0.5f));
    CHECK(re5_lan_menu_is_open());
    CHECK(finger(0, 1, 0.2f, 0.5f));
    CHECK(finger(0, 2, 0.5f, 0.5f));
    CHECK(finger(0, 3, 0.8f, 0.5f));
    CHECK(re5_lan_menu_is_open());

    /* Geometria: triângulos dentro da tela, em 720p e em 1080p. */
    static re5_lan_vtx v[240000];
    float sizes[2][2] = {{1280, 720}, {2400, 1080}};
    for (int k = 0; k < 2; k++) {
        uint32_t n = re5_lan_menu_build(sizes[k][0], sizes[k][1], v, 240000);
        CHECK(n > 600 && n % 3 == 0 && n < 240000);
        int inside = 1;
        for (uint32_t i = 0; i < n; i++)
            if (v[i].x < -1 || v[i].y < -1 || v[i].x > sizes[k][0] || v[i].y > sizes[k][1]) inside = 0;
        CHECK(inside);
    }

    /* Toque fora de qualquer botão não fecha; três dedos fecham. */
    CHECK(finger(1, 7, 0.01f, 0.01f));
    CHECK(finger(0, 7, 0.01f, 0.01f));
    CHECK(re5_lan_menu_is_open());
    finger(1, 1, 0.2f, 0.5f);
    finger(1, 2, 0.5f, 0.5f);
    finger(1, 3, 0.8f, 0.5f);
    finger(0, 1, 0.2f, 0.5f);
    finger(0, 2, 0.5f, 0.5f);
    finger(0, 3, 0.8f, 0.5f);
    CHECK(!re5_lan_menu_is_open());

    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
}
