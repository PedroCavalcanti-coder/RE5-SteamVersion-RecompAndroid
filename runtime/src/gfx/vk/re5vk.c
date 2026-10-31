/*
 * re5vk.c - backend Vulkan próprio atrás da ponte D3D9 (docs/PROJETO_VULKAN_NATIVO.md).
 *
 * RE5_GFX=re5vk liga; o padrão continua a DXVK (RE5_GFX=dxvk). O mesmo código serve ao
 * Windows x64 e ao Android ARM64: só a superfície (Win32 / ANativeWindow) e o nome do
 * carregador do Vulkan mudam.
 *
 * Etapa V2: o IDirect3D9 deste backend repassa as consultas do adaptador (caps, formatos,
 * modos) ao IDirect3D9 da DXVK carregada, então o jogo vê as mesmas caps e escolhe o mesmo
 * caminho de render que com a DXVK (A/B honesto). Só o CreateDevice é nosso: device,
 * swapchain FIFO, Clear e Present em Vulkan. Todo outro método do device aborta com o nome
 * (D8): o nome vem do thunk do guest em execução, porque a ponte chama o método do host
 * de dentro do handler do método de mesmo nome.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#else
#include <dlfcn.h>
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <d3d9.h>

#include "re5/log.h"
#include "re5/re5vk.h"
#include "re5/thunks.h"

/* ---- carregador do Vulkan ---- */

#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE_FNS(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR)                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateDevice)         \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkEnumerateDeviceExtensionProperties) VK_SURFACE_FN(X)
#if defined(_WIN32)
#define VK_SURFACE_FN(X) X(vkCreateWin32SurfaceKHR)
#define VK_SURFACE_EXT VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#elif defined(__ANDROID__)
#define VK_SURFACE_FN(X) X(vkCreateAndroidSurfaceKHR)
#define VK_SURFACE_EXT VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
#else
#define VK_SURFACE_FN(X)
#define VK_SURFACE_EXT NULL
#endif
#define VK_DEVICE_FNS(X)                                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)    \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit)                      \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)             \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)                  \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)                    \
    X(vkDestroySemaphore)

#define VK_DECL(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECL)
VK_INSTANCE_FNS(VK_DECL)
VK_DEVICE_FNS(VK_DECL)

static void vk_load_loader(void)
{
    if (vkGetInstanceProcAddr) return;
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) re5_fatal("re5vk", "sem vulkan-1.dll (erro %lu)", (unsigned long)GetLastError());
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
#if defined(__ANDROID__)
    const char *name = "libvulkan.so";
#else
    const char *name = "libvulkan.so.1";
#endif
    void *lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!lib) re5_fatal("re5vk", "dlopen(\"%s\") falhou: %s", name, dlerror());
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!vkGetInstanceProcAddr) re5_fatal("re5vk", "o carregador do Vulkan não exporta vkGetInstanceProcAddr");
#define VK_LOAD_GLOBAL(name)                                                                                       \
    if (!(name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name))) re5_fatal("re5vk", "sem %s", #name);
    VK_GLOBAL_FNS(VK_LOAD_GLOBAL)
}

static const char *vk_result(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    default: return "VkResult desconhecido";
    }
}

