/*
 * guest_base.h - tradução de endereço do guest (32 bits) para o host.
 *
 * O jogo é x86-32: todo ponteiro que ele manipula é um uint32_t. O runtime
 * reserva um bloco contíguo de 4 GiB no host e o endereço do guest `va` vive
 * em `re5_guest_base + va`. Num host de 64 bits (Linux x64, Android ARM64,
 * Windows x64) isso é uma soma; o código gerado nunca vê um ponteiro do host.
 *
 * Este header não depende de nada além de <stdint.h> porque é incluído no topo
 * do cpu.h gerado, antes de qualquer outro header do runtime.
 */
#ifndef RE5_GUEST_BASE_H
#define RE5_GUEST_BASE_H

#include <stdint.h>

#if UINTPTR_MAX <= 0xFFFFFFFFu
#error "re5-recompiled exige um host de 64 bits (o espaço de 4 GiB do guest é uma reserva do host). Ver docs/ARQUITETURA.md, D2."
#endif

extern uintptr_t re5_guest_base;

/* Endereço do guest -> endereço do host, como inteiro. */
#define RE5_G2H(va) (re5_guest_base + (uintptr_t)(uint32_t)(va))

static inline void *re5_g2h(uint32_t va) { return (void *)RE5_G2H(va); }

/* Ponteiro do host dentro do espaço do guest -> endereço do guest. */
static inline uint32_t re5_h2g(const void *p) { return (uint32_t)((uintptr_t)p - re5_guest_base); }

/* String terminada em zero que mora na memória do guest. */
static inline const char *re5_gstr(uint32_t va) { return va ? (const char *)RE5_G2H(va) : 0; }

#endif /* RE5_GUEST_BASE_H */
