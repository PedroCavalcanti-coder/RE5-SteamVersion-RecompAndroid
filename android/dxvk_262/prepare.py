"""Prepara a DXVK 2.6.2 de third_party/dxvk-2.6.2 para o Android ARM64 (Galaxy A54 / Mali e Adreno)."""
import argparse
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "third_party/dxvk-2.6.2"
SHADERS_SRC = ROOT / "build/dxvk/2.6.2/build/src"

parser = argparse.ArgumentParser()
parser.add_argument("output", type=pathlib.Path)
args = parser.parse_args()

out_src = args.output
out_src.mkdir(parents=True, exist_ok=True)

# 1. Copia subpastas necessárias
for d in ("src/d3d9", "src/dxso", "src/dxbc", "src/dxvk", "src/spirv", "src/util", "src/vulkan", "src/wsi", "include"):
    src_dir = SOURCE / d
    dst_dir = out_src / d
    if src_dir.exists():
        shutil.copytree(src_dir, dst_dir, dirs_exist_ok=True)


# 2. Copia headers dos shaders já gerados
generated = out_src / "generated"
generated.mkdir(exist_ok=True)
shader_count = 0
for h in SHADERS_SRC.rglob("*.h"):
    shutil.copy2(h, generated)
    shader_count += 1
print(f"Copiados {shader_count} headers de shaders pré-compilados para {generated}")

# 3. Gera version.h e buildenv.h
(out_src / "version.h").write_text('#pragma once\n#define DXVK_VERSION "2.6.2-android-arm64"\n')
(out_src / "buildenv.h").write_text(
    '#pragma once\n#define DXVK_TARGET "aarch64"\n#define DXVK_COMPILER "clang"\n#define DXVK_COMPILER_VERSION "18"\n'
)

def patch_file(rel_path, old, new):
    target = out_src / rel_path
    if not target.exists():
        raise SystemExit(f"Arquivo não existe para patch: {target}")
    content = target.read_text(encoding="utf-8")
    if old not in content:
        raise SystemExit(f"Trecho não encontrado em {rel_path}:\n{old[:80]!r}")
    content = content.replace(old, new, 1)
    target.write_text(content, encoding="utf-8")
    print(f"Patched: {rel_path}")

# 3b. Remove includes não usados de d3d11/dxgi em com_guid.cpp e inclui log.h
patch_file(
    "src/util/com/com_guid.cpp",
    '#include "../../d3d11/d3d11_interfaces.h"\n\n#include "../../dxgi/dxgi_interfaces.h"\n',
    '#include "../log/log.h"\n'
)



# 4. Patch: extRobustness2 como opcional para suportar Mali (que não tem VK_EXT_robustness2)
patch_file(
    "src/dxvk/dxvk_extensions.h",
    "DxvkExt extRobustness2                    = { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,                       DxvkExtMode::Required };",
    "DxvkExt extRobustness2                    = { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,                       DxvkExtMode::Optional };"
)

# 5. Patch: dxvk_adapter.cpp - features opcionais de robustness2 e multiDrawIndirect
patch_file(
    "src/dxvk/dxvk_adapter.cpp",
    "    enabledFeatures.extRobustness2.robustBufferAccess2 = VK_TRUE;\n"
    "    enabledFeatures.extRobustness2.robustImageAccess2 = m_deviceFeatures.extRobustness2.robustImageAccess2;\n"
    "    enabledFeatures.extRobustness2.nullDescriptor = VK_TRUE;",
    "    enabledFeatures.extRobustness2.robustBufferAccess2 = m_deviceFeatures.extRobustness2.robustBufferAccess2;\n"
    "    enabledFeatures.extRobustness2.robustImageAccess2 = m_deviceFeatures.extRobustness2.robustImageAccess2;\n"
    "    enabledFeatures.extRobustness2.nullDescriptor = m_deviceFeatures.extRobustness2.nullDescriptor;"
)

patch_file(
    "src/dxvk/dxvk_adapter.cpp",
    "    enabledFeatures.core.features.multiDrawIndirect = VK_TRUE;",
    "    enabledFeatures.core.features.multiDrawIndirect = m_deviceFeatures.core.features.multiDrawIndirect;"
)

