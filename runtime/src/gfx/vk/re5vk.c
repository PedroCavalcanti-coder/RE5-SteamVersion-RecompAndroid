/*
 * re5vk.c - backend Vulkan próprio atrás da ponte D3D9 (docs/PROJETO_VULKAN_NATIVO.md).
 *
 * RE5_GFX=vkadreno ou vkmali liga (pelo seletor, re5/renderer.h); o padrão continua a DXVK (RE5_GFX=dxvk). O mesmo código serve ao
 * Windows x64 e ao Android ARM64: só a superfície (Win32 / ANativeWindow) e o carregador mudam.
 *
 * O IDirect3D9 deste backend repassa as consultas do adaptador (caps, formatos, modos) ao
 * IDirect3D9 da DXVK carregada, então o jogo vê as mesmas caps e escolhe o mesmo caminho de
 * render que com a DXVK (A/B honesto). O device é nosso. Este arquivo: carregador, IDirect3D9,
 * device, memória, envios e Present; ver vk_core.h para o resto. Instância, device, swapchain,
 * memória (VMA) e o cache de passes vêm de ../vkn pela ponte vkn_bridge.h.
 *
 * Método sem implementação aborta com o nome (D8): o nome vem do thunk do guest em execução,
 * porque a ponte chama o método do host de dentro do handler do método de mesmo nome.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <SDL2/SDL.h> /* SDL_AndroidGetInternalStoragePath, para o cache de pipelines */
#endif

#include "vk_core.h"
#include "vk_blit_spv.h"
#include "re5/re5vk.h"
#include "re5/d3d9_bridge.h"
#include "re5/vk_loader.h"
#include "re5/thunks.h"
#include "re5/guest_base.h"
#include "re5/guest_memory.h"
#include "re5/stats.h"
#include "re5/thread.h"

static void pcache_save(vk_device *d);
static void pcache_save_ex(vk_device *d, int wait);

/* ---- carregador ---- */

#define VK_DEFINE(name) PFN_##name name;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_INSTANCE_FNS(VK_DEFINE)
VK_DEVICE_FNS(VK_DEFINE)

static void vk_load_loader(void)
{
    if (vkGetInstanceProcAddr) return;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)re5_vk_get_instance_proc_addr();
    if (!vkGetInstanceProcAddr) re5_fatal("re5vk", "o carregador do Vulkan não exporta vkGetInstanceProcAddr");
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

/* RE5_FPS_LOG: relógio só com a medição ligada (desligada, nenhuma leitura de relógio a mais). */
static inline uint64_t tm_now(const vk_device *d) { return d->tm.on ? re5_stat_now_ns() : 0; }
static void device_idle(vk_device *d);
static int present_drain(vk_device *d);
static void present_push(vk_device *d, uint32_t idx, VkSemaphore wait);

void re5vk_lock(vk_device *d)
{
    if (!d->tm.on) {
        re5_mutex_lock(&d->mu);
        return;
    }
    uint64_t t0 = re5_stat_now_ns();
    re5_mutex_lock(&d->mu);
    d->tm.mutex += re5_stat_now_ns() - t0;   /* já com o mutex: soma sem corrida */
}
void re5vk_unlock(vk_device *d) { re5_mutex_unlock(&d->mu); }

/* Uma linha por segundo (RE5_FPS_LOG): ms somados no segundo em cada espera do re5vk e o tempo de
 * GPU dos envios que terminaram (timestamps do começo ao fim de cada command buffer). Com o mutex.
 * Leitura: gpu_ms perto de 1000 = GPU no limite; em_voo_ms/fence_ms altos com gpu_ms baixo = a CPU
 * espera a GPU sem que ela esteja cheia (sincronização demais); acquire/queue_present altos =
 * apresentação (swapchain/compositor). mutex_ms = threads disputando o device. */
static void timing_tick(vk_device *d)
{
    uint64_t now = re5_stat_now_ns();
    if (!d->tm.t0) {
        d->tm.t0 = now;
        return;
    }
    if (now - d->tm.t0 < 1000000000ull) return;
    double s = (double)(now - d->tm.t0) / 1e9, k = 1e-6 / s;
    RE5_INFO("re5vk",
             "por s: quadros %.1f envios %.1f gpu_ms %.1f (%u envios medidos) acquire_ms %.1f fence_ms %.1f "
             "em_voo_ms %.1f submit_ms %.1f queue_present_ms %.1f thread_present_ms %.1f mutex_ms %.1f passes %u uploads_cb_sep %u uploads_cb_main %u",
             d->tm.presents / s, d->tm.submits / s, (double)d->tm.gpu * k, d->tm.gpu_samples,
             (double)d->tm.acquire * k, (double)d->tm.fence * k, (double)d->tm.inflight * k,
             (double)d->tm.submit * k, (double)d->tm.qpresent * k, (double)d->tm.pq_ns * k, (double)d->tm.mutex * k, d->tm.passes,
             d->tm.up_fast, d->tm.up_slow);
    if (d->tm.passes) {
        char buf[900];
        int n = snprintf(buf, sizeof buf, "passes por s: %u (reabertos no mesmo alvo %u, so MRT mudou %u, trocas desfeitas sem fechar %u) | draws/passe 0:%u 1:%u 2-4:%u 5-19:%u 20+:%u | fechados por:",
                         d->tm.passes, d->tm.reopen, d->tm.reopen_mrt, d->tm.lazy_kept, d->tm.pd_hist[0], d->tm.pd_hist[1], d->tm.pd_hist[2],
                         d->tm.pd_hist[3], d->tm.pd_hist[4]);
        for (uint32_t i = 0; i < d->tm.pw_used && n < (int)sizeof buf - 64; i++)
            n += snprintf(buf + n, sizeof buf - (size_t)n, " %s %u", d->tm.pw_name[i], d->tm.pw_n[i]);
        RE5_INFO("re5vk", "%s", buf);
    }
    if (d->pt.pool && d->pt.passes) {
        /* os 6 tamanhos de alvo que mais gastaram GPU */
        char buf[900];
        int n = snprintf(buf, sizeof buf, "GPU por s: dentro dos passes %.1f ms, entre passes %.1f ms, bordas do envio %.1f ms (%u passes, %u sem medida) | por alvo:",
                         (double)d->pt.in_ns * 1e-6 / s, (double)d->pt.gap_ns * 1e-6 / s, (double)d->pt.edge_ns * 1e-6 / s,
                         d->pt.passes, d->pt.dropped);
        for (int r = 0; r < 6 && n < (int)sizeof buf - 80; r++) {
            uint32_t bi = UINT32_MAX;
            for (uint32_t j = 0; j < d->pt.nby; j++)
                if (d->pt.by[j].n && (bi == UINT32_MAX || d->pt.by[j].ns > d->pt.by[bi].ns)) bi = j;
            if (bi == UINT32_MAX) break;
            n += snprintf(buf + n, sizeof buf - (size_t)n, " %ux%u %u passes %u draws %.1f ms;", d->pt.by[bi].w,
                          d->pt.by[bi].h, d->pt.by[bi].n, d->pt.by[bi].draws, (double)d->pt.by[bi].ns * 1e-6 / s);
            d->pt.by[bi].n = 0;
        }
        RE5_INFO("re5vk", "%s", buf);
    }
    d->pt.in_ns = d->pt.gap_ns = d->pt.edge_ns = 0;
    d->pt.passes = d->pt.dropped = d->pt.nby = 0;
    memset(d->tm.pw_n, 0, sizeof d->tm.pw_n);
    memset(d->tm.pd_hist, 0, sizeof d->tm.pd_hist);
    d->tm.pw_used = d->tm.reopen = d->tm.reopen_mrt = d->tm.lazy_kept = 0;
    d->tm.t0 = now;
    d->tm.acquire = d->tm.fence = d->tm.inflight = d->tm.submit = d->tm.qpresent = d->tm.pq_ns = d->tm.mutex = d->tm.gpu = 0;
    d->tm.submits = d->tm.gpu_samples = d->tm.presents = 0;
    d->tm.passes = d->tm.up_fast = d->tm.up_slow = 0;
}

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
    HRESULT hr = INNER(self)->lpVtbl->CheckDeviceFormat(INNER(self), a, t, af, u, rt, cf);
    if (FAILED(hr) && (cf == D3DFMT_DXT1 || cf == D3DFMT_DXT2 || cf == D3DFMT_DXT3 ||
                       cf == D3DFMT_DXT4 || cf == D3DFMT_DXT5) &&
        !(u & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
        (rt == D3DRTYPE_TEXTURE || rt == D3DRTYPE_CUBETEXTURE || rt == D3DRTYPE_VOLUMETEXTURE)) {
        /* O backend nativo re5vk tem transcodificador DXT -> ETC2 integrado com cache persistente
         * para GPUs sem textureCompressionBC (ex.: ARM Mali). */
        hr = D3D_OK;
    }
    if ((uint32_t)cf > 0xFFFF) {   /* FOURCC (INTZ, ATOC, A2M1, NULL, DF24...): o jogo muda de caminho pela resposta */
        const char *e = getenv("RE5VK_RS_LOG");
        if (e && *e == '1')
            RE5_INFO("re5vk", "RS_LOG CheckDeviceFormat '%c%c%c%c' uso 0x%X tipo %u -> 0x%08X", (int)(cf & 0xFF),
                      (int)((cf >> 8) & 0xFF), (int)((cf >> 16) & 0xFF), (int)((cf >> 24) & 0xFF), (unsigned)u,
                      (unsigned)rt, (unsigned)hr);
    }
    return hr;
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
    HRESULT hr = re5vk_device_create((vk_d3d9 *)self, a, t, focus, flags, pp, out);
    if (FAILED(hr) && INNER(self)) {
        RE5_WARN("re5vk", "re5vk_device_create falhou (0x%08X); caindo de volta para o backend DXVK", (uint32_t)hr);
        return INNER(self)->lpVtbl->CreateDevice(INNER(self), a, t, focus, flags, pp, out);
    }
    return hr;
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
    RE5_INFO("re5vk", "backend Vulkan próprio ligado (RE5_GFX=vkadreno/vkmali): consultas do adaptador pela DXVK, "
                      "device em Vulkan");
    return d;
}

