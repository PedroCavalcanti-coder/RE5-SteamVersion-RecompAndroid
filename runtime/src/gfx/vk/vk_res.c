/*
 * vk_res.c - recursos do backend re5vk: formatos, imagens, superfícies, texturas, buffers,
 * declarações de vértice, shaders e consultas (ver vk_core.h).
 *
 * Texturas: imagem do Vulkan + cópia na CPU por nível/face (como o pool MANAGED do D3D9), que o
 * LockRect entrega; o UnlockRect sobe só o retângulo travado, pelo staging do envio atual.
 * Buffers: memória host-visible e coerente; Lock com DISCARD num buffer que a GPU ainda usa troca
 * a memória (a antiga é destruída quando o envio que a usava termina).
 */
#include <stdio.h>

#include "vk_core.h"

uint32_t re5_d3d9_shader_dwords(const uint32_t *code, uint32_t max);   /* win32/d3d9.c */

/* ---- formatos ---- */

#define SW(r, g, b, a) {VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a}
#define ID SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY)
#define FOURCC(a, b, c, d) ((D3DFORMAT)((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24))

static const vk_format k_formats[] = {
    {D3DFMT_A8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, ID, 1, 1, 4, 0, 0},
    {D3DFMT_X8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, SW(IDENTITY, IDENTITY, IDENTITY, ONE), 1, 1, 4, 0, 0},
    {D3DFMT_A8B8G8R8, VK_FORMAT_R8G8B8A8_UNORM, ID, 1, 1, 4, 0, 0},
    {D3DFMT_X8B8G8R8, VK_FORMAT_R8G8B8A8_UNORM, SW(IDENTITY, IDENTITY, IDENTITY, ONE), 1, 1, 4, 0, 0},
    {D3DFMT_R5G6B5, VK_FORMAT_R5G6B5_UNORM_PACK16, ID, 1, 1, 2, 0, 0},
    {D3DFMT_A1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, ID, 1, 1, 2, 0, 0},
    {D3DFMT_X1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, SW(IDENTITY, IDENTITY, IDENTITY, ONE), 1, 1, 2, 0, 0},
    {D3DFMT_A2B10G10R10, VK_FORMAT_A2B10G10R10_UNORM_PACK32, ID, 1, 1, 4, 0, 0},
    {D3DFMT_A2R10G10B10, VK_FORMAT_A2R10G10B10_UNORM_PACK32, ID, 1, 1, 4, 0, 0},
    {D3DFMT_L8, VK_FORMAT_R8_UNORM, SW(R, R, R, ONE), 1, 1, 1, 0, 0},
    {D3DFMT_A8, VK_FORMAT_R8_UNORM, SW(ZERO, ZERO, ZERO, R), 1, 1, 1, 0, 0},
    {D3DFMT_A8L8, VK_FORMAT_R8G8_UNORM, SW(R, R, R, G), 1, 1, 2, 0, 0},
    {D3DFMT_L16, VK_FORMAT_R16_UNORM, SW(R, R, R, ONE), 1, 1, 2, 0, 0},
    {D3DFMT_G16R16, VK_FORMAT_R16G16_UNORM, SW(R, G, ONE, ONE), 1, 1, 4, 0, 0},
    {D3DFMT_A16B16G16R16, VK_FORMAT_R16G16B16A16_UNORM, ID, 1, 1, 8, 0, 0},
    {D3DFMT_R16F, VK_FORMAT_R16_SFLOAT, SW(R, ONE, ONE, ONE), 1, 1, 2, 0, 0},
    {D3DFMT_G16R16F, VK_FORMAT_R16G16_SFLOAT, SW(R, G, ONE, ONE), 1, 1, 4, 0, 0},
    {D3DFMT_A16B16G16R16F, VK_FORMAT_R16G16B16A16_SFLOAT, ID, 1, 1, 8, 0, 0},
    {D3DFMT_R32F, VK_FORMAT_R32_SFLOAT, SW(R, ONE, ONE, ONE), 1, 1, 4, 0, 0},
    {D3DFMT_G32R32F, VK_FORMAT_R32G32_SFLOAT, SW(R, G, ONE, ONE), 1, 1, 8, 0, 0},
    {D3DFMT_A32B32G32R32F, VK_FORMAT_R32G32B32A32_SFLOAT, ID, 1, 1, 16, 0, 0},
    {D3DFMT_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, ID, 4, 4, 8, 0, 0},
    {D3DFMT_DXT2, VK_FORMAT_BC2_UNORM_BLOCK, ID, 4, 4, 16, 0, 0},
    {D3DFMT_DXT3, VK_FORMAT_BC2_UNORM_BLOCK, ID, 4, 4, 16, 0, 0},
    {D3DFMT_DXT4, VK_FORMAT_BC3_UNORM_BLOCK, ID, 4, 4, 16, 0, 0},
    {D3DFMT_DXT5, VK_FORMAT_BC3_UNORM_BLOCK, ID, 4, 4, 16, 0, 0},
    {D3DFMT_D24S8, VK_FORMAT_D24_UNORM_S8_UINT, ID, 1, 1, 4, 1, 1},
    {D3DFMT_D24X8, VK_FORMAT_D24_UNORM_S8_UINT, ID, 1, 1, 4, 1, 0},
    {D3DFMT_D24FS8, VK_FORMAT_D32_SFLOAT_S8_UINT, ID, 1, 1, 4, 1, 1},
    {D3DFMT_D16, VK_FORMAT_D16_UNORM, ID, 1, 1, 2, 1, 0},
    {D3DFMT_D16_LOCKABLE, VK_FORMAT_D16_UNORM, ID, 1, 1, 2, 1, 0},
    {D3DFMT_D32, VK_FORMAT_D32_SFLOAT, ID, 1, 1, 4, 1, 0},
    {D3DFMT_D32F_LOCKABLE, VK_FORMAT_D32_SFLOAT, ID, 1, 1, 4, 1, 0},
    {FOURCC('I', 'N', 'T', 'Z'), VK_FORMAT_D24_UNORM_S8_UINT, ID, 1, 1, 4, 1, 1},
};

const vk_format *re5vk_format(D3DFORMAT f)
{
    for (size_t i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++)
        if (k_formats[i].d3d == f) return &k_formats[i];
    return NULL;
}

/* ---- imagens ---- */

static int format_ok(vk_device *d, VkFormat f, VkFormatFeatureFlags need)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}

