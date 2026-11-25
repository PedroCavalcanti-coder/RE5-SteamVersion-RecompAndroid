/*
 * vk_res.c - recursos do backend re5vk: formatos, imagens, superfícies, texturas, buffers,
 * declarações de vértice, shaders e consultas (ver vk2_core.h).
 *
 * Texturas: imagem do Vulkan + cópia na CPU por nível/face (como o pool MANAGED do D3D9), que o
 * LockRect entrega; o UnlockRect sobe só o retângulo travado, pelo staging do envio atual.
 * Buffers: memória host-visible e coerente; Lock com DISCARD num buffer que a GPU ainda usa troca
 * a memória (a antiga é destruída quando o envio que a usava termina).
 */
#include <stdio.h>

#include "vk2_core.h"
#include "re5_dxt_to_etc2.h"
#include "re5_tex_cache.h"
#include "vk2_etc2_pool.h"
#include "../../win32/dxt_decode.h"

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
    {FOURCC('I', 'N', 'T', 'Z'), VK_FORMAT_D24_UNORM_S8_UINT, SW(R, R, R, R), 1, 1, 4, 1, 1},
};

const vk_format *vk2_format(D3DFORMAT f)
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

/* D3DSAMP_SRGBTEXTURE: o D3D9 converte sRGB -> linear ao amostrar estes formatos. Sem isso as
 * texturas entram claras demais na iluminação e no HDR (cena estourada na fase 3 do benchmark,
 * 2026-11-04). RE5VK2_SRGB=0 desliga (a imagem deixa de ser mutável; o Mali perde AFBC com ela). */
static VkFormat srgb_pair(VkFormat f)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK2_SRGB");
        on = !(e && *e == '0');
    }
    if (!on) return VK_FORMAT_UNDEFINED;
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int vk2_layout_general(void)
{
    static int g = -1;
    if (g < 0) {
        const char *e = getenv("RE5VK2_LAYOUT_GENERAL");
        g = vk2_layout_general_choice(e, 0);
        RE5_INFO("vk2", "layouts: %s (RE5VK2_LAYOUT_GENERAL=%s)",
                 g ? "tudo em GENERAL, barreira no fim do passe (modo legado)" : "ótimos por sub-recurso (AFBC/UBWC)",
                 e && *e ? e : "padrão");
    }
    return g;
}