# 6. Patch: d3d9_device.cpp - features de desktop tornadas condicionais para não falhar vkCreateDevice no Mali
d3d9_features_old = (
    "    // Geometry shaders are used for some meta ops\n"
    "    enabled.core.features.geometryShader = VK_TRUE;\n"
    "    enabled.core.features.robustBufferAccess = VK_TRUE;\n\n"
    "    enabled.vk12.samplerMirrorClampToEdge = VK_TRUE;\n\n"
    "    enabled.vk13.shaderDemoteToHelperInvocation = VK_TRUE;\n\n"
    "    enabled.extMemoryPriority.memoryPriority = supported.extMemoryPriority.memoryPriority;\n\n"
    "    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor;\n"
    "    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor;\n\n"
    "    // ProcessVertices\n"
    "    enabled.core.features.vertexPipelineStoresAndAtomics = supported.core.features.vertexPipelineStoresAndAtomics;\n"
    "    enabled.vk12.shaderInt8 = supported.vk12.shaderInt8;\n\n"
    "    // DXVK Meta\n"
    "    enabled.core.features.imageCubeArray = VK_TRUE;\n\n"
    "    // SM1 level hardware\n"
    "    enabled.core.features.depthClamp = VK_TRUE;\n"
    "    enabled.core.features.depthBiasClamp = VK_TRUE;\n"
    "    enabled.core.features.fillModeNonSolid = VK_TRUE;\n"
    "    enabled.core.features.pipelineStatisticsQuery = supported.core.features.pipelineStatisticsQuery;\n"
    "    enabled.core.features.sampleRateShading = VK_TRUE;\n"
    "    enabled.core.features.samplerAnisotropy = supported.core.features.samplerAnisotropy;\n"
    "    enabled.core.features.shaderClipDistance = VK_TRUE;\n"
    "    enabled.core.features.shaderCullDistance = VK_TRUE;\n\n"
    "    // Ensure we support real BC formats and unofficial vendor ones.\n"
    "    enabled.core.features.textureCompressionBC = VK_TRUE;\n\n"
    "    // SM2 level hardware\n"
    "    enabled.core.features.occlusionQueryPrecise = VK_TRUE;\n\n"
    "    // SM3 level hardware\n"
    "    enabled.core.features.multiViewport = VK_TRUE;\n"
    "    enabled.core.features.independentBlend = VK_TRUE;"
)

d3d9_features_new = (
    "    // Geometry shaders are used for some meta ops\n"
    "    enabled.core.features.geometryShader = supported.core.features.geometryShader;\n"
    "    enabled.core.features.robustBufferAccess = supported.core.features.robustBufferAccess;\n\n"
    "    enabled.vk12.samplerMirrorClampToEdge = supported.vk12.samplerMirrorClampToEdge;\n\n"
    "    enabled.vk13.shaderDemoteToHelperInvocation = supported.vk13.shaderDemoteToHelperInvocation;\n\n"
    "    enabled.extMemoryPriority.memoryPriority = supported.extMemoryPriority.memoryPriority;\n\n"
    "    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor;\n"
    "    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor;\n\n"
    "    // ProcessVertices\n"
    "    enabled.core.features.vertexPipelineStoresAndAtomics = supported.core.features.vertexPipelineStoresAndAtomics;\n"
    "    enabled.vk12.shaderInt8 = supported.vk12.shaderInt8;\n\n"
    "    // DXVK Meta\n"
    "    enabled.core.features.imageCubeArray = supported.core.features.imageCubeArray;\n\n"
    "    // SM1 level hardware\n"
    "    enabled.core.features.depthClamp = supported.core.features.depthClamp;\n"
    "    enabled.core.features.depthBiasClamp = supported.core.features.depthBiasClamp;\n"
    "    enabled.core.features.fillModeNonSolid = supported.core.features.fillModeNonSolid;\n"
    "    enabled.core.features.pipelineStatisticsQuery = supported.core.features.pipelineStatisticsQuery;\n"
    "    enabled.core.features.sampleRateShading = supported.core.features.sampleRateShading;\n"
    "    enabled.core.features.samplerAnisotropy = supported.core.features.samplerAnisotropy;\n"
    "    enabled.core.features.shaderClipDistance = supported.core.features.shaderClipDistance;\n"
    "    enabled.core.features.shaderCullDistance = supported.core.features.shaderCullDistance;\n\n"
    "    // Ensure we support real BC formats and unofficial vendor ones.\n"
    "    enabled.core.features.textureCompressionBC = supported.core.features.textureCompressionBC;\n\n"
    "    // SM2 level hardware\n"
    "    enabled.core.features.occlusionQueryPrecise = supported.core.features.occlusionQueryPrecise;\n\n"
    "    // SM3 level hardware\n"
    "    enabled.core.features.multiViewport = supported.core.features.multiViewport;\n"
    "    enabled.core.features.independentBlend = supported.core.features.independentBlend;"
)
patch_file("src/d3d9/d3d9_device.cpp", d3d9_features_old, d3d9_features_new)

