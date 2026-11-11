/*
 * test_vk_tbdr.c - testes unitários do oráculo de passes TBDR (runtime/src/gfx/vk/vk_tbdr.c).
 */
#include <string.h>

#include "check.h"
#include "../../runtime/src/gfx/vk/vk_tbdr.h"

static re5vk_tbdr_pass_in make_pass(uint64_t rt0_id, re5vk_tbdr_sub *rt0_s, int rt0_clr,
                                    uint64_t ds_id, re5vk_tbdr_sub *ds_s, int has_stencil,
                                    int z_clr, int st_clr)
{
    re5vk_tbdr_pass_in in;
    memset(&in, 0, sizeof in);
    if (rt0_id) {
        in.color_img[0] = rt0_id;
        in.color_sub[0] = 0;
        in.color_subs[0] = rt0_s;
        if (rt0_clr) in.color_cleared |= 1u;
        else in.color_loaded |= 1u;
    }
    if (ds_id) {
        in.ds_img = ds_id;
        in.ds_sub = 0;
        in.ds_sub_state = ds_s;
        in.has_depth = 1;
        in.has_stencil = has_stencil ? 1u : 0u;
        in.depth_cleared = z_clr ? 1u : 0u;
        in.depth_loaded = z_clr ? 0u : 1u;
        in.stencil_cleared = (has_stencil && st_clr) ? 1u : 0u;
        in.stencil_loaded = (has_stencil && !st_clr) ? 1u : 0u;
    }
    return in;
}

