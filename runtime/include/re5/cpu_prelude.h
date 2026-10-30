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

/* A imagem do re5dx9.exe não tem relocações e carrega sempre na ImageBase (dispatch.c):
 * GVA(abs) é a constante abs. Com o g_image_delta somado, cada endereço absoluto do jogo
 * custava uma leitura extra do delta (pela .refptr do MinGW), refeita depois de toda escrita
 * na memória do guest, porque o may_alias acima diz que ela pode tê-lo mudado. */
#define RECOMP_IMAGE_FIXED 1

/* Endereço "largo" de um operando [reg + disp] (lift.wide_addr): base + deslocamento com
 * sinal, sem a volta em 32 bits. Assim o compilador põe o deslocamento no próprio mov do
 * host (mov 0x24(%rdx,%rbase)) em vez de um lea antes. Só difere do x86 quando reg + disp
 * atravessa 0 ou 4 GiB, o que no jogo só um ponteiro inválido faz; as faixas de guarda do
 * guest_memory.c fazem esse acesso falhar em vez de cair em memória do host.
 *
 * Os acessos do cpu.h (rd32, wr32, rdss...) recebem re5_gaddr por um _Generic: um uint32_t
 * (todo chamador que já existe) continua truncado em 32 bits como antes; só re5_wide passa
 * os 64 bits. */
typedef uint64_t re5_gaddr;
typedef struct { uint64_t v; } re5_wide;
static inline re5_gaddr re5_ga_narrow(uint32_t a) { return a; }
static inline re5_gaddr re5_ga_wide(re5_wide w) { return w.v; }
#define RE5_GA(a) _Generic((a), re5_wide: re5_ga_wide, default: re5_ga_narrow)(a)
#define RE5_WIDE(base, disp) ((re5_wide){(uint64_t)(uint32_t)(base) + (uint64_t)(int64_t)(int32_t)(uint32_t)(disp)})
#define RE5_G2HW(ga) (re5_guest_base + (uintptr_t)(ga))

#endif /* RE5_CPU_PRELUDE_H */
