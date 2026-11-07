/*
 * vk_core.h - estado interno do backend re5vk (RE5_GFX=re5vk): device, envios, memória, recursos.
 *
 * Arquivos: re5vk.c (carregador, IDirect3D9, device, envios, memória, Present), vk_res.c
 * (formatos, buffers, texturas, superfícies, declarações, shaders, consultas), vk_draw.c (estado,
 * pipelines, descritores, passes, Draw/Clear/StretchRect), vk_dxso.c (SM3 -> SPIR-V).
 *
 * Modelo: Vulkan 1.3 (dynamic rendering + extended dynamic state). Toda imagem fica em
 * VK_IMAGE_LAYOUT_GENERAL; a ordem entre passes, cópias e amostragem vem de barreiras de memória
 * no fim de cada passe e em volta de cada cópia. Um mutex do device serializa os métodos (a ponte
 * pode chamar de mais de uma thread do guest; o Present solta o lock do guest).
 */
#ifndef RE5VK_CORE_H
#define RE5VK_CORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <d3d9.h>

#include "re5/log.h"
#include "../../core/host_sync.h"
#include "vk_dxso.h"

#ifndef FOURCC
#define FOURCC(a, b, c, d) ((D3DFORMAT)((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24))
#endif

/* ---- funções do Vulkan (carregadas em tempo de execução) ---- */

#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#if defined(_WIN32)
#define VK_SURFACE_FN(X) X(vkCreateWin32SurfaceKHR)
#elif defined(__ANDROID__)
#define VK_SURFACE_FN(X) X(vkCreateAndroidSurfaceKHR)
#else
#define VK_SURFACE_FN(X)
#endif
#define VK_INSTANCE_FNS(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR)                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateDevice) X(vkGetPhysicalDeviceSurfacePresentModesKHR)         \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkEnumerateDeviceExtensionProperties)                          \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures2)  \
    VK_SURFACE_FN(X)
#define VK_DEVICE_FNS(X)                                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)    \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit)                      \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)             \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)                  \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus)                     \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)                  \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)                    \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                        \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)                              \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdCopyImage) X(vkCmdClearDepthStencilImage)                   \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)                                \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                           \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkDestroyPipelineCache)           \
    X(vkGetPipelineCacheData) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool)        \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBeginRendering) X(vkCmdEndRendering)              \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer)             \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdClearAttachments)               \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable)               \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp)        \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference)                          \
    X(vkCmdSetDepthBias) X(vkCmdSetDepthBiasEnable) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBoundsTestEnable)     \
    X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable)

#define VK_DECL_EXTERN(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECL_EXTERN)
VK_INSTANCE_FNS(VK_DECL_EXTERN)
VK_DEVICE_FNS(VK_DECL_EXTERN)

const char *re5vk_result(VkResult r);
#define VK_CHECK(call)                                                                                             \
    do {                                                                                                           \
        VkResult vr_ = (call);                                                                                     \
        if (vr_ < 0) re5_fatal("re5vk", "%s -> %s (%d)", #call, re5vk_result(vr_), (int)vr_);                     \
    } while (0)

/* ---- memória ---- */

typedef struct {
    VkDeviceMemory mem;
    VkDeviceSize off, size;
    uint8_t *map;            /* NULL se não for host-visible */
    int chunk;               /* -1: alocação dedicada; -2: dedicada na memória do guest */
    uint32_t type;
} vk_alloc;

#define VK_MAX_CHUNKS 256
typedef struct {
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint8_t *map;
    uint32_t type;
    VkDeviceSize (*free)[2];   /* faixas livres [off, size], ordenadas */
    uint32_t guest_va;         /* bloco importado da memória do guest (0 = não) */
    int nfree, capfree;
} vk_chunk;

/* ---- envios (um command buffer por envio; anel de VK_SLOTS) ---- */

#define VK_SLOTS 8
#define VK_UBO_RING (8u << 20)
#define VK_STAGE_RING (32u << 20)

typedef struct {
    int kind;   /* 1 buffer, 2 imagem, 3 view, 4 alocação */
    union { VkBuffer buffer; VkImage image; VkImageView view; } h;
    vk_alloc mem;
} vk_garbage;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    uint64_t serial;           /* 0: livre */
    VkDescriptorPool pools[8];
    int npools, pool_cur;
    VkBuffer ubo;
    vk_alloc ubo_mem;
    VkDeviceSize ubo_used;
    VkBuffer stage;
    vk_alloc stage_mem;
    VkDeviceSize stage_used, stage_size;
    vk_garbage *garbage;
    int ngarbage, capgarbage;
} vk_slot;

/* ---- objetos COM do host ---- */

