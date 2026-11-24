# runtime/ — o que o código gerado precisa para rodar fora do Windows x86.

# cpu.h do pcrecomp reescrito para a base do guest (scripts/gen_cpu_header.py).
set(RE5_GEN_INCLUDE "${CMAKE_BINARY_DIR}/gen_include")
set(RE5_CPU_H_SRC "${RE5_PCRECOMP_DIR}/runtime/recomp32_cpu/cpu.h")
add_custom_command(
    OUTPUT "${RE5_GEN_INCLUDE}/cpu.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RE5_GEN_INCLUDE}"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen_cpu_header.py"
            "${RE5_CPU_H_SRC}" "${RE5_GEN_INCLUDE}/cpu.h"
    DEPENDS "${RE5_CPU_H_SRC}" "${CMAKE_SOURCE_DIR}/scripts/gen_cpu_header.py"
    COMMENT "Gerando cpu.h (pcrecomp recomp32_cpu + base do guest)"
    VERBATIM)
add_custom_target(re5_cpu_header DEPENDS "${RE5_GEN_INCLUDE}/cpu.h")

# Tabela de métodos da ponte COM do Direct3D 9 (etapa 4.2, D4), da ordem do
# d3d9.h da DXVK native. Só o runtime depende dela: o código gerado não recompila.
set(RE5_D3D9_H "${CMAKE_SOURCE_DIR}/third_party/dxvk-native/include/native/directx/d3d9.h")
set(RE5_D3D9_METHODS "${RE5_GEN_INCLUDE}/re5_d3d9_methods.inc")
add_custom_command(
    OUTPUT "${RE5_D3D9_METHODS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RE5_GEN_INCLUDE}"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen_d3d9_bridge.py"
            "${RE5_D3D9_H}" "${RE5_D3D9_METHODS}"
    DEPENDS "${RE5_D3D9_H}" "${CMAKE_SOURCE_DIR}/scripts/gen_d3d9_bridge.py"
    COMMENT "Gerando a tabela de métodos do Direct3D 9 (d3d9.h)"
    VERBATIM)

# Interface comum ao runtime e ao código gerado: includes e flags que mudam a
# semântica. -fno-strict-aliasing e -fwrapv porque o código gerado lê os mesmos
# bytes como tipos diferentes e conta com aritmética de complemento de dois.
# -ffp-contract=off: o Clang (Android) funde a*b+c em fmadd por padrão e o GCC em modo
# GNU também (com FMA); o x86 do jogo arredonda a multiplicação e a soma em separado, e
# o PC e o Android do co-op têm que calcular os mesmos números (PLANO_MESTRE, fase 0.4).
add_library(re5_cpu INTERFACE)
add_dependencies(re5_cpu re5_cpu_header)
target_include_directories(re5_cpu INTERFACE "${RE5_GEN_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/third_party/sse2neon")
if(MSVC)
    target_compile_options(re5_cpu INTERFACE /utf-8)
else()
    # -fno-math-errno: o guest não lê errno; sqrt vira sqrtsd sem o desvio para a libm.
    target_compile_options(re5_cpu INTERFACE -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-math-errno)
endif()

