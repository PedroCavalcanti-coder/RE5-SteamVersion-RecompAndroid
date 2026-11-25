/*
 * vk2_ctx.c - contexto Vulkan 1.1 do backend vk2 (ver vk2_ctx.h). C11, sem VMA nem vk-bootstrap.
 *
 * Memória: blocos grandes por tipo de memória, sub-alocados por uma lista livre ordenada (primeiro
 * que serve, junta vizinhos ao liberar). Toda sub-alocação é alinhada também à bufferImageGranularity
 * (conservador: buffers e imagens podem dividir um bloco sem checar o vizinho). Pedido grande (mais
 * da metade do bloco) ganha bloco próprio. Um mutex protege tudo (o jogo cria recursos de várias
 * threads; a destruição vem do lixo do envio, com o mutex do device).
 */
#include <stdio.h>
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

#include "re5/log.h"
#include "re5/vk_loader.h"
#include "../../core/host_sync.h"
#include "vk2_ctx.h"
#include "vk2_freelist.h"

#define CH "vk2"

/* ---- funções (só as do contexto; o device D3D9 carrega as dele) ---- */

#define CTX_INSTANCE_FNS(X)                                                                                        \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                             \
    X(vkGetPhysicalDeviceFeatures) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)                       \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)                          \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                           \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define CTX_DEVICE_FNS(X)                                                                                          \
    X(vkGetDeviceQueue) X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkAllocateMemory) X(vkFreeMemory)                \
    X(vkMapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)    \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                      \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass)                       \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)               \
    X(vkGetSwapchainImagesKHR)

typedef struct blk {
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint8_t *map;
    uint32_t type;
    int dedicated;
    vk2_fl fl;
    struct blk *next;
} blk;

typedef struct { uint64_t h; VkRenderPass rp; vk2_pass key; } rp_entry;
typedef struct { VkRenderPass rp; VkImageView views[5]; uint32_t n; VkExtent2D ext; VkFramebuffer fb; } fb_entry;

static struct {
    int open;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkSurfaceKHR surface;
    uint32_t qfam;
    VkQueue queue;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    VkSwapchainKHR swap;
    uint32_t nviews;
    VkImageView views[VK2_MAX_SWAP_IMAGES];
    re5_mutex mu;
    blk *blocks[VK_MAX_MEMORY_TYPES];
    uint64_t reserved, used;
    rp_entry *rps;
    uint32_t nrp, caprp;
    fb_entry *fbs;
    uint32_t nfb, capfb;
    re5_mutex fbmu;
#define X(n) PFN_##n n;
    CTX_INSTANCE_FNS(X)
    CTX_DEVICE_FNS(X)
#undef X
} g;

#define F(n) (g.n)

static const char *vkres(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult desconhecido";
    }
}

#define OK(call)                                                                                                   \
    do {                                                                                                           \
        VkResult r_ = (call);                                                                                      \
        if (r_ < 0) re5_fatal(CH, "%s -> %s (%d)", #call, vkres(r_), (int)r_);                                     \
    } while (0)

/* ---- instância e device ---- */

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static VkExtensionProperties *dev_exts(VkPhysicalDevice pd, uint32_t *n)
{
    *n = 0;
    F(vkEnumerateDeviceExtensionProperties)(pd, NULL, n, NULL);
    VkExtensionProperties *e = calloc(*n ? *n : 1, sizeof *e);
    if (e) F(vkEnumerateDeviceExtensionProperties)(pd, NULL, n, e);
    return e;
}

static const char *type_name(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discreta";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrada";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "outra";
    }
}

/* Família gráfica que também apresenta na superfície; -1 se não há. */
static int gfx_family(VkPhysicalDevice pd, VkSurfaceKHR surf)
{
    uint32_t nq = 0;
    F(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nq, NULL);
    VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
    if (!q) return -1;
    F(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nq, q);
    int pick = -1;
    for (uint32_t i = 0; i < nq && pick < 0; i++) {
        VkBool32 sup = VK_FALSE;
        if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            F(vkGetPhysicalDeviceSurfaceSupportKHR)(pd, i, surf, &sup) == VK_SUCCESS && sup)
            pick = (int)i;
    }
    free(q);
    return pick;
}

