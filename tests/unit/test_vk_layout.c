/*
 * test_vk_layout.c - re5vk_layout_pending (runtime/src/gfx/vk/vk_layout.c): quais sub-recursos de
 * uma imagem amostrada e presa como anexo precisam ir a GENERAL antes do draw.
 *
 * O caso que dava lixo no Adreno/Mali: textura com mips, desenho no mip 0 lendo a imagem inteira.
 * O descritor diz GENERAL; o mip 0 (anexo) vai a GENERAL no begin_rendering, e os outros mips
 * precisam ir também (antes ficavam em COLOR_ATTACHMENT/SHADER_READ_ONLY, sem barreira).
 */
#include <string.h>

#include "check.h"
#include "../../runtime/src/gfx/vk/vk_layout.h"

#define GEN VK_IMAGE_LAYOUT_GENERAL
#define COL VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
#define RO  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
#define DS  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL

int main(void)
{
    /* 1. 4 mips, 1 face: mip 0 é o anexo (pulado), mip 1 acabou de ser desenhado (COL, sujo),
     *    mip 2 já em leitura (RO), mip 3 já em GENERAL e limpo. */
    {
        VkImageLayout lay[4] = {COL, COL, RO, GEN};
        uint8_t dirty[4] = {1, 1, 0, 0};
        re5vk_subref att[1] = {{0, 0}};
        uint32_t out[RE5VK_MAX_SUBS];
        memset(out, 0xFF, sizeof out);
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 4, 1, GEN, att, 1, NULL, 0), 1);
        uint32_t n = re5vk_layout_pending(lay, dirty, 4, 1, GEN, att, 1, out, RE5VK_MAX_SUBS);
        CHECK_EQ_U32(n, 2);
        CHECK_EQ_U32(out[0], 1);
        CHECK_EQ_U32(out[1], 2);
        CHECK_EQ_U32(out[2], 0xFFFFFFFFu);   /* nada além do total */
    }

    /* 2. Já em GENERAL mas com escrita pendente: ainda precisa de barreira (memória). */
    {
        VkImageLayout lay[2] = {COL, GEN};
        uint8_t dirty[2] = {1, 1};
        re5vk_subref att[1] = {{0, 0}};
        uint32_t out[RE5VK_MAX_SUBS];
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 2, 1, GEN, att, 1, out, RE5VK_MAX_SUBS), 1);
        CHECK_EQ_U32(out[0], 1);
        dirty[1] = 0;   /* depois da barreira: nada a fazer, sem dividir o passe */
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 2, 1, GEN, att, 1, NULL, 0), 0);
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 2, 1, GEN, att, 1, out, RE5VK_MAX_SUBS), 0);
    }

    /* 3. Cubo 2 níveis x 6 faces, anexo na face 3 nível 0: índice = layer * levels + level. */
    {
        VkImageLayout lay[12];
        uint8_t dirty[12];
        for (int i = 0; i < 12; i++) {
            lay[i] = GEN;
            dirty[i] = 0;
        }
        lay[3 * 2 + 0] = COL;   /* o anexo: pulado mesmo fora de GENERAL */
        dirty[3 * 2 + 0] = 1;
        lay[3 * 2 + 1] = RO;    /* face 3, nível 1 */
        lay[5 * 2 + 0] = COL;   /* face 5, nível 0 */
        re5vk_subref att[1] = {{3, 0}};
        uint32_t out[RE5VK_MAX_SUBS];
        uint32_t n = re5vk_layout_pending(lay, dirty, 2, 6, GEN, att, 1, out, RE5VK_MAX_SUBS);
        CHECK_EQ_U32(n, 2);
        CHECK_EQ_U32(out[0], 7);
        CHECK_EQ_U32(out[1], 10);
        CHECK_EQ_U32(out[0] % 2, 1);   /* nível */
        CHECK_EQ_U32(out[0] / 2, 3);   /* face */
    }

    /* 4. Dois anexos da mesma imagem (MRT nas faces 0 e 1) e nenhum outro sub-recurso: nada a
     *    fazer. Sem anexos: os dois contam. */
    {
        VkImageLayout lay[2] = {COL, DS};
        uint8_t dirty[2] = {1, 1};
        re5vk_subref att[2] = {{0, 0}, {1, 0}};
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 1, 2, GEN, att, 2, NULL, 0), 0);
        uint32_t out[RE5VK_MAX_SUBS];
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 1, 2, GEN, NULL, 0, out, RE5VK_MAX_SUBS), 2);
    }

    /* 5. Mais achados que o cap: devolve o total, grava só cap (quem chama aborta alto). */
    {
        VkImageLayout lay[8];
        uint8_t dirty[8];
        for (int i = 0; i < 8; i++) {
            lay[i] = RO;
            dirty[i] = 0;
        }
        uint32_t out[4] = {0, 0, 0, 0};
        uint32_t guard = 0xABCDu;
        CHECK_EQ_U32(re5vk_layout_pending(lay, dirty, 8, 1, GEN, NULL, 0, out, 3), 8);
        CHECK_EQ_U32(out[2], 2);
        CHECK_EQ_U32(out[3], 0);
        CHECK_EQ_U32(guard, 0xABCDu);
    }

    /* 6. Sem rastreamento (imagem sem sub_layouts): nada. */
    CHECK_EQ_U32(re5vk_layout_pending(NULL, NULL, 4, 1, GEN, NULL, 0, NULL, 0), 0);

    /* 7. Padrão do modo GENERAL: desligado em todas as plataformas (0); RE5VK_LAYOUT_GENERAL=1 liga (1). */
    CHECK_EQ_U32(re5vk_layout_general_choice(NULL, 1), 0);
    CHECK_EQ_U32(re5vk_layout_general_choice("", 1), 0);
    CHECK_EQ_U32(re5vk_layout_general_choice(NULL, 0), 0);
    CHECK_EQ_U32(re5vk_layout_general_choice("0", 1), 0);
    CHECK_EQ_U32(re5vk_layout_general_choice("0", 0), 0);
    CHECK_EQ_U32(re5vk_layout_general_choice("1", 0), 1);
    CHECK_EQ_U32(re5vk_layout_general_choice("1", 1), 1);

    /* 8. Transição para SHADER_READ_ONLY_OPTIMAL: sub-recursos em COLOR_ATTACHMENT ou com escrita pendente. */
    {
        VkImageLayout lay[3] = {COL, RO, RO};
        uint8_t dirty[3] = {1, 1, 0};
        uint32_t out[RE5VK_MAX_SUBS];
        /* Mip 0 está em COL (precisa transicionar); Mip 1 está em RO mas dirty (precisa de barreira); Mip 2 limpo em RO. */
        uint32_t n = re5vk_layout_pending(lay, dirty, 3, 1, RO, NULL, 0, out, RE5VK_MAX_SUBS);
        CHECK_EQ_U32(n, 2);
        CHECK_EQ_U32(out[0], 0);
        CHECK_EQ_U32(out[1], 1);
    }

    /* 9. Transição para COLOR_ATTACHMENT_OPTIMAL: profundidade e texturas de entrada para render target. */
    {
        VkImageLayout lay[4] = {RO, RO, COL, DS};
        uint8_t dirty[4] = {0, 0, 0, 1};
        uint32_t out[RE5VK_MAX_SUBS];
        uint32_t n = re5vk_layout_pending(lay, dirty, 4, 1, COL, NULL, 0, out, RE5VK_MAX_SUBS);
        CHECK_EQ_U32(n, 3);
        CHECK_EQ_U32(out[0], 0);
        CHECK_EQ_U32(out[1], 1);
        CHECK_EQ_U32(out[2], 3);
    }

    /* 10. Depth/Stencil attachment transition: shadow map switching between attachment and sampling. */
    {
        VkImageLayout lay[2] = {DS, RO};
        uint8_t dirty[2] = {1, 0};
        uint32_t out[RE5VK_MAX_SUBS];
        /* Reuso como anexo de profundidade: sub 1 (amostrado antes) precisa ir a DS */
        uint32_t n = re5vk_layout_pending(lay, dirty, 1, 2, DS, NULL, 0, out, RE5VK_MAX_SUBS);
        CHECK_EQ_U32(n, 2); /* sub 0 é DS mas dirty (precisa barreira), sub 1 é RO (layout diff) */
        CHECK_EQ_U32(out[0], 0);
        CHECK_EQ_U32(out[1], 1);

        /* Se sub 0 é o anexo ativo pulado: só sub 1 conta */
        re5vk_subref skip[1] = {{0, 0}};
        n = re5vk_layout_pending(lay, dirty, 1, 2, DS, skip, 1, out, RE5VK_MAX_SUBS);
        CHECK_EQ_U32(n, 1);
        CHECK_EQ_U32(out[0], 1);
    }

    CHECK_DONE();
}
