/*
 * re5vk.c - backend Vulkan próprio atrás da ponte D3D9 (docs/PROJETO_VULKAN_NATIVO.md).
 *
 * RE5_GFX=re5vk liga; o padrão continua a DXVK (RE5_GFX=dxvk). O mesmo código serve ao
 * Windows x64 e ao Android ARM64: só a superfície (Win32 / ANativeWindow) e o carregador mudam.
 *
 * O IDirect3D9 deste backend repassa as consultas do adaptador (caps, formatos, modos) ao
 * IDirect3D9 da DXVK carregada, então o jogo vê as mesmas caps e escolhe o mesmo caminho de
 * render que com a DXVK (A/B honesto). O device é nosso. Este arquivo: carregador, IDirect3D9,
 * device, memória, envios e Present; ver vk_core.h para o resto.
 *
 * Método sem implementação aborta com o nome (D8): o nome vem do thunk do guest em execução,
 * porque a ponte chama o método do host de dentro do handler do método de mesmo nome.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "vk_core.h"
#include "re5/re5vk.h"
#include "re5/thunks.h"
#include "re5/guest_base.h"
#include "re5/guest_memory.h"

/* ---- carregador ---- */

#define VK_DEFINE(name) PFN_##name name;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DEFINE)
VK_INSTANCE_FNS(VK_DEFINE)
VK_DEVICE_FNS(VK_DEFINE)

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

const char *re5vk_result(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    default: return "VkResult desconhecido";
    }
}

/* ---- método sem implementação (D8) ---- */

HRESULT STDMETHODCALLTYPE re5vk_nyi(void *self)
{
    (void)self;
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("re5vk", "%s sem implementação no backend re5vk (RE5_GFX=dxvk volta à DXVK)",
              t ? t->name : "método COM chamado pelo host");
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

void re5vk_lock(vk_device *d) { re5_mutex_lock(&d->mu); }
void re5vk_unlock(vk_device *d) { re5_mutex_unlock(&d->mu); }

/* ---- IDirect3D9: repassa ao da DXVK, menos o CreateDevice ---- */

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
    return re5vk_device_create((vk_d3d9 *)self, a, t, focus, flags, pp, out);
}

