/*
 * IRenderer.h - seletor de renderizador (vkadreno / vkmali / dxvk) e a base dos nativos.
 *
 * Herança:
 *
 *   IRenderer                         interface que re5/renderer.h (C) expõe ao runtime
 *   ├── DxvkRenderer                  fallback: só entrega o IDirect3D9 da libdxvk_d3d9.so
 *   └── NativeVulkanRenderer          abstrata: VulkanContext + VulkanMemory + passes TBDR
 *       ├── AdrenoRenderer            vkadreno: Qualcomm proprietário e Turnip (Mesa)
 *       └── MaliRenderer              vkmali: só as regras TBDR da ARM
 *
 * Passes (TBDR): o jogo fala D3D9 (SetRenderTarget/Clear/Draw/StretchRect); a camada D3D9 traduz
 * para PassDesc. O renderizador escolhe loadOp/storeOp por anexo:
 * Oráculo: desligado por padrão (RE5VK_TBDR=1 liga), como no re5vk desde 11/11: ele prevê pelo
 * quadro anterior e, quando a ordem dos passes muda, já descartou um anexo que era lido (lixo de
 * profundidade/cor medido no aparelho). Desligado, todo anexo usa STORE.
 *
 *   - load: CLEAR se o passe começa com Clear completo; DONT_CARE se vai ser sobrescrito
 *     inteiro (ou se o conteúdo ainda é indefinido); LOAD só quando o conteúdo anterior é lido.
 *   - store: STORE só se alguém lê depois (amostragem, StretchRect, GetRenderTargetData,
 *     Present, LOAD do próximo passe). Profundidade/estêncil: DONT_CARE por padrão; a prova de
 *     que ninguém lê vem do oráculo existente (vk_tbdr.h, re5vk_tbdr_begin_pass), não de palpite.
 *     Sem estado do oráculo para o anexo, o padrão é o seguro (STORE).
 *   - Anexo que nunca sai do tile (profundidade de passe único, MSAA resolvido no passe) usa
 *     MemoryUsage::Transient (LAZILY_ALLOCATED + TRANSIENT_ATTACHMENT).
 * Isso segue as recomendações do ARM Vulkan-Samples (render_passes, msaa) e o modelo de
 * "discardStart/discardEnd" por anexo do Filament (VulkanFboCache).
 */
#ifndef RE5_VKN_IRENDERER_H
#define RE5_VKN_IRENDERER_H

#include "re5/renderer.h"
#include "../vk/vk_tbdr.h"
#include "VulkanContext.h"
#include "VulkanMemory.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace re5::vkn {

/* ---------------------------------------------------------------- interface ---- */

using Create9Fn = void *(*)(uint32_t sdkVersion);

struct RendererCreateInfo {
    Create9Fn dxvkCreate9 = nullptr; /* da ponte d3d9_host.c (consultas de adaptador) */
};

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual re5_renderer_kind kind() const = 0;
    const char *name() const { return re5_renderer_name(kind()); }

    /* Sobe o que não depende de janela. false: já logou; a fachada não cai para outro sozinha. */
    virtual bool initialize(const RendererCreateInfo &info) = 0;
    virtual void shutdown() = 0;

    /* IDirect3D9* do host (o que re5_renderer_create_d3d9 devolve). */
    virtual void *createDirect3D9(uint32_t sdkVersion) = 0;

    /* Ciclo de vida da janela no Android (SDL: APP_WILLENTERBACKGROUND / DIDENTERFOREGROUND). */
    virtual void onSurfaceLost() = 0;
    virtual bool onSurfaceReady(SurfaceFactory factory, void *user, uint32_t width, uint32_t height) = 0;

protected:
    IRenderer() = default;
    IRenderer(const IRenderer &) = delete;
    IRenderer &operator=(const IRenderer &) = delete;
};

/* Fábrica: única criadora de renderizadores (usada pela fachada C). nullptr para RE5_RENDERER_GL. */
std::unique_ptr<IRenderer> createRenderer(re5_renderer_kind kind);

/* ------------------------------------------------------------------- dxvk ---- */

class DxvkRenderer final : public IRenderer {
public:
    re5_renderer_kind kind() const override { return RE5_RENDERER_DXVK; }
    bool initialize(const RendererCreateInfo &info) override;
    void shutdown() override { create9_ = nullptr; }
    void *createDirect3D9(uint32_t sdkVersion) override;
    void onSurfaceLost() override {}   /* a DXVK cuida da própria swapchain */
    bool onSurfaceReady(SurfaceFactory, void *, uint32_t, uint32_t) override { return true; }

private:
    Create9Fn create9_ = nullptr;
};

