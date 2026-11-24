# libdxvk_d3d9.so: DXVK 2.6.2 (D3D9 -> Vulkan) para o Android ARM64
cmake_minimum_required(VERSION 3.22)
project(re5_dxvk262_android C CXX)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "DXVK Android: só arm64-v8a (toolchain do NDK)")
endif()

set(DXVK_SOURCE "" CACHE PATH "Fonte preparada de dxvk-2.6.2")
if(NOT EXISTS "${DXVK_SOURCE}/generated")
  message(FATAL_ERROR "DXVK_SOURCE=${DXVK_SOURCE} não foi preparada: rode android/dxvk_262/prepare.py")
endif()

file(GLOB UTIL
  "${DXVK_SOURCE}/src/util/*.cpp"
  "${DXVK_SOURCE}/src/util/com/*.cpp"
  "${DXVK_SOURCE}/src/util/config/*.cpp"
  "${DXVK_SOURCE}/src/util/log/*.cpp"
  "${DXVK_SOURCE}/src/util/sha1/*.c"
  "${DXVK_SOURCE}/src/util/sha1/*.cpp"
  "${DXVK_SOURCE}/src/util/sync/*.cpp"
)

file(GLOB GRAPHICS
  "${DXVK_SOURCE}/src/dxso/*.cpp"
  "${DXVK_SOURCE}/src/spirv/*.cpp"
  "${DXVK_SOURCE}/src/vulkan/*.cpp"
  "${DXVK_SOURCE}/src/dxvk/*.cpp"
  "${DXVK_SOURCE}/src/dxvk/hud/*.cpp"
  "${DXVK_SOURCE}/src/d3d9/*.cpp"
)
list(FILTER GRAPHICS EXCLUDE REGEX "dxvk_open(vr|xr)\\.cpp$")

set(WSI_SRCS
  "${DXVK_SOURCE}/src/wsi/wsi_platform.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/android_wsi.cpp"
)

add_library(dxvk_d3d9 SHARED ${UTIL} ${GRAPHICS} ${WSI_SRCS})

target_compile_features(dxvk_d3d9 PRIVATE cxx_std_17)
target_compile_definitions(dxvk_d3d9 PRIVATE
  DXVK_NATIVE
  DXVK_PLATFORM_LINUX
  DXVK_WSI_ANDROID
  NOMINMAX
  VK_USE_PLATFORM_ANDROID_KHR
)

target_include_directories(dxvk_d3d9 PRIVATE
  "${DXVK_SOURCE}"
  "${DXVK_SOURCE}/include"
  "${DXVK_SOURCE}/include/vulkan/include"
  "${DXVK_SOURCE}/include/spirv/include"
  "${DXVK_SOURCE}/src/util"
  "${DXVK_SOURCE}/include/native"
  "${DXVK_SOURCE}/include/native/windows"
  "${DXVK_SOURCE}/include/native/directx"
  "${DXVK_SOURCE}/generated"
)

target_compile_options(dxvk_d3d9 PRIVATE -O2 -Wno-deprecated-declarations)
target_link_options(dxvk_d3d9 PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)

find_library(DXVK_VULKAN_LIBRARY NAMES vulkan REQUIRED)
target_link_libraries(dxvk_d3d9 PRIVATE ${DXVK_VULKAN_LIBRARY} android log)

if(ANDROID_STL STREQUAL "c++_static")
    target_link_libraries(dxvk_d3d9 PRIVATE -lc++_static -lc++abi)
endif()