static const struct IDirect3D9Vtbl k_d3d9_vtbl = {
    .QueryInterface = d_QueryInterface, .AddRef = d_AddRef, .Release = d_Release,
    .RegisterSoftwareDevice = d_RegisterSoftwareDevice, .GetAdapterCount = d_GetAdapterCount,
    .GetAdapterIdentifier = d_GetAdapterIdentifier, .GetAdapterModeCount = d_GetAdapterModeCount,
    .EnumAdapterModes = d_EnumAdapterModes, .GetAdapterDisplayMode = d_GetAdapterDisplayMode,
    .CheckDeviceType = d_CheckDeviceType, .CheckDeviceFormat = d_CheckDeviceFormat,
    .CheckDeviceMultiSampleType = d_CheckDeviceMultiSampleType, .CheckDepthStencilMatch = d_CheckDepthStencilMatch,
    .CheckDeviceFormatConversion = d_CheckDeviceFormatConversion, .GetDeviceCaps = d_GetDeviceCaps,
    .GetAdapterMonitor = d_GetAdapterMonitor, .CreateDevice = d_CreateDevice,
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

/* ---- memória: blocos de 64 MiB por tipo, lista de livres; dedicada acima de 16 MiB ---- */

#define CHUNK_SIZE ((VkDeviceSize)64 << 20)
#define DEDICATED_MIN ((VkDeviceSize)16 << 20)

static int mem_type(vk_device *d, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < d->memprops.memoryTypeCount; i++)
        if ((bits >> i & 1) && (d->memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void chunk_free_insert(vk_chunk *c, VkDeviceSize off, VkDeviceSize size)
{
    int i = 0;
    while (i < c->nfree && c->free[i][0] < off) i++;
    /* junta com a vizinha de trás e a da frente */
    if (i > 0 && c->free[i - 1][0] + c->free[i - 1][1] == off) {
        c->free[i - 1][1] += size;
        if (i < c->nfree && off + size == c->free[i][0]) {
            c->free[i - 1][1] += c->free[i][1];
            memmove(&c->free[i], &c->free[i + 1], (size_t)(c->nfree - i - 1) * sizeof c->free[0]);
            c->nfree--;
        }
        return;
    }
    if (i < c->nfree && off + size == c->free[i][0]) {
        c->free[i][0] = off;
        c->free[i][1] += size;
        return;
    }
    if (c->nfree == c->capfree) {
        c->capfree = c->capfree ? c->capfree * 2 : 16;
        c->free = realloc(c->free, (size_t)c->capfree * sizeof c->free[0]);
        if (!c->free) re5_fatal("re5vk", "sem memória");
    }
    memmove(&c->free[i + 1], &c->free[i], (size_t)(c->nfree - i) * sizeof c->free[0]);
    c->free[i][0] = off;
    c->free[i][1] = size;
    c->nfree++;
}

static int chunk_take(vk_chunk *c, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *out)
{
    for (int i = 0; i < c->nfree; i++) {
        VkDeviceSize start = (c->free[i][0] + align - 1) / align * align;
        VkDeviceSize end = c->free[i][0] + c->free[i][1];
        if (start + size > end) continue;
        VkDeviceSize off0 = c->free[i][0];
        /* tira [start, start+size) da faixa i */
        if (start + size < end) {
            c->free[i][0] = start + size;
            c->free[i][1] = end - (start + size);
        } else {
            memmove(&c->free[i], &c->free[i + 1], (size_t)(c->nfree - i - 1) * sizeof c->free[0]);
            c->nfree--;
        }
        if (start > off0) chunk_free_insert(c, off0, start - off0);
        *out = start;
        return 0;
    }
    return -1;
}

vk_alloc re5vk_mem_alloc(vk_device *d, VkMemoryRequirements req, VkMemoryPropertyFlags want, int host)
{
    int t = mem_type(d, req.memoryTypeBits, want);
    if (t < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        t = mem_type(d, req.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) re5_fatal("re5vk", "nenhum tipo de memória 0x%X com as propriedades 0x%X", req.memoryTypeBits, want);
    vk_alloc a = {0};
    a.type = (uint32_t)t;
    a.size = req.size;
    if (req.size >= DEDICATED_MIN) {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = (uint32_t)t};
        VK_CHECK(vkAllocateMemory(d->dev, &ai, NULL, &a.mem));
        if (host) VK_CHECK(vkMapMemory(d->dev, a.mem, 0, VK_WHOLE_SIZE, 0, (void **)&a.map));
        a.chunk = -1;
        return a;
    }
    /* host = buffers (lineares); imagens em blocos separados (bufferImageGranularity) */
    uint32_t key = (uint32_t)t | (host ? 0x100u : 0u);
    for (int i = 0; i < d->nchunks; i++) {
        vk_chunk *c = &d->chunks[i];
        if (c->type != key) continue;
        VkDeviceSize off;
        if (chunk_take(c, req.size, req.alignment ? req.alignment : 1, &off) == 0) {
            a.mem = c->mem;
            a.off = off;
            a.map = c->map ? c->map + off : NULL;
            a.chunk = i;
            return a;
        }
    }
    if (d->nchunks >= VK_MAX_CHUNKS) re5_fatal("re5vk", "blocos de memória esgotados (%d)", VK_MAX_CHUNKS);
    vk_chunk *c = &d->chunks[d->nchunks];
    memset(c, 0, sizeof *c);
    c->size = CHUNK_SIZE;
    c->type = key;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = c->size,
                               .memoryTypeIndex = (uint32_t)t};
    VK_CHECK(vkAllocateMemory(d->dev, &ai, NULL, &c->mem));
    if (host) VK_CHECK(vkMapMemory(d->dev, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->map));
    chunk_free_insert(c, 0, c->size);
    int idx = d->nchunks++;
    VkDeviceSize off;
    if (chunk_take(c, req.size, req.alignment ? req.alignment : 1, &off) != 0)
        re5_fatal("re5vk", "alocação de %llu bytes não cabe num bloco novo", (unsigned long long)req.size);
    a.mem = c->mem;
    a.off = off;
    a.map = c->map ? c->map + off : NULL;
    a.chunk = idx;
    return a;
}

#define GUEST_CHUNK ((VkDeviceSize)16 << 20)

/* Importa [va, va+size) do guest; mem = VK_NULL_HANDLE se o driver recusar. */
static VkDeviceMemory import_guest(vk_device *d, uint32_t va, VkDeviceSize size, uint32_t bits, uint32_t *type)
{
    void *p = re5_g2h(va);
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (d->get_host_ptr_props(d->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hp) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    int t = mem_type(d, bits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) return VK_NULL_HANDLE;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = p};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)t};
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (vkAllocateMemory(d->dev, &ai, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    *type = (uint32_t)t;
    return m;
}

static void zerocopy_off(vk_device *d, const char *why)
{
    RE5_WARN("re5vk", "buffers dinâmicos sem a memória do guest (%s): o Unlock volta a copiar", why);
    d->zerocopy = 0;
}

vk_alloc re5vk_mem_alloc_guest(vk_device *d, VkMemoryRequirements req)
{
    vk_alloc a = {0};
    if (!d->zerocopy) return a;
    VkDeviceSize align = req.alignment > 65536 ? req.alignment : 65536;
    if (req.size > GUEST_CHUNK / 2) {   /* grande: bloco próprio */
        VkDeviceSize size = (req.size + align - 1) / align * align;
        uint32_t va = re5_mem_alloc((uint32_t)size, RE5_PAGE_READWRITE, "re5vk-buffer");
        if (!va) { zerocopy_off(d, "sem espaço no guest"); return a; }
        uint32_t t;
        a.mem = import_guest(d, va, size, req.memoryTypeBits, &t);
        if (!a.mem) { re5_mem_free(va); zerocopy_off(d, "importação recusada"); return a; }
        a.size = size;
        a.map = re5_g2h(va);
        a.chunk = -2;
        a.type = t;
        return a;
    }
    for (int i = 0; i < d->nchunks; i++) {
        vk_chunk *c = &d->chunks[i];
        if (!c->guest_va || !(req.memoryTypeBits >> (c->type & 0xFFu) & 1u)) continue;
        VkDeviceSize off;
        if (chunk_take(c, req.size, req.alignment ? req.alignment : 1, &off) == 0) {
            a.mem = c->mem;
            a.off = off;
            a.size = req.size;
            a.map = c->map + off;
            a.chunk = i;
            a.type = c->type & 0xFFu;
            return a;
        }
    }
    if (d->nchunks >= VK_MAX_CHUNKS) { zerocopy_off(d, "blocos esgotados"); return a; }
    uint32_t va = re5_mem_alloc((uint32_t)GUEST_CHUNK, RE5_PAGE_READWRITE, "re5vk-buffers");
    if (!va) { zerocopy_off(d, "sem espaço no guest"); return a; }
    uint32_t t;
    VkDeviceMemory m = import_guest(d, va, GUEST_CHUNK, req.memoryTypeBits, &t);
    if (!m) { re5_mem_free(va); zerocopy_off(d, "importação recusada"); return a; }
    vk_chunk *c = &d->chunks[d->nchunks];
    memset(c, 0, sizeof *c);
    c->size = GUEST_CHUNK;
    c->type = t | 0x200u;
    c->mem = m;
    c->map = re5_g2h(va);
    c->guest_va = va;
    chunk_free_insert(c, 0, c->size);
    int idx = d->nchunks++;
    VkDeviceSize off;
    if (chunk_take(c, req.size, req.alignment ? req.alignment : 1, &off) != 0) {
        zerocopy_off(d, "não coube no bloco");
        return (vk_alloc){0};
    }
    a.mem = m;
    a.off = off;
    a.size = req.size;
    a.map = c->map + off;
    a.chunk = idx;
    a.type = t;
    return a;
}

void re5vk_mem_free(vk_device *d, vk_alloc *a)
{
    if (!a->mem) return;
    if (a->chunk == -2) {
        vkFreeMemory(d->dev, a->mem, NULL);
        re5_mem_free(re5_h2g(a->map));
        memset(a, 0, sizeof *a);
        return;
    }
    if (a->chunk < 0) vkFreeMemory(d->dev, a->mem, NULL);
    else chunk_free_insert(&d->chunks[a->chunk], a->off, a->size);
    memset(a, 0, sizeof *a);
}

/* ---- envios ---- */

static void garbage_run(vk_device *d, vk_slot *s)
{
    for (int i = 0; i < s->ngarbage; i++) {
        vk_garbage *g = &s->garbage[i];
        if (g->kind == 1) vkDestroyBuffer(d->dev, g->h.buffer, NULL);
        else if (g->kind == 2) vkDestroyImage(d->dev, g->h.image, NULL);
        else if (g->kind == 3) vkDestroyImageView(d->dev, g->h.view, NULL);
        re5vk_mem_free(d, &g->mem);
    }
    s->ngarbage = 0;
}

void re5vk_destroy_later(vk_device *d, int kind, uint64_t handle, vk_alloc mem)
{
    vk_slot *s = &d->slots[d->cur];
    if (s->ngarbage == s->capgarbage) {
        s->capgarbage = s->capgarbage ? s->capgarbage * 2 : 64;
        s->garbage = realloc(s->garbage, (size_t)s->capgarbage * sizeof *s->garbage);
        if (!s->garbage) re5_fatal("re5vk", "sem memória");
    }
    vk_garbage *g = &s->garbage[s->ngarbage++];
    memset(g, 0, sizeof *g);
    g->kind = kind;
    if (kind == 1) g->h.buffer = (VkBuffer)(uintptr_t)handle;
    else if (kind == 2) g->h.image = (VkImage)(uintptr_t)handle;
    else if (kind == 3) g->h.view = (VkImageView)(uintptr_t)handle;
    g->mem = mem;
}

static void slot_wait(vk_device *d, vk_slot *s)
{
    if (!s->serial) return;
    VK_CHECK(vkWaitForFences(d->dev, 1, &s->fence, VK_TRUE, UINT64_MAX));
    if (s->serial > d->done_serial) d->done_serial = s->serial;
    s->serial = 0;
    garbage_run(d, s);
}

void re5vk_poll(vk_device *d)
{
    for (int i = 0; i < VK_SLOTS; i++) {
        vk_slot *s = &d->slots[i];
        if (s->serial && vkGetFenceStatus(d->dev, s->fence) == VK_SUCCESS) slot_wait(d, s);
    }
}

void re5vk_wait_serial(vk_device *d, uint64_t serial)
{
    if (serial <= d->done_serial) return;
    if (serial >= d->serial) re5vk_flush(d, 0);   /* o envio em gravação: manda antes de esperar */
    for (int i = 0; i < VK_SLOTS; i++)
        if (d->slots[i].serial && d->slots[i].serial <= serial) slot_wait(d, &d->slots[i]);
}

static void slot_begin(vk_device *d, int i)
{
    vk_slot *s = &d->slots[i];
    slot_wait(d, s);
    VK_CHECK(vkResetFences(d->dev, 1, &s->fence));
    for (int p = 0; p < s->npools; p++) VK_CHECK(vkResetDescriptorPool(d->dev, s->pools[p], 0));
    s->pool_cur = 0;
    s->ubo_used = 0;
    s->stage_used = 0;
    VK_CHECK(vkResetCommandBuffer(s->cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(s->cmd, &bi));
    d->cur = i;
    d->bound_pipe = VK_NULL_HANDLE;
    d->vb_valid = 0;   /* command buffer novo: nenhum vertex/index buffer ligado */
    d->bound_ib = VK_NULL_HANDLE;
    /* UBO, descritores e estado dinâmico são por envio (o pool do set anterior é zerado na volta) */
    d->vs_dirty = d->ps_dirty = d->desc_dirty = d->dyn_dirty = 1;
    d->set = VK_NULL_HANDLE;
}

VkCommandBuffer re5vk_cmd(vk_device *d) { return d->slots[d->cur].cmd; }

static void submit(vk_device *d, VkSemaphore wait, VkSemaphore signal)
{
    vk_slot *s = &d->slots[d->cur];
    VK_CHECK(vkEndCommandBuffer(s->cmd));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait ? 1u : 0u,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &ws,
        .commandBufferCount = 1,
        .pCommandBuffers = &s->cmd,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores = &signal,
    };
    VK_CHECK(vkQueueSubmit(d->queue, 1, &si, s->fence));
    s->serial = d->serial++;
}

void re5vk_flush(vk_device *d, int wait)
{
    re5vk_end_rendering(d);
    int prev = d->cur;
    submit(d, VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (wait) slot_wait(d, &d->slots[prev]);
    slot_begin(d, (prev + 1) % VK_SLOTS);
}

static uint8_t *ring_take(vk_device *d, int ubo, VkDeviceSize size, VkDeviceSize align, VkBuffer *buf,
                          VkDeviceSize *off)
{
    for (int pass = 0; pass < 2; pass++) {
        vk_slot *s = &d->slots[d->cur];
        VkDeviceSize used = ubo ? s->ubo_used : s->stage_used, cap = ubo ? VK_UBO_RING : s->stage_size;
        VkDeviceSize o = (used + align - 1) / align * align;
        if (o + size <= cap) {
            if (ubo) s->ubo_used = o + size;
            else s->stage_used = o + size;
            *buf = ubo ? s->ubo : s->stage;
            *off = o;
            return (ubo ? s->ubo_mem.map : s->stage_mem.map) + o;
        }
        if (size > cap) break;
        re5vk_flush(d, 0);   /* anel do envio cheio: manda e continua no próximo */
    }
    if (ubo) re5_fatal("re5vk", "bloco de constantes de %llu bytes", (unsigned long long)size);
    /* staging maior que o anel: buffer próprio, destruído depois do envio */
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer b;
    VK_CHECK(vkCreateBuffer(d->dev, &bci, NULL, &b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->dev, b, &req);
    vk_alloc a = re5vk_mem_alloc(d, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VK_CHECK(vkBindBufferMemory(d->dev, b, a.mem, a.off));
    re5vk_destroy_later(d, 1, (uint64_t)(uintptr_t)b, a);
    *buf = b;
    *off = 0;
    return a.map;
}

uint8_t *re5vk_stage(vk_device *d, VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off)
{
    return ring_take(d, 0, size, align < 16 ? 16 : align, buf, off);
}

uint8_t *re5vk_ubo(vk_device *d, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off)
{
    VkDeviceSize al = d->props.limits.minUniformBufferOffsetAlignment;
    return ring_take(d, 1, size, al < 16 ? 16 : al, buf, off);
}

static VkDescriptorPool pool_new(vk_device *d)
{
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * 2048},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 20 * 2048},
    };
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2048,
                                     .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool p;
    VK_CHECK(vkCreateDescriptorPool(d->dev, &ci, NULL, &p));
    return p;
}

VkDescriptorSet re5vk_alloc_set(vk_device *d)
{
    vk_slot *s = &d->slots[d->cur];
    for (;;) {
        if (s->pool_cur >= s->npools) {
            if (s->npools >= 8) {   /* muitos draws num envio só: manda e continua */
                re5vk_flush(d, 0);
                s = &d->slots[d->cur];
                continue;
            }
            s->pools[s->npools++] = pool_new(d);
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->pool_cur], .descriptorSetCount = 1,
                                          .pSetLayouts = &d->dsl};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(d->dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
            re5_fatal("re5vk", "vkAllocateDescriptorSets -> %s", re5vk_result(r));
        s->pool_cur++;
    }
}

void re5vk_barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                         0, NULL);
}

