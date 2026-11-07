/*
 * overrides.c - funções do guest substituídas por C escrito à mão.
 *
 * Para cada VA em config/overrides.txt, o scripts/run_lift.py gera
 * `L_<VA>(c) { re5_override(c, VA); }` em vez do lift. Use quando o lift é
 * correto mas inviável (comportamento indefinido do x86 de que o código
 * depende, detecção de CPU, memcpy com saltos computados...) e documente o
 * motivo nos dois lugares.
 */
#include <string.h>

#include "re5/log.h"
#include "re5/runtime.h"

/* Os corpos são chamados direto pelo L_<VA> gerado (run_lift.py): sem busca na tabela. */
void re5_ov_00426230(CPU *c)
{
    /* HasCPUID(): x86 testa o bit ID de EFLAGS via pushfd/popfd.
     * Em host recompiled, a CPU sempre suporta CPUID. Retorna 1 em eax. */
    c->eax = 1;
    c->esp += 4; /* ret */
}

/* __sse2_available do CRT (_get_sse2_info, 0x00E7D4FE, chamada de 0x00E7D55E que grava 0x012B11AC):
 * o mesmo teste do bit ID por pushfd/popfd do HasCPUID, e depois cpuid(1).EDX bit 26 + um movapd
 * protegido por SEH. Sem o bit, o lift pulava o cpuid e o CRT inteiro rodava como numa CPU sem
 * SSE2: _ftol2 pelo caminho x87 (0x00E63DB6, 3,3% da thread principal no perfil de 04/11),
 * _controlfp sem o MXCSR e memset/memcpy sem a via vetorial. Toda CPU que roda o jogo tem SSE2 (o
 * próprio exe usa SSE2 solto), então devolve 1 como no Windows. Função cdecl sem argumentos. */
void re5_ov_00E7D4FE(CPU *c)
{
    c->eax = 1;
    c->esp += 4;
}

/* memset do CRT (0x00E63D00), memcpy (0x00E63EC0) e memmove (0x00E69B90), cdecl: (dst, val|src, n),
 * devolvem dst. O CRT do VS2005 trata sobreposição nos dois de cópia (o mesmo corpo), então os dois
 * viram memmove do host. Eram o 3º lugar do perfil em carregamento e aparecem em jogo; o lift
 * passava por laços de bytes e, com o SSE2 ligado acima, pelos laços de movdqa (_VEC_memcpy). */
void re5_ov_00E63D00(CPU *c)
{
    uint32_t d = rd32(c->esp + 4u), v = rd32(c->esp + 8u), n = rd32(c->esp + 12u);
    if (n) memset(rep_ptr(d), (int)(v & 0xFFu), n);
    c->eax = d;
    c->esp += 4u;
}

static void crt_memmove(CPU *c)
{
    uint32_t d = rd32(c->esp + 4u), s = rd32(c->esp + 8u), n = rd32(c->esp + 12u);
    if (n && d != s) memmove(rep_ptr(d), rep_ptr(s), n);
    c->eax = d;
    c->esp += 4u;
}

void re5_ov_00E63EC0(CPU *c) { crt_memmove(c); }
void re5_ov_00E69B90(CPU *c) { crt_memmove(c); }

/* _ftol2_sse (0x00E63D80) com o SSE2 ligado, que é o que o Windows roda: tira st(0), trunca para
 * int32 com cvttsd2si (fora da faixa ou NaN = 0x80000000) e devolve em eax. edx fica como estava
 * (o caminho SSE2 do CRT não o toca), ecx também. */
void re5_ov_00E63D80(CPU *c)
{
    double v = fpop(c);
    uint32_t r;
    if (v != v || v >= 2147483648.0 || v <= -2147483649.0) r = 0x80000000u;
    else r = (uint32_t)(int32_t)v;
    c->eax = r;
    c->esp += 4u;
}

/* _ftol2 x87 (0x00E63DB6; o _ftol2_sse cai nele sem SSE2 e 15 pontos o chamam direto, como o
 * limitador de quadro em 0x00426F42): tira st(0) e devolve em edx:eax o int64 truncado. O corpo do
 * CRT arredonda com fistp pelo RC atual e corrige pelo sinal da diferença, o que dá truncamento;
 * NaN e fora da faixa saem como o inteiro indefinido 0x8000000000000000 (o fistp). ecx do CRT é
 * lixo para quem chama. */
void re5_ov_00E63DB6(CPU *c)
{
    double v = fpop(c);
    uint64_t r;
    if (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) r = 0x8000000000000000ull;
    else r = (uint64_t)(int64_t)v;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
    c->esp += 4u;
}

