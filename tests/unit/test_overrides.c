/* Overrides nativos (fase P5): re5_ov_00428D60 (sqrtf) igual ao que o lift faz com
 * `fld dword [ebp+8]; fsqrt` - sqrt em double do float do argumento, empilhado em st(0), esp += 4. */
#include <math.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

void re5_ov_00428D60(CPU *c);

static uint64_t bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

int main(void)
{
    CHECK(re5_mem_init() == 0);
    CHECK(re5_mem_map_fixed(0x00400000u, 0x10000u, RE5_PAGE_READWRITE, "pilha de teste") == 0);
    static CPU c;
    const float xs[] = {0.0f, -0.0f, 1.0f, 2.0f, 0.25f, 3.0f, 1e-30f, 1e30f, 123456.789f, -1.0f, -0.5f,
                        INFINITY, -INFINITY, NAN, 1.17549435e-38f, 1e-45f};
    for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        memset(&c, 0, sizeof c);
        c.esp = 0x0040F000u;
        wr32(c.esp, 0x00401234u);            /* endereço de retorno */
        wrf32(c.esp + 4u, xs[i]);
        uint32_t ebp = 0x11223344u;
        c.ebp = ebp;
        re5_ov_00428D60(&c);
        double ref = sqrt((double)xs[i]);
        CHECK_EQ_U32((uint32_t)(bits(*fst(&c, 0)) >> 32), (uint32_t)(bits(ref) >> 32));
        CHECK_EQ_U32((uint32_t)bits(*fst(&c, 0)), (uint32_t)bits(ref));
        CHECK_EQ_U32(c.esp, 0x0040F004u);    /* ret sem argumentos: só o endereço de retorno */
        CHECK_EQ_U32(c.ebp, ebp);
        CHECK_EQ_U32((uint32_t)c.fpu_top, 7u);   /* um valor empilhado */
    }
    return 0;
}
