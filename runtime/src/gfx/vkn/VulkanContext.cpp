/*
 * VulkanContext.cpp - instância, device e swapchain pelo vk-bootstrap (ver VulkanContext.h).
 */
#include "VulkanContext.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "VkBootstrap.h"

extern "C" {
#include "re5/log.h"
}

namespace re5::vkn {

namespace {

constexpr const char *kCh = "vkn";

constexpr uint32_t kVendorQualcomm = 0x5143;
constexpr uint32_t kVendorArm = 0x13B5;

GpuVendor vendorFromId(uint32_t id)
{
    if (id == kVendorQualcomm) return GpuVendor::Qualcomm;
    if (id == kVendorArm) return GpuVendor::Arm;
    return GpuVendor::Other;
}

const char *vendorName(GpuVendor v)
{
    switch (v) {
    case GpuVendor::Qualcomm: return "Qualcomm";
    case GpuVendor::Arm: return "Arm";
    case GpuVendor::Other: return "outro";
    default: return "desconhecido";
    }
}

const char *driverName(GpuDriver d)
{
    switch (d) {
    case GpuDriver::QualcommProprietary: return "Qualcomm proprietário";
    case GpuDriver::MesaTurnip: return "Mesa Turnip";
    case GpuDriver::ArmProprietary: return "Arm proprietário";
    case GpuDriver::MesaPanVk: return "Mesa PanVK";
    case GpuDriver::Other: return "outro";
    default: return "desconhecido";
    }
}

/* RE5_VKN_QUIRK_<NOME>=0/1 por cima do valor escolhido. */
void envBool(const char *name, bool &value)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return;
    value = v[0] == '1';
    RE5_INFO(kCh, "%s=%c (forçado pelo ambiente)", name, value ? '1' : '0');
}

void applyQuirkEnv(DriverQuirks &q)
{
    envBool("RE5_VKN_QUIRK_AVOID_MID_PASS_CLEAR", q.avoidMidPassClearAttachments);
    envBool("RE5_VKN_QUIRK_BROKEN_PIPELINE_CACHE", q.brokenPipelineCacheData);
    envBool("RE5_VKN_QUIRK_BROKEN_EDS", q.brokenExtendedDynamicState);
    envBool("RE5_VKN_QUIRK_EXPLICIT_DEPTH_RESOLVE", q.needsExplicitDepthResolve);
    envBool("RE5_VKN_QUIRK_RENDER_PASS", q.preferRenderPassOverDynamic);
}

std::string vkbError(const vkb::Error &e)
{
    std::string s = e.type.message();
    if (e.vk_result != VK_SUCCESS) s += " (VkResult " + std::to_string(static_cast<int>(e.vk_result)) + ")";
    for (const auto &r : e.detailed_failure_reasons) s += "; " + r;
    return s;
}

bool rotated(VkSurfaceTransformFlagBitsKHR t)
{
    return (t & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR |
                 VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR |
                 VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR)) != 0;
}

} // namespace

struct VulkanContext::Impl {
    vkb::Instance instance;
    vkb::PhysicalDevice physical;
    vkb::Device device;
    vkb::Swapchain swapchain;
    bool hasSwapchain = false;

    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getSurfaceCaps = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties getFormatProps = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getProps2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures getFeatures = nullptr;
};

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext()
{
    destroy();
}

