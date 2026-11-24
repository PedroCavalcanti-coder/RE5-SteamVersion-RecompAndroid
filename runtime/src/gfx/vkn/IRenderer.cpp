/*
 * IRenderer.cpp - política TBDR, caches de passe/framebuffer, ciclo de quadro e os renderizadores
 * concretos (ver IRenderer.h).
 */
#include "IRenderer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "re5/log.h"
#include "re5/re5vk.h"
#include "re5/vk_loader.h"
}

namespace re5::vkn {

namespace {

constexpr const char *kCh = "vkn";

bool envFlag(const char *name, bool def)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return def;
    return v[0] == '1';
}

/* FNV-1a 64 sobre campos (sem bytes de preenchimento). */
struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(uint64_t v)
    {
        for (int i = 0; i < 8; i++) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
};

void hashOps(Fnv &f, const AttachmentOps &o)
{
    f.add(o.format);
    f.add(o.samples);
    f.add(o.load);
    f.add(o.store);
    f.add(o.stencilLoad);
    f.add(o.stencilStore);
    f.add(o.initialLayout);
    f.add(o.finalLayout);
    f.add(o.subpassLayout);
}

} // namespace

/* ----------------------------------------------------------------- política ---- */

namespace policy {

VkAttachmentLoadOp loadOp(const AttachmentUse &use, VkImageLayout current)
{
    if (use.clearAtStart) return VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (use.fullyOverwritten) return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    if (current == VK_IMAGE_LAYOUT_UNDEFINED) return VK_ATTACHMENT_LOAD_OP_DONT_CARE; /* nada a carregar */
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

AttachmentOps armColor(const AttachmentUse &use, VkFormat format, VkSampleCountFlagBits samples,
                       VkImageLayout current, bool sampled)
{
    AttachmentOps o;
    o.format = format;
    o.samples = samples;
    o.load = loadOp(use, current);
    o.store = (use.readAfter || use.backbuffer) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    o.initialLayout = o.load == VK_ATTACHMENT_LOAD_OP_LOAD ? current : VK_IMAGE_LAYOUT_UNDEFINED;
    if (use.backbuffer)
        o.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    else if (sampled && o.store == VK_ATTACHMENT_STORE_OP_STORE)
        o.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    else
        o.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return o;
}

AttachmentOps armDepthStencil(const AttachmentUse &depth, const AttachmentUse &stencil, bool hasDepth,
                              bool hasStencil, VkFormat format, VkSampleCountFlagBits samples,
                              VkImageLayout current, bool sampled)
{
    AttachmentOps o;
    o.format = format;
    o.samples = samples;
    if (hasDepth) {
        o.load = loadOp(depth, current);
        o.store = depth.readAfter ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    if (hasStencil) {
        o.stencilLoad = loadOp(stencil, current);
        o.stencilStore = stencil.readAfter ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    const bool loads = o.load == VK_ATTACHMENT_LOAD_OP_LOAD || o.stencilLoad == VK_ATTACHMENT_LOAD_OP_LOAD;
    o.initialLayout = loads ? current : VK_IMAGE_LAYOUT_UNDEFINED;
    const bool stores = o.store == VK_ATTACHMENT_STORE_OP_STORE || o.stencilStore == VK_ATTACHMENT_STORE_OP_STORE;
    o.finalLayout = (sampled && stores) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    return o;
}

AttachmentOps adrenoDepthStencil(const AttachmentUse &depth, const AttachmentUse &stencil, bool hasDepth,
                                 bool hasStencil, VkFormat format, VkSampleCountFlagBits samples,
                                 VkImageLayout current, bool sampled, bool storeOpNone)
{
    AttachmentOps o = armDepthStencil(depth, stencil, hasDepth, hasStencil, format, samples, current, sampled);
    if (!storeOpNone) return o;
    /* Carregado, não escrito e lido depois: o conteúdo na RAM já é o certo; nada a gravar. */
    if (hasDepth && o.load == VK_ATTACHMENT_LOAD_OP_LOAD && !depth.written && o.store == VK_ATTACHMENT_STORE_OP_STORE)
        o.store = VK_ATTACHMENT_STORE_OP_NONE;
    if (hasStencil && o.stencilLoad == VK_ATTACHMENT_LOAD_OP_LOAD && !stencil.written &&
        o.stencilStore == VK_ATTACHMENT_STORE_OP_STORE)
        o.stencilStore = VK_ATTACHMENT_STORE_OP_NONE;
    return o;
}

} // namespace policy

/* ----------------------------------------------------------------- chaves ---- */

bool AttachmentOps::operator==(const AttachmentOps &o) const
{
    return format == o.format && samples == o.samples && load == o.load && store == o.store &&
           stencilLoad == o.stencilLoad && stencilStore == o.stencilStore && initialLayout == o.initialLayout &&
           finalLayout == o.finalLayout && subpassLayout == o.subpassLayout;
}

bool RenderPassKey::operator==(const RenderPassKey &o) const
{
    if (colorCount != o.colorCount || hasDepthStencil != o.hasDepthStencil) return false;
    for (uint32_t i = 0; i < colorCount; i++)
        if (color[i] != o.color[i]) return false;
    return !hasDepthStencil || depthStencil == o.depthStencil;
}

size_t RenderPassKeyHash::operator()(const RenderPassKey &k) const
{
    Fnv f;
    f.add(k.colorCount);
    for (uint32_t i = 0; i < k.colorCount; i++) hashOps(f, k.color[i]);
    f.add(k.hasDepthStencil);
    if (k.hasDepthStencil) hashOps(f, k.depthStencil);
    return static_cast<size_t>(f.h);
}

bool FramebufferKey::operator==(const FramebufferKey &o) const
{
    if (viewCount != o.viewCount || extent.width != o.extent.width || extent.height != o.extent.height ||
        swapchainGeneration != o.swapchainGeneration)
        return false;
    for (uint32_t i = 0; i < viewCount; i++)
        if (views[i] != o.views[i] || formats[i] != o.formats[i] || samples[i] != o.samples[i]) return false;
    return true;
}

size_t FramebufferKeyHash::operator()(const FramebufferKey &k) const
{
    Fnv f;
    f.add(k.viewCount);
    for (uint32_t i = 0; i < k.viewCount; i++) {
        f.add(reinterpret_cast<uint64_t>(k.views[i]));
        f.add(k.formats[i]);
        f.add(k.samples[i]);
    }
    f.add(k.extent.width);
    f.add(k.extent.height);
    f.add(k.swapchainGeneration);
    return static_cast<size_t>(f.h);
}

/* ------------------------------------------------------------------- dxvk ---- */

bool DxvkRenderer::initialize(const RendererCreateInfo &info)
{
    create9_ = info.dxvkCreate9;
    if (!create9_) {
        RE5_ERROR(kCh, "dxvk: Direct3DCreate9 da DXVK não foi entregue ao seletor");
        return false;
    }
    return true;
}

void *DxvkRenderer::createDirect3D9(uint32_t sdkVersion)
{
    if (!create9_) re5_fatal(kCh, "dxvk: createDirect3D9 antes de initialize");
    return create9_(sdkVersion);
}

/* --------------------------------------------------------- base nativa ---- */

NativeVulkanRenderer::NativeVulkanRenderer(GpuVendor requiredVendor) : requiredVendor_(requiredVendor) {}

NativeVulkanRenderer::~NativeVulkanRenderer()
{
    shutdown();
}

bool NativeVulkanRenderer::initialize(const RendererCreateInfo &info)
{
    create9_ = info.dxvkCreate9;
    anyGpu_ = envFlag("RE5_VKN_ANY_GPU", false);
    /* O device Vulkan só sobe com a janela (onSurfaceReady). */
    return true;
}

void *NativeVulkanRenderer::createDirect3D9(uint32_t sdkVersion)
{
    if (!create9_) re5_fatal(kCh, "%s: Direct3DCreate9 da DXVK não foi entregue ao seletor", name());
    void *inner = create9_(sdkVersion);
    if (!inner) return nullptr;
    /*
     * O IDirect3DDevice9 é o de runtime/src/gfx/vk (C): no CreateDevice ele abre este renderizador
     * pela ponte vkn_bridge.h (openForD3D9) e usa daqui o contexto, a memória e os passes.
     */
    RE5_INFO(kCh, "%s: device D3D9 nativo (vk-bootstrap + VMA + VkRenderPass); adaptador consultado na DXVK",
             name());
    return re5vk_wrap_d3d9(inner);
}

bool NativeVulkanRenderer::acceptDevice(const VulkanContext &) const
{
    return true;
}

AttachmentOps NativeVulkanRenderer::chooseColorOps(const AttachmentUse &use, VkFormat format,
                                                   VkSampleCountFlagBits samples, VkImageLayout current,
                                                   bool sampled) const
{
    return policy::armColor(use, format, samples, current, sampled);
}

AttachmentOps NativeVulkanRenderer::chooseDepthStencilOps(const AttachmentUse &depth, const AttachmentUse &stencil,
                                                          bool hasDepth, bool hasStencil, VkFormat format,
                                                          VkSampleCountFlagBits samples, VkImageLayout current,
                                                          bool sampled) const
{
    return policy::armDepthStencil(depth, stencil, hasDepth, hasStencil, format, samples, current, sampled);
}

bool NativeVulkanRenderer::createDevice(SurfaceFactory factory, void *user, uint32_t width, uint32_t height)
{
    if (ctx_.isCreated()) return true;
    anyGpu_ = envFlag("RE5_VKN_ANY_GPU", anyGpu_);

    ContextCreateInfo ci;
    ci.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(re5_vk_get_instance_proc_addr());
    ci.createSurface = factory;
    ci.surfaceUser = user;
    ci.width = width;
    ci.height = height;
    if (!envFlag("RE5_VKN_VSYNC", true)) {
        ci.swapchain.presentModes[0] = VK_PRESENT_MODE_MAILBOX_KHR;
        ci.swapchain.presentModes[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    ci.validation = envFlag("RE5_VKN_VALIDATION", false);
    ci.requiredVendor = anyGpu_ ? GpuVendor::Unknown : requiredVendor_;
    if (!ctx_.create(ci)) return false;
    if (!anyGpu_ && !acceptDevice(ctx_)) {
        ctx_.destroy();
        return false;
    }
    DriverQuirks q = ctx_.quirks();
    tuneQuirks(q);
    ctx_.overrideQuirks(q);

    const char *mb = std::getenv("RE5_VKN_STAGING_MB");
    const VkDeviceSize ring = (VkDeviceSize)(mb && *mb ? std::atoi(mb) : 32) << 20;
    if (!mem_.create(ctx_, ring)) {
        ctx_.destroy();
        return false;
    }
    mem_.setImageRetiredCallback(&NativeVulkanRenderer::onImageRetired, this);
    mem_.setRecordingSerial(nextSerial_.load());
    if (!createFrames()) {
        shutdown();
        return false;
    }
    syncPresentSemaphores();
    re5vk_tbdr_init(&tbdr_, envFlag("RE5VK_TBDR", false));
    RE5_INFO(kCh, "%s: device pronto (%u quadros em voo, staging %llu MiB, oráculo TBDR %s)", name(),
             kFramesInFlight, (unsigned long long)(ring >> 20), tbdr_.enabled ? "ligado" : "desligado");
    return true;
}

bool NativeVulkanRenderer::openForD3D9(ContextCreateInfo ci)
{
    if (ctx_.isCreated()) re5_fatal(kCh, "%s: device D3D9 aberto duas vezes", name());
    anyGpu_ = envFlag("RE5_VKN_ANY_GPU", anyGpu_);
    ci.validation = envFlag("RE5_VKN_VALIDATION", false);
    ci.requiredVendor = anyGpu_ ? GpuVendor::Unknown : requiredVendor_;
    if (!ctx_.create(ci)) return false;
    if (!anyGpu_ && !acceptDevice(ctx_)) {
        ctx_.destroy();
        return false;
    }
    DriverQuirks q = ctx_.quirks();
    tuneQuirks(q);
    ctx_.overrideQuirks(q);
    if (!mem_.create(ctx_, 0)) { /* os anéis de staging e de UBO são do device */
        ctx_.destroy();
        return false;
    }
    hosted_ = true;
    RE5_INFO(kCh, "%s: contexto e memória do device D3D9 prontos (vk-bootstrap + VMA)", name());
    return true;
}

VkFramebuffer NativeVulkanRenderer::framebufferFor(const RenderPassKey &key, VkRenderPass pass, const VkImageView *views,
                                                   uint32_t n, VkExtent2D extent)
{
    if (n > kMaxColorAttachments + 1) re5_fatal(kCh, "framebuffer com %u anexos", n);
    FramebufferKey fk;
    fk.pass = pass;
    for (uint32_t i = 0; i < kMaxColorAttachments; i++) {
        fk.formats[i] = i < key.colorCount ? key.color[i].format : VK_FORMAT_UNDEFINED;
        fk.samples[i] = i < key.colorCount ? key.color[i].samples : VK_SAMPLE_COUNT_1_BIT;
    }
    fk.formats[kMaxColorAttachments] = key.hasDepthStencil ? key.depthStencil.format : VK_FORMAT_UNDEFINED;
    fk.samples[kMaxColorAttachments] = key.hasDepthStencil ? key.depthStencil.samples : VK_SAMPLE_COUNT_1_BIT;
    /* Todas as posições entram na chave (formatos e vazias); ids ~0: do device D3D9, saem só por
     * forgetView (o expurgo do backbuffer olha id 0). */
    for (uint32_t i = 0; i < kMaxColorAttachments + 1; i++) {
        fk.views[i] = i < n ? views[i] : VK_NULL_HANDLE;
        fk.imageIds[i] = ~0ull;
    }
    fk.viewCount = kMaxColorAttachments + 1;
    fk.extent = extent;
    std::lock_guard<std::mutex> lk(fbMutex_);
    auto it = fbCache_.find(fk);
    if (it != fbCache_.end()) return it->second;
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = pass;
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = extent.width;
    ci.height = extent.height;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = ctx_.fns().vkCreateFramebuffer(ctx_.device(), &ci, nullptr, &fb);
    if (r != VK_SUCCESS) re5_fatal(kCh, "vkCreateFramebuffer -> %d", (int)r);
    fbCache_.emplace(fk, fb);
    return fb;
}

void NativeVulkanRenderer::forgetView(VkImageView view)
{
    if (view == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lk(fbMutex_);
    for (auto it = fbCache_.begin(); it != fbCache_.end();) {
        bool uses = false;
        for (uint32_t i = 0; i < it->first.viewCount; i++)
            if (it->first.views[i] == view) uses = true;
        if (uses) {
            ctx_.fns().vkDestroyFramebuffer(ctx_.device(), it->second, nullptr);
            it = fbCache_.erase(it);
        } else {
            ++it;
        }
    }
}

bool NativeVulkanRenderer::createFrames()
{
    const DeviceFns &f = ctx_.fns();
    VkDevice dev = ctx_.device();
    for (auto &s : frames_) {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = ctx_.graphicsQueue().family;
        if (f.vkCreateCommandPool(dev, &pi, nullptr, &s.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (f.vkAllocateCommandBuffers(dev, &ai, &s.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (f.vkCreateFence(dev, &fi, nullptr, &s.fence) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (f.vkCreateSemaphore(dev, &si, nullptr, &s.imageReady) != VK_SUCCESS) return false;
        s.serial = 0;
    }
    return true;
}

void NativeVulkanRenderer::destroyFrames()
{
    const DeviceFns &f = ctx_.fns();
    VkDevice dev = ctx_.device();
    for (auto &s : frames_) {
        if (s.imageReady) f.vkDestroySemaphore(dev, s.imageReady, nullptr);
        if (s.fence) f.vkDestroyFence(dev, s.fence, nullptr);
        if (s.pool) f.vkDestroyCommandPool(dev, s.pool, nullptr); /* libera o cmd junto */
        s = FrameSlot{};
    }
}

void NativeVulkanRenderer::destroyPresentSemaphores()
{
    for (VkSemaphore s : renderDone_) ctx_.fns().vkDestroySemaphore(ctx_.device(), s, nullptr);
    renderDone_.clear();
}

void NativeVulkanRenderer::syncPresentSemaphores()
{
    const size_t want = ctx_.swapchain().images.size();
    if (renderDone_.size() == want) return;
    /* Só chamado com o device ocioso (criação ou recriação da swapchain). */
    destroyPresentSemaphores();
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderDone_.resize(want, VK_NULL_HANDLE);
    for (auto &s : renderDone_)
        if (ctx_.fns().vkCreateSemaphore(ctx_.device(), &si, nullptr, &s) != VK_SUCCESS)
            re5_fatal(kCh, "vkCreateSemaphore (present) falhou");
}

void NativeVulkanRenderer::shutdown()
{
    if (!ctx_.isCreated()) return;
    ctx_.fns().vkDeviceWaitIdle(ctx_.device());
    inPass_ = false;
    inFrame_ = false;
    mem_.setImageRetiredCallback(nullptr, nullptr);
    destroyCaches();
    destroyPresentSemaphores();
    destroyFrames();
    mem_.collect(UINT64_MAX);
    mem_.destroy(); /* aborta listando o que vazou */
    ctx_.destroy();
}

void NativeVulkanRenderer::onSurfaceLost()
{
    if (!ctx_.isCreated()) return;
    if (inFrame_) re5_fatal(kCh, "%s: janela perdida no meio de um quadro", name());
    ctx_.releaseSurface(); /* espera o device */
    purgeBackbufferFramebuffers();
    destroyPresentSemaphores();
}

bool NativeVulkanRenderer::onSurfaceReady(SurfaceFactory factory, void *user, uint32_t width, uint32_t height)
{
    if (!ctx_.isCreated()) return createDevice(factory, user, width, height);
    if (!ctx_.recreateSurface(factory, user)) return false;
    purgeBackbufferFramebuffers();
    syncPresentSemaphores();
    return true;
}

bool NativeVulkanRenderer::recreateSwapchain()
{
    const VkExtent2D e = ctx_.swapchain().extent;
    /* 0x0: a superfície diz o tamanho (rotação/redimensionamento no Android). */
    if (!ctx_.recreateSwapchain(0, 0) && !ctx_.recreateSwapchain(e.width, e.height)) return false;
    purgeBackbufferFramebuffers();
    syncPresentSemaphores();
    return true;
}

VkCommandBuffer NativeVulkanRenderer::commandBuffer() const
{
    return frames_[frameIndex_].cmd;
}

bool NativeVulkanRenderer::beginFrame()
{
    if (!ctx_.isCreated()) re5_fatal(kCh, "%s: beginFrame sem device", name());
    if (inFrame_) re5_fatal(kCh, "%s: beginFrame dentro de um quadro", name());
    const DeviceFns &f = ctx_.fns();
    VkDevice dev = ctx_.device();
    FrameSlot &s = frames_[frameIndex_];

    if (f.vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        re5_fatal(kCh, "vkWaitForFences falhou (device perdido?)");
    /* A fila é em ordem: o slot concluído implica todos os seriais anteriores. */
    completedSerial_ = std::max(completedSerial_, s.serial);
    collectFramebuffers(completedSerial_);
    mem_.collect(completedSerial_);

    if (!ctx_.headless()) {
        SwapchainStatus st = ctx_.acquire(s.imageReady, &imageIndex_);
        if (st == SwapchainStatus::Recreate) {
            if (!recreateSwapchain()) return false;
            st = ctx_.acquire(s.imageReady, &imageIndex_);
            if (st == SwapchainStatus::Recreate) return false;
        }
    }

    f.vkResetFences(dev, 1, &s.fence);
    f.vkResetCommandPool(dev, s.pool, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (f.vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS) re5_fatal(kCh, "vkBeginCommandBuffer falhou");
    s.serial = nextSerial_.load();
    inFrame_ = true;
    backbufferWritten_ = false;
    backbufferLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void NativeVulkanRenderer::endFrame()
{
    if (!inFrame_) re5_fatal(kCh, "%s: endFrame sem beginFrame", name());
    if (inPass_) endPass();
    const DeviceFns &f = ctx_.fns();
    FrameSlot &s = frames_[frameIndex_];
    mem_.recordUploads(s.cmd, s.serial);

    const bool present = !ctx_.headless();
    if (present && !backbufferWritten_) {
        /* Nada desenhou no backbuffer: apresenta preto em vez de lixo (UNDEFINED). */
        VkImage img = ctx_.swapchain().images[imageIndex_];
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        f.vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &b);
        VkClearColorValue black{};
        f.vkCmdClearColorImage(s.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        f.vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &b);
    }
    if (f.vkEndCommandBuffer(s.cmd) != VK_SUCCESS) re5_fatal(kCh, "vkEndCommandBuffer falhou");

    const VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    if (present) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s.imageReady;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone_[imageIndex_];
    }
    VkResult r = f.vkQueueSubmit(ctx_.graphicsQueue().queue, 1, &si, s.fence);
    if (r != VK_SUCCESS) re5_fatal(kCh, "vkQueueSubmit -> %d", (int)r);

    if (present) {
        SwapchainStatus st = ctx_.present(renderDone_[imageIndex_], imageIndex_);
        if (st != SwapchainStatus::Ok && !recreateSwapchain())
            RE5_WARN(kCh, "%s: swapchain não recriada agora; tenta no próximo quadro", name());
    }

    re5vk_tbdr_end_frame(&tbdr_);
    mem_.setRecordingSerial(++nextSerial_);
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    inFrame_ = false;
}

void NativeVulkanRenderer::beginPass(PassDesc &desc)
{
    if (!inFrame_) re5_fatal(kCh, "%s: beginPass fora de um quadro", name());
    if (inPass_) re5_fatal(kCh, "%s: beginPass com passe aberto", name());
    if (desc.colorCount > kMaxColorAttachments) re5_fatal(kCh, "%u alvos de cor (máx. 4)", desc.colorCount);
    curPass_ = desc;
    beginPassInternal();
    desc = curPass_; /* devolve os vereditos (readAfter) */
}

void NativeVulkanRenderer::beginPassInternal()
{
    PassDesc &d = curPass_;
    const DeviceFns &f = ctx_.fns();
    FrameSlot &s = frames_[frameIndex_];

    /* Cópias não podem ficar dentro do passe. */
    mem_.recordUploads(s.cmd, s.serial);

    for (uint32_t i = 0; i < d.colorCount; i++)
        if (!d.color[i].backbuffer && !d.color[i].image) re5_fatal(kCh, "alvo de cor %u sem imagem", i);
    if ((d.hasDepth || d.hasStencil) && !d.depth.image) re5_fatal(kCh, "passe com profundidade sem imagem");
    if (ctx_.headless())
        for (uint32_t i = 0; i < d.colorCount; i++)
            if (d.color[i].backbuffer) re5_fatal(kCh, "backbuffer num contexto sem janela");

    /* ---- oráculo TBDR: quem precisa de STORE ---- */
    re5vk_tbdr_pass_in in{};
    in.width = d.extent.width;
    in.height = d.extent.height;
    for (uint32_t i = 0; i < d.colorCount; i++) {
        AttachmentUse &u = d.color[i];
        in.color_is_backbuffer[i] = u.backbuffer ? 1u : 0u;
        in.color_is_texture[i] = (u.image && (u.image->usage() & VK_IMAGE_USAGE_SAMPLED_BIT)) ? 1u : 0u;
        if (u.backbuffer || !u.tbdr) continue;
        in.color_img[i] = u.image->id();
        in.color_sub[i] = u.layer * u.image->mipLevels() + u.mipLevel;
        in.color_subs[i] = u.tbdr;
        if (u.clearAtStart) in.color_cleared |= (uint8_t)(1u << i);
        if (policy::loadOp(u, u.image->layout()) == VK_ATTACHMENT_LOAD_OP_LOAD) in.color_loaded |= (uint8_t)(1u << i);
    }
    re5vk_tbdr_sub *dsSub = d.depth.tbdr ? d.depth.tbdr : d.stencil.tbdr;
    if ((d.hasDepth || d.hasStencil) && dsSub) {
        Image *img = d.depth.image;
        in.ds_img = img->id();
        in.ds_sub = d.depth.layer * img->mipLevels() + d.depth.mipLevel;
        in.ds_sub_state = dsSub;
        in.has_depth = d.hasDepth;
        in.has_stencil = d.hasStencil;
        in.depth_cleared = d.depth.clearAtStart;
        in.depth_loaded = d.hasDepth && policy::loadOp(d.depth, img->layout()) == VK_ATTACHMENT_LOAD_OP_LOAD;
        in.stencil_cleared = d.stencil.clearAtStart;
        in.stencil_loaded = d.hasStencil && policy::loadOp(d.stencil, img->layout()) == VK_ATTACHMENT_LOAD_OP_LOAD;
        in.ds_is_texture = (img->usage() & VK_IMAGE_USAGE_SAMPLED_BIT) ? 1u : 0u;
        in.stencil_enabled = (d.hasStencil && (d.stencil.clearAtStart || d.stencil.written)) ? 1u : 0u;
        in.depth_write_enabled = d.depth.written ? 1u : 0u;
    }
    re5vk_tbdr_pass_out out{};
    re5vk_tbdr_begin_pass(&tbdr_, &in, &out);
    for (uint32_t i = 0; i < d.colorCount; i++) {
        AttachmentUse &u = d.color[i];
        u.readAfter = u.backbuffer || !u.tbdr || !(out.color_store_dont_care >> i & 1u);
    }
    d.depth.readAfter = !dsSub || !out.depth_store_dont_care;
    d.stencil.readAfter = !dsSub || !out.stencil_store_dont_care;

    /* ---- ops, passe e views ---- */
    RenderPassKey key;
    FramebufferKey fk;
    std::array<VkClearValue, kMaxColorAttachments + 1> clears{};
    VkExtent2D extent = d.extent;
    key.colorCount = d.colorCount;
    for (uint32_t i = 0; i < d.colorCount; i++) {
        AttachmentUse &u = d.color[i];
        if (u.backbuffer) {
            const SwapchainState &sc = ctx_.swapchain();
            key.color[i] = chooseColorOps(u, sc.format, VK_SAMPLE_COUNT_1_BIT, backbufferLayout_, false);
            fk.views[i] = sc.views[imageIndex_];
            fk.imageIds[i] = 0;
            if (!extent.width) extent = sc.extent;
        } else {
            Image &img = *u.image;
            key.color[i] = chooseColorOps(u, img.format(), img.samples(), img.layout(),
                                          (img.usage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
            fk.views[i] = mem_.attachmentView(img, u.mipLevel, u.layer);
            fk.imageIds[i] = img.id();
            if (!extent.width)
                extent = {std::max(1u, img.extent().width >> u.mipLevel), std::max(1u, img.extent().height >> u.mipLevel)};
        }
        clears[i] = u.clear;
    }
    uint32_t nviews = d.colorCount;
    if (d.hasDepth || d.hasStencil) {
        Image &img = *d.depth.image;
        key.hasDepthStencil = true;
        key.depthStencil = chooseDepthStencilOps(d.depth, d.stencil, d.hasDepth, d.hasStencil, img.format(),
                                                 img.samples(), img.layout(),
                                                 (img.usage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
        fk.views[nviews] = mem_.attachmentView(img, d.depth.mipLevel, d.depth.layer);
        fk.imageIds[nviews] = img.id();
        clears[nviews].depthStencil = {d.depth.clear.depthStencil.depth, d.stencil.clear.depthStencil.stencil};
        if (!extent.width)
            extent = {std::max(1u, img.extent().width >> d.depth.mipLevel),
                      std::max(1u, img.extent().height >> d.depth.mipLevel)};
        nviews++;
    }
    if (!extent.width || !extent.height) re5_fatal(kCh, "passe sem anexos nem tamanho");

    VkRenderPass rp = getRenderPass(key);
    fk.pass = rp;
    for (uint32_t i = 0; i < key.colorCount; i++) {
        fk.formats[i] = key.color[i].format;
        fk.samples[i] = key.color[i].samples;
    }
    if (key.hasDepthStencil) {
        fk.formats[key.colorCount] = key.depthStencil.format;
        fk.samples[key.colorCount] = key.depthStencil.samples;
    }
    fk.viewCount = nviews;
    fk.extent = extent;
    fk.swapchainGeneration = ctx_.swapchain().generation;
    VkFramebuffer fb = getFramebuffer(fk);

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, extent};
    bi.clearValueCount = nviews;
    bi.pClearValues = clears.data();
    f.vkCmdBeginRenderPass(s.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

    /* Layout depois do passe (ninguém fora do passe toca a imagem até endPass). */
    for (uint32_t i = 0; i < d.colorCount; i++) {
        if (d.color[i].backbuffer) {
            backbufferLayout_ = key.color[i].finalLayout;
            backbufferWritten_ = true;
        } else {
            d.color[i].image->setLayout(key.color[i].finalLayout);
        }
    }
    if (key.hasDepthStencil) d.depth.image->setLayout(key.depthStencil.finalLayout);

    inPass_ = true;
    curRenderPass_ = rp;
}

void NativeVulkanRenderer::endPass()
{
    if (!inPass_) re5_fatal(kCh, "%s: endPass sem passe aberto", name());
    ctx_.fns().vkCmdEndRenderPass(frames_[frameIndex_].cmd);
    inPass_ = false;
    curRenderPass_ = VK_NULL_HANDLE;
}

void NativeVulkanRenderer::midPassClear(uint32_t colorMask, bool depth, bool stencil, const VkClearColorValue &color,
                                        VkClearDepthStencilValue depthStencil)
{
    if (!inPass_) re5_fatal(kCh, "%s: midPassClear sem passe aberto", name());
    PassDesc &d = curPass_;
    if (splitPassOnMidClear() || ctx_.quirks().avoidMidPassClearAttachments) {
        endPass();
        for (uint32_t i = 0; i < d.colorCount; i++) {
            AttachmentUse &u = d.color[i];
            const bool c = (colorMask >> i) & 1u;
            u.clearAtStart = c;
            u.fullyOverwritten = false;
            if (c) u.clear.color = color;
        }
        d.depth.clearAtStart = depth && d.hasDepth;
        d.depth.fullyOverwritten = false;
        if (depth) d.depth.clear.depthStencil.depth = depthStencil.depth;
        d.stencil.clearAtStart = stencil && d.hasStencil;
        d.stencil.fullyOverwritten = false;
        if (stencil) d.stencil.clear.depthStencil.stencil = depthStencil.stencil;
        beginPassInternal();
        return;
    }
    std::array<VkClearAttachment, kMaxColorAttachments + 1> atts{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < d.colorCount; i++) {
        if (!((colorMask >> i) & 1u)) continue;
        atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        atts[n].colorAttachment = i;
        atts[n].clearValue.color = color;
        n++;
    }
    VkImageAspectFlags dsAspect = 0;
    if (depth && d.hasDepth) dsAspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (stencil && d.hasStencil) dsAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dsAspect) {
        atts[n].aspectMask = dsAspect;
        atts[n].clearValue.depthStencil = depthStencil;
        n++;
    }
    if (!n) return;
    VkExtent2D e = d.extent;
    if (!e.width) {
        const Image *img = d.colorCount && d.color[0].image ? d.color[0].image : d.depth.image;
        if (img) e = {img->extent().width, img->extent().height};
        else e = ctx_.swapchain().extent;
    }
    VkClearRect rect{{{0, 0}, e}, 0, 1};
    ctx_.fns().vkCmdClearAttachments(frames_[frameIndex_].cmd, n, atts.data(), 1, &rect);
}

VkRenderPass NativeVulkanRenderer::getRenderPass(const RenderPassKey &key)
{
    auto it = passCache_.find(key);
    if (it != passCache_.end()) return it->second;

    std::array<VkAttachmentDescription, kMaxColorAttachments + 1> att{};
    std::array<VkAttachmentReference, kMaxColorAttachments> colorRefs{};
    VkAttachmentReference dsRef{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < key.colorCount; i++) {
        const AttachmentOps &o = key.color[i];
        if (o.format == VK_FORMAT_UNDEFINED) {
            colorRefs[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
            continue;
        }
        att[n].format = o.format;
        att[n].samples = o.samples;
        att[n].loadOp = o.load;
        att[n].storeOp = o.store;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = o.initialLayout;
        att[n].finalLayout = o.finalLayout;
        colorRefs[i] = {n, o.subpassLayout != VK_IMAGE_LAYOUT_UNDEFINED ? o.subpassLayout
                                                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        n++;
    }
    if (key.hasDepthStencil) {
        const AttachmentOps &o = key.depthStencil;
        att[n].format = o.format;
        att[n].samples = o.samples;
        att[n].loadOp = o.load;
        att[n].storeOp = o.store;
        att[n].stencilLoadOp = o.stencilLoad;
        att[n].stencilStoreOp = o.stencilStore;
        att[n].initialLayout = o.initialLayout;
        att[n].finalLayout = o.finalLayout;
        dsRef = {n, o.subpassLayout != VK_IMAGE_LAYOUT_UNDEFINED ? o.subpassLayout
                                                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        n++;
    }

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = key.colorCount;
    sub.pColorAttachments = colorRefs.data();
    sub.pDepthStencilAttachment = key.hasDepthStencil ? &dsRef : nullptr;

    /*
     * Dependências externas estreitas (sem ALL_COMMANDS): antes, escritas de anexo/cópia e
     * leituras de shader do passe anterior; depois, leituras de shader/cópia do passe seguinte.
     */
    const VkPipelineStageFlags attStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    std::array<VkSubpassDependency, 2> dep{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = attStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstStageMask = attStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstAccessMask = attAccess | VK_ACCESS_SHADER_READ_BIT;
    dep[0].dependencyFlags = 0; /* a origem inclui cópias (fora do espaço do framebuffer) */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = attStages;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = attStages | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].dstAccessMask = attAccess | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = n;
    ci.pAttachments = att.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    /* RE5_VKN_RP_DEPS=0: só as dependências implícitas (bisseção de custo de GPU no Mali; o device
     * D3D9 já emite as próprias barreiras entre passes). */
    static const bool deps = envFlag("RE5_VKN_RP_DEPS", true);
    ci.dependencyCount = deps ? (uint32_t)dep.size() : 0u;
    ci.pDependencies = dep.data();
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = ctx_.fns().vkCreateRenderPass(ctx_.device(), &ci, nullptr, &rp);
    if (r != VK_SUCCESS) re5_fatal(kCh, "vkCreateRenderPass -> %d", (int)r);
    passCache_.emplace(key, rp);
    return rp;
}

VkFramebuffer NativeVulkanRenderer::getFramebuffer(const FramebufferKey &key)
{
    std::lock_guard<std::mutex> lk(fbMutex_);
    auto it = fbCache_.find(key);
    if (it != fbCache_.end()) return it->second;
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = key.pass;
    ci.attachmentCount = key.viewCount;
    ci.pAttachments = key.views.data();
    ci.width = key.extent.width;
    ci.height = key.extent.height;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = ctx_.fns().vkCreateFramebuffer(ctx_.device(), &ci, nullptr, &fb);
    if (r != VK_SUCCESS) re5_fatal(kCh, "vkCreateFramebuffer -> %d", (int)r);
    fbCache_.emplace(key, fb);
    return fb;
}

size_t NativeVulkanRenderer::cachedFramebuffers() const
{
    std::lock_guard<std::mutex> lk(fbMutex_);
    return fbCache_.size();
}

void NativeVulkanRenderer::onImageRetired(uint64_t imageId, void *user)
{
    auto *self = static_cast<NativeVulkanRenderer *>(user);
    /* O framebuffer pode estar no envio sendo gravado: sai depois dele. */
    const uint64_t serial = self->nextSerial_.load();
    std::lock_guard<std::mutex> lk(self->fbMutex_);
    for (auto it = self->fbCache_.begin(); it != self->fbCache_.end();) {
        bool uses = false;
        for (uint32_t i = 0; i < it->first.viewCount; i++)
            if (it->first.imageIds[i] == imageId) uses = true;
        if (uses) {
            self->fbRetired_.push_back(RetiredFb{serial, it->second});
            it = self->fbCache_.erase(it);
        } else {
            ++it;
        }
    }
}

void NativeVulkanRenderer::collectFramebuffers(uint64_t completedSerial)
{
    std::lock_guard<std::mutex> lk(fbMutex_);
    auto keep = fbRetired_.begin();
    for (auto &r : fbRetired_) {
        if (r.serial <= completedSerial) ctx_.fns().vkDestroyFramebuffer(ctx_.device(), r.fb, nullptr);
        else *keep++ = r;
    }
    fbRetired_.erase(keep, fbRetired_.end());
}

void NativeVulkanRenderer::purgeBackbufferFramebuffers()
{
    std::lock_guard<std::mutex> lk(fbMutex_);
    for (auto it = fbCache_.begin(); it != fbCache_.end();) {
        bool bb = false;
        for (uint32_t i = 0; i < it->first.viewCount; i++)
            if (it->first.imageIds[i] == 0) bb = true;
        if (bb) {
            ctx_.fns().vkDestroyFramebuffer(ctx_.device(), it->second, nullptr); /* device ocioso */
            it = fbCache_.erase(it);
        } else {
            ++it;
        }
    }
}

void NativeVulkanRenderer::destroyCaches()
{
    const DeviceFns &f = ctx_.fns();
    VkDevice dev = ctx_.device();
    {
        std::lock_guard<std::mutex> lk(fbMutex_);
        for (auto &kv : fbCache_) f.vkDestroyFramebuffer(dev, kv.second, nullptr);
        for (auto &r : fbRetired_) f.vkDestroyFramebuffer(dev, r.fb, nullptr);
        fbCache_.clear();
        fbRetired_.clear();
    }
    for (auto &kv : passCache_) f.vkDestroyRenderPass(dev, kv.second, nullptr);
    passCache_.clear();
}

/* ------------------------------------------------------------ vkadreno ---- */

AttachmentOps AdrenoRenderer::chooseDepthStencilOps(const AttachmentUse &depth, const AttachmentUse &stencil,
                                                    bool hasDepth, bool hasStencil, VkFormat format,
                                                    VkSampleCountFlagBits samples, VkImageLayout current,
                                                    bool sampled) const
{
    return policy::adrenoDepthStencil(depth, stencil, hasDepth, hasStencil, format, samples, current, sampled,
                                      ctx_.caps().storeOpNone);
}

bool AdrenoRenderer::acceptDevice(const VulkanContext &ctx) const
{
    if (ctx.vendor() != GpuVendor::Qualcomm) {
        RE5_ERROR(kCh, "vkadreno exige GPU Adreno; esta é '%s'. Use vkmali ou dxvk.", ctx.properties().deviceName);
        return false;
    }
    RE5_INFO(kCh, "vkadreno: driver %s", ctx.driver() == GpuDriver::MesaTurnip ? "Turnip (Mesa)" : "Qualcomm");
    return true;
}

/* -------------------------------------------------------------- vkmali ---- */

bool MaliRenderer::acceptDevice(const VulkanContext &ctx) const
{
    if (ctx.vendor() != GpuVendor::Arm) {
        RE5_ERROR(kCh, "vkmali exige GPU Mali; esta é '%s'. Use vkadreno ou dxvk.", ctx.properties().deviceName);
        return false;
    }
    if (!ctx.caps().lazilyAllocatedMemory)
        RE5_WARN(kCh, "vkmali: driver sem memória LAZILY_ALLOCATED; profundidade transitória vai para RAM");
    return true;
}

/* ---------------------------------------------------------------- fábrica ---- */

std::unique_ptr<IRenderer> createRenderer(re5_renderer_kind kind)
{
    switch (kind) {
    case RE5_RENDERER_DXVK: return std::make_unique<DxvkRenderer>();
    case RE5_RENDERER_VKADRENO: return std::make_unique<AdrenoRenderer>();
    case RE5_RENDERER_VKMALI: return std::make_unique<MaliRenderer>();
    default: return nullptr;
    }
}

} // namespace re5::vkn
