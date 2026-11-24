/*
 * vk_tbdr.c - implementação do oráculo de passes TBDR (ver vk_tbdr.h).
 */
#include "vk_tbdr.h"

#include <string.h>

static uint64_t fnv64_mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v & 0xFFu);
        h *= 0x100000001b3ull;
        v >>= 8;
    }
    return h;
}

static uint64_t calc_target_sig(const re5vk_tbdr_pass_in *in)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 4; i++) {
        h = fnv64_mix(h, in->color_img[i]);
        h = fnv64_mix(h, (uint64_t)in->color_sub[i]);
    }
    h = fnv64_mix(h, in->ds_img);
    h = fnv64_mix(h, (uint64_t)in->ds_sub);
    h = fnv64_mix(h, ((uint64_t)in->has_depth << 8) | in->has_stencil);
    return h ? h : 1ull;
}

static re5vk_tbdr_hist *hist_find(re5vk_tbdr *t, uint64_t pass_key, int create)
{
    uint32_t mask = RE5VK_TBDR_MAP_CAP - 1u;
    uint32_t idx = (uint32_t)(pass_key ^ (pass_key >> 32)) & mask;
    for (uint32_t step = 0; step < RE5VK_TBDR_MAP_CAP; step++) {
        re5vk_tbdr_hist *e = &t->map[(idx + step) & mask];
        if (!e->used) {
            if (!create) return NULL;
            memset(e, 0, sizeof *e);
            e->used = 1;
            e->pass_key = pass_key;
            return e;
        }
        if (e->pass_key == pass_key) return e;
    }
    return NULL;
}

static void mark_aspect_read(re5vk_tbdr *t, re5vk_tbdr_sub *s, int slot, uint8_t bit, uint8_t color_slot_hint)
{
    if (!t || !s) return;
    int32_t wp = s->writer_pass[slot];
    if (wp < 0) return;

    if (s->writer_frame[slot] == t->frame_id && (uint32_t)wp < t->n_cur) {
        re5vk_tbdr_rec *r = &t->cur[wp];
        if (slot == 0) {
            uint8_t cmask = (color_slot_hint < 4) ? (uint8_t)(1u << color_slot_hint) : 0x0Fu;
            cmask &= r->color_mask;
            r->color_live |= cmask;
            if (r->color_used_dc & cmask) {
                t->stat_mismatches++;
                t->stats.stat_mismatches++;
                s->sticky_live |= RE5VK_TBDR_COLOR;
                re5vk_tbdr_hist *h = hist_find(t, r->pass_key, 1);
                if (h) h->color_live |= cmask;
            }
        } else if (slot == 1) {
            r->depth_live = 1;
            if (r->depth_used_dc) {
                t->stat_mismatches++;
                t->stats.stat_mismatches++;
                s->sticky_live |= RE5VK_TBDR_DEPTH;
                re5vk_tbdr_hist *h = hist_find(t, r->pass_key, 1);
                if (h) h->depth_live = 1;
            }
        } else {
            r->stencil_live = 1;
            if (r->stencil_used_dc) {
                t->stat_mismatches++;
                t->stats.stat_mismatches++;
                s->sticky_live |= RE5VK_TBDR_STENCIL;
                re5vk_tbdr_hist *h = hist_find(t, r->pass_key, 1);
                if (h) h->stencil_live = 1;
            }
        }
        return;
    }

    if (s->writer_frame[slot] < t->frame_id) {
        /* Conteúdo escrito em quadro anterior ainda vivo neste quadro -> nunca mais descartar */
        s->sticky_live |= bit;
        if (s->writer_dc & bit) {
            t->stat_mismatches++;
            t->stats.stat_mismatches++;
            s->writer_dc &= (uint8_t)~bit;
        }
        if (s->writer_key[slot]) {
            re5vk_tbdr_hist *h = hist_find(t, s->writer_key[slot], 1);
            if (h) {
                if (slot == 0) h->color_live |= 0x0Fu;
                else if (slot == 1) h->depth_live = 1;
                else h->stencil_live = 1;
            }
        }
    }
}

void re5vk_tbdr_init(re5vk_tbdr *t, int enabled)
{
    memset(t, 0, sizeof *t);
    t->enabled = enabled ? 1 : 0;
    t->frame_id = 1;
}

void re5vk_tbdr_get_stats(const re5vk_tbdr *t, re5vk_tbdr_stats *out)
{
    if (!t || !out) return;
    *out = t->stats;
}

void re5vk_tbdr_reset_stats(re5vk_tbdr *t)
{
    if (!t) return;
    memset(&t->stats, 0, sizeof t->stats);
}

void re5vk_tbdr_sub_reset(re5vk_tbdr_sub *s)
{
    if (!s) return;
    s->writer_pass[0] = s->writer_pass[1] = s->writer_pass[2] = -1;
    s->writer_frame[0] = s->writer_frame[1] = s->writer_frame[2] = 0;
    s->writer_key[0] = s->writer_key[1] = s->writer_key[2] = 0;
    s->writer_dc = 0;
    s->sticky_live = 0;
}

