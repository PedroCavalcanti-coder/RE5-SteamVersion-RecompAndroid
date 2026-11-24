/*
 * VulkanContext.h - instância, device, filas e swapchain dos renderizadores nativos (vkadreno/vkmali).
 *
 * Toda a criação passa pelo vk-bootstrap (vkb::InstanceBuilder / PhysicalDeviceSelector /
 * DeviceBuilder / SwapchainBuilder). Nenhum vkCreateInstance/vkCreateDevice/vkCreateSwapchainKHR
 * direto fora de VulkanContext.cpp.
 *
 * Carregamento: o libvulkan não é ligado em tempo de compilação (VK_NO_PROTOTYPES). O
 * vkGetInstanceProcAddr vem de re5_vk_get_instance_proc_addr() (re5/vk_loader.h), que já resolve
 * o driver do sistema ou um driver customizado via AdrenoTools (Turnip). O mesmo ponteiro é
 * passado ao vk-bootstrap (InstanceBuilder(PFN_vkGetInstanceProcAddr)) e ao VMA
 * (VmaVulkanFunctions), para os três enxergarem o mesmo driver.
 *
 * Os tipos do vk-bootstrap ficam escondidos atrás de Impl: quem inclui este header não puxa
 * VkBootstrap.h.
 */
#ifndef RE5_VKN_VULKAN_CONTEXT_H
#define RE5_VKN_VULKAN_CONTEXT_H

#if defined(__ANDROID__)
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace re5::vkn {

/* Fabricante/driver, de VkPhysicalDeviceProperties::vendorID e VkPhysicalDeviceDriverProperties. */
enum class GpuVendor : uint8_t { Unknown, Qualcomm, Arm, Other };
enum class GpuDriver : uint8_t { Unknown, QualcommProprietary, MesaTurnip, ArmProprietary, MesaPanVk, Other };

/*
 * Quirks de driver. Cada flag é preenchida em VulkanContext::create() a partir do driverID e da
 * versão do driver, e só vira 'true' por padrão com medição registrada no STATUS.md; todas podem
 * ser forçadas por variável de ambiente (RE5_VKN_QUIRK_<NOME>=0/1) para bisseção.
 */
struct DriverQuirks {
    bool avoidMidPassClearAttachments = false; /* limpar só por loadOp=CLEAR (sem vkCmdClearAttachments) */
    bool brokenPipelineCacheData      = false; /* não persistir vkGetPipelineCacheData */
    bool brokenExtendedDynamicState   = false; /* compilar o estado na pipeline em vez de usar EDS */
    bool needsExplicitDepthResolve    = false; /* resolver profundidade fora do passe */
    bool preferRenderPassOverDynamic  = true;  /* VkRenderPass (D10, ver ARQUITETURA.md) */
};

/* O que o device ganhou (extensões/recursos opcionais que mudam caminhos do renderizador). */
struct DeviceCaps {
    uint32_t apiVersion = 0;
    bool lazilyAllocatedMemory = false; /* existe tipo LAZILY_ALLOCATED: anexos transitórios sem RAM */
    bool extendedDynamicState  = false; /* VK_EXT_extended_dynamic_state (ou 1.3) */
    bool externalMemoryHost    = false; /* VK_EXT_external_memory_host: buffers sobre a memória do guest */
    bool timelineSemaphore     = false;
    bool imagelessFramebuffer  = false; /* VK_KHR_imageless_framebuffer (ou 1.2): cache de FB por formato */
    bool samplerYcbcr          = false;
    bool storeOpNone           = false; /* VK_KHR/EXT_load_store_op_none ou VK_QCOM_render_pass_store_ops */
    bool extendedDynamicState2 = false; /* VK_EXT_extended_dynamic_state2 (ou 1.3) */
    bool dynamicRendering      = false; /* VK_KHR_dynamic_rendering (ou 1.3 core) */
    bool imageFormatList       = false; /* VkImageFormatListCreateInfo válido (1.2 core ou a extensão) */
    bool swapchainTransferDst  = false; /* a superfície aceita TRANSFER_DST nas imagens da swapchain */
    bool robustBufferAccess2   = false; /* VK_EXT_robustness2 com robustBufferAccess2 ligado */
    uint32_t instanceApiVersion = 0;    /* o que a instância pediu (VMA não pode passar disso) */
    VkPhysicalDeviceFeatures enabledFeatures{};
    VkDeviceSize nonCoherentAtomSize = 1;
    VkDeviceSize minImportedHostPointerAlignment = 0;
};

/* Fila e família. O renderizador usa uma fila gráfica que também apresenta (caso comum no Android). */
struct QueueRef {
    VkQueue  queue  = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
};

/*
 * Funções do device usadas pelos nativos, resolvidas por vkGetDeviceProcAddr do device criado
 * (nada global: o re5vk antigo já define vkXxx globais em C, e os dois convivem no binário).
 */
#define RE5_VKN_DEVICE_FNS(X)                                                                        \
    X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkAcquireNextImageKHR)                 \
    X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool)        \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence)       \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) X(vkCreateSemaphore)   \
    X(vkDestroySemaphore) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass)           \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCmdBeginRenderPass)    \
    X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage)       \
    X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCreateBuffer) X(vkDestroyBuffer)                \
    X(vkGetBufferMemoryRequirements)

