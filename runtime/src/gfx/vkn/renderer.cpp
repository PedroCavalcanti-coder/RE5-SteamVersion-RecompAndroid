/*
 * renderer.cpp - fachada C do seletor de renderizador (re5/renderer.h).
 */
#include "IRenderer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

extern "C" {
#include "re5/log.h"
}

namespace {

constexpr const char *kCh = "renderer";

std::mutex g_mutex;
/* Nunca destruído na saída do processo: o jogo sai com texturas e buffers do D3D9 vivos (o
 * re5vk sempre largou tudo para o sistema), e o destrutor estático derrubaria a memória e
 * abortaria no relatório de vazamento. Só re5_renderer_shutdown() solta, de propósito. */
std::unique_ptr<re5::vkn::IRenderer> &g_renderer = *new std::unique_ptr<re5::vkn::IRenderer>();
bool g_selected = false;
re5_renderer_kind g_kind = RE5_RENDERER_DXVK;

} // namespace

extern "C" int re5_renderer_parse(const char *env, re5_renderer_kind *out)
{
    re5_renderer_kind k;
    if (!env || !*env || !std::strcmp(env, "dxvk")) k = RE5_RENDERER_DXVK;
    else if (!std::strcmp(env, "vkadreno")) k = RE5_RENDERER_VKADRENO;
    else if (!std::strcmp(env, "vkmali")) k = RE5_RENDERER_VKMALI;
    else if (!std::strcmp(env, "gl")) k = RE5_RENDERER_GL;
    else return -1; /* inclusive "re5vk" */
    if (out) *out = k;
    return 0;
}

extern "C" const char *re5_renderer_name(re5_renderer_kind kind)
{
    switch (kind) {
    case RE5_RENDERER_DXVK: return "dxvk";
    case RE5_RENDERER_VKADRENO: return "vkadreno";
    case RE5_RENDERER_VKMALI: return "vkmali";
    case RE5_RENDERER_GL: return "gl";
    }
    return "?";
}

extern "C" int re5_renderer_is_native_vk(re5_renderer_kind kind)
{
    return kind == RE5_RENDERER_VKADRENO || kind == RE5_RENDERER_VKMALI;
}

extern "C" re5_renderer_kind re5_renderer_selected(void)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_selected) {
        const char *env = std::getenv("RE5_GFX");
        if (re5_renderer_parse(env, &g_kind) != 0) {
            if (env && !std::strcmp(env, "re5vk"))
                re5_fatal(kCh, "RE5_GFX=re5vk foi substituído: use vkadreno (Adreno), vkmali (Mali) ou dxvk");
            re5_fatal(kCh, "RE5_GFX=%s: valores aceitos: dxvk (padrão), vkadreno, vkmali, gl", env);
        }
        g_selected = true;
        RE5_INFO(kCh, "renderizador: %s", re5_renderer_name(g_kind));
    }
    return g_kind;
}

extern "C" void *re5_renderer_create_d3d9(uint32_t sdk_version, void *(*dxvk_create9)(uint32_t))
{
    const re5_renderer_kind kind = re5_renderer_selected();
    if (kind == RE5_RENDERER_GL)
        re5_fatal(kCh, "RE5_GFX=gl usa o tradutor externo de d3d9_host.c, não o seletor");
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_renderer) {
        auto r = re5::vkn::createRenderer(kind);
        re5::vkn::RendererCreateInfo ci;
        ci.dxvkCreate9 = dxvk_create9;
        if (!r || !r->initialize(ci)) {
            RE5_ERROR(kCh, "o renderizador %s não subiu", re5_renderer_name(kind));
            return nullptr;
        }
        g_renderer = std::move(r);
    }
    return g_renderer->createDirect3D9(sdk_version);
}

extern "C" void re5_renderer_shutdown(void)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_renderer) {
        g_renderer->shutdown();
        g_renderer.reset();
    }
}

/* ================================================================== vkn_bridge.h ==== */

#include "vkn_bridge.h"

extern "C" {
#include "re5/vk_loader.h"
}