typedef struct vk_device vk_device;

enum { VKO_SURFACE = 1, VKO_TEXTURE, VKO_CUBE, VKO_VOLUME, VKO_VB, VKO_IB, VKO_DECL, VKO_VS, VKO_PS, VKO_QUERY };

typedef struct {
    const void *lpVtbl;
    long refs;
    vk_device *dev;
    int kind;
} vk_obj;

typedef struct {
    D3DFORMAT d3d;
    VkFormat vk;
    VkComponentMapping swz;
    uint8_t bw, bh, bytes;       /* bloco em pixels e bytes por bloco */
    uint8_t depth, stencil;      /* formato de profundidade (e stencil) */
} vk_format;

typedef struct vk_image vk_image;
struct vk_image {             /* imagem de textura, alvo de render, profundidade ou back buffer */
    VkImage image;
    vk_alloc mem;
    VkImageView view;          /* para amostrar (todos os níveis e faces) */
    VkImageView *rt_views;     /* por (camada, nível): para render target / anexo */
    const vk_format *fmt;
    VkFormat vkf;              /* o formato escolhido (fmt->vk ou o substituto que a GPU aceita) */
    uint32_t w, h, d, levels, layers;
    VkImageType type;
    uint64_t last_use;
    VkFormat srgb;             /* par sRGB do formato (D3DSAMP_SRGBTEXTURE), ou UNDEFINED */
    VkImageView view_srgb;     /* criada no primeiro uso */
};

typedef struct {
    vk_obj o;
    vk_obj *container;         /* textura dona (com referência) ou NULL */
    vk_image *img;             /* da textura dona ou própria */
    int own_img;
    uint32_t level, layer;
    D3DSURFACE_DESC desc;
    uint8_t *lock;             /* staging do LockRect (malloc) */
    RECT lock_rect;
    int locked;
} vk_surface;

typedef struct {
    vk_obj o;
    vk_image img;
    D3DFORMAT format;
    DWORD usage;
    D3DPOOL pool;
    vk_surface **surf;         /* layers * levels, criadas sob demanda */
    struct { uint8_t *data; RECT r; D3DBOX box; int on; } *locks;   /* layers * levels */
} vk_texture;

typedef struct {
    vk_obj o;
    VkBuffer buf;
    vk_alloc mem;
    UINT size;
    DWORD usage, fvf;
    D3DFORMAT format;          /* índices: D3DFMT_INDEX16/32 */
    D3DPOOL pool;
    uint64_t last_use;
    int locked;
} vk_buffer;

typedef struct {
    vk_obj o;
    D3DVERTEXELEMENT9 el[64];
    uint32_t n;                /* sem o D3DDECL_END */
} vk_decl;

#define VK_MAX_VARIANTS 16   /* PS: profundidade x alpha test; VS: entradas inteiras (Adreno) */
typedef struct {
    vk_obj o;
    uint32_t *code;
    uint32_t ndw;
    uint32_t hash;
    struct { re5vk_dxso_variant var; re5vk_shader info; VkShaderModule mod; } v[VK_MAX_VARIANTS];
    int nv;
} vk_shader;

typedef struct {
    vk_obj o;
    D3DQUERYTYPE type;
    uint64_t serial;           /* envio que fecha a consulta (evento); 0 = não emitida */
} vk_query;

/* ---- device ---- */

#define VK_MAX_IMAGES 8
#define VK_MAX_SAMPLERS 20      /* 0-15 PS, 16-19 VS (D3DVERTEXTEXTURESAMPLER0 = 257) */

typedef struct { uint64_t key[6]; VkPipeline pipe; } vk_pipe_entry;
typedef struct { uint32_t key[14]; VkSampler s; } vk_sampler_entry;

struct vk_device {
    const struct IDirect3DDevice9Vtbl *lpVtbl;
    long refs;
    struct vk_d3d9 *d3d;
    re5_mutex mu;
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DPRESENT_PARAMETERS pp;

    VkInstance inst;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkSurfaceKHR surface;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;

    VkSwapchainKHR swap;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    uint32_t nimages;
    VkImage images[VK_MAX_IMAGES];
    VkImageView image_views[VK_MAX_IMAGES];
    int swap_has_transfer_dst;
    int can_blit_to_swap;
    VkSampler present_sampler;
    VkDescriptorSetLayout present_dsl;
    VkPipelineLayout present_layout;
    VkDescriptorPool present_pool;
    VkDescriptorSet present_set;
    VkImageView present_view;   /* o que o present_set aponta */
    VkPipeline present_pipe;
    VkSemaphore done[VK_MAX_IMAGES];
    VkSemaphore acquired[VK_SLOTS];