#define VK_CHECK(call)                                                                                             \
    do {                                                                                                           \
        VkResult vr_ = (call);                                                                                     \
        if (vr_ < 0) re5_fatal("re5vk", "%s -> %s (%d)", #call, vk_result(vr_), (int)vr_);                        \
    } while (0)

/* ---- método sem implementação (D8) ---- */

static HRESULT STDMETHODCALLTYPE nyi(void *self)
{
    (void)self;
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("re5vk", "%s sem implementação no backend re5vk (RE5_GFX=dxvk volta à DXVK)",
              t ? t->name : "método do IDirect3DDevice9 chamado pelo host");
    return E_FAIL;
}

static int guid_is(REFIID a, uint32_t d1)
{
    const unsigned char *p = (const unsigned char *)a;
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return v == d1;
}
#define IID_IUNKNOWN_D1          0x00000000u
#define IID_IDIRECT3D9_D1        0x81BDCBCAu
#define IID_IDIRECT3DDEVICE9_D1  0xD0223B96u

/* ---- IDirect3D9: repassa ao da DXVK, menos o CreateDevice ---- */

typedef struct {
    const struct IDirect3D9Vtbl *lpVtbl;
    IDirect3D9 *inner;   /* o IDirect3D9 da DXVK: caps, formatos, modos */
    long refs;
} vk_d3d9;

typedef struct vk_device vk_device;
static HRESULT vk_device_create(vk_d3d9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out);

#define INNER(self) (((vk_d3d9 *)(self))->inner)

static HRESULT STDMETHODCALLTYPE d_QueryInterface(IDirect3D9 *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (guid_is(riid, IID_IUNKNOWN_D1) || guid_is(riid, IID_IDIRECT3D9_D1)) {
        *out = self;
        ((vk_d3d9 *)self)->refs++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE d_AddRef(IDirect3D9 *self) { return (ULONG)++((vk_d3d9 *)self)->refs; }
static ULONG STDMETHODCALLTYPE d_Release(IDirect3D9 *self)
{
    vk_d3d9 *d = (vk_d3d9 *)self;
    long n = --d->refs;
    if (n == 0) {
        d->inner->lpVtbl->Release(d->inner);
        free(d);
    }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE d_RegisterSoftwareDevice(IDirect3D9 *self, void *fn)
{
    return INNER(self)->lpVtbl->RegisterSoftwareDevice(INNER(self), fn);
}
static UINT STDMETHODCALLTYPE d_GetAdapterCount(IDirect3D9 *self) { return INNER(self)->lpVtbl->GetAdapterCount(INNER(self)); }
static HRESULT STDMETHODCALLTYPE d_GetAdapterIdentifier(IDirect3D9 *self, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id)
{
    return INNER(self)->lpVtbl->GetAdapterIdentifier(INNER(self), a, f, id);
}
static UINT STDMETHODCALLTYPE d_GetAdapterModeCount(IDirect3D9 *self, UINT a, D3DFORMAT f)
{
    return INNER(self)->lpVtbl->GetAdapterModeCount(INNER(self), a, f);
}
static HRESULT STDMETHODCALLTYPE d_EnumAdapterModes(IDirect3D9 *self, UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE *dm)
{
    return INNER(self)->lpVtbl->EnumAdapterModes(INNER(self), a, f, m, dm);
}
static HRESULT STDMETHODCALLTYPE d_GetAdapterDisplayMode(IDirect3D9 *self, UINT a, D3DDISPLAYMODE *dm)
{
    return INNER(self)->lpVtbl->GetAdapterDisplayMode(INNER(self), a, dm);
}
static HRESULT STDMETHODCALLTYPE d_CheckDeviceType(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT df, D3DFORMAT bf,
                                                   BOOL w)
{
    return INNER(self)->lpVtbl->CheckDeviceType(INNER(self), a, t, df, bf, w);
}
static HRESULT STDMETHODCALLTYPE d_CheckDeviceFormat(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u,
                                                     D3DRESOURCETYPE rt, D3DFORMAT cf)
{
    return INNER(self)->lpVtbl->CheckDeviceFormat(INNER(self), a, t, af, u, rt, cf);
}
static HRESULT STDMETHODCALLTYPE d_CheckDeviceMultiSampleType(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT f,
                                                              BOOL w, D3DMULTISAMPLE_TYPE m, DWORD *q)
{
    return INNER(self)->lpVtbl->CheckDeviceMultiSampleType(INNER(self), a, t, f, w, m, q);
}
static HRESULT STDMETHODCALLTYPE d_CheckDepthStencilMatch(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT af,
                                                          D3DFORMAT rf, D3DFORMAT df)
{
    return INNER(self)->lpVtbl->CheckDepthStencilMatch(INNER(self), a, t, af, rf, df);
}
static HRESULT STDMETHODCALLTYPE d_CheckDeviceFormatConversion(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT s,
                                                               D3DFORMAT d)
{
    return INNER(self)->lpVtbl->CheckDeviceFormatConversion(INNER(self), a, t, s, d);
}
static HRESULT STDMETHODCALLTYPE d_GetDeviceCaps(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DCAPS9 *caps)
{
    return INNER(self)->lpVtbl->GetDeviceCaps(INNER(self), a, t, caps);
}
static HMONITOR STDMETHODCALLTYPE d_GetAdapterMonitor(IDirect3D9 *self, UINT a)
{
    return INNER(self)->lpVtbl->GetAdapterMonitor(INNER(self), a);
}
static HRESULT STDMETHODCALLTYPE d_CreateDevice(IDirect3D9 *self, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags,
                                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    return vk_device_create((vk_d3d9 *)self, a, t, focus, flags, pp, out);
}

static const struct IDirect3D9Vtbl k_d3d9_vtbl = {
    .QueryInterface = d_QueryInterface,
    .AddRef = d_AddRef,
    .Release = d_Release,
    .RegisterSoftwareDevice = d_RegisterSoftwareDevice,
    .GetAdapterCount = d_GetAdapterCount,
    .GetAdapterIdentifier = d_GetAdapterIdentifier,
    .GetAdapterModeCount = d_GetAdapterModeCount,
    .EnumAdapterModes = d_EnumAdapterModes,
    .GetAdapterDisplayMode = d_GetAdapterDisplayMode,
    .CheckDeviceType = d_CheckDeviceType,
    .CheckDeviceFormat = d_CheckDeviceFormat,
    .CheckDeviceMultiSampleType = d_CheckDeviceMultiSampleType,
    .CheckDepthStencilMatch = d_CheckDepthStencilMatch,
    .CheckDeviceFormatConversion = d_CheckDeviceFormatConversion,
    .GetDeviceCaps = d_GetDeviceCaps,
    .GetAdapterMonitor = d_GetAdapterMonitor,
    .CreateDevice = d_CreateDevice,
};

void *re5vk_wrap_d3d9(void *inner)
{
    if (!inner) return NULL;
    vk_d3d9 *d = calloc(1, sizeof *d);
    if (!d) re5_fatal("re5vk", "sem memória");
    d->lpVtbl = &k_d3d9_vtbl;
    d->inner = inner;
    d->refs = 1;
    RE5_INFO("re5vk", "backend Vulkan próprio ligado (RE5_GFX=re5vk): consultas do adaptador pela DXVK, "
                      "device em Vulkan");
    return d;
}

/* ---- IDirect3DDevice9 ---- */

enum { VK_FRAMES = 2, VK_MAX_IMAGES = 8 };

struct vk_device {
    const struct IDirect3DDevice9Vtbl *lpVtbl;
    long refs;
    vk_d3d9 *d3d;
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DPRESENT_PARAMETERS pp;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkSwapchainKHR swap;
    VkFormat format;
    VkExtent2D extent;
    uint32_t nimages;
    VkImage images[VK_MAX_IMAGES];
    VkSemaphore done[VK_MAX_IMAGES];   /* por imagem: a apresentação espera o desenho dela */
    VkCommandPool pool;
    VkCommandBuffer cmd[VK_FRAMES];
    VkFence fence[VK_FRAMES];
    VkSemaphore acquired[VK_FRAMES];
    uint32_t frame;
    float clear[4];   /* cor do último Clear(D3DCLEAR_TARGET); a V2 desenha só isso */
    uint64_t presents;
};

static struct IDirect3DDevice9Vtbl g_dev_vtbl;

static void swapchain_create(vk_device *d)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surface, &caps));
    uint32_t nf = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &nf, NULL));
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &nf, fmts));
    d->format = fmts[0].format;
    VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            d->format = fmts[i].format;
            cs = fmts[i].colorSpace;
            break;
        }
    }
    d->extent = caps.currentExtent;
    if (d->extent.width == 0xFFFFFFFFu) {
        d->extent.width = d->pp.BackBufferWidth ? d->pp.BackBufferWidth : 800;
        d->extent.height = d->pp.BackBufferHeight ? d->pp.BackBufferHeight : 600;
    }
    uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    /* Identidade, como a DXVK: no Android paisagem o compositor gira e o present diz SUBOPTIMAL. */
    VkSurfaceTransformFlagBitsKHR xf = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                           : caps.currentTransform;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = d->surface,
        .minImageCount = n,
        .imageFormat = d->format,
        .imageColorSpace = cs,
        .imageExtent = d->extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = xf,
        .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,   /* A4: sem quadros jogados fora; o único modo garantido */
        .clipped = VK_TRUE,
        .oldSwapchain = d->swap,
    };
    VkSwapchainKHR sw;
    VK_CHECK(vkCreateSwapchainKHR(d->dev, &ci, NULL, &sw));
    if (d->swap) vkDestroySwapchainKHR(d->dev, d->swap, NULL);
    d->swap = sw;
    d->nimages = VK_MAX_IMAGES;
    VK_CHECK(vkGetSwapchainImagesKHR(d->dev, d->swap, &d->nimages, d->images));
    for (uint32_t i = 0; i < d->nimages; i++) {
        if (d->done[i]) continue;
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(d->dev, &si, NULL, &d->done[i]));
    }
    RE5_INFO("re5vk", "swapchain %ux%u, formato %d, %u imagens, FIFO", d->extent.width, d->extent.height,
             (int)d->format, d->nimages);
}