namespace {

using re5::vkn::AttachmentOps;
using re5::vkn::NativeVulkanRenderer;
using re5::vkn::RawAlloc;
using re5::vkn::RenderPassKey;

/* O renderizador nativo do processo (cria se o seletor ainda não criou). Com g_mutex preso. */
NativeVulkanRenderer *nativeLocked()
{
    if (!g_renderer) {
        const re5_renderer_kind k = g_selected ? g_kind : RE5_RENDERER_DXVK;
        if (!re5_renderer_is_native_vk(k)) return nullptr;
        g_renderer = re5::vkn::createRenderer(k);
        re5::vkn::RendererCreateInfo ci;
        g_renderer->initialize(ci);
    }
    return dynamic_cast<NativeVulkanRenderer *>(g_renderer.get());
}

NativeVulkanRenderer *native()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    NativeVulkanRenderer *r = nativeLocked();
    if (!r || !r->hostedForD3D9()) re5_fatal(kCh, "ponte do device D3D9 usada sem re5_vkn_open");
    return r;
}

re5::vkn::SwapchainOptions swapOptions(const re5_vkn_swap_req *s)
{
    re5::vkn::SwapchainOptions o;
    o.preRotate = false; /* o device D3D9 não gira a projeção: o compositor gira, como na DXVK */
    for (int i = 0; i < 3; i++) o.presentModes[i] = s->modes[i];
    o.minImageCount = s->min_images;
    return o;
}

void fillSwapchain(const re5::vkn::VulkanContext &ctx, re5_vkn_swapchain *sc)
{
    const re5::vkn::SwapchainState &s = ctx.swapchain();
    if (s.images.size() > RE5_VKN_MAX_SWAP_IMAGES)
        re5_fatal(kCh, "swapchain com %u imagens (máx. %d)", (unsigned)s.images.size(), RE5_VKN_MAX_SWAP_IMAGES);
    *sc = re5_vkn_swapchain{};
    sc->swapchain = s.handle;
    sc->format = s.format;
    sc->extent = s.extent;
    sc->mode = s.presentMode;
    sc->count = (uint32_t)s.images.size();
    for (uint32_t i = 0; i < sc->count; i++) {
        sc->images[i] = s.images[i];
        sc->views[i] = s.views[i];
    }
    sc->transfer_dst = ctx.caps().swapchainTransferDst;
}

AttachmentOps opsOf(const re5_vkn_att &a, bool ds)
{
    AttachmentOps o;
    o.format = a.format;
    if (a.format == VK_FORMAT_UNDEFINED) return o;
    o.samples = VK_SAMPLE_COUNT_1_BIT;
    o.load = a.load;
    o.store = a.store;
    o.stencilLoad = ds ? a.stencil_load : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    o.stencilStore = ds ? a.stencil_store : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    o.initialLayout = o.finalLayout = o.subpassLayout = a.layout;
    return o;
}

RenderPassKey keyOf(const re5_vkn_pass *p)
{
    if (p->ncolor > 4) re5_fatal(kCh, "passe com %u cores", p->ncolor);
    RenderPassKey k;
    k.colorCount = p->ncolor;
    for (uint32_t i = 0; i < p->ncolor; i++) k.color[i] = opsOf(p->color[i], false);
    k.hasDepthStencil = p->has_ds != 0;
    if (k.hasDepthStencil) k.depthStencil = opsOf(p->ds, true);
    return k;
}

/* RawAlloc e re5_vkn_alloc têm os mesmos campos; a conversão é explícita. */
RawAlloc rawOf(const re5_vkn_alloc *a)
{
    RawAlloc r;
    r.alloc = static_cast<VmaAllocation>(a->alloc);
    r.map = a->map;
    r.block = static_cast<re5::vkn::HostBlock *>(a->block);
    r.sub = reinterpret_cast<VmaVirtualAllocation>(static_cast<uintptr_t>(a->sub));
    return r;
}

void outOf(const RawAlloc &r, re5_vkn_alloc *a)
{
    a->alloc = r.alloc;
    a->map = static_cast<uint8_t *>(r.map);
    a->block = r.block;
    a->sub = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(r.sub));
}

} // namespace

