/*
 * vkn_bridge.h - ponte C entre o device D3D9 nativo (runtime/src/gfx/vk, C11) e os renderizadores
 * nativos (vkadreno/vkmali, C++17: VulkanContext, VulkanMemory, NativeVulkanRenderer).
 *
 * O device D3D9 não cria instância, device, swapchain nem memória por conta própria: tudo passa
 * por aqui (vk-bootstrap e VMA do outro lado). Ele continua dono dos envios, dos anéis por envio,
 * dos layouts por sub-recurso e da destruição adiada (o lixo de cada envio chama os *_destroy
 * daqui quando a GPU terminou).
 *
 * Implementado em renderer.cpp (usa o renderizador selecionado por RE5_GFX).
 */
#ifndef RE5_VKN_BRIDGE_H
#define RE5_VKN_BRIDGE_H

#include <stdint.h>
#include <stdlib.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RE5_VKN_MAX_SWAP_IMAGES 8

/* ---- device e swapchain ---- */

typedef VkSurfaceKHR (*re5_vkn_surface_fn)(VkInstance instance, void *user);

typedef struct {
    VkPresentModeKHR modes[3];   /* preferência; FIFO é o último recurso */
    uint32_t min_images;         /* 0 = mínimo da superfície + 1 */
    uint32_t width, height;      /* 0 = o tamanho atual da superfície */
} re5_vkn_swap_req;

typedef struct {
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    uint32_t count;
    VkImage images[RE5_VKN_MAX_SWAP_IMAGES];
    VkImageView views[RE5_VKN_MAX_SWAP_IMAGES];   /* do contexto: o device não as destrói */
    int transfer_dst;            /* as imagens aceitam TRANSFER_DST (blit no Present) */
} re5_vkn_swapchain;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkSurfaceKHR surface;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    uint32_t api_version;        /* min(instância, device): decide nomes KHR/EXT das funções */
    VkPhysicalDeviceFeatures features;   /* os que ligaram */
    int host_import;             /* VK_EXT_external_memory_host ligado */
    int image_format_list;       /* VkImageFormatListCreateInfo válido */
    int has_eds2;                /* extendedDynamicState2 ligado */
    int dynamic_rendering;       /* dynamicRendering ligado */
    int robust_buffer_access2;   /* VK_EXT_robustness2 robustBufferAccess2 ligado (RE5VK_ROBUST2=0 desliga) */
    int core;                    /* modo Vulkan 1.1: sem extended dynamic state (re5_vkn_core_mode) */
} re5_vkn_device;

/*
 * Modo Vulkan 1.1 (RE5VK_CORE, padrão 1 no Android, 0 no resto): o device D3D9 não usa
 * VK_EXT_extended_dynamic_state(2). Cull, topologia, profundidade, stencil, depth bias e stride de
 * vértice vão na chave do pipeline; só os estados dinâmicos do núcleo 1.0 ficam no draw. Motivo
 * (A54, Mali r38, 20/11/2026): com o stride dinâmico a DXVK 2.6.2 desenhava linhas até no 2D e o
 * backend nativo, faixas nas cabeças; com o stride no pipeline as linhas sumiram.
 */
static inline int re5_vkn_core_mode(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_CORE");
#if defined(__ANDROID__)
        on = !(e && *e == '0');
#else
        on = e && *e == '1';
#endif
    }
    return on;
}

/*
 * Abre o device do renderizador selecionado (vkadreno/vkmali) com EDS1+EDS2 obrigatórios e a
 * swapchain pedida. 0 = ok; -1 = não subiu (motivo no log; o chamador cai para a DXVK).
 */
int re5_vkn_open(re5_vkn_surface_fn create_surface, void *surface_user, const re5_vkn_swap_req *swap,
                 re5_vkn_device *dev, re5_vkn_swapchain *sc);

/* Recria a swapchain (device ocioso). Os framebuffers das views antigas saem do cache. 0 = ok. */
int re5_vkn_swapchain_recreate(const re5_vkn_swap_req *swap, re5_vkn_swapchain *sc);

/* Troca a superfície (Android: janela nova depois do segundo plano) e recria a swapchain nela.
 * Device ocioso. *surface recebe a nova. 0 = ok. */
int re5_vkn_surface_recreate(re5_vkn_surface_fn create_surface, void *surface_user, const re5_vkn_swap_req *swap,
                             re5_vkn_swapchain *sc, VkSurfaceKHR *surface);

/* ---- memória (VMA) ---- */

typedef struct {
    void *alloc;                 /* VmaAllocation (NULL numa subdivisão de bloco do host) */
    uint8_t *map;                /* mapeado, ou NULL */
    void *block;                 /* bloco do host, ou NULL */
    uint64_t sub;                /* subdivisão no bloco */
} re5_vkn_alloc;

enum {
    RE5_VKN_MEM_DEVICE = 0,      /* DEVICE_LOCAL preferido (imagens, estáticos) */
    RE5_VKN_MEM_HOST = 1,        /* HOST_VISIBLE | HOST_COHERENT, mapeado (anéis, buffers do D3D9) */
};

VkBuffer re5_vkn_buffer_create(const VkBufferCreateInfo *ci, int mem, const char *name, re5_vkn_alloc *out);
VkImage re5_vkn_image_create(const VkImageCreateInfo *ci, const char *name, re5_vkn_alloc *out);
/* Imediato: só depois que a GPU terminou (o lixo do envio garante). */
void re5_vkn_buffer_destroy(VkBuffer b, re5_vkn_alloc *a);
void re5_vkn_image_destroy(VkImage img, re5_vkn_alloc *a);

/* Memória do guest como memória do device (Unlock sem cópia). NULL = o driver recusou. */
void *re5_vkn_host_block(void *host, VkDeviceSize size);
/* Buffer dentro do bloco; VK_NULL_HANDLE se não couber (o chamador abre outro bloco). */
VkBuffer re5_vkn_host_buffer(void *block, const VkBufferCreateInfo *ci, re5_vkn_alloc *out);
/* Solta um bloco sem buffers vivos (o do buffer grande dedicado). */
void re5_vkn_host_block_free(void *block);

/* ---- passes (VkRenderPass + VkFramebuffer em cache) ---- */

typedef struct {
    VkFormat format;             /* VK_FORMAT_UNDEFINED: posição vazia */
    VkAttachmentLoadOp load, stencil_load;
    VkAttachmentStoreOp store, stencil_store;
    VkImageLayout layout;        /* o do anexo durante o passe (= inicial = final: o device transiciona) */
} re5_vkn_att;

typedef struct {
    uint32_t ncolor;
    re5_vkn_att color[4];
    int has_ds;
    re5_vkn_att ds;
} re5_vkn_pass;

VkRenderPass re5_vkn_render_pass(const re5_vkn_pass *p);
/* Passe compatível para criar pipelines (só formatos contam). */
VkRenderPass re5_vkn_compat_pass(uint32_t ncolor, const VkFormat color[4], VkFormat ds, int stencil);
/* views: só as posições usadas, na ordem (cores e depois profundidade). */
VkFramebuffer re5_vkn_framebuffer(const re5_vkn_pass *p, VkRenderPass rp, const VkImageView *views, uint32_t n,
                                  VkExtent2D extent);
/* A view vai ser destruída (GPU já terminou com ela): framebuffers com ela saem do cache. */
void re5_vkn_forget_view(VkImageView view);

#ifdef __cplusplus
}
#endif

#endif /* RE5_VKN_BRIDGE_H */
