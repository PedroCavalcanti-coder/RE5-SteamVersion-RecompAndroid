/*
 * renderer.h - seletor de renderizador (fachada C do IRenderer, runtime/src/gfx/vkn/IRenderer.h).
 *
 * O runtime é C11; os renderizadores nativos novos são C++17 (vk-bootstrap e VMA são C++).
 * Esta fachada é a única porta entre os dois mundos: d3d9_host.c e user32.c continuam em C e
 * só enxergam o enum e as funções re5_renderer_*.
 *
 * Valores de RE5_GFX (gravados pelo launcher Android, pref "gfx_renderer"):
 *   ""/"dxvk"  -> RE5_RENDERER_DXVK      tradução D3D9 -> Vulkan pela DXVK (fallback, padrão)
 *   "vkadreno" -> RE5_RENDERER_VKADRENO  Vulkan nativo, ajustes Qualcomm (driver proprietário e Turnip)
 *   "vkmali"   -> RE5_RENDERER_VKMALI    Vulkan nativo, só regras TBDR da ARM
 *   "gl"       -> RE5_RENDERER_GL        tradutor D3D9 -> GL externo (inalterado)
 * "re5vk" (valor antigo) é recusado com mensagem que aponta os novos nomes (D8: falhar alto);
 * o launcher migra a preferência salva.
 */
#ifndef RE5_RENDERER_H
#define RE5_RENDERER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RE5_RENDERER_DXVK     = 0,
    RE5_RENDERER_VKADRENO = 1,
    RE5_RENDERER_VKMALI   = 2,
    RE5_RENDERER_GL       = 3
} re5_renderer_kind;

/* Interpreta RE5_GFX. 0 = válido, -1 = desconhecido (inclusive "re5vk"). */
int re5_renderer_parse(const char *env, re5_renderer_kind *out);

/* Nome canônico ("dxvk", "vkadreno", ...) para logs e para o launcher. */
const char *re5_renderer_name(re5_renderer_kind kind);

/* Renderizador escolhido para este processo (lê RE5_GFX uma vez; aborta se inválido). */
re5_renderer_kind re5_renderer_selected(void);

/* 1 se o renderizador é um dos backends Vulkan nativos (vkadreno/vkmali). */
int re5_renderer_is_native_vk(re5_renderer_kind kind);

/*
 * Direct3DCreate9 do renderizador selecionado: devolve um IDirect3D9* do host.
 * dxvk_create9: o Direct3DCreate9 da libdxvk_d3d9.so (a ponte d3d9_host.c o tem).
 * DXVK: devolve o dela. Nativos: o embrulho do re5 (consultas de adaptador seguem
 * respondidas pela DXVK, como hoje).
 * NULL se o renderizador não subiu (o motivo já foi logado). RE5_RENDERER_GL aborta: o GL
 * externo não passa pelo seletor.
 */
void *re5_renderer_create_d3d9(uint32_t sdk_version, void *(*dxvk_create9)(uint32_t));

/* Derruba o renderizador (fim do processo / troca de janela no Android). Idempotente. */
void re5_renderer_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* RE5_RENDERER_H */
