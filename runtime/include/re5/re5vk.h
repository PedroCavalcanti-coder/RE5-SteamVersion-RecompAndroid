/*
 * re5vk.h - backend Vulkan próprio (RE5_GFX=re5vk, docs/PROJETO_VULKAN_NATIVO.md).
 */
#ifndef RE5_RE5VK_H
#define RE5_RE5VK_H

/* Embrulha o IDirect3D9 do host (o da DXVK): as consultas do adaptador vão para ele, o
 * CreateDevice cria um device Vulkan próprio. Devolve um IDirect3D9* do host. */
void *re5vk_wrap_d3d9(void *inner);

/* RE5_GFX=re5vk? (o padrão, RE5_GFX vazio ou "dxvk", é a DXVK) */
int re5vk_enabled(void);

#endif /* RE5_RE5VK_H */
