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
    target_compile_options(re5_cpu INTERFACE -fno-strict-aliasing -fwrapv -ffp-contract=off)
endif()

add_library(re5_runtime STATIC
    src/core/cpu_ext.c
    src/core/crash.c
    src/core/dispatch.c
    src/core/guest_memory.c
    src/core/handle_table.c
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
