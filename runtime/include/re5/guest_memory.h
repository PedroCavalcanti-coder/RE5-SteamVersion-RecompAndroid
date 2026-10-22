/*
 * guest_memory.h - o espaço de endereçamento de 4 GiB do guest.
 *
 * re5_mem_init() reserva 4 GiB contíguos (sem commit) e fixa re5_guest_base.
 * Regiões são comprometidas sob demanda, com a semântica do VirtualAlloc:
 * granularidade de alocação de 64 KiB, páginas de 4 KiB, proteções do Win32.
 *
 * Mapa do espaço do guest:
 *   0x00000000-0x0000FFFF  nunca mapeado: ponteiro nulo do guest falha no host
 *   0x00400000-...         imagem do re5dx9.exe (base fixa, sem relocações)
 *   0x10000000-0xFFFBFFFF  alocações dinâmicas (VirtualAlloc, heaps, pilhas, TEBs)
 *   0xFFFC0000-0xFFFEFFFF  thunks: endereços que o dispatch entrega ao host
 *   0xFFFF0000-0xFFFFFFFF  nunca mapeado
 *
 * Execução é só metadado: o host nunca executa bytes do guest.
 */
#ifndef RE5_GUEST_MEMORY_H
#define RE5_GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Proteções, com os valores do Win32 (PAGE_*), para o HLE repassar direto. */
#define RE5_PAGE_NOACCESS          0x01u
#define RE5_PAGE_READONLY          0x02u
#define RE5_PAGE_READWRITE         0x04u
#define RE5_PAGE_WRITECOPY         0x08u
#define RE5_PAGE_EXECUTE           0x10u
#define RE5_PAGE_EXECUTE_READ      0x20u
#define RE5_PAGE_EXECUTE_READWRITE 0x40u

#define RE5_MEM_PAGE        0x1000u
#define RE5_MEM_GRANULARITY 0x10000u
#define RE5_MEM_DYN_LO      0x10000000u
#define RE5_MEM_DYN_HI      0xFFFC0000u

/* Reserva o espaço e define re5_guest_base. 0 = ok. Idempotente. */
int  re5_mem_init(void);
void re5_mem_shutdown(void);

/* Compromete [va, va+size) num endereço fixo (ex.: a imagem em 0x00400000).
 * Falha se qualquer parte já estiver em uso. 0 = ok. */
int re5_mem_map_fixed(uint32_t va, uint32_t size, uint32_t prot, const char *tag);

/* Aloca `size` bytes zerados em qualquer endereço livre da faixa dinâmica.
 * Retorna o VA (alinhado a 64 KiB) ou 0. */
uint32_t re5_mem_alloc(uint32_t size, uint32_t prot, const char *tag);

/* Libera uma região inteira, pelo VA retornado na alocação. 0 = ok. */
int re5_mem_free(uint32_t va);

/* Muda a proteção de páginas dentro de uma região. Retorna a anterior em *old. */
int re5_mem_protect(uint32_t va, uint32_t size, uint32_t prot, uint32_t *old);

/* Zera [va, va+size), que tem de estar numa só região; as páginas inteiras
 * voltam para o host (MEM_DECOMMIT do guest). 0 ou -1 fora de região. */
int re5_mem_zero(uint32_t va, uint32_t size);

/* 1 se [va, va+size) está inteiro dentro de regiões comprometidas. */
int re5_mem_is_mapped(uint32_t va, uint32_t size);

/* Para VirtualQuery e relatórios de crash. */
typedef struct {
    uint32_t    base;
    uint32_t    size;
    uint32_t    prot;
    const char *tag;
} re5_mem_region;

int re5_mem_query(uint32_t va, re5_mem_region *out);

/* Imprime o mapa de regiões (diagnóstico). */
void re5_mem_dump(void);

#endif /* RE5_GUEST_MEMORY_H */
