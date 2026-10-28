# tests/ — unitários do runtime, ponta a ponta com PE sintético e testes Python do pipeline.

foreach(t guest_memory handle_table dispatch user32 xinput ws2_32 dsound x87 heap_mt)
    add_executable(test_${t} unit/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE re5_runtime)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# kernel32: o argumento é uma pasta nova (o teste a cria) que faz de C:\RE5 nos testes de arquivo.
add_executable(test_kernel32 unit/test_kernel32.c)
target_link_libraries(test_kernel32 PRIVATE re5_runtime)
set(RE5_K32_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/kernel32_fixture")
add_test(NAME kernel32_fixture_setup COMMAND ${CMAKE_COMMAND} -E rm -rf "${RE5_K32_FIXTURE}")
set_tests_properties(kernel32_fixture_setup PROPERTIES FIXTURES_SETUP k32_dir)
add_test(NAME kernel32 COMMAND test_kernel32 "${RE5_K32_FIXTURE}")
set_tests_properties(kernel32 PROPERTIES FIXTURES_REQUIRED k32_dir ENVIRONMENT "RE5_GUEST_LOCK=global")
# O mesmo no modo do lock só no HLE (código do guest em paralelo; o padrão no x86-64), sem os arquivos.
add_test(NAME kernel32_hle COMMAND test_kernel32)
set_tests_properties(kernel32_hle PROPERTIES ENVIRONMENT "RE5_GUEST_LOCK=hle")

# Ponte COM do Direct3D 9 com backend falso (sem GPU).
add_executable(test_d3d9 unit/test_d3d9.c)
target_link_libraries(test_d3d9 PRIVATE re5_runtime re5_d3d9_headers)
add_test(NAME d3d9 COMMAND test_d3d9)

add_executable(test_dxt_decode unit/test_dxt_decode.c)
add_test(NAME dxt_decode COMMAND test_dxt_decode)

# Estouro da pilha do host com relatório (filtro com pilha de reserva).
if(WIN32)
    add_executable(test_crash unit/test_crash.c)
    target_link_libraries(test_crash PRIVATE re5_runtime)
    add_test(NAME crash COMMAND test_crash)
endif()

# A mesma ponte sobre o d3d9.dll x64 do Windows (pula com 77 sem vídeo ou GPU).
if(WIN32)
    add_executable(test_d3d9_host unit/test_d3d9_host.c)
    target_link_libraries(test_d3d9_host PRIVATE re5_runtime re5_d3d9_headers)
    add_test(NAME d3d9_host COMMAND test_d3d9_host)
    set_tests_properties(d3d9_host PROPERTIES SKIP_RETURN_CODE 77)
endif()

# DirectInput8 sobre SDL2 (sem janela). No Windows, GUIDs e formatos do MinGW.
add_executable(test_dinput8 unit/test_dinput8.c)
target_link_libraries(test_dinput8 PRIVATE re5_runtime)
if(WIN32)
    target_link_libraries(test_dinput8 PRIVATE dinput8 dxguid)
endif()
add_test(NAME dinput8 COMMAND test_dinput8)

# IWMSyncReader (wmvcore) sobre o FFmpeg com um vídeo do jogo; pula com 77 sem ele.
set(RE5_GAME_DIR "${CMAKE_SOURCE_DIR}/bin_input/game" CACHE PATH "Pasta com os dados do jogo (nativePC_MT)")
add_executable(test_wmvcore unit/test_wmvcore.c)
target_link_libraries(test_wmvcore PRIVATE re5_runtime)
add_test(NAME wmvcore COMMAND test_wmvcore "${RE5_GAME_DIR}/nativePC_MT/movie/bh5logo.wmv")
set_tests_properties(wmvcore PROPERTIES SKIP_RETURN_CODE 77)

# Gravação de AVI (avifil32 + msvfw32): o argumento é uma pasta nova que faz de C:\RE5.
add_executable(test_avifil32 unit/test_avifil32.c)
target_link_libraries(test_avifil32 PRIVATE re5_runtime)
set(RE5_AVI_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/avi_fixture")
add_test(NAME avifil32_fixture_setup COMMAND ${CMAKE_COMMAND} -E rm -rf "${RE5_AVI_FIXTURE}")
set_tests_properties(avifil32_fixture_setup PROPERTIES FIXTURES_SETUP avi_dir)
add_test(NAME avifil32 COMMAND test_avifil32 "${RE5_AVI_FIXTURE}")
set_tests_properties(avifil32 PROPERTIES FIXTURES_REQUIRED avi_dir)

add_executable(test_steam_api unit/test_steam_api.c)
target_link_libraries(test_steam_api PRIVATE re5_runtime)
add_test(NAME steam_api COMMAND test_steam_api "${CMAKE_CURRENT_BINARY_DIR}/steam_fixture")

add_executable(test_steam_lan unit/test_steam_lan.c)
target_link_libraries(test_steam_lan PRIVATE re5_runtime)
set(RE5_LAN_ID_TEST "${CMAKE_CURRENT_BINARY_DIR}/lan_identity/re5_lan.ini")
set(RE5_LAN_ID_NEW "${CMAKE_CURRENT_BINARY_DIR}/lan_identity_new/sub/re5_lan.ini")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lan_identity")
add_test(NAME steam_lan COMMAND test_steam_lan "${RE5_LAN_ID_TEST}")
set_tests_properties(steam_lan PROPERTIES ENVIRONMENT "RE5_LAN_CONFIG=${RE5_LAN_ID_TEST}")
add_test(NAME steam_lan_identity COMMAND test_steam_lan --identity "${RE5_LAN_ID_NEW}")
set_tests_properties(steam_lan_identity PROPERTIES ENVIRONMENT "RE5_LAN_CONFIG=${RE5_LAN_ID_NEW};LANG=pt_BR.UTF-8;LC_ALL=;LC_CTYPE=")
add_executable(test_lan_menu unit/test_lan_menu.c)
target_link_libraries(test_lan_menu PRIVATE re5_runtime)
set(RE5_LAN_ID_MENU "${CMAKE_CURRENT_BINARY_DIR}/lan_identity_menu/re5_lan.ini")
add_test(NAME lan_menu COMMAND test_lan_menu "${RE5_LAN_ID_MENU}")
set_tests_properties(lan_menu PROPERTIES ENVIRONMENT "RE5_LAN_CONFIG=${RE5_LAN_ID_MENU};RE5_LAN_PORT=27060")
add_test(NAME lan_coop COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_lan_coop.py" "$<TARGET_FILE:test_steam_lan>")

if(NOT WIN32)
    add_executable(test_vfs unit/test_vfs.c)
    target_link_libraries(test_vfs PRIVATE re5_runtime)
    add_test(NAME vfs COMMAND test_vfs "${CMAKE_CURRENT_BINARY_DIR}/vfs_fixture")
endif()

# Ponta a ponta: precisa de clang + lld-link + llvm-dlltool para gerar um PE32.
find_program(RE5_CLANG NAMES clang clang-19 clang-18 clang-17)
find_program(RE5_LLD_LINK NAMES lld-link lld-link-19 lld-link-18 lld-link-17)
find_program(RE5_DLLTOOL NAMES llvm-dlltool llvm-dlltool-19 llvm-dlltool-18 llvm-dlltool-17)
if(RE5_CLANG AND RE5_LLD_LINK AND RE5_DLLTOOL)
    set(FX "${CMAKE_CURRENT_BINARY_DIR}/mini_pe")
    set(FX_GEN "${FX}/gen/re5_0000.c" "${FX}/gen/re5_dispatch_table.c")
    add_custom_command(
        OUTPUT ${FX_GEN} "${FX}/fx.exe"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/e2e/build_fixture.py" --out "${FX}"
                --clang "${RE5_CLANG}" --lld-link "${RE5_LLD_LINK}" --dlltool "${RE5_DLLTOOL}"
        DEPENDS e2e/mini_pe/fx.c e2e/build_fixture.py
                "${CMAKE_SOURCE_DIR}/scripts/recon.py" "${CMAKE_SOURCE_DIR}/scripts/run_lift.py"
        COMMENT "PE32 sintético -> recon -> catálogo -> lift"
        VERBATIM)
    add_executable(test_e2e_mini_pe e2e/test_mini_pe.c ${FX_GEN})
    target_include_directories(test_e2e_mini_pe PRIVATE "${FX}/gen")
    target_link_libraries(test_e2e_mini_pe PRIVATE re5_runtime)
    add_test(NAME e2e_mini_pe COMMAND test_e2e_mini_pe "${FX}/fx.exe")
else()
    message(STATUS "re5: clang/lld-link/llvm-dlltool ausentes; teste de ponta a ponta desativado")
endif()

# Teste diferencial do lift32_cpu contra o Unicorn com instruções do próprio RE5
# (scripts/difftest_cpu.py; pula com 77 sem catálogo/binário ou sem unicorn).
add_test(NAME difftest_cpu
         COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/difftest_cpu.py" --per-group 1 --seeds 2)
set_tests_properties(difftest_cpu PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200
                     ENVIRONMENT "RE5_BUILD=${CMAKE_BINARY_DIR};RECOMP_CC=${CMAKE_C_COMPILER}")
add_test(NAME python_pipeline
         COMMAND Python3::Interpreter -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/python" -v)
set_tests_properties(python_pipeline PROPERTIES ENVIRONMENT "RE5_TEST_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
