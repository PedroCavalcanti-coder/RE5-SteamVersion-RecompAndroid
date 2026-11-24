/*
 * test_vkn_gpu.cpp - renderizador nativo de ponta a ponta numa GPU de verdade, sem janela:
 * vk-bootstrap sobe instância/device, VMA aloca, uploads por staging, passes com o oráculo TBDR
 * (profundidade vira STORE_OP_DONT_CARE depois de quadros estáveis), clear no meio do passe,
 * framebuffers descartados com a imagem e nenhum vazamento no fim (VulkanMemory::destroy aborta).
 *
 * O oráculo é opcional (desligado por padrão): o CTest roda este teste com RE5VK_TBDR=1.
 * Sem Vulkan na máquina: sai com 77 (CTest marca como pulado). Na máquina de desenvolvimento a GPU
 * não é Adreno nem Mali: RE5_VKN_ANY_GPU=1 (o teste liga) aceita qualquer uma.
 */
#include "check.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "IRenderer.h"

#if defined(_WIN32)
#include <malloc.h>
static void set_env(const char *k, const char *v)
{
    _putenv_s(k, v);
}
static void *aligned_mem(size_t align, size_t size)
{
    return _aligned_malloc(size, align);
}
static void aligned_free(void *p)
{
    _aligned_free(p);
}
#else
static void set_env(const char *k, const char *v)
{
    setenv(k, v, 1);
}
static void *aligned_mem(size_t align, size_t size)
{
    return std::aligned_alloc(align, size);
}
static void aligned_free(void *p)
{
    std::free(p);
}
#endif

using namespace re5::vkn;