static VkImageAspectFlags aspect_of(const vk_format *f, int for_view)
{
    if (!f->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    if (for_view) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (f->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

int re5vk_image_create(vk_device *d, vk_image *im, VkImageType type, const vk_format *fmt, uint32_t w, uint32_t h,
                       uint32_t depth, uint32_t levels, uint32_t layers, int cube, int rt, int ds)
{
    memset(im, 0, sizeof *im);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (rt) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if (ds) need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkFormat vf = fmt->vk;
    if (!format_ok(d, vf, need)) {
        /* D24S8 não existe na AMD: D32F+S8, como a DXVK */
        if (vf == VK_FORMAT_D24_UNORM_S8_UINT && format_ok(d, VK_FORMAT_D32_SFLOAT_S8_UINT, need))
            vf = VK_FORMAT_D32_SFLOAT_S8_UINT;
        else
            re5_fatal("re5vk", "formato D3D %u (Vulkan %d) sem suporte para %s nesta GPU", (unsigned)fmt->d3d, (int)vf,
                      ds ? "profundidade" : rt ? "render target" : "textura");
    }
    if (!levels) {
        uint32_t m = w > h ? w : h;
        if (depth > m) m = depth;
        levels = 1;
        while (m > 1) { m >>= 1; levels++; }
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!ds || format_ok(d, vf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (rt) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ds) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = type,
        .format = vf,
        .extent = {w, h, depth},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(d->dev, &ci, NULL, &im->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d->dev, im->image, &req);
    im->mem = re5vk_mem_alloc(d, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VK_CHECK(vkBindImageMemory(d->dev, im->image, im->mem.mem, im->mem.off));
    im->fmt = fmt;
    im->vkf = vf;
    im->w = w;
    im->h = h;
    im->d = depth;
    im->levels = levels;
    im->layers = layers;
    im->type = type;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = im->image,
            .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                                                                                 : VK_IMAGE_VIEW_TYPE_2D,
            .format = vf,
            .components = fmt->swz,
            .subresourceRange = {aspect_of(fmt, 1), 0, levels, 0, layers},
        };
        VK_CHECK(vkCreateImageView(d->dev, &vi, NULL, &im->view));
    }
    im->rt_views = calloc((size_t)levels * layers, sizeof *im->rt_views);
    /* tudo em GENERAL desde o começo (vk_core.h) */
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->image,
        .subresourceRange = {aspect_of(fmt, 0), 0, levels, 0, layers},
    };
    re5vk_end_rendering(d);
    VkCommandBuffer cb = re5vk_cmd(d);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
    /* Conteúdo inicial zerado, como a DXVK e os drivers D3D9 entregam. Sem isto a imagem nasce
     * com o que havia na memória: num alvo FP16 da cadeia de luminância do HDR, um NaN de lixo
     * entra na adaptação de exposição (que mistura o quadro anterior) e nunca mais sai - a cena
     * 3D fica preta enquanto menus e vídeo, que desenham direto no back buffer, saem certos.
     * Formato comprimido (BC) não aceita clear; ele sempre chega pelo upload. */
    if (fmt->bw == 1 && fmt->bh == 1) {
        VkImageSubresourceRange all = {aspect_of(fmt, 0), 0, levels, 0, layers};
        if (fmt->depth) {
            VkClearDepthStencilValue z = {0.0f, 0};
            vkCmdClearDepthStencilImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
        } else {
            VkClearColorValue z = {{0.0f, 0.0f, 0.0f, 0.0f}};
            vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
        }
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                             0, NULL);
    }
    return 0;
}

VkImageView re5vk_image_rt_view(vk_device *d, vk_image *im, uint32_t layer, uint32_t level)
{
    VkImageView *v = &im->rt_views[layer * im->levels + level];
    if (*v) return *v;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = im->vkf,
        .subresourceRange = {aspect_of(im->fmt, 0), level, 1, layer, 1},
    };
    VK_CHECK(vkCreateImageView(d->dev, &vi, NULL, v));
    return *v;
}

void re5vk_image_destroy(vk_device *d, vk_image *im)
{
    if (!im->image) return;
    for (uint32_t i = 0; i < im->levels * im->layers; i++)
        if (im->rt_views && im->rt_views[i]) re5vk_destroy_later(d, 3, (uint64_t)(uintptr_t)im->rt_views[i], (vk_alloc){0});
    if (im->view) re5vk_destroy_later(d, 3, (uint64_t)(uintptr_t)im->view, (vk_alloc){0});
    re5vk_destroy_later(d, 2, (uint64_t)(uintptr_t)im->image, im->mem);
    free(im->rt_views);
    memset(im, 0, sizeof *im);
}

/* Sobe a região [x, y, z] x [w, h, depth] de `data` (linhas de `pitch` bytes, fatias de
 * `slice` bytes) para o nível/camada da imagem. */