/* sqrtf do jogo (fase P5): push ebp; fld dword [ebp+8]; fsqrt; pop ebp; ret. Era 9,7% das
 * amostras do perfil em jogo (28/10). Mesma conta do lift: sqrt em double do float do argumento,
 * resultado empilhado em st(0); ebp/esp voltam ao que eram, fora o `ret` (esp += 4). */
void re5_ov_00428D60(CPU *c)
{
    fpush(c, __builtin_sqrt((double)rdf32(c->esp + 4u)));
    c->esp += 4u;
}

/* IMA ADPCM do jogo: um passo. `x` é o índice novo sem limite; o retorno é o x86 ao pé da letra:
 * a = x < 0 ? 0 : x (not/sar/and), depois a + min(0, 0x58 - a) (sub/sar/and/add) = min(a, 88). */
static inline uint32_t adpcm_clamp(uint32_t x)
{
    uint32_t a = (uint32_t)((int32_t)~x >> 31) & x;
    uint32_t d = 0x58u - a;
    return a + ((uint32_t)((int32_t)d >> 31) & d);
}

/* Preditor += (2n - 15) * passo[idx] (lea + imul, 32 bits com volta); idx = limite(tab[n] + idx). */
#define ADPCM_STEP(nib)                                                                    \
    do {                                                                                   \
        pred += (2u * (nib) - 15u) * rd32(step + idx * 4u);                                \
        idx = adpcm_clamp(rd32(itab + (nib) * 4u) + idx);                                  \
    } while (0)

/* Decodificador IMA ADPCM (4 bits -> amostra de 32 bits) do áudio do jogo, fase P5: 6,6% das
 * amostras do perfil em jogo. Convenção própria (LTCG): eax = número de amostras, ecx = objeto
 * cujo [ecx] é o destino, [esp+4] = bytes de entrada, [esp+8] = estado {+0 preditor, +0x18
 * índice, +0x30 byte guardado, +0x34 meio byte pendente}; `ret` sem limpar argumentos.
 *
 * Reproduz tudo o que o original deixa visível: as gravações da pilha (pushes, a contagem
 * gravada por cima do 1º argumento, o contador do laço no lugar do ecx salvo), o estado e os
 * registradores de volta (eax, ecx, edx; ebx/ebp/esi/edi restaurados). Inclusive a saída
 * antecipada que não grava preditor/índice no estado e devolve edx = estado. Flags: o lift não
 * as devolve (mortas no `ret`), e aqui também não. Validado por scripts/difftest_override.py. */
void re5_ov_006702B0(CPU *c)
{
    const uint32_t S = c->esp, step = GVA(0x0109AAC0u), itab = GVA(0x0109AA80u);
    const uint32_t in0 = rd32(S + 4u), st = rd32(S + 8u), ecx0 = c->ecx;
    uint32_t n = c->eax, in = in0, out = rd32(ecx0);
    uint32_t pred = rd32(st), idx = rd32(st + 0x18u);

    wr32(S - 4u, ecx0);
    wr32(S - 8u, c->ebx);
    wr32(S - 12u, c->ebp);
    wr32(S - 16u, c->esi);
    wr32(S - 20u, c->edi);
    wr32(S + 4u, n);
    if (rd32(st + 0x34u) != 0) {
        uint32_t lo = rd32(st + 0x30u) & 0xFu;
        ADPCM_STEP(lo);
        wr32(out, pred);
        out += 4u;
        n -= 1u;
        wr32(S + 4u, n);
        if (n == 0) {
            wr32(st + 0x34u, 0);
            c->eax = idx;
            c->ecx = ecx0;
            c->edx = st;
            c->esp = S + 4u;
            return;
        }
    }
    uint32_t pairs = n >> 1;
    uint32_t ecx_ret = ecx0;
    if (pairs) {
        for (uint32_t k = pairs; k; k--) {
            uint32_t b = rd8(in);
            ADPCM_STEP(b >> 4);
            wr32(out, pred);
            ADPCM_STEP(b & 0xFu);
            wr32(out + 4u, pred);
            out += 8u;
            in += 1u;
        }
        wr32(S - 4u, 0);      /* o contador do laço, no lugar do ecx salvo, termina em 0 */
        ecx_ret = 0;
    }
    if (n & 1u) {
        uint32_t b = rd8(in);
        ADPCM_STEP(b >> 4);
        wr32(out, pred);
        wr32(st + 0x30u, b);
        wr32(st + 0x34u, 1);
    } else {
        wr32(st + 0x34u, 0);
    }
    wr32(st, pred);
    wr32(st + 0x18u, idx);
    c->eax = idx;
    c->ecx = ecx_ret;
    c->edx = pred;
    c->esp = S + 4u;
}
#undef ADPCM_STEP