/* ---- memória: VMA pela ponte (vkn_bridge.h) ---- */

VkBuffer re5vk_buffer_new(vk_device *d, VkDeviceSize size, VkBufferUsageFlags usage, int host, const char *name,
                          vk_alloc *a)
{
    (void)d;
    memset(a, 0, sizeof *a);
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VkBuffer b = re5_vkn_buffer_create(&ci, host ? RE5_VKN_MEM_HOST : RE5_VKN_MEM_DEVICE, name, &a->v);
    a->map = a->v.map;
    return b;
}

#define GUEST_CHUNK ((VkDeviceSize)16 << 20)
#define GUEST_ALIGN ((VkDeviceSize)65536)

static void zerocopy_off(vk_device *d, const char *why)
{
    RE5_WARN("re5vk", "buffers dinâmicos sem a memória do guest (%s): o Unlock volta a copiar", why);
    d->zerocopy = 0;
}

/* Bloco novo de memória do guest importado como memória do device; NULL = recusado. */
static void *guest_block(vk_device *d, VkDeviceSize size, uint32_t *va_out)
{
    uint32_t va = re5_mem_alloc((uint32_t)size, RE5_PAGE_READWRITE, "re5vk-buffers");
    if (!va) { zerocopy_off(d, "sem espaço no guest"); return NULL; }
    void *blk = re5_vkn_host_block(re5_g2h(va), size);
    if (!blk) { re5_mem_free(va); zerocopy_off(d, "importação recusada"); return NULL; }
    *va_out = va;
    return blk;
}

VkBuffer re5vk_buffer_new_guest(vk_device *d, VkDeviceSize size, VkBufferUsageFlags usage, vk_alloc *a)
{
    memset(a, 0, sizeof *a);
    if (!d->zerocopy) return VK_NULL_HANDLE;
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = size,
                             .usage = usage};
    VkBuffer b;
    if (size > GUEST_CHUNK / 2) {   /* grande: bloco próprio, solto junto com o buffer */
        VkDeviceSize bsize = (size + GUEST_ALIGN - 1) / GUEST_ALIGN * GUEST_ALIGN + GUEST_ALIGN;
        uint32_t va = 0;
        void *blk = guest_block(d, bsize, &va);
        if (!blk) return VK_NULL_HANDLE;
        b = re5_vkn_host_buffer(blk, &ci, &a->v);
        if (!b) {
            re5_vkn_host_block_free(blk);
            re5_mem_free(va);
            zerocopy_off(d, "não coube no bloco");
            return VK_NULL_HANDLE;
        }
        a->own_block = blk;
        a->guest_va = va;
        a->map = a->v.map;
        return b;
    }
    for (int i = 0; i < d->nchunks; i++) {
        b = re5_vkn_host_buffer(d->chunks[i].block, &ci, &a->v);
        if (b) {
            a->map = a->v.map;
            return b;
        }
    }
    if (d->nchunks >= VK_MAX_CHUNKS) { zerocopy_off(d, "blocos esgotados"); return VK_NULL_HANDLE; }
    uint32_t va = 0;
    void *blk = guest_block(d, GUEST_CHUNK, &va);
    if (!blk) return VK_NULL_HANDLE;
    d->chunks[d->nchunks].block = blk;
    d->chunks[d->nchunks].guest_va = va;
    d->nchunks++;
    b = re5_vkn_host_buffer(blk, &ci, &a->v);
    if (!b) { zerocopy_off(d, "não coube no bloco"); return VK_NULL_HANDLE; }
    a->map = a->v.map;
    return b;
}

void re5vk_buffer_free(vk_device *d, VkBuffer b, vk_alloc *a)
{
    (void)d;
    if (!b) return;
    re5_vkn_buffer_destroy(b, &a->v);
    if (a->own_block) {
        re5_vkn_host_block_free(a->own_block);
        re5_mem_free(a->guest_va);
    }
    memset(a, 0, sizeof *a);
}

/* ---- envios ---- */

static void garbage_run(vk_device *d, vk_slot *s)
{
    for (int i = 0; i < s->ngarbage; i++) {
        vk_garbage *g = &s->garbage[i];
        if (g->kind == 1) {
            re5vk_buffer_free(d, g->h.buffer, &g->mem);
        } else if (g->kind == 2) {
            re5_vkn_image_destroy(g->h.image, &g->mem.v);
        } else if (g->kind == 3) {
            re5_vkn_forget_view(g->h.view);   /* framebuffers com ela saem do cache antes */
            vkDestroyImageView(d->dev, g->h.view, NULL);
        }
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

void re5vk_pt_begin(vk_device *d, VkCommandBuffer cb, uint32_t w, uint32_t h)
{
    if (!d->pt.pool || d->pt.open) return;
    uint32_t n = d->pt.n[d->cur];
    if (n >= RE5VK_PT_CAP) { d->pt.dropped++; return; }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d->pt.pool, (uint32_t)d->cur * 2u * RE5VK_PT_CAP + 2u * n);
    d->pt.w[d->cur][n] = (uint16_t)(w > 65535 ? 65535 : w);
    d->pt.h[d->cur][n] = (uint16_t)(h > 65535 ? 65535 : h);
    d->pt.open = 1;
}

void re5vk_pt_end(vk_device *d, VkCommandBuffer cb, uint32_t draws)
{
    if (!d->pt.pool || !d->pt.open) return;
    uint32_t n = d->pt.n[d->cur];
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->pt.pool,
                        (uint32_t)d->cur * 2u * RE5VK_PT_CAP + 2u * n + 1u);
    d->pt.draws[d->cur][n] = (uint16_t)(draws > 65535 ? 65535 : draws);
    d->pt.n[d->cur] = n + 1;
    d->pt.open = 0;
}