struct DeviceFns {
#define RE5_VKN_FN_MEMBER(name) PFN_##name name = nullptr;
    RE5_VKN_DEVICE_FNS(RE5_VKN_FN_MEMBER)
#undef RE5_VKN_FN_MEMBER
    /* opcional: só com VK_EXT_external_memory_host */
    PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT = nullptr;
    /* opcional: só com dynamicRendering */
    PFN_vkCmdBeginRendering vkCmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering vkCmdEndRendering = nullptr;
};

/*
 * Superfície: o contexto não conhece SDL nem ANativeWindow; quem chama sabe criar a superfície.
 * createSurface == nullptr sobe um contexto sem janela (headless: sem swapchain; testes e
 * ferramentas).
 */
using SurfaceFactory = VkSurfaceKHR (*)(VkInstance instance, void *user);

/*
 * Como a swapchain é feita. O padrão segue as regras ARM/Qualcomm (pré-rotação); o device D3D9
 * pede identidade (ele não gira a projeção; o compositor gira, como na DXVK).
 */
struct SwapchainOptions {
    bool preRotate = true;
    VkPresentModeKHR presentModes[3] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR};
    uint32_t minImageCount = 0; /* 0 = mínimo da superfície + 1 */
};

struct ContextCreateInfo {
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr; /* de re5_vk_get_instance_proc_addr() */
    SurfaceFactory createSurface = nullptr;
    void *surfaceUser = nullptr;
    uint32_t width = 0, height = 0;           /* tamanho pedido pelo D3DPRESENT_PARAMETERS */
    bool validation = false;                  /* RE5_VKN_VALIDATION=1; só em build de depuração */
    uint32_t minApiVersion = VK_API_VERSION_1_1; /* Mali Bifrost antigos ainda são 1.1 */
    bool preferApi13 = false;                 /* instância 1.3 quando o carregador tiver */
    bool requireExtendedDynamicState = false; /* EDS1+EDS2 (1.3 ou extensões); sem eles, falha */
    bool wantImageFormatList = false;         /* liga VK_KHR_image_format_list em devices 1.1 */
    /* VK_EXT_robustness2 robustBufferAccess2 (precisa de optionalFeatures.robustBufferAccess): leitura
     * fora do buffer devolve 0 de verdade. Só o robustBufferAccess "v1" deixa o driver devolver
     * qualquer valor de dentro do buffer (Mali/Adreno prendem no limite; desktop devolve 0). */
    bool wantRobustness2 = false;
    VkPhysicalDeviceFeatures optionalFeatures{}; /* cada um ligado se o device tiver */
    SwapchainOptions swapchain;
    GpuVendor requiredVendor = GpuVendor::Unknown; /* vkadreno exige Qualcomm, vkmali exige Arm */
};