/* Propaga posição e cor pela cadeia de pais (+0x144) de um nó, da raiz para baixo, fase P5:
 * 3,8% das amostras do perfil em jogo, quase tudo ponteiro seguido pelo lift. Em cada filho:
 * +0x190 e +0x194 = pai.+0x190 + +0x3c; bytes +0x198..0x19a = +0x34; +0x19b = alfa do pai *
 * alfa (+0x37) / 255 pela multiplicação mágica do MSVC. A cadeia vai para o vetor de 0x100
 * bytes na pilha do guest como no original (inclusive o estouro além de 64 nós), e a ordem das
 * leituras e escritas é a mesma, para valer até com nós que se sobrepõem. */
void re5_ov_00CD5FB0(CPU *c)
{
    const uint32_t S = c->esp, arr = S - 0x100u;
    uint32_t node = c->ecx, i = 0;

    wr32(S - 0x104u, c->esi);
    do {
        wr32(arr + i * 4u, node);
        node = rd32(node + 0x144u);
        i++;
    } while (node);
    uint32_t root = rd32(arr + (i - 1u) * 4u);
    uint32_t v = rd32(root + 0x3cu);
    wr32(root + 0x190u, v);
    wr32(root + 0x194u, v);
    v = rd32(root + 0x34u);
    wr32(root + 0x198u, v);
    int32_t k = (int32_t)(i - 2u);
    if (k < 0) {
        c->eax = root;
        c->ecx = v;
        c->esp = S + 4u;
        return;
    }
    wr32(S - 0x108u, c->edi);
    uint32_t eax = 0, edx = 0, n = 0;
    for (; k >= 0; k--) {
        n = rd32(arr + (uint32_t)k * 4u);
        uint32_t p = rd32(n + 0x144u), e = rd32(n + 0x3cu);
        wr32(n + 0x190u, rd32(p + 0x190u) + e);
        wr32(n + 0x194u, rd32(p + 0x190u) + e);
        uint32_t col = rd32(n + 0x34u);
        wr8(n + 0x19au, (uint8_t)(col >> 16));
        wr8(n + 0x199u, (uint8_t)(col >> 8));
        wr8(n + 0x198u, (uint8_t)col);
        uint64_t prod = 0x80808081ull * (uint32_t)(rd8(p + 0x19bu) * (col >> 24));
        eax = (uint32_t)prod;
        edx = (uint32_t)(prod >> 32) >> 7;
        wr8(n + 0x19bu, (uint8_t)edx);
    }
    c->eax = eax;
    c->ecx = n;
    c->edx = edx;
    c->esp = S + 4u;
}

static const re5_override_def k_overrides[] = {
    {0x00426230u, re5_ov_00426230, "HasCPUID: teste de EFLAGS.ID via pushfd/popfd -> sempre 1"},
    {0x00E7D4FEu, re5_ov_00E7D4FE, "_get_sse2_info do CRT: teste de EFLAGS.ID via pushfd/popfd -> SSE2 sempre"},
    {0x00E63D00u, re5_ov_00E63D00, "memset do CRT -> memset do host"},
    {0x00E63EC0u, re5_ov_00E63EC0, "memcpy do CRT (trata sobreposição) -> memmove do host"},
    {0x00E69B90u, re5_ov_00E69B90, "memmove do CRT -> memmove do host"},
    {0x00E63D80u, re5_ov_00E63D80, "_ftol2_sse do CRT: cvttsd2si de st(0) em eax"},
    {0x00E63DB6u, re5_ov_00E63DB6, "_ftol2 x87 do CRT: st(0) truncado para int64 em edx:eax"},
    {0x00428D60u, re5_ov_00428D60, "sqrtf: fld dword [ebp+8]; fsqrt - folha pura, 9,7% das amostras em jogo"},
    {0x006702B0u, re5_ov_006702B0, "IMA ADPCM do áudio - folha pura, 6,6% das amostras em jogo"},
    {0x00CD5FB0u, re5_ov_00CD5FB0, "cadeia de pais (posição/cor/alfa) - folha pura, 3,8% das amostras em jogo"},
    {0, NULL, NULL},
};

re5_lifted_fn re5_override_get(uint32_t va)
{
    for (const re5_override_def *o = k_overrides; o->fn; o++) {
        if (o->va == va) return o->fn;
    }
    return NULL;
}

void re5_override(CPU *c, uint32_t va)
{
    re5_lifted_fn fn = re5_override_get(va);
    if (fn) {
        fn(c);
        return;
    }
    re5_fatal("override", "0x%08X está em config/overrides.txt mas não tem corpo em runtime/src/core/overrides.c", va);
}