# 7. Patch: wsi_platform.cpp - adiciona AndroidWSI
patch_file(
    "src/wsi/wsi_platform.cpp",
    "#if defined(DXVK_WSI_GLFW)\n    &GlfwWSI,\n#endif",
    "#if defined(DXVK_WSI_GLFW)\n    &GlfwWSI,\n#endif\n#if defined(DXVK_WSI_ANDROID)\n    &AndroidWSI,\n#endif"
)
patch_file(
    "src/wsi/wsi_platform.cpp",
    "namespace dxvk::wsi {",
    "namespace dxvk::wsi {\n#if defined(DXVK_WSI_ANDROID)\n  extern const WsiBootstrap AndroidWSI;\n#endif"
)
patch_file(
    "src/wsi/wsi_platform.cpp",
    "if (hint == b->name && b->createDriver(&s_driver)) {",
    'if ((hint == b->name || hint == "SDL2" || hint == "Android") && b->createDriver(&s_driver)) {'
)

# 8. Patch: dxvk_presenter.cpp - tolera VK_SUBOPTIMAL_KHR no Android
patch_file(
    "src/dxvk/dxvk_presenter.cpp",
    "if (m_acquireStatus != VK_SUCCESS || !m_swapchain) {",
    "if ((m_acquireStatus != VK_SUCCESS && m_acquireStatus != VK_SUBOPTIMAL_KHR) || !m_swapchain) {"
)
patch_file(
    "src/dxvk/dxvk_presenter.cpp",
    "    if (status == VK_SUCCESS) {",
    "    if (status == VK_SUCCESS || status == VK_SUBOPTIMAL_KHR) {"
)

# 9. Patch: log.cpp - logcat no Android
patch_file(
    "src/util/log/log.cpp",
    '#include "log.h"\n',
    '#include "log.h"\n#if defined(__ANDROID__)\n#include <android/log.h>\n#endif\n'
)
patch_file(
    "src/util/log/log.cpp",
    "          std::cerr << adjusted;",
    "#if defined(__ANDROID__)\n"
    '          if (level >= LogLevel::Warn) __android_log_print(level >= LogLevel::Error ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, "DXVK", "%s", adjusted.c_str());\n'
    "#else\n"
    "          std::cerr << adjusted;\n"
    "#endif"
)

# 10. Patch: vulkan_loader.cpp - suporte a adrenotools
patch_file(
    "src/vulkan/vulkan_loader.cpp",
    '#include "vulkan_loader.h"\n',
    '#include "vulkan_loader.h"\nextern "C" PFN_vkGetInstanceProcAddr dxvkAndroidGetInstanceProcAddr();\n'
)
patch_file(
    "src/vulkan/vulkan_loader.cpp",
    "  static std::pair<HMODULE, PFN_vkGetInstanceProcAddr> loadVulkanLibrary() {",
    "  static std::pair<HMODULE, PFN_vkGetInstanceProcAddr> loadVulkanLibrary() {\n"
    "    auto customGipa = dxvkAndroidGetInstanceProcAddr();\n"
    "    if (customGipa) return std::make_pair(nullptr, customGipa);\n"
)

