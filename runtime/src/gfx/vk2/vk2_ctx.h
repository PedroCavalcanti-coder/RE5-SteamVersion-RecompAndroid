/*
 * vk2_ctx.h - contexto Vulkan 1.1 do backend vk2 (RE5_GFX=vk2), em C11 puro.
 *
 * Dono de: instância, superfície, escolha da GPU, device, swapchain, memória de GPU (blocos com
 * sub-alocação própria, sem VMA) e do cache de VkRenderPass/VkFramebuffer. O device D3D9 (vk2_dev.c
 * e vizinhos) fica dono dos envios, dos anéis por quadro, dos layouts por sub-recurso e da
 * destruição adiada: nada aqui espera a GPU.
 *
 * Vulkan 1.1 puro (decisão do Prompt 2): sem extended dynamic state, sem robustness2/nullDescriptor,
 * sem dynamic rendering, sem importar memória do guest. Só núcleo 1.0/1.1 mais VK_KHR_swapchain; o que
 * é opcional (image_format_list) é detectado e avisado.
 */
#ifndef VK2_CTX_H
#define VK2_CTX_H

#include <stdint.h>
#include <stdlib.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK2_MAX_SWAP_IMAGES 8

/* ---- device e swapchain ---- */

typedef VkSurfaceKHR (*vk2_surface_fn)(VkInstance instance, void *user);

typedef struct {
    VkPresentModeKHR modes[3];   /* preferência; FIFO é o último recurso */
    uint32_t min_images;         /* 0 = mínimo da superfície + 1 */
    uint32_t width, height;      /* 0 = o tamanho atual da superfície */
} vk2_swap_req;

typedef struct {
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    uint32_t count;
    VkImage images[VK2_MAX_SWAP_IMAGES];
    VkImageView views[VK2_MAX_SWAP_IMAGES];   /* do contexto: o device não as destrói */
    int transfer_dst;            /* as imagens aceitam TRANSFER_DST (blit no Present) */
} vk2_swapchain;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkSurfaceKHR surface;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    uint32_t api_version;        /* min(instância, device) */
    VkPhysicalDeviceFeatures features;   /* os que ligaram */
    int image_format_list;       /* VkImageFormatListCreateInfo válido (1.2 ou VK_KHR_image_format_list) */
} vk2_device;

/* RE5VK2_GPU=<índice> escolhe a GPU na lista (log mostra todas); sem ele: discreta, depois integrada.
 * 0 = ok; -1 = não subiu (motivo no log; o chamador cai para a DXVK). */
int vk2_open(vk2_surface_fn create_surface, void *surface_user, const vk2_swap_req *swap, vk2_device *dev,
             vk2_swapchain *sc);

/* Recria a swapchain (device ocioso). Os framebuffers das views antigas saem do cache. 0 = ok. */
int vk2_swapchain_recreate(const vk2_swap_req *swap, vk2_swapchain *sc);

/* Troca a superfície (Android: janela nova depois do segundo plano) e recria a swapchain nela.
 * Device ocioso. *surface recebe a nova. 0 = ok. */
int vk2_surface_recreate(vk2_surface_fn create_surface, void *surface_user, const vk2_swap_req *swap,
                         vk2_swapchain *sc, VkSurfaceKHR *surface);

/* ---- memória de GPU ---- */

typedef struct {
    void *alloc;                 /* bloco de memória (vk2_ctx.c) */
    uint8_t *map;                /* mapeado, ou NULL */
    void *block;                 /* reservado (zerado) */
    uint64_t sub;                /* deslocamento dentro do bloco */
} vk2_alloc;

enum {
    VK2_MEM_DEVICE = 0,          /* DEVICE_LOCAL preferido (imagens, estáticos) */
    VK2_MEM_HOST = 1,            /* HOST_VISIBLE | HOST_COHERENT, mapeado (anéis, buffers do D3D9) */
};

VkBuffer vk2_mem_buffer_create(const VkBufferCreateInfo *ci, int mem, const char *name, vk2_alloc *out);
VkImage vk2_mem_image_create(const VkImageCreateInfo *ci, const char *name, vk2_alloc *out);
/* Imediato: só depois que a GPU terminou (o lixo do envio garante). */
void vk2_mem_buffer_destroy(VkBuffer b, vk2_alloc *a);
void vk2_mem_image_destroy(VkImage img, vk2_alloc *a);

/* Bytes de blocos reservados e em uso (diagnóstico, RE5_FPS_LOG). */
void vk2_mem_stats(uint64_t *reserved, uint64_t *used);

/* ---- passes (VkRenderPass + VkFramebuffer em cache) ---- */

typedef struct {
    VkFormat format;             /* VK_FORMAT_UNDEFINED: posição vazia */
    VkAttachmentLoadOp load, stencil_load;
    VkAttachmentStoreOp store, stencil_store;
    VkImageLayout layout;        /* o do anexo durante o passe (= inicial = final: o device transiciona) */
} vk2_att;

typedef struct {
    uint32_t ncolor;
    vk2_att color[4];
    int has_ds;
    vk2_att ds;
} vk2_pass;

VkRenderPass vk2_render_pass(const vk2_pass *p);
/* Passe compatível para criar pipelines (só formatos contam). */
VkRenderPass vk2_compat_pass(uint32_t ncolor, const VkFormat color[4], VkFormat ds, int stencil);
/* views: só as posições usadas, na ordem (cores e depois profundidade). */
VkFramebuffer vk2_framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, VkExtent2D extent);
/* A view vai ser destruída (GPU já terminou com ela): framebuffers com ela saem do cache. */
void vk2_forget_view(VkImageView view);
/* Quantos passes e framebuffers o cache tem (diagnóstico e testes). */
void vk2_pass_cache_stats(uint32_t *passes, uint32_t *framebuffers);

#ifdef __cplusplus
}
#endif

#endif /* VK2_CTX_H */
