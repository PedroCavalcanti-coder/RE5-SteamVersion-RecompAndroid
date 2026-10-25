/*
 * lan_menu.h - menu de perfil do multiplayer LAN desenhado por cima do jogo
 * (runtime/src/win32/lan_menu.c).
 *
 * Abre e fecha com F12 ou com um toque de três dedos. Edita o nome que os outros
 * jogadores veem, o país (a bandeira que o jogo mostra, via GetIPCountry) e os IPs
 * de host para procurar direto quando a rede não deixa passar broadcast. Mostra o
 * ID e o IP do aparelho e os membros da sala com ping e tempo sem resposta.
 * Enquanto aberto, teclado, mouse e toque vão para o menu, não para o jogo.
 */
#ifndef RE5_LAN_MENU_H
#define RE5_LAN_MENU_H

#include <stdint.h>

union SDL_Event;
struct IDirect3DDevice9;

/* Um evento do SDL antes do user32. 1 = o menu ficou com ele. */
int  re5_lan_menu_event(const union SDL_Event *e);
int  re5_lan_menu_is_open(void);
void re5_lan_menu_toggle(void);
/* No Present, com o device do host. Não faz nada com o menu fechado. */
void re5_lan_menu_draw(struct IDirect3DDevice9 *dev);

/* Para os testes: os triângulos do menu num back buffer w x h (D3DFVF_XYZRHW |
 * D3DFVF_DIFFUSE). Devolve o número de vértices escritos (múltiplo de 3). */
typedef struct { float x, y, z, rhw; uint32_t color; } re5_lan_vtx;
uint32_t re5_lan_menu_build(float w, float h, re5_lan_vtx *out, uint32_t max);

#endif
