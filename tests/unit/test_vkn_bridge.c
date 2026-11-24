/*
 * test_vkn_bridge.c - a ponte C do device D3D9 (runtime/src/gfx/vkn/vkn_bridge.h) numa GPU de
 * verdade, sem janela, em C11 (o header tem que servir ao C do device):
 *   - re5_vkn_open sobe instância/device pelo vk-bootstrap com EDS1+EDS2;
 *   - buffers host/device e imagem pelo VMA (map só no host);
 *   - VkRenderPass em cache: mesma chave -> mesmo handle; ops diferentes -> outro passe;
 *     posição vazia (RT1 sem RT0) aceita; passe compatível para pipelines;
 *   - framebuffer em cache e descartado por re5_vkn_forget_view;
 *   - bloco de memória do host importado e subdividido (se o device tiver a extensão);
 *   - re5_renderer_shutdown no fim: VulkanMemory aborta se algo vazou.
 * Sem Vulkan utilizável: sai com 77 (pulado).
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "re5/renderer.h"
#include "vkn_bridge.h"

#if defined(_WIN32)
#include <malloc.h>
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
static void *aligned_mem(size_t al, size_t n) { return _aligned_malloc(n, al); }
static void aligned_free(void *p) { _aligned_free(p); }
#else
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
static void *aligned_mem(size_t al, size_t n) { return aligned_alloc(al, n); }
static void aligned_free(void *p) { free(p); }
#endif

static re5_vkn_att color_att(VkFormat f, VkAttachmentLoadOp load, VkImageLayout l)
{
    re5_vkn_att a;
    memset(&a, 0, sizeof a);
    a.format = f;
    a.load = load;
    a.store = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.layout = l;
    return a;
}

int main(void)
{
    set_env("RE5_GFX", "vkmali");
    set_env("RE5_VKN_ANY_GPU", "1");
    set_env("RE5VK_CORE", "1");   /* modo Vulkan 1.1: abre sem exigir extended dynamic state */
    re5_vkn_swap_req req;
    memset(&req, 0, sizeof req);
    req.modes[0] = req.modes[1] = req.modes[2] = VK_PRESENT_MODE_FIFO_KHR;
    re5_vkn_device dev;
    re5_vkn_swapchain sc;
    if (re5_vkn_open(NULL, NULL, &req, &dev, &sc) != 0) {
        printf("sem Vulkan utilizável (ou sem EDS1/EDS2): pulando\n");
        return 77;
    }
    CHECK(dev.device != VK_NULL_HANDLE && dev.queue != VK_NULL_HANDLE);
    CHECK(sc.count == 0);   /* sem janela: sem swapchain */
    CHECK(dev.api_version >= VK_API_VERSION_1_1);
    CHECK(dev.core == 1);

    /* robustBufferAccess2 (VK_EXT_robustness2): ligado sempre que a GPU tem a extensão e o v1,
     * como a DXVK; sem ele, vértice/constante fora do buffer não lê 0 em Mali/Adreno. */
    {
        PFN_vkEnumerateDeviceExtensionProperties enum_ext =
            (PFN_vkEnumerateDeviceExtensionProperties)dev.gipa(dev.instance, "vkEnumerateDeviceExtensionProperties");
        PFN_vkGetPhysicalDeviceFeatures get_feat =
            (PFN_vkGetPhysicalDeviceFeatures)dev.gipa(dev.instance, "vkGetPhysicalDeviceFeatures");
        CHECK(enum_ext && get_feat);
        uint32_t n = 0;
        enum_ext(dev.physical, NULL, &n, NULL);
        VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext);
        enum_ext(dev.physical, NULL, &n, ext);
        int has_rb2 = 0;
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(ext[i].extensionName, "VK_EXT_robustness2")) has_rb2 = 1;
        free(ext);
        VkPhysicalDeviceFeatures have;
        get_feat(dev.physical, &have);
        CHECK(dev.features.robustBufferAccess == have.robustBufferAccess);
        CHECK(dev.robust_buffer_access2 == (has_rb2 && have.robustBufferAccess));
        printf("robustBufferAccess=%d robustBufferAccess2=%d (extensão %s)\n", (int)dev.features.robustBufferAccess,
               dev.robust_buffer_access2, has_rb2 ? "presente" : "ausente");
    }

    PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)dev.gdpa(dev.device, "vkCreateImageView");
    PFN_vkDestroyImageView destroy_view = (PFN_vkDestroyImageView)dev.gdpa(dev.device, "vkDestroyImageView");
    CHECK(create_view && destroy_view);

    /* ---- memória ---- */
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 65536;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    re5_vkn_alloc hb, db;
    VkBuffer host = re5_vkn_buffer_create(&bci, RE5_VKN_MEM_HOST, "teste.host", &hb);
    VkBuffer devb = re5_vkn_buffer_create(&bci, RE5_VKN_MEM_DEVICE, "teste.device", &db);
    CHECK(host && devb && hb.map != NULL);
    memset(hb.map, 0x5A, 65536);   /* coerente: sem flush */

    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent.width = ici.extent.height = 64;
    ici.extent.depth = 1;
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    re5_vkn_alloc ia;
    VkImage img = re5_vkn_image_create(&ici, "teste.rt", &ia);
    CHECK(img && ia.alloc && !ia.map);

    /* ---- passes ---- */
    re5_vkn_pass p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1;
    p.color[0] = color_att(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderPass rp1 = re5_vkn_render_pass(&p);
    VkRenderPass rp1b = re5_vkn_render_pass(&p);
    CHECK(rp1 && rp1 == rp1b);
    re5_vkn_pass q = p;
    q.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderPass rp2 = re5_vkn_render_pass(&q);
    CHECK(rp2 && rp2 != rp1);

    re5_vkn_pass gap;   /* RT1 sem RT0 */
    memset(&gap, 0, sizeof gap);
    gap.ncolor = 2;
    gap.color[1] = color_att(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL);
    CHECK(re5_vkn_render_pass(&gap) != VK_NULL_HANDLE);

    VkFormat fmts[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkRenderPass compat = re5_vkn_compat_pass(1, fmts, VK_FORMAT_UNDEFINED, 0);
    CHECK(compat && compat == re5_vkn_compat_pass(1, fmts, VK_FORMAT_UNDEFINED, 0));

    /* ---- framebuffer: cache e descarte pela view ---- */
    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof vci);
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = vci.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    CHECK(create_view(dev.device, &vci, NULL, &view) == VK_SUCCESS);
    VkExtent2D ext = {64, 64};
    VkFramebuffer fb1 = re5_vkn_framebuffer(&p, rp1, &view, 1, ext);
    VkFramebuffer fb2 = re5_vkn_framebuffer(&q, rp2, &view, 1, ext);   /* passe compatível: mesmo FB */
    CHECK(fb1 && fb1 == fb2);
    re5_vkn_forget_view(view);
    destroy_view(dev.device, view, NULL);

    /* ---- bloco de memória do host ---- */
    void *guest = NULL;
    if (dev.host_import) {
        const size_t size = (size_t)4 << 20;
        guest = aligned_mem(65536, size);
        void *blk = re5_vkn_host_block(guest, size);
        CHECK(blk != NULL);
        if (blk) {
            VkExternalMemoryBufferCreateInfo emb;
            memset(&emb, 0, sizeof emb);
            emb.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            emb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo gci = bci;
            gci.pNext = &emb;
            re5_vkn_alloc g1, g2;
            VkBuffer b1 = re5_vkn_host_buffer(blk, &gci, &g1);
            VkBuffer b2 = re5_vkn_host_buffer(blk, &gci, &g2);
            CHECK(b1 && b2 && g1.block == blk && g1.map && g2.map && g1.map != g2.map);
            CHECK((uint8_t *)g1.map >= (uint8_t *)guest && (uint8_t *)g1.map < (uint8_t *)guest + size);
            re5_vkn_buffer_destroy(b1, &g1);
            re5_vkn_buffer_destroy(b2, &g2);
            re5_vkn_host_block_free(blk);
        }
    } else {
        printf("(sem VK_EXT_external_memory_host: blocos do host não testados)\n");
    }

    re5_vkn_buffer_destroy(host, &hb);
    re5_vkn_buffer_destroy(devb, &db);
    re5_vkn_image_destroy(img, &ia);
    re5_renderer_shutdown();   /* aborta se a memória vazou */
    if (guest) aligned_free(guest);
    CHECK_DONE();
}