void re5vk_tbdr_begin_pass(re5vk_tbdr *t, const re5vk_tbdr_pass_in *in, re5vk_tbdr_pass_out *out)
{
    memset(out, 0, sizeof *out);
    out->pass_idx = -1;
    if (!t || !t->enabled || !in) return;

    /* Primeiro propaga dependências de leitura dos anexos que entram em LOAD neste passe. */
    for (int i = 0; i < 4; i++) {
        if (!in->color_img[i] || !in->color_subs[i]) continue;
        if (in->color_loaded & (1u << i))
            mark_aspect_read(t, in->color_subs[i], 0, RE5VK_TBDR_COLOR, (uint8_t)i);
    }
    if (in->ds_img && in->ds_sub_state) {
        if (in->has_depth && in->depth_loaded)
            mark_aspect_read(t, in->ds_sub_state, 1, RE5VK_TBDR_DEPTH, 0);
        if (in->has_stencil && in->stencil_loaded)
            mark_aspect_read(t, in->ds_sub_state, 2, RE5VK_TBDR_STENCIL, 0);
    }

    if (t->n_cur >= RE5VK_TBDR_MAX_PASSES) return;

    uint64_t tsig = calc_target_sig(in);
    uint32_t ord = 0;
    for (uint32_t i = 0; i < t->n_cur; i++)
        if (t->cur[i].target_sig == tsig && !t->cur[i].is_split) ord++;
    uint64_t pkey = fnv64_mix(tsig, (uint64_t)ord);
    if (!pkey) pkey = 1ull;

    int32_t idx = (int32_t)t->n_cur++;
    re5vk_tbdr_rec *r = &t->cur[idx];
    memset(r, 0, sizeof *r);
    r->target_sig = tsig;
    r->pass_key = pkey;
    r->is_split = in->is_split ? 1 : 0;
    for (int i = 0; i < 4; i++)
        if (in->color_img[i]) r->color_mask |= (uint8_t)(1u << i);
    r->has_depth = in->has_depth ? 1 : 0;
    r->has_stencil = in->has_stencil ? 1 : 0;

    re5vk_tbdr_hist *h = hist_find(t, pkey, 0);

    /* 1. SE É SPLIT: Força STORE em tudo (continuidade física do tile entre draws) */
    if (in->is_split) {
        t->stats.stat_splits++;
        t->stats.stat_conservative_fallback++;
        /* out->*_store_dont_care permanece 0 */
    } else {
        /* 2. STENCIL: Regra determinística pura D3D9 */
        int stencil_active = in->stencil_enabled || in->stencil_cleared || in->stencil_loaded;
        if (r->has_stencil) {
            if (!stencil_active) {
                /* Stencil não ativado nem limpo nem carregado neste passe: descarta sem custo de memória */
                out->stencil_store_dont_care = 1;
                r->stencil_used_dc = 1;
                t->stats.dont_care_stencil++;
                t->stats.stat_discarded++;
                t->stats.bytes_saved_est += (uint64_t)in->width * in->height * 1;
            } else if (h && h->obs_count >= 2) {
                uint8_t sticky = in->ds_sub_state ? (in->ds_sub_state->sticky_live & RE5VK_TBDR_STENCIL) : 1u;
                if (sticky) {
                    r->stencil_sticky_blocked = 1;
                    t->stats.store_stencil++;
                    t->stats.stat_conservative_fallback++;
                } else if (!h->stencil_live) {
                    out->stencil_store_dont_care = 1;
                    r->stencil_used_dc = 1;
                    t->stats.dont_care_stencil++;
                    t->stats.stat_discarded++;
                    t->stats.bytes_saved_est += (uint64_t)in->width * in->height * 1;
                } else {
                    t->stats.store_stencil++;
                }
            } else {
                t->stats.store_stencil++;
                t->stats.stat_conservative_fallback++;
            }
        }

        /* 3. COLOR: Regra estrita de proteção de texturas e backbuffer */
        for (int i = 0; i < 4; i++) {
            if (!(r->color_mask & (1u << i))) continue;
            /* Backbuffer ou Textura de RT: SEMPRE STORE (elimina lixo de textura e tela preta) */
            if (in->color_is_backbuffer[i] || in->color_is_texture[i]) {
                t->stats.store_color++;
                t->stats.stat_conservative_fallback++;
                continue;
            }
            uint8_t sticky = in->color_subs[i] ? (in->color_subs[i]->sticky_live & RE5VK_TBDR_COLOR) : 1u;
            if (sticky) {
                r->color_sticky_blocked |= (uint8_t)(1u << i);
                t->stats.store_color++;
                t->stats.stat_conservative_fallback++;
                continue;
            }
            if (h && h->obs_count >= 2 && !(h->color_live & (1u << i))) {
                out->color_store_dont_care |= (uint8_t)(1u << i);
                r->color_used_dc |= (uint8_t)(1u << i);
                t->stats.dont_care_color++;
                t->stats.stat_discarded++;
                t->stats.bytes_saved_est += (uint64_t)in->width * in->height * 4;
            } else {
                t->stats.store_color++;
                t->stats.stat_conservative_fallback++;
            }
        }

        /* 4. DEPTH: Regra estrita de proteção de Shadow Maps e descarte seguro de Depth Standalone */
        if (r->has_depth) {
            /* Textura de profundidade (Shadow Map): SEMPRE STORE quando escrito (elimina sombras quebradas) */
            if (in->ds_is_texture) {
                t->stats.store_depth++;
                t->stats.stat_conservative_fallback++;
            } else {
                uint8_t sticky = in->ds_sub_state ? (in->ds_sub_state->sticky_live & RE5VK_TBDR_DEPTH) : 1u;
                if (sticky) {
                    r->depth_sticky_blocked = 1;
                    t->stats.store_depth++;
                    t->stats.stat_conservative_fallback++;
                } else if (h && h->obs_count >= 2 && !h->depth_live) {
                    out->depth_store_dont_care = 1;
                    r->depth_used_dc = 1;
                    t->stats.dont_care_depth++;
                    t->stats.stat_discarded++;
                    t->stats.bytes_saved_est += (uint64_t)in->width * in->height * 4;
                } else {
                    t->stats.store_depth++;
                    t->stats.stat_conservative_fallback++;
                }
            }
        }
    }

    t->stats.total_passes++;
    t->stats.total_store = t->stats.store_color + t->stats.store_depth + t->stats.store_stencil;
    t->stats.total_dont_care = t->stats.dont_care_color + t->stats.dont_care_depth + t->stats.dont_care_stencil;
    t->stat_dont_care = t->stats.total_dont_care;

    /* Atualiza os escritores correntes para os passes/leituras seguintes. */
    for (int i = 0; i < 4; i++) {
        if (!in->color_img[i] || !in->color_subs[i]) continue;
        in->color_subs[i]->writer_pass[0] = idx;
        in->color_subs[i]->writer_frame[0] = t->frame_id;
        in->color_subs[i]->writer_key[0] = pkey;
        if (out->color_store_dont_care & (1u << i)) in->color_subs[i]->writer_dc |= RE5VK_TBDR_COLOR;
        else in->color_subs[i]->writer_dc &= (uint8_t)~RE5VK_TBDR_COLOR;
    }
    if (in->ds_img && in->ds_sub_state) {
        if (in->has_depth) {
            in->ds_sub_state->writer_pass[1] = idx;
            in->ds_sub_state->writer_frame[1] = t->frame_id;
            in->ds_sub_state->writer_key[1] = pkey;
            if (out->depth_store_dont_care) in->ds_sub_state->writer_dc |= RE5VK_TBDR_DEPTH;
            else in->ds_sub_state->writer_dc &= (uint8_t)~RE5VK_TBDR_DEPTH;
        }
        if (in->has_stencil) {
            in->ds_sub_state->writer_pass[2] = idx;
            in->ds_sub_state->writer_frame[2] = t->frame_id;
            in->ds_sub_state->writer_key[2] = pkey;
            if (out->stencil_store_dont_care) in->ds_sub_state->writer_dc |= RE5VK_TBDR_STENCIL;
            else in->ds_sub_state->writer_dc &= (uint8_t)~RE5VK_TBDR_STENCIL;
        }
    }

    out->pass_idx = idx;
}