/* Envio pronto: soma o tempo de cada passe, os intervalos entre eles e as bordas do envio. */
static void pt_collect(vk_device *d, int si, const uint64_t *sub_ts)
{
    uint32_t n = d->pt.n[si];
    if (!n) return;
    static uint64_t ts[2 * RE5VK_PT_CAP];
    if (vkGetQueryPoolResults(d->dev, d->pt.pool, (uint32_t)si * 2u * RE5VK_PT_CAP, 2u * n, sizeof ts, ts,
                              sizeof ts[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    uint64_t mask = d->tm.valid_bits >= 64 ? ~0ull : (1ull << d->tm.valid_bits) - 1ull;
    double k = d->tm.period;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t a = ts[2 * i], b = ts[2 * i + 1];
        uint64_t ns = (uint64_t)((double)((b - a) & mask) * k);
        d->pt.in_ns += ns;
        if (i) d->pt.gap_ns += (uint64_t)((double)((a - ts[2 * i - 1]) & mask) * k);
        uint32_t w = d->pt.w[si][i], h = d->pt.h[si][i], j = 0;
        while (j < d->pt.nby && (d->pt.by[j].w != w || d->pt.by[j].h != h)) j++;
        if (j == d->pt.nby && d->pt.nby < RE5VK_PT_BUCKETS) d->pt.by[d->pt.nby++] = (re5vk_pt_bucket){w, h, 0, 0, 0};
        if (j < d->pt.nby) {
            d->pt.by[j].n++;
            d->pt.by[j].draws += d->pt.draws[si][i];
            d->pt.by[j].ns += ns;
        }
    }
    if (sub_ts) {   /* do começo do envio ao primeiro passe e do último ao fim */
        d->pt.edge_ns += (uint64_t)((double)((ts[0] - sub_ts[0]) & mask) * k);
        d->pt.edge_ns += (uint64_t)((double)((sub_ts[1] - ts[2 * n - 1]) & mask) * k);
    }
    d->pt.passes += n;
}

static void slot_wait(vk_device *d, vk_slot *s)
{
    if (!s->serial) return;
    uint64_t t0 = tm_now(d);
    VK_CHECK(vkWaitForFences(d->dev, 1, &s->fence, VK_TRUE, UINT64_MAX));
    if (d->tm.on) {
        d->tm.fence += re5_stat_now_ns() - t0;
        if (d->tm.pool) {   /* envio pronto: os resultados já estão lá, sem esperar */
            uint64_t ts[2];
            uint32_t q = 2u * (uint32_t)(s - d->slots);
            if (vkGetQueryPoolResults(d->dev, d->tm.pool, q, 2, sizeof ts, ts, sizeof ts[0],
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                uint64_t mask = d->tm.valid_bits >= 64 ? ~0ull : (1ull << d->tm.valid_bits) - 1ull;
                d->tm.gpu += (uint64_t)((double)((ts[1] - ts[0]) & mask) * d->tm.period);
                d->tm.gpu_samples++;
                if (d->pt.pool) pt_collect(d, (int)(s - d->slots), ts);
            }
        }
    }
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
    s->ucmd_open = 0;
    VK_CHECK(vkResetCommandBuffer(s->cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(s->cmd, &bi));
    if (d->pt.pool) {
        vkCmdResetQueryPool(s->cmd, d->pt.pool, (uint32_t)i * 2u * RE5VK_PT_CAP, 2u * RE5VK_PT_CAP);
        d->pt.n[i] = 0;
        d->pt.open = 0;
    }
    if (d->tm.pool) {
        vkCmdResetQueryPool(s->cmd, d->tm.pool, 2u * (uint32_t)i, 2);
        vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d->tm.pool, 2u * (uint32_t)i);
    }
    d->cur = i;
    d->bound_pipe = VK_NULL_HANDLE;
    d->vb_valid = 0;   /* command buffer novo: nenhum vertex/index buffer ligado */
    d->bound_ib = VK_NULL_HANDLE;
    d->dyn_valid = 0;
    d->bound_vs_buf = VK_NULL_HANDLE;
    d->bound_ps_buf = VK_NULL_HANDLE;
    d->bound_set = VK_NULL_HANDLE;
    d->bound_vs_off = d->bound_ps_off = 0;
    d->bound_samp_sig = 0;
    /* UBO, descritores e estado dinâmico são por envio (o pool do set anterior é zerado na volta) */
    d->set_cache_gen++;   /* os sets do cache eram do pool que acabou de zerar */
    d->set_cache_n = 0;
    d->vs_dirty = d->ps_dirty = d->desc_dirty = d->dyn_dirty = 1;
    d->set = VK_NULL_HANDLE;
}

VkCommandBuffer re5vk_cmd(vk_device *d) { return d->slots[d->cur].cmd; }

/* Command buffer de upload do envio atual (aberto sob demanda). Roda ANTES do principal no mesmo
 * vkQueueSubmit. So serve para subrecurso nunca usado no envio (layout UNDEFINED): nada gravado antes
 * no cb principal o toca, entao a ordem de execucao nao muda o resultado. */
VkCommandBuffer re5vk_upload_cmd(vk_device *d)
{
    vk_slot *s = &d->slots[d->cur];
    if (!s->ucmd_open) {
        VK_CHECK(vkResetCommandBuffer(s->ucmd, 0));
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK_CHECK(vkBeginCommandBuffer(s->ucmd, &bi));
        s->ucmd_open = 1;
    }
    return s->ucmd;
}

static void submit(vk_device *d, VkSemaphore wait, VkSemaphore signal)
{
    vk_slot *s = &d->slots[d->cur];
    if (d->tm.pool)
        vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->tm.pool, 2u * (uint32_t)d->cur + 1u);
    VK_CHECK(vkEndCommandBuffer(s->cmd));
    VkCommandBuffer cbs[2];
    uint32_t ncb = 0;
    if (s->ucmd_open) {
        VK_CHECK(vkEndCommandBuffer(s->ucmd));
        cbs[ncb++] = s->ucmd;
        s->ucmd_open = 0;
    }
    cbs[ncb++] = s->cmd;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait ? 1u : 0u,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &ws,
        .commandBufferCount = ncb,
        .pCommandBuffers = cbs,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores = &signal,
    };
    uint64_t t0 = tm_now(d);
    re5_mutex_lock(&d->qmu);
    VkResult sr = vkQueueSubmit(d->queue, 1, &si, s->fence);
    re5_mutex_unlock(&d->qmu);
    VK_CHECK(sr);
    if (d->tm.on) {
        d->tm.submit += re5_stat_now_ns() - t0;
        d->tm.submits++;
    }
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
    vk_alloc a;
    VkBuffer b = re5vk_buffer_new(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, "re5vk.staging_grande", &a);
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
        VK_CHECK(vkAllocateCommandBuffers(d->dev, &ai, &s->ucmd));
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(d->dev, &fi, NULL, &s->fence));
        s->pools[s->npools++] = pool_new(d);
        for (int k = 0; k < 2; k++) {
            VkDeviceSize size = k ? VK_STAGE_RING : VK_UBO_RING;
            vk_alloc a;
            VkBuffer b = re5vk_buffer_new(d, size, k ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                          1, k ? "re5vk.anel_staging" : "re5vk.anel_ubo", &a);
            if (k) { s->stage = b; s->stage_mem = a; s->stage_size = size; }
            else { s->ubo = b; s->ubo_mem = a; }
        }
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(d->dev, &sci, NULL, &d->acquired[i]));
    }
    /* Timestamps de GPU só com RE5_FPS_LOG (d->tm.on), e se a fila os tiver. */
    if (d->tm.on) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
        if (!qp) re5_fatal("re5vk", "sem memória");
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nq, qp);
        d->tm.valid_bits = d->qfam < nq ? qp[d->qfam].timestampValidBits : 0;
        free(qp);
        d->tm.period = d->props.limits.timestampPeriod;
        if (d->tm.valid_bits && d->tm.period > 0.0f) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2u * VK_SLOTS};
            VK_CHECK(vkCreateQueryPool(d->dev, &qi, NULL, &d->tm.pool));
            const char *pe = getenv("RE5VK_PASS_TIMING");
            if (pe && *pe == '1') {
                VkQueryPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                            .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = 2u * RE5VK_PT_CAP * VK_SLOTS};
                VK_CHECK(vkCreateQueryPool(d->dev, &pi, NULL, &d->pt.pool));
                RE5_INFO("re5vk", "RE5VK_PASS_TIMING=1: tempo de GPU por passe (até %u passes por envio)", RE5VK_PT_CAP);
            }
        }
        RE5_INFO("re5vk", "RE5_FPS_LOG: tempos por segundo ligados; timestamps de GPU: %s (%u bits, %.3f ns/tique)",
                 d->tm.pool ? "sim" : "não (fila sem timestamp)", d->tm.valid_bits, (double)d->tm.period);
    }
    d->serial = 1;
    slot_begin(d, 0);
}