/* -------------------------------------------------------- descrição de passe ---- */

constexpr uint32_t kMaxColorAttachments = 4; /* D3D9: 4 MRTs */

/* O que o passe faz com cada anexo, em termos do D3D9 (sem VkAttachment*Op ainda). */
struct AttachmentUse {
    Image *image = nullptr;         /* nullptr + backbuffer = imagem corrente da swapchain */
    bool backbuffer = false;
    uint32_t mipLevel = 0, layer = 0;
    bool clearAtStart = false;      /* Clear completo antes do 1º draw */
    bool fullyOverwritten = false;  /* sem Clear, mas o passe cobre o alvo inteiro */
    bool written = true;            /* o passe escreve (false: profundidade só testada) */
    bool readAfter = true;          /* preenchido por beginPass com o veredito do oráculo */
    VkClearValue clear{};
    re5vk_tbdr_sub *tbdr = nullptr; /* estado do oráculo deste sub-recurso (nullptr = STORE seguro) */
};

struct PassDesc {
    std::array<AttachmentUse, kMaxColorAttachments> color{};
    uint32_t colorCount = 0;
    AttachmentUse depth{};          /* aspecto profundidade */
    AttachmentUse stencil{};        /* aspecto estêncil (mesma imagem de depth; image é ignorado) */
    bool hasDepth = false, hasStencil = false;
    VkExtent2D extent{};
};

/* Ops efetivas, já decididas pela política do fabricante. Também é a chave do cache. */
struct AttachmentOps {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkAttachmentLoadOp stencilLoad = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp stencilStore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; /* UNDEFINED quando nada é carregado */
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout subpassLayout = VK_IMAGE_LAYOUT_UNDEFINED; /* UNDEFINED = o ótimo do tipo de anexo */
    bool operator==(const AttachmentOps &o) const;
    bool operator!=(const AttachmentOps &o) const { return !(*this == o); }
};

/*
 * Política pura (sem GPU; testada em tests/unit/test_vkn_policy.cpp).
 *   current: layout atual da imagem; sampled: a imagem tem VK_IMAGE_USAGE_SAMPLED_BIT.
 *   storeOpNone: o device aceita VK_ATTACHMENT_STORE_OP_NONE.
 */
namespace policy {
VkAttachmentLoadOp loadOp(const AttachmentUse &use, VkImageLayout current);
AttachmentOps armColor(const AttachmentUse &use, VkFormat format, VkSampleCountFlagBits samples,
                       VkImageLayout current, bool sampled);
AttachmentOps armDepthStencil(const AttachmentUse &depth, const AttachmentUse &stencil, bool hasDepth,
                              bool hasStencil, VkFormat format, VkSampleCountFlagBits samples,
                              VkImageLayout current, bool sampled);
/* Adreno: aspecto carregado, lido depois e não escrito -> STORE_OP_NONE (sem resolve GMEM->RAM). */
AttachmentOps adrenoDepthStencil(const AttachmentUse &depth, const AttachmentUse &stencil, bool hasDepth,
                                 bool hasStencil, VkFormat format, VkSampleCountFlagBits samples,
                                 VkImageLayout current, bool sampled, bool storeOpNone);
} // namespace policy

/* color[i].format == VK_FORMAT_UNDEFINED: posição vazia (VK_ATTACHMENT_UNUSED), como RT1 sem RT0. */
struct RenderPassKey {
    std::array<AttachmentOps, kMaxColorAttachments> color{};
    uint32_t colorCount = 0;
    AttachmentOps depthStencil{};
    bool hasDepthStencil = false;
    bool operator==(const RenderPassKey &o) const;
};
struct RenderPassKeyHash {
    size_t operator()(const RenderPassKey &k) const;
};

/*
 * Framebuffer: chaveado por views + extent + formatos/amostras (compatibilidade de passe, não as
 * ops: um framebuffer serve a todo VkRenderPass compatível). 'pass' é só o usado para criá-lo.
 * ids das imagens para descartar quando morrem.
 */