void re5vk_tbdr_mark_read(re5vk_tbdr *t, re5vk_tbdr_sub *s, uint8_t aspect_mask)
{
    if (!t || !t->enabled || !s) return;
    if (aspect_mask & RE5VK_TBDR_COLOR) mark_aspect_read(t, s, 0, RE5VK_TBDR_COLOR, 4);
    if (aspect_mask & RE5VK_TBDR_DEPTH) mark_aspect_read(t, s, 1, RE5VK_TBDR_DEPTH, 0);
    if (aspect_mask & RE5VK_TBDR_STENCIL) mark_aspect_read(t, s, 2, RE5VK_TBDR_STENCIL, 0);
}

void re5vk_tbdr_mark_external_write(re5vk_tbdr_sub *s, uint8_t aspect_mask)
{
    if (!s) return;
    if (aspect_mask & RE5VK_TBDR_COLOR) s->writer_pass[0] = -1;
    if (aspect_mask & RE5VK_TBDR_DEPTH) s->writer_pass[1] = -1;
    if (aspect_mask & RE5VK_TBDR_STENCIL) s->writer_pass[2] = -1;
}

void re5vk_tbdr_end_frame(re5vk_tbdr *t)
{
    if (!t || !t->enabled) return;
    for (uint32_t i = 0; i < t->n_cur; i++) {
        re5vk_tbdr_rec *r = &t->cur[i];
        re5vk_tbdr_hist *h = hist_find(t, r->pass_key, 1);
        if (!h) continue;
        h->color_live |= r->color_live | r->color_sticky_blocked;
        if (r->depth_live || r->depth_sticky_blocked) h->depth_live = 1;
        if (r->stencil_live || r->stencil_sticky_blocked) h->stencil_live = 1;
        if (h->obs_count < 255u) h->obs_count++;
    }
    t->n_cur = 0;
    t->frame_id++;
    if (t->frame_id == 0) {
        memset(t->map, 0, sizeof t->map);
        t->frame_id = 1;
    }
}