/* ---- swapchain ---- */

static void present_pipe_create(vk_device *d)
{
    if (d->present_pipe) {
        vkDestroyPipeline(d->dev, d->present_pipe, NULL);
        d->present_pipe = VK_NULL_HANDLE;
    }
    if (!d->present_sampler) {
        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        VK_CHECK(vkCreateSampler(d->dev, &sci, NULL, &d->present_sampler));
    }
    if (!d->present_dsl) {
        VkDescriptorSetLayoutBinding b = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        VkDescriptorSetLayoutCreateInfo lci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &b,
        };
        VK_CHECK(vkCreateDescriptorSetLayout(d->dev, &lci, NULL, &d->present_dsl));
        VkPipelineLayoutCreateInfo plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &d->present_dsl,
        };
        VK_CHECK(vkCreatePipelineLayout(d->dev, &plci, NULL, &d->present_layout));
        VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo dpci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &dps,
        };
        VK_CHECK(vkCreateDescriptorPool(d->dev, &dpci, NULL, &d->present_pool));
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = d->present_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &d->present_dsl,
        };
        VK_CHECK(vkAllocateDescriptorSets(d->dev, &dsai, &d->present_set));
    }
    VkShaderModuleCreateInfo smci_vs = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_blit_vs_spv,
        .pCode = k_blit_vs_spv,
    };
    VkShaderModule vs_mod;
    VK_CHECK(vkCreateShaderModule(d->dev, &smci_vs, NULL, &vs_mod));

    VkShaderModuleCreateInfo smci_ps = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_blit_ps_spv,
        .pCode = k_blit_ps_spv,
    };
    VkShaderModule ps_mod;
    VK_CHECK(vkCreateShaderModule(d->dev, &smci_ps, NULL, &ps_mod));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs_mod, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = ps_mod, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo bs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat sf[4] = {d->swap_format, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .renderPass = re5_vkn_compat_pass(1, sf, VK_FORMAT_UNDEFINED, 0),
        .subpass = 0,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &bs,
        .pDynamicState = &dy,
        .layout = d->present_layout,
    };
    VK_CHECK(vkCreateGraphicsPipelines(d->dev, d->pcache, 1, &ci, NULL, &d->present_pipe));
    vkDestroyShaderModule(d->dev, vs_mod, NULL);
    vkDestroyShaderModule(d->dev, ps_mod, NULL);
}

/* Modo de apresentação como a DXVK: o jogo pede "sem vsync" (D3DPRESENT_INTERVAL_IMMEDIATE) ->
 * MAILBOX, senão IMMEDIATE; com vsync, FIFO. RE5VK_PRESENT=fifo|mailbox|immediate força.
 * Medido no PC (Radeon Vega, janela): FIFO forçado travava o menu em ~33 fps. Pelo menos 3 imagens. */