static int create_instance(void)
{
    PFN_vkEnumerateInstanceVersion enum_ver =
        (PFN_vkEnumerateInstanceVersion)g.gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t iver = VK_API_VERSION_1_0;
    if (enum_ver) enum_ver(&iver);
    if (iver < VK_API_VERSION_1_1) {
        RE5_WARN(CH, "o carregador só tem Vulkan %u.%u: o vk2 precisa do 1.1", VK_API_VERSION_MAJOR(iver),
                 VK_API_VERSION_MINOR(iver));
        return -1;
    }
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)g.gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)g.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!enum_ext || !create) {
        RE5_WARN(CH, "o carregador não exporta vkCreateInstance");
        return -1;
    }
    const char *plat =
#if defined(_WIN32)
        "VK_KHR_win32_surface";
#elif defined(__ANDROID__)
        "VK_KHR_android_surface";
#else
        NULL;
#endif
    if (!plat) {
        RE5_WARN(CH, "este host não tem superfície Vulkan no vk2 (só Windows e Android)");
        return -1;
    }
    uint32_t n = 0;
    enum_ext(NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    if (!e) return -1;
    enum_ext(NULL, &n, e);
    int ok = has_ext(e, n, "VK_KHR_surface") && has_ext(e, n, plat);
    free(e);
    if (!ok) {
        RE5_WARN(CH, "a instância não oferece VK_KHR_surface + %s", plat);
        return -1;
    }
    const char *names[2] = {"VK_KHR_surface", plat};
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "re5",
                            .applicationVersion = 1, .pEngineName = "re5-vk2", .engineVersion = 1,
                            .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
                               .enabledExtensionCount = 2, .ppEnabledExtensionNames = names};
    VkResult r = create(&ci, NULL, &g.inst);
    if (r != VK_SUCCESS) {
        RE5_WARN(CH, "vkCreateInstance -> %s", vkres(r));
        return -1;
    }
