/*
 * test_vk2_freelist.c - sub-alocador do vk2_ctx.c (vk2_freelist.h): alinhamento, partição, junção dos
 * vizinhos e conservação de bytes, inclusive num embaralhamento longo com invariantes.
 */
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "../../runtime/src/gfx/vk2/vk2_freelist.h"

static int sorted_disjoint(const vk2_fl *f)
{
    for (uint32_t i = 0; i < f->n; i++) {
        if (!f->s[i].size) return 0;
        if (i && f->s[i - 1].off + f->s[i - 1].size >= f->s[i].off) return 0;   /* vizinhos não juntados */
    }
    return 1;
}

int main(void)
{
    vk2_fl f;
    CHECK(vk2_fl_init(&f, 1024) == 0);
    uint64_t a, b, c;

    /* alinhamento: o começo do bloco serve; o seguinte sobe ao múltiplo */
    CHECK(vk2_fl_take(&f, 100, 64, &a) == 1 && a == 0);
    CHECK(vk2_fl_take(&f, 100, 64, &b) == 1 && b == 128);   /* 100 -> 128 */
    CHECK(f.n == 2 && f.s[0].off == 100 && f.s[0].size == 28);   /* o buraco de alinhamento volta à lista */
    CHECK(vk2_fl_take(&f, 20, 4, &c) == 1 && c == 100);          /* e serve a um pedido que cabe nele */
    CHECK(vk2_fl_free_bytes(&f) == 1024 - 100 - 100 - 20);

    /* devolver junta com os vizinhos: tudo volta a ser um trecho só */
    CHECK(vk2_fl_give(&f, a, 100) == 0);
    CHECK(vk2_fl_give(&f, c, 20) == 0);
    CHECK(vk2_fl_give(&f, b, 100) == 0);
    CHECK(f.n == 1 && f.s[0].off == 0 && f.s[0].size == 1024);

    /* não cabe */
    CHECK(vk2_fl_take(&f, 2000, 1, &a) == 0);
    CHECK(vk2_fl_take(&f, 1024, 1, &a) == 1 && a == 0 && f.n == 0);
    CHECK(vk2_fl_take(&f, 1, 1, &b) == 0);
    CHECK(vk2_fl_give(&f, 0, 1024) == 0 && f.n == 1);

    /* embaralhamento determinístico: nunca sobrepõe, conserva os bytes, sempre volta ao inteiro */
    enum { N = 400 };
    uint64_t off[N], sz[N];
    uint32_t live = 0, seed = 12345;
    uint64_t total = 1u << 20, used = 0;
    vk2_fl_destroy(&f);
    CHECK(vk2_fl_init(&f, total) == 0);
    for (int it = 0; it < 20000; it++) {
        seed = seed * 1664525u + 1013904223u;
        if ((seed >> 16) % 3 != 0 && live < N) {
            uint64_t s = 1 + (seed >> 8) % 3000, al = 1u << ((seed >> 4) % 7), o;
            if (vk2_fl_take(&f, s, al, &o)) {
                CHECK(o % al == 0 && o + s <= total);
                for (uint32_t k = 0; k < live; k++) CHECK(o + s <= off[k] || off[k] + sz[k] <= o);   /* sem sobreposição */
                off[live] = o;
                sz[live++] = s;
                used += s;
            }
        } else if (live) {
            uint32_t k = (seed >> 8) % live;
            CHECK(vk2_fl_give(&f, off[k], sz[k]) == 0);
            used -= sz[k];
            off[k] = off[live - 1];
            sz[k] = sz[live - 1];
            live--;
        }
        CHECK(sorted_disjoint(&f));
        CHECK(vk2_fl_free_bytes(&f) + used + 0 <= total);
    }
    while (live) {
        live--;
        CHECK(vk2_fl_give(&f, off[live], sz[live]) == 0);
    }
    CHECK(f.n == 1 && f.s[0].off == 0 && f.s[0].size == total);
    vk2_fl_destroy(&f);
    CHECK_DONE();
}
