# libdxvk_d3d9.so: DXVK native 1.9.2b (D3D9 -> Vulkan) para o Android ARM64.
# A fonte vem de android/dxvk/prepare.py; scripts/build_dxvk_android.sh faz tudo
# (preparar, configurar com o NDK, compilar e copiar para o jniLibs do app).
cmake_minimum_required(VERSION 3.22)
project(re5_dxvk_android C CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "DXVK Android: só arm64-v8a (toolchain do NDK)")
endif()
set(DXVK_SOURCE "" CACHE PATH "Fonte preparada por android/dxvk/prepare.py")
if(NOT EXISTS "${DXVK_SOURCE}/PINNED_REVISION" OR NOT EXISTS "${DXVK_SOURCE}/generated")
  message(FATAL_ERROR "DXVK_SOURCE=${DXVK_SOURCE} não foi preparada: rode android/dxvk/prepare.py")
endif()
# Patches desatualizados se a receita do NFSU2 mudou depois do prepare.
set(DXVK_RECIPE "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/nfsu2-reference/ports/dxvk_android/prepare.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DXVK_RECIPE}" "${DXVK_SOURCE}/ANDROID_PREPARE_SHA256")
file(SHA256 "${DXVK_RECIPE}" DXVK_EXPECTED_SHA256)
file(READ "${DXVK_SOURCE}/ANDROID_PREPARE_SHA256" DXVK_ACTUAL_SHA256)
string(STRIP "${DXVK_ACTUAL_SHA256}" DXVK_ACTUAL_SHA256)
if(NOT DXVK_ACTUAL_SHA256 STREQUAL DXVK_EXPECTED_SHA256)
  message(FATAL_ERROR "Patches da DXVK desatualizados: rode android/dxvk/prepare.py de novo")
endif()

file(GLOB UTIL "${DXVK_SOURCE}/src/util/*.cpp" "${DXVK_SOURCE}/src/util/com/*.cpp"
  "${DXVK_SOURCE}/src/util/config/*.cpp" "${DXVK_SOURCE}/src/util/log/*.cpp"
  "${DXVK_SOURCE}/src/util/sha1/*.c" "${DXVK_SOURCE}/src/util/sha1/*.cpp"
  "${DXVK_SOURCE}/src/util/sync/*.cpp")
list(FILTER UTIL EXCLUDE REGEX "util_gdi.cpp$")
list(APPEND UTIL "${DXVK_SOURCE}/src/util/platform/util_env_linux.cpp"
  "${DXVK_SOURCE}/src/util/platform/util_luid_linux.cpp"
  "${DXVK_SOURCE}/src/util/platform/util_string_linux.cpp"
  "${DXVK_SOURCE}/src/util/platform/thread_native.cpp")
file(GLOB GRAPHICS "${DXVK_SOURCE}/src/dxso/*.cpp" "${DXVK_SOURCE}/src/spirv/*.cpp"
  "${DXVK_SOURCE}/src/vulkan/*.cpp" "${DXVK_SOURCE}/src/dxvk/*.cpp"
  "${DXVK_SOURCE}/src/dxvk/hud/*.cpp" "${DXVK_SOURCE}/src/d3d9/*.cpp")
list(FILTER GRAPHICS EXCLUDE REGEX "dxvk_open(vr|xr).cpp$")

add_library(dxvk_d3d9 SHARED ${UTIL} ${GRAPHICS} android_wsi.cpp)
target_compile_features(dxvk_d3d9 PRIVATE cxx_std_17)
target_compile_definitions(dxvk_d3d9 PRIVATE DXVK_NATIVE DXVK_PLATFORM_LINUX DXVK_WSI_ANDROID NOMINMAX
  VK_USE_PLATFORM_ANDROID_KHR)
target_include_directories(dxvk_d3d9 PRIVATE "${DXVK_SOURCE}" "${DXVK_SOURCE}/include"
  "${DXVK_SOURCE}/src/util"
  "${DXVK_SOURCE}/include/native" "${DXVK_SOURCE}/include/native/windows"
  "${DXVK_SOURCE}/include/native/directx" "${DXVK_SOURCE}/generated")
target_compile_options(dxvk_d3d9 PRIVATE -O2 -Wno-deprecated-declarations)
# libc++ estática e escondida: a libre5.so tem a sua; entre as duas só passam
# interfaces COM. Páginas de 16 KB para os aparelhos novos.
target_link_options(dxvk_d3d9 PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
find_library(DXVK_VULKAN_LIBRARY NAMES vulkan REQUIRED)
target_link_libraries(dxvk_d3d9 PRIVATE ${DXVK_VULKAN_LIBRARY} android log)