static void slots_create(vk_device *d)
{
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = d->qfam};
    VK_CHECK(vkCreateCommandPool(d->dev, &pci, NULL, &d->pool));
    for (int i = 0; i < VK_SLOTS; i++) {
        vk_slot *s = &d->slots[i];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = d->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(d->dev, &ai, &s->cmd));
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(d->dev, &fi, NULL, &s->fence));
        s->pools[s->npools++] = pool_new(d);
        for (int k = 0; k < 2; k++) {
            VkDeviceSize size = k ? VK_STAGE_RING : VK_UBO_RING;
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                      .usage = k ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                 : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
            VkBuffer b;
            VK_CHECK(vkCreateBuffer(d->dev, &bci, NULL, &b));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(d->dev, b, &req);
            vk_alloc a = re5vk_mem_alloc(d, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
            VK_CHECK(vkBindBufferMemory(d->dev, b, a.mem, a.off));
            if (k) { s->stage = b; s->stage_mem = a; s->stage_size = size; }
            else { s->ubo = b; s->ubo_mem = a; }
        }
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(d->dev, &sci, NULL, &d->acquired[i]));
    }
    d->serial = 1;
    slot_begin(d, 0);
}

/* ---- swapchain ---- */

static void swapchain_create(vk_device *d)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surface, &caps));
    uint32_t nf = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &nf, NULL));
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surface, &nf, fmts));
    d->swap_format = fmts[0].format;
    VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            d->swap_format = fmts[i].format;
            cs = fmts[i].colorSpace;
            break;
        }
    }
    d->swap_extent = caps.currentExtent;
    if (d->swap_extent.width == 0xFFFFFFFFu) {
        d->swap_extent.width = d->pp.BackBufferWidth;
        d->swap_extent.height = d->pp.BackBufferHeight;
    }
    uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    /* Identidade, como a DXVK: no Android paisagem o compositor gira e o present diz SUBOPTIMAL. */
    VkSurfaceTransformFlagBitsKHR xf = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                           : caps.currentTransform;
    /* Modo de apresentação como a DXVK: o jogo pede "sem vsync" (D3DPRESENT_INTERVAL_IMMEDIATE) ->
     * MAILBOX, senão IMMEDIATE; com vsync, FIFO. RE5VK_PRESENT=fifo|mailbox|immediate força.
     * Medido no PC (Radeon Vega, janela): FIFO forçado travava o menu em ~33 fps. */
    VkPresentModeKHR modes[8];
    uint32_t nm = 8;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, d->surface, &nm, modes) < 0) nm = 0;
    const char *want = getenv("RE5VK_PRESENT");
    VkPresentModeKHR pref[3] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR};
    if (want && !strcmp(want, "mailbox")) pref[0] = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (want && !strcmp(want, "immediate")) pref[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (!(want && !strcmp(want, "fifo")) && d->pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) {
        pref[0] = VK_PRESENT_MODE_MAILBOX_KHR;
        pref[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (int p = 0; p < 3 && mode == VK_PRESENT_MODE_FIFO_KHR; p++)
        for (uint32_t i = 0; i < nm; i++)
            if (modes[i] == pref[p]) { mode = pref[p]; break; }
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = d->surface,
        .minImageCount = n,
        .imageFormat = d->swap_format,
        .imageColorSpace = cs,
        .imageExtent = d->swap_extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = xf,
        .compositeAlpha = alpha,
        .presentMode = mode,
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
    RE5_INFO("re5vk", "swapchain %ux%u, formato %d, %u imagens, %s", d->swap_extent.width, d->swap_extent.height,
             (int)d->swap_format, d->nimages,
             mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO");
}

/* ---- back buffer e profundidade automática ---- */

static vk_surface *own_surface(vk_device *d, uint32_t w, uint32_t h, D3DFORMAT f, int depth)
{
    const vk_format *fmt = re5vk_format(f);
    if (!fmt) re5_fatal("re5vk", "formato %u do back buffer/profundidade sem tradução", (unsigned)f);
    vk_image *im = calloc(1, sizeof *im);
    if (!im || re5vk_image_create(d, im, VK_IMAGE_TYPE_2D, fmt, w, h, 1, 1, 1, 0, !depth, depth) != 0)
        re5_fatal("re5vk", "imagem %ux%u formato %u", w, h, (unsigned)f);
    return re5vk_surface_new(d, NULL, im, 1, 0, 0, f, depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET,
                             D3DPOOL_DEFAULT);
}

static void backbuffer_create(vk_device *d)
{
    if (!d->pp.BackBufferWidth || !d->pp.BackBufferHeight) {
        d->pp.BackBufferWidth = d->swap_extent.width;
        d->pp.BackBufferHeight = d->swap_extent.height;
    }
    if (d->pp.BackBufferFormat == D3DFMT_UNKNOWN) d->pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (d->pp.MultiSampleType > D3DMULTISAMPLE_NONMASKABLE)
        re5_fatal("re5vk", "back buffer com MSAA %u sem implementação", (unsigned)d->pp.MultiSampleType);
    d->backbuffer = own_surface(d, d->pp.BackBufferWidth, d->pp.BackBufferHeight, d->pp.BackBufferFormat, 0);
    d->auto_depth = d->pp.EnableAutoDepthStencil ? own_surface(d, d->pp.BackBufferWidth, d->pp.BackBufferHeight,
                                                               d->pp.AutoDepthStencilFormat, 1)
                                                 : NULL;
    RE5_INFO("re5vk", "back buffer %ux%u formato %u%s", d->pp.BackBufferWidth, d->pp.BackBufferHeight,
             (unsigned)d->pp.BackBufferFormat, d->auto_depth ? ", com profundidade automática" : "");
}

/* ---- métodos do device deste arquivo ---- */

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
    if (n == 0) {
        /* O processo sai logo depois (o RE5 solta o device no fim): espera a GPU e larga o resto. */
        vkDeviceWaitIdle(d->dev);
        RE5_INFO("re5vk", "device liberado depois de %llu Present", (unsigned long long)d->presents);
    }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE v_TestCooperativeLevel(IDirect3DDevice9 *self) { return D3D_OK; }
static UINT STDMETHODCALLTYPE v_GetAvailableTextureMem(IDirect3DDevice9 *self)
{
    vk_device *d = (vk_device *)self;
    VkDeviceSize best = 0;
    for (uint32_t i = 0; i < d->memprops.memoryHeapCount; i++)
        if ((d->memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && d->memprops.memoryHeaps[i].size > best)
            best = d->memprops.memoryHeaps[i].size;
    return best > 0xFFE00000u ? 0xFFE00000u : (UINT)best;
}
static HRESULT STDMETHODCALLTYPE v_EvictManagedResources(IDirect3DDevice9 *self) { return D3D_OK; }
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
static HRESULT STDMETHODCALLTYPE v_GetDisplayMode(IDirect3DDevice9 *self, UINT sc, D3DDISPLAYMODE *dm)
{
    vk_device *d = (vk_device *)self;
    if (sc) return D3DERR_INVALIDCALL;
    return d->d3d->inner->lpVtbl->GetAdapterDisplayMode(d->d3d->inner, d->cp.AdapterOrdinal, dm);
}
static HRESULT STDMETHODCALLTYPE v_GetCreationParameters(IDirect3DDevice9 *self, D3DDEVICE_CREATION_PARAMETERS *cp)
{
    if (!cp) return D3DERR_INVALIDCALL;
    *cp = ((vk_device *)self)->cp;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetBackBuffer(IDirect3DDevice9 *self, UINT sc, UINT bb, D3DBACKBUFFER_TYPE t,
                                                 IDirect3DSurface9 **out)
{
    vk_device *d = (vk_device *)self;
    if (sc || bb || !out) return D3DERR_INVALIDCALL;
    re5vk_addref(&d->backbuffer->o);
    *out = (IDirect3DSurface9 *)d->backbuffer;
    return D3D_OK;
}
static void STDMETHODCALLTYPE v_SetGammaRamp(IDirect3DDevice9 *self, UINT sc, DWORD flags, const D3DGAMMARAMP *r)
{
    vk_device *d = (vk_device *)self;
    if (!r || d->warned_gamma) return;
    for (int i = 0; i < 256; i++) {
        WORD want = (WORD)(i * 257);
        if (r->red[i] != want || r->green[i] != want || r->blue[i] != want) {
            RE5_WARN("re5vk", "SetGammaRamp não identidade: a rampa ainda não é aplicada no Present "
                              "(brilho do jogo sem efeito no re5vk)");
            d->warned_gamma = 1;
            return;
        }
    }
}
static HRESULT STDMETHODCALLTYPE v_BeginScene(IDirect3DDevice9 *self)
{
    ((vk_device *)self)->in_scene = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_EndScene(IDirect3DDevice9 *self)
{
    ((vk_device *)self)->in_scene = 0;
    return D3D_OK;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access)
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
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

/* Quadros em voo antes de o Present esperar a GPU (RE5VK_LATENCY, padrão 3). */
static int latency(void)
{
    static int v;
    if (!v) {
        const char *e = getenv("RE5VK_LATENCY");
        v = e && atoi(e) > 0 ? atoi(e) : 3;
        if (v > VK_SLOTS - 2) v = VK_SLOTS - 2;
    }
    return v;
}

static HRESULT STDMETHODCALLTYPE v_Present(IDirect3DDevice9 *self, const RECT *src, const RECT *dst, HWND over,
                                           const RGNDATA *dirty)
{
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    re5vk_end_rendering(d);
    /* A janela mudou de tamanho (tela cheia, Reset, arrastar): a swapchain acompanha, como na DXVK.
     * Antes ela só era refeita com OUT_OF_DATE, que o driver AMD no Windows não devolve: a imagem
     * ficava com o tamanho da janela da criação (1280x720), no canto da tela cheia ou cortada na
     * janela menor. O blit abaixo escala o back buffer para o tamanho novo. */
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surface, &caps) == VK_SUCCESS &&
        caps.currentExtent.width != 0xFFFFFFFFu) {
        if (!caps.currentExtent.width || !caps.currentExtent.height) {   /* minimizada: nada a apresentar */
            re5vk_unlock(d);
            return D3D_OK;
        }
        if (caps.currentExtent.width != d->swap_extent.width || caps.currentExtent.height != d->swap_extent.height) {
            re5vk_flush(d, 1);
            vkDeviceWaitIdle(d->dev);
            swapchain_create(d);
        }
    }
    VkCommandBuffer cb = re5vk_cmd(d);
    VkSemaphore acq = d->acquired[d->cur];
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(d->dev, d->swap, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        re5vk_flush(d, 1);
        vkDeviceWaitIdle(d->dev);
        swapchain_create(d);
        re5vk_unlock(d);
        return D3D_OK;
    }
    if (r < 0) re5_fatal("re5vk", "vkAcquireNextImageKHR -> %s", re5vk_result(r));
    VkImage img = d->images[idx];
    re5vk_barrier_all(cb);
    image_barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
    vk_image *bb = d->backbuffer->img;
    VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)bb->w, (int32_t)bb->h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {(int32_t)d->swap_extent.width, (int32_t)d->swap_extent.height, 1}},
    };
    vkCmdBlitImage(cb, bb->image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    image_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    bb->last_use = d->serial;
    int prev = d->cur;
    uint64_t this_serial = d->serial;
    submit(d, acq, d->done[idx]);
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &d->done[idx],
        .swapchainCount = 1,
        .pSwapchains = &d->swap,
        .pImageIndices = &idx,
    };
    /* A espera da GPU e do compositor não segura o device: outras threads podem criar recursos. */
    r = vkQueuePresentKHR(d->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(d->dev);
        swapchain_create(d);
    } else if (r < 0) {
        re5_fatal("re5vk", "vkQueuePresentKHR -> %s", re5vk_result(r));
    }
    slot_begin(d, (prev + 1) % VK_SLOTS);
    re5vk_unlock(d);
    /* limite de quadros em voo, fora do mutex */
    if (this_serial > (uint64_t)latency()) {
        re5vk_lock(d);
        uint64_t want = this_serial - (uint64_t)latency();
        for (int i = 0; i < VK_SLOTS; i++)
            if (d->slots[i].serial && d->slots[i].serial <= want && i != d->cur) slot_wait(d, &d->slots[i]);
        re5vk_unlock(d);
    }
    if (++d->presents == 1) RE5_INFO("re5vk", "primeiro Present");
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_Reset(IDirect3DDevice9 *self, D3DPRESENT_PARAMETERS *pp)
{
    vk_device *d = (vk_device *)self;
    if (!pp) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    re5vk_flush(d, 1);
    vkDeviceWaitIdle(d->dev);
    for (int i = 0; i < 4; i++) {
        if (d->rt[i]) re5vk_release(&d->rt[i]->o);
        d->rt[i] = NULL;
    }
    if (d->ds) re5vk_release(&d->ds->o);
    d->ds = NULL;
    re5vk_release(&d->backbuffer->o);
    if (d->auto_depth) re5vk_release(&d->auto_depth->o);
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    d->pp = *pp;
    backbuffer_create(d);
    pp->BackBufferWidth = d->pp.BackBufferWidth;
    pp->BackBufferHeight = d->pp.BackBufferHeight;
    re5vk_state_reset(d);
    re5vk_unlock(d);
    return D3D_OK;
}

static struct IDirect3DDevice9Vtbl g_dev_vtbl;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void layout_create(vk_device *d)
{
    VkDescriptorSetLayoutBinding b[RE5VK_NBINDINGS];
    for (uint32_t i = 0; i < RE5VK_NBINDINGS; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = i == RE5VK_BIND_VS_UBO || i >= RE5VK_BIND_VS_SAMP ? VK_SHADER_STAGE_VERTEX_BIT
                                                                            : VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = RE5VK_NBINDINGS, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(d->dev, &lci, NULL, &d->dsl));
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &d->dsl};
    VK_CHECK(vkCreatePipelineLayout(d->dev, &pci, NULL, &d->layout));
    VkPipelineCacheCreateInfo cci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VK_CHECK(vkCreatePipelineCache(d->dev, &cci, NULL, &d->pcache));
}

HRESULT re5vk_device_create(vk_d3d9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                            D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    if (!pp || !out) return D3DERR_INVALIDCALL;
    vk_load_loader();
    HWND win = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if (!win) re5_fatal("re5vk", "CreateDevice sem janela");
#if !defined(_WIN32) && !defined(__ANDROID__)
    re5_fatal("re5vk", "este host não tem superfície Vulkan no re5vk (só Windows e Android)");
#endif

    vk_device *d = calloc(1, sizeof *d);
    if (!d) re5_fatal("re5vk", "sem memória");
    if (!g_dev_vtbl.QueryInterface) {
        void **slot = (void **)&g_dev_vtbl;
        for (size_t i = 0; i < sizeof g_dev_vtbl / sizeof(void *); i++) slot[i] = (void *)re5vk_nyi;
        g_dev_vtbl.QueryInterface = v_QueryInterface;
        g_dev_vtbl.AddRef = v_AddRef;
        g_dev_vtbl.Release = v_Release;
        g_dev_vtbl.TestCooperativeLevel = v_TestCooperativeLevel;
        g_dev_vtbl.GetAvailableTextureMem = v_GetAvailableTextureMem;
        g_dev_vtbl.EvictManagedResources = v_EvictManagedResources;
        g_dev_vtbl.GetDirect3D = v_GetDirect3D;
        g_dev_vtbl.GetDeviceCaps = v_GetDeviceCaps;
        g_dev_vtbl.GetDisplayMode = v_GetDisplayMode;
        g_dev_vtbl.GetCreationParameters = v_GetCreationParameters;
        g_dev_vtbl.GetBackBuffer = v_GetBackBuffer;
        g_dev_vtbl.SetGammaRamp = v_SetGammaRamp;
        g_dev_vtbl.BeginScene = v_BeginScene;
        g_dev_vtbl.EndScene = v_EndScene;
        g_dev_vtbl.Present = v_Present;
        g_dev_vtbl.Reset = v_Reset;
        g_dev_vtbl.CreateTexture = re5vk_CreateTexture;
        g_dev_vtbl.CreateCubeTexture = re5vk_CreateCubeTexture;
        g_dev_vtbl.CreateVolumeTexture = re5vk_CreateVolumeTexture;
        g_dev_vtbl.CreateVertexBuffer = re5vk_CreateVertexBuffer;
        g_dev_vtbl.CreateIndexBuffer = re5vk_CreateIndexBuffer;
        g_dev_vtbl.CreateRenderTarget = re5vk_CreateRenderTarget;
        g_dev_vtbl.CreateDepthStencilSurface = re5vk_CreateDepthStencilSurface;
        g_dev_vtbl.CreateVertexDeclaration = re5vk_CreateVertexDeclaration;
        g_dev_vtbl.CreateVertexShader = re5vk_CreateVertexShader;
        g_dev_vtbl.CreatePixelShader = re5vk_CreatePixelShader;
        g_dev_vtbl.CreateQuery = re5vk_CreateQuery;
        g_dev_vtbl.CreateOffscreenPlainSurface = re5vk_CreateOffscreenPlainSurface;
        g_dev_vtbl.GetRenderTargetData = re5vk_GetRenderTargetData;
        re5vk_draw_vtable(&g_dev_vtbl);
    }
    d->lpVtbl = &g_dev_vtbl;
    d->refs = 1;
    d->d3d = d3d;
    d3d->refs++;
    re5_mutex_init(&d->mu);
    d->cp = (D3DDEVICE_CREATION_PARAMETERS){adapter, type, focus, flags};
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;
    d->pp = *pp;

    const char *iexts[2] = {VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
                            VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#elif defined(__ANDROID__)
                            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
#else
                            NULL
#endif
    };
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "re5",
                             .pEngineName = "re5vk", .apiVersion = VK_API_VERSION_1_3};
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

    /* GPU: a primeira com Vulkan 1.3 e uma fila de gráficos que apresenta nesta superfície. */
    uint32_t npd = 8;
    VkPhysicalDevice pds[8];
    VK_CHECK(vkEnumeratePhysicalDevices(d->inst, &npd, pds));
    for (uint32_t i = 0; i < npd && !d->phys; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
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
    if (!d->phys) re5_fatal("re5vk", "nenhuma GPU com Vulkan 1.3 e fila de gráficos que apresente na janela");
    vkGetPhysicalDeviceProperties(d->phys, &d->props);
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memprops);
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(d->phys, NULL, &next, NULL);
    VkExtensionProperties *exts = calloc(next ? next : 1, sizeof *exts);
    if (exts) vkEnumerateDeviceExtensionProperties(d->phys, NULL, &next, exts);
    if (!exts || !has_ext(exts, next, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        re5_fatal("re5vk", "GPU '%s' sem VK_KHR_swapchain", d->props.deviceName);
    const char *zc_env = getenv("RE5VK_ZEROCOPY");
    /* Desligado por padrão: empate no A/B do usuário (93,5 x 93,9 fixo; 60,8 x 60,3 variável) e gasta
     * espaço de endereço do guest. RE5VK_ZEROCOPY=1 liga. */
    int want_zc = zc_env && *zc_env == '1' && has_ext(exts, next, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    free(exts);

    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vkGetPhysicalDeviceFeatures2(d->phys, &f2);
    if (!f13.dynamicRendering) re5_fatal("re5vk", "GPU '%s' sem dynamicRendering", d->props.deviceName);
    VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures2 e2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e13};
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.textureCompressionBC = f2.features.textureCompressionBC;
    e2.features.robustBufferAccess = f2.features.robustBufferAccess;   /* c[a0] fora do limite lê 0, como no D3D9 */
    e2.features.independentBlend = f2.features.independentBlend;
    e2.features.depthBiasClamp = f2.features.depthBiasClamp;
    e2.features.fillModeNonSolid = f2.features.fillModeNonSolid;
    RE5_INFO("re5vk", "GPU '%s' (Vulkan %u.%u.%u, driver 0x%X), fila %u, BC %s", d->props.deviceName,
             VK_API_VERSION_MAJOR(d->props.apiVersion), VK_API_VERSION_MINOR(d->props.apiVersion),
             VK_API_VERSION_PATCH(d->props.apiVersion), d->props.driverVersion, d->qfam,
             f2.features.textureCompressionBC ? "sim" : "não");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = d->qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = want_zc ? 2u : 1u,
                              .ppEnabledExtensionNames = dexts};
    VK_CHECK(vkCreateDevice(d->phys, &dci, NULL, &d->dev));
    if (want_zc) {
        d->get_host_ptr_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(d->dev,
                                                                                     "vkGetMemoryHostPointerPropertiesEXT");
        d->zerocopy = d->get_host_ptr_props != NULL;
    }
    RE5_INFO("re5vk", "buffers dinâmicos na memória do guest (Unlock sem cópia): %s",
             d->zerocopy ? "sim" : !(zc_env && *zc_env == '1') ? "não (RE5VK_ZEROCOPY=1 liga)" : "não (sem VK_EXT_external_memory_host)");
#define VK_LOAD_DEVICE(name)                                                                                       \
    if (!(name = (PFN_##name)vkGetDeviceProcAddr(d->dev, #name))) re5_fatal("re5vk", "sem %s", #name);
    VK_DEVICE_FNS(VK_LOAD_DEVICE)
    vkGetDeviceQueue(d->dev, d->qfam, 0, &d->queue);

    layout_create(d);
    slots_create(d);
    swapchain_create(d);
    re5vk_res_vtables(d);
    re5vk_init_dummies(d);
    backbuffer_create(d);
    pp->BackBufferWidth = d->pp.BackBufferWidth;
    pp->BackBufferHeight = d->pp.BackBufferHeight;
    pp->BackBufferFormat = d->pp.BackBufferFormat;
    re5vk_state_reset(d);
    *out = (IDirect3DDevice9 *)d;
    return D3D_OK;
}
