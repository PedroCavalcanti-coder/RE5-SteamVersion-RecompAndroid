# generated/ — o código C que o scripts/run_lift.py gera a partir do re5dx9.exe.
#
# Com generated/src/re5_dispatch_table.c presente, compila tudo que está lá;
# senão, o stub com a tabela vazia (o host carrega a imagem e para com uma
# mensagem dizendo para rodar a Fase 2).
set(RE5_TABLE "${RE5_GENERATED_DIR}/re5_dispatch_table.c")
if(EXISTS "${RE5_TABLE}")
    file(GLOB RE5_GENERATED_SOURCES CONFIGURE_DEPENDS "${RE5_GENERATED_DIR}/re5_*.c")
    list(LENGTH RE5_GENERATED_SOURCES n)
    message(STATUS "re5: código gerado em ${RE5_GENERATED_DIR} (${n} arquivos)")
    add_library(re5_generated STATIC ${RE5_GENERATED_SOURCES})
    target_include_directories(re5_generated PRIVATE "${RE5_GENERATED_DIR}")
    # Milhões de linhas de C mecânico: avisos não ajudam. Nível de otimização em
    # RE5_GENERATED_OPT (medir com scripts/bench_re5.py antes de mudar o padrão).
    set(RE5_GENERATED_OPT "-O1" CACHE STRING "Otimização do código gerado (GCC/Clang)")
    if(MSVC)
        target_compile_options(re5_generated PRIVATE /w /O1)
    else()
        # -foptimize-sibling-calls: o jmp de cauda entre funções (RE5_TAILCALL) vira jmp também
        # em -O1; onde o compilador não consegue, cai numa chamada normal (mesma semântica).
        target_compile_options(re5_generated PRIVATE -w ${RE5_GENERATED_OPT} -foptimize-sibling-calls)
    endif()
else()
    message(STATUS "re5: sem código gerado; usando generated/stub (Fase 2 pendente)")
    add_library(re5_generated STATIC stub/re5_generated_stub.c)
endif()
# O código gerado chama o runtime (dispatch, recomp_cpuid...): o runtime vem
# depois dele na linha de link, senão o ld não puxa os objetos que só ele usa.
target_link_libraries(re5_generated PUBLIC re5_cpu re5_runtime)
