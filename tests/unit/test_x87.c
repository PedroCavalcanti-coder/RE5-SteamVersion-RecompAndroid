/*
 * Palavra de controle do x87 e MXCSR no código traduzido (lift32_cpu + cpu.h).
 *
 * As funções abaixo estão no formato exato que o lift32_cpu gera (as linhas de
 * fnstcw/fldcw/fistp/frndint/fnstenv/fldenv são as que ele emite; ver
 * tests/python/test_lifter_gaps.py). Cobre o idioma do MSVC que o motor de som
 * do RE5 usa para truncar (0x00496FE9: fnstcw; or eax, 0xC00; fldcw; fistp;
 * fldcw), floor/ceil inline (RC 01/10), o "inteiro indefinido" do estouro, o
 * ambiente FPU levando a palavra, o estado inicial de uma thread nova e o FTZ
 * que o ldmxcsr da thread de som (0x0048C5A2) liga no host.
 */
#include <math.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"

#define VA_TO_INT  0x00401000u   /* int f(float x, int rc_bits): fistp dword sob o RC pedido */
#define VA_RNDINT  0x00401100u   /* float g(float x, int rc_bits): frndint sob o RC pedido   */
#define VA_ENV     0x00401200u   /* fnstenv [esp]; fldcw 0x0C7F; fldenv [esp]; fnstcw -> eax */

/*   sub esp, 8
 *   fld dword ptr [esp + 0xc]
 *   fnstcw word ptr [esp + 4]
 *   movzx eax, word ptr [esp + 4]
 *   or eax, dword ptr [esp + 0x10]
 *   mov dword ptr [esp], eax
 *   fldcw word ptr [esp]
 *   fistp dword ptr [esp]
 *   mov eax, dword ptr [esp]
 *   fldcw word ptr [esp + 4]
 *   add esp, 8
 *   ret */
static void L_00401000(CPU *c)
{
    c->esp -= 8u;
    fpush(c, rdf32((c->esp + 0x0000000Cu)));
    wr16((c->esp + 0x00000004u), (uint16_t)c->fpu_cw);
    c->eax = (uint32_t)rd16((c->esp + 0x00000004u));
    c->eax |= rd32((c->esp + 0x00000010u));
    wr32((c->esp), c->eax);
    c->fpu_cw = rd16((c->esp));
    wri32((c->esp), fpu_rint(c, *fst(c, 0))); fpop(c);
    c->eax = rd32((c->esp));
    c->fpu_cw = rd16((c->esp + 0x00000004u));
    c->esp += 8u;
    c->esp += 4; return;
}

/* O mesmo com frndint no lugar do fistp; o resultado volta por [esp] como float. */
static void L_00401100(CPU *c)
{
    c->esp -= 8u;
    fpush(c, rdf32((c->esp + 0x0000000Cu)));
    wr16((c->esp + 0x00000004u), (uint16_t)c->fpu_cw);
    c->eax = (uint32_t)rd16((c->esp + 0x00000004u));
    c->eax |= rd32((c->esp + 0x00000010u));
    wr32((c->esp), c->eax);
    c->fpu_cw = rd16((c->esp));
    *fst(c, 0) = fpu_rint(c, *fst(c, 0));
    wrf32((c->esp), *fst(c, 0)); fpop(c);
    c->eax = rd32((c->esp));
    c->fpu_cw = rd16((c->esp + 0x00000004u));
    c->esp += 8u;
    c->esp += 4; return;
}

/*   sub esp, 0x20
 *   fnstenv [esp]
 *   mov word ptr [esp + 0x1c], 0xc7f
 *   fldcw word ptr [esp + 0x1c]
 *   fldenv [esp]
 *   fnstcw word ptr [esp + 0x1c]
 *   movzx eax, word ptr [esp + 0x1c]
 *   add esp, 0x20
 *   ret */
static void L_00401200(CPU *c)
{
    c->esp -= 0x20u;
    { uint32_t _a = (c->esp); wr32(_a, c->fpu_cw & 0xFFFFu); wr32(_a + 4, (c->fpu_sw & 0x4700u) | ((uint32_t)(c->fpu_top & 7) << 11)); wr32(_a + 8, 0u); wr32(_a + 12, 0u); wr32(_a + 16, 0u); wr32(_a + 20, 0u); wr32(_a + 24, 0u); }
    wr16((c->esp + 0x0000001Cu), 0x0C7Fu);
    c->fpu_cw = rd16((c->esp + 0x0000001Cu));
    { uint32_t _a = (c->esp); uint32_t _sw = rd32(_a + 4); c->fpu_sw = _sw & 0x4700u; c->fpu_cw = rd32(_a) & 0xFFFFu; }
    wr16((c->esp + 0x0000001Cu), (uint16_t)c->fpu_cw);
    c->eax = (uint32_t)rd16((c->esp + 0x0000001Cu));
    c->esp += 0x20u;
    c->esp += 4; return;
}