VkImageAspectFlags vk2_img_aspect(const vk_format *f, int for_view)
{
    if (!f->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    if (for_view) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (f->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static VkImageAspectFlags aspect_of(const vk_format *f, int for_view)
{
    return vk2_img_aspect(f, for_view);
}

VkImageLayout vk2_img_sub_layout(const vk_image *im, uint32_t layer, uint32_t level)
{
    if (!im || !im->sub_layouts) return VK_IMAGE_LAYOUT_UNDEFINED;
    return im->sub_layouts[(size_t)layer * im->levels + level];
}

static void layout_stage_src(VkImageLayout l, uint32_t read_stages, VkPipelineStageFlags *stg, VkAccessFlags *acc)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PREINITIALIZED:
        *stg = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        *acc = 0;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *acc = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *acc = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *acc = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *acc = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        /* Sempre VS+FS (alem do registrado): o registro de estagios pode ter perdido o VS (textura de
         * vertice lida dentro de um passe ja aberto) e a escrita seguinte corria com a leitura. */
        *stg = (VkPipelineStageFlags)read_stages | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *acc = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        *stg = read_stages ? (VkPipelineStageFlags)read_stages
                           : (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        *acc = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        *stg = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        *acc = 0;
        break;
    default: /* GENERAL */
        *stg = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *acc = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        break;
    }
}

static void layout_stage_dst(VkImageLayout l, VkPipelineStageFlags shader_stages, VkPipelineStageFlags *stg, VkAccessFlags *acc)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *acc = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *acc = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *acc = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stg = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *acc = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stg = shader_stages ? shader_stages
                             : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        *acc = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        *stg = shader_stages ? shader_stages
                             : (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        *acc = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        *stg = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        *acc = 0;
        break;
    default: /* GENERAL */
        *stg = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        *acc = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
               VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    }
}

int vk2_img_needs_barrier(const vk_image *im, uint32_t base_level, uint32_t level_count,
                            uint32_t base_layer, uint32_t layer_count, VkImageLayout want_layout)
{
    if (!im || !im->sub_layouts) return 0;
    for (uint32_t ly = base_layer; ly < base_layer + layer_count; ly++) {
        for (uint32_t lv = base_level; lv < base_level + level_count; lv++) {
            size_t i = (size_t)ly * im->levels + lv;
            if (im->sub_layouts[i] != want_layout || im->sub_dirty[i]) return 1;
        }
    }
    return 0;
}

void vk2_img_mark_written(vk_image *im, uint32_t base_level, uint32_t level_count,
                            uint32_t base_layer, uint32_t layer_count)
{
    if (!im || !im->sub_dirty) return;
    for (uint32_t ly = base_layer; ly < base_layer + layer_count; ly++) {
        for (uint32_t lv = base_level; lv < base_level + level_count; lv++) {
            size_t i = (size_t)ly * im->levels + lv;
            im->sub_dirty[i] = 1;
            im->sub_stages[i] = 0;
        }
    }
}

void vk2_img_transition(vk_device *d, VkCommandBuffer cb, vk_image *im, uint32_t base_level, uint32_t level_count,
                          uint32_t base_layer, uint32_t layer_count, VkImageLayout new_layout,
                          VkPipelineStageFlags shader_stages)
{
    (void)d;
    if (!im || !im->sub_layouts || !level_count || !layer_count) return;
    VkImageAspectFlags asp = aspect_of(im->fmt, 0);
    int is_ro = (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                 new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    /* Caminho rápido: verifica se todos os sub-recursos do intervalo partilham o mesmo estado. */
    size_t i0 = (size_t)base_layer * im->levels + base_level;
    VkImageLayout l0 = im->sub_layouts[i0];
    uint8_t d0 = im->sub_dirty[i0];
    uint32_t s0 = im->sub_stages[i0];
    int uniform = 1;
    for (uint32_t ly = base_layer; ly < base_layer + layer_count && uniform; ly++) {
        for (uint32_t lv = base_level; lv < base_level + level_count; lv++) {
            size_t idx = (size_t)ly * im->levels + lv;
            if (im->sub_layouts[idx] != l0 || im->sub_dirty[idx] != d0 || im->sub_stages[idx] != s0) {
                uniform = 0;
                break;
            }
        }
    }

    if (uniform) {
        if (l0 == new_layout && !d0) {
            if (is_ro && shader_stages) {
                for (uint32_t ly = base_layer; ly < base_layer + layer_count; ly++)
                    for (uint32_t lv = base_level; lv < base_level + level_count; lv++)
                        im->sub_stages[(size_t)ly * im->levels + lv] |= (uint32_t)shader_stages;
            }
            return;
        }
        VkPipelineStageFlags ss, ds;
        VkAccessFlags sa, da;
        layout_stage_src(l0, s0, &ss, &sa);
        layout_stage_dst(new_layout, shader_stages, &ds, &da);
        VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = sa,
            .dstAccessMask = da,
            .oldLayout = l0,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = im->image,
            .subresourceRange = {asp, base_level, level_count, base_layer, layer_count},
        };
        vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
        for (uint32_t ly = base_layer; ly < base_layer + layer_count; ly++) {
            for (uint32_t lv = base_level; lv < base_level + level_count; lv++) {
                size_t idx = (size_t)ly * im->levels + lv;
                im->sub_layouts[idx] = new_layout;
                im->sub_dirty[idx] = 0;
                im->sub_stages[idx] = is_ro ? (uint32_t)(shader_stages ? shader_stages : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) : 0u;
            }
        }
        return;
    }

    VkImageMemoryBarrier bars[64];
    uint32_t nb = 0;
    VkPipelineStageFlags ss_union = 0, ds_union = 0;
    for (uint32_t ly = base_layer; ly < base_layer + layer_count; ly++) {
        for (uint32_t lv = base_level; lv < base_level + level_count; lv++) {
            size_t idx = (size_t)ly * im->levels + lv;
            VkImageLayout old = im->sub_layouts[idx];
            if (old == new_layout && !im->sub_dirty[idx]) {
                if (is_ro && shader_stages) im->sub_stages[idx] |= (uint32_t)shader_stages;
                continue;
            }
            VkPipelineStageFlags ss, ds;
            VkAccessFlags sa, da;
            layout_stage_src(old, im->sub_stages[idx], &ss, &sa);
            layout_stage_dst(new_layout, shader_stages, &ds, &da);
            ss_union |= ss;
            ds_union |= ds;
            bars[nb++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = sa,
                .dstAccessMask = da,
                .oldLayout = old,
                .newLayout = new_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = im->image,
                .subresourceRange = {asp, lv, 1, ly, 1},
            };
            im->sub_layouts[idx] = new_layout;
            im->sub_dirty[idx] = 0;
            im->sub_stages[idx] = is_ro ? (uint32_t)(shader_stages ? shader_stages : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) : 0u;
            if (nb == 64) {
                vkCmdPipelineBarrier(cb, ss_union, ds_union, 0, 0, NULL, 0, NULL, nb, bars);
                nb = 0;
                ss_union = ds_union = 0;
            }
        }
    }
    if (nb) vkCmdPipelineBarrier(cb, ss_union, ds_union, 0, 0, NULL, 0, NULL, nb, bars);
}

int vk2_image_create(vk_device *d, vk_image *im, VkImageType type, const vk_format *fmt, uint32_t w, uint32_t h,
                       uint32_t depth, uint32_t levels, uint32_t layers, int cube, int rt, int ds)
{
    memset(im, 0, sizeof *im);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (rt) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if (ds) need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkFormat vf = fmt->vk;
    /* RE5VK2_D32=1: profundidade D24 vira D32F+S8 mesmo onde o D24 existe (como na AMD do PC).
     * Diagnóstico das manchas de sombra no Mali (D24 real), que no PC não aparecem. */
    if (vf == VK_FORMAT_D24_UNORM_S8_UINT) {
        static int force32 = -1;
        if (force32 < 0) {
            const char *e = getenv("RE5VK2_D32");
            force32 = e && *e == '1';
        }
        if (force32 && format_ok(d, VK_FORMAT_D32_SFLOAT_S8_UINT, need)) vf = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    if (fmt->bw == 4 && !rt && !ds) {
        static int said;
        if (!said) {
            said = 1;
            if (format_ok(d, vf, need)) RE5_INFO("vk2", "texturas DXT: BC nativo");
            else RE5_INFO("vk2", "texturas DXT: transcodificadas para ETC2/EAC (cache em disco, versão %u)",
                          (unsigned)RE5_TEX_CACHE_VERSION);
        }
    }
    if (!format_ok(d, vf, need)) {
        /* D24S8 não existe na AMD: D32F+S8, como a DXVK */
        if (vf == VK_FORMAT_D24_UNORM_S8_UINT && format_ok(d, VK_FORMAT_D32_SFLOAT_S8_UINT, need)) {
            vf = VK_FORMAT_D32_SFLOAT_S8_UINT;
        } else if (vf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && format_ok(d, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, need)) {
            /* Mali / mobile sem BC: transcodificação direta para ETC2 RGBA8 EAC (preserva o alpha 1-bit de folhas/cabelos) */
            vf = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        } else if (vf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && format_ok(d, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, need)) {
            /* Fallback para GPUs mobile que só tenham ETC2 RGB8 */
            vf = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        } else if ((vf == VK_FORMAT_BC2_UNORM_BLOCK || vf == VK_FORMAT_BC3_UNORM_BLOCK) &&
                   format_ok(d, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, need)) {
            /* Mali / mobile sem BC: transcodificação direta para ETC2 RGBA8 EAC (mesmo tamanho: 8 bpp) */
            vf = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        } else {
            re5_fatal("vk2", "formato D3D %u (Vulkan %d) sem suporte para %s nesta GPU", (unsigned)fmt->d3d, (int)vf,
                      ds ? "profundidade" : rt ? "render target" : "textura");
        }
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
    /* Com par sRGB: imagem mutável, com a lista dos dois formatos (o driver mantém a compressão
     * quando sabe quais vistas vão existir). */
    VkFormat sv = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !ds ? srgb_pair(vf) : VK_FORMAT_UNDEFINED;
    if (sv != VK_FORMAT_UNDEFINED && !format_ok(d, sv, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) sv = VK_FORMAT_UNDEFINED;
#if defined(__ANDROID__)
    /* MUTABLE_FORMAT desliga a compressão de framebuffer (AFBC no Mali, UBWC em parte dos Adreno)
     * justamente nos render targets, que são o grosso da banda. Só texturas comuns ganham o par sRGB;
     * RE5VK2_RT_MUTABLE=1 devolve o par aos render targets. Se um RT sem o par for amostrado com
     * D3DSAMP_SRGBTEXTURE, vk2_image_sampled_view avisa (uma vez) em vez de errar a gama calado. */
    static int rt_mut = -1;
    if (rt_mut < 0) {
        const char *e = getenv("RE5VK2_RT_MUTABLE");
        rt_mut = e && *e == '1';
    }
    if (rt && !rt_mut && sv != VK_FORMAT_UNDEFINED) {
        sv = VK_FORMAT_UNDEFINED;
        im->srgb_dropped = 1;
    }
#endif
    VkFormat views[2] = {vf, sv};
    VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                      .viewFormatCount = 2, .pViewFormats = views};
    if (sv != VK_FORMAT_UNDEFINED) {
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
#if defined(__ANDROID__)
        if (d->has_fmt_list) ci.pNext = &fl;   /* Vulkan 1.1 sem VK_KHR_image_format_list: a lista é inválida */
#else
        ci.pNext = &fl;
#endif
    }
    memset(&im->mem, 0, sizeof im->mem);
    im->image = vk2_mem_image_create(&ci, rt ? "re5vk.render_target" : ds ? "re5vk.profundidade" : "re5vk.textura",
                                     &im->mem.v);   /* vmaCreateImage */
    im->srgb = sv;
    im->fmt = fmt;
    im->vkf = vf;
    im->w = w;
    im->h = h;
    im->d = depth;
    im->levels = levels;
    im->layers = layers;
    im->type = type;
    im->id = ++d->next_img_id;
    size_t nsub = (size_t)levels * layers;
    im->sub_layouts = calloc(nsub, sizeof *im->sub_layouts);
    im->sub_dirty = calloc(nsub, sizeof *im->sub_dirty);
    im->sub_stages = calloc(nsub, sizeof *im->sub_stages);
    im->tbdr_subs = calloc(nsub, sizeof *im->tbdr_subs);
    if (!im->sub_layouts || !im->sub_dirty || !im->sub_stages || !im->tbdr_subs)
        re5_fatal("vk2", "sem memória");
    for (size_t i = 0; i < nsub; i++) vk2_tbdr_sub_reset(&im->tbdr_subs[i]);
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
    im->rt_views = calloc(nsub, sizeof *im->rt_views);
    im->attach = (uint8_t)(rt || ds);
    int gen = vk2_layout_general();
    if (gen || (fmt->bw == 1 && fmt->bh == 1)) {
        vk2_end_rendering(d);
        VkCommandBuffer cb = vk2_cmd(d);
        VkImageLayout init_layout = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vk2_img_transition(d, cb, im, 0, levels, 0, layers, init_layout, 0);
        /* Conteúdo inicial zerado, como a DXVK e os drivers D3D9 entregam. Sem isto a imagem nasce
         * com o que havia na memória: num alvo FP16 da cadeia de luminância do HDR, um NaN de lixo
         * entra na adaptação de exposição (que mistura o quadro anterior) e nunca mais sai - a cena
         * 3D fica preta enquanto menus e vídeo, que desenham direto no back buffer, saem certos.
         * Formato comprimido (BC) não aceita clear; ele sempre chega pelo upload (em UNDEFINED até o upload). */
        if (fmt->bw == 1 && fmt->bh == 1) {
            VkImageSubresourceRange all = {aspect_of(fmt, 0), 0, levels, 0, layers};
            if (fmt->depth) {
                VkClearDepthStencilValue z = {0.0f, 0};
                vkCmdClearDepthStencilImage(cb, im->image, init_layout, &z, 1, &all);
            } else {
                VkClearColorValue z = {{0.0f, 0.0f, 0.0f, 0.0f}};
                vkCmdClearColorImage(cb, im->image, init_layout, &z, 1, &all);
            }
            vk2_img_mark_written(im, 0, levels, 0, layers);
            if (!gen) {
                VkImageLayout rest_layout = rt ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : ds ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vk2_img_transition(d, cb, im, 0, levels, 0, layers, rest_layout, 0);
            }
        }
    }
    return 0;
}

VkImageView vk2_image_rt_view(vk_device *d, vk_image *im, uint32_t layer, uint32_t level)
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

VkImageView vk2_image_sampled_view(vk_device *d, vk_image *im, int srgb)
{
#if defined(__ANDROID__)
    if (srgb && im->srgb_dropped) {
        static int warned;
        if (!warned) {
            warned = 1;
            RE5_WARN("vk2", "render target %ux%u amostrado com D3DSAMP_SRGBTEXTURE sem vista sRGB (gama linear); "
                              "RE5VK2_RT_MUTABLE=1 corrige ao custo da compressão AFBC/UBWC", im->w, im->h);
        }
    }
#endif
    if (!srgb || im->srgb == VK_FORMAT_UNDEFINED) return im->view;
    if (im->view_srgb) return im->view_srgb;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->image,
        .viewType = im->layers == 6 && im->type == VK_IMAGE_TYPE_2D ? VK_IMAGE_VIEW_TYPE_CUBE
                  : im->type == VK_IMAGE_TYPE_3D                    ? VK_IMAGE_VIEW_TYPE_3D
                                                                    : VK_IMAGE_VIEW_TYPE_2D,
        .format = im->srgb,
        .components = im->fmt->swz,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, 0, im->layers},
    };
    VK_CHECK(vkCreateImageView(d->dev, &vi, NULL, &im->view_srgb));
    return im->view_srgb;
}

void vk2_image_destroy(vk_device *d, vk_image *im)
{
    if (!im->image) return;
    if (im->view_srgb) vk2_destroy_later(d, 3, (uint64_t)(uintptr_t)im->view_srgb, (vk_alloc){0});
    for (uint32_t i = 0; i < im->levels * im->layers; i++)
        if (im->rt_views && im->rt_views[i]) vk2_destroy_later(d, 3, (uint64_t)(uintptr_t)im->rt_views[i], (vk_alloc){0});
    if (im->view) vk2_destroy_later(d, 3, (uint64_t)(uintptr_t)im->view, (vk_alloc){0});
    vk2_destroy_later(d, 2, (uint64_t)(uintptr_t)im->image, im->mem);
    free(im->rt_views);
    free(im->sub_layouts);
    free(im->sub_dirty);
    free(im->sub_stages);
    free(im->tbdr_subs);
    memset(im, 0, sizeof *im);
}

/* Sobe a região [x, y, z] x [w, h, depth] de `data` (linhas de `pitch` bytes, fatias de
 * `slice` bytes) para o nível/camada da imagem. */
static int upload_cb_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK2_UPLOAD_CB");
        on = !(e && *e == '0');
    }
    return on;
}