struct FramebufferKey {
    VkRenderPass pass = VK_NULL_HANDLE; /* fora da igualdade */
    std::array<VkFormat, kMaxColorAttachments + 1> formats{};
    std::array<VkSampleCountFlagBits, kMaxColorAttachments + 1> samples{};
    std::array<VkImageView, kMaxColorAttachments + 1> views{};
    std::array<uint64_t, kMaxColorAttachments + 1> imageIds{}; /* 0 = backbuffer */
    uint32_t viewCount = 0;
    VkExtent2D extent{};
    uint64_t swapchainGeneration = 0;
    bool operator==(const FramebufferKey &o) const;
};
struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey &k) const;
};

/* --------------------------------------------------------- base nativa ---- */

class NativeVulkanRenderer : public IRenderer {
public:
    ~NativeVulkanRenderer() override;

    bool initialize(const RendererCreateInfo &info) override;
    void shutdown() override;
    void *createDirect3D9(uint32_t sdkVersion) override;
    void onSurfaceLost() override;
    bool onSurfaceReady(SurfaceFactory factory, void *user, uint32_t width, uint32_t height) override;

    /* Sobe contexto, memória e quadros (onSurfaceReady usa; testes chamam com factory nullptr). */
    bool createDevice(SurfaceFactory factory, void *user, uint32_t width, uint32_t height);

    /*
     * Modo do device D3D9 (vkn_bridge.h): só contexto e memória. Envios, quadros e layouts são do
     * device; daqui ele usa os caches de VkRenderPass/VkFramebuffer abaixo.
     */
    bool openForD3D9(ContextCreateInfo ci);
    bool hostedForD3D9() const { return hosted_; }
    VkRenderPass renderPassFor(const RenderPassKey &key) { return getRenderPass(key); }
    /* views: só as posições usadas, na ordem (cores e depois profundidade). */
    VkFramebuffer framebufferFor(const RenderPassKey &key, VkRenderPass pass, const VkImageView *views, uint32_t n,
                                 VkExtent2D extent);
    /* A view vai ser destruída (GPU já terminou com ela): framebuffers que a usam saem já. */
    void forgetView(VkImageView view);

    /* Quadro: espera o slot (fence), coleta a memória concluída, adquire a imagem. */
    bool beginFrame();
    void endFrame(); /* submit + present; fim de quadro no oráculo TBDR */

    /* Abre/fecha o passe; entre eles a camada D3D9 grava draws em commandBuffer(). */
    void beginPass(PassDesc &desc);
    void endPass();

    /*
     * Clear no meio do passe aberto (D3D9 Clear depois de draws). Com splitPassOnMidClear() o
     * passe é fechado e reaberto com loadOp=CLEAR nos aspectos limpos; senão vkCmdClearAttachments.
     */
    void midPassClear(uint32_t colorMask, bool depth, bool stencil, const VkClearColorValue &color,
                      VkClearDepthStencilValue depthStencil);

    /* Profundidade que nunca é lida deve nascer Transient (LAZILY_ALLOCATED)? */
    bool wantsTransientDepth() const { return depthCanBeTransient() && ctx_.caps().lazilyAllocatedMemory; }

    VkCommandBuffer commandBuffer() const;
    uint64_t currentSerial() const { return nextSerial_.load(); }
    uint64_t completedSerial() const { return completedSerial_; }
    bool inPass() const { return inPass_; }
    /* Oráculo TBDR: a camada D3D9 avisa leituras (amostragem, StretchRect...) com re5vk_tbdr_mark_read. */
    re5vk_tbdr &tbdrOracle() { return tbdr_; }
    VulkanContext &context() { return ctx_; }
    VulkanMemory &memory() { return mem_; }
    size_t cachedRenderPasses() const { return passCache_.size(); }
    size_t cachedFramebuffers() const;

protected:
    explicit NativeVulkanRenderer(GpuVendor requiredVendor);

    /* ---- pontos de política do fabricante (padrão: regras ARM) ---- */
    virtual AttachmentOps chooseColorOps(const AttachmentUse &use, VkFormat format, VkSampleCountFlagBits samples,
                                         VkImageLayout current, bool sampled) const;
    virtual AttachmentOps chooseDepthStencilOps(const AttachmentUse &depth, const AttachmentUse &stencil,
                                                bool hasDepth, bool hasStencil, VkFormat format,
                                                VkSampleCountFlagBits samples, VkImageLayout current,
                                                bool sampled) const;
    virtual bool depthCanBeTransient() const { return false; }
    virtual bool splitPassOnMidClear() const { return true; }
    /* Validação do device escolhido; false aborta com o motivo. */
    virtual bool acceptDevice(const VulkanContext &ctx) const;
    /* Ajustes de quirks depois do create() do contexto (só com medição; padrão: nada). */
    virtual void tuneQuirks(DriverQuirks & /*q*/) const {}