# Fase P4 (PLANO_MESTRE_OTIMIZACAO): PGO e -march, sem mudar a semântica de ponto flutuante
# (-ffp-contract=off acima vale para todos). Fluxo do PGO, com a chave por build:
#   1. cmake -DRE5_PGO=gen ...      -> build, rodar scripts/bench_re5.py (grava .gcda em RE5_PGO_DIR)
#   2. cmake -DRE5_PGO=use ...      -> rebuild (o perfil vale enquanto o código gerado não mudar)
# RE5_MARCH=x86-64-v2 (SSE4.2/POPCNT) só em host x86-64; v3 traz FMA, que o -ffp-contract=off não usa.
set(RE5_PGO "off" CACHE STRING "PGO do runtime e do código gerado: off | gen | use")
set(RE5_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Pasta dos perfis do PGO")
# Padrão x86-64-v2 (SSE4.1/4.2, POPCNT: todo PC desde ~2010): floor/ceil/trunc/nearbyint do fistp e do
# frndint (o jogo converte float->int o tempo todo) viram um roundsd em vez de chamada à libm.
# RE5_MARCH="" volta ao x86-64 base.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(RE5_MARCH_DEFAULT "x86-64-v2")
else()
    set(RE5_MARCH_DEFAULT "")
endif()
set(RE5_MARCH "${RE5_MARCH_DEFAULT}" CACHE STRING "-march= para o host x86-64 (vazio = padrão do compilador)")
if(NOT MSVC)
    if(RE5_PGO STREQUAL "gen")
        target_compile_options(re5_cpu INTERFACE -fprofile-generate -fprofile-update=atomic -fprofile-dir=${RE5_PGO_DIR})
        target_link_options(re5_cpu INTERFACE -fprofile-generate)
    elseif(RE5_PGO STREQUAL "use")
        target_compile_options(re5_cpu INTERFACE -fprofile-use -fprofile-partial-training -fprofile-correction
                               -fprofile-dir=${RE5_PGO_DIR} -Wno-missing-profile)
        target_link_options(re5_cpu INTERFACE -fprofile-use)
    elseif(NOT RE5_PGO STREQUAL "off")
        message(FATAL_ERROR "RE5_PGO deve ser off, gen ou use (veio '${RE5_PGO}')")
    endif()
    if(RE5_MARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x86")
        target_compile_options(re5_cpu INTERFACE -march=${RE5_MARCH})
    endif()
    # Android ARM64: usa agendamento genérico ("generic") por padrão para não penalizar os
    # pipelines ultra-largos de SoCs modernos (Oryon 8-wide / Cortex-X3/X4). Só -mtune: o
    # binário continua ARMv8.0 compatível com qualquer aparelho, com LSE via outline atomics.
    if(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(RE5_ANDROID_TUNE "generic" CACHE STRING "-mtune= no Android ARM64 (vazio = padrão do NDK)")
        if(RE5_ANDROID_TUNE)
            target_compile_options(re5_cpu INTERFACE -mtune=${RE5_ANDROID_TUNE})
        endif()
        # RE5_ANDROID_MARCH (ex.: armv8.2-a): atômicas LSE em linha em vez das chamadas __aarch64_cas/swp
        # (outline atomics) e outras instruções do ARMv8.2. Só roda em aparelho com ARMv8.2+ (A55/A76 ou
        # mais novo); vazio = ARMv8.0, roda em qualquer um. Teste de desempenho de 23/11 no A54.
        set(RE5_ANDROID_MARCH "" CACHE STRING "-march= no Android ARM64 (vazio = ARMv8.0 do NDK)")
        if(RE5_ANDROID_MARCH)
            target_compile_options(re5_cpu INTERFACE -march=${RE5_ANDROID_MARCH})
        endif()
    endif()
endif()

# Cobertura de funções (RE5_COV, teclas F8/F9 com RE5_COV=1): uma leitura numa tabela de 11 MB a
# cada chamada de função gerada, 5,8% do tempo do código gerado (perfil por linha, 28/10). Só para
# depuração; vale para o runtime e o código gerado (interface do re5_cpu).
option(RE5_COVERAGE "Código gerado marca a cobertura de funções (RE5_COV)" OFF)
if(RE5_COVERAGE)
    target_compile_definitions(re5_cpu INTERFACE RE5_COVERAGE=1)
endif()

add_library(re5_runtime STATIC
    src/core/cpu_ext.c
    src/core/crash.c
    src/core/dispatch.c
    src/core/guest_memory.c
    src/core/handle_table.c
    src/core/perf_hint.c
    src/core/image.c
    src/core/imports.c
    src/core/log.c
    src/core/memdump.c
    src/core/names.c
    src/core/overrides.c
    src/core/runtime.c
    src/core/thread.c
    src/core/thunks.c
    src/core/trace.c
    src/core/vfs.c
    src/gfx/d3d9_host.c
    src/gfx/vk/re5vk.c
    src/gfx/vk/vk_draw.c
    src/gfx/vk/vk_dxso.c
    src/gfx/vk/vk_etc2_pool.c
    src/gfx/vk/vk_layout.c
    src/gfx/vk/vk_loader.c
    src/gfx/vk/vk_res.c
    src/gfx/vk/vk_tbdr.c
    src/gfx/vkn/IRenderer.cpp
    src/gfx/vkn/VulkanContext.cpp
    src/gfx/vkn/VulkanMemory.cpp
    src/gfx/vkn/renderer.cpp
    "${CMAKE_SOURCE_DIR}/third_party/vk-bootstrap/src/VkBootstrap.cpp"
    src/win32/advapi32.c
    src/win32/avifil32.c
    src/win32/coldebug.c
    src/win32/lan_menu.c
    src/win32/d3d9.c
    src/win32/dinput8.c
    src/win32/dsound.c
    src/win32/gdi32.c
    src/win32/iphlpapi.c
    src/win32/kernel32.c
    src/win32/kernel32_env.c
    src/win32/kernel32_file.c
    src/win32/kernel32_heap.c
    src/win32/kernel32_nls.c
    src/win32/kernel32_sync.c
    src/win32/ole32.c
    src/win32/oleaut32.c
    src/win32/shell32.c
    src/win32/steam_api.c
    src/win32/steam_lan.c
    src/win32/touch.c
    src/win32/user32.c
    src/win32/winmm.c
    src/win32/wmvcore.c
    src/win32/ws2_32.c
    src/win32/ws2_32_sock.c
    src/win32/xinput1_3.c
    "${RE5_D3D9_METHODS}"
)
target_link_libraries(re5_runtime PUBLIC re5_cpu SDL2-static)
# d3d9.h: no Windows o do MinGW/SDK; fora dele o da DXVK native (que também
# traz os tipos do Windows que ele usa). Privado: o código gerado não o vê.
add_library(re5_d3d9_headers INTERFACE)
if(NOT WIN32)
    target_include_directories(re5_d3d9_headers INTERFACE
        "${CMAKE_SOURCE_DIR}/third_party/dxvk-native/include/native/directx"
        "${CMAKE_SOURCE_DIR}/third_party/dxvk-native/include/native/windows")
    target_link_libraries(re5_runtime PUBLIC ${CMAKE_DL_LIBS})
endif()
target_link_libraries(re5_runtime PRIVATE re5_d3d9_headers)
# re5vk (RE5_GFX=re5vk): só os headers do Vulkan (os da DXVK 2.6.2, com Win32 e Android);
# o carregador é aberto em tempo de execução, nada a linkar.
set_source_files_properties(src/gfx/vk/re5vk.c src/gfx/vk/vk_draw.c src/gfx/vk/vk_res.c src/gfx/vk/vk_layout.c PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party/dxvk-2.6.2/include/vulkan/include")
# Renderizadores nativos vkadreno/vkmali (src/gfx/vkn, C++17): vk-bootstrap v1.4.363 casa com os
# headers Vulkan 1.4.363 de third_party/dxvk; VMA v3.5.0 (só header). O libvulkan continua aberto
# em tempo de execução (re5_vk_get_instance_proc_addr), nada a linkar.
set(RE5_VKN_INCLUDES
    "${CMAKE_SOURCE_DIR}/third_party/dxvk/include/vulkan/include"
    "${CMAKE_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include"
    "${CMAKE_SOURCE_DIR}/third_party/vk-bootstrap/src")
set(RE5_VKN_SOURCES src/gfx/vkn/IRenderer.cpp src/gfx/vkn/VulkanContext.cpp src/gfx/vkn/VulkanMemory.cpp
    src/gfx/vkn/renderer.cpp)
set_source_files_properties(${RE5_VKN_SOURCES} PROPERTIES INCLUDE_DIRECTORIES "${RE5_VKN_INCLUDES}")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third_party/vk-bootstrap/src/VkBootstrap.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${RE5_VKN_INCLUDES}" COMPILE_OPTIONS "-w")
if(NOT MSVC)
    # O VMA é código de terceiros num .cpp nosso: sem os avisos dele.
    set_source_files_properties(src/gfx/vkn/VulkanMemory.cpp PROPERTIES COMPILE_OPTIONS "-Wno-nullability-completeness;-Wno-unused-variable")
endif()
set_target_properties(re5_runtime PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set(RE5_VKN_INCLUDES "${RE5_VKN_INCLUDES}" PARENT_SCOPE)
if(WIN32)
    target_link_libraries(re5_runtime PUBLIC ws2_32 winmm)   # sockets do Winsock; timeBeginPeriod do host
endif()

# FFmpeg mínimo (asf + vc1/wmapro) para o wmvcore (etapa 4.5), compilado por
# scripts/build_ffmpeg.sh ou build_ffmpeg_android.sh. Os headers são os do fonte mais os gerados no build.
if(ANDROID)
    set(RE5_FFMPEG_DEFAULT "${CMAKE_SOURCE_DIR}/build/ffmpeg-android")
else()
    set(RE5_FFMPEG_DEFAULT "${CMAKE_SOURCE_DIR}/build/ffmpeg")
endif()
set(RE5_FFMPEG_DIR "${RE5_FFMPEG_DEFAULT}" CACHE PATH "Build do FFmpeg (scripts/build_ffmpeg.sh ou build_ffmpeg_android.sh)")
set(RE5_FFMPEG_LIBS
    "${RE5_FFMPEG_DIR}/libavformat/libavformat.a"
    "${RE5_FFMPEG_DIR}/libavcodec/libavcodec.a"
    "${RE5_FFMPEG_DIR}/libavutil/libavutil.a")
foreach(lib ${RE5_FFMPEG_LIBS})
    if(NOT EXISTS "${lib}")
        message(FATAL_ERROR "FFmpeg não encontrado (${lib}). Rode: scripts/build_ffmpeg.sh (ou scripts/build_ffmpeg_android.sh) "
                            "\"${RE5_FFMPEG_DIR}\" ou aponte RE5_FFMPEG_DIR para um build pronto.")
    endif()
endforeach()
set_source_files_properties(src/win32/wmvcore.c PROPERTIES
    INCLUDE_DIRECTORIES "${RE5_FFMPEG_DIR};${CMAKE_SOURCE_DIR}/third_party/FFmpeg")
target_link_libraries(re5_runtime PUBLIC ${RE5_FFMPEG_LIBS})
if(WIN32)
    target_link_libraries(re5_runtime PUBLIC bcrypt ole32 user32)   # dependências do libavutil/libavcodec
endif()
if(NOT MSVC)
    target_compile_options(re5_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
                                           -Wno-missing-field-initializers)   # {nome, fn, bytes, 0}: flags = 0
    # Sem tipo de build o runtime saía em -O0: dispatch, thunks, lock e mixer rodam
    # a cada chamada do jogo. Só o runtime: o código gerado tem as flags dele.
    if(NOT CMAKE_BUILD_TYPE)
        target_compile_options(re5_runtime PRIVATE -O2)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(re5_runtime PUBLIC Threads::Threads m)
endif()
if(TARGET adrenotools)
    target_link_libraries(re5_runtime PUBLIC adrenotools)
    target_compile_definitions(re5_runtime PRIVATE RE5_HAS_ADRENOTOOLS=1)
endif()
