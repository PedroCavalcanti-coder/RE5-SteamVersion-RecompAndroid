/*
 * vk2.h - backend Vulkan 1.1 nativo (RE5_GFX=vk2, runtime/src/gfx/vk2/).
 */
#ifndef RE5_VK2_H
#define RE5_VK2_H

/* Envolve o IDirect3D9 do host (DXVK, que segue respondendo as consultas do adaptador): o device
 * criado por ele é o Vulkan nativo. NULL entra, NULL sai. */
void *vk2_wrap_d3d9(void *inner);

#endif /* RE5_VK2_H */