const re5_func_entry re5_lifted_table[] = {
    {VA_TO_INT, L_00401000},
    {VA_RNDINT, L_00401100},
    {VA_ENV, L_00401200},
};
const uint32_t re5_lifted_count = 3;

static re5_thread *g_t;

static uint32_t to_int(float x, uint32_t rc)
{
    uint32_t a[2];
    memcpy(&a[0], &x, 4);
    a[1] = rc;
    return re5_guest_call(g_t, VA_TO_INT, 2, a);
}

static float rnd(float x, uint32_t rc)
{
    uint32_t a[2], r;
    float f;
    memcpy(&a[0], &x, 4);
    a[1] = rc;
    r = re5_guest_call(g_t, VA_RNDINT, 2, a);
    memcpy(&f, &r, 4);
    return f;
}

int main(void)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_mem_map_fixed(0x00400000u, 0x200000u, RE5_PAGE_READWRITE, "imagem") == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    g_t = re5_thread_attach(1u << 20);
    CHECK(g_t != NULL);
    CPU *c = &g_t->cpu;

    /* Thread nova: palavra de controle do Windows; MXCSR com FTZ/DAZ (RE5_FTZ=0: o do Windows). */
    CHECK_EQ_U32(c->fpu_cw, 0x027Fu);
    CHECK_EQ_U32(c->mxcsr, re5_guest_mxcsr_default());

    /* RC = 11 (or 0xC00): o _ftol do MSVC, truncando para zero. */
    CHECK_EQ_U32(to_int(2.7f, 0xC00), 2);
    CHECK_EQ_U32(to_int(-2.7f, 0xC00), (uint32_t)-2);
    CHECK_EQ_U32(to_int(0.999f, 0xC00), 0);
    /* RC = 01 (floor) e 10 (ceil). */
    CHECK_EQ_U32(to_int(2.7f, 0x400), 2);
    CHECK_EQ_U32(to_int(-2.5f, 0x400), (uint32_t)-3);
    CHECK_EQ_U32(to_int(2.1f, 0x800), 3);
    CHECK_EQ_U32(to_int(-2.7f, 0x800), (uint32_t)-2);
    /* RC = 00: ao par mais próximo. */
    CHECK_EQ_U32(to_int(2.7f, 0), 3);
    CHECK_EQ_U32(to_int(2.5f, 0), 2);
    CHECK_EQ_U32(to_int(3.5f, 0), 4);
    /* A função restaura a palavra que leu. */
    CHECK_EQ_U32(c->fpu_cw, 0x027Fu);

    /* Estouro e NaN: o inteiro indefinido, não o que o C faria. */
    CHECK_EQ_U32(to_int(3.0e9f, 0xC00), 0x80000000u);
    CHECK_EQ_U32(to_int(-3.0e9f, 0xC00), 0x80000000u);
    CHECK_EQ_U32(to_int(NAN, 0), 0x80000000u);
    wri16(0x00500000u, 40000.0);
    CHECK_EQ_U32(rd16(0x00500000u), 0x8000u);
    wri16(0x00500000u, -32768.0);
    CHECK_EQ_U32(rd16(0x00500000u), 0x8000u);
    wri16(0x00500000u, 32767.0);
    CHECK_EQ_U32(rd16(0x00500000u), 0x7FFFu);
    wri64(0x00500000u, -5.0);
    CHECK_EQ_U32(rd32(0x00500000u), 0xFFFFFFFBu);
    CHECK_EQ_U32(rd32(0x00500004u), 0xFFFFFFFFu);

    /* frndint segue o mesmo campo (o floor/ceil x87 do CRT). */
    CHECK(rnd(2.7f, 0x400) == 2.0f);
    CHECK(rnd(-2.2f, 0x400) == -3.0f);
    CHECK(rnd(2.2f, 0x800) == 3.0f);
    CHECK(rnd(-2.7f, 0xC00) == -2.0f);
    CHECK(rnd(2.5f, 0) == 2.0f);

    /* fnstenv grava a palavra atual e fldenv a traz de volta. */
    CHECK_EQ_U32(re5_guest_call(g_t, VA_ENV, 0, NULL), 0x027Fu);
    c->fpu_cw = 0x0E7F;
    CHECK_EQ_U32(re5_guest_call(g_t, VA_ENV, 0, NULL), 0x0E7Fu);
    c->fpu_cw = 0x027F;

    /* rep stos/movs em bloco (cpu.h): o mesmo resultado do laço elemento a elemento. */
    {
        const uint32_t m = 0x00510000u;
        for (uint32_t i = 0; i < 64; i++) wr8(m + i, (uint8_t)i);
        c->edi = m + 1; c->ecx = 5; c->eax = 0xAABBCCDDu;
        rep_stos(c, 4);                                   /* valor não repetível: palavra a palavra */
        CHECK_EQ_U32(rd32(m + 1), 0xAABBCCDDu);
        CHECK_EQ_U32(rd32(m + 17), 0xAABBCCDDu);
        CHECK_EQ_U32(rd8(m + 21), 21);                    /* parou no fim */
        CHECK_EQ_U32(c->edi, m + 21);
        CHECK_EQ_U32(c->ecx, 0);
        c->edi = m; c->ecx = 3; c->eax = 0x12121212u;
        rep_stos(c, 4);                                   /* bytes iguais: memset */
        CHECK_EQ_U32(rd32(m + 8), 0x12121212u);
        CHECK_EQ_U32(rd8(m + 12), 0xAA);                  /* palavra de m+9, byte alto */
        for (uint32_t i = 0; i < 64; i++) wr8(m + i, (uint8_t)i);
        c->esi = m; c->edi = m + 4; c->ecx = 4;           /* destino dentro da origem: replica */
        rep_movs(c, 4);
        CHECK_EQ_U32(rd32(m + 4), 0x03020100u);
        CHECK_EQ_U32(rd32(m + 16), 0x03020100u);
        CHECK_EQ_U32(c->esi, m + 16);
        CHECK_EQ_U32(c->edi, m + 20);
        for (uint32_t i = 0; i < 64; i++) wr8(m + i, (uint8_t)i);
        c->esi = m + 8; c->edi = m; c->ecx = 16;          /* destino abaixo: como memmove */
        rep_movs(c, 1);
        CHECK_EQ_U32(rd8(m + 0), 8);
        CHECK_EQ_U32(rd8(m + 15), 23);
        c->ecx = 0; c->edi = 0;                           /* ecx 0: nada (nem o endereço inválido) */
        rep_movs(c, 4);
        rep_stos(c, 4);
        CHECK_EQ_U32(c->edi, 0);

        /* DF=1 (std): o memmove do CRT (0x00E69D6F) copia para cima, de trás para frente,
         * quando destino e origem se sobrepõem; ESI/EDI apontam para o último dword. */
        for (uint32_t i = 0; i < 64; i++) wr8(m + i, (uint8_t)i);
        c->df = 1;
        c->esi = m + 12; c->edi = m + 16; c->ecx = 4;     /* [m, m+16) -> [m+4, m+20) */
        rep_movs(c, 4);
        CHECK_EQ_U32(rd32(m + 4), 0x03020100u);           /* sem replicar: cópia fiel */
        CHECK_EQ_U32(rd32(m + 16), 0x0F0E0D0Cu);
        CHECK_EQ_U32(rd32(m + 0), 0x03020100u);           /* abaixo do destino: intacto */
        CHECK_EQ_U32(c->esi, m - 4);
        CHECK_EQ_U32(c->edi, m);
        for (uint32_t i = 0; i < 64; i++) wr8(m + i, (uint8_t)i);
        c->esi = m + 16; c->edi = m + 12; c->ecx = 4;     /* origem dentro do destino: replica, como o x86 */
        rep_movs(c, 4);
        CHECK_EQ_U32(rd32(m + 12), 0x13121110u);
        CHECK_EQ_U32(rd32(m + 0), 0x13121110u);
        c->edi = m + 60; c->ecx = 2; c->eax = 0;          /* stos para trás: [m+56, m+64) */
        rep_stos(c, 4);
        CHECK_EQ_U32(rd32(m + 56), 0);
        CHECK_EQ_U32(rd32(m + 60), 0);
        CHECK_EQ_U32(rd8(m + 55), 55);
        CHECK_EQ_U32(c->edi, m + 52);
        CHECK_EQ_U32(eflags_pack(c) & 0x400u, 0x400u);   /* pushfd vê o DF */
        eflags_unpack(c, 0x202u);                         /* popfd com DF=0 */
        CHECK_EQ_U32(c->df, 0);
        CHECK_EQ_U32(STR_STEP(c, 4), 4);
        c->df = 1;
        CHECK_EQ_U32(STR_STEP(c, 2), 0xFFFFFFFEu);
        c->df = 0;
    }

    /* ldmxcsr com FTZ|DAZ: guardado para o stmxcsr e aplicado ao SSE do host. */
    cpu_ldmxcsr(c, 0x9FC0u);
    CHECK_EQ_U32(c->mxcsr, 0x9FC0u);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK_EQ_U32(_mm_getcsr() & 0x8040u, 0x8040u);
    {
        volatile float tiny = 1.0e-38f, k = 1.0e-3f;
        CHECK(tiny * k == 0.0f);                  /* o produto subnormal vira zero */
    }
    cpu_ldmxcsr(c, 0x1F80u);
    CHECK_EQ_U32(_mm_getcsr() & 0x8040u, 0u);
    {
        volatile float tiny = 1.0e-38f, k = 1.0e-3f;
        CHECK(tiny * k != 0.0f);
    }
#else
    cpu_ldmxcsr(c, 0x1F80u);
#endif

    CHECK_DONE();
}