#define X(nm)                                                                                                      \
    if (!(F(nm) = (PFN_##nm)g.gipa(g.inst, #nm))) { RE5_WARN(CH, "sem " #nm); return -1; }
    CTX_INSTANCE_FNS(X)
#undef X
    return 0;
}

/* Pontuação de uma GPU para escolha automática (maior vence); -1 = não serve. */
static int score_gpu(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *p)
{
    if (p->apiVersion < VK_API_VERSION_1_1) return -1;
    if (gfx_family(pd, g.surface) < 0) return -1;
    uint32_t n;
    VkExtensionProperties *e = dev_exts(pd, &n);
    int sw = e && has_ext(e, n, "VK_KHR_swapchain");
    free(e);
    if (!sw) return -1;
    switch (p->deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    default: return 1;
    }
}

static int pick_gpu(void)
{
    uint32_t n = 0;
    F(vkEnumeratePhysicalDevices)(g.inst, &n, NULL);
    VkPhysicalDevice *pd = calloc(n ? n : 1, sizeof *pd);
    if (!pd || !n) { free(pd); RE5_WARN(CH, "nenhuma GPU Vulkan"); return -1; }
    F(vkEnumeratePhysicalDevices)(g.inst, &n, pd);
    const char *want = getenv("RE5VK2_GPU");
    int forced = want && *want ? atoi(want) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        F(vkGetPhysicalDeviceProperties)(pd[i], &p);
        int s = score_gpu(pd[i], &p);
        RE5_INFO(CH, "GPU %u: '%s' (%s, Vulkan %u.%u.%u)%s", i, p.deviceName, type_name(p.deviceType),
                 VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                 VK_API_VERSION_PATCH(p.apiVersion), s < 0 ? " - não serve ao vk2" : "");
        if (forced >= 0) {
            if ((int)i == forced && s >= 0) best = (int)i;
        } else if (s > best_score) {
            best_score = s;
            best = (int)i;
        }
    }
    if (best >= 0) g.phys = pd[best];
    free(pd);
    if (best < 0) RE5_WARN(CH, "nenhuma GPU serve ao vk2 (Vulkan 1.1 + VK_KHR_swapchain + fila gráfica que apresente)");
    return best < 0 ? -1 : 0;
}

static int create_device(vk2_device *out)
{
    int fam = gfx_family(g.phys, g.surface);
    g.qfam = (uint32_t)fam;
    F(vkGetPhysicalDeviceProperties)(g.phys, &g.props);
    F(vkGetPhysicalDeviceMemoryProperties)(g.phys, &g.mem);
    VkPhysicalDeviceFeatures have, want;
    F(vkGetPhysicalDeviceFeatures)(g.phys, &have);
    memset(&want, 0, sizeof want);
    want.robustBufferAccess = have.robustBufferAccess;     /* leitura fora do buffer não derruba o driver */
    want.independentBlend = have.independentBlend;
    want.depthBiasClamp = have.depthBiasClamp;
    want.fillModeNonSolid = have.fillModeNonSolid;
    want.samplerAnisotropy = have.samplerAnisotropy;
    want.textureCompressionBC = have.textureCompressionBC;
    want.textureCompressionETC2 = have.textureCompressionETC2;
    want.textureCompressionASTC_LDR = have.textureCompressionASTC_LDR;
    want.occlusionQueryPrecise = have.occlusionQueryPrecise;   /* D3DQUERYTYPE_OCCLUSION devolve contagem de pixels */
    uint32_t ne;
    VkExtensionProperties *ex = dev_exts(g.phys, &ne);
    const char *names[3];
    uint32_t nn = 0;
    names[nn++] = "VK_KHR_swapchain";
    int fmt_list = g.props.apiVersion >= VK_API_VERSION_1_2;
    if (!fmt_list && ex && has_ext(ex, ne, "VK_KHR_image_format_list")) {
        names[nn++] = "VK_KHR_image_format_list";
        fmt_list = 1;
    }
    free(ex);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = nn, .ppEnabledExtensionNames = names,
                             .pEnabledFeatures = &want};
    VkResult r = F(vkCreateDevice)(g.phys, &di, NULL, &g.dev);
    if (r != VK_SUCCESS) {
        RE5_WARN(CH, "vkCreateDevice -> %s", vkres(r));
        return -1;
    }
#define X(nm)                                                                                                      \
    if (!(F(nm) = (PFN_##nm)F(vkGetDeviceProcAddr)(g.dev, #nm))) { RE5_WARN(CH, "sem " #nm); return -1; }
    CTX_DEVICE_FNS(X)
#undef X
    F(vkGetDeviceQueue)(g.dev, g.qfam, 0, &g.queue);
    out->instance = g.inst;
    out->physical = g.phys;
    out->device = g.dev;
    out->surface = g.surface;
    out->queue = g.queue;
    out->queue_family = g.qfam;
    out->gipa = g.gipa;
    out->gdpa = F(vkGetDeviceProcAddr);
    out->api_version = g.props.apiVersion < VK_API_VERSION_1_1 ? g.props.apiVersion : VK_API_VERSION_1_1;
    out->features = want;
    out->image_format_list = fmt_list;
    return 0;
}

/* ---- swapchain ---- */

static void swap_views_destroy(void)
{
    for (uint32_t i = 0; i < g.nviews; i++) {
        vk2_forget_view(g.views[i]);
        F(vkDestroyImageView)(g.dev, g.views[i], NULL);
    }
    g.nviews = 0;
}

static int swapchain_build(const vk2_swap_req *rq, vk2_swapchain *sc)
{
    VkSurfaceCapabilitiesKHR caps;
    if (F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(g.phys, g.surface, &caps) != VK_SUCCESS) return -1;
    uint32_t nf = 0, nm = 0;
    F(vkGetPhysicalDeviceSurfaceFormatsKHR)(g.phys, g.surface, &nf, NULL);
    F(vkGetPhysicalDeviceSurfacePresentModesKHR)(g.phys, g.surface, &nm, NULL);
    VkSurfaceFormatKHR *fm = calloc(nf ? nf : 1, sizeof *fm);
    VkPresentModeKHR *pm = calloc(nm ? nm : 1, sizeof *pm);
    if (!fm || !pm || !nf || !nm) { free(fm); free(pm); return -1; }
    F(vkGetPhysicalDeviceSurfaceFormatsKHR)(g.phys, g.surface, &nf, fm);
    F(vkGetPhysicalDeviceSurfacePresentModesKHR)(g.phys, g.surface, &nm, pm);

    /* O jogo escreve em espaço gama (D3D9 sem SRGBWRITE no back buffer): UNORM. */
    VkSurfaceFormatKHR sel = fm[0];
    int found = 0;
    static const VkFormat pref[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (int k = 0; k < 2 && !found; k++)
        for (uint32_t i = 0; i < nf; i++)
            if (fm[i].format == pref[k] && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                sel = fm[i];
                found = 1;
                break;
            }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (int k = 0; k < 3; k++) {
        int ok = 0;
        for (uint32_t i = 0; i < nm; i++) ok |= pm[i] == rq->modes[k];
        if (ok) { mode = rq->modes[k]; break; }
    }
    free(fm);
    free(pm);

    uint32_t count = rq->min_images ? rq->min_images : caps.minImageCount + 1;
    if (count < caps.minImageCount) count = caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > VK2_MAX_SWAP_IMAGES) count = VK2_MAX_SWAP_IMAGES;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = rq->width ? rq->width : caps.minImageExtent.width;
        ext.height = rq->height ? rq->height : caps.minImageExtent.height;
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) return -1;   /* minimizada */

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int tdst = !!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (tdst) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* Transformação identidade, como a DXVK: o compositor gira (o jogo não gira a projeção). */
    VkSurfaceTransformFlagBitsKHR pre = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g.surface, .minImageCount = count,
        .imageFormat = sel.format, .imageColorSpace = sel.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = pre,
        .compositeAlpha = alpha, .presentMode = mode, .clipped = VK_TRUE, .oldSwapchain = g.swap,
    };
    VkSwapchainKHR ns = VK_NULL_HANDLE;
    VkResult r = F(vkCreateSwapchainKHR)(g.dev, &ci, NULL, &ns);
    if (g.swap) F(vkDestroySwapchainKHR)(g.dev, g.swap, NULL);
    g.swap = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) {
        RE5_WARN(CH, "vkCreateSwapchainKHR -> %s", vkres(r));
        return -1;
    }
    g.swap = ns;
    uint32_t ni = 0;
    F(vkGetSwapchainImagesKHR)(g.dev, ns, &ni, NULL);
    if (ni > VK2_MAX_SWAP_IMAGES) re5_fatal(CH, "swapchain com %u imagens (máx. %d)", ni, VK2_MAX_SWAP_IMAGES);
    memset(sc, 0, sizeof *sc);
    F(vkGetSwapchainImagesKHR)(g.dev, ns, &ni, sc->images);
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = sc->images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sel.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        OK(F(vkCreateImageView)(g.dev, &vi, NULL, &sc->views[i]));
        g.views[i] = sc->views[i];
    }
    g.nviews = ni;
    sc->swapchain = ns;
    sc->format = sel.format;
    sc->extent = ext;
    sc->mode = mode;
    sc->count = ni;
    sc->transfer_dst = tdst;
    RE5_INFO(CH, "swapchain %ux%u, %u imagens, formato %d, %s, transformação 0x%X (da tela 0x%X)", ext.width,
             ext.height, ni, (int)sel.format,
             mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO",
             (unsigned)pre, (unsigned)caps.currentTransform);
    return 0;
}

