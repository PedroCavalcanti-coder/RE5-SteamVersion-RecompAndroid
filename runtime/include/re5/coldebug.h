/*
 * coldebug.h - linhas de colisão do Havok desenhadas por cima do quadro (runtime/src/gfx/coldebug.c).
 *
 * Com RE5_COLDEBUG=1 a tecla F6 liga/desliga. O módulo acha na memória do guest os corpos rígidos
 * e os fantasmas (personagens) do Havok 5.5 pelas vtables, lê as formas e desenha as arestas por
 * cima do back buffer, antes do Present. A matriz de câmera sai das constantes de vertex shader
 * que o jogo envia (a que melhor projeta os personagens para dentro da tela).
 */
#ifndef RE5_COLDEBUG_H
#define RE5_COLDEBUG_H

#include <stdint.h>

struct IDirect3DDevice9;

void re5_coldebug_key(void);
/* Cada SetVertexShaderConstantF do guest (antes de repassar ao host). */
void re5_coldebug_vs_const(uint32_t start, const float *data, uint32_t count4);
/* No Present, com o device do host. Não faz nada com a sobreposição desligada. */
void re5_coldebug_draw(struct IDirect3DDevice9 *dev);

#endif