    VulkanContext ctx_;
    VulkanMemory mem_;

private:
    struct FrameSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageReady = VK_NULL_HANDLE;
        uint64_t serial = 0;
    };
    struct RetiredFb {
        uint64_t serial;
        VkFramebuffer fb;
    };
    static constexpr uint32_t kFramesInFlight = 2; /* 3 aumenta latência sem ganho em TBDR */

    VkRenderPass getRenderPass(const RenderPassKey &key);
    VkFramebuffer getFramebuffer(const FramebufferKey &key);
    void destroyCaches();
    void destroyFrames();
    bool createFrames();
    void syncPresentSemaphores();
    void destroyPresentSemaphores();
    void collectFramebuffers(uint64_t completedSerial);
    void purgeBackbufferFramebuffers(); /* device ocioso: swapchain recriada ou perdida */
    bool recreateSwapchain();
    static void onImageRetired(uint64_t imageId, void *user);
    void beginPassInternal();

    GpuVendor requiredVendor_;
    bool hosted_ = false;
    bool anyGpu_ = false; /* RE5_VKN_ANY_GPU=1: aceita qualquer GPU (desenvolvimento no PC) */
    Create9Fn create9_ = nullptr;
    std::array<FrameSlot, kFramesInFlight> frames_{};
    std::vector<VkSemaphore> renderDone_; /* um por imagem da swapchain (reuso seguro no present) */
    uint32_t frameIndex_ = 0;
    uint32_t imageIndex_ = 0;
    std::atomic<uint64_t> nextSerial_{1}; /* serial do envio sendo gravado (ou o próximo) */
    uint64_t completedSerial_ = 0;
    bool inFrame_ = false;
    bool inPass_ = false;
    bool backbufferWritten_ = false;
    VkImageLayout backbufferLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    PassDesc curPass_{};
    VkRenderPass curRenderPass_ = VK_NULL_HANDLE;

    std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> passCache_;
    mutable std::mutex fbMutex_; /* imagens morrem em outras threads (Release do guest) */
    std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> fbCache_;
    std::vector<RetiredFb> fbRetired_;
    re5vk_tbdr tbdr_{}; /* oráculo existente (vk_tbdr.c), reaproveitado */
};

/* ------------------------------------------------------------ vkadreno ---- */

/*
 * Adreno (GMEM / FlexRender): passes picados por clears/cópias no meio custam resolves extras;
 * clear no meio do passe vira reabertura com loadOp=CLEAR, e STORE só com leitura provada.
 * Específico: profundidade carregada e só testada usa STORE_OP_NONE quando o device aceita
 * (VK_QCOM_render_pass_store_ops / load_store_op_none), evitando o resolve GMEM -> RAM.
 * Profundidade transitória fica desligada (padrão da base) até haver medição no Adreno.
 */
class AdrenoRenderer final : public NativeVulkanRenderer {
public:
    AdrenoRenderer() : NativeVulkanRenderer(GpuVendor::Qualcomm) {}
    re5_renderer_kind kind() const override { return RE5_RENDERER_VKADRENO; }

protected:
    AttachmentOps chooseDepthStencilOps(const AttachmentUse &depth, const AttachmentUse &stencil, bool hasDepth,
                                        bool hasStencil, VkFormat format, VkSampleCountFlagBits samples,
                                        VkImageLayout current, bool sampled) const override;
    bool acceptDevice(const VulkanContext &ctx) const override;
};

/* -------------------------------------------------------------- vkmali ---- */

/*
 * Mali: regras puras do ARM Vulkan-Samples - DONT_CARE/CLEAR em vez de LOAD, profundidade
 * transitória em LAZILY_ALLOCATED, nada de vkCmdClearAttachments quando um loadOp resolve,
 * pré-rotação na swapchain.
 */
class MaliRenderer final : public NativeVulkanRenderer {
public:
    MaliRenderer() : NativeVulkanRenderer(GpuVendor::Arm) {}
    re5_renderer_kind kind() const override { return RE5_RENDERER_VKMALI; }

protected:
    bool depthCanBeTransient() const override { return true; }
    bool acceptDevice(const VulkanContext &ctx) const override;
};

} // namespace re5::vkn

#endif /* RE5_VKN_IRENDERER_H */