/*
 * Swapchain com pré-rotação (prática ARM/Qualcomm): currentTransform da superfície vira
 * preTransform; o renderizador gira a projeção em vez de o compositor girar a imagem.
 */
struct SwapchainState {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    uint64_t generation = 0; /* sobe a cada recriação: caches de framebuffer comparam */
};

/* Resultado de acquire/present: Recreate não é erro, é pedido de recreateSwapchain(). */
enum class SwapchainStatus : uint8_t { Ok, Suboptimal, Recreate };

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext(); /* vkDeviceWaitIdle + destruição na ordem inversa (via vkb::destroy_*) */

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;
    VulkanContext(VulkanContext &&) = delete;
    VulkanContext &operator=(VulkanContext &&) = delete;

    /* Sobe instância, superfície, device físico, device e swapchain. false: já logou o motivo. */
    bool create(const ContextCreateInfo &info);
    void destroy(); /* idempotente */

    /* Troca de janela (Android: surfaceDestroyed/surfaceCreated) sem derrubar o device. */
    void releaseSurface(); /* espera o device; derruba swapchain e superfície */
    bool recreateSurface(SurfaceFactory factory, void *user);

    /* Quirks ajustados pelo renderizador; as variáveis RE5_VKN_QUIRK_* continuam por cima. */
    void overrideQuirks(const DriverQuirks &q);
    bool recreateSwapchain(uint32_t width, uint32_t height);
    /* Troca as opções (ex.: Reset do D3D9 com outro PresentationInterval) e recria. */
    bool recreateSwapchain(uint32_t width, uint32_t height, const SwapchainOptions &opt);

    /* Envolvem vkAcquireNextImageKHR / vkQueuePresentKHR e traduzem OUT_OF_DATE/SUBOPTIMAL. */
    SwapchainStatus acquire(VkSemaphore imageReady, uint32_t *imageIndex, uint64_t timeoutNs = UINT64_MAX);
    SwapchainStatus present(VkSemaphore renderDone, uint32_t imageIndex);

    bool isCreated() const { return device_ != VK_NULL_HANDLE; }
    bool headless() const { return surface_ == VK_NULL_HANDLE; }
    const DeviceFns &fns() const { return fns_; }

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VkSurfaceKHR surface() const { return surface_; }
    const QueueRef &graphicsQueue() const { return graphics_; }
    const SwapchainState &swapchain() const { return swapchain_; }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr() const { return gipa_; }
    PFN_vkGetDeviceProcAddr getDeviceProcAddr() const { return gdpa_; }

    GpuVendor vendor() const { return vendor_; }
    GpuDriver driver() const { return driver_; }
    const DeviceCaps &caps() const { return caps_; }
    const DriverQuirks &quirks() const { return quirks_; }
    const VkPhysicalDeviceProperties &properties() const { return props_; }
    const VkPhysicalDeviceMemoryProperties &memoryProperties() const { return memProps_; }

    /* Formato de profundidade suportado como anexo ótimo (D24S8 nem sempre existe no Mali). */
    VkFormat pickDepthFormat(bool needStencil) const;

private:
    struct Impl; /* vkb::Instance, vkb::PhysicalDevice, vkb::Device, vkb::Swapchain */
    std::unique_ptr<Impl> impl_;

    bool createSwapchainLocked(uint32_t width, uint32_t height, VkSwapchainKHR old);
    void destroySwapchainViews();
    void detectVendorAndQuirks();

    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    PFN_vkGetDeviceProcAddr gdpa_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    QueueRef graphics_{};
    SwapchainState swapchain_{};
    DeviceFns fns_{};

    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
    GpuVendor vendor_ = GpuVendor::Unknown;
    GpuDriver driver_ = GpuDriver::Unknown;
    DeviceCaps caps_{};
    DriverQuirks quirks_{};
    ContextCreateInfo info_{};
};

} // namespace re5::vkn

#endif /* RE5_VKN_VULKAN_CONTEXT_H */