static re5_vkn_swap_req swap_req(const vk_device *d)
{
    re5_vkn_swap_req r = {{VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR}, 3,
                          d->pp.BackBufferWidth, d->pp.BackBufferHeight};
    const char *want = getenv("RE5VK_PRESENT");
    if (want && !strcmp(want, "mailbox")) r.modes[0] = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (want && !strcmp(want, "immediate")) r.modes[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
#if defined(__ANDROID__)
    /* Android: FIFO com 3 imagens mesmo com o jogo pedindo "sem vsync". O A54 ficava em MAILBOX com
     * 5 imagens (o carregador do Android faz MAILBOX trocando o intervalo da BufferQueue), com o
     * vkQueuePresentKHR preso até a GPU acabar o quadro. RE5VK_PRESENT=mailbox volta ao anterior. */
    else if (0) {
#else
    else if (!(want && !strcmp(want, "fifo")) && d->pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) {
#endif
        r.modes[0] = VK_PRESENT_MODE_MAILBOX_KHR;
        r.modes[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return r;
}

/* Swapchain feita pelo contexto (vk-bootstrap, transformação identidade como a DXVK). Sem
 * TRANSFER_DST nas imagens (Adreno), o Present desenha em vez de copiar (present_pipe_create). */
static void swapchain_adopt(vk_device *d, const re5_vkn_swapchain *sc)
{
    d->swap = sc->swapchain;
    d->swap_format = sc->format;
    d->swap_extent = sc->extent;
    d->nimages = sc->count;
    d->swap_has_transfer_dst = sc->transfer_dst;
    for (uint32_t i = 0; i < sc->count; i++) {
        d->images[i] = sc->images[i];
        d->image_views[i] = sc->views[i];   /* do contexto: não destruir aqui */
        d->swap_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        if (d->done[i]) continue;
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(d->dev, &si, NULL, &d->done[i]));
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(d->phys, d->swap_format, &fp);
    d->can_blit_to_swap = d->swap_has_transfer_dst && ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0);
    present_pipe_create(d);
    RE5_INFO("re5vk", "swapchain %ux%u, formato %d, %u imagens, %s (apresentação: %s)", d->swap_extent.width,
             d->swap_extent.height, (int)d->swap_format, d->nimages,
             sc->mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : sc->mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO",
             d->can_blit_to_swap ? "blit" : "render");
}

/* Android: o buffer da janela no tamanho do back buffer (ANativeWindow_setBuffersGeometry), como a DXVK.
 * O compositor escala para a tela sem custo de GPU; antes a swapchain era 2340x1080 e cada Present
 * fazia um blit de 800x600 para ela (A54, 20/11/2026). RE5VK_SWAP_FULL=1 volta ao tamanho da tela. */
static void frame_rate_hint(void *anw)
{
#if defined(__ANDROID__)
    re5_android_frame_rate(anw);
#else
    (void)anw;
#endif
}

static void window_geometry(vk_device *d)
{
#if defined(__ANDROID__)
    static int full = -1;
    static int (*set_geom)(void *, int32_t, int32_t, int32_t);
    if (full < 0) {
        const char *e = getenv("RE5VK_SWAP_FULL");
        full = e && *e == '1';
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        set_geom = lib ? (int (*)(void *, int32_t, int32_t, int32_t))dlsym(lib, "ANativeWindow_setBuffersGeometry") : NULL;
    }
    frame_rate_hint(d->anw);
    if (full || !set_geom || !d->anw || !d->pp.BackBufferWidth || !d->pp.BackBufferHeight) return;
    int r = set_geom(d->anw, (int32_t)d->pp.BackBufferWidth, (int32_t)d->pp.BackBufferHeight, 0);
    RE5_INFO("re5vk", "buffer da janela %ux%u (o compositor escala para a tela): %s", d->pp.BackBufferWidth,
             d->pp.BackBufferHeight, r == 0 ? "ok" : "recusado");
#else
    (void)d;
#endif
}

/* Recria (device ocioso). 0 = ok; -1 = a superfície não deixou agora (tenta no próximo Present). */
static int swapchain_create(vk_device *d)
{
    window_geometry(d);
    re5_vkn_swap_req r = swap_req(d);
    re5_vkn_swapchain sc;
    if (re5_vkn_swapchain_recreate(&r, &sc) != 0) {
        RE5_WARN("re5vk", "swapchain não recriada agora; tenta no próximo Present");
        return -1;
    }
    swapchain_adopt(d, &sc);
    return 0;
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
        device_idle(d);
        pcache_save_ex(d, 1);
        RE5_INFO("re5vk", "device liberado depois de %llu Present | TBDR passes=%u STORE=%u (col=%u z=%u st=%u) DONT_CARE=%u (col=%u z=%u st=%u) disc=%u reusd=%u fallback=%u splits=%u mismatches=%u est_band_saved=%.2f MB",
                 (unsigned long long)d->presents,
                 d->tbdr.stats.total_passes,
                 d->tbdr.stats.total_store, d->tbdr.stats.store_color, d->tbdr.stats.store_depth, d->tbdr.stats.store_stencil,
                 d->tbdr.stats.total_dont_care, d->tbdr.stats.dont_care_color, d->tbdr.stats.dont_care_depth, d->tbdr.stats.dont_care_stencil,
                 d->tbdr.stats.stat_discarded, d->tbdr.stats.stat_reused, d->tbdr.stats.stat_conservative_fallback,
                 d->tbdr.stats.stat_splits, d->tbdr.stats.stat_mismatches,
                 (double)d->tbdr.stats.bytes_saved_est / (1024.0 * 1024.0));
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
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
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
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- thread de apresentação (RE5VK_PRESENT_THREAD, padrão 1 no Android, 0 no PC) ----
 * No A54 (Mali-G68, JM) o vkQueuePresentKHR só volta quando a GPU acaba o quadro (390-440 ms/s), e
 * a thread de render do jogo ficava presa nele: CPU e GPU se revezavam (quadro = CPU + GPU, a GPU
 * via buracos e o governador ficava no clock mínimo). Aqui o present vai para outra thread e o jogo
 * monta o quadro N+1 enquanto a GPU faz o N. Um present pendente no máximo: o Present seguinte
 * espera o anterior antes do vkAcquireNextImageKHR (a swapchain não aceita acquire e present ao
 * mesmo tempo, e um acquire bloqueado com presents na fila poderia não voltar). A fila fica sob
 * d->qmu (vkQueueSubmit de um re5vk_flush no meio do quadro). */
static void present_worker(void *arg)
{
    vk_device *d = (vk_device *)arg;
    re5_mutex_lock(&d->pq.mu);
    for (;;) {
        while (!d->pq.pending) re5_cond_wait_ms(&d->pq.cv, &d->pq.mu, 0xFFFFFFFFu);
        uint32_t idx = d->pq.idx;
        VkSemaphore w = d->pq.wait;
        re5_mutex_unlock(&d->pq.mu);
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &w,
            .swapchainCount = 1,
            .pSwapchains = &d->swap,
            .pImageIndices = &idx,
        };
        uint64_t t0 = re5_stat_now_ns();
        re5_mutex_lock(&d->qmu);
        VkResult r = vkQueuePresentKHR(d->queue, &pi);
        re5_mutex_unlock(&d->qmu);
        uint64_t dt = re5_stat_now_ns() - t0;
        if (r < 0 && r != VK_ERROR_OUT_OF_DATE_KHR) re5_fatal("re5vk", "vkQueuePresentKHR -> %s", re5vk_result(r));
        re5_mutex_lock(&d->pq.mu);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) d->pq.stale = 1;
        d->pq.ns += dt;
        d->pq.pending = 0;
        re5_cond_broadcast(&d->pq.cv);
    }
}

static void present_thread_init(vk_device *d)
{
    const char *e = getenv("RE5VK_PRESENT_THREAD");
#if defined(__ANDROID__)
    d->pq.on = !(e && *e == '0');
#else
    d->pq.on = e && *e == '1';
#endif
    re5_mutex_init(&d->pq.mu);
    re5_cond_init(&d->pq.cv);
}

/* Espera o present pendente. Devolve 1 se a swapchain ficou velha (OUT_OF_DATE na thread). */
static int present_drain(vk_device *d)
{
    if (!d->pq.on || !d->pq.started) return 0;
    uint64_t t0 = tm_now(d);
    re5_mutex_lock(&d->pq.mu);
    while (d->pq.pending) re5_cond_wait_ms(&d->pq.cv, &d->pq.mu, 0xFFFFFFFFu);
    int stale = d->pq.stale;
    d->pq.stale = 0;
    if (d->tm.on) {   /* qpresent_ms = a espera do jogo; o tempo na thread vai para pq_ns */
        d->tm.qpresent += re5_stat_now_ns() - t0;
        d->tm.pq_ns += d->pq.ns;
    }
    d->pq.ns = 0;
    re5_mutex_unlock(&d->pq.mu);
    return stale;
}

/* vkDeviceWaitIdle exige a fila livre: sem present pendente e sem envio concorrente. */
static void device_idle(vk_device *d)
{
    present_drain(d);
    re5_mutex_lock(&d->qmu);
    vkDeviceWaitIdle(d->dev);
    re5_mutex_unlock(&d->qmu);
}

static void present_push(vk_device *d, uint32_t idx, VkSemaphore wait)
{
    re5_mutex_lock(&d->pq.mu);
    if (!d->pq.started) {
        d->pq.started = 1;
        if (re5_host_thread_start(present_worker, d, 256 * 1024) != 0) re5_fatal("re5vk", "thread de present não criada");
        RE5_INFO("re5vk", "present numa thread própria (RE5VK_PRESENT_THREAD=0 desliga)");
    }
    d->pq.idx = idx;
    d->pq.wait = wait;
    d->pq.pending = 1;
    re5_cond_broadcast(&d->pq.cv);
    re5_mutex_unlock(&d->pq.mu);
}

/* Quadros em voo antes de o Present esperar a GPU (RE5VK_LATENCY, padrão 2 no Android, 3 no PC). */
static int latency(void)
{
    static int v;
    if (!v) {
        const char *e = getenv("RE5VK_LATENCY");
#if defined(__ANDROID__)
        v = e && atoi(e) > 0 ? atoi(e) : 2;
#else
        v = e && atoi(e) > 0 ? atoi(e) : 3;
#endif
        if (v > VK_SLOTS - 2) v = VK_SLOTS - 2;
#if defined(__ANDROID__)
        /* RE5_QUERY_LAG=n (d3d9.c, padrão 3 no Android) dá como pronto o evento emitido n Present
         * atrás sem perguntar à GPU, e o jogo então reusa a geometria com Lock(NOOVERWRITE). Isso só
         * é verdade se o Present n-1 depois do Issue já esperou o envio do evento: o Present espera o
         * envio (serial atual - latência), e o serial do Issue é no máximo o do Present seguinte
         * menos 1, então a latência tem que ser <= n-1. Com lag 3 e latência 2, o pipelining mantém
         * a GPU cheia sem risco de corrida de memória. RE5VK_LATENCY_UNSAFE=1 tira o limite. */
        const char *lg = getenv("RE5_QUERY_LAG"), *un = getenv("RE5VK_LATENCY_UNSAFE");
        int lag = lg ? atoi(lg) : 3;
        if (lag > 0 && !(un && *un == '1') && v > lag - 1) {
            int cap = lag - 1 > 0 ? lag - 1 : 1;
            if (lag == 1)   /* lag 1: o evento do quadro corrente nunca pode ser dado como pronto */
                RE5_WARN("re5vk", "RE5_QUERY_LAG=1 não é seguro com NOOVERWRITE; use 0 ou >= 2");
            RE5_INFO("re5vk", "RE5VK_LATENCY %d -> %d (RE5_QUERY_LAG=%d)", v, cap, lag);
            v = cap;
        }
#endif
    }
    return v;
}

static VkSurfaceKHR create_surface(VkInstance inst, void *win);

/* RE5VK_GPU_PAD=<n> (EXPERIMENTAL, gasta bateria): n limpezas de uma imagem 1024x1024 RGBA8 no fim
 * de cada quadro. No A54 o governador da GPU (Interactive) fica em ~230-260 MHz com 67-72% de uso
 * e só sobe com carga contínua; a ideia é encher os intervalos para ele subir o clock e o quadro
 * real ficar mais rápido. 0 (padrão) desliga. */
static void gpu_pad(vk_device *d, VkCommandBuffer cb)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("RE5VK_GPU_PAD");
        n = e ? atoi(e) : 0;
        if (n < 0) n = 0;
        if (n) RE5_WARN("re5vk", "RE5VK_GPU_PAD=%d: carga de preenchimento na GPU (experimental)", n);
    }
    if (!n) return;
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!d->pad_img) {
        VkImageCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1024, 1024, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        d->pad_img = re5_vkn_image_create(&ci, "re5vk.gpu_pad", &d->pad_alloc);
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = d->pad_img, .subresourceRange = rr};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    for (int i = 0; i < n; i++) {
        VkClearColorValue c = {.float32 = {(float)(i & 1), 0.5f, 0.25f, 1.0f}};
        vkCmdClearColorImage(cb, d->pad_img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rr);
    }
}

