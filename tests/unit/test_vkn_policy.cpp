/*
 * test_vkn_policy.cpp - política TBDR dos renderizadores nativos (IRenderer.h, namespace policy),
 * chaves do cache de VkRenderPass e o seletor RE5_GFX (re5/renderer.h). Sem GPU.
 */
#include "check.h"

#include "IRenderer.h"

using namespace re5::vkn;

static const VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;
static const VkFormat kDs = VK_FORMAT_D24_UNORM_S8_UINT;
static const VkImageLayout kRo = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

int main()
{
    /* 1. loadOp: CLEAR > sobrescrito > conteúdo indefinido > LOAD */
    {
        AttachmentUse u;
        CHECK(policy::loadOp(u, kRo) == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(policy::loadOp(u, VK_IMAGE_LAYOUT_UNDEFINED) == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        u.fullyOverwritten = true;
        CHECK(policy::loadOp(u, kRo) == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        u.clearAtStart = true;
        CHECK(policy::loadOp(u, kRo) == VK_ATTACHMENT_LOAD_OP_CLEAR);
    }

    /* 2. cor: STORE só com leitura depois; amostrável vai a SHADER_READ_ONLY; backbuffer sempre apresenta */
    {
        AttachmentUse u;
        u.clearAtStart = true;
        u.readAfter = false;
        AttachmentOps o = policy::armColor(u, kColor, VK_SAMPLE_COUNT_1_BIT, kRo, true);
        CHECK(o.load == VK_ATTACHMENT_LOAD_OP_CLEAR);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_DONT_CARE);
        CHECK(o.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        CHECK(o.finalLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        u.clearAtStart = false;
        u.readAfter = true;
        o = policy::armColor(u, kColor, VK_SAMPLE_COUNT_1_BIT, kRo, true);
        CHECK(o.load == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(o.initialLayout == kRo);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_STORE);
        CHECK(o.finalLayout == kRo);

        AttachmentUse bb;
        bb.backbuffer = true;
        bb.readAfter = false; /* o Present lê, o oráculo não manda aqui */
        bb.clearAtStart = true;
        o = policy::armColor(bb, kColor, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, false);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_STORE);
        CHECK(o.finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    /* 3. profundidade: padrão seguro (STORE); com o veredito do oráculo, DONT_CARE nos dois aspectos */
    {
        AttachmentUse d, s;
        d.clearAtStart = s.clearAtStart = true;
        AttachmentOps o = policy::armDepthStencil(d, s, true, true, kDs, VK_SAMPLE_COUNT_1_BIT,
                                                  VK_IMAGE_LAYOUT_UNDEFINED, false);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_STORE);
        CHECK(o.stencilStore == VK_ATTACHMENT_STORE_OP_STORE);

        d.readAfter = s.readAfter = false;
        o = policy::armDepthStencil(d, s, true, true, kDs, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, true);
        CHECK(o.load == VK_ATTACHMENT_LOAD_OP_CLEAR);
        CHECK(o.stencilLoad == VK_ATTACHMENT_LOAD_OP_CLEAR);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_DONT_CARE);
        CHECK(o.stencilStore == VK_ATTACHMENT_STORE_OP_DONT_CARE);
        CHECK(o.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        CHECK(o.finalLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL); /* nada guardado: não vai a leitura */

        /* só profundidade: estêncil fica DONT_CARE/DONT_CARE */
        d.readAfter = true;
        o = policy::armDepthStencil(d, s, true, false, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, false);
        CHECK(o.stencilLoad == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        CHECK(o.stencilStore == VK_ATTACHMENT_STORE_OP_DONT_CARE);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_STORE);
    }

    /* 4. Adreno: profundidade carregada, só testada e lida depois -> STORE_OP_NONE (se o device aceita) */
    {
        AttachmentUse d, s;
        d.written = false;
        const VkImageLayout ds = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        AttachmentOps o = policy::adrenoDepthStencil(d, s, true, false, kDs, VK_SAMPLE_COUNT_1_BIT, ds, false, true);
        CHECK(o.load == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_NONE);
        o = policy::adrenoDepthStencil(d, s, true, false, kDs, VK_SAMPLE_COUNT_1_BIT, ds, false, false);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_STORE);
        d.written = true;
        o = policy::adrenoDepthStencil(d, s, true, false, kDs, VK_SAMPLE_COUNT_1_BIT, ds, false, true);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_STORE);
        d.written = false;
        d.readAfter = false;
        o = policy::adrenoDepthStencil(d, s, true, false, kDs, VK_SAMPLE_COUNT_1_BIT, ds, false, true);
        CHECK(o.store == VK_ATTACHMENT_STORE_OP_DONT_CARE);
    }

    /* 5. chave do cache de passes: só os anexos em uso contam */
    {
        RenderPassKey a, b;
        a.colorCount = b.colorCount = 1;
        a.color[0].format = b.color[0].format = kColor;
        a.color[1].format = VK_FORMAT_R16G16B16A16_SFLOAT; /* fora de colorCount: ignorado */
        RenderPassKeyHash h;
        CHECK(a == b);
        CHECK(h(a) == h(b));
        b.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
        CHECK(!(a == b));
        b.color[0].store = a.color[0].store;
        b.hasDepthStencil = true;
        CHECK(!(a == b));
    }

    /* 6. seletor RE5_GFX */
    {
        re5_renderer_kind k = RE5_RENDERER_GL;
        CHECK(re5_renderer_parse(nullptr, &k) == 0 && k == RE5_RENDERER_DXVK);
        CHECK(re5_renderer_parse("", &k) == 0 && k == RE5_RENDERER_DXVK);
        CHECK(re5_renderer_parse("dxvk", &k) == 0 && k == RE5_RENDERER_DXVK);
        CHECK(re5_renderer_parse("vkadreno", &k) == 0 && k == RE5_RENDERER_VKADRENO);
        CHECK(re5_renderer_parse("vkmali", &k) == 0 && k == RE5_RENDERER_VKMALI);
        CHECK(re5_renderer_parse("gl", &k) == 0 && k == RE5_RENDERER_GL);
        CHECK(re5_renderer_parse("re5vk", &k) == -1);
        CHECK(re5_renderer_parse("vulkan", &k) == -1);
        const re5_renderer_kind all[] = {RE5_RENDERER_DXVK, RE5_RENDERER_VKADRENO, RE5_RENDERER_VKMALI, RE5_RENDERER_GL};
        for (re5_renderer_kind x : all) {
            re5_renderer_kind back;
            CHECK(re5_renderer_parse(re5_renderer_name(x), &back) == 0 && back == x);
        }
        CHECK(re5_renderer_is_native_vk(RE5_RENDERER_VKADRENO) && re5_renderer_is_native_vk(RE5_RENDERER_VKMALI));
        CHECK(!re5_renderer_is_native_vk(RE5_RENDERER_DXVK) && !re5_renderer_is_native_vk(RE5_RENDERER_GL));
        auto r = createRenderer(RE5_RENDERER_VKADRENO);
        CHECK(r && r->kind() == RE5_RENDERER_VKADRENO && dynamic_cast<NativeVulkanRenderer *>(r.get()));
        r = createRenderer(RE5_RENDERER_DXVK);
        CHECK(r && dynamic_cast<DxvkRenderer *>(r.get()));
        CHECK(!createRenderer(RE5_RENDERER_GL));
    }

    CHECK_DONE();
}