int main(void)
{
    /* 1. Aquecimento (>= 2 quadros observados antes de DONT_CARE) + dead attachment + partial depth clear */
    re5vk_tbdr t;
    re5vk_tbdr_init(&t, 1);

    re5vk_tbdr_sub rt_tmp, ds, rt_out;
    re5vk_tbdr_sub_reset(&rt_tmp);
    re5vk_tbdr_sub_reset(&ds);
    re5vk_tbdr_sub_reset(&rt_out);

    for (int f = 0; f < 4; f++) {
        re5vk_tbdr_pass_in p0 = make_pass(1, &rt_tmp, 1, 2, &ds, 1, 1, 1);
        re5vk_tbdr_pass_out o0;
        re5vk_tbdr_begin_pass(&t, &p0, &o0);

        /* Passe 1: lê rt_tmp por shader, limpa APENAS depth do ds (mantém stencil!), escreve rt_out */
        re5vk_tbdr_mark_read(&t, &rt_tmp, RE5VK_TBDR_COLOR);
        re5vk_tbdr_pass_in p1 = make_pass(3, &rt_out, 1, 2, &ds, 1, 1, 0);
        re5vk_tbdr_pass_out o1;
        re5vk_tbdr_begin_pass(&t, &p1, &o1);
        /* Passe 2 (ou Present): backbuffer rt_out é lido, ds do passe 1 nunca é lido depois. */
        re5vk_tbdr_mark_read(&t, &rt_out, RE5VK_TBDR_COLOR);

        if (f < 2) {
            CHECK_EQ_U32(o0.color_store_dont_care, 0);
            CHECK_EQ_U32(o0.depth_store_dont_care, 0);
            CHECK_EQ_U32(o0.stencil_store_dont_care, 0);
            CHECK_EQ_U32(o1.color_store_dont_care, 0);
            CHECK_EQ_U32(o1.depth_store_dont_care, 0);
            CHECK_EQ_U32(o1.stencil_store_dont_care, 0);
        } else {
            CHECK_EQ_U32(o0.color_store_dont_care, 0);   /* rt_tmp lido pelo passe 1 */
            CHECK_EQ_U32(o0.depth_store_dont_care, 1);   /* depth sobrescrito sem leitura no passe 1 */
            CHECK_EQ_U32(o0.stencil_store_dont_care, 0); /* stencil carregado pelo passe 1 */
            CHECK_EQ_U32(o1.color_store_dont_care, 0);   /* rt_out lido antes do fim do quadro */
            CHECK_EQ_U32(o1.depth_store_dont_care, 1);   /* morto no fim do quadro */
            CHECK_EQ_U32(o1.stencil_store_dont_care, 1); /* morto no fim do quadro */
        }
        re5vk_tbdr_end_frame(&t);
    }
    CHECK_EQ_U32(t.stat_mismatches, 0);
    CHECK(t.stat_dont_care >= 6);

    /* 2. Persistência entre quadros (sticky_live): ex. cadeia de exposição HDR */
    re5vk_tbdr_init(&t, 1);
    re5vk_tbdr_sub hdr;
    re5vk_tbdr_sub_reset(&hdr);
    /* Quadro 1: escreve hdr sem leitura no quadro 1 */
    re5vk_tbdr_pass_in ph = make_pass(10, &hdr, 1, 0, NULL, 0, 0, 0);
    re5vk_tbdr_pass_out oh;
    re5vk_tbdr_begin_pass(&t, &ph, &oh);
    re5vk_tbdr_end_frame(&t);
    /* Quadro 2: lê hdr ANTES de sobrescrevê-lo (conteúdo do quadro 1 vivo entre quadros!) */
    re5vk_tbdr_mark_read(&t, &hdr, RE5VK_TBDR_COLOR);
    re5vk_tbdr_begin_pass(&t, &ph, &oh);
    re5vk_tbdr_end_frame(&t);
    CHECK((hdr.sticky_live & RE5VK_TBDR_COLOR) != 0);
    /* Quadros 3..5: mesmo sem leitura visível neste passe, sticky_live impede DONT_CARE para sempre */
    for (int f = 0; f < 3; f++) {
        re5vk_tbdr_begin_pass(&t, &ph, &oh);
        CHECK_EQ_U32(oh.color_store_dont_care, 0);
        re5vk_tbdr_end_frame(&t);
    }

    /* 3. Invalidação imediata em caso de mismatch (mudança de caminho de shader / cena) */
    re5vk_tbdr_init(&t, 1);
    re5vk_tbdr_sub dyn;
    re5vk_tbdr_sub_reset(&dyn);
    re5vk_tbdr_pass_in pd = make_pass(20, &dyn, 1, 0, NULL, 0, 0, 0);
    re5vk_tbdr_pass_out od;
    for (int f = 0; f < 2; f++) {
        re5vk_tbdr_begin_pass(&t, &pd, &od);
        re5vk_tbdr_end_frame(&t);
    }
    /* Quadro 3: oráculo autoriza DONT_CARE, mas uma leitura inesperada acontece depois! */
    re5vk_tbdr_begin_pass(&t, &pd, &od);
    CHECK_EQ_U32(od.color_store_dont_care, 1);
    re5vk_tbdr_mark_read(&t, &dyn, RE5VK_TBDR_COLOR);
    CHECK_EQ_U32(t.stat_mismatches, 1);
    re5vk_tbdr_end_frame(&t);
    /* Quadro 4+: permanentemente revogado */
    re5vk_tbdr_begin_pass(&t, &pd, &od);
    CHECK_EQ_U32(od.color_store_dont_care, 0);
    re5vk_tbdr_end_frame(&t);

    /* 4. Mesmo alvo escrito 2x no quadro (ordinal 0 lido via LOAD, ordinal 1 morto) + external_write */
    re5vk_tbdr_init(&t, 1);
    re5vk_tbdr_sub rt2;
    re5vk_tbdr_sub_reset(&rt2);
    for (int f = 0; f < 4; f++) {
        re5vk_tbdr_pass_in p_w0 = make_pass(30, &rt2, 1, 0, NULL, 0, 0, 0);
        re5vk_tbdr_pass_out o_w0;
        re5vk_tbdr_begin_pass(&t, &p_w0, &o_w0);

        re5vk_tbdr_pass_in p_load = make_pass(30, &rt2, 0, 0, NULL, 0, 0, 0); /* ordinal 1: color_loaded = 1 */
        re5vk_tbdr_pass_out o_load;
        re5vk_tbdr_begin_pass(&t, &p_load, &o_load);

        if (f >= 2) {
            CHECK_EQ_U32(o_w0.color_store_dont_care, 0);  /* alimentou p_load */
            CHECK_EQ_U32(o_load.color_store_dont_care, 1); /* ninguém leu depois de p_load */
        }
        re5vk_tbdr_end_frame(&t);
    }
    re5vk_tbdr_mark_external_write(&rt2, RE5VK_TBDR_COLOR);
    CHECK_EQ_U32((uint32_t)(rt2.writer_pass[0] < 0), 1);

    /* 5. Desligado (RE5VK_TBDR=0) */
    re5vk_tbdr_init(&t, 0);
    re5vk_tbdr_begin_pass(&t, &pd, &od);
    CHECK(od.pass_idx == -1);
    CHECK_EQ_U32(od.color_store_dont_care, 0);
    CHECK_EQ_U32(od.depth_store_dont_care, 0);
    CHECK_EQ_U32(od.stencil_store_dont_care, 0);

    CHECK_DONE();
}
