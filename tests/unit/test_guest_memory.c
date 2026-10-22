/* Espaço do guest: base de 4 GiB, mapeamento fixo, alocação, proteção, acesso via cpu.h. */
#include <string.h>

#include "check.h"
#include "re5/runtime.h"

/* O runtime não define a tabela de funções geradas: cada executável define a sua. */
const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

int main(void)
{
    CHECK(re5_mem_init() == 0);
    CHECK(re5_guest_base != 0);

    /* A imagem do jogo vai para 0x00400000 e não pode ser mapeada duas vezes. */
    CHECK(re5_mem_map_fixed(0x00400000u, 0x18FF000u, RE5_PAGE_READWRITE, "imagem") == 0);
    CHECK(re5_mem_map_fixed(0x00800000u, 0x1000u, RE5_PAGE_READWRITE, "sobreposta") != 0);
    CHECK(re5_mem_map_fixed(0x00001000u, 0x1000u, RE5_PAGE_READWRITE, "nulo") != 0);  /* 64 KiB nulos */
    CHECK(re5_mem_is_mapped(0x00400000u, 0x18FF000u));
    CHECK(!re5_mem_is_mapped(0x00400000u, 0x18FF001u));

    /* Os acessos do cpu.h gerado passam pela base: o guest escreve, o host lê em base+va. */
    wr32(0x00401000u, 0xDEADBEEFu);
    CHECK_EQ_U32(rd32(0x00401000u), 0xDEADBEEFu);
    CHECK_EQ_U32(*(uint32_t *)re5_g2h(0x00401000u), 0xDEADBEEFu);
    wr32(0x00401003u, 0x11223344u);   /* desalinhado, como o x86 permite */
    CHECK_EQ_U32(rd32(0x00401003u), 0x11223344u);
    CHECK_EQ_U32(rd8(0x00401003u), 0x44u);

    /* Alocações dinâmicas: alinhadas a 64 KiB, zeradas, sem sobreposição, reaproveitáveis. */
    uint32_t a = re5_mem_alloc(100, RE5_PAGE_READWRITE, "a");
    uint32_t b = re5_mem_alloc(0x20000, RE5_PAGE_READWRITE, "b");
    CHECK(a >= RE5_MEM_DYN_LO && b >= RE5_MEM_DYN_LO);
    CHECK((a % RE5_MEM_GRANULARITY) == 0 && (b % RE5_MEM_GRANULARITY) == 0);
    CHECK(b >= a + RE5_MEM_GRANULARITY || a >= b + 0x20000u);
    CHECK_EQ_U32(rd32(b + 0x1FFFCu), 0);
    wr32(a, 0x12345678u);
    CHECK(re5_mem_free(a) == 0);
    CHECK(re5_mem_free(a) != 0);
    uint32_t a2 = re5_mem_alloc(100, RE5_PAGE_READWRITE, "a2");
    CHECK_EQ_U32(a2, a);               /* o buraco é reaproveitado... */
    CHECK_EQ_U32(rd32(a2), 0);         /* ...e volta zerado */

    re5_mem_region r;
    CHECK(re5_mem_query(b + 0x100u, &r) == 0);
    CHECK_EQ_U32(r.base, b);
    CHECK_EQ_U32(r.size, 0x20000u);
    uint32_t old = 0;
    CHECK(re5_mem_protect(b, 0x1000u, RE5_PAGE_READONLY, &old) == 0);
    CHECK_EQ_U32(old, RE5_PAGE_READWRITE);
    CHECK(re5_mem_query(0x0F000000u, &r) != 0);

    CHECK_DONE();
}
