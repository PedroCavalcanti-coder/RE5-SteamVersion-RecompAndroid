# re5-recompiled — build raiz.
#
#   cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
#
# Alvos:
#   re5_runtime    biblioteca estática: memória do guest, dispatch, thunks, HLE Win32
#   re5_generated  código gerado (generated/src) ou o stub vazio antes da Fase 2
#   re5            executável host
cmake_minimum_required(VERSION 3.22)
project(re5_recompiled LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RE5_BUILD_TESTS "Testes unitários e de ponta a ponta" ON)
set(RE5_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/src" CACHE PATH
    "Saída do scripts/run_lift.py lift (vazia = usa o stub)")
set(RE5_PCRECOMP_DIR "${CMAKE_SOURCE_DIR}/tools/pcrecomp" CACHE PATH "pcrecomp vendorizado")

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

# SDL2 (janela cross-platform, eventos e superfície de apresentação)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL2 EXCLUDE_FROM_ALL)

add_subdirectory(runtime)
add_subdirectory(generated)

add_executable(re5 runtime/src/host/main.c)
target_link_libraries(re5 PRIVATE re5_runtime re5_generated)

# Vulkan no Windows: os d3d9.dll x64 da DXVK (scripts/build_dxvk.sh <versão>) vão
# para dxvk/<versão>/ ao lado do re5.exe; o runtime carrega a mais nova que a
# GPU aceita (runtime/src/gfx/d3d9_host.c; RE5_D3D9_DLL=d3d9.dll usa o do sistema).
set(RE5_DXVK_DIR "${CMAKE_SOURCE_DIR}/build/dxvk" CACHE PATH "Build da DXVK (scripts/build_dxvk.sh)")
if(WIN32)
    set(RE5_DXVK_FOUND "")
    foreach(ver 3.1.1 2.6.2)
        if(EXISTS "${RE5_DXVK_DIR}/${ver}/d3d9.dll")
            list(APPEND RE5_DXVK_FOUND ${ver})
            add_custom_command(TARGET re5 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:re5>/dxvk/${ver}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RE5_DXVK_DIR}/${ver}/d3d9.dll"
                        "$<TARGET_FILE_DIR:re5>/dxvk/${ver}/d3d9.dll"
                VERBATIM)
        endif()
    endforeach()
    if(NOT RE5_DXVK_FOUND)
        message(WARNING "Nenhuma DXVK em ${RE5_DXVK_DIR}/<versão>: o re5 usará o d3d9.dll do sistema. "
                        "Rode scripts/build_dxvk.sh 2.6.2 (e 3.1.1) para o Vulkan.")
    endif()
endif()

if(RE5_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