static void upload(vk_device *d, vk_image *im, uint32_t layer, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                   uint32_t w, uint32_t h, uint32_t depth, const uint8_t *data, uint32_t pitch, uint32_t slice)
{
    const vk_format *f = im->fmt;
    uint32_t bw = f->bw, bh = f->bh;
    uint32_t row = (w + bw - 1) / bw * f->bytes, rows = (h + bh - 1) / bh;
    VkDeviceSize size = (VkDeviceSize)row * rows * depth;
    VkBuffer buf;
    VkDeviceSize off;
    re5vk_end_rendering(d);
    uint8_t *p = re5vk_stage(d, size, f->bytes, &buf, &off);
    for (uint32_t s = 0; s < depth; s++)
        for (uint32_t r = 0; r < rows; r++)
            memcpy(p + ((size_t)s * rows + r) * row, data + (size_t)s * slice + (size_t)r * pitch, row);
    VkCommandBuffer cb = re5vk_cmd(d);
    re5vk_barrier_all(cb);
    VkBufferImageCopy c = {
        .bufferOffset = off,
        .bufferRowLength = (w + bw - 1) / bw * bw,
        .bufferImageHeight = rows * bh,
        .imageSubresource = {aspect_of(f, 1), level, layer, 1},
        .imageOffset = {(int32_t)x, (int32_t)y, (int32_t)z},
        .imageExtent = {w, h, depth},
    };
    vkCmdCopyBufferToImage(cb, buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    re5vk_barrier_all(cb);
    im->last_use = d->serial;
}

/* ---- tabelas de métodos ---- */

static struct IDirect3DSurface9Vtbl g_vt_surface;
static struct IDirect3DTexture9Vtbl g_vt_texture;
static struct IDirect3DCubeTexture9Vtbl g_vt_cube;
static struct IDirect3DVolumeTexture9Vtbl g_vt_volume;
static struct IDirect3DVertexBuffer9Vtbl g_vt_vb;
static struct IDirect3DIndexBuffer9Vtbl g_vt_ib;
static struct IDirect3DVertexDeclaration9Vtbl g_vt_decl;
static struct IDirect3DVertexShader9Vtbl g_vt_vs;
static struct IDirect3DPixelShader9Vtbl g_vt_ps;
static struct IDirect3DQuery9Vtbl g_vt_query;

static void fill_nyi(void *vt, size_t size)
{
    void **s = vt;
    for (size_t i = 0; i < size / sizeof(void *); i++) s[i] = (void *)re5vk_nyi;
}

/* ---- IUnknown e IDirect3DResource9 comuns ---- */

static vk_obj *refs_owner(vk_obj *o)
{
    /* superfície de uma textura: a contagem é a da textura (como no D3D9) */
    if (o->kind == VKO_SURFACE && ((vk_surface *)o)->container) return ((vk_surface *)o)->container;
    return o;
}

void re5vk_addref(vk_obj *o)
{
    if (o) refs_owner(o)->refs++;
}

static HRESULT STDMETHODCALLTYPE o_QueryInterface(void *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    /* A ponte só pergunta pela própria interface ou por uma base: devolve o mesmo objeto. */
    re5vk_addref(refs_owner(self));
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE o_AddRef(void *self)
{
    vk_obj *o = refs_owner(self);
    return (ULONG)++o->refs;
}
static ULONG STDMETHODCALLTYPE o_Release(void *self)
{
    vk_obj *o = refs_owner(self);
    long n = o->refs - 1;
    re5vk_release(o);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE o_GetDevice(void *self, IDirect3DDevice9 **out)
{
    vk_obj *o = self;
    if (!out) return D3DERR_INVALIDCALL;
    o->dev->refs++;
    *out = (IDirect3DDevice9 *)o->dev;
    return D3D_OK;
}
static DWORD STDMETHODCALLTYPE o_SetPriority(void *self, DWORD p) { return 0; }
static DWORD STDMETHODCALLTYPE o_GetPriority(void *self) { return 0; }
static void STDMETHODCALLTYPE o_PreLoad(void *self) {}

static void tex_destroy(vk_texture *t);

void re5vk_release(vk_obj *o)
{
    if (!o) return;
    o = refs_owner(o);
    if (--o->refs > 0) return;
    vk_device *d = o->dev;
    re5vk_lock(d);
    switch (o->kind) {
    case VKO_SURFACE: {
        vk_surface *s = (vk_surface *)o;
        if (s->own_img) {
            re5vk_image_destroy(d, s->img);
            free(s->img);
        }
        free(s->lock);
        free(s);
        break;
    }
    case VKO_TEXTURE: case VKO_CUBE: case VKO_VOLUME: tex_destroy((vk_texture *)o); break;
    case VKO_VB: case VKO_IB: {
        vk_buffer *b = (vk_buffer *)o;
        re5vk_destroy_later(d, 1, (uint64_t)(uintptr_t)b->buf, b->mem);
        free(b);
        break;
    }
    case VKO_VS: case VKO_PS: {
        vk_shader *s = (vk_shader *)o;
        for (int i = 0; i < s->nv; i++) {
            vkDestroyShaderModule(d->dev, s->v[i].mod, NULL);   /* os pipelines já criados continuam válidos */
            free(s->v[i].info.spv);
        }
        free(s->code);
        free(s);
        break;
    }
    default: free(o); break;
    }
    re5vk_unlock(d);
}

static void obj_init(vk_obj *o, const void *vt, vk_device *d, int kind)
{
    o->lpVtbl = vt;
    o->refs = 1;
    o->dev = d;
    o->kind = kind;
}

/* ---- superfícies ---- */

vk_surface *re5vk_surface_new(vk_device *d, vk_obj *container, vk_image *img, int own, uint32_t layer, uint32_t level,
                              D3DFORMAT format, DWORD usage, D3DPOOL pool)
{
    vk_surface *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("re5vk", "sem memória");
    obj_init(&s->o, &g_vt_surface, d, VKO_SURFACE);
    s->container = container;
    s->img = img;
    s->own_img = own;
    s->layer = layer;
    s->level = level;
    uint32_t w = img->w >> level, h = img->h >> level;
    s->desc = (D3DSURFACE_DESC){format, D3DRTYPE_SURFACE, usage, pool, D3DMULTISAMPLE_NONE, 0, w ? w : 1, h ? h : 1};
    return s;
}

static D3DRESOURCETYPE STDMETHODCALLTYPE s_GetType(IDirect3DSurface9 *self) { return D3DRTYPE_SURFACE; }
static HRESULT STDMETHODCALLTYPE s_GetDesc(IDirect3DSurface9 *self, D3DSURFACE_DESC *desc)
{
    if (!desc) return D3DERR_INVALIDCALL;
    *desc = ((vk_surface *)self)->desc;
    return D3D_OK;
}

/* Superfície de memória do sistema (CreateOffscreenPlainSurface + GetRenderTargetData): só
 * CPU, o que basta para o RE5_D3D9_SHOT da ponte. */
static HRESULT STDMETHODCALLTYPE s_LockRect(IDirect3DSurface9 *self, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    vk_surface *s = (vk_surface *)self;
    if (!lr) return D3DERR_INVALIDCALL;
    if (s->img) re5_fatal("re5vk", "LockRect de superfície na GPU sem implementação");
    const vk_format *f = re5vk_format(s->desc.Format);
    uint32_t pitch = s->desc.Width * f->bytes;
    lr->Pitch = (INT)pitch;
    lr->pBits = s->lock + (r ? (size_t)r->top * pitch + (size_t)r->left * f->bytes : 0);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE s_UnlockRect(IDirect3DSurface9 *self) { return D3D_OK; }

HRESULT STDMETHODCALLTYPE re5vk_CreateOffscreenPlainSurface(IDirect3DDevice9 *self, UINT w, UINT h, D3DFORMAT format,
                                                            D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *sh)
{
    const vk_format *f = re5vk_format(format);
    if (!out || sh || !f || f->bw != 1) return D3DERR_INVALIDCALL;
    if (pool != D3DPOOL_SYSTEMMEM) re5_fatal("re5vk", "CreateOffscreenPlainSurface fora de D3DPOOL_SYSTEMMEM");
    vk_surface *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("re5vk", "sem memória");
    obj_init(&s->o, &g_vt_surface, (vk_device *)self, VKO_SURFACE);
    s->desc = (D3DSURFACE_DESC){format, D3DRTYPE_SURFACE, 0, pool, D3DMULTISAMPLE_NONE, 0, w, h};
    s->lock = calloc((size_t)w * h, f->bytes);
    if (!s->lock) re5_fatal("re5vk", "sem memória");
    *out = (IDirect3DSurface9 *)s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_GetRenderTargetData(IDirect3DDevice9 *self, IDirect3DSurface9 *rt,
                                                    IDirect3DSurface9 *dst)
{
    vk_device *d = (vk_device *)self;
    vk_surface *s = (vk_surface *)rt, *t = (vk_surface *)dst;
    if (!s || !t || !s->img || t->img || s->desc.Width != t->desc.Width || s->desc.Height != t->desc.Height)
        return D3DERR_INVALIDCALL;
    const vk_format *f = s->img->fmt;
    VkDeviceSize size = (VkDeviceSize)s->desc.Width * s->desc.Height * f->bytes;
    re5vk_lock(d);
    re5vk_end_rendering(d);
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t *p = re5vk_stage(d, size, 16, &buf, &off);
    VkCommandBuffer cb = re5vk_cmd(d);
    re5vk_barrier_all(cb);
    VkBufferImageCopy c = {.bufferOffset = off,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->level, s->layer, 1},
                           .imageExtent = {s->desc.Width, s->desc.Height, 1}};
    vkCmdCopyImageToBuffer(cb, s->img->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    re5vk_barrier_all(cb);
    re5vk_flush(d, 1);   /* espera a GPU: o staging do envio anterior só volta a ser usado 8 envios depois */
    memcpy(t->lock, p, (size_t)size);
    re5vk_unlock(d);
    return D3D_OK;
}

/* ---- texturas ---- */

static vk_texture *tex_new(vk_device *d, int kind, const void *vt, D3DFORMAT format, DWORD usage, D3DPOOL pool,
                           uint32_t w, uint32_t h, uint32_t depth, uint32_t levels)
{
    const vk_format *f = re5vk_format(format);
    if (!f) re5_fatal("re5vk", "CreateTexture: formato %u (0x%08X) sem tradução", (unsigned)format, (unsigned)format);
    if (usage & D3DUSAGE_AUTOGENMIPMAP) re5_fatal("re5vk", "D3DUSAGE_AUTOGENMIPMAP sem implementação");
    vk_texture *t = calloc(1, sizeof *t);
    if (!t) re5_fatal("re5vk", "sem memória");
    obj_init(&t->o, vt, d, kind);
    t->format = format;
    t->usage = usage;
    t->pool = pool;
    int cube = kind == VKO_CUBE;
    re5vk_image_create(d, &t->img, kind == VKO_VOLUME ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, f, w, h, depth, levels,
                       cube ? 6 : 1, cube, !!(usage & D3DUSAGE_RENDERTARGET), !!(usage & D3DUSAGE_DEPTHSTENCIL));
    size_t n = (size_t)t->img.levels * t->img.layers;
    t->surf = calloc(n, sizeof *t->surf);
    t->locks = calloc(n, sizeof *t->locks);
    if (!t->surf || !t->locks) re5_fatal("re5vk", "sem memória");
    return t;
}

static void tex_destroy(vk_texture *t)
{
    vk_device *d = t->o.dev;
    size_t n = (size_t)t->img.levels * t->img.layers;
    for (size_t i = 0; i < n; i++) {
        free(t->surf[i]);   /* sem referência própria: vivem com a textura */
        free(t->locks[i].data);
    }
    re5vk_image_destroy(d, &t->img);
    free(t->surf);
    free(t->locks);
    free(t);
}

static vk_surface *tex_surface(vk_texture *t, uint32_t layer, uint32_t level)
{
    size_t i = (size_t)layer * t->img.levels + level;
    if (!t->surf[i])
        t->surf[i] = re5vk_surface_new(t->o.dev, &t->o, &t->img, 0, layer, level, t->format, t->usage, t->pool);
    return t->surf[i];
}

static void level_desc(vk_texture *t, uint32_t level, D3DSURFACE_DESC *d)
{
    uint32_t w = t->img.w >> level, h = t->img.h >> level;
    *d = (D3DSURFACE_DESC){t->format, D3DRTYPE_SURFACE, t->usage, t->pool, D3DMULTISAMPLE_NONE, 0, w ? w : 1, h ? h : 1};
}

/* Bytes por linha e linhas (em blocos) de um nível. */
static void level_layout(vk_texture *t, uint32_t level, uint32_t *pitch, uint32_t *rows, uint32_t *slices)
{
    const vk_format *f = t->img.fmt;
    uint32_t w = t->img.w >> level, h = t->img.h >> level, dd = t->img.d >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!dd) dd = 1;
    *pitch = (w + f->bw - 1) / f->bw * f->bytes;
    *rows = (h + f->bh - 1) / f->bh;
    *slices = dd;
}

static HRESULT tex_lock(vk_texture *t, uint32_t layer, uint32_t level, D3DLOCKED_RECT *lr, D3DLOCKED_BOX *lb,
                        const RECT *rect, const D3DBOX *box, DWORD flags)
{
    if (level >= t->img.levels || layer >= t->img.layers) return D3DERR_INVALIDCALL;
    if (t->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        re5_fatal("re5vk", "LockRect de render target/profundidade sem implementação");
    vk_device *d = t->o.dev;
    re5vk_lock(d);
    size_t i = (size_t)layer * t->img.levels + level;
    uint32_t pitch, rows, slices;
    level_layout(t, level, &pitch, &rows, &slices);
    if (!t->locks[i].data) {
        t->locks[i].data = calloc((size_t)pitch * rows * slices, 1);
        if (!t->locks[i].data) re5_fatal("re5vk", "sem memória para travar %u bytes", pitch * rows * slices);
    }
    const vk_format *f = t->img.fmt;
    uint32_t w = t->img.w >> level, h = t->img.h >> level, dd = t->img.d >> level;
    RECT r = rect ? *rect : (RECT){0, 0, (LONG)(w ? w : 1), (LONG)(h ? h : 1)};
    D3DBOX bx = box ? *box : (D3DBOX){0, 0, w ? w : 1, h ? h : 1, 0, dd ? dd : 1};
    if (lb) r = (RECT){(LONG)bx.Left, (LONG)bx.Top, (LONG)bx.Right, (LONG)bx.Bottom};
    t->locks[i].r = r;
    t->locks[i].box = bx;
    t->locks[i].on = !(flags & D3DLOCK_READONLY);
    size_t off = (size_t)(r.top / f->bh) * pitch + (size_t)(r.left / f->bw) * f->bytes;
    if (lb) {
        off += (size_t)bx.Front * pitch * rows;
        lb->RowPitch = (INT)pitch;
        lb->SlicePitch = (INT)(pitch * rows);
        lb->pBits = t->locks[i].data + off;
    } else {
        lr->Pitch = (INT)pitch;
        lr->pBits = t->locks[i].data + off;
    }
    re5vk_unlock(d);
    return D3D_OK;
}

static HRESULT tex_unlock(vk_texture *t, uint32_t layer, uint32_t level)
{
    if (level >= t->img.levels || layer >= t->img.layers) return D3DERR_INVALIDCALL;
    vk_device *d = t->o.dev;
    re5vk_lock(d);
    size_t i = (size_t)layer * t->img.levels + level;
    if (t->locks[i].on && t->locks[i].data) {
        const vk_format *f = t->img.fmt;
        uint32_t pitch, rows, slices;
        level_layout(t, level, &pitch, &rows, &slices);
        RECT r = t->locks[i].r;
        D3DBOX bx = t->locks[i].box;
        /* região alinhada aos blocos (DXT) e limitada ao nível */
        uint32_t lw = t->img.w >> level, lh = t->img.h >> level;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        uint32_t x = (uint32_t)r.left / f->bw * f->bw, y = (uint32_t)r.top / f->bh * f->bh;
        uint32_t x2 = (uint32_t)r.right, y2 = (uint32_t)r.bottom;
        if (x2 > lw) x2 = lw;
        if (y2 > lh) y2 = lh;
        uint32_t z = t->img.type == VK_IMAGE_TYPE_3D ? bx.Front : 0;
        uint32_t nz = t->img.type == VK_IMAGE_TYPE_3D ? bx.Back - bx.Front : 1;
        const uint8_t *src = t->locks[i].data + (size_t)z * pitch * rows + (size_t)(y / f->bh) * pitch +
                             (size_t)(x / f->bw) * f->bytes;
        upload(d, &t->img, layer, level, x, y, z, x2 - x, y2 - y, nz, src, pitch, pitch * rows);
        t->locks[i].on = 0;
    }
    re5vk_unlock(d);
    return D3D_OK;
}

static D3DRESOURCETYPE STDMETHODCALLTYPE t_GetType(IDirect3DTexture9 *self) { return D3DRTYPE_TEXTURE; }
static DWORD STDMETHODCALLTYPE t_GetLevelCount(IDirect3DTexture9 *self) { return ((vk_texture *)self)->img.levels; }
static HRESULT STDMETHODCALLTYPE t_GetLevelDesc(IDirect3DTexture9 *self, UINT level, D3DSURFACE_DESC *desc)
{
    vk_texture *t = (vk_texture *)self;
    if (!desc || level >= t->img.levels) return D3DERR_INVALIDCALL;
    level_desc(t, level, desc);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE t_GetSurfaceLevel(IDirect3DTexture9 *self, UINT level, IDirect3DSurface9 **out)
{
    vk_texture *t = (vk_texture *)self;
    if (!out || level >= t->img.levels) return D3DERR_INVALIDCALL;
    t->o.refs++;
    *out = (IDirect3DSurface9 *)tex_surface(t, 0, level);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE t_LockRect(IDirect3DTexture9 *self, UINT level, D3DLOCKED_RECT *lr, const RECT *r,
                                            DWORD flags)
{
    if (!lr) return D3DERR_INVALIDCALL;
    return tex_lock((vk_texture *)self, 0, level, lr, NULL, r, NULL, flags);
}
static HRESULT STDMETHODCALLTYPE t_UnlockRect(IDirect3DTexture9 *self, UINT level)
{
    return tex_unlock((vk_texture *)self, 0, level);
}

static D3DRESOURCETYPE STDMETHODCALLTYPE x_GetType(IDirect3DCubeTexture9 *self) { return D3DRTYPE_CUBETEXTURE; }
static DWORD STDMETHODCALLTYPE x_GetLevelCount(IDirect3DCubeTexture9 *self) { return ((vk_texture *)self)->img.levels; }
static HRESULT STDMETHODCALLTYPE x_GetLevelDesc(IDirect3DCubeTexture9 *self, UINT level, D3DSURFACE_DESC *desc)
{
    vk_texture *t = (vk_texture *)self;
    if (!desc || level >= t->img.levels) return D3DERR_INVALIDCALL;
    level_desc(t, level, desc);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE x_GetCubeMapSurface(IDirect3DCubeTexture9 *self, D3DCUBEMAP_FACES face, UINT level,
                                                     IDirect3DSurface9 **out)
{
    vk_texture *t = (vk_texture *)self;
    if (!out || level >= t->img.levels || (uint32_t)face >= 6) return D3DERR_INVALIDCALL;
    t->o.refs++;
    *out = (IDirect3DSurface9 *)tex_surface(t, (uint32_t)face, level);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE x_LockRect(IDirect3DCubeTexture9 *self, D3DCUBEMAP_FACES face, UINT level,
                                            D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    if (!lr) return D3DERR_INVALIDCALL;
    return tex_lock((vk_texture *)self, (uint32_t)face, level, lr, NULL, r, NULL, flags);
}
static HRESULT STDMETHODCALLTYPE x_UnlockRect(IDirect3DCubeTexture9 *self, D3DCUBEMAP_FACES face, UINT level)
{
    return tex_unlock((vk_texture *)self, (uint32_t)face, level);
}

static D3DRESOURCETYPE STDMETHODCALLTYPE w_GetType(IDirect3DVolumeTexture9 *self) { return D3DRTYPE_VOLUMETEXTURE; }
static DWORD STDMETHODCALLTYPE w_GetLevelCount(IDirect3DVolumeTexture9 *self) { return ((vk_texture *)self)->img.levels; }
static HRESULT STDMETHODCALLTYPE w_GetLevelDesc(IDirect3DVolumeTexture9 *self, UINT level, D3DVOLUME_DESC *desc)
{
    vk_texture *t = (vk_texture *)self;
    if (!desc || level >= t->img.levels) return D3DERR_INVALIDCALL;
    uint32_t w = t->img.w >> level, h = t->img.h >> level, dd = t->img.d >> level;
    *desc = (D3DVOLUME_DESC){t->format, D3DRTYPE_VOLUME, t->usage, t->pool, w ? w : 1, h ? h : 1, dd ? dd : 1};
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE w_LockBox(IDirect3DVolumeTexture9 *self, UINT level, D3DLOCKED_BOX *lb,
                                           const D3DBOX *box, DWORD flags)
{
    if (!lb) return D3DERR_INVALIDCALL;
    return tex_lock((vk_texture *)self, 0, level, NULL, lb, NULL, box, flags);
}
static HRESULT STDMETHODCALLTYPE w_UnlockBox(IDirect3DVolumeTexture9 *self, UINT level)
{
    return tex_unlock((vk_texture *)self, 0, level);
}

HRESULT STDMETHODCALLTYPE re5vk_CreateTexture(IDirect3DDevice9 *self, UINT w, UINT h, UINT levels, DWORD usage,
                                              D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    *out = (IDirect3DTexture9 *)tex_new(d, VKO_TEXTURE, &g_vt_texture, format, usage, pool, w, h, 1, levels);
    re5vk_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateCubeTexture(IDirect3DDevice9 *self, UINT edge, UINT levels, DWORD usage,
                                                  D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **out,
                                                  HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    *out = (IDirect3DCubeTexture9 *)tex_new(d, VKO_CUBE, &g_vt_cube, format, usage, pool, edge, edge, 1, levels);
    re5vk_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateVolumeTexture(IDirect3DDevice9 *self, UINT w, UINT h, UINT depth, UINT levels,
                                                    DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                    IDirect3DVolumeTexture9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    *out = (IDirect3DVolumeTexture9 *)tex_new(d, VKO_VOLUME, &g_vt_volume, format, usage, pool, w, h, depth, levels);
    re5vk_unlock(d);
    return D3D_OK;
}

static HRESULT surface_create(vk_device *d, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, int depth,
                              IDirect3DSurface9 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    if (ms > D3DMULTISAMPLE_NONMASKABLE) re5_fatal("re5vk", "superfície com MSAA %u sem implementação", (unsigned)ms);
    const vk_format *f = re5vk_format(format);
    if (!f) re5_fatal("re5vk", "superfície com formato %u (0x%08X) sem tradução", (unsigned)format, (unsigned)format);
    re5vk_lock(d);
    vk_image *im = calloc(1, sizeof *im);
    if (!im) re5_fatal("re5vk", "sem memória");
    re5vk_image_create(d, im, VK_IMAGE_TYPE_2D, f, w, h, 1, 1, 1, 0, !depth, depth);
    *out = (IDirect3DSurface9 *)re5vk_surface_new(d, NULL, im, 1, 0, 0, format,
                                                  depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    re5vk_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateRenderTarget(IDirect3DDevice9 *self, UINT w, UINT h, D3DFORMAT format,
                                                   D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lockable,
                                                   IDirect3DSurface9 **out, HANDLE *sh)
{
    if (sh) return D3DERR_INVALIDCALL;
    RE5_DEBUG("re5vk", "CreateRenderTarget %ux%u formato %u", w, h, (unsigned)format);
    return surface_create((vk_device *)self, w, h, format, ms, 0, out);
}

HRESULT STDMETHODCALLTYPE re5vk_CreateDepthStencilSurface(IDirect3DDevice9 *self, UINT w, UINT h, D3DFORMAT format,
                                                          D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard,
                                                          IDirect3DSurface9 **out, HANDLE *sh)
{
    if (sh) return D3DERR_INVALIDCALL;
    RE5_DEBUG("re5vk", "CreateDepthStencilSurface %ux%u formato %u", w, h, (unsigned)format);
    return surface_create((vk_device *)self, w, h, format, ms, 1, out);
}

/* ---- buffers ---- */

static void buffer_alloc(vk_device *d, vk_buffer *b)
{
    VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = b->size ? b->size : 4,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VK_CHECK(vkCreateBuffer(d->dev, &ci, NULL, &b->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->dev, b->buf, &req);
    b->mem = re5vk_mem_alloc(d, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VK_CHECK(vkBindBufferMemory(d->dev, b->buf, b->mem.mem, b->mem.off));
}

static vk_buffer *buffer_new(vk_device *d, int kind, const void *vt, UINT size, DWORD usage, D3DPOOL pool)
{
    vk_buffer *b = calloc(1, sizeof *b);
    if (!b) re5_fatal("re5vk", "sem memória");
    obj_init(&b->o, vt, d, kind);
    b->size = size;
    b->usage = usage;
    b->pool = pool;
    buffer_alloc(d, b);
    memset(b->mem.map, 0, b->size);
    return b;
}

static HRESULT buffer_lock(vk_buffer *b, UINT off, UINT size, void **pp, DWORD flags)
{
    if (!pp || off > b->size) return D3DERR_INVALIDCALL;
    vk_device *d = b->o.dev;
    re5vk_lock(d);
    re5vk_poll(d);
    if (b->last_use > d->done_serial) {   /* a GPU ainda pode ler o conteúdo atual */
        if (flags & D3DLOCK_DISCARD) {
            re5vk_destroy_later(d, 1, (uint64_t)(uintptr_t)b->buf, b->mem);
            buffer_alloc(d, b);
            b->last_use = 0;
        } else if (!(flags & D3DLOCK_NOOVERWRITE) && !(flags & D3DLOCK_READONLY)) {
            re5vk_wait_serial(d, b->last_use);
        }
    }
    *pp = b->mem.map + off;
    b->locked = 1;
    re5vk_unlock(d);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE b_Lock(IDirect3DVertexBuffer9 *self, UINT off, UINT size, void **pp, DWORD flags)
{
    return buffer_lock((vk_buffer *)self, off, size, pp, flags);
}
static HRESULT STDMETHODCALLTYPE b_Unlock(IDirect3DVertexBuffer9 *self)
{
    ((vk_buffer *)self)->locked = 0;   /* memória coerente: nada a copiar */
    return D3D_OK;
}
static D3DRESOURCETYPE STDMETHODCALLTYPE vb_GetType(IDirect3DVertexBuffer9 *self) { return D3DRTYPE_VERTEXBUFFER; }
static D3DRESOURCETYPE STDMETHODCALLTYPE ib_GetType(IDirect3DIndexBuffer9 *self) { return D3DRTYPE_INDEXBUFFER; }
static HRESULT STDMETHODCALLTYPE vb_GetDesc(IDirect3DVertexBuffer9 *self, D3DVERTEXBUFFER_DESC *desc)
{
    vk_buffer *b = (vk_buffer *)self;
    if (!desc) return D3DERR_INVALIDCALL;
    *desc = (D3DVERTEXBUFFER_DESC){D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, b->usage, b->pool, b->size, b->fvf};
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE ib_GetDesc(IDirect3DIndexBuffer9 *self, D3DINDEXBUFFER_DESC *desc)
{
    vk_buffer *b = (vk_buffer *)self;
    if (!desc) return D3DERR_INVALIDCALL;
    *desc = (D3DINDEXBUFFER_DESC){b->format, D3DRTYPE_INDEXBUFFER, b->usage, b->pool, b->size};
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateVertexBuffer(IDirect3DDevice9 *self, UINT size, DWORD usage, DWORD fvf,
                                                   D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    vk_buffer *b = buffer_new(d, VKO_VB, &g_vt_vb, size, usage, pool);
    b->fvf = fvf;
    b->format = D3DFMT_VERTEXDATA;
    *out = (IDirect3DVertexBuffer9 *)b;
    re5vk_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateIndexBuffer(IDirect3DDevice9 *self, UINT size, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    vk_buffer *b = buffer_new(d, VKO_IB, &g_vt_ib, size, usage, pool);
    b->format = format;
    *out = (IDirect3DIndexBuffer9 *)b;
    re5vk_unlock(d);
    return D3D_OK;
}

/* ---- declarações de vértice ---- */

static HRESULT STDMETHODCALLTYPE dl_GetDeclaration(IDirect3DVertexDeclaration9 *self, D3DVERTEXELEMENT9 *el, UINT *n)
{
    vk_decl *dl = (vk_decl *)self;
    if (!n) return D3DERR_INVALIDCALL;
    if (el) {
        memcpy(el, dl->el, dl->n * sizeof *el);
        el[dl->n] = (D3DVERTEXELEMENT9){0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0};
    }
    *n = dl->n + 1;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateVertexDeclaration(IDirect3DDevice9 *self, const D3DVERTEXELEMENT9 *el,
                                                        IDirect3DVertexDeclaration9 **out)
{
    if (!el || !out) return D3DERR_INVALIDCALL;
    vk_decl *dl = calloc(1, sizeof *dl);
    if (!dl) re5_fatal("re5vk", "sem memória");
    obj_init(&dl->o, &g_vt_decl, (vk_device *)self, VKO_DECL);
    while (el[dl->n].Stream != 0xFF && dl->n < 63) {
        dl->el[dl->n] = el[dl->n];
        dl->n++;
    }
    *out = (IDirect3DVertexDeclaration9 *)dl;
    return D3D_OK;
}

/* ---- shaders ---- */

static HRESULT STDMETHODCALLTYPE sh_GetFunction(IDirect3DVertexShader9 *self, void *data, UINT *size)
{
    vk_shader *s = (vk_shader *)self;
    if (!size) return D3DERR_INVALIDCALL;
    if (data) memcpy(data, s->code, s->ndw * 4u);
    *size = s->ndw * 4u;
    return D3D_OK;
}

static HRESULT shader_create(vk_device *d, const DWORD *code, int pixel, void **out)
{
    if (!code || !out) return D3DERR_INVALIDCALL;
    uint32_t n = re5_d3d9_shader_dwords((const uint32_t *)code, 1u << 20);
    if (!n) re5_fatal("re5vk", "shader sem token de fim");
    vk_shader *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("re5vk", "sem memória");
    obj_init(&s->o, pixel ? (const void *)&g_vt_ps : (const void *)&g_vt_vs, d, pixel ? VKO_PS : VKO_VS);
    s->code = malloc(n * 4u);
    if (!s->code) re5_fatal("re5vk", "sem memória");
    memcpy(s->code, code, n * 4u);
    s->ndw = n;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n * 4; i++) h = (h ^ ((const uint8_t *)code)[i]) * 16777619u;
    s->hash = h;
    *out = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateVertexShader(IDirect3DDevice9 *self, const DWORD *code,
                                                   IDirect3DVertexShader9 **out)
{
    return shader_create((vk_device *)self, code, 0, (void **)out);
}

HRESULT STDMETHODCALLTYPE re5vk_CreatePixelShader(IDirect3DDevice9 *self, const DWORD *code,
                                                  IDirect3DPixelShader9 **out)
{
    return shader_create((vk_device *)self, code, 1, (void **)out);
}

HRESULT re5vk_shader_variant(vk_device *d, vk_shader *s, const re5vk_dxso_variant *var, VkShaderModule *mod,
                             const re5vk_shader **info)
{
    for (int i = 0; i < s->nv; i++) {
        if (s->v[i].var.depth_samplers == var->depth_samplers && s->v[i].var.alpha_func == var->alpha_func) {
            *mod = s->v[i].mod;
            *info = &s->v[i].info;
            return D3D_OK;
        }
    }
    if (s->nv >= VK_MAX_VARIANTS) re5_fatal("re5vk", "shader %08x com variantes demais", s->hash);
    char err[256];
    re5vk_shader *out = &s->v[s->nv].info;
    if (re5vk_dxso_compile(s->code, s->ndw, var, out, err, sizeof err) != 0)
        re5_fatal("re5vk", "shader %s_%08x: %s (RE5_D3D9_SHADER_DUMP=<pasta> guarda o bytecode)",
                  s->o.kind == VKO_PS ? "ps" : "vs", s->hash, err);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = out->nwords * 4, .pCode = out->spv};
    VK_CHECK(vkCreateShaderModule(d->dev, &ci, NULL, &s->v[s->nv].mod));
    s->v[s->nv].var = *var;
    *mod = s->v[s->nv].mod;
    *info = out;
    s->nv++;
    return D3D_OK;
}

/* ---- consultas (só evento: o RE5 sincroniza quadros com elas) ---- */

static D3DQUERYTYPE STDMETHODCALLTYPE q_GetType(IDirect3DQuery9 *self) { return ((vk_query *)self)->type; }
static DWORD STDMETHODCALLTYPE q_GetDataSize(IDirect3DQuery9 *self) { return sizeof(BOOL); }
static HRESULT STDMETHODCALLTYPE q_Issue(IDirect3DQuery9 *self, DWORD flags)
{
    vk_query *q = (vk_query *)self;
    if (flags & D3DISSUE_END) q->serial = q->o.dev->serial;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE q_GetData(IDirect3DQuery9 *self, void *data, DWORD size, DWORD flags)
{
    vk_query *q = (vk_query *)self;
    vk_device *d = q->o.dev;
    re5vk_lock(d);
    re5vk_poll(d);
    int done = !q->serial || q->serial <= d->done_serial;
    if (!done && (flags & D3DGETDATA_FLUSH) && q->serial >= d->serial) re5vk_flush(d, 0);
    re5vk_unlock(d);
    if (!done) return S_FALSE;
    if (data && size >= sizeof(BOOL)) *(BOOL *)data = TRUE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE re5vk_CreateQuery(IDirect3DDevice9 *self, D3DQUERYTYPE type, IDirect3DQuery9 **out)
{
    if (type != D3DQUERYTYPE_EVENT) {
        if (!out) return D3DERR_NOTAVAILABLE;   /* sondagem: sem suporte ainda */
        re5_fatal("re5vk", "CreateQuery tipo %u sem implementação (só D3DQUERYTYPE_EVENT)", (unsigned)type);
    }
    if (!out) return D3D_OK;
    vk_query *q = calloc(1, sizeof *q);
    if (!q) re5_fatal("re5vk", "sem memória");
    obj_init(&q->o, &g_vt_query, (vk_device *)self, VKO_QUERY);
    q->type = type;
    *out = (IDirect3DQuery9 *)q;
    return D3D_OK;
}

/* ---- tabelas ---- */

#define COMMON(vt)                                                                                                 \
    do {                                                                                                           \
        fill_nyi(&(vt), sizeof(vt));                                                                              \
        (vt).QueryInterface = (void *)o_QueryInterface;                                                            \
        (vt).AddRef = (void *)o_AddRef;                                                                            \
        (vt).Release = (void *)o_Release;                                                                          \
        (vt).GetDevice = (void *)o_GetDevice;                                                                      \
    } while (0)
#define RESOURCE(vt)                                                                                               \
    do {                                                                                                           \
        COMMON(vt);                                                                                                \
        (vt).SetPriority = (void *)o_SetPriority;                                                                  \
        (vt).GetPriority = (void *)o_GetPriority;                                                                  \
        (vt).PreLoad = (void *)o_PreLoad;                                                                          \
    } while (0)

void re5vk_res_vtables(vk_device *d)
{
    (void)d;
    if (g_vt_surface.QueryInterface) return;
    RESOURCE(g_vt_surface);
    g_vt_surface.GetType = s_GetType;
    g_vt_surface.GetDesc = s_GetDesc;
    g_vt_surface.LockRect = s_LockRect;
    g_vt_surface.UnlockRect = s_UnlockRect;

    RESOURCE(g_vt_texture);
    g_vt_texture.GetType = t_GetType;
    g_vt_texture.GetLevelCount = t_GetLevelCount;
    g_vt_texture.GetLevelDesc = t_GetLevelDesc;
    g_vt_texture.GetSurfaceLevel = t_GetSurfaceLevel;
    g_vt_texture.LockRect = t_LockRect;
    g_vt_texture.UnlockRect = t_UnlockRect;

    RESOURCE(g_vt_cube);
    g_vt_cube.GetType = x_GetType;
    g_vt_cube.GetLevelCount = x_GetLevelCount;
    g_vt_cube.GetLevelDesc = x_GetLevelDesc;
    g_vt_cube.GetCubeMapSurface = x_GetCubeMapSurface;
    g_vt_cube.LockRect = x_LockRect;
    g_vt_cube.UnlockRect = x_UnlockRect;

    RESOURCE(g_vt_volume);
    g_vt_volume.GetType = w_GetType;
    g_vt_volume.GetLevelCount = w_GetLevelCount;
    g_vt_volume.GetLevelDesc = w_GetLevelDesc;
    g_vt_volume.LockBox = w_LockBox;
    g_vt_volume.UnlockBox = w_UnlockBox;

    RESOURCE(g_vt_vb);
    g_vt_vb.GetType = vb_GetType;
    g_vt_vb.Lock = b_Lock;
    g_vt_vb.Unlock = b_Unlock;
    g_vt_vb.GetDesc = vb_GetDesc;

    RESOURCE(g_vt_ib);
    g_vt_ib.GetType = ib_GetType;
    g_vt_ib.Lock = (void *)b_Lock;
    g_vt_ib.Unlock = (void *)b_Unlock;
    g_vt_ib.GetDesc = ib_GetDesc;

    COMMON(g_vt_decl);
    g_vt_decl.GetDeclaration = dl_GetDeclaration;
    COMMON(g_vt_vs);
    g_vt_vs.GetFunction = sh_GetFunction;
    COMMON(g_vt_ps);
    g_vt_ps.GetFunction = (void *)sh_GetFunction;
    COMMON(g_vt_query);
    g_vt_query.GetType = q_GetType;
    g_vt_query.GetDataSize = q_GetDataSize;
    g_vt_query.Issue = q_Issue;
    g_vt_query.GetData = q_GetData;
}

/* Texturas nulas (o que um sampler sem textura lê no D3D9: preto opaco) e o buffer de vértices
 * zerado das entradas do VS que a declaração não tem. */
void re5vk_init_dummies(vk_device *d)
{
    const vk_format *f = re5vk_format(D3DFMT_A8B8G8R8);
    re5vk_image_create(d, &d->dummy2d, VK_IMAGE_TYPE_2D, f, 1, 1, 1, 1, 1, 0, 0, 0);
    re5vk_image_create(d, &d->dummycube, VK_IMAGE_TYPE_2D, f, 1, 1, 1, 1, 6, 1, 0, 0);
    re5vk_image_create(d, &d->dummy3d, VK_IMAGE_TYPE_3D, f, 1, 1, 1, 1, 1, 0, 0, 0);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    VkCommandBuffer cb = re5vk_cmd(d);
    re5vk_barrier_all(cb);
    all.layerCount = 1;
    vkCmdClearColorImage(cb, d->dummy2d.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    vkCmdClearColorImage(cb, d->dummy3d.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    all.layerCount = 6;
    vkCmdClearColorImage(cb, d->dummycube.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    re5vk_barrier_all(cb);
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VK_CHECK(vkCreateBuffer(d->dev, &ci, NULL, &d->zero_vb));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->dev, d->zero_vb, &req);
    d->zero_vb_mem = re5vk_mem_alloc(d, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VK_CHECK(vkBindBufferMemory(d->dev, d->zero_vb, d->zero_vb_mem.mem, d->zero_vb_mem.off));
    memset(d->zero_vb_mem.map, 0, 256);
}