int vk2_open(vk2_surface_fn create_surface, void *surface_user, const vk2_swap_req *swap, vk2_device *dev,
             vk2_swapchain *sc)
{
    if (g.open) re5_fatal(CH, "vk2_open chamado duas vezes");
    memset(&g, 0, sizeof g);
    re5_mutex_init(&g.mu);
    re5_mutex_init(&g.fbmu);
    g.gipa = (PFN_vkGetInstanceProcAddr)re5_vk_get_instance_proc_addr();
    if (!g.gipa) {
        RE5_WARN(CH, "sem libvulkan");
        return -1;
    }
    if (create_instance() != 0) return -1;
    g.surface = create_surface(g.inst, surface_user);
    if (!g.surface) {
        RE5_WARN(CH, "a superfície Vulkan não foi criada");
        return -1;
    }
    if (pick_gpu() != 0 || create_device(dev) != 0) return -1;
    if (swapchain_build(swap, sc) != 0) {
        RE5_WARN(CH, "swapchain não criada");
        return -1;
    }
    g.open = 1;
    RE5_INFO(CH, "GPU escolhida: '%s', Vulkan %u.%u.%u, driver 0x%X, fila %u, bufferImageGranularity %llu",
             g.props.deviceName, VK_API_VERSION_MAJOR(g.props.apiVersion), VK_API_VERSION_MINOR(g.props.apiVersion),
             VK_API_VERSION_PATCH(g.props.apiVersion), g.props.driverVersion, g.qfam,
             (unsigned long long)g.props.limits.bufferImageGranularity);
    return 0;
}