# 11. Patch: dxvk_swapchain_blitter.cpp - sem nullDescriptor no Mali, descritores nulos causam crash no driver
blitter_null_desc_old = """    if (m_gammaView) {
      gammaDescriptor.imageView = m_gammaView->handle();
      gammaDescriptor.imageLayout = m_gammaView->image()->info().layout;
    }

    VkDescriptorImageInfo hudDescriptor = { };

    if (m_hudView) {
      hudDescriptor.imageView = m_hudView->handle();
      hudDescriptor.imageLayout = m_hudImage->info().layout;
    }

    VkDescriptorImageInfo cursorDescriptor = { };
    cursorDescriptor.sampler = m_samplerCursorNearest->handle();

    if (m_cursorView) {
      VkExtent3D extent = m_cursorImage->info().extent;

      if (m_cursorRect.extent.width != extent.width
       || m_cursorRect.extent.height != extent.height)
        cursorDescriptor.sampler = m_samplerCursorLinear->handle();

      cursorDescriptor.imageLayout = m_cursorImage->info().layout;
      cursorDescriptor.imageView = m_cursorView->handle();
    }"""

blitter_null_desc_new = """    if (m_gammaView) {
      gammaDescriptor.imageView = m_gammaView->handle();
      gammaDescriptor.imageLayout = m_gammaView->image()->info().layout;
    } else {
      gammaDescriptor.imageView = srcView->handle();
      gammaDescriptor.imageLayout = srcView->image()->info().layout;
    }

    VkDescriptorImageInfo hudDescriptor = { };

    if (m_hudView) {
      hudDescriptor.imageView = m_hudView->handle();
      hudDescriptor.imageLayout = m_hudImage->info().layout;
    } else {
      hudDescriptor.imageView = srcView->handle();
      hudDescriptor.imageLayout = srcView->image()->info().layout;
    }

    VkDescriptorImageInfo cursorDescriptor = { };
    cursorDescriptor.sampler = m_samplerCursorNearest->handle();

    if (m_cursorView) {
      VkExtent3D extent = m_cursorImage->info().extent;

      if (m_cursorRect.extent.width != extent.width
       || m_cursorRect.extent.height != extent.height)
        cursorDescriptor.sampler = m_samplerCursorLinear->handle();

      cursorDescriptor.imageLayout = m_cursorImage->info().layout;
      cursorDescriptor.imageView = m_cursorView->handle();
    } else {
      cursorDescriptor.imageView = srcView->handle();
      cursorDescriptor.imageLayout = srcView->image()->info().layout;
    }"""
patch_file("src/dxvk/dxvk_swapchain_blitter.cpp", blitter_null_desc_old, blitter_null_desc_new)

# 12. Patch: dxvk_unbound.h e dxvk_unbound.cpp - dummy image view para GPUs sem nullDescriptor (Mali)
unbound_h_old = """    VkSampler samplerHandle();
    
  private:
    
    DxvkDevice*             m_device;

    std::atomic<VkSampler>  m_samplerHandle = { VK_NULL_HANDLE };
    std::atomic<VkBuffer>   m_bufferHandle  = { VK_NULL_HANDLE };

    dxvk::mutex             m_mutex;
    Rc<DxvkSampler>         m_sampler;
    Rc<DxvkBuffer>          m_buffer;"""

unbound_h_new = """    VkSampler samplerHandle();

    VkImageView imageViewHandle();
    
  private:
    
    DxvkDevice*             m_device;

    std::atomic<VkSampler>   m_samplerHandle   = { VK_NULL_HANDLE };
    std::atomic<VkBuffer>    m_bufferHandle    = { VK_NULL_HANDLE };
    std::atomic<VkImageView> m_imageViewHandle = { VK_NULL_HANDLE };

    dxvk::mutex             m_mutex;
    Rc<DxvkSampler>         m_sampler;
    Rc<DxvkBuffer>          m_buffer;
    Rc<DxvkImage>           m_image;
    Rc<DxvkImageView>       m_imageView;"""
patch_file("src/dxvk/dxvk_unbound.h", unbound_h_old, unbound_h_new)

