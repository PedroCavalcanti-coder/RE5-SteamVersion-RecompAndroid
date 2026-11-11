/*
 * vk_tbdr.h - oráculo de passes para GPUs TBDR (Adreno / Mali) no re5vk.
 *
 * Em GPUs baseadas em tiles, VK_ATTACHMENT_STORE_OP_STORE grava a memória de tile de volta
 * na RAM ao fim de todo passe, mesmo para buffers de profundidade ou RTs intermediários que
 * nenhum passe seguinte lê antes do próximo Clear completo ou do fim do quadro.
 *
 * O oráculo observa os passes do quadro N e só autoriza VK_ATTACHMENT_STORE_OP_DONT_CARE em
 * um anexo quando:
 *   1) o mesmo passe (mesmos alvos e ocorrência no quadro) apareceu >= 2 quadros seguidos;
 *   2) nenhum passe/leitura (LOAD, amostragem, StretchRect, GetRenderTargetData, Present)
 *      consumiu aquele aspecto antes de um novo Clear completo ou do fim do quadro;
 *   3) o sub-recurso nunca foi marcado como persistente entre quadros (sticky_live).
 *
 * RE5VK_TBDR=0 desliga o oráculo (mantém STORE sempre).
 */
#ifndef RE5VK_TBDR_H
#define RE5VK_TBDR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RE5VK_TBDR_MAX_PASSES 512u
#define RE5VK_TBDR_MAP_CAP    1024u

enum {
    RE5VK_TBDR_COLOR   = 1u,
    RE5VK_TBDR_DEPTH   = 2u,
    RE5VK_TBDR_STENCIL = 4u,
};

typedef struct {
    uint64_t img_id;    /* ID único monotônico da vk_image (0 = ausente) */
    uint32_t sub;       /* layer * levels + level */
    uint8_t  cleared;   /* 1 se LOAD_OP_CLEAR total (sobrescreve sem ler escritor anterior) */
    uint8_t  loaded;    /* 1 se LOAD_OP_LOAD (lê o conteúdo do escritor anterior) */
} re5vk_tbdr_att_desc;

typedef struct {
    int32_t  writer_pass[3];   /* índice do último passe no quadro em writer_frame[k]; -1 = nenhum */
    uint32_t writer_frame[3];  /* quadro de writer_pass[k] (0 = color, 1 = depth, 2 = stencil) */
    uint64_t writer_key[3];    /* chave do passe escritor (para invalidação entre quadros) */
    uint8_t  writer_dc;        /* bits RE5VK_TBDR_* se o escritor acima usou DONT_CARE */
    uint8_t  sticky_live;      /* RE5VK_TBDR_* : já foi lido entre quadros -> nunca usar DONT_CARE */
} re5vk_tbdr_sub;

typedef struct {
    uint64_t color_img[4];
    uint32_t color_sub[4];
    uint8_t  color_cleared;    /* bitmask bits 0..3 */
    uint8_t  color_loaded;     /* bitmask bits 0..3 */
    uint64_t ds_img;
    uint32_t ds_sub;
    uint8_t  has_depth;
    uint8_t  has_stencil;
    uint8_t  depth_cleared;
    uint8_t  depth_loaded;
    uint8_t  stencil_cleared;
    uint8_t  stencil_loaded;
    re5vk_tbdr_sub *color_subs[4];
    re5vk_tbdr_sub *ds_sub_state;
} re5vk_tbdr_pass_in;

typedef struct {
    int32_t pass_idx;              /* -1 se excedeu RE5VK_TBDR_MAX_PASSES */
    uint8_t color_store_dont_care; /* bits 0..3 */
    uint8_t depth_store_dont_care;
    uint8_t stencil_store_dont_care;
} re5vk_tbdr_pass_out;

typedef struct {
    uint64_t target_sig;
    uint64_t pass_key;
    uint8_t  color_mask;
    uint8_t  has_depth;
    uint8_t  has_stencil;
    uint8_t  color_live;           /* bits 0..3: precisou de STORE neste quadro */
    uint8_t  depth_live;
    uint8_t  stencil_live;
    uint8_t  color_used_dc;        /* bits 0..3: emitiu DONT_CARE neste quadro */
    uint8_t  depth_used_dc;
    uint8_t  stencil_used_dc;
    uint8_t  color_sticky_blocked; /* bits 0..3: anexo tinha sticky_live */
    uint8_t  depth_sticky_blocked;
    uint8_t  stencil_sticky_blocked;
} re5vk_tbdr_rec;

typedef struct {
    uint64_t pass_key;
    uint8_t  used;
    uint8_t  obs_count;      /* quantos quadros válidos viram esta chave (cap 255) */
    uint8_t  color_live;     /* bits 0..3: se 1 em qualquer quadro, nunca mais descarta */
    uint8_t  depth_live;
    uint8_t  stencil_live;
} re5vk_tbdr_hist;

typedef struct {
    int             enabled;
    uint32_t        frame_id;
    uint32_t        n_cur;
    re5vk_tbdr_rec  cur[RE5VK_TBDR_MAX_PASSES];
    re5vk_tbdr_hist map[RE5VK_TBDR_MAP_CAP];
    uint32_t        stat_dont_care;
    uint32_t        stat_mismatches;
} re5vk_tbdr;

void re5vk_tbdr_init(re5vk_tbdr *t, int enabled);
void re5vk_tbdr_sub_reset(re5vk_tbdr_sub *s);
void re5vk_tbdr_begin_pass(re5vk_tbdr *t, const re5vk_tbdr_pass_in *in, re5vk_tbdr_pass_out *out);
void re5vk_tbdr_mark_read(re5vk_tbdr *t, re5vk_tbdr_sub *s, uint8_t aspect_mask);
void re5vk_tbdr_mark_external_write(re5vk_tbdr_sub *s, uint8_t aspect_mask);
void re5vk_tbdr_end_frame(re5vk_tbdr *t);

#ifdef __cplusplus
}
#endif

#endif /* RE5VK_TBDR_H */