int vk2_swapchain_recreate(const vk2_swap_req *swap, vk2_swapchain *sc)
{
    swap_views_destroy();
    return swapchain_build(swap, sc);
}

int vk2_surface_recreate(vk2_surface_fn create_surface, void *surface_user, const vk2_swap_req *swap,
                         vk2_swapchain *sc, VkSurfaceKHR *surface)
{
    swap_views_destroy();
    if (g.swap) {
        F(vkDestroySwapchainKHR)(g.dev, g.swap, NULL);
        g.swap = VK_NULL_HANDLE;
    }
    VkSurfaceKHR ns = create_surface(g.inst, surface_user);
    if (!ns) return -1;
    if (g.surface) F(vkDestroySurfaceKHR)(g.inst, g.surface, NULL);
    g.surface = ns;
    if (surface) *surface = ns;
    return swapchain_build(swap, sc);
}

/* ---- memória ---- */

#define BLOCK_DEVICE ((VkDeviceSize)64 << 20)
#define BLOCK_HOST ((VkDeviceSize)32 << 20)

#define align_up vk2_fl_align

/* Tipos de memória que servem ao pedido, do melhor ao pior (escrito em order[], devolve quantos). */
static uint32_t type_order(uint32_t bits, int host, uint32_t *order)
{
    uint32_t n = 0;
    int score[VK_MAX_MEMORY_TYPES];
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++) {
        if (!(bits >> i & 1)) continue;
        VkMemoryPropertyFlags f = g.mem.memoryTypes[i].propertyFlags;
        int s;
        if (host) {
            if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                continue;
            /* escrita sequencial da CPU: sem cache é melhor. Memória de SISTEMA antes da DEVICE_LOCAL: numa placa discreta a
             * DEVICE_LOCAL visível é a janela BAR da VRAM, onde o memcpy da CPU passa pelo PCIe (Unlock de 5 MB levava ~17 ms
             * no PC). No celular a memória é unificada: todos os tipos são DEVICE_LOCAL e nada muda. */
            s = ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 0 : 2) + ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 0 : 1);
        } else {
            if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) continue;
            s = ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0) + ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 0 : 2);
        }
        uint32_t j = n++;
        while (j && score[j - 1] < s) { score[j] = score[j - 1]; order[j] = order[j - 1]; j--; }
        score[j] = s;
        order[j] = i;
    }
    return n;
}

static blk *blk_new(uint32_t type, VkDeviceSize size, int dedicated)
{
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size,
                               .memoryTypeIndex = type};
    VkDeviceMemory m;
    if (F(vkAllocateMemory)(g.dev, &ai, NULL, &m) != VK_SUCCESS) return NULL;
    blk *b = calloc(1, sizeof *b);
    if (!b) { F(vkFreeMemory)(g.dev, m, NULL); return NULL; }
    b->mem = m;
    b->size = size;
    b->type = type;
    b->dedicated = dedicated;
    if (g.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void *p = NULL;
        if (F(vkMapMemory)(g.dev, m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            F(vkFreeMemory)(g.dev, m, NULL);
            free(b);
            return NULL;
        }
        b->map = p;
    }
    if (!dedicated && vk2_fl_init(&b->fl, size) != 0) re5_fatal(CH, "sem memória");
    b->next = g.blocks[type];
    g.blocks[type] = b;
    g.reserved += size;
    return b;
}

/* Aloca e preenche a (alloc, map, sub). 0 = ok. */
static int mem_alloc(const VkMemoryRequirements *rq, int host, vk2_alloc *a)
{
    uint32_t order[VK_MAX_MEMORY_TYPES];
    uint32_t nt = type_order(rq->memoryTypeBits, host, order);
    uint64_t gran = g.props.limits.bufferImageGranularity;
    uint64_t align = rq->alignment > gran ? rq->alignment : gran, size = align_up(rq->size, gran);
    VkDeviceSize bs = host ? BLOCK_HOST : BLOCK_DEVICE;
    re5_mutex_lock(&g.mu);
    for (uint32_t k = 0; k < nt; k++) {
        uint32_t t = order[k];
        blk *b = NULL;
        uint64_t off = 0;
        if (size <= bs / 2) {
            for (b = g.blocks[t]; b; b = b->next)
                if (!b->dedicated && vk2_fl_take(&b->fl, size, align, &off)) break;
            if (!b) {
                b = blk_new(t, bs, 0);
                if (b && !vk2_fl_take(&b->fl, size, align, &off)) b = NULL;
            }
        } else {
            b = blk_new(t, size, 1);
            off = 0;
        }
        if (!b) continue;
        a->alloc = b;
        a->map = b->map ? b->map + off : NULL;
        a->block = NULL;
        a->sub = off;
        g.used += size;
        re5_mutex_unlock(&g.mu);
        return 0;
    }
    re5_mutex_unlock(&g.mu);
    return -1;
}