bool VulkanContext::create(const ContextCreateInfo &info)
{
    destroy();
    info_ = info;
    gipa_ = info.getInstanceProcAddr;
    if (!gipa_) {
        RE5_ERROR(kCh, "VulkanContext::create sem vkGetInstanceProcAddr (re5_vk_get_instance_proc_addr devolveu NULL?)");
        return false;
    }
    impl_ = std::make_unique<Impl>();
    const bool headless = info.createSurface == nullptr;

    /* ---- instância ---- */
    uint32_t instApi = info.minApiVersion;
    if (info.preferApi13) {
        auto sys = vkb::SystemInfo::get_system_info(gipa_);
        if (sys && sys.value().is_instance_version_available(VK_API_VERSION_1_3)) instApi = VK_API_VERSION_1_3;
    }
    vkb::InstanceBuilder ib(gipa_);
    ib.set_app_name("re5").set_engine_name("re5-vkn").require_api_version(instApi).set_headless(headless);
    if (info.validation) ib.request_validation_layers(true).use_default_debug_messenger();
    auto ir = ib.build();
    if (!ir) {
        RE5_ERROR(kCh, "vkb::InstanceBuilder: %s", vkbError(ir.full_error()).c_str());
        impl_.reset();
        return false;
    }
    impl_->instance = ir.value();
    instance_ = impl_->instance.instance;

    impl_->getSurfaceCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        gipa_(instance_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    impl_->getSurfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        gipa_(instance_, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    impl_->getFormatProps = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        gipa_(instance_, "vkGetPhysicalDeviceFormatProperties"));
    impl_->getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gipa_(instance_, "vkGetPhysicalDeviceProperties2"));
    if (!impl_->getFormatProps || !impl_->getProps2) {
        RE5_ERROR(kCh, "instância Vulkan sem vkGetPhysicalDeviceFormatProperties/Properties2 (precisa de 1.1)");
        destroy();
        return false;
    }

    /* ---- superfície ---- */
    if (!headless) {
        surface_ = info.createSurface(instance_, info.surfaceUser);
        if (surface_ == VK_NULL_HANDLE) {
            RE5_ERROR(kCh, "a fábrica de superfície devolveu VK_NULL_HANDLE");
            destroy();
            return false;
        }
    }

    /* ---- device físico ---- */
    vkb::PhysicalDeviceSelector sel(impl_->instance);
    sel.set_minimum_version(info.minApiVersion)
        .allow_any_gpu_device_type(true)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::integrated);
    if (!headless) sel.set_surface(surface_);
    auto list = sel.select_devices();
    if (!list || list.value().empty()) {
        RE5_ERROR(kCh, "nenhuma GPU Vulkan serve: %s",
                  list ? "lista vazia" : vkbError(list.full_error()).c_str());
        destroy();
        return false;
    }
    vkb::PhysicalDevice *chosen = nullptr;
    std::string seen;
    for (auto &pd : list.value()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%s (vendor 0x%04X)", seen.empty() ? "" : ", ", pd.name.c_str(),
                      pd.properties.vendorID);
        seen += buf;
        if (!chosen && (info.requiredVendor == GpuVendor::Unknown ||
                        vendorFromId(pd.properties.vendorID) == info.requiredVendor))
            chosen = &pd;
    }
    if (!chosen) {
        RE5_ERROR(kCh, "este renderizador exige GPU %s e o aparelho tem: %s. Escolha outro renderizador no launcher.",
                  vendorName(info.requiredVendor), seen.c_str());
        destroy();
        return false;
    }
    impl_->physical = *chosen;
    vkb::PhysicalDevice &pd = impl_->physical;
    physical_ = pd.physical_device;

    /* opcionais: só ligados se existem */
    caps_ = DeviceCaps{};
    caps_.externalMemoryHost = pd.enable_extension_if_present(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    caps_.storeOpNone = pd.enable_extension_if_present("VK_KHR_load_store_op_none") ||
                        pd.enable_extension_if_present("VK_EXT_load_store_op_none") ||
                        pd.enable_extension_if_present("VK_QCOM_render_pass_store_ops");
    caps_.instanceApiVersion = instApi;
    const uint32_t effApi = std::min(instApi, pd.properties.apiVersion);
    /* Recursos do núcleo 1.0: cada um à parte (enable_features_if_present é tudo ou nada). */
    {
        const VkBool32 *want = &info.optionalFeatures.robustBufferAccess;
        const size_t n = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
        for (size_t i = 0; i < n; i++) {
            if (!want[i]) continue;
            VkPhysicalDeviceFeatures one{};
            (&one.robustBufferAccess)[i] = VK_TRUE;
            pd.enable_features_if_present(one);
        }
    }
    /* robustBufferAccess2: como a DXVK (que exige a extensão). Vértice ou constante fora do buffer lê 0. */
    if (info.wantRobustness2 && info.optionalFeatures.robustBufferAccess && pd.features.robustBufferAccess &&
        pd.is_extension_present("VK_EXT_robustness2")) {
        VkPhysicalDeviceRobustness2FeaturesEXT rb2{};
        rb2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        rb2.robustBufferAccess2 = VK_TRUE;
        caps_.robustBufferAccess2 = pd.enable_extension_features_if_present(rb2) &&
                                    pd.enable_extension_if_present("VK_EXT_robustness2");
    }
    if (effApi >= VK_API_VERSION_1_3) {
        caps_.extendedDynamicState = caps_.extendedDynamicState2 = true; /* núcleo, sem bit de recurso */
        VkPhysicalDeviceVulkan13Features v13{};
        v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        v13.dynamicRendering = VK_TRUE;
        if (pd.enable_extension_features_if_present(v13)) {
            caps_.dynamicRendering = true;
        }
    } else {
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT e1{};
        e1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
        e1.extendedDynamicState = VK_TRUE;
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT e2{};
        e2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
        e2.extendedDynamicState2 = VK_TRUE;
        if (info.requireExtendedDynamicState) {
            caps_.extendedDynamicState = pd.is_extension_present(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) &&
                                         pd.enable_extension_features_if_present(e1) &&
                                         pd.enable_extension_if_present(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
            caps_.extendedDynamicState2 = pd.is_extension_present(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) &&
                                          pd.enable_extension_features_if_present(e2) &&
                                          pd.enable_extension_if_present(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        }
    }
    if (!caps_.dynamicRendering && pd.is_extension_present(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
        VkPhysicalDeviceDynamicRenderingFeaturesKHR drf{};
        drf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
        drf.dynamicRendering = VK_TRUE;
        if (pd.enable_extension_features_if_present(drf) &&
            pd.enable_extension_if_present(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
            caps_.dynamicRendering = true;
        }
    }
    if (info.requireExtendedDynamicState && !caps_.extendedDynamicState) {
        RE5_ERROR(kCh, "GPU '%s' sem extendedDynamicState (o device D3D9 nativo precisa deste recurso)",
                  pd.properties.deviceName);
        destroy();
        return false;
    }
    caps_.imageFormatList = effApi >= VK_API_VERSION_1_2 ||
                            (info.wantImageFormatList && pd.enable_extension_if_present("VK_KHR_image_format_list"));
    caps_.timelineSemaphore = pd.is_extension_present("VK_KHR_timeline_semaphore");
    caps_.imagelessFramebuffer = pd.is_extension_present("VK_KHR_imageless_framebuffer");
    caps_.samplerYcbcr = pd.is_extension_present("VK_KHR_sampler_ycbcr_conversion");

    /* ---- device ---- */
    vkb::DeviceBuilder db(pd);
    auto dr = db.build();
    if (!dr) {
        RE5_ERROR(kCh, "vkb::DeviceBuilder: %s", vkbError(dr.full_error()).c_str());
        destroy();
        return false;
    }
    impl_->device = dr.value();
    device_ = impl_->device.device;
    gdpa_ = impl_->device.fp_vkGetDeviceProcAddr;

    auto q = impl_->device.get_queue(vkb::QueueType::graphics);
    auto qi = impl_->device.get_queue_index(vkb::QueueType::graphics);
    if (!q || !qi) {
        RE5_ERROR(kCh, "device sem fila gráfica");
        destroy();
        return false;
    }
    graphics_.queue = q.value();
    graphics_.family = qi.value();
    if (!headless) {
        auto pi = impl_->device.get_queue_index(vkb::QueueType::present);
        if (!pi || pi.value() != graphics_.family) {
            RE5_ERROR(kCh, "a fila gráfica (família %u) não apresenta nesta superfície; filas separadas não são suportadas",
                      graphics_.family);
            destroy();
            return false;
        }
    }

    /* ---- funções do device ---- */
#define RE5_VKN_LOAD(name)                                                                             \
    fns_.name = reinterpret_cast<PFN_##name>(gdpa_(device_, #name));
    RE5_VKN_DEVICE_FNS(RE5_VKN_LOAD)
#undef RE5_VKN_LOAD
#define RE5_VKN_CHECK(name)                                                                            \
    if (!fns_.name && !(headless && (!std::strcmp(#name, "vkAcquireNextImageKHR") ||                   \
                                     !std::strcmp(#name, "vkQueuePresentKHR")))) {                     \
        RE5_ERROR(kCh, "o driver não resolveu %s", #name);                                            \
        destroy();                                                                                     \
        return false;                                                                                  \
    }
    RE5_VKN_DEVICE_FNS(RE5_VKN_CHECK)
#undef RE5_VKN_CHECK
    if (caps_.externalMemoryHost)
        fns_.vkGetMemoryHostPointerPropertiesEXT = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            gdpa_(device_, "vkGetMemoryHostPointerPropertiesEXT"));
    if (caps_.dynamicRendering) {
        fns_.vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa_(device_, "vkCmdBeginRendering"));
        if (!fns_.vkCmdBeginRendering)
            fns_.vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa_(device_, "vkCmdBeginRenderingKHR"));
        fns_.vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(gdpa_(device_, "vkCmdEndRendering"));
        if (!fns_.vkCmdEndRendering)
            fns_.vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(gdpa_(device_, "vkCmdEndRenderingKHR"));
    }

    /* ---- propriedades ---- */
    props_ = pd.properties;
    memProps_ = pd.memory_properties;
    /* O que de fato ligou (pedido e presente). */
    impl_->getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa_(instance_, "vkGetPhysicalDeviceFeatures"));
    if (impl_->getFeatures) {
        VkPhysicalDeviceFeatures have{};
        impl_->getFeatures(physical_, &have);
        const VkBool32 *w = &info.optionalFeatures.robustBufferAccess, *h = &have.robustBufferAccess;
        VkBool32 *e = &caps_.enabledFeatures.robustBufferAccess;
        for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++) e[i] = w[i] && h[i];
    }
    caps_.apiVersion = props_.apiVersion;
    caps_.nonCoherentAtomSize = props_.limits.nonCoherentAtomSize ? props_.limits.nonCoherentAtomSize : 1;
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; i++)
        if (memProps_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            caps_.lazilyAllocatedMemory = true;

    detectVendorAndQuirks();

    RE5_INFO(kCh, "GPU: %s | %s / %s | Vulkan %u.%u.%u | lazy=%d hostptr=%d store_none=%d dyn_rend=%d eds2=%d "
                  "robust=%d robust2=%d%s",
             props_.deviceName, vendorName(vendor_), driverName(driver_), VK_API_VERSION_MAJOR(props_.apiVersion),
             VK_API_VERSION_MINOR(props_.apiVersion), VK_API_VERSION_PATCH(props_.apiVersion),
             caps_.lazilyAllocatedMemory, caps_.externalMemoryHost, caps_.storeOpNone, caps_.dynamicRendering,
             caps_.extendedDynamicState2, caps_.enabledFeatures.robustBufferAccess ? 1 : 0,
             caps_.robustBufferAccess2 ? 1 : 0, headless ? " | sem janela" : "");

    if (!headless && !createSwapchainLocked(info.width, info.height, VK_NULL_HANDLE)) {
        destroy();
        return false;
    }
    return true;
}

void VulkanContext::detectVendorAndQuirks()
{
    vendor_ = vendorFromId(props_.vendorID);
    driver_ = GpuDriver::Unknown;

    const bool hasDriverProps = impl_->physical.is_extension_present("VK_KHR_driver_properties") ||
                                props_.apiVersion >= VK_API_VERSION_1_2;
    VkPhysicalDeviceDriverProperties drv{};
    drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{};
    host.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    void **next = &p2.pNext;
    if (hasDriverProps) {
        *next = &drv;
        next = &drv.pNext;
    }
    if (caps_.externalMemoryHost) *next = &host;
    impl_->getProps2(physical_, &p2);

    if (hasDriverProps) {
        switch (drv.driverID) {
        case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: driver_ = GpuDriver::QualcommProprietary; break;
        case VK_DRIVER_ID_MESA_TURNIP: driver_ = GpuDriver::MesaTurnip; break;
        case VK_DRIVER_ID_ARM_PROPRIETARY: driver_ = GpuDriver::ArmProprietary; break;
        case VK_DRIVER_ID_MESA_PANVK: driver_ = GpuDriver::MesaPanVk; break;
        default: driver_ = GpuDriver::Other; break;
        }
        RE5_INFO(kCh, "driver: %s %s", drv.driverName, drv.driverInfo);
    }
    if (caps_.externalMemoryHost) caps_.minImportedHostPointerAlignment = host.minImportedHostPointerAlignment;

    /* Nenhum quirk ligado por padrão: só com medição registrada no STATUS.md. */
    quirks_ = DriverQuirks{};
    applyQuirkEnv(quirks_);
}

void VulkanContext::overrideQuirks(const DriverQuirks &q)
{
    quirks_ = q;
    applyQuirkEnv(quirks_);
}

bool VulkanContext::createSwapchainLocked(uint32_t width, uint32_t height, VkSwapchainKHR old)
{
    VkSurfaceCapabilitiesKHR sc{};
    if (!impl_->getSurfaceCaps || impl_->getSurfaceCaps(physical_, surface_, &sc) != VK_SUCCESS) {
        RE5_ERROR(kCh, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR falhou");
        return false;
    }
    if (width == 0 || height == 0) {
        width = sc.currentExtent.width;
        height = sc.currentExtent.height;
    }
    if (width == 0 || height == 0 || width == UINT32_MAX) {
        RE5_WARN(kCh, "superfície sem tamanho (janela minimizada?); swapchain adiada");
        return false;
    }

    const SwapchainOptions &opt = info_.swapchain;
    VkSurfaceTransformFlagBitsKHR pre = sc.currentTransform;
    if (opt.preRotate) {
        /* Pré-rotação: o compositor não gira; a projeção do renderizador gira (preTransform). */
        if (rotated(pre)) std::swap(width, height);
    } else if (sc.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        /* Identidade, como a DXVK: no Android paisagem o compositor gira (o present diz SUBOPTIMAL). */
        pre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    /* TRANSFER_DST só se a superfície aceitar: pedir bit sem suporte é inválido (o Adreno falha
     * com VK_ERROR_INITIALIZATION_FAILED). */
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    caps_.swapchainTransferDst = (sc.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    if (caps_.swapchainTransferDst) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(sc.supportedCompositeAlpha & alpha)) {
        if (sc.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        else if (sc.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else
            alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }

    /* O jogo escreve em espaço gama (D3D9 sem SRGBWRITE no backbuffer): formatos UNORM. */
    vkb::SwapchainBuilder sb(impl_->device, surface_);
    sb.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .add_fallback_format({VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(opt.presentModes[0])
        .add_fallback_present_mode(opt.presentModes[1])
        .add_fallback_present_mode(opt.presentModes[2])
        .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .set_image_usage_flags(usage)
        .set_pre_transform_flags(pre)
        .set_composite_alpha_flags(alpha)
        .set_old_swapchain(old);
    if (opt.minImageCount) {
        uint32_t n = std::max(opt.minImageCount, sc.minImageCount);
        if (sc.maxImageCount) n = std::min(n, sc.maxImageCount);
        sb.set_desired_min_image_count(n);
    }
    auto r = sb.build();
    if (!r) {
        RE5_ERROR(kCh, "vkb::SwapchainBuilder: %s", vkbError(r.full_error()).c_str());
        return false;
    }

    /* A antiga sai depois que a nova existe (oldSwapchain); o device já está ocioso aqui. */
    if (impl_->hasSwapchain) {
        destroySwapchainViews();
        vkb::destroy_swapchain(impl_->swapchain);
        impl_->hasSwapchain = false;
    }
    impl_->swapchain = r.value();
    impl_->hasSwapchain = true;

    auto imgs = impl_->swapchain.get_images();
    auto views = impl_->swapchain.get_image_views();
    if (!imgs || !views) {
        RE5_ERROR(kCh, "não consegui as imagens/views da swapchain");
        vkb::destroy_swapchain(impl_->swapchain);
        impl_->hasSwapchain = false;
        swapchain_ = SwapchainState{};
        return false;
    }
    const uint64_t gen = swapchain_.generation + 1;
    swapchain_.handle = impl_->swapchain.swapchain;
    swapchain_.format = impl_->swapchain.image_format;
    swapchain_.colorSpace = impl_->swapchain.color_space;
    swapchain_.extent = impl_->swapchain.extent;
    swapchain_.preTransform = pre;
    swapchain_.presentMode = impl_->swapchain.present_mode;
    swapchain_.images = imgs.value();
    swapchain_.views = views.value();
    swapchain_.generation = gen;
    RE5_INFO(kCh, "swapchain %ux%u, %llu imagens, formato %d, present %d, transform 0x%X (da tela 0x%X) (geração %llu)",
             swapchain_.extent.width, swapchain_.extent.height, (unsigned long long)swapchain_.images.size(),
             (int)swapchain_.format,
             (int)swapchain_.presentMode, (unsigned)pre, (unsigned)sc.currentTransform, (unsigned long long)gen);
    return true;
}

void VulkanContext::destroySwapchainViews()
{
    if (!swapchain_.views.empty() && impl_ && impl_->hasSwapchain)
        impl_->swapchain.destroy_image_views(swapchain_.views);
    swapchain_.views.clear();
    swapchain_.images.clear();
}

bool VulkanContext::recreateSwapchain(uint32_t width, uint32_t height, const SwapchainOptions &opt)
{
    info_.swapchain = opt;
    return recreateSwapchain(width, height);
}

bool VulkanContext::recreateSwapchain(uint32_t width, uint32_t height)
{
    if (!device_ || surface_ == VK_NULL_HANDLE) return false;
    fns_.vkDeviceWaitIdle(device_);
    info_.width = width;
    info_.height = height;
    return createSwapchainLocked(width, height, impl_->hasSwapchain ? impl_->swapchain.swapchain : VK_NULL_HANDLE);
}

void VulkanContext::releaseSurface()
{
    if (!impl_) return;
    if (device_) fns_.vkDeviceWaitIdle(device_);
    if (impl_->hasSwapchain) {
        destroySwapchainViews();
        vkb::destroy_swapchain(impl_->swapchain);
        impl_->hasSwapchain = false;
    }
    const uint64_t gen = swapchain_.generation;
    swapchain_ = SwapchainState{};
    swapchain_.generation = gen;
    if (surface_ != VK_NULL_HANDLE) {
        vkb::destroy_surface(impl_->instance, surface_);
        surface_ = VK_NULL_HANDLE;
    }
}

bool VulkanContext::recreateSurface(SurfaceFactory factory, void *user)
{
    if (!device_ || !factory) return false;
    releaseSurface();
    info_.createSurface = factory;
    info_.surfaceUser = user;
    surface_ = factory(instance_, user);
    if (surface_ == VK_NULL_HANDLE) {
        RE5_ERROR(kCh, "a fábrica de superfície devolveu VK_NULL_HANDLE na troca de janela");
        return false;
    }
    VkBool32 ok = VK_FALSE;
    if (!impl_->getSurfaceSupport ||
        impl_->getSurfaceSupport(physical_, graphics_.family, surface_, &ok) != VK_SUCCESS || !ok) {
        RE5_ERROR(kCh, "a nova superfície não aceita apresentação pela fila %u", graphics_.family);
        return false;
    }
    return createSwapchainLocked(info_.width, info_.height, VK_NULL_HANDLE);
}

SwapchainStatus VulkanContext::acquire(VkSemaphore imageReady, uint32_t *imageIndex, uint64_t timeoutNs)
{
    if (swapchain_.handle == VK_NULL_HANDLE) return SwapchainStatus::Recreate;
    VkResult r = fns_.vkAcquireNextImageKHR(device_, swapchain_.handle, timeoutNs, imageReady, VK_NULL_HANDLE,
                                            imageIndex);
    if (r == VK_SUCCESS) return SwapchainStatus::Ok;
    if (r == VK_SUBOPTIMAL_KHR) return SwapchainStatus::Suboptimal; /* imagem adquirida mesmo assim */
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) return SwapchainStatus::Recreate;
    re5_fatal(kCh, "vkAcquireNextImageKHR -> %d", (int)r);
}

SwapchainStatus VulkanContext::present(VkSemaphore renderDone, uint32_t imageIndex)
{
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = renderDone != VK_NULL_HANDLE ? 1u : 0u;
    pi.pWaitSemaphores = &renderDone;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_.handle;
    pi.pImageIndices = &imageIndex;
    VkResult r = fns_.vkQueuePresentKHR(graphics_.queue, &pi);
    if (r == VK_SUCCESS) return SwapchainStatus::Ok;
    if (r == VK_SUBOPTIMAL_KHR) return SwapchainStatus::Suboptimal;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) return SwapchainStatus::Recreate;
    re5_fatal(kCh, "vkQueuePresentKHR -> %d", (int)r);
}

VkFormat VulkanContext::pickDepthFormat(bool needStencil) const
{
    static const VkFormat withStencil[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    static const VkFormat depthOnly[] = {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT,
                                         VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    const VkFormat *list = needStencil ? withStencil : depthOnly;
    const size_t n = needStencil ? 2 : 4;
    for (size_t i = 0; i < n; i++) {
        VkFormatProperties fp{};
        impl_->getFormatProps(physical_, list[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return list[i];
    }
    re5_fatal(kCh, "nenhum formato de profundidade%s serve de anexo nesta GPU", needStencil ? "+estêncil" : "");
}

void VulkanContext::destroy()
{
    if (!impl_) return;
    if (device_ && fns_.vkDeviceWaitIdle) fns_.vkDeviceWaitIdle(device_);
    if (impl_->hasSwapchain) {
        destroySwapchainViews();
        vkb::destroy_swapchain(impl_->swapchain);
        impl_->hasSwapchain = false;
    }
    if (device_) vkb::destroy_device(impl_->device);
    if (surface_ != VK_NULL_HANDLE) vkb::destroy_surface(impl_->instance, surface_);
    if (instance_) vkb::destroy_instance(impl_->instance);
    impl_.reset();
    instance_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    graphics_ = QueueRef{};
    swapchain_ = SwapchainState{};
    fns_ = DeviceFns{};
    gdpa_ = nullptr;
}

} // namespace re5::vkn