/* Faixa de uma textura DXT recodificada para ETC2/EAC, com o cache em disco. Sem o lock do device: o hash,
 * a E/S do cache e a recodificação levam milissegundos e seguravam a thread de render (A54, 20/11/2026). */
static int etc2_target(const vk_image *im)
{
    const vk_format *f = im->fmt;
    return (im->vkf == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK || im->vkf == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) &&
           (f->d3d == D3DFMT_DXT1 || f->d3d == D3DFMT_DXT2 || f->d3d == D3DFMT_DXT3 || f->d3d == D3DFMT_DXT4 ||
            f->d3d == D3DFMT_DXT5);
}

static uint8_t *etc2_encode(const vk_image *im, uint32_t level, uint32_t w, uint32_t h, uint32_t depth,
                            const uint8_t *data, uint32_t pitch, uint32_t slice)
{
    const vk_format *f = im->fmt;
    uint32_t bb = im->vkf == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK ? 16 : 8;
    uint32_t row = (w + f->bw - 1) / f->bw * bb, rows = (h + f->bh - 1) / f->bh;
    uint8_t *out = malloc((size_t)row * rows * depth);
    if (!out) re5_fatal("vk2", "sem memória");
    int mode = (f->d3d == D3DFMT_DXT1) ? 1 : (f->d3d == D3DFMT_DXT2 || f->d3d == D3DFMT_DXT3) ? 3 : 5;
    for (uint32_t s = 0; s < depth; s++) {
        uint8_t *dst_slice = out + (size_t)s * rows * row;
        const uint8_t *src_slice = data + (size_t)s * slice;
        uint64_t hash = re5_tex_calc_hash(w, h, level, (uint32_t)f->d3d, src_slice, (size_t)rows * pitch);
        if (!re5_tex_cache_load(hash, (uint32_t)(rows * row), dst_slice)) {
            vk2_etc2_surface(mode, w, h, src_slice, pitch, dst_slice, row);
            re5_tex_cache_save(hash, w, h, im->levels, (uint32_t)f->d3d, dst_slice, (uint32_t)(rows * row));
        }
    }
    return out;
}