static void mem_free(const VkMemoryRequirements *rq, vk2_alloc *a)
{
    blk *b = a->alloc;
    if (!b) return;
    uint64_t gran = g.props.limits.bufferImageGranularity, size = align_up(rq->size, gran);
    re5_mutex_lock(&g.mu);
    g.used -= size;
    if (b->dedicated) {
        blk **pp = &g.blocks[b->type];
        while (*pp && *pp != b) pp = &(*pp)->next;
        if (*pp) *pp = b->next;
        F(vkFreeMemory)(g.dev, b->mem, NULL);   /* o unmap é implícito */
        g.reserved -= b->size;
        free(b);
    } else {
        vk2_fl_give(&b->fl, a->sub, size);
        /* Bloco inteiro livre e há outro do mesmo tipo: devolve a memória ao sistema (o pico do
         * carregamento não fica reservado a vida toda). Um bloco de reserva por tipo sempre fica. */
        if (b->fl.n == 1 && b->fl.s[0].size == b->size) {
            int others = 0;
            for (blk *o = g.blocks[b->type]; o; o = o->next) others += o != b && !o->dedicated;
            if (others) {
                blk **pp = &g.blocks[b->type];
                while (*pp && *pp != b) pp = &(*pp)->next;
                if (*pp) *pp = b->next;
                vk2_fl_destroy(&b->fl);
                F(vkFreeMemory)(g.dev, b->mem, NULL);
                g.reserved -= b->size;
                free(b);
            }
        }
    }
    re5_mutex_unlock(&g.mu);
    memset(a, 0, sizeof *a);
}

VkBuffer vk2_mem_buffer_create(const VkBufferCreateInfo *ci, int mem, const char *name, vk2_alloc *out)
{
    memset(out, 0, sizeof *out);
    VkBuffer b;
    OK(F(vkCreateBuffer)(g.dev, ci, NULL, &b));
    VkMemoryRequirements rq;
    F(vkGetBufferMemoryRequirements)(g.dev, b, &rq);
    if (mem_alloc(&rq, mem == VK2_MEM_HOST, out) != 0)
        re5_fatal(CH, "sem memória de GPU para o buffer '%s' (%llu bytes, %s)", name ? name : "?",
                  (unsigned long long)rq.size, mem == VK2_MEM_HOST ? "host" : "device");
    OK(F(vkBindBufferMemory)(g.dev, b, ((blk *)out->alloc)->mem, out->sub));
    if (mem == VK2_MEM_HOST && !out->map) re5_fatal(CH, "buffer '%s' host-visible sem mapeamento", name ? name : "?");
    return b;
}

VkImage vk2_mem_image_create(const VkImageCreateInfo *ci, const char *name, vk2_alloc *out)
{
    memset(out, 0, sizeof *out);
    VkImage im;
    OK(F(vkCreateImage)(g.dev, ci, NULL, &im));
    VkMemoryRequirements rq;
    F(vkGetImageMemoryRequirements)(g.dev, im, &rq);
    if (mem_alloc(&rq, 0, out) != 0)
        re5_fatal(CH, "sem memória de GPU para a imagem '%s' (%ux%u, %llu bytes)", name ? name : "?", ci->extent.width,
                  ci->extent.height, (unsigned long long)rq.size);
    OK(F(vkBindImageMemory)(g.dev, im, ((blk *)out->alloc)->mem, out->sub));
    return im;
}

void vk2_mem_buffer_destroy(VkBuffer b, vk2_alloc *a)
{
    if (!b) return;
    VkMemoryRequirements rq;
    F(vkGetBufferMemoryRequirements)(g.dev, b, &rq);
    F(vkDestroyBuffer)(g.dev, b, NULL);
    mem_free(&rq, a);
}

