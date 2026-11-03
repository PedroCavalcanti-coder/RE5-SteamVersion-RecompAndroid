/*
 * vkprobe.c - o que o Vulkan do aparelho oferece do que a DXVK 2.6.2 pede (fase A4.3).
 *
 *   NDK: aarch64-linux-android29-clang -O1 -o vkprobe vkprobe.c -lvulkan
 *   adb push vkprobe /data/local/tmp/ && adb shell /data/local/tmp/vkprobe
 *
 * Imprime versão, driver, as features que a DXVK 2.6.2 liga (dxvk_adapter.cpp e
 * d3d9_device.cpp) e as extensões que ela exige ou usa.
 */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define F(s, name) printf("  %-40s %s\n", #name, (s).name ? "sim" : "NAO")

static const char *k_exts[] = {
    "VK_EXT_robustness2", "VK_KHR_swapchain", "VK_EXT_extended_dynamic_state3", "VK_EXT_custom_border_color",
    "VK_EXT_depth_clip_enable", "VK_EXT_transform_feedback", "VK_EXT_vertex_attribute_divisor",
    "VK_KHR_load_store_op_none", "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_EXT_graphics_pipeline_library",
    "VK_KHR_pipeline_library", "VK_EXT_line_rasterization", "VK_EXT_memory_priority", "VK_EXT_memory_budget",
    "VK_EXT_attachment_feedback_loop_layout", "VK_EXT_non_seamless_cube_map", "VK_EXT_depth_bias_control",
    "VK_EXT_shader_module_identifier", "VK_EXT_multi_draw", "VK_EXT_conservative_rasterization",
    "VK_EXT_shader_stencil_export", "VK_EXT_swapchain_maintenance1", "VK_KHR_present_id", "VK_KHR_present_wait",
    NULL};

int main(void)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) { printf("vkCreateInstance falhou\n"); return 1; }
    uint32_t n = 4;
    VkPhysicalDevice pd[4];
    vkEnumeratePhysicalDevices(inst, &n, pd);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        printf("%s: Vulkan %u.%u.%u, driver 0x%X\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        VkPhysicalDeviceRobustness2FeaturesEXT rb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &rb};
        VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
        VkPhysicalDeviceVulkan11Features v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
        VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
        vkGetPhysicalDeviceFeatures2(pd[i], &f);
        printf("core (DXVK liga):\n");
        F(f.features, robustBufferAccess); F(f.features, fullDrawIndexUint32); F(f.features, imageCubeArray);
        F(f.features, independentBlend); F(f.features, geometryShader); F(f.features, sampleRateShading);
        F(f.features, multiDrawIndirect); F(f.features, depthClamp); F(f.features, depthBiasClamp);
        F(f.features, fillModeNonSolid); F(f.features, multiViewport); F(f.features, textureCompressionBC);
        F(f.features, textureCompressionETC2); F(f.features, textureCompressionASTC_LDR);
        F(f.features, occlusionQueryPrecise); F(f.features, shaderClipDistance); F(f.features, shaderCullDistance);
        F(f.features, wideLines); F(f.features, samplerAnisotropy);
        printf("1.1/1.2/1.3:\n");
        F(v11, shaderDrawParameters); F(v12, samplerMirrorClampToEdge); F(v12, hostQueryReset);
        F(v12, timelineSemaphore); F(v12, vulkanMemoryModel); F(v12, bufferDeviceAddress);
        F(v13, synchronization2); F(v13, dynamicRendering); F(v13, maintenance4); F(v13, shaderDemoteToHelperInvocation);
        printf("robustness2:\n");
        F(rb, robustBufferAccess2); F(rb, robustImageAccess2); F(rb, nullDescriptor);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd[i], NULL, &ne, NULL);
        VkExtensionProperties e[512];
        if (ne > 512) ne = 512;
        vkEnumerateDeviceExtensionProperties(pd[i], NULL, &ne, e);
        printf("extensões (%u no total):\n", ne);
        for (const char **x = k_exts; *x; x++) {
            int has = 0;
            for (uint32_t k = 0; k < ne; k++) has |= !strcmp(e[k].extensionName, *x);
            printf("  %-40s %s\n", *x, has ? "sim" : "NAO");
        }
    }
    vkDestroyInstance(inst, NULL);
    return 0;
}