unbound_cpp_impl = """  VkImageView DxvkUnboundResources::imageViewHandle() {
    VkImageView view = m_imageViewHandle.load();

    if (likely(view != VK_NULL_HANDLE))
      return view;

    std::lock_guard lock(m_mutex);
    view = m_imageViewHandle.load();

    if (view)
      return view;

    DxvkImageCreateInfo info;
    info.type        = VK_IMAGE_TYPE_2D;
    info.format      = VK_FORMAT_R8G8B8A8_UNORM;
    info.flags       = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent      = { 1, 1, 1 };
    info.numLayers   = 6;
    info.mipLevels   = 1;
    info.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                     | VK_IMAGE_USAGE_SAMPLED_BIT
                     | VK_IMAGE_USAGE_STORAGE_BIT;
    info.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT
                     | m_device->getShaderPipelineStages();
    info.access      = VK_ACCESS_SHADER_READ_BIT;
    info.layout      = VK_IMAGE_LAYOUT_GENERAL;
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;

    m_image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    DxvkImageViewKey viewInfo = { };
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.mipIndex = 0u;
    viewInfo.mipCount = 1u;
    viewInfo.layerIndex = 0u;
    viewInfo.layerCount = 1u;

    m_imageView = m_image->createView(viewInfo);
    view = m_imageView->handle();

    m_imageViewHandle.store(view, std::memory_order_release);
    return view;
  }

}"""
unbound_cpp_old = """    std::memset(buffer->mapPtr(0), 0, info.size);
    return buffer;
  }
  
}"""

unbound_cpp_new = """    std::memset(buffer->mapPtr(0), 0, info.size);
    return buffer;
  }
  
""" + unbound_cpp_impl

patch_file("src/dxvk/dxvk_unbound.cpp", unbound_cpp_old, unbound_cpp_new)

# 13. Patch: dxvk_context.cpp - usa dummy image view em vez de nullDescriptor
patch_file(
    "src/dxvk/dxvk_context.cpp",
    """            } else {
              descriptorInfo.image.sampler = VK_NULL_HANDLE;
              descriptorInfo.image.imageView = VK_NULL_HANDLE;
              descriptorInfo.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
          } break;

          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:""",
    """            } else {
              descriptorInfo.image.sampler = VK_NULL_HANDLE;
              descriptorInfo.image.imageView = m_common->dummyResources().imageViewHandle();
              descriptorInfo.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
          } break;

          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:"""
)

patch_file(
    "src/dxvk/dxvk_context.cpp",
    """            } else {
              descriptorInfo.image.sampler = VK_NULL_HANDLE;
              descriptorInfo.image.imageView = VK_NULL_HANDLE;
              descriptorInfo.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
          } break;

          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:""",
    """            } else {
              descriptorInfo.image.sampler = VK_NULL_HANDLE;
              descriptorInfo.image.imageView = m_common->dummyResources().imageViewHandle();
              descriptorInfo.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
          } break;

          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:"""
)

patch_file(
    "src/dxvk/dxvk_context.cpp",
    """            } else {
              descriptorInfo.image.sampler = m_common->dummyResources().samplerHandle();
              descriptorInfo.image.imageView = VK_NULL_HANDLE;
              descriptorInfo.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
          } break;

          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:""",
    """            } else {
              descriptorInfo.image.sampler = m_common->dummyResources().samplerHandle();
              descriptorInfo.image.imageView = m_common->dummyResources().imageViewHandle();
              descriptorInfo.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
          } break;

          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:"""
)

# 14. Buffers não ligados sem nullDescriptor (Mali r38, A54, 20/11/2026): a 2.6.2 ainda deixava
#     VK_NULL_HANDLE em uniform/storage buffer e texel buffer não ligados; o driver do Mali derruba o
#     processo em vkUpdateDescriptorSets (vulkan::descriptor_set::write_descriptor_set, SIGSEGV em
#     0x28) logo no primeiro quadro 3D. Passam a usar o buffer zerado de reserva e uma view dele.
def patch_all(rel_path, old, new):
    target = out_src / rel_path
    content = target.read_text(encoding="utf-8")
    n = content.count(old)
    if not n:
        raise SystemExit(f"Trecho não encontrado em {rel_path}:\n{old[:80]!r}")
    target.write_text(content.replace(old, new), encoding="utf-8")
    print(f"Patched ({n}x): {rel_path}")