/* pre: a faixa já recodificada (etc2_encode, fora do lock); NULL recodifica aqui. */
static void upload(vk_device *d, vk_image *im, uint32_t layer, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                   uint32_t w, uint32_t h, uint32_t depth, const uint8_t *data, uint32_t pitch, uint32_t slice,
                   const uint8_t *pre)
{
    if (!w || !h || !depth) return;
    const vk_format *f = im->fmt;
    uint32_t bw = f->bw, bh = f->bh;
    int is_etc2_target = (im->vkf == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK ||
                          im->vkf == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    int is_dxt_source = (f->d3d == D3DFMT_DXT1 || f->d3d == D3DFMT_DXT2 || f->d3d == D3DFMT_DXT3 ||
                         f->d3d == D3DFMT_DXT4 || f->d3d == D3DFMT_DXT5);
    uint32_t dst_block_bytes = (is_etc2_target && is_dxt_source) ?
        ((im->vkf == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) ? 16 : 8) : f->bytes;
    uint32_t row = (w + bw - 1) / bw * dst_block_bytes, rows = (h + bh - 1) / bh;
    VkDeviceSize size = (VkDeviceSize)row * rows * depth;
    VkBuffer buf;
    VkDeviceSize off;
    /* Textura nova (subrecurso ainda UNDEFINED, so amostrada): grava no cb de upload, que roda antes do
     * principal, sem fechar o passe da cena (num tiler cada fechamento regrava e relê o tile inteiro;
     * na carga sobem milhares de texturas). RE5VK2_UPLOAD_CB=0 volta ao caminho antigo. */
    /* (até 20/11 testava !im->rt_views, que existe em toda imagem: o caminho nunca rodava) */
    int sep = upload_cb_on() && !vk2_layout_general() && !im->attach && !f->depth && im->sub_layouts &&
              im->sub_layouts[(size_t)layer * im->levels + level] == VK_IMAGE_LAYOUT_UNDEFINED;
    if (!sep) vk2_end_rendering(d);
    uint8_t *p = vk2_stage(d, size, dst_block_bytes, &buf, &off);

    if (is_etc2_target && is_dxt_source && pre) {
        memcpy(p, pre, (size_t)row * rows * depth);
    } else if (is_etc2_target && is_dxt_source) {
        int mode = (f->d3d == D3DFMT_DXT1) ? 1 : (f->d3d == D3DFMT_DXT2 || f->d3d == D3DFMT_DXT3) ? 3 : 5;
        for (uint32_t s = 0; s < depth; s++) {
            uint8_t *dst_slice = p + (size_t)s * rows * row;
            const uint8_t *src_slice = data + (size_t)s * slice;
            uint64_t hash = re5_tex_calc_hash(w, h, level, (uint32_t)f->d3d, src_slice, (size_t)rows * pitch);
            if (!re5_tex_cache_load(hash, (uint32_t)(rows * row), dst_slice)) {
                vk2_etc2_surface(mode, w, h, src_slice, pitch, dst_slice, row);
                re5_tex_cache_save(hash, w, h, im->levels, (uint32_t)f->d3d, dst_slice, (uint32_t)(rows * row));
            }
        }
    } else {
        for (uint32_t s = 0; s < depth; s++)
            for (uint32_t r = 0; r < rows; r++)
                memcpy(p + ((size_t)s * rows + r) * row, data + (size_t)s * slice + (size_t)r * pitch, row);
    }
    VkCommandBuffer cb = sep ? vk2_upload_cmd(d) : vk2_cmd(d);
    if (d->tm.on) { if (sep) d->tm.up_fast++; else d->tm.up_slow++; }
    VkImageLayout dst_layout = vk2_layout_general() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vk2_img_transition(d, cb, im, level, 1, layer, 1, dst_layout, 0);
    VkBufferImageCopy c = {
        .bufferOffset = off,
        .bufferRowLength = (w + bw - 1) / bw * bw,
        .bufferImageHeight = rows * bh,
        .imageSubresource = {aspect_of(f, 1), level, layer, 1},
        .imageOffset = {(int32_t)x, (int32_t)y, (int32_t)z},
        .imageExtent = {w, h, depth},
    };
    vkCmdCopyBufferToImage(cb, buf, im->image, dst_layout, 1, &c);
    vk2_img_mark_written(im, level, 1, layer, 1);
    vk2_tbdr_mark_external_write(&im->tbdr_subs[(size_t)layer * im->levels + level],
                                   RE5VK2_TBDR_COLOR | RE5VK2_TBDR_DEPTH | RE5VK2_TBDR_STENCIL);
    if (!vk2_layout_general() && !im->rt_views && !im->fmt->depth) {
        vk2_img_transition(d, cb, im, level, 1, layer, 1,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
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
    for (size_t i = 0; i < size / sizeof(void *); i++) s[i] = (void *)vk2_nyi;
}

/* ---- IUnknown e IDirect3DResource9 comuns ---- */

static vk_obj *refs_owner(vk_obj *o)
{
    /* superfície de uma textura: a contagem é a da textura (como no D3D9) */
    if (o->kind == VKO_SURFACE && ((vk_surface *)o)->container) return ((vk_surface *)o)->container;
    return o;
}

void vk2_addref(vk_obj *o)
{
    if (o) refs_owner(o)->refs++;
}

static HRESULT STDMETHODCALLTYPE o_QueryInterface(void *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    /* A ponte só pergunta pela própria interface ou por uma base: devolve o mesmo objeto. */
    vk2_addref(refs_owner(self));
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
    vk2_release(o);
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
static void vk2_occ_deactivate(vk_device *d, vk_query *q);

void vk2_release(vk_obj *o)
{
    if (!o) return;
    o = refs_owner(o);
    if (--o->refs > 0) return;
    vk_device *d = o->dev;
    vk2_lock(d);
    switch (o->kind) {
    case VKO_SURFACE: {
        vk_surface *s = (vk_surface *)o;
        if (s->own_img) {
            vk2_image_destroy(d, s->img);
            free(s->img);
        }
        free(s->lock);
        free(s);
        break;
    }
    case VKO_TEXTURE: case VKO_CUBE: case VKO_VOLUME: tex_destroy((vk_texture *)o); break;
    case VKO_VB: case VKO_IB: {
        vk_buffer *b = (vk_buffer *)o;
        vk2_destroy_later(d, 1, (uint64_t)(uintptr_t)b->buf, b->mem);
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
    case VKO_QUERY: {
        vk_query *q = (vk_query *)o;
        if (q->active) {   /* liberada entre BEGIN e END: fecha o trecho aberto e sai da lista */
            if (q->open_slot >= 0) vkCmdEndQuery(vk2_cmd(d), d->occ.pool, (uint32_t)q->open_slot);
            vk2_occ_deactivate(d, q);
        }
        free(q->slots);
        free(q);
        break;
    }
    default: free(o); break;
    }
    vk2_unlock(d);
}

static void obj_init(vk_obj *o, const void *vt, vk_device *d, int kind)
{
    o->lpVtbl = vt;
    o->refs = 1;
    o->dev = d;
    o->kind = kind;
}

/* ---- superfícies ---- */

vk_surface *vk2_surface_new(vk_device *d, vk_obj *container, vk_image *img, int own, uint32_t layer, uint32_t level,
                              D3DFORMAT format, DWORD usage, D3DPOOL pool)
{
    vk_surface *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("vk2", "sem memória");
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
    if (s->img) re5_fatal("vk2", "LockRect de superfície na GPU sem implementação");
    const vk_format *f = vk2_format(s->desc.Format);
    uint32_t pitch = s->desc.Width * f->bytes;
    lr->Pitch = (INT)pitch;
    lr->pBits = s->lock + (r ? (size_t)r->top * pitch + (size_t)r->left * f->bytes : 0);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE s_UnlockRect(IDirect3DSurface9 *self) { return D3D_OK; }

HRESULT STDMETHODCALLTYPE vk2_CreateOffscreenPlainSurface(IDirect3DDevice9 *self, UINT w, UINT h, D3DFORMAT format,
                                                            D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *sh)
{
    const vk_format *f = vk2_format(format);
    if (!out || sh || !f || f->bw != 1) return D3DERR_INVALIDCALL;
    if (pool != D3DPOOL_SYSTEMMEM) re5_fatal("vk2", "CreateOffscreenPlainSurface fora de D3DPOOL_SYSTEMMEM");
    vk_surface *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("vk2", "sem memória");
    obj_init(&s->o, &g_vt_surface, (vk_device *)self, VKO_SURFACE);
    s->desc = (D3DSURFACE_DESC){format, D3DRTYPE_SURFACE, 0, pool, D3DMULTISAMPLE_NONE, 0, w, h};
    s->lock = calloc((size_t)w * h, f->bytes);
    if (!s->lock) re5_fatal("vk2", "sem memória");
    *out = (IDirect3DSurface9 *)s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE vk2_GetRenderTargetData(IDirect3DDevice9 *self, IDirect3DSurface9 *rt,
                                                    IDirect3DSurface9 *dst)
{
    vk_device *d = (vk_device *)self;
    vk_surface *s = (vk_surface *)rt, *t = (vk_surface *)dst;
    if (!s || !t || !s->img || t->img || s->desc.Width != t->desc.Width || s->desc.Height != t->desc.Height)
        return D3DERR_INVALIDCALL;
    const vk_format *f = s->img->fmt;
    VkDeviceSize size = (VkDeviceSize)s->desc.Width * s->desc.Height * f->bytes;
    vk2_lock(d);
    vk2_end_rendering(d);
    vk2_tbdr_mark_read(&d->tbdr, &s->img->tbdr_subs[(size_t)s->layer * s->img->levels + s->level], RE5VK2_TBDR_COLOR);
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t *p = vk2_stage(d, size, 16, &buf, &off);
    VkCommandBuffer cb = vk2_cmd(d);
    VkImageLayout src_layout = vk2_layout_general() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vk2_img_transition(d, cb, s->img, s->level, 1, s->layer, 1, src_layout, 0);
    VkBufferImageCopy c = {.bufferOffset = off,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->level, s->layer, 1},
                           .imageExtent = {s->desc.Width, s->desc.Height, 1}};
    vkCmdCopyImageToBuffer(cb, s->img->image, src_layout, buf, 1, &c);
    vk2_flush(d, 1);   /* espera a GPU: a fence garante visibilidade ao host antes do memcpy */
    memcpy(t->lock, p, (size_t)size);
    vk2_unlock(d);
    return D3D_OK;
}

/* ---- texturas ---- */

static vk_texture *tex_new(vk_device *d, int kind, const void *vt, D3DFORMAT format, DWORD usage, D3DPOOL pool,
                           uint32_t w, uint32_t h, uint32_t depth, uint32_t levels)
{
    const vk_format *f = vk2_format(format);
    if (!f) re5_fatal("vk2", "CreateTexture: formato %u (0x%08X) sem tradução", (unsigned)format, (unsigned)format);
    if (usage & D3DUSAGE_AUTOGENMIPMAP) re5_fatal("vk2", "D3DUSAGE_AUTOGENMIPMAP sem implementação");
    vk_texture *t = calloc(1, sizeof *t);
    if (!t) re5_fatal("vk2", "sem memória");
    obj_init(&t->o, vt, d, kind);
    t->format = format;
    t->usage = usage;
    t->pool = pool;
    int cube = kind == VKO_CUBE;
    vk2_image_create(d, &t->img, kind == VKO_VOLUME ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, f, w, h, depth, levels,
                       cube ? 6 : 1, cube, !!(usage & D3DUSAGE_RENDERTARGET), !!(usage & D3DUSAGE_DEPTHSTENCIL));
    size_t n = (size_t)t->img.levels * t->img.layers;
    t->surf = calloc(n, sizeof *t->surf);
    t->locks = calloc(n, sizeof *t->locks);
    if (!t->surf || !t->locks) re5_fatal("vk2", "sem memória");
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
    vk2_image_destroy(d, &t->img);
    free(t->surf);
    free(t->locks);
    free(t);
}

static vk_surface *tex_surface(vk_texture *t, uint32_t layer, uint32_t level)
{
    size_t i = (size_t)layer * t->img.levels + level;
    if (!t->surf[i])
        t->surf[i] = vk2_surface_new(t->o.dev, &t->o, &t->img, 0, layer, level, t->format, t->usage, t->pool);
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
        re5_fatal("vk2", "LockRect de render target/profundidade sem implementação");
    vk_device *d = t->o.dev;
    vk2_lock(d);
    size_t i = (size_t)layer * t->img.levels + level;
    uint32_t pitch, rows, slices;
    level_layout(t, level, &pitch, &rows, &slices);
    if (!t->locks[i].data) {
        t->locks[i].data = calloc((size_t)pitch * rows * slices, 1);
        if (!t->locks[i].data) re5_fatal("vk2", "sem memória para travar %u bytes", pitch * rows * slices);
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
    vk2_unlock(d);
    return D3D_OK;
}

static HRESULT tex_unlock(vk_texture *t, uint32_t layer, uint32_t level)
{
    if (level >= t->img.levels || layer >= t->img.layers) return D3DERR_INVALIDCALL;
    vk_device *d = t->o.dev;
    size_t i = (size_t)layer * t->img.levels + level;
    /* O Lock/Unlock de um nível é da thread do jogo que o pediu; a recodificação DXT -> ETC2 (hash, cache
     * em disco, codificador) sai do lock do device para não parar a thread de render. */
    uint8_t *pre = NULL;
    if (t->locks[i].on && t->locks[i].data && etc2_target(&t->img)) {
        const vk_format *f = t->img.fmt;
        uint32_t pitch, rows, slices;
        level_layout(t, level, &pitch, &rows, &slices);
        RECT r = t->locks[i].r;
        D3DBOX bx = t->locks[i].box;
        uint32_t lw = t->img.w >> level, lh = t->img.h >> level;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        uint32_t x = (uint32_t)r.left / f->bw * f->bw, y = (uint32_t)r.top / f->bh * f->bh;
        uint32_t x2 = (uint32_t)r.right, y2 = (uint32_t)r.bottom;
        if (x2 > lw) x2 = lw;
        if (y2 > lh) y2 = lh;
        uint32_t z = t->img.type == VK_IMAGE_TYPE_3D ? bx.Front : 0;
        uint32_t nz = t->img.type == VK_IMAGE_TYPE_3D ? bx.Back - bx.Front : 1;
        if (x2 > x && y2 > y) {
            const uint8_t *src = t->locks[i].data + (size_t)z * pitch * rows + (size_t)(y / f->bh) * pitch +
                                 (size_t)(x / f->bw) * f->bytes;
            pre = etc2_encode(&t->img, level, x2 - x, y2 - y, nz, src, pitch, pitch * rows);
        }
    }
    vk2_lock(d);
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
        if (x2 > x && y2 > y) {
            const uint8_t *src = t->locks[i].data + (size_t)z * pitch * rows + (size_t)(y / f->bh) * pitch +
                                 (size_t)(x / f->bw) * f->bytes;
            upload(d, &t->img, layer, level, x, y, z, x2 - x, y2 - y, nz, src, pitch, pitch * rows, pre);
        }
        t->locks[i].on = 0;
    }
    vk2_unlock(d);
    free(pre);
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

HRESULT STDMETHODCALLTYPE vk2_CreateTexture(IDirect3DDevice9 *self, UINT w, UINT h, UINT levels, DWORD usage,
                                              D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    vk2_lock(d);
    *out = (IDirect3DTexture9 *)tex_new(d, VKO_TEXTURE, &g_vt_texture, format, usage, pool, w, h, 1, levels);
    vk2_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE vk2_CreateCubeTexture(IDirect3DDevice9 *self, UINT edge, UINT levels, DWORD usage,
                                                  D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **out,
                                                  HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    vk2_lock(d);
    *out = (IDirect3DCubeTexture9 *)tex_new(d, VKO_CUBE, &g_vt_cube, format, usage, pool, edge, edge, 1, levels);
    vk2_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE vk2_CreateVolumeTexture(IDirect3DDevice9 *self, UINT w, UINT h, UINT depth, UINT levels,
                                                    DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                    IDirect3DVolumeTexture9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    vk2_lock(d);
    *out = (IDirect3DVolumeTexture9 *)tex_new(d, VKO_VOLUME, &g_vt_volume, format, usage, pool, w, h, depth, levels);
    vk2_unlock(d);
    return D3D_OK;
}

static HRESULT surface_create(vk_device *d, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, int depth,
                              IDirect3DSurface9 **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    if (ms > D3DMULTISAMPLE_NONMASKABLE) re5_fatal("vk2", "superfície com MSAA %u sem implementação", (unsigned)ms);
    const vk_format *f = vk2_format(format);
    if (!f) re5_fatal("vk2", "superfície com formato %u (0x%08X) sem tradução", (unsigned)format, (unsigned)format);
    vk2_lock(d);
    vk_image *im = calloc(1, sizeof *im);
    if (!im) re5_fatal("vk2", "sem memória");
    vk2_image_create(d, im, VK_IMAGE_TYPE_2D, f, w, h, 1, 1, 1, 0, !depth, depth);
    *out = (IDirect3DSurface9 *)vk2_surface_new(d, NULL, im, 1, 0, 0, format,
                                                  depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    vk2_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE vk2_CreateRenderTarget(IDirect3DDevice9 *self, UINT w, UINT h, D3DFORMAT format,
                                                   D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lockable,
                                                   IDirect3DSurface9 **out, HANDLE *sh)
{
    if (sh) return D3DERR_INVALIDCALL;
    RE5_DEBUG("vk2", "CreateRenderTarget %ux%u formato %u", w, h, (unsigned)format);
    return surface_create((vk_device *)self, w, h, format, ms, 0, out);
}

HRESULT STDMETHODCALLTYPE vk2_CreateDepthStencilSurface(IDirect3DDevice9 *self, UINT w, UINT h, D3DFORMAT format,
                                                          D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard,
                                                          IDirect3DSurface9 **out, HANDLE *sh)
{
    if (sh) return D3DERR_INVALIDCALL;
    RE5_DEBUG("vk2", "CreateDepthStencilSurface %ux%u formato %u", w, h, (unsigned)format);
    return surface_create((vk_device *)self, w, h, format, ms, 1, out);
}

/* ---- buffers ---- */

static void buffer_alloc(vk_device *d, vk_buffer *b)
{
    const VkDeviceSize size = b->size ? b->size : 4;
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    /* DYNAMIC (travado a cada quadro): na memória do guest, e o Unlock da ponte não copia. Os
     * estáticos ficam fora, para não gastar o espaço de 32 bits do guest com malhas. */
    b->buf = vk2_buffer_new(d, size, usage, 1, b->o.kind == VKO_IB ? "re5vk.index_buffer" : "re5vk.vertex_buffer",
                              &b->mem);
}

static vk_buffer *buffer_new(vk_device *d, int kind, const void *vt, UINT size, DWORD usage, D3DPOOL pool)
{
    vk_buffer *b = calloc(1, sizeof *b);
    if (!b) re5_fatal("vk2", "sem memória");
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
    vk2_lock(d);
    vk2_poll(d);
    if (b->last_use > d->done_serial) {   /* a GPU ainda pode ler o conteúdo atual */
        if (flags & D3DLOCK_DISCARD) {
            vk2_destroy_later(d, 1, (uint64_t)(uintptr_t)b->buf, b->mem);
            buffer_alloc(d, b);
            b->last_use = 0;
        } else if (!(flags & D3DLOCK_NOOVERWRITE) && !(flags & D3DLOCK_READONLY)) {
            vk2_wait_serial(d, b->last_use);
        }
    }
    *pp = b->mem.map + off;
    b->locked = 1;
    vk2_unlock(d);
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

HRESULT STDMETHODCALLTYPE vk2_CreateVertexBuffer(IDirect3DDevice9 *self, UINT size, DWORD usage, DWORD fvf,
                                                   D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    vk2_lock(d);
    vk_buffer *b = buffer_new(d, VKO_VB, &g_vt_vb, size, usage, pool);
    b->fvf = fvf;
    b->format = D3DFMT_VERTEXDATA;
    *out = (IDirect3DVertexBuffer9 *)b;
    vk2_unlock(d);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE vk2_CreateIndexBuffer(IDirect3DDevice9 *self, UINT size, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *sh)
{
    if (!out || sh) return D3DERR_INVALIDCALL;
    if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    vk_device *d = (vk_device *)self;
    vk2_lock(d);
    vk_buffer *b = buffer_new(d, VKO_IB, &g_vt_ib, size, usage, pool);
    b->format = format;
    *out = (IDirect3DIndexBuffer9 *)b;
    vk2_unlock(d);
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

HRESULT STDMETHODCALLTYPE vk2_CreateVertexDeclaration(IDirect3DDevice9 *self, const D3DVERTEXELEMENT9 *el,
                                                        IDirect3DVertexDeclaration9 **out)
{
    if (!el || !out) return D3DERR_INVALIDCALL;
    vk_decl *dl = calloc(1, sizeof *dl);
    if (!dl) re5_fatal("vk2", "sem memória");
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

/* Shader sem objeto COM visível ao jogo: o pré-aquecimento (vk2_warm.c) cria os dele nas threads auxiliares. */
vk_shader *vk2_shader_raw(vk_device *d, const uint32_t *code, uint32_t n, int pixel)
{
    vk_shader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    obj_init(&s->o, pixel ? (const void *)&g_vt_ps : (const void *)&g_vt_vs, d, pixel ? VKO_PS : VKO_VS);
    s->code = malloc(n * 4u);
    if (!s->code) { free(s); return NULL; }
    memcpy(s->code, code, n * 4u);
    s->ndw = n;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n * 4; i++) h = (h ^ ((const uint8_t *)code)[i]) * 16777619u;
    s->hash = h;
    return s;
}

void vk2_shader_raw_free(vk_device *d, vk_shader *s)
{
    for (int i = 0; i < s->nv; i++) {
        vkDestroyShaderModule(d->dev, s->v[i].mod, NULL);
        free(s->v[i].info.spv);
    }
    free(s->code);
    free(s);
}

static HRESULT shader_create(vk_device *d, const DWORD *code, int pixel, void **out)
{
    if (!code || !out) return D3DERR_INVALIDCALL;
    uint32_t n = re5_d3d9_shader_dwords((const uint32_t *)code, 1u << 20);
    if (!n) re5_fatal("vk2", "shader sem token de fim");
    vk_shader *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("vk2", "sem memória");
    obj_init(&s->o, pixel ? (const void *)&g_vt_ps : (const void *)&g_vt_vs, d, pixel ? VKO_PS : VKO_VS);
    s->code = malloc(n * 4u);
    if (!s->code) re5_fatal("vk2", "sem memória");
    memcpy(s->code, code, n * 4u);
    s->ndw = n;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n * 4; i++) h = (h ^ ((const uint8_t *)code)[i]) * 16777619u;
    s->hash = h;
    *out = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE vk2_CreateVertexShader(IDirect3DDevice9 *self, const DWORD *code,
                                                   IDirect3DVertexShader9 **out)
{
    return shader_create((vk_device *)self, code, 0, (void **)out);
}

HRESULT STDMETHODCALLTYPE vk2_CreatePixelShader(IDirect3DDevice9 *self, const DWORD *code,
                                                  IDirect3DPixelShader9 **out)
{
    return shader_create((vk_device *)self, code, 1, (void **)out);
}

/* Sem abortar: E_FAIL se a tradução recusa (o pré-aquecimento pula o pipeline). err recebe o motivo. */
HRESULT vk2_shader_variant_try(vk_device *d, vk_shader *s, const vk2_dxso_variant *var, VkShaderModule *mod,
                               const vk2_shader **info, char *err, size_t errsz)
{
    for (int i = 0; i < s->nv; i++) {
        if (s->v[i].var.depth_samplers == var->depth_samplers && s->v[i].var.alpha_func == var->alpha_func &&
            s->v[i].var.in_uint == var->in_uint && s->v[i].var.in_sint == var->in_sint &&
            s->v[i].var.in_udec3 == var->in_udec3) {
            *mod = s->v[i].mod;
            *info = &s->v[i].info;
            return D3D_OK;
        }
    }
    if (s->nv >= VK_MAX_VARIANTS) {
        snprintf(err, errsz, "variantes demais");
        return E_FAIL;
    }
    vk2_shader *out = &s->v[s->nv].info;
    if (vk2_dxso_compile(s->code, s->ndw, var, out, err, errsz) != 0) return E_FAIL;
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = out->nwords * 4, .pCode = out->spv};
    if (vkCreateShaderModule(d->dev, &ci, NULL, &s->v[s->nv].mod) != VK_SUCCESS) {
        free(out->spv);
        snprintf(err, errsz, "vkCreateShaderModule falhou");
        return E_FAIL;
    }
    s->v[s->nv].var = *var;
    *mod = s->v[s->nv].mod;
    *info = out;
    s->nv++;
    return D3D_OK;
}

HRESULT vk2_shader_variant(vk_device *d, vk_shader *s, const vk2_dxso_variant *var, VkShaderModule *mod,
                             const vk2_shader **info)
{
    char err[256];
    if (vk2_shader_variant_try(d, s, var, mod, info, err, sizeof err) != D3D_OK)
        re5_fatal("vk2", "shader %s_%08x: %s (RE5_D3D9_SHADER_DUMP=<pasta> guarda o bytecode)",
                  s->o.kind == VKO_PS ? "ps" : "vs", s->hash, err);
    return D3D_OK;
}

/* ---- consultas: evento (o RE5 sincroniza quadros com elas) e oclusão ----
 * Oclusão: sem ela o vk2 respondia D3DERR_NOTAVAILABLE à sondagem CreateQuery(OCCLUSION, NULL), e o
 * jogo ia por um caminho sem teste de visibilidade das fontes de luz (suspeita da cena estourada perto
 * de chamas). A DXVK implementa a consulta. */

static int occ_init(vk_device *d)
{
    if (d->occ.pool) return 1;
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION,
                                .queryCount = RE5VK2_OCC_SLOTS};
    if (vkCreateQueryPool(d->dev, &qi, NULL, &d->occ.pool) != VK_SUCCESS) {
        d->occ.pool = VK_NULL_HANDLE;
        return 0;
    }
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(d->phys, &f);
    d->occ.precise = f.occlusionQueryPrecise;   /* o contexto liga a feature quando existe */
    d->occ.head = d->occ.ready = 0;
    RE5_INFO("vk2", "consultas de oclusão: %u slots, %s", RE5VK2_OCC_SLOTS,
             d->occ.precise ? "precisas" : "não precisas (só 0/1)");
    return 1;
}

static void occ_reset_range(VkCommandBuffer cb, vk_device *d, uint64_t from, uint64_t to)
{
    while (from < to) {
        uint32_t a = (uint32_t)(from % RE5VK2_OCC_SLOTS);
        uint32_t n = (uint32_t)(to - from);
        if (a + n > RE5VK2_OCC_SLOTS) n = RE5VK2_OCC_SLOTS - a;
        vkCmdResetQueryPool(cb, d->occ.pool, a, n);
        from += n;
    }
}

void vk2_occ_before_pass(vk_device *d, VkCommandBuffer cb)
{
    if (!d->occ.pool) return;
    /* reserva: as ativas + folga para BEGIN emitidos com o passe já aberto */
    uint64_t want = d->occ.head + (uint64_t)d->occ.nactive + 32u;
    if (d->occ.ready >= want) return;
    uint64_t from = d->occ.ready > d->occ.head ? d->occ.ready : d->occ.head;
    uint64_t to = d->occ.head + (uint64_t)d->occ.nactive + 128u;
    occ_reset_range(cb, d, from, to);
    d->occ.ready = to;
}

static void occ_seg_begin(vk_device *d, VkCommandBuffer cb, vk_query *q)
{
    uint32_t slot = (uint32_t)(d->occ.head++ % RE5VK2_OCC_SLOTS);
    vkCmdBeginQuery(cb, d->occ.pool, slot, d->occ.precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    if (q->nslots == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 8;
        q->slots = realloc(q->slots, q->cap * sizeof *q->slots);
        if (!q->slots) re5_fatal("vk2", "sem memória");
    }
    q->slots[q->nslots++] = slot;
    q->open_slot = (int32_t)slot;
}

void vk2_occ_pass_begin(vk_device *d, VkCommandBuffer cb)
{
    for (int i = 0; i < d->occ.nactive; i++) {
        if (d->occ.ready <= d->occ.head) re5_fatal("vk2", "oclusão: sem slot zerado ao abrir o passe");
        occ_seg_begin(d, cb, d->occ.active[i]);
    }
}

void vk2_occ_pass_end(vk_device *d, VkCommandBuffer cb)
{
    for (int i = 0; i < d->occ.nactive; i++) {
        vk_query *q = d->occ.active[i];
        if (q->open_slot >= 0) {
            vkCmdEndQuery(cb, d->occ.pool, (uint32_t)q->open_slot);
            q->open_slot = -1;
        }
    }
}

static void vk2_occ_deactivate(vk_device *d, vk_query *q)
{
    for (int i = 0; i < d->occ.nactive; i++)
        if (d->occ.active[i] == q) {
            d->occ.active[i] = d->occ.active[--d->occ.nactive];
            break;
        }
    q->active = 0;
}

static D3DQUERYTYPE STDMETHODCALLTYPE q_GetType(IDirect3DQuery9 *self) { return ((vk_query *)self)->type; }
static DWORD STDMETHODCALLTYPE q_GetDataSize(IDirect3DQuery9 *self)
{
    return ((vk_query *)self)->type == D3DQUERYTYPE_OCCLUSION ? sizeof(DWORD) : sizeof(BOOL);
}
static HRESULT STDMETHODCALLTYPE q_Issue(IDirect3DQuery9 *self, DWORD flags)
{
    vk_query *q = (vk_query *)self;
    vk_device *d = q->o.dev;
    if (q->type != D3DQUERYTYPE_OCCLUSION) {
        if (flags & D3DISSUE_END) q->serial = d->serial;
        return D3D_OK;
    }
    vk2_lock(d);
    if (flags & D3DISSUE_BEGIN) {
        if (q->active) {   /* BEGIN de novo: recomeça a medida */
            if (q->open_slot >= 0) vkCmdEndQuery(vk2_cmd(d), d->occ.pool, (uint32_t)q->open_slot);
            q->open_slot = -1;
            vk2_occ_deactivate(d, q);
        }
        if (d->occ.nactive >= RE5VK2_OCC_ACTIVE) re5_fatal("vk2", "oclusão: consultas ativas demais");
        q->nslots = 0;
        q->serial = 0;
        q->open_slot = -1;
        q->active = 1;
        d->occ.active[d->occ.nactive++] = q;
        if (d->rendering) {
            if (d->occ.ready > d->occ.head) occ_seg_begin(d, vk2_cmd(d), q);
            else vk2_end_rendering(d);   /* sem slot zerado: o próximo passe abre o trecho */
        }
    }
    if (flags & D3DISSUE_END) {
        if (q->active) {
            if (q->open_slot >= 0) vkCmdEndQuery(vk2_cmd(d), d->occ.pool, (uint32_t)q->open_slot);
            q->open_slot = -1;
            vk2_occ_deactivate(d, q);
        }
        q->serial = d->serial;
    }
    vk2_unlock(d);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE q_GetData(IDirect3DQuery9 *self, void *data, DWORD size, DWORD flags)
{
    vk_query *q = (vk_query *)self;
    vk_device *d = q->o.dev;
    vk2_lock(d);
    vk2_poll(d);
    int done = !q->active && (!q->serial || q->serial <= d->done_serial);
    if (!done && !q->active && (flags & D3DGETDATA_FLUSH) && q->serial >= d->serial) vk2_flush(d, 0);
    uint64_t sum = 0;
    if (done && q->type == D3DQUERYTYPE_OCCLUSION)
        for (uint32_t i = 0; i < q->nslots; i++) {
            uint64_t v = 0;
            VkResult r = vkGetQueryPoolResults(d->dev, d->occ.pool, q->slots[i], 1, sizeof v, &v, sizeof v,
                                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (r == VK_SUCCESS) sum += v;
        }
    vk2_unlock(d);
    if (!done) return S_FALSE;
    if (q->type == D3DQUERYTYPE_OCCLUSION) {
        if (data && size >= sizeof(DWORD)) *(DWORD *)data = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)sum;
    } else if (data && size >= sizeof(BOOL)) {
        *(BOOL *)data = TRUE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE vk2_CreateQuery(IDirect3DDevice9 *self, D3DQUERYTYPE type, IDirect3DQuery9 **out)
{
    vk_device *d = (vk_device *)self;
    if (type == D3DQUERYTYPE_OCCLUSION) {
        vk2_lock(d);
        int ok = !d->pq_on && occ_init(d);   /* a fila de passes (diagnóstico) grava em secundários: sem oclusão */
        vk2_unlock(d);
        if (!ok) return D3DERR_NOTAVAILABLE;
    } else if (type != D3DQUERYTYPE_EVENT) {
        if (!out) return D3DERR_NOTAVAILABLE;   /* sondagem: sem suporte ainda */
        re5_fatal("vk2", "CreateQuery tipo %u sem implementação (só EVENT e OCCLUSION)", (unsigned)type);
    }
    if (!out) return D3D_OK;
    vk_query *q = calloc(1, sizeof *q);
    if (!q) re5_fatal("vk2", "sem memória");
    obj_init(&q->o, &g_vt_query, d, VKO_QUERY);
    q->type = type;
    q->open_slot = -1;
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

void vk2_res_vtables(vk_device *d)
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
void vk2_init_dummies(vk_device *d)
{
    const vk_format *f = vk2_format(D3DFMT_A8B8G8R8);
    vk2_image_create(d, &d->dummy2d, VK_IMAGE_TYPE_2D, f, 1, 1, 1, 1, 1, 0, 0, 0);
    vk2_image_create(d, &d->dummycube, VK_IMAGE_TYPE_2D, f, 1, 1, 1, 1, 6, 1, 0, 0);
    vk2_image_create(d, &d->dummy3d, VK_IMAGE_TYPE_3D, f, 1, 1, 1, 1, 1, 0, 0, 0);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    VkCommandBuffer cb = vk2_cmd(d);
    int gen = vk2_layout_general();
    VkImageLayout dst_layout = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageLayout ro_layout = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk2_img_transition(d, cb, &d->dummy2d, 0, 1, 0, 1, dst_layout, 0);
    vk2_img_transition(d, cb, &d->dummy3d, 0, 1, 0, 1, dst_layout, 0);
    vk2_img_transition(d, cb, &d->dummycube, 0, 1, 0, 6, dst_layout, 0);
    all.layerCount = 1;
    vkCmdClearColorImage(cb, d->dummy2d.image, dst_layout, &black, 1, &all);
    vkCmdClearColorImage(cb, d->dummy3d.image, dst_layout, &black, 1, &all);
    all.layerCount = 6;
    vkCmdClearColorImage(cb, d->dummycube.image, dst_layout, &black, 1, &all);
    vk2_img_mark_written(&d->dummy2d, 0, 1, 0, 1);
    vk2_img_mark_written(&d->dummy3d, 0, 1, 0, 1);
    vk2_img_mark_written(&d->dummycube, 0, 1, 0, 6);
    vk2_img_transition(d, cb, &d->dummy2d, 0, 1, 0, 1, ro_layout,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk2_img_transition(d, cb, &d->dummy3d, 0, 1, 0, 1, ro_layout,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk2_img_transition(d, cb, &d->dummycube, 0, 1, 0, 6, ro_layout,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    d->zero_vb = vk2_buffer_new(d, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1, "re5vk.vertex_zero", &d->zero_vb_mem);
    memset(d->zero_vb_mem.map, 0, 256);
    {   /* D3D9: elemento ausente na declaracao le (0,0,0,1) (a DXVK tambem). RE5VK2_MISSING_W1=0 deixa tudo zero. */
        const char *e = getenv("RE5VK2_MISSING_W1");
        if (!(e && *e == '0'))
            for (int i = 0; i < 256 / 16; i++) ((float *)d->zero_vb_mem.map)[i * 4 + 3] = 1.0f;
    }
}
