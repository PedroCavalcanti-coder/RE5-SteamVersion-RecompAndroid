/*
 * vk_layout.h - contas de layout por sub-recurso do re5vk que não precisam de GPU (testáveis).
 *
 * Cada vk_image guarda o layout corrente e se há escrita pendente de barreira para cada
 * sub-recurso (índice layer * levels + level). Aqui fica a escolha de quais sub-recursos precisam
 * de barreira/transição para chegar a um layout, pulando os que estão presos como anexo.
 */
#ifndef RE5VK_LAYOUT_H
#define RE5VK_LAYOUT_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maior número de sub-recursos de uma imagem do jogo (16 níveis x 6 faces = 96). */
#define RE5VK_MAX_SUBS 128u

typedef struct {
    uint32_t layer, level;
} re5vk_subref;

/*
 * Sub-recursos de uma imagem (levels x layers) que não estão em 'want' ou têm escrita pendente
 * (dirty != 0), fora dos nskip de 'skip' (os anexos do passe).
 * out == NULL: só pergunta; devolve 1 no primeiro achado, 0 se nenhum.
 * out != NULL: grava até cap índices (layer * levels + level), em ordem, e devolve o total achado
 * (pode passar de cap; quem chama confere).
 */
uint32_t re5vk_layout_pending(const VkImageLayout *layouts, const uint8_t *dirty, uint32_t levels,
                              uint32_t layers, VkImageLayout want, const re5vk_subref *skip, uint32_t nskip,
                              uint32_t *out, uint32_t cap);

/*
 * Tudo em VK_IMAGE_LAYOUT_GENERAL com barreira global no fim do passe (o re5vk até 10/11)?
 * env = RE5VK_LAYOUT_GENERAL ("1" liga, "0" desliga, NULL/vazio = padrão).
 * Padrão: desligado (0) em todas as plataformas (Android e PC). Layouts ótimos por sub-recurso
 * preservam compressão AFBC no Mali e UBWC no Adreno, sincronizam riscos WAR/RAW exatamente
 * por imagem e eliminam barreiras globais no fim de passe.
 */
int re5vk_layout_general_choice(const char *env, int android);

#ifdef __cplusplus
}
#endif

#endif /* RE5VK_LAYOUT_H */