#if defined(__ANDROID__)
/* Voltou do segundo plano (d3d9_host.c): superfície e swapchain na janela nova. Com o mutex. */
static void surface_follow(vk_device *d)
{
    uint32_t gen = re5_android_surface_gen();
    if (gen == d->surf_gen) return;
    void *w = re5_android_current_window();
    if (!w) return;   /* ainda sem superfície: tenta no próximo Present */
    re5vk_flush(d, 1);
    device_idle(d);
    d->anw = w;
    window_geometry(d);
    re5_vkn_swap_req r = swap_req(d);
    re5_vkn_swapchain sc;
    if (re5_vkn_surface_recreate(create_surface, w, &r, &sc, &d->surface) != 0) {
        RE5_WARN("re5vk", "superfície nova não criada agora; tenta no próximo Present");
        return;
    }
    swapchain_adopt(d, &sc);
    d->surf_gen = gen;
    RE5_INFO("re5vk", "janela nova depois do segundo plano: superfície e swapchain recriadas");
}
#endif

static HRESULT STDMETHODCALLTYPE v_Present(IDirect3DDevice9 *self, const RECT *src, const RECT *dst, HWND over,
                                           const RGNDATA *dirty)
{
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    re5vk_end_rendering(d);
#if defined(__ANDROID__)
    surface_follow(d);
#endif
    vk_image *bb = d->backbuffer->img;
    if (bb->tbdr_subs)
        re5vk_tbdr_mark_read(&d->tbdr, &bb->tbdr_subs[0], RE5VK_TBDR_COLOR);
#if !defined(__ANDROID__)
    /* A janela mudou de tamanho (tela cheia, Reset, arrastar): a swapchain acompanha, como na DXVK.
     * No Android pula a consulta por quadro (chamada Binder síncrona ao SurfaceFlinger); a recriação
     * ocorre via VK_ERROR_OUT_OF_DATE_KHR. */
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surface, &caps) == VK_SUCCESS &&
        caps.currentExtent.width != 0xFFFFFFFFu) {
        if (!caps.currentExtent.width || !caps.currentExtent.height) {   /* minimizada: nada a apresentar */
            re5vk_tbdr_end_frame(&d->tbdr);
            re5vk_unlock(d);
            return D3D_OK;
        }
        if (caps.currentExtent.width != d->swap_extent.width || caps.currentExtent.height != d->swap_extent.height) {
            re5vk_flush(d, 1);
            device_idle(d);
            swapchain_create(d);
        }
    }