int main()
{
    set_env("RE5_VKN_ANY_GPU", "1");
    auto r = createRenderer(RE5_RENDERER_VKMALI);
    auto *nr = dynamic_cast<NativeVulkanRenderer *>(r.get());
    CHECK(nr != nullptr);
    RendererCreateInfo ci;
    CHECK(r->initialize(ci));
    if (!nr->createDevice(nullptr, nullptr, 0, 0)) {
        printf("sem Vulkan utilizável nesta máquina: pulando\n");
        return 77;
    }
    VulkanContext &ctx = nr->context();
    VulkanMemory &mem = nr->memory();
    CHECK(ctx.headless());
    const MemoryStats base = mem.stats(); /* o anel de staging já é um buffer vivo */

    void *guest = nullptr;
    {
        /* ---- buffers: malha estática por staging e buffer dinâmico mapeado ---- */
        std::vector<float> verts(3 * 1024, 1.0f);
        Buffer vb = mem.uploadStatic(verts.data(), verts.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     "teste.malha_sbc1");
        CHECK(vb && !vb.mapped());
        BufferDesc dd;
        dd.size = 4096;
        dd.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        dd.memory = MemoryUsage::Stream;
        dd.debugName = "teste.dinamico";
        Buffer dyn = mem.createBuffer(dd);
        CHECK(dyn.mapped() != nullptr);
        std::memset(dyn.mapped(), 0xAB, 4096);
        dyn.flush(0, VK_WHOLE_SIZE);

        /* ---- textura com 2 mips por staging ---- */
        ImageDesc td;
        td.format = VK_FORMAT_R8G8B8A8_UNORM;
        td.extent = {64, 64, 1};
        td.mipLevels = 2;
        td.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        td.debugName = "teste.textura";
        Image tex = mem.createImage(td);
        std::vector<uint8_t> px(64 * 64 * 4, 0x7F);
        ImageUploadRegion regs[2];
        regs[0].extent = {64, 64, 1};
        regs[0].data = px.data();
        regs[0].bytes = 64 * 64 * 4;
        regs[1].mipLevel = 1;
        regs[1].extent = {32, 32, 1};
        regs[1].data = px.data();
        regs[1].bytes = 32 * 32 * 4;
        mem.uploadImage(tex, regs, 2);

        /* ---- alvos: cor amostrável + profundidade/estêncil (transitória se o device deixar) ---- */
        ImageDesc rd;
        rd.format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.extent = {128, 128, 1};
        rd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        rd.debugName = "teste.rt";
        Image rt = mem.createImage(rd);
        ImageDesc zd;
        zd.format = ctx.pickDepthFormat(true);
        zd.extent = {128, 128, 1};
        zd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        zd.memory = nr->wantsTransientDepth() ? MemoryUsage::Transient : MemoryUsage::GpuOnly;
        zd.debugName = "teste.profundidade";
        Image ds = mem.createImage(zd);
        CHECK(ds.transient() == nr->wantsTransientDepth());

        MemoryStats st = mem.stats();
        CHECK_EQ_U32(st.liveBuffers - base.liveBuffers, 2);
        CHECK_EQ_U32(st.liveImages - base.liveImages, 3);

        /* ---- buffer sobre memória "do guest" (VK_EXT_external_memory_host) ---- */
        Buffer hb;
        if (ctx.caps().externalMemoryHost) {
            const size_t al = (size_t)ctx.caps().minImportedHostPointerAlignment;
            guest = aligned_mem(al, al * 4);
            hb = mem.importHostBuffer(guest, al * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "teste.guest");
            CHECK(hb.mapped() == guest);
        } else {
            printf("(device sem VK_EXT_external_memory_host: importação não testada)\n");
        }

        /* ---- quadros: o oráculo libera DONT_CARE na profundidade, a cor (lida) fica STORE ---- */
        re5vk_tbdr_sub colorSub, dsSub;
        re5vk_tbdr_sub_reset(&colorSub);
        re5vk_tbdr_sub_reset(&dsSub);
        bool depthDc[6] = {};
        bool colorKept = true;
        for (int f = 0; f < 6; f++) {
            CHECK(nr->beginFrame());
            PassDesc d;
            d.colorCount = 1;
            d.color[0].image = &rt;
            d.color[0].clearAtStart = true;
            d.color[0].tbdr = &colorSub;
            d.hasDepth = d.hasStencil = true;
            d.depth.image = &ds;
            d.depth.clearAtStart = true;
            d.depth.clear.depthStencil.depth = 1.0f;
            d.depth.tbdr = &dsSub;
            d.stencil.clearAtStart = true;
            nr->beginPass(d);
            nr->endPass();
            /* a cor é amostrada depois (como uma textura de pós-processamento) */
            re5vk_tbdr_mark_read(&nr->tbdrOracle(), &colorSub, RE5VK_TBDR_COLOR);
            depthDc[f] = !d.depth.readAfter;
            colorKept = colorKept && d.color[0].readAfter;
            CHECK(rt.layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || !d.color[0].readAfter);
            nr->endFrame();
        }
        CHECK(!depthDc[0]);   /* primeiro quadro: sem histórico, STORE seguro */
        CHECK(depthDc[5]);    /* quadros estáveis sem leitura: DONT_CARE */
        CHECK(colorKept);     /* a cor é lida: nunca DONT_CARE */
        CHECK(tex.layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        printf("passes em cache: %zu, framebuffers: %zu\n", nr->cachedRenderPasses(), nr->cachedFramebuffers());
        CHECK(nr->cachedRenderPasses() >= 1 && nr->cachedRenderPasses() <= 3);
        CHECK_EQ_U32(nr->cachedFramebuffers(), 1);

        /* ---- clear no meio do passe: Mali reabre o passe com loadOp=CLEAR ---- */
        CHECK(nr->beginFrame());
        {
            PassDesc d;
            d.colorCount = 1;
            d.color[0].image = &rt;
            d.color[0].clearAtStart = true;
            nr->beginPass(d);
            VkClearColorValue red{};
            red.float32[0] = 1.0f;
            nr->midPassClear(1u, false, false, red, VkClearDepthStencilValue{1.0f, 0});
            CHECK(nr->inPass());
            nr->endPass();
        }
        nr->endFrame();

        /* ---- a imagem morre: o framebuffer dela sai do cache ---- */
        const size_t fbBefore = nr->cachedFramebuffers();
        rt = Image{};
        CHECK(nr->cachedFramebuffers() < fbBefore);
    }
    /* Tudo destruído: depois de quadros suficientes nada fica pendente. */
    for (int f = 0; f < 3; f++) {
        CHECK(nr->beginFrame());
        nr->endFrame();
    }
    MemoryStats end = mem.stats();
    CHECK_EQ_U32(end.liveBuffers, base.liveBuffers);
    CHECK_EQ_U32(end.liveImages, base.liveImages);
    CHECK_EQ_U32(end.pendingRetire, 0);

    r.reset(); /* shutdown: VulkanMemory::destroy aborta se algo vazou */
    if (guest) aligned_free(guest);
    CHECK_DONE();
}
