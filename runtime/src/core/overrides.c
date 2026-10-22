/*
 * overrides.c - funções do guest substituídas por C escrito à mão.
 *
 * Para cada VA em config/overrides.txt, o scripts/run_lift.py gera
 * `L_<VA>(c) { re5_override(c, VA); }` em vez do lift. Use quando o lift é
 * correto mas inviável (comportamento indefinido do x86 de que o código
 * depende, detecção de CPU, memcpy com saltos computados...) e documente o
 * motivo nos dois lugares.
 */
#include "re5/log.h"
#include "re5/runtime.h"

static void ov_00426230(CPU *c)
{
    /* HasCPUID(): x86 testa o bit ID de EFLAGS via pushfd/popfd.
     * Em host recompiled, a CPU sempre suporta CPUID. Retorna 1 em eax. */
    c->eax = 1;
    c->esp += 4; /* ret */
}

static const re5_override_def k_overrides[] = {
    {0x00426230u, ov_00426230, "HasCPUID: teste de EFLAGS.ID via pushfd/popfd -> sempre 1"},
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
