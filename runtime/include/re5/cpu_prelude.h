/*
 * cpu_prelude.h - incluído no TOPO do cpu.h gerado (scripts/gen_cpu_header.py).
 *
 * Configura o cpu.h do pcrecomp para este runtime antes de ele ser lido:
 *   - fs: sempre simulado (cada thread do guest tem um TEB na memória do guest);
 *   - RECOMP_TODO vai para o runtime, que diz onde e o quê antes de abortar;
 *   - tipos com alinhamento 1 para os acessos escalares à memória do guest.
 */
#ifndef RE5_CPU_PRELUDE_H
#define RE5_CPU_PRELUDE_H

#include <stdint.h>
#include <stdlib.h>
#include "re5/platform.h"
#include "re5/guest_base.h"

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include "sse2neon.h"
#endif

#ifndef RECOMP_FS_SIMULATED
#define RECOMP_FS_SIMULATED 1
#endif

/* Instrução que o lifter não soube traduzir: nunca retorna. */
RE5_NORETURN void re5_recomp_todo(uint32_t va, const char *what);
#define RECOMP_TODO(va, text) re5_recomp_todo((va), (text))

/* x86 aceita qualquer alinhamento; em C um uint32_t* desalinhado é UB e o
 * compilador ARM64 pode usar instruções que exigem alinhamento. may_alias
 * porque o jogo lê os mesmos bytes como int, float e vetor. */
#if defined(__GNUC__) || defined(__clang__)
typedef uint8_t  __attribute__((aligned(1), may_alias)) RE5_UA_U8;
typedef uint16_t __attribute__((aligned(1), may_alias)) RE5_UA_U16;
typedef uint32_t __attribute__((aligned(1), may_alias)) RE5_UA_U32;
#else
typedef uint8_t  RE5_UA_U8;
typedef uint16_t RE5_UA_U16;
typedef uint32_t RE5_UA_U32;
#endif

#endif /* RE5_CPU_PRELUDE_H */