    VkCommandPool pool;
    vk_slot slots[VK_SLOTS];
    int cur;                   /* envio sendo gravado */
    uint64_t serial;           /* do envio sendo gravado (crescente a partir de 1) */
    uint64_t done_serial;      /* último envio que a GPU terminou */

    vk_chunk chunks[VK_MAX_CHUNKS];
    int nchunks;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipelineCache pcache;
    uint32_t pcache_saved; /* npipes na última gravação do cache em disco */
    vk_pipe_entry *pipes;
    uint32_t npipes, cappipes;
    vk_sampler_entry *samplers;
    uint32_t nsamplers, capsamplers;

    vk_surface *backbuffer, *auto_depth;
    vk_image dummy2d, dummycube, dummy3d;   /* textura nula: (0, 0, 0, 1) */
    VkBuffer zero_vb;
    vk_alloc zero_vb_mem;

    /* estado do D3D9 */
    vk_surface *rt[4], *ds;
    D3DVIEWPORT9 vp;
    RECT scissor;
    DWORD rs[256];
    DWORD ss[VK_MAX_SAMPLERS][14];
    vk_obj *tex[VK_MAX_SAMPLERS];
    struct { vk_buffer *vb; UINT off, stride; } streams[16];
    vk_buffer *ib;
    vk_decl *decl;
    vk_shader *vs, *ps;
    float vsf[256][4], psf[224][4];
    int32_t vsi[16][4], psi[16][4];
    uint32_t vsb, psb;
    int vs_dirty, ps_dirty;
    /* o que a região atual do UBO tem (prepare copia só o que o shader lê): c[0..n-1], i[], b */
    uint16_t vs_ubo_f, ps_ubo_f;
    uint8_t vs_ubo_ib, ps_ubo_ib;
    /* O que está ligado no command buffer atual (zerado a cada slot_begin): o draw só religa o
     * que mudou. Compara o VkBuffer, não o objeto: o Lock com DISCARD troca a memória. */
    VkBuffer bound_vb[16];
    VkDeviceSize bound_vo[16], bound_vstride[16];
    int vb_valid;
    /* Buffers DYNAMIC na memória do guest (VK_EXT_external_memory_host): o Lock devolve um
     * endereço do guest e a ponte entrega ao jogo sem sombra nem cópia no Unlock. */
    int zerocopy;
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_ptr_props;
    VkBuffer bound_ib;
    VkIndexType bound_ibt;
    /* Pipeline do draw anterior: refeito só quando muda algo que entra nele (pipe_gen sobe em
     * todo SetRenderState com valor novo, shaders, declaração, alvos e textura que troca entre
     * profundidade e cor) ou a classe de topologia. */
    uint32_t pipe_gen, last_gen;
    uint32_t last_topo;
    VkPipeline last_pipe;
    const re5vk_shader *last_vsi, *last_psi;
    re5vk_dxso_variant last_psv;
    VkDeviceSize vs_ubo_off, ps_ubo_off;
    VkBuffer vs_ubo_buf, ps_ubo_buf;
    int in_scene;

    /* passe de render aberto */
    int rendering;
    VkExtent2D render_area;
    VkPipeline bound_pipe;
    int dyn_dirty, desc_dirty;
    int has_eds1, has_eds2;
    VkDescriptorSet set;
    /* VkSampler do slot (sem e com comparação), válido até o próximo SetSamplerState nele:
     * sampler_get é busca linear com memcmp em toda a lista. */
    VkSampler smp_cache[VK_MAX_SAMPLERS][2];
    /* Sets já escritos neste envio (o pool zera a cada slot_begin): o mesmo conjunto de UBOs,
     * views e samplers volta muito num quadro (material A, B, A...). Chave = os handles. */
    struct vk_set_cache_entry {
        uint64_t key[2 + 2 * RE5VK_NBINDINGS];
        uint32_t nkey;
        uint64_t hash, gen;    /* gen != set_cache_gen: vaga */
        VkDescriptorSet set;
    } *set_cache;
    uint32_t set_cache_n, set_cache_cap;
    uint64_t set_cache_gen;   /* sobe a cada slot_begin (re5vk.c) */
    uint32_t pipe_hash_cap;
    struct { int on; float color[4][4]; float z; uint32_t stencil; DWORD flags; } pending_clear;

    uint64_t presents;
    int warned_gamma, warned_clip;
};

typedef struct vk_d3d9 {
    const struct IDirect3D9Vtbl *lpVtbl;
    IDirect3D9 *inner;
    long refs;
} vk_d3d9;