static void device_destroy(vk_device *d)
{
    if (d->dev) {
        vkDeviceWaitIdle(d->dev);
        for (int i = 0; i < VK_FRAMES; i++) {
            if (d->fence[i]) vkDestroyFence(d->dev, d->fence[i], NULL);
            if (d->acquired[i]) vkDestroySemaphore(d->dev, d->acquired[i], NULL);
        }
        for (uint32_t i = 0; i < VK_MAX_IMAGES; i++)
            if (d->done[i]) vkDestroySemaphore(d->dev, d->done[i], NULL);
        if (d->pool) vkDestroyCommandPool(d->dev, d->pool, NULL);
        if (d->swap) vkDestroySwapchainKHR(d->dev, d->swap, NULL);
        vkDestroyDevice(d->dev, NULL);
    }
    if (d->surface) vkDestroySurfaceKHR(d->inst, d->surface, NULL);
    if (d->inst) vkDestroyInstance(d->inst, NULL);
    if (d->d3d) d->d3d->lpVtbl->Release((IDirect3D9 *)d->d3d);
    free(d);
}

static HRESULT STDMETHODCALLTYPE v_QueryInterface(IDirect3DDevice9 *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (guid_is(riid, IID_IUNKNOWN_D1) || guid_is(riid, IID_IDIRECT3DDEVICE9_D1)) {
        *out = self;
        ((vk_device *)self)->refs++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE v_AddRef(IDirect3DDevice9 *self) { return (ULONG)++((vk_device *)self)->refs; }
static ULONG STDMETHODCALLTYPE v_Release(IDirect3DDevice9 *self)
{
    vk_device *d = (vk_device *)self;
    long n = --d->refs;
    if (n == 0) device_destroy(d);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE v_TestCooperativeLevel(IDirect3DDevice9 *self) { return D3D_OK; }
static HRESULT STDMETHODCALLTYPE v_GetDirect3D(IDirect3DDevice9 *self, IDirect3D9 **out)
{
    vk_device *d = (vk_device *)self;
    if (!out) return D3DERR_INVALIDCALL;
    d->d3d->refs++;
    *out = (IDirect3D9 *)d->d3d;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetDeviceCaps(IDirect3DDevice9 *self, D3DCAPS9 *caps)
{
    vk_device *d = (vk_device *)self;
    return d->d3d->inner->lpVtbl->GetDeviceCaps(d->d3d->inner, d->cp.AdapterOrdinal, d->cp.DeviceType, caps);
}
static HRESULT STDMETHODCALLTYPE v_GetCreationParameters(IDirect3DDevice9 *self, D3DDEVICE_CREATION_PARAMETERS *cp)
{
    if (!cp) return D3DERR_INVALIDCALL;
    *cp = ((vk_device *)self)->cp;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_BeginScene(IDirect3DDevice9 *self) { return D3D_OK; }
static HRESULT STDMETHODCALLTYPE v_EndScene(IDirect3DDevice9 *self) { return D3D_OK; }
static HRESULT STDMETHODCALLTYPE v_Clear(IDirect3DDevice9 *self, DWORD count, const D3DRECT *rects, DWORD flags,
                                         D3DCOLOR color, float z, DWORD stencil)
{
    vk_device *d = (vk_device *)self;
    if (flags & D3DCLEAR_TARGET) {
        d->clear[0] = (float)((color >> 16) & 0xFF) / 255.0f;
        d->clear[1] = (float)((color >> 8) & 0xFF) / 255.0f;
        d->clear[2] = (float)(color & 0xFF) / 255.0f;
        d->clear[3] = (float)(color >> 24) / 255.0f;
    }
    return D3D_OK;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static HRESULT STDMETHODCALLTYPE v_Present(IDirect3DDevice9 *self, const RECT *src, const RECT *dst, HWND over,
                                           const RGNDATA *dirty)
{
    vk_device *d = (vk_device *)self;
    uint32_t f = d->frame;
    VK_CHECK(vkWaitForFences(d->dev, 1, &d->fence[f], VK_TRUE, UINT64_MAX));
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(d->dev, d->swap, UINT64_MAX, d->acquired[f], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(d->dev);
        swapchain_create(d);
        return D3D_OK;   /* o semáforo não foi sinalizado: este quadro não sai */
    }
    if (r < 0) re5_fatal("re5vk", "vkAcquireNextImageKHR -> %s", vk_result(r));
    VK_CHECK(vkResetFences(d->dev, 1, &d->fence[f]));
    VkCommandBuffer cb = d->cmd[f];
    VK_CHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    VkImage img = d->images[idx];
    barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue cc;
    memcpy(cc.float32, d->clear, sizeof cc.float32);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &d->acquired[f],
        .pWaitDstStageMask = &wait,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &d->done[idx],
    };
    VK_CHECK(vkQueueSubmit(d->queue, 1, &si, d->fence[f]));
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &d->done[idx],
        .swapchainCount = 1,
        .pSwapchains = &d->swap,
        .pImageIndices = &idx,
    };
    r = vkQueuePresentKHR(d->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(d->dev);
        swapchain_create(d);
    } else if (r < 0) {
        re5_fatal("re5vk", "vkQueuePresentKHR -> %s", vk_result(r));
    }
    d->frame = (f + 1) % VK_FRAMES;
    if (++d->presents == 1) RE5_INFO("re5vk", "primeiro Present");
    return D3D_OK;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static HRESULT vk_device_create(vk_d3d9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    if (!pp || !out) return D3DERR_INVALIDCALL;
    vk_load_loader();
    HWND win = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if (!win) re5_fatal("re5vk", "CreateDevice sem janela");
    if (!VK_SURFACE_EXT) re5_fatal("re5vk", "este host não tem superfície Vulkan no re5vk (só Windows e Android)");

    vk_device *d = calloc(1, sizeof *d);
    if (!d) re5_fatal("re5vk", "sem memória");
    if (!g_dev_vtbl.QueryInterface) {
        void **slot = (void **)&g_dev_vtbl;
        for (size_t i = 0; i < sizeof g_dev_vtbl / sizeof(void *); i++) slot[i] = (void *)nyi;
        g_dev_vtbl.QueryInterface = v_QueryInterface;
        g_dev_vtbl.AddRef = v_AddRef;
        g_dev_vtbl.Release = v_Release;
        g_dev_vtbl.TestCooperativeLevel = v_TestCooperativeLevel;
        g_dev_vtbl.GetDirect3D = v_GetDirect3D;
        g_dev_vtbl.GetDeviceCaps = v_GetDeviceCaps;
        g_dev_vtbl.GetCreationParameters = v_GetCreationParameters;
        g_dev_vtbl.BeginScene = v_BeginScene;
        g_dev_vtbl.EndScene = v_EndScene;
        g_dev_vtbl.Clear = v_Clear;
        g_dev_vtbl.Present = v_Present;
    }
    d->lpVtbl = &g_dev_vtbl;
    d->refs = 1;
    d->d3d = d3d;
    d3d->refs++;
    d->cp = (D3DDEVICE_CREATION_PARAMETERS){adapter, type, focus, flags};
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    d->pp = *pp;

    const char *iexts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_SURFACE_EXT};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "re5",
                             .pEngineName = "re5vk", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts};
    VK_CHECK(vkCreateInstance(&ici, NULL, &d->inst));
#define VK_LOAD_INSTANCE(name)                                                                                     \
    if (!(name = (PFN_##name)vkGetInstanceProcAddr(d->inst, #name))) re5_fatal("re5vk", "sem %s", #name);
    VK_INSTANCE_FNS(VK_LOAD_INSTANCE)

#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                       .hinstance = GetModuleHandleA(NULL), .hwnd = (HWND)win};
    VK_CHECK(vkCreateWin32SurfaceKHR(d->inst, &sci, NULL, &d->surface));
#elif defined(__ANDROID__)
    VkAndroidSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                         .window = (struct ANativeWindow *)win};
    VK_CHECK(vkCreateAndroidSurfaceKHR(d->inst, &sci, NULL, &d->surface));
#endif

    /* GPU: a primeira com uma fila de gráficos que apresenta nesta superfície. */
    uint32_t npd = 8;
    VkPhysicalDevice pds[8];
    VK_CHECK(vkEnumeratePhysicalDevices(d->inst, &npd, pds));
    for (uint32_t i = 0; i < npd && !d->phys; i++) {
        uint32_t nq = 16;
        VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, d->surface, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                d->phys = pds[i];
                d->qfam = q;
                break;
            }
        }
    }
    if (!d->phys) re5_fatal("re5vk", "nenhuma GPU com fila de gráficos que apresente na janela");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(d->phys, &props);
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(d->phys, NULL, &next, NULL);
    VkExtensionProperties *exts = calloc(next ? next : 1, sizeof *exts);
    if (exts) vkEnumerateDeviceExtensionProperties(d->phys, NULL, &next, exts);
    if (!exts || !has_ext(exts, next, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        re5_fatal("re5vk", "GPU '%s' sem VK_KHR_swapchain", props.deviceName);
    free(exts);
    RE5_INFO("re5vk", "GPU '%s' (Vulkan %u.%u.%u, driver 0x%X), fila %u", props.deviceName,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, d->qfam);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = d->qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts};
    VK_CHECK(vkCreateDevice(d->phys, &dci, NULL, &d->dev));
#define VK_LOAD_DEVICE(name)                                                                                       \
    if (!(name = (PFN_##name)vkGetDeviceProcAddr(d->dev, #name))) re5_fatal("re5vk", "sem %s", #name);
    VK_DEVICE_FNS(VK_LOAD_DEVICE)
    vkGetDeviceQueue(d->dev, d->qfam, 0, &d->queue);

    swapchain_create(d);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = d->qfam};
    VK_CHECK(vkCreateCommandPool(d->dev, &pci, NULL, &d->pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = d->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = VK_FRAMES};
    VK_CHECK(vkAllocateCommandBuffers(d->dev, &ai, d->cmd));
    for (int i = 0; i < VK_FRAMES; i++) {
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(d->dev, &fi, NULL, &d->fence[i]));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(d->dev, &si, NULL, &d->acquired[i]));
    }
    *out = (IDirect3DDevice9 *)d;
    return D3D_OK;
}