void vk2_mem_image_destroy(VkImage img, vk2_alloc *a)
{
    if (!img) return;
    VkMemoryRequirements rq;
    F(vkGetImageMemoryRequirements)(g.dev, img, &rq);
    F(vkDestroyImage)(g.dev, img, NULL);
    mem_free(&rq, a);
}

void vk2_mem_stats(uint64_t *reserved, uint64_t *used)
{
    re5_mutex_lock(&g.mu);
    if (reserved) *reserved = g.reserved;
    if (used) *used = g.used;
    re5_mutex_unlock(&g.mu);
}

/* ---- passes e framebuffers ---- */

/* Chave normalizada: posições vazias zeradas, para memcmp. */
static void pass_norm(const vk2_pass *p, vk2_pass *k)
{
    memset(k, 0, sizeof *k);
    if (p->ncolor > 4) re5_fatal(CH, "passe com %u cores", p->ncolor);
    k->ncolor = p->ncolor;
    for (uint32_t i = 0; i < p->ncolor; i++)
        if (p->color[i].format != VK_FORMAT_UNDEFINED) {
            k->color[i] = p->color[i];
            k->color[i].stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k->color[i].stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
    k->has_ds = p->has_ds != 0;
    if (k->has_ds) k->ds = p->ds;
}

static uint64_t pass_hash(const vk2_pass *k)
{
    const uint8_t *b = (const uint8_t *)k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h | 1;
}

VkRenderPass vk2_render_pass(const vk2_pass *p)
{
    vk2_pass key;
    pass_norm(p, &key);
    uint64_t h = pass_hash(&key);
    re5_mutex_lock(&g.fbmu);
    for (uint32_t i = 0; i < g.nrp; i++)
        if (g.rps[i].h == h && !memcmp(&g.rps[i].key, &key, sizeof key)) {
            VkRenderPass rp = g.rps[i].rp;
            re5_mutex_unlock(&g.fbmu);
            return rp;
        }
    VkAttachmentDescription att[5];
    VkAttachmentReference cref[4], dref = {0};
    memset(att, 0, sizeof att);
    uint32_t n = 0;
    for (uint32_t i = 0; i < key.ncolor; i++) {
        const vk2_att *o = &key.color[i];
        if (o->format == VK_FORMAT_UNDEFINED) {
            cref[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
            continue;
        }
        att[n] = (VkAttachmentDescription){.format = o->format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = o->load,
                                           .storeOp = o->store, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                           .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           .initialLayout = o->layout, .finalLayout = o->layout};
        cref[i] = (VkAttachmentReference){n, o->layout};
        n++;
    }
    if (key.has_ds) {
        const vk2_att *o = &key.ds;
        att[n] = (VkAttachmentDescription){.format = o->format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = o->load,
                                           .storeOp = o->store, .stencilLoadOp = o->stencil_load,
                                           .stencilStoreOp = o->stencil_store, .initialLayout = o->layout,
                                           .finalLayout = o->layout};
        dref = (VkAttachmentReference){n, o->layout};
        n++;
    }
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = key.ncolor, .pColorAttachments = cref,
                                .pDepthStencilAttachment = key.has_ds ? &dref : NULL};
    /* Dependências externas estreitas: o device já emite as barreiras de imagem (layouts); estas só
     * ordenam o passe contra o anterior e o seguinte. */
    const VkPipelineStageFlags att_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags att_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* Na Mali (duas filas internas: vértice/compute e fragmento) uma dependência fragmento -> vértice entre passes
     * obriga o vértice/binning do passe seguinte a esperar o fragmento do anterior (bolha de pipeline; guia da Arm).
     * A leitura de textura no vertex shader (animação facial) já tem a barreira de imagem própria do device
     * (vk2_img_transition com VERTEX_SHADER como destino). RE5VK2_RP_VS_DEP=1 devolve o VERTEX_SHADER aqui. */
    static int vs_dep_on = -1;
    if (vs_dep_on < 0) {
        const char *e = getenv("RE5VK2_RP_VS_DEP");
        vs_dep_on = e && *e == '1';
        RE5_INFO(CH, "dependência de saída do passe com VERTEX_SHADER como destino: %s (RE5VK2_RP_VS_DEP=1 liga)", vs_dep_on ? "sim" : "não");
    }
    const VkPipelineStageFlags vs_dep = vs_dep_on ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : 0;
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0,
         att_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
         att_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
         att_access | VK_ACCESS_SHADER_READ_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, att_stages,
         att_stages | vs_dep | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         att_access | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0},
    };
    /* RE5VK2_RP_DEPS=0: sem dependências explícitas externo<->passe. Elas ordenam a saída de cor de passes CONSECUTIVOS mesmo
     * quando os alvos são independentes, e a Mali (fragmento em fila própria) perde a sobreposição entre eles. As barreiras de
     * imagem do device (vk2_img_transition, por sub-recurso, com "escrito" pendente) cobrem os hazards reais. Padrão 1 (como era). */
    static int rp_deps = -1;
    if (rp_deps < 0) {
        const char *e = getenv("RE5VK2_RP_DEPS");
        rp_deps = !(e && *e == '0');
        RE5_INFO(CH, "dependências explícitas do passe: %s (RE5VK2_RP_DEPS=0 tira)", rp_deps ? "sim" : "não");
    }
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n,
                                 .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub,
                                 .dependencyCount = rp_deps ? 2u : 0u, .pDependencies = dep};
    VkRenderPass rp;
    OK(F(vkCreateRenderPass)(g.dev, &ci, NULL, &rp));
    if (g.nrp == g.caprp) {
        g.caprp = g.caprp ? g.caprp * 2 : 32;
        g.rps = realloc(g.rps, g.caprp * sizeof *g.rps);
        if (!g.rps) re5_fatal(CH, "sem memória");
    }
    g.rps[g.nrp++] = (rp_entry){h, rp, key};
    re5_mutex_unlock(&g.fbmu);
    return rp;
}