extern "C" int re5_vkn_open(re5_vkn_surface_fn create_surface, void *surface_user, const re5_vkn_swap_req *swap,
                            re5_vkn_device *dev, re5_vkn_swapchain *sc)
{
    (void)re5_renderer_selected();
    NativeVulkanRenderer *r;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        r = nativeLocked();
    }
    if (!r) re5_fatal(kCh, "re5_vkn_open com RE5_GFX=%s (não é um Vulkan nativo)", re5_renderer_name(g_kind));

    re5::vkn::ContextCreateInfo ci;
    ci.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(re5_vk_get_instance_proc_addr());
    ci.createSurface = create_surface;
    ci.surfaceUser = surface_user;
    ci.width = swap->width;
    ci.height = swap->height;
    ci.preferApi13 = true;
    ci.requireExtendedDynamicState = !re5_vkn_core_mode();   /* modo Vulkan 1.1: dispensa EDS */
    ci.wantImageFormatList = true;
    ci.optionalFeatures.samplerAnisotropy = VK_TRUE;
    ci.optionalFeatures.textureCompressionBC = VK_TRUE;
    ci.optionalFeatures.textureCompressionETC2 = VK_TRUE;
    ci.optionalFeatures.textureCompressionASTC_LDR = VK_TRUE;
    /* Leitura fora do buffer (vértice, c[a0]) lê 0, como no D3D9 e na DXVK, só com robustBufferAccess2:
     * o "v1" sozinho deixa Mali/Adreno devolverem outro trecho do buffer. RE5VK_ROBUST2=0 desliga o 2. */
    ci.optionalFeatures.robustBufferAccess = VK_TRUE;
    {
        const char *rb = std::getenv("RE5VK_ROBUST2");
        ci.wantRobustness2 = !(rb && rb[0] == '0');
    }
    ci.optionalFeatures.independentBlend = VK_TRUE;
    ci.optionalFeatures.depthBiasClamp = VK_TRUE;
    ci.optionalFeatures.fillModeNonSolid = VK_TRUE;
    ci.swapchain = swapOptions(swap);
    if (!r->openForD3D9(ci)) return -1;

    const re5::vkn::VulkanContext &ctx = r->context();
    *dev = re5_vkn_device{};
    dev->instance = ctx.instance();
    dev->physical = ctx.physicalDevice();
    dev->device = ctx.device();
    dev->surface = ctx.surface();
    dev->queue = ctx.graphicsQueue().queue;
    dev->queue_family = ctx.graphicsQueue().family;
    dev->gipa = ctx.getInstanceProcAddr();
    dev->gdpa = ctx.getDeviceProcAddr();
    dev->api_version = std::min(ctx.caps().instanceApiVersion, ctx.properties().apiVersion);
    dev->features = ctx.caps().enabledFeatures;
    dev->host_import = ctx.caps().externalMemoryHost;
    dev->image_format_list = ctx.caps().imageFormatList;
    dev->has_eds2 = ctx.caps().extendedDynamicState2 ? 1 : 0;
    dev->dynamic_rendering = ctx.caps().dynamicRendering ? 1 : 0;
    dev->robust_buffer_access2 = ctx.caps().robustBufferAccess2 ? 1 : 0;
    dev->core = re5_vkn_core_mode();
    fillSwapchain(ctx, sc);
    return 0;
}

extern "C" int re5_vkn_swapchain_recreate(const re5_vkn_swap_req *swap, re5_vkn_swapchain *sc)
{
    NativeVulkanRenderer *r = native();
    for (VkImageView v : r->context().swapchain().views) r->forgetView(v);
    if (!r->context().recreateSwapchain(swap->width, swap->height, swapOptions(swap))) return -1;
    fillSwapchain(r->context(), sc);
    return 0;
}

extern "C" int re5_vkn_surface_recreate(re5_vkn_surface_fn create_surface, void *surface_user,
                                        const re5_vkn_swap_req *swap, re5_vkn_swapchain *sc, VkSurfaceKHR *surface)
{
    NativeVulkanRenderer *r = native();
    for (VkImageView v : r->context().swapchain().views) r->forgetView(v);
    if (!r->context().recreateSurface(create_surface, surface_user)) return -1;
    for (VkImageView v : r->context().swapchain().views) r->forgetView(v);
    if (!r->context().recreateSwapchain(swap->width, swap->height, swapOptions(swap))) return -1;
    fillSwapchain(r->context(), sc);
    if (surface) *surface = r->context().surface();
    return 0;
}