/* re5vk.c */
HRESULT re5vk_device_create(vk_d3d9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                            D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out);
HRESULT STDMETHODCALLTYPE re5vk_nyi(void *self);
void re5vk_lock(vk_device *d);
void re5vk_unlock(vk_device *d);
/* Memória do guest importada como memória do device (VK_EXT_external_memory_host): `map` é um
 * endereço do guest no espaço do host. mem = VK_NULL_HANDLE se não deu (o chamador volta ao
 * caminho comum). */
vk_alloc re5vk_mem_alloc_guest(vk_device *d, VkMemoryRequirements req);
vk_alloc re5vk_mem_alloc(vk_device *d, VkMemoryRequirements req, VkMemoryPropertyFlags want, int host);
void re5vk_mem_free(vk_device *d, vk_alloc *a);
void re5vk_destroy_later(vk_device *d, int kind, uint64_t handle, vk_alloc mem);
VkCommandBuffer re5vk_cmd(vk_device *d);
void re5vk_flush(vk_device *d, int wait);          /* envia o que foi gravado; wait: espera a GPU */
void re5vk_wait_serial(vk_device *d, uint64_t s);  /* espera a GPU terminar o envio s */
void re5vk_poll(vk_device *d);                     /* atualiza done_serial sem esperar */
/* staging do envio atual: bytes host-visible copiáveis por vkCmdCopy*; NULL buffer = esgotado */
uint8_t *re5vk_stage(vk_device *d, VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
uint8_t *re5vk_ubo(vk_device *d, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off);
VkDescriptorSet re5vk_alloc_set(vk_device *d);
void re5vk_barrier_all(VkCommandBuffer cb);

/* vk_res.c */
const vk_format *re5vk_format(D3DFORMAT f);
int re5vk_image_create(vk_device *d, vk_image *im, VkImageType type, const vk_format *fmt, uint32_t w, uint32_t h,
                       uint32_t depth, uint32_t levels, uint32_t layers, int cube, int rt, int ds);
void re5vk_image_destroy(vk_device *d, vk_image *im);
VkImageView re5vk_image_rt_view(vk_device *d, vk_image *im, uint32_t layer, uint32_t level);
/* Vista para amostrar: a sRGB (sRGB -> linear na leitura) se srgb e o formato tem par. */
VkImageView re5vk_image_sampled_view(vk_device *d, vk_image *im, int srgb);
vk_surface *re5vk_surface_new(vk_device *d, vk_obj *container, vk_image *img, int own, uint32_t layer, uint32_t level,
                              D3DFORMAT format, DWORD usage, D3DPOOL pool);
void re5vk_res_vtables(vk_device *d);
void re5vk_init_dummies(vk_device *d);
HRESULT re5vk_shader_variant(vk_device *d, vk_shader *s, const re5vk_dxso_variant *var, VkShaderModule *mod,
                             const re5vk_shader **info);

/* métodos de criação do device implementados em vk_res.c */
HRESULT STDMETHODCALLTYPE re5vk_CreateTexture(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                              IDirect3DTexture9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateCubeTexture(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                  IDirect3DCubeTexture9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateVolumeTexture(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
                                                    D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateVertexBuffer(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL,
                                                   IDirect3DVertexBuffer9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateIndexBuffer(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                  IDirect3DIndexBuffer9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateRenderTarget(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
                                                   DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateDepthStencilSurface(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT,
                                                          D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **,
                                                          HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_CreateVertexDeclaration(IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *,
                                                        IDirect3DVertexDeclaration9 **);
HRESULT STDMETHODCALLTYPE re5vk_CreateVertexShader(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
HRESULT STDMETHODCALLTYPE re5vk_CreatePixelShader(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
HRESULT STDMETHODCALLTYPE re5vk_CreateOffscreenPlainSurface(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL,
                                                            IDirect3DSurface9 **, HANDLE *);
HRESULT STDMETHODCALLTYPE re5vk_GetRenderTargetData(IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
HRESULT STDMETHODCALLTYPE re5vk_CreateQuery(IDirect3DDevice9 *, D3DQUERYTYPE, IDirect3DQuery9 **);

/* vk_draw.c */
void re5vk_draw_vtable(struct IDirect3DDevice9Vtbl *vt);
void re5vk_state_reset(vk_device *d);          /* estado padrão do D3D9 (CreateDevice, Reset) */
void re5vk_end_rendering(vk_device *d);
void re5vk_draw_destroy(vk_device *d);

/* Referências como no D3D9: a superfície de uma textura conta na textura. NULL é aceito. */
void re5vk_addref(vk_obj *o);    /* vk_res.c */
void re5vk_release(vk_obj *o);   /* vk_res.c: Release genérico (destrói na última) */

#endif /* RE5VK_CORE_H */