VkRenderPass vk2_compat_pass(uint32_t ncolor, const VkFormat color[4], VkFormat ds, int stencil)
{
    vk2_pass p;
    memset(&p, 0, sizeof p);
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
    return vk2_render_pass(&p);
}

VkFramebuffer vk2_framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, VkExtent2D extent)
{
    if (n > 5) re5_fatal(CH, "framebuffer com %u anexos", n);
    re5_mutex_lock(&g.fbmu);
    for (uint32_t i = 0; i < g.nfb; i++) {
        fb_entry *e = &g.fbs[i];
        if (e->rp == rp && e->n == n && e->ext.width == extent.width && e->ext.height == extent.height &&
            !memcmp(e->views, views, n * sizeof views[0])) {
            VkFramebuffer fb = e->fb;
            re5_mutex_unlock(&g.fbmu);
            return fb;
        }
    }
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                  .attachmentCount = n, .pAttachments = views, .width = extent.width,
                                  .height = extent.height, .layers = 1};
    VkFramebuffer fb;
    OK(F(vkCreateFramebuffer)(g.dev, &ci, NULL, &fb));
    if (g.nfb == g.capfb) {
        g.capfb = g.capfb ? g.capfb * 2 : 256;
        g.fbs = realloc(g.fbs, g.capfb * sizeof *g.fbs);
        if (!g.fbs) re5_fatal(CH, "sem memória");
    }
    fb_entry *e = &g.fbs[g.nfb++];
    memset(e, 0, sizeof *e);
    e->rp = rp;
    e->n = n;
    e->ext = extent;
    e->fb = fb;
    memcpy(e->views, views, n * sizeof views[0]);
    re5_mutex_unlock(&g.fbmu);
    return fb;
}

void vk2_forget_view(VkImageView view)
{
    if (!view) return;
    re5_mutex_lock(&g.fbmu);
    for (uint32_t i = 0; i < g.nfb;) {
        fb_entry *e = &g.fbs[i];
        int uses = 0;
        for (uint32_t k = 0; k < e->n; k++) uses |= e->views[k] == view;
        if (uses) {
            F(vkDestroyFramebuffer)(g.dev, e->fb, NULL);   /* o chamador garante que a GPU acabou */
            g.fbs[i] = g.fbs[--g.nfb];
        } else {
            i++;
        }
    }
    re5_mutex_unlock(&g.fbmu);
}

void vk2_pass_cache_stats(uint32_t *passes, uint32_t *framebuffers)
{
    re5_mutex_lock(&g.fbmu);
    if (passes) *passes = g.nrp;
    if (framebuffers) *framebuffers = g.nfb;
    re5_mutex_unlock(&g.fbmu);
}
