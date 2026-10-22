/*
 * cpu_ext.c - cpuid e rdtsc do guest (etapa 2.1).
 *
 * São as duas instruções que descrevem a máquina, então a resposta é uma
 * decisão do runtime, não do lifter:
 *
 *   cpuid  um Core 2 genérico: MMX, SSE e SSE2 sim; SSE3 em diante não. Os bits
 *          de recurso escolhem os caminhos de código do jogo e do CRT, e os
 *          caminhos SSE2 já passam pelo lifter; extensões mais novas levariam o
 *          jogo a instruções que o lift32_cpu ainda não traduz. Folhas que não
 *          estão aqui devolvem zero, como uma CPU que não as tem.
 *   rdtsc  relógio monotônico do host a 3 GHz virtuais: cresce sempre, nunca
 *          volta, e a razão com o QueryPerformanceCounter é estável.
 */
#include <string.h>

#include "re5/runtime.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
static uint64_t host_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

#define TSC_GHZ 3u

/* CPUID.1:EDX: FPU, TSC, CX8, CMOV, MMX, FXSR, SSE, SSE2. */
#define FEAT_EDX ((1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26))
/* Família 6, modelo 15, stepping 11, 2 núcleos, 3 GHz: o E6850 real. O mesmo
 * nome e a mesma frequência estão no registro (advapi32.c, ~MHz), para quem
 * calibrar o rdtsc pelo registro chegar ao mesmo TSC_GHZ. */
#define BRAND "Intel(R) Core(TM)2 Duo CPU     E6850  @ 3.00GHz"

static uint32_t chars4(const char *s)
{
    return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2] << 16 |
           (uint32_t)(uint8_t)s[3] << 24;
}

void recomp_cpuid(CPU *c)
{
    uint32_t leaf = c->eax;
    c->eax = c->ebx = c->ecx = c->edx = 0;
    switch (leaf) {
    case 0:
        c->eax = 1;
        c->ebx = chars4("Genu");
        c->edx = chars4("ineI");
        c->ecx = chars4("ntel");
        break;
    case 1:
        c->eax = 0x000006FBu;                 /* família 6, modelo 15, stepping 11 */
        c->ebx = 8u << 8;                     /* CLFLUSH de 64 bytes, APIC 0 */
        c->edx = FEAT_EDX;
        break;
    case 0x80000000u:
        c->eax = 0x80000004u;
        break;
    case 0x80000002u:
    case 0x80000003u:
    case 0x80000004u: {
        char brand[48];
        memset(brand, 0, sizeof brand);
        memcpy(brand, BRAND, sizeof BRAND < sizeof brand ? sizeof BRAND : sizeof brand - 1);
        const char *p = brand + 16 * (leaf - 0x80000002u);
        c->eax = chars4(p);
        c->ebx = chars4(p + 4);
        c->ecx = chars4(p + 8);
        c->edx = chars4(p + 12);
        break;
    }
    default:
        break;
    }
}

void recomp_rdtsc(CPU *c)
{
    uint64_t t = host_ns() * TSC_GHZ;
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)(t >> 32);
}