#endif
    VkCommandBuffer cb = re5vk_cmd(d);
    gpu_pad(d, cb);
    VkSemaphore acq = d->acquired[d->cur];
    uint32_t idx = 0;
    if (present_drain(d)) {   /* a thread de present viu OUT_OF_DATE */
        re5vk_flush(d, 1);
        device_idle(d);
        swapchain_create(d);
        re5vk_tbdr_end_frame(&d->tbdr);
        re5vk_unlock(d);
        return D3D_OK;
    }
    uint64_t ta = tm_now(d);
    VkResult r = vkAcquireNextImageKHR(d->dev, d->swap, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    if (d->tm.on) d->tm.acquire += re5_stat_now_ns() - ta;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        re5vk_flush(d, 1);
        device_idle(d);
        swapchain_create(d);
        re5vk_tbdr_end_frame(&d->tbdr);
        re5vk_unlock(d);
        return D3D_OK;
    }
    if (r < 0 && r != VK_SUBOPTIMAL_KHR) re5_fatal("re5vk", "vkAcquireNextImageKHR -> %s", re5vk_result(r));
    VkImage img = d->images[idx];
    int gen = re5vk_layout_general();
    if (d->can_blit_to_swap) {
        VkImageLayout bb_src = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        re5vk_img_transition(d, cb, bb, 0, 1, 0, 1, bb_src, 0);
        image_barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {(int32_t)bb->w, (int32_t)bb->h, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0}, {(int32_t)d->swap_extent.width, (int32_t)d->swap_extent.height, 1}},
        };
        vkCmdBlitImage(cb, bb->image, bb_src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        image_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    } else {
        /* Render fallback para Snapdragon / Adreno e GPUs sem TRANSFER_DST no swapchain */
        VkImageLayout bb_ro = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        re5vk_img_transition(d, cb, bb, 0, 1, 0, 1, bb_ro, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        VkDescriptorImageInfo dii = {
            .sampler = d->present_sampler,
            .imageView = bb->view,
            .imageLayout = bb_ro,
        };
        VkWriteDescriptorSet w = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = d->present_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dii,
        };
        if (d->present_view != bb->view || d->present_img_layout != bb_ro) {
            vkUpdateDescriptorSets(d->dev, 1, &w, 0, NULL);
            d->present_view = bb->view;
            d->present_img_layout = bb_ro;
        }
        image_barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        /* O desenho cobre a tela inteira: DONT_CARE na carga, STORE para apresentar. */
        re5_vkn_pass pass = {.ncolor = 1};
        pass.color[0] = (re5_vkn_att){d->swap_format, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                      VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkRenderPass rp = re5_vkn_render_pass(&pass);
        VkRenderPassBeginInfo rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp,
            .framebuffer = re5_vkn_framebuffer(&pass, rp, &d->image_views[idx], 1, d->swap_extent),
            .renderArea = {{0, 0}, d->swap_extent},
        };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->present_pipe);
        VkViewport v = {0.0f, 0.0f, (float)d->swap_extent.width, (float)d->swap_extent.height, 0.0f, 1.0f};
        vkCmdSetViewport(cb, 0, 1, &v);
        VkRect2D sc = {{0, 0}, d->swap_extent};
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->present_layout, 0, 1, &d->present_set, 0, NULL);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        image_barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
    }
    d->swap_layouts[idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bb->last_use = d->serial;
    re5vk_tbdr_end_frame(&d->tbdr);
    static uint32_t s_tbdr_frame = 0;
    if (d->tbdr.enabled && (++s_tbdr_frame % 120 == 0)) {
        static int s_dbg = -1;
        if (s_dbg < 0) {
            const char *e = getenv("RE5VK_TBDR_DEBUG");
            s_dbg = (e && *e == '1') ? 1 : 0;
        }
        if (s_dbg) {
            RE5_INFO("re5vk", "TBDR [frame %u]: passes=%u STORE=%u (col=%u z=%u st=%u) DONT_CARE=%u (col=%u z=%u st=%u) disc=%u reusd=%u fallback=%u splits=%u mismatches=%u saved=%.2f MB",
                     s_tbdr_frame,
                     d->tbdr.stats.total_passes,
                     d->tbdr.stats.total_store, d->tbdr.stats.store_color, d->tbdr.stats.store_depth, d->tbdr.stats.store_stencil,
                     d->tbdr.stats.total_dont_care, d->tbdr.stats.dont_care_color, d->tbdr.stats.dont_care_depth, d->tbdr.stats.dont_care_stencil,
                     d->tbdr.stats.stat_discarded, d->tbdr.stats.stat_reused, d->tbdr.stats.stat_conservative_fallback,
                     d->tbdr.stats.stat_splits, d->tbdr.stats.stat_mismatches,
                     (double)d->tbdr.stats.bytes_saved_est / (1024.0 * 1024.0));
        }
    }
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
    /* vkQueuePresentKHR com o mutex: a fila do Vulkan exige acesso exclusivo, e outra thread pode
     * estar num re5vk_flush (Lock de buffer, GetRenderTargetData). */
    if (d->pq.on) {
        present_push(d, idx, d->done[idx]);
        r = VK_SUCCESS;
        if (d->tm.on) d->tm.presents++;
    } else {
        uint64_t tp = tm_now(d);
        re5_mutex_lock(&d->qmu);
        r = vkQueuePresentKHR(d->queue, &pi);
        re5_mutex_unlock(&d->qmu);
        if (d->tm.on) {
            d->tm.qpresent += re5_stat_now_ns() - tp;
            d->tm.presents++;
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        device_idle(d);
        swapchain_create(d);
    } else if (r < 0) {
        re5_fatal("re5vk", "vkQueuePresentKHR -> %s", re5vk_result(r));
    }
    slot_begin(d, (prev + 1) % VK_SLOTS);
    re5vk_unlock(d);
    /* Limite de quadros em voo. A espera é com o mutex: fora dele, outra thread pode reusar o slot
     * (vkResetFences no slot_begin) enquanto esta espera a fence, e a espera nunca termina. */
    if (this_serial > (uint64_t)latency()) {
        re5vk_lock(d);
        uint64_t want = this_serial - (uint64_t)latency();
        uint64_t f0 = d->tm.fence;
        for (int i = 0; i < VK_SLOTS; i++)
            if (d->slots[i].serial && d->slots[i].serial <= want && i != d->cur) slot_wait(d, &d->slots[i]);
        /* a espera do limite de quadros em voo sai do fence_ms e vai para o em_voo_ms */
        d->tm.inflight += d->tm.fence - f0;
        d->tm.fence = f0;
        re5vk_unlock(d);
    }
    if (d->tm.on) {
        re5vk_lock(d);
        timing_tick(d);
        re5vk_unlock(d);
    }
    if (++d->presents == 1) RE5_INFO("re5vk", "primeiro Present");
    if (d->presents % 300 == 0) pcache_save(d); /* ~10-40 s: o Android mata o processo sem Release */
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_Reset(IDirect3DDevice9 *self, D3DPRESENT_PARAMETERS *pp)
{
    vk_device *d = (vk_device *)self;
    if (!pp) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    re5vk_flush(d, 1);
    device_idle(d);
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
    int resized = d->pp.BackBufferWidth != pp->BackBufferWidth || d->pp.BackBufferHeight != pp->BackBufferHeight;
    d->pp = *pp;
#if defined(__ANDROID__)
    if (resized) swapchain_create(d);   /* o buffer da janela acompanha o back buffer (window_geometry) */
#else
    (void)resized;
#endif
    backbuffer_create(d);
    pp->BackBufferWidth = d->pp.BackBufferWidth;
    pp->BackBufferHeight = d->pp.BackBufferHeight;
    re5vk_state_reset(d);
    re5vk_unlock(d);
    return D3D_OK;
}

static struct IDirect3DDevice9Vtbl g_dev_vtbl;

/* ---- VkPipelineCache persistente ----
 * Sem ele cada sessão recompila no driver todos os pipelines (shader DXSO→SPIR-V→ISA da GPU),
 * o que no celular é o engasgo de "primeira vez que aparece um efeito". O blob vai para
 * RE5VK_PCACHE ou, por padrão, re5vk.pcache na pasta interna do app (Android) / na pasta atual.
 * O cabeçalho (VkPipelineCacheHeaderVersionOne: vendor, device, UUID) é conferido aqui antes de
 * entregar ao driver: alguns drivers de celular tropeçam em blob de outro driver em vez de
 * ignorá-lo como manda a especificação. RE5VK_PCACHE=0 desliga. */
static const char *pcache_path(void)
{
    static char path[1024];
    static int done;
    if (done) return path[0] ? path : NULL;
    done = 1;
    const char *e = getenv("RE5VK_PCACHE");
    if (e && !strcmp(e, "0")) return NULL;
    if (e && *e) {
        snprintf(path, sizeof path, "%s", e);
        return path;
    }
#if defined(__ANDROID__)
    const char *base = SDL_AndroidGetInternalStoragePath();
    if (!base) return NULL;
    snprintf(path, sizeof path, "%s/re5vk.pcache", base);
#else
    snprintf(path, sizeof path, "re5vk.pcache");
#endif
    return path;
}

static void *pcache_load(vk_device *d, size_t *out_n)
{
    *out_n = 0;
    const char *path = pcache_path();
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n >= 32 && n < (256L << 20) ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (!b) return NULL;
    uint32_t hlen, hver, vendor, device;
    memcpy(&hlen, b, 4); memcpy(&hver, b + 4, 4); memcpy(&vendor, b + 8, 4); memcpy(&device, b + 12, 4);
    if (hlen < 32 || hver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vendor != d->props.vendorID ||
        device != d->props.deviceID || memcmp(b + 16, d->props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        RE5_INFO("re5vk", "cache de pipelines em %s é de outro driver: começando do zero", path);
        free(b);
        return NULL;
    }
    RE5_INFO("re5vk", "cache de pipelines: %ld bytes de %s", n, path);
    *out_n = (size_t)n;
    return b;
}

/* Grava se houve pipeline novo desde a última vez (arquivo temporário + rename: um processo morto
 * no meio — o Android mata sem aviso — não deixa blob truncado). */
/* Escrita do arquivo numa thread própria: no Present ela travava o quadro (2 a 17 MB em disco a cada
 * 300 Present enquanto surgiam pipelines novos; A54, 20/11/2026). Uma escrita por vez. */
typedef struct { void *b; size_t n; uint32_t npipes; char path[1024]; } pcache_job;
static int g_pcache_busy;

static void pcache_write(void *p)
{
    pcache_job *j = p;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(j->b, 1, j->n, f) == j->n;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(j->path); /* rename do Windows não sobrescreve */
        ok = rename(tmp, j->path) == 0;
    }
    if (ok) RE5_INFO("re5vk", "cache de pipelines gravado: %llu bytes, %u pipelines", (unsigned long long)j->n, j->npipes);
    else RE5_WARN("re5vk", "não consegui gravar o cache de pipelines em %s", j->path);
    free(j->b);
    free(j);
    __atomic_store_n(&g_pcache_busy, 0, __ATOMIC_RELEASE);
}

/* wait = 1 no Release do device (o processo pode acabar logo depois): grava nesta thread. */
static void pcache_save_ex(vk_device *d, int wait)
{
    const char *path = pcache_path();
    if (!path || d->npipes == d->pcache_saved) return;
    if (__atomic_exchange_n(&g_pcache_busy, 1, __ATOMIC_ACQ_REL)) {
        if (!wait) return;   /* outra escrita em andamento: fica para a próxima */
        while (__atomic_load_n(&g_pcache_busy, __ATOMIC_ACQUIRE)) { /* a escrita de fundo termina logo */ }
        __atomic_store_n(&g_pcache_busy, 1, __ATOMIC_RELEASE);
    }
    size_t n = 0;
    pcache_job *j = NULL;
    if (vkGetPipelineCacheData(d->dev, d->pcache, &n, NULL) == VK_SUCCESS && n && (j = calloc(1, sizeof *j)) &&
        (j->b = malloc(n)) && vkGetPipelineCacheData(d->dev, d->pcache, &n, j->b) == VK_SUCCESS) {
        j->n = n;
        j->npipes = d->npipes;
        snprintf(j->path, sizeof j->path, "%s", path);
        d->pcache_saved = d->npipes;
        if (wait || re5_host_thread_start(pcache_write, j, 256 * 1024) != 0) pcache_write(j);
        return;
    }
    if (j) free(j->b);
    free(j);
    __atomic_store_n(&g_pcache_busy, 0, __ATOMIC_RELEASE);
}

static void pcache_save(vk_device *d) { pcache_save_ex(d, 0); }

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
    size_t n = 0;
    void *blob = pcache_load(d, &n);
    cci.initialDataSize = n;
    cci.pInitialData = blob;
    VK_CHECK(vkCreatePipelineCache(d->dev, &cci, NULL, &d->pcache));
    free(blob);
}

/* Superfície da janela do jogo, criada pelo contexto (vk-bootstrap) na hora certa. */
static VkSurfaceKHR create_surface(VkInstance inst, void *win)
{
    VkSurfaceKHR s = VK_NULL_HANDLE;
#if defined(_WIN32)
    PFN_vkCreateWin32SurfaceKHR f = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
    VkWin32SurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                       .hinstance = GetModuleHandleA(NULL), .hwnd = (HWND)win};
    if (!f || f(inst, &sci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
#elif defined(__ANDROID__)
    PFN_vkCreateAndroidSurfaceKHR f =
        (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(inst, "vkCreateAndroidSurfaceKHR");
    VkAndroidSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                         .window = (struct ANativeWindow *)win};
    if (!f || f(inst, &sci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
#else
    (void)inst;
    (void)win;
#endif
    return s;
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
    re5_mutex_init(&d->qmu);
    present_thread_init(d);
    d->cp = (D3DDEVICE_CREATION_PARAMETERS){adapter, type, focus, flags};
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;
    d->pp = *pp;

    /* Instância, superfície, GPU, device e swapchain: VulkanContext (vk-bootstrap), com EDS1+EDS2
     * obrigatórios (o re5vk põe cull, profundidade, stencil e topologia SÓ como estado dinâmico) e a
     * memória pelo VMA. Falhou: a DXVK assume (d_CreateDevice). */
    re5_vkn_swap_req sreq = swap_req(d);
    re5_vkn_device vd;
    re5_vkn_swapchain sc;
    d->anw = (void *)win;
#if defined(__ANDROID__)
    d->surf_gen = re5_android_surface_gen();
#endif
    window_geometry(d);
    if (re5_vkn_open(create_surface, (void *)win, &sreq, &vd, &sc) != 0) {
        RE5_WARN("re5vk", "o renderizador nativo não subiu nesta GPU (motivo acima)");
        free(d);
        return D3DERR_NOTAVAILABLE;
    }
    d->inst = vd.instance;
    d->phys = vd.physical;
    d->dev = vd.device;
    d->surface = vd.surface;
    d->queue = vd.queue;
    d->qfam = vd.queue_family;
#define VK_LOAD_INSTANCE(name)                                                                                     \
    if (!(name = (PFN_##name)vkGetInstanceProcAddr(d->inst, #name))) re5_fatal("re5vk", "sem %s", #name);
    VK_INSTANCE_FNS(VK_LOAD_INSTANCE)
    vkGetPhysicalDeviceProperties(d->phys, &d->props);
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memprops);
    d->core = vd.core;
    d->has_eds1 = !d->core;
    d->has_eds2 = d->core ? 0 : vd.has_eds2;
    RE5_INFO("re5vk", "estado dinâmico: %s", d->core ? "só núcleo 1.0 (modo Vulkan 1.1, RE5VK_CORE=0 volta ao EDS)"
                                                        : d->has_eds2 ? "EDS1+EDS2" : "EDS1");
#if defined(__ANDROID__)
    d->has_fmt_list = vd.image_format_list;
#endif
    const char *zc_env = getenv("RE5VK_ZEROCOPY");
    d->zerocopy = zc_env && *zc_env == '1' && vd.host_import;
    RE5_INFO("re5vk", "buffers dinâmicos na memória do guest (Unlock sem cópia): %s",
             d->zerocopy ? "sim" : !(zc_env && *zc_env == '1') ? "não (RE5VK_ZEROCOPY=1 liga)" : "não (sem VK_EXT_external_memory_host)");

#define VK_LOAD_DEVICE(name)                                                                                           \
    do {                                                                                                               \
        /* nome do núcleo (1.3) ou da extensão (KHR/EXT, drivers 1.1/1.2 de celular) */                              \
        name = (PFN_##name)vd.gdpa(d->dev, #name);                                                                    \
        if (!name) name = (PFN_##name)vd.gdpa(d->dev, #name "KHR");                                                    \
        if (!name) name = (PFN_##name)vd.gdpa(d->dev, #name "EXT");                                                    \
        if (!name) {                                                                                                   \
            if ((!d->has_eds2 && (!strcmp(#name, "vkCmdSetDepthBiasEnable") ||                                     \
                                  !strcmp(#name, "vkCmdSetPrimitiveRestartEnable") ||                              \
                                  !strcmp(#name, "vkCmdSetRasterizerDiscardEnable"))) ||                           \
                (d->core && (!strncmp(#name, "vkCmdSet", 8) || !strcmp(#name, "vkCmdBindVertexBuffers2")))) {      \
                /* EDS2 opcional; no modo núcleo as funções de EDS não são chamadas (NULL se faltarem) */          \
            } else {                                                                                                   \
                re5_fatal("re5vk", "sem %s", #name);                                                                   \
            }                                                                                                          \
        }                                                                                                              \
    } while (0);
    VK_DEVICE_FNS(VK_LOAD_DEVICE)
    d->features = vd.features;
    RE5_INFO("re5vk", "GPU '%s' (Vulkan %u.%u.%u, driver 0x%X), fila %u, BC %s, ETC2 %s, ASTC %s, anisotropia %s", d->props.deviceName,
             VK_API_VERSION_MAJOR(vd.api_version), VK_API_VERSION_MINOR(vd.api_version),
             VK_API_VERSION_PATCH(vd.api_version), d->props.driverVersion, d->qfam,
             vd.features.textureCompressionBC ? "sim" : "não",
             vd.features.textureCompressionETC2 ? "sim" : "não",
             vd.features.textureCompressionASTC_LDR ? "sim" : "não",
             vd.features.samplerAnisotropy ? "sim" : "não");

    layout_create(d);
    {
        const char *fl = getenv("RE5_FPS_LOG");
        d->tm.on = fl && *fl && *fl != '0';
    }
    {
        /* Desligado por padrão (2026-11-11): ele prevê pelo quadro anterior e só aprende que um anexo
         * era lido depois de já tê-lo descartado; quando a ordem dos passes muda (passe dividido por
         * textura nova, sombra que não é refeita todo quadro) sai profundidade/cor de lixo. É o mesmo
         * DONT_CARE na profundidade reaproveitada já revertido em 07/11 (STATUS). RE5VK_TBDR=1 liga. */
        const char *tbdr_e = getenv("RE5VK_TBDR");
        int tbdr_on = tbdr_e && *tbdr_e == '1';
        re5vk_tbdr_init(&d->tbdr, tbdr_on);
        RE5_INFO("re5vk", "oráculo TBDR (STORE_OP_DONT_CARE): %s", tbdr_on ? "ligado (RE5VK_TBDR=1)" : "desligado");
    }
    slots_create(d);
    swapchain_adopt(d, &sc);
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