patch_file(
    "src/dxvk/dxvk_unbound.h",
    "    VkImageView imageViewHandle();",
    "    VkImageView imageViewHandle();\n\n"
    "    /** \\brief View R32_UINT do buffer zerado (texel buffer não ligado, sem nullDescriptor) */\n"
    "    VkBufferView bufferViewHandle();"
)
patch_file(
    "src/dxvk/dxvk_unbound.h",
    "    Rc<DxvkImageView>       m_imageView;",
    "    Rc<DxvkImageView>       m_imageView;\n"
    "    std::atomic<VkBufferView> m_bufferViewHandle = { VK_NULL_HANDLE };\n"
    "    Rc<DxvkBufferView>      m_bufferView;"
)
patch_file(
    "src/dxvk/dxvk_unbound.cpp",
    "    m_imageViewHandle.store(view, std::memory_order_release);\n    return view;\n  }\n",
    "    m_imageViewHandle.store(view, std::memory_order_release);\n    return view;\n  }\n\n"
    "  VkBufferView DxvkUnboundResources::bufferViewHandle() {\n"
    "    VkBufferView view = m_bufferViewHandle.load();\n\n"
    "    if (likely(view != VK_NULL_HANDLE))\n"
    "      return view;\n\n"
    "    bufferHandle();   /* cria o buffer antes de pegar o mutex (ele também o pega) */\n"
    "    std::lock_guard lock(m_mutex);\n"
    "    view = m_bufferViewHandle.load();\n\n"
    "    if (view)\n"
    "      return view;\n\n"
    "    DxvkBufferViewKey key;\n"
    "    key.format = VK_FORMAT_R32_UINT;\n"
    "    key.usage  = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;\n"
    "    key.offset = 0;\n"
    "    key.size   = MaxUniformBufferSize;\n"
    "    m_bufferView = m_buffer->createView(key);\n"
    "    view = m_bufferView->handle();\n\n"
    "    m_bufferViewHandle.store(view, std::memory_order_release);\n"
    "    return view;\n"
    "  }\n"
)
patch_all(
    "src/dxvk/dxvk_context.cpp",
    "              descriptorInfo.texelBuffer = VK_NULL_HANDLE;",
    "              descriptorInfo.texelBuffer = m_common->dummyResources().bufferViewHandle();"
)
patch_all(
    "src/dxvk/dxvk_context.cpp",
    "              descriptorInfo.buffer.buffer = VK_NULL_HANDLE;\n"
    "              descriptorInfo.buffer.offset = 0;\n"
    "              descriptorInfo.buffer.range = VK_WHOLE_SIZE;",
    "              descriptorInfo.buffer.buffer = m_common->dummyResources().bufferHandle();\n"
    "              descriptorInfo.buffer.offset = 0;\n"
    "              descriptorInfo.buffer.range = VK_WHOLE_SIZE;"
)

# 15. Stride de vértice estático (Mali r38, hipótese de 20/11/2026): com o stride dinâmico do
#     extended dynamic state (VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE + vkCmdBindVertexBuffers2)
#     a 2.6.2 e o backend nativo mostram linhas/faixas no Mali; a 1.9.2b (stride no pipeline) não.
#     Padrão: stride sempre no pipeline. DXVK_RE5_DYNAMIC_STRIDE=1 volta ao comportamento da DXVK.
patch_file(
    "src/dxvk/dxvk_context.cpp",
    "    bool oldDynamicStrides = m_flags.test(DxvkContextFlag::GpDynamicVertexStrides);\n"
    "    bool newDynamicStrides = true;",
    "    bool oldDynamicStrides = m_flags.test(DxvkContextFlag::GpDynamicVertexStrides);\n"
    "    static const bool re5DynamicStrides = [] {\n"
    "      const char* e = std::getenv(\"DXVK_RE5_DYNAMIC_STRIDE\");\n"
    "      return e && e[0] == '1';\n"
    "    }();\n"
    "    bool newDynamicStrides = re5DynamicStrides;"
)

print("DXVK 2.6.2 preparada com sucesso para Android ARM64!")