extern "C" VkBuffer re5_vkn_buffer_create(const VkBufferCreateInfo *ci, int mem, const char *name, re5_vkn_alloc *out)
{
    RawAlloc ra;
    const bool host = mem == RE5_VKN_MEM_HOST;
    VkBuffer b = native()->memory().createRawBuffer(
        *ci, host ? re5::vkn::MemoryUsage::Stream : re5::vkn::MemoryUsage::GpuOnly,
        host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0, name, &ra);
    if (host && !ra.map) re5_fatal(kCh, "buffer '%s' host-visible sem mapeamento", name ? name : "?");
    outOf(ra, out);
    return b;
}

extern "C" VkImage re5_vkn_image_create(const VkImageCreateInfo *ci, const char *name, re5_vkn_alloc *out)
{
    RawAlloc ra;
    VkImage img = native()->memory().createRawImage(*ci, re5::vkn::MemoryUsage::GpuOnly, name, &ra);
    outOf(ra, out);
    return img;
}

extern "C" void re5_vkn_buffer_destroy(VkBuffer b, re5_vkn_alloc *a)
{
    if (b == VK_NULL_HANDLE) return;
    RawAlloc ra = rawOf(a);
    native()->memory().destroyRawBuffer(b, ra);
    *a = re5_vkn_alloc{};
}

extern "C" void re5_vkn_image_destroy(VkImage img, re5_vkn_alloc *a)
{
    if (img == VK_NULL_HANDLE) return;
    RawAlloc ra = rawOf(a);
    native()->memory().destroyRawImage(img, ra);
    *a = re5_vkn_alloc{};
}

extern "C" void *re5_vkn_host_block(void *host, VkDeviceSize size)
{
    return native()->memory().importHostBlock(host, size, "re5vk.guest_block");
}

extern "C" VkBuffer re5_vkn_host_buffer(void *block, const VkBufferCreateInfo *ci, re5_vkn_alloc *out)
{
    RawAlloc ra;
    VkBuffer b = native()->memory().createRawHostBuffer(static_cast<re5::vkn::HostBlock *>(block), *ci, &ra);
    if (b != VK_NULL_HANDLE) outOf(ra, out);
    return b;
}

extern "C" void re5_vkn_host_block_free(void *block)
{
    native()->memory().freeHostBlock(static_cast<re5::vkn::HostBlock *>(block));
}

extern "C" VkRenderPass re5_vkn_render_pass(const re5_vkn_pass *p)
{
    return native()->renderPassFor(keyOf(p));
}

extern "C" VkRenderPass re5_vkn_compat_pass(uint32_t ncolor, const VkFormat color[4], VkFormat ds, int stencil)
{
    re5_vkn_pass p{};
    p.ncolor = ncolor;
    for (uint32_t i = 0; i < ncolor && i < 4; i++) {
        p.color[i].format = color[i];
        p.color[i].load = VK_ATTACHMENT_LOAD_OP_LOAD;
        p.color[i].store = VK_ATTACHMENT_STORE_OP_STORE;
        p.color[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (ds != VK_FORMAT_UNDEFINED) {
        p.has_ds = 1;
        p.ds.format = ds;
        p.ds.load = VK_ATTACHMENT_LOAD_OP_LOAD;
        p.ds.store = VK_ATTACHMENT_STORE_OP_STORE;
        p.ds.stencil_load = stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        p.ds.stencil_store = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        p.ds.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    return re5_vkn_render_pass(&p);
}

extern "C" VkFramebuffer re5_vkn_framebuffer(const re5_vkn_pass *p, VkRenderPass rp, const VkImageView *views,
                                             uint32_t n, VkExtent2D extent)
{
    return native()->framebufferFor(keyOf(p), rp, views, n, extent);
}

extern "C" void re5_vkn_forget_view(VkImageView view)
{
    native()->forgetView(view);
}
