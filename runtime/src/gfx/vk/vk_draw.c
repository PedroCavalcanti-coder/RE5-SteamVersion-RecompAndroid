/*
 * vk_draw.c - estado do D3D9, pipelines, descritores, passes e Draw/Clear/StretchRect do backend
 * re5vk (ver vk_core.h).
 *
 * Quase todo o estado fixo do D3D9 é dinâmico no Vulkan 1.3 (cull, profundidade, stencil, viewport,
 * stride, depth bias); no pipeline entram só os shaders (com a variante), a declaração de vértices,
 * o blend e os formatos dos alvos. A chave é por conteúdo (hash do bytecode e da declaração), nunca
 * por ponteiro: um endereço reaproveitado depois de um Release pegaria o pipeline errado.
 *
 * Viewport: meio pixel do D3D9 (centro do pixel nos inteiros) com x + 0,5 e y invertido pela
 * altura negativa; com isso a ordem dos vértices na tela é a mesma do D3D9 e o cull bate.
 */
#include <math.h>
#include <stdio.h>

#include "vk_core.h"

#define RS(d, s) ((d)->rs[(s)])

/* ---- utilidades ---- */

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    return h;
}

static float f_of(DWORD v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static int sampler_slot(DWORD stage)
{
    if (stage < 16) return (int)stage;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
        return 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}

static void set_obj(vk_obj **slot, vk_obj *o)
{
    if (*slot == o) return;
    re5vk_addref(o);
    vk_obj *old = *slot;
    *slot = o;
    re5vk_release(old);
}

/* ---- estado padrão ---- */

void re5vk_state_reset(vk_device *d)
{
    re5vk_end_rendering(d);
    for (int i = 0; i < 4; i++) set_obj((vk_obj **)&d->rt[i], NULL);
    set_obj((vk_obj **)&d->ds, NULL);
    set_obj((vk_obj **)&d->rt[0], &d->backbuffer->o);
    if (d->auto_depth) set_obj((vk_obj **)&d->ds, &d->auto_depth->o);
    memset(d->rs, 0, sizeof d->rs);
    d->rs[D3DRS_ZENABLE] = d->auto_depth ? D3DZB_TRUE : D3DZB_FALSE;
    d->rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    d->rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    d->rs[D3DRS_ZWRITEENABLE] = TRUE;
    d->rs[D3DRS_LASTPIXEL] = TRUE;
    d->rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    d->rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    d->rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    d->rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    d->rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    d->rs[D3DRS_STENCILFAIL] = d->rs[D3DRS_STENCILZFAIL] = d->rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    d->rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    d->rs[D3DRS_STENCILMASK] = d->rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    d->rs[D3DRS_CCW_STENCILFAIL] = d->rs[D3DRS_CCW_STENCILZFAIL] = d->rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    d->rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    d->rs[D3DRS_COLORWRITEENABLE] = d->rs[D3DRS_COLORWRITEENABLE1] = 0xF;
    d->rs[D3DRS_COLORWRITEENABLE2] = d->rs[D3DRS_COLORWRITEENABLE3] = 0xF;
    d->rs[D3DRS_BLENDOP] = d->rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    d->rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    d->rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    d->rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFFu;
    d->rs[D3DRS_POINTSIZE] = 0x3F800000u;
    for (int s = 0; s < VK_MAX_SAMPLERS; s++) {
        memset(d->ss[s], 0, sizeof d->ss[s]);
        d->ss[s][D3DSAMP_ADDRESSU] = d->ss[s][D3DSAMP_ADDRESSV] = d->ss[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        d->ss[s][D3DSAMP_MAGFILTER] = d->ss[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        d->ss[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        d->ss[s][D3DSAMP_MAXANISOTROPY] = 1;
        set_obj(&d->tex[s], NULL);
    }
    for (int i = 0; i < 16; i++) {
        set_obj((vk_obj **)&d->streams[i].vb, NULL);
        d->streams[i].off = d->streams[i].stride = 0;
    }
    set_obj((vk_obj **)&d->ib, NULL);
    set_obj((vk_obj **)&d->decl, NULL);
    set_obj((vk_obj **)&d->vs, NULL);
    set_obj((vk_obj **)&d->ps, NULL);
    memset(d->vsf, 0, sizeof d->vsf);
    memset(d->psf, 0, sizeof d->psf);
    memset(d->vsi, 0, sizeof d->vsi);
    memset(d->psi, 0, sizeof d->psi);
    d->vsb = d->psb = 0;
    vk_image *bb = d->backbuffer->img;
    d->vp = (D3DVIEWPORT9){0, 0, bb->w, bb->h, 0.0f, 1.0f};
    d->scissor = (RECT){0, 0, (LONG)bb->w, (LONG)bb->h};
    d->vs_dirty = d->ps_dirty = d->dyn_dirty = d->desc_dirty = 1;
    memset(d->smp_cache, 0, sizeof d->smp_cache);
    d->pending_clear.on = 0;
    d->pipe_gen++;
}

/* ---- passes de render ---- */

static int tex_is_depth(vk_obj *t);

/* RE5VK_LAZY_RT (padrão 1; =0 volta ao antigo): SetRenderTarget/SetDepthStencilSurface não fecham o
 * passe na hora; fecha no próximo draw/Clear só se o conjunto de alvos mudou de fato. O RE5 troca o
 * RT0 para outro alvo e volta sem desenhar no meio (A54, 20/11: ~54 passes por quadro reabertos no
 * mesmo alvo); cada reabertura num tiler grava e recarrega a tela inteira. */
static int lazy_rt(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_LAZY_RT");
        on = !(e && *e == '0');
    }
    return on;
}

/* RE5VK_PASS_TRACE=<n> (DIAGNOSTICO): loga cada abertura/fechamento de passe e troca de alvo de um
 * quadro a cada n (contado em Present), para ver a sequência que o jogo faz. */
static int pass_trace_on(const vk_device *d)
{
    static long want = -2;
    if (want == -2) {
        const char *e = getenv("RE5VK_PASS_TRACE");
        want = e ? atol(e) : -1;
    }
    return want > 0 && d->presents && d->presents % (uint64_t)want == 0;
}
#define PTRACE(d, ...) do { if (pass_trace_on(d)) RE5_INFO("re5vk", "PASS_TRACE " __VA_ARGS__); } while (0)

static int pass_matches(const vk_device *d)
{
    for (int i = 0; i < 4; i++)
        if (d->pass_rt[i] != d->rt[i]) return 0;
    return d->pass_ds == d->ds;
}

/* Passe aberto com alvos diferentes dos atuais: fecha agora (com o motivo da troca adiada). */
static void pass_sync(vk_device *d)
{
    if (!d->rendering || pass_matches(d)) return;
    re5vk_end_rendering_why(d, d->deferred_why ? d->deferred_why : "troca_de_alvo");
}

static void begin_rendering_with_layouts(vk_device *d, const VkImageLayout rt_layouts[4], VkImageLayout ds_layout)
{
    pass_sync(d);
    if (d->rendering) return;
    int gen = re5vk_layout_general();
    re5_vkn_pass pass;
    memset(&pass, 0, sizeof pass);
    VkImageView views[5];
    VkClearValue clears[5];
    uint32_t nviews = 0;
    uint32_t ncol = 0;
    VkExtent2D area = {0xFFFFFFFFu, 0xFFFFFFFFu};

    re5vk_tbdr_pass_in ti;
    memset(&ti, 0, sizeof ti);
    int clr_col = d->pending_clear.on && (d->pending_clear.flags & D3DCLEAR_TARGET);
    int clr_z   = d->pending_clear.on && (d->pending_clear.flags & D3DCLEAR_ZBUFFER);
    int clr_st  = d->pending_clear.on && (d->pending_clear.flags & D3DCLEAR_STENCIL);

    ti.is_split = d->pass_was_split ? 1u : 0u;
    d->pass_was_split = 0;

    for (uint32_t i = 0; i < 4; i++) {
        vk_surface *s = d->rt[i];
        d->render_rt_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!s) continue;
        ti.color_img[i] = s->img->id;
        ti.color_sub[i] = s->layer * s->img->levels + s->level;
        ti.color_subs[i] = &s->img->tbdr_subs[ti.color_sub[i]];
        ti.color_is_backbuffer[i] = (s == d->backbuffer) ? 1u : 0u;
        ti.color_is_texture[i] = (s->container != NULL || (s->img && (s->img->view != VK_NULL_HANDLE))) ? 1u : 0u;
        if (clr_col) ti.color_cleared |= (uint8_t)(1u << i);
        else ti.color_loaded |= (uint8_t)(1u << i);
    }
    vk_surface *ds = d->ds;
    d->render_ds_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ds) {
        ti.ds_img = ds->img->id;
        ti.ds_sub = ds->layer * ds->img->levels + ds->level;
        ti.ds_sub_state = &ds->img->tbdr_subs[ti.ds_sub];
        ti.has_depth = 1;
        ti.has_stencil = ds->img->fmt->stencil ? 1u : 0u;
        ti.depth_cleared = clr_z ? 1u : 0u;
        ti.depth_loaded = clr_z ? 0u : 1u;
        ti.stencil_cleared = clr_st ? 1u : 0u;
        ti.stencil_loaded = (ti.has_stencil && !clr_st) ? 1u : 0u;
        ti.ds_is_auto = (ds == d->auto_depth) ? 1u : 0u;
        ti.ds_is_texture = (ds->container != NULL || tex_is_depth(ds->container) ||
                            (ds->img && ds->img->view != VK_NULL_HANDLE && ds != d->auto_depth)) ? 1u : 0u;
        ti.stencil_enabled = (RS(d, D3DRS_STENCILENABLE) || clr_st) ? 1u : 0u;
        ti.depth_write_enabled = (RS(d, D3DRS_ZWRITEENABLE) || clr_z) ? 1u : 0u;
    }
    ti.width = ds ? ds->desc.Width : (d->rt[0] ? d->rt[0]->desc.Width : 0);
    ti.height = ds ? ds->desc.Height : (d->rt[0] ? d->rt[0]->desc.Height : 0);
    re5vk_tbdr_pass_out to;
    re5vk_tbdr_begin_pass(&d->tbdr, &ti, &to);

    /* VkRenderPass: o layout do anexo é o inicial e o final (as transições já foram feitas acima,
     * por sub-recurso); load/store por anexo como antes (CLEAR do Clear pendente, DONT_CARE só com
     * o oráculo TBDR ligado). Posições vazias (RT1 sem RT0) ficam VK_ATTACHMENT_UNUSED. */
    VkCommandBuffer cb = re5vk_cmd(d);
    for (uint32_t i = 0; i < 4; i++) {
        vk_surface *s = d->rt[i];
        if (!s) continue;
        VkImageLayout l = gen ? VK_IMAGE_LAYOUT_GENERAL : rt_layouts[i];
        re5vk_img_transition(d, cb, s->img, s->level, 1, s->layer, 1, l, 0);
        re5vk_img_mark_written(s->img, s->level, 1, s->layer, 1);
        d->render_rt_layouts[i] = l;
        re5_vkn_att *a = &pass.color[i];
        a->format = s->img->vkf;
        a->layout = l;
        a->load = clr_col ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a->store = (to.color_store_dont_care & (1u << i)) ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        a->stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a->stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        memset(&clears[nviews], 0, sizeof clears[nviews]);
        if (clr_col) memcpy(clears[nviews].color.float32, d->pending_clear.color[0], 16);
        views[nviews++] = re5vk_image_rt_view(d, s->img, s->layer, s->level);
        if (s->desc.Width < area.width) area.width = s->desc.Width;
        if (s->desc.Height < area.height) area.height = s->desc.Height;
        s->img->last_use = d->serial;
        ncol = i + 1;
    }
    pass.ncolor = ncol;
    if (ds) {
        VkImageLayout l = gen ? VK_IMAGE_LAYOUT_GENERAL : ds_layout;
        re5vk_img_transition(d, cb, ds->img, ds->level, 1, ds->layer, 1, l, 0);
        re5vk_img_mark_written(ds->img, ds->level, 1, ds->layer, 1);
        d->render_ds_layout = l;
        int st = ds->img->fmt->stencil != 0;
        pass.has_ds = 1;
        pass.ds.format = ds->img->vkf;
        pass.ds.layout = l;
        pass.ds.load = clr_z ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        pass.ds.store = to.depth_store_dont_care ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        pass.ds.stencil_load = (!st || !ti.stencil_enabled) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                               : clr_st ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        pass.ds.stencil_store = (!st || to.stencil_store_dont_care) ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                                                    : VK_ATTACHMENT_STORE_OP_STORE;
        memset(&clears[nviews], 0, sizeof clears[nviews]);
        clears[nviews].depthStencil.depth = d->pending_clear.z;
        clears[nviews].depthStencil.stencil = d->pending_clear.stencil;
        views[nviews++] = re5vk_image_rt_view(d, ds->img, ds->layer, ds->level);
        if (ds->desc.Width < area.width) area.width = ds->desc.Width;
        if (ds->desc.Height < area.height) area.height = ds->desc.Height;
        ds->img->last_use = d->serial;
    }
    if (area.width == 0xFFFFFFFFu) re5_fatal("re5vk", "draw sem render target nem profundidade");
    d->pending_clear.on = 0;
    VkRenderPass rp = re5_vkn_render_pass(&pass);
    VkRenderPassBeginInfo rbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp,
        .framebuffer = re5_vkn_framebuffer(&pass, rp, views, nviews, area),
        .renderArea = {{0, 0}, area},
        .clearValueCount = nviews,
        .pClearValues = clears,
    };
    re5vk_pt_begin(d, cb, area.width, area.height);
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    d->tm.passes++;
    PTRACE(d, "abre rt0 %p nivel %u (%ux%u) rt1 %p ds %p", d->rt[0] ? (void *)d->rt[0]->img : NULL,
           d->rt[0] ? d->rt[0]->level : 0u, area.width, area.height,
           d->rt[1] ? (void *)d->rt[1]->img : NULL, d->ds ? (void *)d->ds->img : NULL);
    for (int i = 0; i < 4; i++) set_obj((vk_obj **)&d->pass_rt[i], d->rt[i] ? &d->rt[i]->o : NULL);
    set_obj((vk_obj **)&d->pass_ds, d->ds ? &d->ds->o : NULL);
    d->deferred_why = NULL;
    {
        /* mesmo sub-recurso (imagem + nível + camada): cadeias de mip desenhadas nível a nível não contam */
        const void *rt0 = d->rt[0] ? (const void *)d->rt[0]->img : NULL, *ds = d->ds ? (const void *)d->ds->img : NULL;
        uint32_t sub0 = d->rt[0] ? d->rt[0]->layer << 16 | d->rt[0]->level : 0;
        if (rt0 == d->last_pass_rt0 && sub0 == d->last_pass_sub0 && ds == d->last_pass_ds) {
            d->tm.reopen++;
            uint32_t m = 0;
            for (int i = 1; i < 4; i++) m |= (d->rt[i] ? 1u : 0u) << i;
            if (m != d->last_pass_mrt) d->tm.reopen_mrt++;
        }
    }
    d->pass_draws = 0;
    d->rendering = 1;
    d->render_area = area;
}

static void begin_rendering(vk_device *d)
{
    int gen = re5vk_layout_general();
    VkImageLayout rt_l[4];
    for (int i = 0; i < 4; i++)
        rt_l[i] = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageLayout ds_l = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    begin_rendering_with_layouts(d, rt_l, ds_l);
}

/* Barreira de fim de passe: com layouts ótimos por sub-recurso (padrão), cada transição ou reuso
 * emite apenas a barreira exata naquela imagem, permitindo sobreposição entre passes independentes.
 * Com RE5VK_LAYOUT_GENERAL=1 ou RE5VK_BARRIER=all, mantém a barreira global de fim de passe. */
static int pass_end_barrier(VkCommandBuffer cb)
{
    static int all = -1;
    if (all < 0) {
        const char *e = getenv("RE5VK_BARRIER");
        all = e && !strcmp(e, "all");
        if (all) RE5_INFO("re5vk", "RE5VK_BARRIER=all: barreira total no fim de cada passe");
    }
    if (all) {
        re5vk_barrier_all(cb);
        return 1;
    }
    if (!re5vk_layout_general()) return 0;
    VkMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT
#if defined(__ANDROID__)
                             /* textura escrita num passe e lida no vertex shader (vsi->samplers) */
                             | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
#endif
                             ,
                         0, 1, &b, 0, NULL, 0, NULL);
    return 1;
}

static void sub_clean(vk_image *im, uint32_t layer, uint32_t level)
{
    if (!im || !im->sub_dirty) return;
    size_t i = (size_t)layer * im->levels + level;
    im->sub_dirty[i] = 0;
    im->sub_stages[i] = 0;
}

/* 1 se algum sub-recurso tem escrita pendente (alvo desenhado e ainda não protegido por barreira) */
static int img_any_dirty(const vk_image *im)
{
    if (!im->sub_dirty) return 0;
    for (uint32_t i = 0; i < im->layers * im->levels; i++)
        if (im->sub_dirty[i]) return 1;
    return 0;
}

static void pass_why_count(vk_device *d, const char *why)
{
    if (d->split_why) why = d->split_why;
    d->split_why = NULL;
    uint32_t i = 0;
    while (i < d->tm.pw_used && strcmp(d->tm.pw_name[i], why)) i++;
    if (i == d->tm.pw_used) {
        if (i == RE5VK_PW_MAX) i = RE5VK_PW_MAX - 1, why = "outros";   /* tabela cheia: junta no último */
        else d->tm.pw_name[d->tm.pw_used++] = why;
    }
    d->tm.pw_n[i]++;
    uint32_t n = d->pass_draws;
    d->tm.pd_hist[n == 0 ? 0 : n == 1 ? 1 : n < 5 ? 2 : n < 20 ? 3 : 4]++;
    d->last_pass_rt0 = d->pass_rt[0] ? (const void *)d->pass_rt[0]->img : NULL;
    d->last_pass_sub0 = d->pass_rt[0] ? d->pass_rt[0]->layer << 16 | d->pass_rt[0]->level : 0;
    d->last_pass_ds = d->pass_ds ? (const void *)d->pass_ds->img : NULL;
    d->last_pass_mrt = 0;
    for (int i = 1; i < 4; i++) d->last_pass_mrt |= (d->pass_rt[i] ? 1u : 0u) << i;
}

void re5vk_end_rendering_why(vk_device *d, const char *why)
{
    if (!d->rendering && d->pending_clear.on) begin_rendering(d);   /* um Clear sem draw: aplica */
    if (!d->rendering) { d->split_why = NULL; return; }
    PTRACE(d, "fecha (%u draws) por %s", d->pass_draws, d->split_why ? d->split_why : why);
    pass_why_count(d, why);
    vkCmdEndRenderPass(re5vk_cmd(d));
    re5vk_pt_end(d, re5vk_cmd(d), d->pass_draws);
    /* Com a barreira global (modo GENERAL), o que o passe escreveu já está visível para amostrar,
     * para o passe seguinte e para cópias: os anexos deixam de ter "escrita pendente". Sem isto, o
     * primeiro draw que amostrasse um alvo recém-desenhado dividia o passe aberto à toa (no celular,
     * cada divisão grava e recarrega os tiles inteiros), o que o re5vk até 10/11 não fazia. */
    if (pass_end_barrier(re5vk_cmd(d))) {
        for (int i = 0; i < 4; i++)
            if (d->pass_rt[i]) sub_clean(d->pass_rt[i]->img, d->pass_rt[i]->layer, d->pass_rt[i]->level);
        if (d->pass_ds) sub_clean(d->pass_ds->img, d->pass_ds->layer, d->pass_ds->level);
    }
    d->rendering = 0;
    d->deferred_why = NULL;
    for (int i = 0; i < 4; i++) set_obj((vk_obj **)&d->pass_rt[i], NULL);
    set_obj((vk_obj **)&d->pass_ds, NULL);
}

/* ---- render targets ---- */

static HRESULT STDMETHODCALLTYPE v_SetRenderTarget(IDirect3DDevice9 *self, DWORD i, IDirect3DSurface9 *surf)
{
    vk_device *d = (vk_device *)self;
    if (i >= 4 || (i == 0 && !surf)) return D3DERR_INVALIDCALL;
    vk_surface *s = (vk_surface *)surf;
    if (s && !(s->desc.Usage & D3DUSAGE_RENDERTARGET)) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    if (d->rt[i] != s) {
        static const char *const why[4][3] = {
            {"SetRT:rt0->nulo", "SetRT:rt0->outro", "SetRT:rt0->de_nulo"},
            {"SetRT:rt1->nulo", "SetRT:rt1->outro", "SetRT:rt1->de_nulo"},
            {"SetRT:rt2->nulo", "SetRT:rt2->outro", "SetRT:rt2->de_nulo"},
            {"SetRT:rt3->nulo", "SetRT:rt3->outro", "SetRT:rt3->de_nulo"}};
        const char *w = why[i][!s ? 0 : !d->rt[i] ? 2 : 1];
        PTRACE(d, "SetRT %lu surf %p img %p (aberto %d)", (unsigned long)i, (void *)s, s ? (void *)s->img : NULL, d->rendering);
        if (lazy_rt() && !d->pending_clear.on) {
            /* adiado: o passe aberto segue até o próximo draw/Clear; se a troca for desfeita antes, fica */
            if (d->rendering && !d->deferred_why) d->deferred_why = w;
        } else {
            if (d->rendering) d->split_why = w;
            re5vk_end_rendering(d);
        }
        set_obj((vk_obj **)&d->rt[i], s ? &s->o : NULL);
        if (d->rendering && pass_matches(d)) { d->deferred_why = NULL; d->tm.lazy_kept++; }
        d->pipe_gen++;
    }
    if (i == 0) {   /* D3D9: o viewport e o scissor voltam ao alvo inteiro */
        d->vp = (D3DVIEWPORT9){0, 0, s->desc.Width, s->desc.Height, 0.0f, 1.0f};
        d->scissor = (RECT){0, 0, (LONG)s->desc.Width, (LONG)s->desc.Height};
        d->dyn_dirty = 1;
    }
    re5vk_unlock(d);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_GetRenderTarget(IDirect3DDevice9 *self, DWORD i, IDirect3DSurface9 **out)
{
    vk_device *d = (vk_device *)self;
    if (i >= 4 || !out) return D3DERR_INVALIDCALL;
    if (!d->rt[i]) { *out = NULL; return D3DERR_NOTFOUND; }
    re5vk_addref(&d->rt[i]->o);
    *out = (IDirect3DSurface9 *)d->rt[i];
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_SetDepthStencilSurface(IDirect3DDevice9 *self, IDirect3DSurface9 *surf)
{
    vk_device *d = (vk_device *)self;
    vk_surface *s = (vk_surface *)surf;
    re5vk_lock(d);
    if (d->ds != s) {
        const char *w = !s ? "SetDS:nulo" : !d->ds ? "SetDS:de_nulo" : "SetDS:outro";
        PTRACE(d, "SetDS surf %p img %p (aberto %d)", (void *)s, s ? (void *)s->img : NULL, d->rendering);
        if (lazy_rt() && !d->pending_clear.on) {
            if (d->rendering && !d->deferred_why) d->deferred_why = w;
        } else {
            if (d->rendering) d->split_why = w;
            re5vk_end_rendering(d);
        }
        set_obj((vk_obj **)&d->ds, s ? &s->o : NULL);
        if (d->rendering && pass_matches(d)) { d->deferred_why = NULL; d->tm.lazy_kept++; }
        d->dyn_dirty = 1;
        d->pipe_gen++;
    }
    re5vk_unlock(d);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_GetDepthStencilSurface(IDirect3DDevice9 *self, IDirect3DSurface9 **out)
{
    vk_device *d = (vk_device *)self;
    if (!out) return D3DERR_INVALIDCALL;
    if (!d->ds) { *out = NULL; return D3DERR_NOTFOUND; }
    re5vk_addref(&d->ds->o);
    *out = (IDirect3DSurface9 *)d->ds;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_SetViewport(IDirect3DDevice9 *self, const D3DVIEWPORT9 *vp)
{
    vk_device *d = (vk_device *)self;
    if (!vp) return D3DERR_INVALIDCALL;
    d->vp = *vp;
    d->dyn_dirty = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetViewport(IDirect3DDevice9 *self, D3DVIEWPORT9 *vp)
{
    if (!vp) return D3DERR_INVALIDCALL;
    *vp = ((vk_device *)self)->vp;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetScissorRect(IDirect3DDevice9 *self, const RECT *r)
{
    vk_device *d = (vk_device *)self;
    if (!r) return D3DERR_INVALIDCALL;
    d->scissor = *r;
    d->dyn_dirty = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetScissorRect(IDirect3DDevice9 *self, RECT *r)
{
    if (!r) return D3DERR_INVALIDCALL;
    *r = ((vk_device *)self)->scissor;
    return D3D_OK;
}

/* ---- Clear ---- */

static void clamp_rect(VkRect2D *r, VkExtent2D area)
{
    int32_t x2 = r->offset.x + (int32_t)r->extent.width, y2 = r->offset.y + (int32_t)r->extent.height;
    if (r->offset.x < 0) r->offset.x = 0;
    if (r->offset.y < 0) r->offset.y = 0;
    if (x2 > (int32_t)area.width) x2 = (int32_t)area.width;
    if (y2 > (int32_t)area.height) y2 = (int32_t)area.height;
    r->extent.width = x2 > r->offset.x ? (uint32_t)(x2 - r->offset.x) : 0;
    r->extent.height = y2 > r->offset.y ? (uint32_t)(y2 - r->offset.y) : 0;
}

static VkExtent2D target_size(vk_device *d)
{
    if (d->rt[0]) return (VkExtent2D){d->rt[0]->desc.Width, d->rt[0]->desc.Height};
    if (d->ds) return (VkExtent2D){d->ds->desc.Width, d->ds->desc.Height};
    return (VkExtent2D){0, 0};
}

/* RE5VK_SKIP_SMALL_PASSES (DIAGNÓSTICO, imagem errada): não desenha nem limpa alvos pequenos (cadeias
 * de bloom/mip, ~80 dos ~100 passes por quadro no A54), então esses passes nem abrem. =1: alvos com
 * até 1/16 dos pixels do back buffer; =<n> > 1: até n pixels. Mede o teto de fps sem esses passes. */
static int small_target(const vk_device *d)
{
    static long lim = -1;
    if (lim < 0) {
        const char *e = getenv("RE5VK_SKIP_SMALL_PASSES");
        lim = e ? atol(e) : 0;
        if (lim < 0) lim = 0;
        if (lim) RE5_WARN("re5vk", "RE5VK_SKIP_SMALL_PASSES=%ld: alvos pequenos não são desenhados (diagnóstico)", lim);
    }
    if (!lim || !d->rt[0]) return 0;
    uint64_t px = (uint64_t)d->rt[0]->desc.Width * d->rt[0]->desc.Height;
    uint64_t max = lim == 1 ? (uint64_t)d->pp.BackBufferWidth * d->pp.BackBufferHeight / 16u : (uint64_t)lim;
    return px <= max;
}

static HRESULT STDMETHODCALLTYPE v_Clear(IDirect3DDevice9 *self, DWORD count, const D3DRECT *rects, DWORD flags,
                                         D3DCOLOR color, float z, DWORD stencil)
{
    vk_device *d = (vk_device *)self;
    if (small_target(d)) return D3D_OK;
    if (!d->ds) flags &= ~(DWORD)(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (d->ds && !d->ds->img->fmt->stencil) flags &= ~(DWORD)D3DCLEAR_STENCIL;
    if (!flags) return D3D_OK;
    re5vk_lock(d);
    pass_sync(d);   /* troca de alvo adiada: o Clear é do conjunto atual */
    float c[4] = {(float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f,
                  (float)(color & 0xFF) / 255.0f, (float)(color >> 24) / 255.0f};
    VkExtent2D full = target_size(d);
    /* área: viewport, com o scissor se ligado, e os retângulos */
    VkRect2D base = {{(int32_t)d->vp.X, (int32_t)d->vp.Y}, {d->vp.Width, d->vp.Height}};
    clamp_rect(&base, full);
    if (RS(d, D3DRS_SCISSORTESTENABLE)) {
        VkRect2D s = {{d->scissor.left, d->scissor.top},
                      {(uint32_t)(d->scissor.right - d->scissor.left), (uint32_t)(d->scissor.bottom - d->scissor.top)}};
        int32_t x1 = s.offset.x > base.offset.x ? s.offset.x : base.offset.x;
        int32_t y1 = s.offset.y > base.offset.y ? s.offset.y : base.offset.y;
        int32_t x2 = s.offset.x + (int32_t)s.extent.width, y2 = s.offset.y + (int32_t)s.extent.height;
        int32_t bx2 = base.offset.x + (int32_t)base.extent.width, by2 = base.offset.y + (int32_t)base.extent.height;
        if (bx2 < x2) x2 = bx2;
        if (by2 < y2) y2 = by2;
        base = (VkRect2D){{x1, y1}, {x2 > x1 ? (uint32_t)(x2 - x1) : 0, y2 > y1 ? (uint32_t)(y2 - y1) : 0}};
    }
    int whole = !count && base.offset.x == 0 && base.offset.y == 0 && base.extent.width == full.width &&
                base.extent.height == full.height;
    /* todos os anexos do tamanho do alvo (senão o loadOp limparia além do viewport) */
    for (int i = 0; i < 4 && whole; i++)
        if (d->rt[i] && (d->rt[i]->desc.Width != full.width || d->rt[i]->desc.Height != full.height)) whole = 0;
    if (whole && d->ds && (d->ds->desc.Width != full.width || d->ds->desc.Height != full.height)) whole = 0;
#if defined(__ANDROID__)
    /* Clear(TARGET) seguido de Clear(ZBUFFER) antes do draw: junta os dois no mesmo loadOp CLEAR
     * (sem isso o segundo abria o passe e virava vkCmdClearAttachments). O Clear mais novo vence
     * nos aspectos que ele limpa, como no D3D9. */
    if (whole && !d->rendering && d->pending_clear.on && d->pending_clear.flags != flags) {
        if (flags & D3DCLEAR_TARGET)
            for (int i = 0; i < 4; i++) memcpy(d->pending_clear.color[i], c, sizeof c);
        if (flags & D3DCLEAR_ZBUFFER) d->pending_clear.z = z;
        if (flags & D3DCLEAR_STENCIL) d->pending_clear.stencil = stencil;
        d->pending_clear.flags |= flags;
        re5vk_unlock(d);
        return D3D_OK;
    }
#endif
    if (whole && !d->rendering && (!d->pending_clear.on || d->pending_clear.flags == flags)) {
        /* começo do passe: loadOp CLEAR, sem carregar os tiles (o que o Mali gosta) */
        d->pending_clear.on = 1;
        d->pending_clear.flags = flags;
        for (int i = 0; i < 4; i++) memcpy(d->pending_clear.color[i], c, sizeof c);
        d->pending_clear.z = z;
        d->pending_clear.stencil = stencil;
        re5vk_unlock(d);
        return D3D_OK;
    }
    begin_rendering(d);
    VkClearAttachment at[5];
    uint32_t na = 0;
    if (flags & D3DCLEAR_TARGET)
        for (uint32_t i = 0; i < 4; i++)
            if (d->rt[i]) {
                at[na] = (VkClearAttachment){VK_IMAGE_ASPECT_COLOR_BIT, i, {.color = {.float32 = {c[0], c[1], c[2], c[3]}}}};
                na++;
            }
    if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        VkImageAspectFlags a = 0;
        if (flags & D3DCLEAR_ZBUFFER) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & D3DCLEAR_STENCIL) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        at[na++] = (VkClearAttachment){a, 0, {.depthStencil = {z, stencil}}};
    }
    VkClearRect cr[64];
    uint32_t nr = 0;
    if (!count) {
        cr[nr++] = (VkClearRect){base, 0, 1};
    } else {
        for (DWORD k = 0; k < count && nr < 64; k++) {
            VkRect2D r = {{rects[k].x1, rects[k].y1},
                          {(uint32_t)(rects[k].x2 - rects[k].x1), (uint32_t)(rects[k].y2 - rects[k].y1)}};
            clamp_rect(&r, d->render_area);
            if (r.extent.width && r.extent.height) cr[nr++] = (VkClearRect){r, 0, 1};
        }
    }
    for (uint32_t k = 0; k < nr; k++) clamp_rect(&cr[k].rect, d->render_area);
    if (na && nr) vkCmdClearAttachments(re5vk_cmd(d), na, at, nr, cr);
    re5vk_unlock(d);
    return D3D_OK;
}

/* ---- estados ---- */

/* RE5VK_RS_LOG=1 (diagnóstico): a primeira vez de cada valor dos estados de recorte/transparência
 * (alpha test, blend, e POINTSIZE/ADAPTIVETESS_Y, onde ficam os truques A2M1/ATOC de alpha to
 * coverage). Para comparar o que o jogo faz no PC e no Mali. */
static int rs_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_RS_LOG");
        on = e && *e == '1';
    }
    return on;
}

/* Com RE5VK_RS_LOG=1: no primeiro draw com alpha test de cada PS, as constantes c0..c7 que o jogo
 * mandou (calculadas pela CPU do guest: no ARM vêm do código recompilado com sse2neon). */
static void ps_const_log(const vk_device *d, uint32_t ps_hash, int func)
{
    static uint32_t seen[128];
    static int nseen;
    if (!rs_log_on()) return;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == ps_hash) return;
    if (nseen >= 128) return;
    seen[nseen++] = ps_hash;
    char buf[640];
    int n = snprintf(buf, sizeof buf, "RS_LOG PS %08X alpha func %d ref %u:", (unsigned)ps_hash, func,
                     (unsigned)(d->rs[D3DRS_ALPHAREF] & 0xFF));
    for (int r = 0; r < 8 && n < (int)sizeof buf - 64; r++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, " c%d(%g %g %g %g)", r, d->psf[r][0], d->psf[r][1],
                      d->psf[r][2], d->psf[r][3]);
    RE5_INFO("re5vk", "%s", buf);
}

static void rs_log(D3DRENDERSTATETYPE s, DWORD v)
{
    static uint32_t seen[512][2];
    static int nseen;
    if (!rs_log_on()) return;
    switch ((uint32_t)s) {
    case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAREF: case D3DRS_ALPHAFUNC: case D3DRS_ALPHABLENDENABLE:
    case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_POINTSIZE: case D3DRS_ADAPTIVETESS_X:
    case D3DRS_ADAPTIVETESS_Y: case D3DRS_MULTISAMPLEANTIALIAS: case D3DRS_SRGBWRITEENABLE:
    case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_COLORWRITEENABLE:
        break;
    default:
        return;
    }
    for (int i = 0; i < nseen; i++)
        if (seen[i][0] == (uint32_t)s && seen[i][1] == v) return;
    if (nseen < 512) {
        seen[nseen][0] = (uint32_t)s;
        seen[nseen][1] = v;
        nseen++;
    }
    RE5_INFO("re5vk", "RS_LOG estado %u = 0x%08X ('%c%c%c%c')", (unsigned)s, (unsigned)v, (int)(v & 0x7F) >= 32 ? (int)(v & 0xFF) : '.',
             (int)((v >> 8) & 0x7F) >= 32 ? (int)((v >> 8) & 0xFF) : '.', (int)((v >> 16) & 0x7F) >= 32 ? (int)((v >> 16) & 0xFF) : '.',
             (int)((v >> 24) & 0x7F) >= 32 ? (int)((v >> 24) & 0xFF) : '.');
}

static HRESULT STDMETHODCALLTYPE v_SetRenderState(IDirect3DDevice9 *self, D3DRENDERSTATETYPE s, DWORD v)
{
    vk_device *d = (vk_device *)self;
    if ((uint32_t)s >= 256) return D3DERR_INVALIDCALL;
    rs_log(s, v);
    if (d->rs[s] == v) return D3D_OK;
    d->rs[s] = v;
    /* Só os estados que entram na chave do pipeline ou na variante do PS (prepare) refazem a
     * busca; Z, stencil, bias, cull, scissor e blend factor são estado dinâmico (set_dynamic), e
     * ALPHAREF vai no UBO. Antes todo SetRenderState com valor novo subia pipe_gen, e o draw
     * seguinte recalculava variantes, chave e hash da declaração. */
    switch (s) {
    case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAFUNC:
        d->pipe_gen++;
        d->ps_dirty = 1;
        break;
    case D3DRS_FILLMODE: case D3DRS_ALPHABLENDENABLE:
    case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP: case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA: case D3DRS_COLORWRITEENABLE:
    case D3DRS_COLORWRITEENABLE1: case D3DRS_COLORWRITEENABLE2: case D3DRS_COLORWRITEENABLE3:
        d->pipe_gen++;
        break;
    case D3DRS_ALPHAREF:
        d->ps_dirty = 1;
        break;
    case D3DRS_CULLMODE: case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC: case D3DRS_STENCILENABLE:
    case D3DRS_STENCILFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILZFAIL: case D3DRS_STENCILFUNC:
    case D3DRS_TWOSIDEDSTENCILMODE: case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILPASS:
    case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILFUNC: case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
        if (d->core) d->pipe_gen++;   /* modo Vulkan 1.1: vão na chave do pipeline */
        d->dyn_dirty = 1;
        break;
    default:
        d->dyn_dirty = 1;
        break;
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetRenderState(IDirect3DDevice9 *self, D3DRENDERSTATETYPE s, DWORD *v)
{
    if ((uint32_t)s >= 256 || !v) return D3DERR_INVALIDCALL;
    *v = ((vk_device *)self)->rs[s];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetSamplerState(IDirect3DDevice9 *self, DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v)
{
    vk_device *d = (vk_device *)self;
    int s = sampler_slot(stage);
    if (s < 0 || (uint32_t)t >= 14) return D3DERR_INVALIDCALL;
    if (d->ss[s][t] != v) {
        d->ss[s][t] = v;
        d->desc_dirty = 1;
        memset(d->smp_cache[s], 0, sizeof d->smp_cache[s]);
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetSamplerState(IDirect3DDevice9 *self, DWORD stage, D3DSAMPLERSTATETYPE t, DWORD *v)
{
    int s = sampler_slot(stage);
    if (s < 0 || (uint32_t)t >= 14 || !v) return D3DERR_INVALIDCALL;
    *v = ((vk_device *)self)->ss[s][t];
    return D3D_OK;
}
static int tex_is_depth(vk_obj *t);

static HRESULT STDMETHODCALLTYPE v_SetTexture(IDirect3DDevice9 *self, DWORD stage, IDirect3DBaseTexture9 *t)
{
    vk_device *d = (vk_device *)self;
    int s = sampler_slot(stage);
    if (s < 0) return D3DERR_INVALIDCALL;
    if (d->tex[s] != (vk_obj *)t) {
        if (tex_is_depth(d->tex[s]) != tex_is_depth((vk_obj *)t)) d->pipe_gen++;   /* variante do PS */
        re5vk_lock(d);
        set_obj(&d->tex[s], (vk_obj *)t);
        re5vk_unlock(d);
        d->desc_dirty = 1;
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetTexture(IDirect3DDevice9 *self, DWORD stage, IDirect3DBaseTexture9 **out)
{
    vk_device *d = (vk_device *)self;
    int s = sampler_slot(stage);
    if (s < 0 || !out) return D3DERR_INVALIDCALL;
    re5vk_addref(d->tex[s]);
    *out = (IDirect3DBaseTexture9 *)d->tex[s];
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetStreamSource(IDirect3DDevice9 *self, UINT i, IDirect3DVertexBuffer9 *vb, UINT off,
                                                   UINT stride)
{
    vk_device *d = (vk_device *)self;
    if (i >= 16) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    set_obj((vk_obj **)&d->streams[i].vb, (vk_obj *)vb);
    if (d->streams[i].stride != stride) d->pipe_gen++;   /* o stride entra na chave do pipeline */
    d->streams[i].off = off;
    d->streams[i].stride = stride;
    re5vk_unlock(d);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetStreamSource(IDirect3DDevice9 *self, UINT i, IDirect3DVertexBuffer9 **vb, UINT *off,
                                                   UINT *stride)
{
    vk_device *d = (vk_device *)self;
    if (i >= 16 || !vb || !off || !stride) return D3DERR_INVALIDCALL;
    re5vk_addref(&d->streams[i].vb->o);
    *vb = (IDirect3DVertexBuffer9 *)d->streams[i].vb;
    *off = d->streams[i].off;
    *stride = d->streams[i].stride;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetIndices(IDirect3DDevice9 *self, IDirect3DIndexBuffer9 *ib)
{
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    set_obj((vk_obj **)&d->ib, (vk_obj *)ib);
    re5vk_unlock(d);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetVertexDeclaration(IDirect3DDevice9 *self, IDirect3DVertexDeclaration9 *dl)
{
    vk_device *d = (vk_device *)self;
    re5vk_lock(d);
    if (d->decl != (vk_decl *)dl) d->pipe_gen++;
    set_obj((vk_obj **)&d->decl, (vk_obj *)dl);
    re5vk_unlock(d);
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetVertexDeclaration(IDirect3DDevice9 *self, IDirect3DVertexDeclaration9 **out)
{
    vk_device *d = (vk_device *)self;
    if (!out) return D3DERR_INVALIDCALL;
    re5vk_addref(&d->decl->o);
    *out = (IDirect3DVertexDeclaration9 *)d->decl;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetVertexShader(IDirect3DDevice9 *self, IDirect3DVertexShader9 *s)
{
    vk_device *d = (vk_device *)self;
    if (d->vs != (vk_shader *)s) {
        re5vk_lock(d);
        set_obj((vk_obj **)&d->vs, (vk_obj *)s);
        re5vk_unlock(d);
        d->desc_dirty = 1;
        d->pipe_gen++;
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetPixelShader(IDirect3DDevice9 *self, IDirect3DPixelShader9 *s)
{
    vk_device *d = (vk_device *)self;
    if (d->ps != (vk_shader *)s) {
        re5vk_lock(d);
        set_obj((vk_obj **)&d->ps, (vk_obj *)s);
        re5vk_unlock(d);
        d->desc_dirty = 1;
        d->pipe_gen++;
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetVertexShader(IDirect3DDevice9 *self, IDirect3DVertexShader9 **out)
{
    vk_device *d = (vk_device *)self;
    if (!out) return D3DERR_INVALIDCALL;
    re5vk_addref(&d->vs->o);
    *out = (IDirect3DVertexShader9 *)d->vs;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_GetPixelShader(IDirect3DDevice9 *self, IDirect3DPixelShader9 **out)
{
    vk_device *d = (vk_device *)self;
    if (!out) return D3DERR_INVALIDCALL;
    re5vk_addref(&d->ps->o);
    *out = (IDirect3DPixelShader9 *)d->ps;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_SetVSConstF(IDirect3DDevice9 *self, UINT start, const float *v, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!v || start + n > 256) return D3DERR_INVALIDCALL;
    memcpy(d->vsf[start], v, n * 16u);
    d->vs_dirty = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetPSConstF(IDirect3DDevice9 *self, UINT start, const float *v, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!v || start + n > 224) return D3DERR_INVALIDCALL;
    memcpy(d->psf[start], v, n * 16u);
    d->ps_dirty = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetVSConstI(IDirect3DDevice9 *self, UINT start, const int *v, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!v || start + n > 16) return D3DERR_INVALIDCALL;
    memcpy(d->vsi[start], v, n * 16u);
    d->vs_dirty = 1;
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetPSConstI(IDirect3DDevice9 *self, UINT start, const int *v, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!v || start + n > 16) return D3DERR_INVALIDCALL;
    memcpy(d->psi[start], v, n * 16u);
    d->ps_dirty = 1;
    return D3D_OK;
}
static HRESULT set_bools(uint32_t *bits, UINT start, const BOOL *v, UINT n)
{
    if (!v || start + n > 16) return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < n; i++) {
        if (v[i]) *bits |= 1u << (start + i);
        else *bits &= ~(1u << (start + i));
    }
    return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE v_SetVSConstB(IDirect3DDevice9 *self, UINT start, const BOOL *v, UINT n)
{
    vk_device *d = (vk_device *)self;
    d->vs_dirty = 1;
    return set_bools(&d->vsb, start, v, n);
}
static HRESULT STDMETHODCALLTYPE v_SetPSConstB(IDirect3DDevice9 *self, UINT start, const BOOL *v, UINT n)
{
    vk_device *d = (vk_device *)self;
    d->ps_dirty = 1;
    return set_bools(&d->psb, start, v, n);
}
static HRESULT STDMETHODCALLTYPE v_SetClipPlane(IDirect3DDevice9 *self, DWORD i, const float *plane)
{
    /* Guardado e não aplicado (sem gl_ClipDistance, como a DXVK no Mali); avisado no draw. */
    return i < 6 && plane ? D3D_OK : D3DERR_INVALIDCALL;
}

/* ---- samplers ---- */

static VkFilter filter_of(DWORD f) { return f == D3DTEXF_POINT || f == D3DTEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

static VkSamplerAddressMode address_of(DWORD a)
{
    switch (a) {
    case D3DTADDRESS_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case D3DTADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3DTADDRESS_MIRRORONCE: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/* O formato aceita filtro linear nesta GPU? No Mali os float de 32 bits (R32F, A32B32G32R32F: texturas
 * de vértice da animação facial, mapas de sombra) não aceitam, e amostrar com LINEAR é indefinido: o
 * Mali devolvia lixo (listras pretas saindo da cabeça dos personagens, luz e sombra erradas). O Intel
 * filtra float de 32 bits, por isso o PC não mostrava. Como a DXVK: cai para NEAREST. */
static int format_linear(vk_device *d, VkFormat f)
{
    static int8_t memo[256];   /* 0 = não consultado, 1 = sim, 2 = não */
    if ((uint32_t)f < 256 && memo[f]) return memo[f] == 1;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    int ok = !!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if ((uint32_t)f < 256) {
        memo[f] = ok ? 1 : 2;
        if (!ok) RE5_INFO("re5vk", "formato Vulkan %d sem filtro linear nesta GPU: amostrado com NEAREST", (int)f);
    }
    return ok;
}

/* RE5VK_VTX_NEAREST=0 desliga. Textura de vertice (animacao facial: texldl nos VS de skinning) sempre
 * com NEAREST: o D3D9 em hardware so garante amostragem por ponto em textura de vertice, e o filtro
 * linear em float16/32 no estagio de vertice do Mali e a suspeita da linha atravessando a cabeca.
 * CONTORNO ate o teste no celular. */
static int vtx_nearest(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_VTX_NEAREST");
        on = !(e && *e == '0');
    }
    return on;
}

static int static_stride_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_STATIC_STRIDE");
        on = !(e && *e == '0');
    }
    return on;
}

static int stage_note_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_STAGE_NOTE");
        on = !(e && *e == '0');
    }
    return on;
}

static VkSampler sampler_get(vk_device *d, int s, int compare, int multimip, int nolin)
{
    uint32_t key[16];
    memcpy(key, d->ss[s], 13 * 4);
    key[13] = (uint32_t)compare;
    key[14] = (uint32_t)multimip;
    key[15] = (uint32_t)nolin;
    for (uint32_t i = 0; i < d->nsamplers; i++)
        if (!memcmp(d->samplers[i].key, key, sizeof key)) return d->samplers[i].s;
    const DWORD *st = d->ss[s];
    int aniso = st[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || st[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC;
    float maxa = (float)st[D3DSAMP_MAXANISOTROPY];
    if (maxa > d->props.limits.maxSamplerAnisotropy) maxa = d->props.limits.maxSamplerAnisotropy;
    DWORD bc = st[D3DSAMP_BORDERCOLOR];
    VkBorderColor border = bc == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : (bc & 0xFFFFFF) == 0 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    float minlod = (float)st[D3DSAMP_MAXMIPLEVEL];
    VkSamplerCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = nolin ? VK_FILTER_NEAREST : filter_of(st[D3DSAMP_MAGFILTER]),
        .minFilter = nolin ? VK_FILTER_NEAREST : filter_of(st[D3DSAMP_MINFILTER]),
        .mipmapMode = (!multimip || nolin) ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                     : (st[D3DSAMP_MIPFILTER] == D3DTEXF_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                : VK_SAMPLER_MIPMAP_MODE_NEAREST),
        .addressModeU = address_of(st[D3DSAMP_ADDRESSU]),
        .addressModeV = address_of(st[D3DSAMP_ADDRESSV]),
        .addressModeW = address_of(st[D3DSAMP_ADDRESSW]),
        .mipLodBias = !multimip ? 0.0f : f_of(st[D3DSAMP_MIPMAPLODBIAS]),
        .anisotropyEnable = multimip && aniso && maxa > 1.0f && !nolin,
        .maxAnisotropy = maxa < 1.0f ? 1.0f : maxa,
        .compareEnable = compare ? VK_TRUE : VK_FALSE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .minLod = !multimip ? 0.0f : minlod,
        .maxLod = !multimip ? 1.0f : (st[D3DSAMP_MIPFILTER] == D3DTEXF_NONE ? minlod + 0.25f : VK_LOD_CLAMP_NONE),
        .borderColor = border,
    };
    VkSampler smp;
    VK_CHECK(vkCreateSampler(d->dev, &ci, NULL, &smp));
    if (d->nsamplers == d->capsamplers) {
        d->capsamplers = d->capsamplers ? d->capsamplers * 2 : 64;
        d->samplers = realloc(d->samplers, d->capsamplers * sizeof *d->samplers);
        if (!d->samplers) re5_fatal("re5vk", "sem memória");
    }
    memcpy(d->samplers[d->nsamplers].key, key, sizeof key);
    d->samplers[d->nsamplers++].s = smp;
    return smp;
}

static VkSampler sampler_cached(vk_device *d, int s, int compare, int multimip, int nolin)
{
    VkSampler *p = &d->smp_cache[s][compare != 0][multimip != 0][nolin != 0];
    if (!*p) *p = sampler_get(d, s, compare, multimip, nolin);
    return *p;
}

/* ---- cache de descriptor sets por envio ---- */

#define SET_CACHE_CAP 4096u   /* potência de 2; metade cheia = para de guardar até o próximo envio */

static uint64_t set_key_hash(const uint64_t *k, uint32_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ k[i]) * 0x100000001b3ull;
    return h ^ (h >> 29);
}

/* Devolve o slot da chave (achado ou vago); NULL sem tabela. */
static struct vk_set_cache_entry *set_cache_slot(vk_device *d, const uint64_t *k, uint32_t n, uint64_t h, int *found)
{
    *found = 0;
    if (!d->set_cache) {
        d->set_cache = calloc(SET_CACHE_CAP, sizeof *d->set_cache);
        if (!d->set_cache) return NULL;
        d->set_cache_cap = SET_CACHE_CAP;
    }
    uint32_t m = d->set_cache_cap - 1;
    for (uint32_t i = (uint32_t)h & m;; i = (i + 1) & m) {
        struct vk_set_cache_entry *e = &d->set_cache[i];
        if (e->gen != d->set_cache_gen) return e;
        if (e->hash == h && e->nkey == n && !memcmp(e->key, k, n * sizeof k[0])) {
            *found = 1;
            return e;
        }
    }
}

/* ---- pipelines ---- */

static VkFormat vertex_format(BYTE type);

/* Formato do atributo para a GPU. Os *SCALED (inteiro lido como float) são opcionais no Vulkan e
 * o Adreno não os tem: nesse caso o atributo vai como UINT/SINT (ou R32_UINT para UDEC3) e o VS
 * converte (variante in_uint/in_sint/in_udec3). *intk: 0 float, 1 uint, 2 sint, 3 udec3 packed. */
static VkFormat vertex_format_dev(vk_device *d, BYTE type, int *intk)
{
    *intk = 0;
    VkFormat vf = vertex_format(type);
    if (vf == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;

    static struct {
        VkFormat fmt;
        int intk;
        int8_t status; /* 0: não consultado, 1: resolvido */
    } dev_cache[32];

    if (type < 32 && dev_cache[type].status == 1) {
        *intk = dev_cache[type].intk;
        return dev_cache[type].fmt;
    }

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(d->phys, vf, &fp);
    RE5_INFO("re5vk", "D3DDECLTYPE %u -> formato Vulkan %d: %s", (unsigned)type, (int)vf,
             (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "suportado" : "SEM suporte");
    if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
        if (type < 32) {
            dev_cache[type].fmt = vf;
            dev_cache[type].intk = 0;
            dev_cache[type].status = 1;
        }
        return vf;
    }

    VkFormat chosen = VK_FORMAT_UNDEFINED;
    int chosen_intk = 0;

    switch (type) {
    case D3DDECLTYPE_UBYTE4:
        vkGetPhysicalDeviceFormatProperties(d->phys, VK_FORMAT_R8G8B8A8_UINT, &fp);
        if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
            chosen = VK_FORMAT_R8G8B8A8_UINT;
            chosen_intk = 1;
        }
        break;

    case D3DDECLTYPE_SHORT2:
        vkGetPhysicalDeviceFormatProperties(d->phys, VK_FORMAT_R16G16_SINT, &fp);
        if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
            chosen = VK_FORMAT_R16G16_SINT;
            chosen_intk = 2;
        }
        break;

    case D3DDECLTYPE_SHORT4:
        vkGetPhysicalDeviceFormatProperties(d->phys, VK_FORMAT_R16G16B16A16_SINT, &fp);
        if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
            chosen = VK_FORMAT_R16G16B16A16_SINT;
            chosen_intk = 2;
        }
        break;

    case D3DDECLTYPE_UDEC3:
        /* Tenta UINT_PACK32 primeiro */
        vkGetPhysicalDeviceFormatProperties(d->phys, VK_FORMAT_A2B10G10R10_UINT_PACK32, &fp);
        if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
            chosen = VK_FORMAT_A2B10G10R10_UINT_PACK32;
            chosen_intk = 1;
        } else {
            /* Fallback R32_UINT: mandatório em Vulkan, mesmo tamanho (4 bytes), desempacota 10-10-10-2 no VS */
            vkGetPhysicalDeviceFormatProperties(d->phys, VK_FORMAT_R32_UINT, &fp);
            if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
                chosen = VK_FORMAT_R32_UINT;
                chosen_intk = 3;
            }
        }
        break;

    default:
        break;
    }

    if (chosen != VK_FORMAT_UNDEFINED) {
        RE5_INFO("re5vk", "D3DDECLTYPE %u: sem suporte a %d, usando fallback %d (intk=%d)",
                 (unsigned)type, (int)vf, (int)chosen, chosen_intk);
        if (type < 32) {
            dev_cache[type].fmt = chosen;
            dev_cache[type].intk = chosen_intk;
            dev_cache[type].status = 1;
        }
        *intk = chosen_intk;
        return chosen;
    }

    /* Sem suporte e sem fallback: o atributo chegaria com lixo (comportamento indefinido no Vulkan). */
    RE5_ERROR("re5vk", "D3DDECLTYPE %u: formato Vulkan %d sem suporte a vertex buffer nesta GPU e sem fallback",
              (unsigned)type, (int)vf);
    if (type < 32) {
        dev_cache[type].fmt = vf;
        dev_cache[type].intk = 0;
        dev_cache[type].status = 1;
    }
    return vf;
}

/* Elemento da declaração que alimenta o v<r> do VS (pela semântica). */
static const D3DVERTEXELEMENT9 *decl_elem(vk_device *d, const re5vk_shader *vsi, uint32_t r)
{
    for (uint32_t i = 0; d->decl && i < d->decl->n; i++)
        if (d->decl->el[i].Usage == vsi->in_usage[r] && d->decl->el[i].UsageIndex == vsi->in_index[r])
            return &d->decl->el[i];
    return NULL;
}

static VkFormat vertex_format(BYTE type)
{
    switch (type) {
    case D3DDECLTYPE_FLOAT1: return VK_FORMAT_R32_SFLOAT;
    case D3DDECLTYPE_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
    case D3DDECLTYPE_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
    case D3DDECLTYPE_FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case D3DDECLTYPE_D3DCOLOR: return VK_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4: return VK_FORMAT_R8G8B8A8_USCALED;
    case D3DDECLTYPE_SHORT2: return VK_FORMAT_R16G16_SSCALED;
    case D3DDECLTYPE_SHORT4: return VK_FORMAT_R16G16B16A16_SSCALED;
    case D3DDECLTYPE_UBYTE4N: return VK_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N: return VK_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N: return VK_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N: return VK_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N: return VK_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_UDEC3: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case D3DDECLTYPE_DEC3N: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case D3DDECLTYPE_FLOAT16_2: return VK_FORMAT_R16G16_SFLOAT;
    case D3DDECLTYPE_FLOAT16_4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkBlendFactor blend_of(DWORD b, int alpha)
{
    switch (b) {
    case D3DBLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3DBLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3DBLEND_SRCCOLOR: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: case D3DBLEND_BOTHSRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: case D3DBLEND_BOTHINVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3DBLEND_BLENDFACTOR: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3DBLEND_INVBLENDFACTOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
                                               : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: re5_fatal("re5vk", "D3DBLEND %u sem tradução", (unsigned)b); return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blendop_of(DWORD o)
{
    switch (o) {
    case D3DBLENDOP_ADD: return VK_BLEND_OP_ADD;
    case D3DBLENDOP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3DBLENDOP_REVSUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3DBLENDOP_MIN: return VK_BLEND_OP_MIN;
    case D3DBLENDOP_MAX: return VK_BLEND_OP_MAX;
    default: re5_fatal("re5vk", "D3DBLENDOP %u sem tradução", (unsigned)o); return VK_BLEND_OP_ADD;
    }
}

typedef struct {
    uint32_t vs_hash, vs_len, ps_hash, ps_len;
    re5vk_dxso_variant vsv, psv;
    uint32_t decl_hash;
    uint32_t stride[16];        /* stride de cada stream usado pela declaracao (vai estatico no pipeline) */
    uint32_t topo_class;        /* 0 pontos, 1 linhas, 2 triângulos */
    uint32_t fill;
    uint32_t blend[6];          /* enable, src, dst, op, srca, dsta/opa empacotados */
    uint32_t write[4];
    VkFormat color[4], depth;
    uint8_t stencil;
    /* modo Vulkan 1.1 (d->core): estado que o EDS deixaria dinâmico; zero fora dele */
    uint8_t c_cull, c_ztest, c_zwrite, c_zop, c_stencil, c_two, c_bias, c_pad;
    uint32_t c_sops;            /* frente: fail|pass<<4|zfail<<8|func<<12; trás: o mesmo <<16 */
} pipe_key;

/* Estado de rasterização/profundidade do D3D9 no formato do Vulkan (o mesmo do set_dynamic). */
static VkStencilOp stencil_op(DWORD o);
static VkCompareOp cmp_op(DWORD f);
static void core_state(const vk_device *d, pipe_key *k)
{
    DWORD cull = d->rs[D3DRS_CULLMODE];
    k->c_cull = (uint8_t)(cull == D3DCULL_CCW ? VK_CULL_MODE_BACK_BIT : cull == D3DCULL_CW ? VK_CULL_MODE_FRONT_BIT
                                                                                          : VK_CULL_MODE_NONE);
    int has_depth = d->ds != NULL;
    k->c_ztest = (has_depth && d->rs[D3DRS_ZENABLE] != D3DZB_FALSE) ? 1 : 0;
    k->c_zwrite = (k->c_ztest && d->rs[D3DRS_ZWRITEENABLE]) ? 1 : 0;
    k->c_zop = k->c_ztest ? (uint8_t)cmp_op(d->rs[D3DRS_ZFUNC]) : 0;   /* sem teste, o op não importa */
    k->c_stencil = (has_depth && d->ds->img->fmt->stencil && d->rs[D3DRS_STENCILENABLE]) ? 1 : 0;
    if (k->c_stencil) {
        k->c_two = d->rs[D3DRS_TWOSIDEDSTENCILMODE] ? 1 : 0;
        uint32_t f = (uint32_t)stencil_op(d->rs[D3DRS_STENCILFAIL]) | (uint32_t)stencil_op(d->rs[D3DRS_STENCILPASS]) << 4 |
                     (uint32_t)stencil_op(d->rs[D3DRS_STENCILZFAIL]) << 8 | (uint32_t)cmp_op(d->rs[D3DRS_STENCILFUNC]) << 12;
        uint32_t b = k->c_two ? ((uint32_t)stencil_op(d->rs[D3DRS_CCW_STENCILFAIL]) |
                                 (uint32_t)stencil_op(d->rs[D3DRS_CCW_STENCILPASS]) << 4 |
                                 (uint32_t)stencil_op(d->rs[D3DRS_CCW_STENCILZFAIL]) << 8 |
                                 (uint32_t)cmp_op(d->rs[D3DRS_CCW_STENCILFUNC]) << 12)
                              : f;
        k->c_sops = f | b << 16;
    }
    float bias = f_of(d->rs[D3DRS_DEPTHBIAS]), slope = f_of(d->rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    k->c_bias = (has_depth && (bias != 0.0f || slope != 0.0f)) ? 1 : 0;
}

static uint64_t key_hash(const pipe_key *k)
{
    uint32_t h = fnv(k, sizeof *k, 2166136261u);
    return h ? h : 1;
}

/* tabela aberta: (hash, chave) -> pipeline */
typedef struct { uint64_t h; pipe_key k; VkPipeline p; } pipe_slot;
static pipe_slot *g_pipes;
static uint32_t g_pipe_cap, g_pipe_n;

static VkPipeline *pipe_find(const pipe_key *k, uint64_t h)
{
    if (!g_pipes || g_pipe_n * 2 >= g_pipe_cap) {
        uint32_t cap = g_pipe_cap ? g_pipe_cap * 2 : 1024;
        pipe_slot *n = calloc(cap, sizeof *n);
        if (!n) re5_fatal("re5vk", "sem memória");
        for (uint32_t i = 0; i < g_pipe_cap; i++) {
            if (!g_pipes[i].h) continue;
            uint32_t j = (uint32_t)g_pipes[i].h & (cap - 1);
            while (n[j].h) j = (j + 1) & (cap - 1);
            n[j] = g_pipes[i];
        }
        free(g_pipes);
        g_pipes = n;
        g_pipe_cap = cap;
    }
    uint32_t j = (uint32_t)h & (g_pipe_cap - 1);
    while (g_pipes[j].h) {
        if (g_pipes[j].h == h && !memcmp(&g_pipes[j].k, k, sizeof *k)) return &g_pipes[j].p;
        j = (j + 1) & (g_pipe_cap - 1);
    }
    g_pipes[j].h = h;
    g_pipes[j].k = *k;
    g_pipes[j].p = VK_NULL_HANDLE;
    g_pipe_n++;
    return &g_pipes[j].p;
}

static uint32_t topo_class(D3DPRIMITIVETYPE t)
{
    if (t == D3DPT_POINTLIST) return 0;
    if (t == D3DPT_LINELIST || t == D3DPT_LINESTRIP) return 1;
    return 2;
}

static VkPrimitiveTopology topology(D3DPRIMITIVETYPE t)
{
    switch (t) {
    case D3DPT_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3DPT_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3DPT_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3DPT_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case D3DPT_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: re5_fatal("re5vk", "D3DPRIMITIVETYPE %u", (unsigned)t); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static int format_blends(vk_device *d, VkFormat f)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    return !!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

/* RE5VK_MASKED_KILL=1 liga (experimento). Com a escrita de cor toda mascarada (passada só de profundidade), o
 * driver do Mali pula o fragment shader, e com ele o discard do alpha test/texkill: a profundidade do
 * quad inteiro fica gravada, e o que está atrás das folhas, da cerca de arame e do cabelo some (sobra
 * o fundo preto). Confirmado no A54 com RE5VK_SKIP_NOCOLOR=1. Nesses draws, a escrita fica ligada com
 * blend ZERO/ONE (a cor no alvo não muda) e o shader roda. */
static int masked_kill_fix(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_MASKED_KILL");
        on = e && *e == '1';   /* não resolveu no A54 (2026-11-18): só com =1 */
    }
    return on;
}

static VkPipeline pipeline_create(vk_device *d, const pipe_key *k, VkShaderModule vsm, const re5vk_shader *vsi,
                                  VkShaderModule psm, const re5vk_shader *psi)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vsm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = psm, .pName = "main"},
    };
    /* entradas do VS pela semântica da declaração */
    VkVertexInputAttributeDescription attr[16];
    VkVertexInputBindingDescription bind[17];
    uint32_t na = 0, nb = 0, used_streams = 0;
    int need_zero = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!(vsi->inputs >> r & 1)) continue;
        const D3DVERTEXELEMENT9 *e = decl_elem(d, vsi, r);
        if (!e) {   /* sem dado: buffer zerado com stride 0 */
            attr[na++] = (VkVertexInputAttributeDescription){r, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            need_zero = 1;
            continue;
        }
        int intk;
        VkFormat vf = vertex_format_dev(d, e->Type, &intk);
        int expected_intk = (k->vsv.in_uint >> r & 1) ? 1 : (k->vsv.in_sint >> r & 1) ? 2 : (k->vsv.in_udec3 >> r & 1) ? 3 : 0;
        if (intk != expected_intk)
            re5_fatal("re5vk", "v%u: variante do VS não bate com o formato de vértice (intk=%d vs %d)", r, intk, expected_intk);
        if (vf == VK_FORMAT_UNDEFINED) re5_fatal("re5vk", "D3DDECLTYPE %u sem tradução", (unsigned)e->Type);
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(d->phys, vf, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            re5_fatal("re5vk", "D3DDECLTYPE %u (Vulkan %d) sem suporte de vértice nesta GPU", (unsigned)e->Type, (int)vf);
        if (e->Stream >= 15) re5_fatal("re5vk", "stream %u (o 15 é o buffer zerado do re5vk)", (unsigned)e->Stream);
        attr[na++] = (VkVertexInputAttributeDescription){r, e->Stream, vf, e->Offset};
        used_streams |= 1u << e->Stream;
    }
    for (uint32_t s = 0; s < 15; s++)
        if (used_streams >> s & 1) {
            /* Stride tambem estatico (o valor do draw que cria o pipeline): com o estado dinamico ele e
             * ignorado, mas um driver Mali que so respeita o stride dinamico no binding 0 leria o stream 1
             * (tangente, UV2 das cabecas) com stride 0 = mesmo vertice para todos. RE5VK_STATIC_STRIDE=0 tira. */
            uint32_t st = (static_stride_on() || d->core) ? k->stride[s] : 0u;
            bind[nb++] = (VkVertexInputBindingDescription){s, st, VK_VERTEX_INPUT_RATE_VERTEX};
        }
    if (need_zero) bind[nb++] = (VkVertexInputBindingDescription){15, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = bind,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = attr};
    static const VkPrimitiveTopology cls[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = k->topo_class >= 100 ? (VkPrimitiveTopology)(k->topo_class - 100)
                                                                                  : cls[k->topo_class],
                                                 .primitiveRestartEnable = VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = k->fill == D3DFILL_WIREFRAME ? VK_POLYGON_MODE_LINE
                     : k->fill == D3DFILL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .depthBiasEnable = d->core ? k->c_bias : d->has_eds2 ? VK_FALSE : VK_TRUE,
        .cullMode = d->core ? k->c_cull : VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .rasterizerDiscardEnable = VK_FALSE,
    };
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* Máscaras de stencil não-zero no estado estático, mesmo sendo dinâmicas: driver da ARM antes do
     * r43 quebra o estado dinâmico com discard quando a máscara estática de escrita é 0 (ANGLE,
     * useNonZeroStencilWriteMaskStaticState, anglebug 7556). O valor real vem do estado dinâmico. */
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .front = {.compareMask = 0xFF, .writeMask = 0xFF},
                                                .back = {.compareMask = 0xFF, .writeMask = 0xFF},
                                                .maxDepthBounds = 1.0f};
    if (d->core) {
        ds.depthTestEnable = k->c_ztest;
        ds.depthWriteEnable = k->c_zwrite;
        ds.depthCompareOp = (VkCompareOp)k->c_zop;
        ds.stencilTestEnable = k->c_stencil;
        uint32_t f = k->c_sops & 0xFFFF, b = k->c_sops >> 16;
        ds.front.failOp = (VkStencilOp)(f & 15);
        ds.front.passOp = (VkStencilOp)(f >> 4 & 15);
        ds.front.depthFailOp = (VkStencilOp)(f >> 8 & 15);
        ds.front.compareOp = (VkCompareOp)(f >> 12 & 15);
        ds.back.failOp = (VkStencilOp)(b & 15);
        ds.back.passOp = (VkStencilOp)(b >> 4 & 15);
        ds.back.depthFailOp = (VkStencilOp)(b >> 8 & 15);
        ds.back.compareOp = (VkCompareOp)(b >> 12 & 15);
    }
    VkPipelineColorBlendAttachmentState cb[4];
    uint32_t ncol = 0;
    for (uint32_t i = 0; i < 4; i++) {
        memset(&cb[i], 0, sizeof cb[i]);
        if (k->color[i] == VK_FORMAT_UNDEFINED) continue;
        ncol = i + 1;
        uint32_t w = k->write[i];
        cb[i].colorWriteMask = (w & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (w & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                               (w & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) | (w & 8 ? VK_COLOR_COMPONENT_A_BIT : 0);
        if (k->blend[0] && format_blends(d, k->color[i])) {
            cb[i].blendEnable = VK_TRUE;
            cb[i].srcColorBlendFactor = blend_of(k->blend[1], 0);
            cb[i].dstColorBlendFactor = blend_of(k->blend[2], 0);
            cb[i].colorBlendOp = blendop_of(k->blend[3]);
            cb[i].srcAlphaBlendFactor = blend_of(k->blend[4], 1);
            cb[i].dstAlphaBlendFactor = blend_of(k->blend[5] & 0xFFFF, 1);
            cb[i].alphaBlendOp = blendop_of(k->blend[5] >> 16);
            /* BOTHSRCALPHA: o destino é o inverso da fonte */
            if (k->blend[1] == D3DBLEND_BOTHSRCALPHA) {
                cb[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
        }
    }
    if (rs_log_on()) {   /* draws sem escrita de cor: o que a correção do discard vê */
        int any_w = 0;
        for (uint32_t i = 0; i < ncol; i++) any_w |= cb[i].colorWriteMask != 0;
        if (!any_w) {
            static int n;
            if (n++ < 40)
                RE5_INFO("re5vk", "RS_LOG sem cor: ps %08X kill %d alpha %d ncol %u rt0 %d blends %d depth %d write0 %u",
                         (unsigned)k->ps_hash, psi ? psi->uses_kill : -1, (int)k->psv.alpha_func, ncol,
                         (int)k->color[0], k->color[0] ? format_blends(d, k->color[0]) : -1, (int)k->depth,
                         (unsigned)k->write[0]);
        }
    }
    if (psi && psi->uses_kill && masked_kill_fix() && ncol) {
        int any_write = 0;
        for (uint32_t i = 0; i < ncol; i++) any_write |= cb[i].colorWriteMask != 0;
        for (uint32_t i = 0; !any_write && i < ncol; i++) {
            if (k->color[i] == VK_FORMAT_UNDEFINED || !format_blends(d, k->color[i])) continue;
            cb[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                   VK_COLOR_COMPONENT_A_BIT;
            cb[i].blendEnable = VK_TRUE;
            cb[i].srcColorBlendFactor = cb[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cb[i].dstColorBlendFactor = cb[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb[i].colorBlendOp = cb[i].alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        }
    }
    VkPipelineColorBlendStateCreateInfo bs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = ncol, .pAttachments = cb};
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
    };
    /* núcleo 1.0: viewport, scissor, depth bias, blend constants e máscaras/referência do stencil */
    static const VkDynamicState dyn_core[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    uint32_t dyn_count = d->core ? (uint32_t)(sizeof dyn_core / sizeof dyn_core[0])
                       : d->has_eds2 ? (uint32_t)(sizeof dyn / sizeof dyn[0]) : 17u;
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = dyn_count, .pDynamicStates = d->core ? dyn_core : dyn};
    /* Qualquer passe com os mesmos formatos é compatível (ops e layouts não contam). */
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .renderPass = re5_vkn_compat_pass(ncol, k->color, k->depth, k->stencil),
        .subpass = 0,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &bs,
        .pDynamicState = &dy,
        .layout = d->layout,
    };
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(d->dev, d->pcache, 1, &ci, NULL, &p));
    /* Conta para o pcache_save: sem isto o contador ficava em 0 e o cache de pipelines nunca era
     * gravado (toda abertura recompilava tudo: carregamento longo e engasgos). */
    d->npipes++;
    return p;
}

/* ---- draw ---- */

static VkStencilOp stencil_op(DWORD o) { return (VkStencilOp)(o >= 1 && o <= 8 ? o - 1 : 0); }
static VkCompareOp cmp_op(DWORD f) { return (VkCompareOp)(f >= 1 && f <= 8 ? f - 1 : 7); }

static int tex_is_depth(vk_obj *t)
{
    if (!t || (t->kind != VKO_TEXTURE)) return 0;
    vk_texture *tex = (vk_texture *)t;
    /* INTZ/DF16/DF24 devolvem a profundidade crua, sem comparação (como a DXVK,
     * D3D9CommonTexture::DetermineShadowState); só D16/D24 comuns são mapa de sombra. */
    if (tex->format == FOURCC('I', 'N', 'T', 'Z') || tex->format == FOURCC('D', 'F', '1', '6') ||
        tex->format == FOURCC('D', 'F', '2', '4'))
        return 0;
    return tex->img.fmt->depth;
}

static void set_dynamic(vk_device *d, D3DPRIMITIVETYPE pt)
{
    VkCommandBuffer cb = re5vk_cmd(d);
    VkPrimitiveTopology t = topology(pt);
    int force = !d->dyn_valid;

    if (!d->core && (force || d->bound_dyn.topo != t)) {
        vkCmdSetPrimitiveTopology(cb, t);
        d->bound_dyn.topo = t;
    }
    if (force && !d->core) {
        vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
        vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
        if (d->has_eds2) {
            vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
            vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
        }
    }

    /* Sempre confere o scissor contra d->render_area mesmo se !dyn_dirty (novo passe pode ter área diferente) */
    VkRect2D sc = {{0, 0}, d->render_area};
    if (RS(d, D3DRS_SCISSORTESTENABLE)) {
        sc = (VkRect2D){{d->scissor.left, d->scissor.top},
                        {(uint32_t)(d->scissor.right > d->scissor.left ? d->scissor.right - d->scissor.left : 0),
                         (uint32_t)(d->scissor.bottom > d->scissor.top ? d->scissor.bottom - d->scissor.top : 0)}};
        clamp_rect(&sc, d->render_area);
    }
    if (force || memcmp(&d->bound_dyn.scissor, &sc, sizeof sc) != 0) {
        vkCmdSetScissor(cb, 0, 1, &sc);
        d->bound_dyn.scissor = sc;
    }

    if (!force && !d->dyn_dirty) return;
    d->dyn_dirty = 0;
    d->dyn_valid = 1;

    VkViewport v = {(float)d->vp.X + 0.5f, (float)(d->vp.Y + d->vp.Height) + 0.5f, (float)d->vp.Width,
                    -(float)d->vp.Height, d->vp.MinZ, d->vp.MaxZ};
    if (force || memcmp(&d->bound_dyn.vp, &v, sizeof v) != 0) {
        vkCmdSetViewport(cb, 0, 1, &v);
        d->bound_dyn.vp = v;
    }
    /* cull: na tela (y para baixo nos dois) a ordem é a do D3D9 */
    DWORD cull = RS(d, D3DRS_CULLMODE);
    VkCullModeFlags cm = cull == D3DCULL_CCW ? VK_CULL_MODE_BACK_BIT : cull == D3DCULL_CW ? VK_CULL_MODE_FRONT_BIT
                                                                                          : VK_CULL_MODE_NONE;
    if (!d->core && (force || d->bound_dyn.cull != cm)) {
        vkCmdSetCullMode(cb, cm);
        d->bound_dyn.cull = cm;
    }

    int has_depth = d->ds != NULL;
    VkBool32 ztest = (has_depth && RS(d, D3DRS_ZENABLE) != D3DZB_FALSE) ? VK_TRUE : VK_FALSE;
    VkBool32 zwrite = (ztest && RS(d, D3DRS_ZWRITEENABLE)) ? VK_TRUE : VK_FALSE;
    VkCompareOp zop = cmp_op(RS(d, D3DRS_ZFUNC));
    if (!d->core && (force || d->bound_dyn.depth_test != ztest)) {
        vkCmdSetDepthTestEnable(cb, ztest);
        d->bound_dyn.depth_test = ztest;
    }
    if (!d->core && (force || d->bound_dyn.depth_write != zwrite)) {
        vkCmdSetDepthWriteEnable(cb, zwrite);
        d->bound_dyn.depth_write = zwrite;
    }
    if (!d->core && (force || d->bound_dyn.depth_op != zop)) {
        vkCmdSetDepthCompareOp(cb, zop);
        d->bound_dyn.depth_op = zop;
    }

    VkBool32 stencil = (has_depth && d->ds->img->fmt->stencil && RS(d, D3DRS_STENCILENABLE)) ? VK_TRUE : VK_FALSE;
    if (!d->core && (force || d->bound_dyn.stencil_test != stencil)) {
        vkCmdSetStencilTestEnable(cb, stencil);
        d->bound_dyn.stencil_test = stencil;
    }
    if (force || stencil) {
        VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
        DWORD two = RS(d, D3DRS_TWOSIDEDSTENCILMODE) ? 1u : 0u;
        VkStencilOp sf_f = stencil_op(RS(d, D3DRS_STENCILFAIL));
        VkStencilOp sf_p = stencil_op(RS(d, D3DRS_STENCILPASS));
        VkStencilOp sf_z = stencil_op(RS(d, D3DRS_STENCILZFAIL));
        VkCompareOp sf_c = cmp_op(RS(d, D3DRS_STENCILFUNC));
        if (!d->core && (force || d->bound_dyn.stencil_two_sided != two || d->bound_dyn.sf_fail != sf_f ||
            d->bound_dyn.sf_pass != sf_p || d->bound_dyn.sf_zfail != sf_z || d->bound_dyn.sf_func != sf_c)) {
            vkCmdSetStencilOp(cb, two ? VK_STENCIL_FACE_FRONT_BIT : both, sf_f, sf_p, sf_z, sf_c);
            d->bound_dyn.sf_fail = sf_f;
            d->bound_dyn.sf_pass = sf_p;
            d->bound_dyn.sf_zfail = sf_z;
            d->bound_dyn.sf_func = sf_c;
        }
        if (two && !d->core) {
            VkStencilOp sb_f = stencil_op(RS(d, D3DRS_CCW_STENCILFAIL));
            VkStencilOp sb_p = stencil_op(RS(d, D3DRS_CCW_STENCILPASS));
            VkStencilOp sb_z = stencil_op(RS(d, D3DRS_CCW_STENCILZFAIL));
            VkCompareOp sb_c = cmp_op(RS(d, D3DRS_CCW_STENCILFUNC));
            if (force || !d->bound_dyn.stencil_two_sided || d->bound_dyn.sb_fail != sb_f ||
                d->bound_dyn.sb_pass != sb_p || d->bound_dyn.sb_zfail != sb_z || d->bound_dyn.sb_func != sb_c) {
                vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, sb_f, sb_p, sb_z, sb_c);
                d->bound_dyn.sb_fail = sb_f;
                d->bound_dyn.sb_pass = sb_p;
                d->bound_dyn.sb_zfail = sb_z;
                d->bound_dyn.sb_func = sb_c;
            }
        }
        d->bound_dyn.stencil_two_sided = two;
        uint32_t cmask = RS(d, D3DRS_STENCILMASK), wmask = RS(d, D3DRS_STENCILWRITEMASK), ref = RS(d, D3DRS_STENCILREF);
        if (force || d->bound_dyn.stencil_cmp_mask != cmask) {
            vkCmdSetStencilCompareMask(cb, both, cmask);
            d->bound_dyn.stencil_cmp_mask = cmask;
        }
        if (force || d->bound_dyn.stencil_write_mask != wmask) {
            vkCmdSetStencilWriteMask(cb, both, wmask);
            d->bound_dyn.stencil_write_mask = wmask;
        }
        if (force || d->bound_dyn.stencil_ref != ref) {
            vkCmdSetStencilReference(cb, both, ref);
            d->bound_dyn.stencil_ref = ref;
        }
    }

    /* depth bias: o do D3D9 é em unidades de profundidade; o do Vulkan, no menor degrau do formato */
    float bias = f_of(RS(d, D3DRS_DEPTHBIAS)), slope = f_of(RS(d, D3DRS_SLOPESCALEDEPTHBIAS));
    VkBool32 bias_on = (has_depth && (bias != 0.0f || slope != 0.0f)) ? VK_TRUE : VK_FALSE;
    if (d->has_eds2 && (force || d->bound_dyn.bias_on != bias_on)) {
        vkCmdSetDepthBiasEnable(cb, bias_on);
        d->bound_dyn.bias_on = bias_on;
    }
    if (bias_on) {
        VkFormat df = d->ds->img->vkf;
        float r = df == VK_FORMAT_D16_UNORM ? 65535.0f : (df == VK_FORMAT_D24_UNORM_S8_UINT ? 16777215.0f : 8388608.0f);
        float vb = bias * r;
        if (force || d->bound_dyn.depth_bias != vb || d->bound_dyn.depth_slope != slope) {
            vkCmdSetDepthBias(cb, vb, 0.0f, slope);
            d->bound_dyn.depth_bias = vb;
            d->bound_dyn.depth_slope = slope;
        }
    } else if (force || !d->has_eds2) {
        if (force || d->bound_dyn.depth_bias != 0.0f || d->bound_dyn.depth_slope != 0.0f) {
            vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
            d->bound_dyn.depth_bias = 0.0f;
            d->bound_dyn.depth_slope = 0.0f;
        }
    }

    DWORD bf = RS(d, D3DRS_BLENDFACTOR);
    float bc[4] = {(float)((bf >> 16) & 0xFF) / 255.0f, (float)((bf >> 8) & 0xFF) / 255.0f, (float)(bf & 0xFF) / 255.0f,
                   (float)(bf >> 24) / 255.0f};
    if (force || memcmp(d->bound_dyn.blend_const, bc, sizeof bc) != 0) {
        vkCmdSetBlendConstants(cb, bc);
        memcpy(d->bound_dyn.blend_const, bc, sizeof bc);
    }
}

static vk_image *bound_image(vk_device *d, int slot, int want_type, int *is_depth)
{
    vk_obj *t = d->tex[slot];
    *is_depth = 0;
    int kind = want_type == RE5VK_TEX_CUBE ? VKO_CUBE : want_type == RE5VK_TEX_3D ? VKO_VOLUME : VKO_TEXTURE;
    if (t && t->kind == kind) {
        vk_texture *tx = (vk_texture *)t;
        *is_depth = tx->img.fmt->depth;
        tx->img.last_use = d->serial;
        return &tx->img;
    }
    return want_type == RE5VK_TEX_CUBE ? &d->dummycube : want_type == RE5VK_TEX_3D ? &d->dummy3d : &d->dummy2d;
}

static int img_is_attached(const vk_device *d, const vk_image *im)
{
    for (int i = 0; i < 4; i++)
        if (d->rt[i] && d->rt[i]->img == im) return 1;
    if (d->ds && d->ds->img == im) return 1;
    return 0;
}

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
static inline void copy_floats_flush_nan(void *dst, const void *src, size_t count_vec4)
{
    float32x4_t *d = (float32x4_t *)dst;
    const float32x4_t *s = (const float32x4_t *)src;
    for (size_t i = 0; i < count_vec4; i++) {
        float32x4_t v = s[i];
        uint32x4_t mask = vceqq_f32(v, v);
        d[i] = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(v), mask));
    }
}
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
static inline void copy_floats_flush_nan(void *dst, const void *src, size_t count_vec4)
{
    __m128 *d = (__m128 *)dst;
    const __m128 *s = (const __m128 *)src;
    for (size_t i = 0; i < count_vec4; i++) {
        __m128 v = _mm_loadu_ps((const float *)&s[i]);
        __m128 mask = _mm_cmpeq_ps(v, v);
        _mm_storeu_ps((float *)&d[i], _mm_and_ps(v, mask));
    }
}
#else
#include <math.h>
static inline void copy_floats_flush_nan(void *dst, const void *src, size_t count_vec4)
{
    float *d = (float *)dst;
    const float *s = (const float *)src;
    size_t n = count_vec4 * 4;
    for (size_t i = 0; i < n; i++) {
        d[i] = isnan(s[i]) ? 0.0f : s[i];
    }
}
#endif

static void upload_vs_ubo(vk_device *d, const re5vk_shader *vsi, uint8_t vs_ib)
{
    uint8_t *u = re5vk_ubo(d, RE5VK_VS_UBO_SIZE, &d->vs_ubo_buf, &d->vs_ubo_off);
    copy_floats_flush_nan(u, d->vsf, (size_t)vsi->const_f);
    if (vsi->uses_i) memcpy(u + RE5VK_VS_UBO_I, d->vsi, sizeof d->vsi);
    if (vsi->uses_b) memcpy(u + RE5VK_VS_UBO_B, &d->vsb, 4);
    d->vs_ubo_f = vsi->const_f;
    d->vs_ubo_ib = vs_ib;
    d->vs_dirty = 0;
}

/* force = 1: os dois blocos de novo, no anel do envio corrente (depois de um flush no meio do draw). */
static void upload_ubos(vk_device *d, const re5vk_shader *vsi, const re5vk_shader *psi, uint8_t vs_ib, uint8_t ps_ib,
                        int force)
{
    if (force || d->vs_dirty || vsi->const_f > d->vs_ubo_f || (vs_ib & ~d->vs_ubo_ib)) upload_vs_ubo(d, vsi, vs_ib);
    if (force || d->ps_dirty || psi->const_f > d->ps_ubo_f || (ps_ib & ~d->ps_ubo_ib)) {
        uint64_t ser = d->serial;
        uint8_t *u = re5vk_ubo(d, RE5VK_PS_UBO_SIZE, &d->ps_ubo_buf, &d->ps_ubo_off);
        copy_floats_flush_nan(u, d->psf, (size_t)psi->const_f);
        if (psi->uses_i) memcpy(u + RE5VK_PS_UBO_I, d->psi, sizeof d->psi);
        if (psi->uses_b) memcpy(u + RE5VK_PS_UBO_B, &d->psb, 4);
        float aref = (float)(RS(d, D3DRS_ALPHAREF) & 0xFF) / 255.0f;
        memcpy(u + RE5VK_PS_UBO_AREF, &aref, 4);
        d->ps_ubo_f = psi->const_f;
        d->ps_ubo_ib = ps_ib;
        d->ps_dirty = 0;
        /* O anel do PS encheu e mandou o envio: o bloco do VS ficou no anel do envio anterior,
         * que volta a ser escrito quando aquele slot for reaproveitado. Refaz no anel novo.
         * (Antes a condição era "&& d->vs_dirty", sempre falsa aqui: nunca refazia.) */
        if (d->serial != ser) upload_vs_ubo(d, vsi, vs_ib);
    }
}

/* O sub-recurso (layer, level) de im está preso como alvo de cor ou profundidade agora? */
static int sub_is_attached(const vk_device *d, const vk_image *im, uint32_t layer, uint32_t level)
{
    for (int i = 0; i < 4; i++)
        if (d->rt[i] && d->rt[i]->img == im && d->rt[i]->layer == layer && d->rt[i]->level == level) return 1;
    return d->ds && d->ds->img == im && d->ds->layer == layer && d->ds->level == level;
}

/* Imagem amostrada que também está presa como anexo (ex.: desenhar no mip 0 lendo os outros mips,
 * ou a profundidade presa e lida no mesmo draw). O descritor diz GENERAL para a imagem inteira, e o
 * anexo vai a GENERAL no begin_rendering; os outros sub-recursos também precisam estar em GENERAL e
 * com as escritas visíveis. Antes eles ficavam em COLOR_ATTACHMENT/SHADER_READ_ONLY sem barreira:
 * descritor e layout real divergiam. Na AMD o layout quase não importa; no Adreno (UBWC) e no Mali
 * (AFBC) a leitura sai como lixo. apply = 0 só pergunta se falta barreira. */
static int conflict_subs(vk_device *d, vk_image *im, int apply)
{
    re5vk_subref att[5];
    uint32_t na = 0;
    for (int i = 0; i < 4; i++)
        if (d->rt[i] && d->rt[i]->img == im) att[na++] = (re5vk_subref){d->rt[i]->layer, d->rt[i]->level};
    if (d->ds && d->ds->img == im) att[na++] = (re5vk_subref){d->ds->layer, d->ds->level};
    if (!apply)
        return re5vk_layout_pending(im->sub_layouts, im->sub_dirty, im->levels, im->layers, VK_IMAGE_LAYOUT_GENERAL,
                                    att, na, NULL, 0) != 0;
    uint32_t idx[RE5VK_MAX_SUBS];
    uint32_t n = re5vk_layout_pending(im->sub_layouts, im->sub_dirty, im->levels, im->layers, VK_IMAGE_LAYOUT_GENERAL,
                                      att, na, idx, RE5VK_MAX_SUBS);
    if (n > RE5VK_MAX_SUBS)
        re5_fatal("re5vk", "imagem com %u sub-recursos a transicionar (máximo %u)", n, RE5VK_MAX_SUBS);
    for (uint32_t k = 0; k < n; k++)
        re5vk_img_transition(d, re5vk_cmd(d), im, idx[k] % im->levels, 1, idx[k] / im->levels, 1,
                             VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return n != 0;
}

static void prepare(vk_device *d, D3DPRIMITIVETYPE pt)
{
    pass_sync(d);   /* antes de olhar layouts e conflitos: eles valem para o conjunto de alvos atual */
    if (!d->vs || !d->ps) re5_fatal("re5vk", "draw sem vertex/pixel shader (fixed function sem implementação)");
    if (RS(d, D3DRS_CLIPPLANEENABLE) && !d->warned_clip) {
        RE5_WARN("re5vk", "D3DRS_CLIPPLANEENABLE=0x%X: planos de recorte não aplicados (como a DXVK no Mali)",
                 (unsigned)RS(d, D3DRS_CLIPPLANEENABLE));
        d->warned_clip = 1;
    }
    if (RS(d, D3DRS_SRGBWRITEENABLE)) re5_fatal("re5vk", "D3DRS_SRGBWRITEENABLE sem implementação");

    /* Variantes e pipeline ANTES de begin_rendering para saber quais texturas entram no draw e
     * preparar as transições de layout fora do passe (exigido pelo Vulkan). */
    re5vk_dxso_variant psv;
    const re5vk_shader *vsi, *psi;
    VkPipeline pipe;
    uint32_t tc = d->core ? 100u + (uint32_t)topology(pt) : topo_class(pt);
    if (d->last_pipe && d->last_gen == d->pipe_gen && d->last_topo == tc) {
        pipe = d->last_pipe;
        vsi = d->last_vsi;
        psi = d->last_psi;
        psv = d->last_psv;
    } else {
        re5vk_dxso_variant vsv;
        memset(&vsv, 0, sizeof vsv);
        VkShaderModule vsm, psm;
        /* variantes */
        memset(&psv, 0, sizeof psv);
        re5vk_shader_variant(d, d->vs, &vsv, &vsm, &vsi);
        /* entradas inteiras (GPU sem *SCALED): a variante sai das entradas do VS e da declaração */
        for (uint32_t r = 0; r < 16; r++) {
            const D3DVERTEXELEMENT9 *e = (vsi->inputs >> r & 1) ? decl_elem(d, vsi, r) : NULL;
            int intk = 0;
            if (e) vertex_format_dev(d, e->Type, &intk);
            if (intk == 1) vsv.in_uint |= (uint16_t)(1u << r);
            if (intk == 2) vsv.in_sint |= (uint16_t)(1u << r);
            if (intk == 3) vsv.in_udec3 |= (uint16_t)(1u << r);
        }
        if (vsv.in_uint || vsv.in_sint || vsv.in_udec3) re5vk_shader_variant(d, d->vs, &vsv, &vsm, &vsi);
        /* primeiro sem as profundidades, para saber os samplers do PS */
        psv.alpha_func = RS(d, D3DRS_ALPHATESTENABLE) ? (uint8_t)RS(d, D3DRS_ALPHAFUNC) : 0;
        if (psv.alpha_func == D3DCMP_ALWAYS) psv.alpha_func = 0;
        if (psv.alpha_func) ps_const_log(d, d->ps->hash, psv.alpha_func);
        for (int s = 0; s < 16; s++)
            if (tex_is_depth(d->tex[s])) psv.depth_samplers |= (uint16_t)(1u << s);
        re5vk_shader_variant(d, d->ps, &psv, &psm, &psi);

        pipe_key k;
        memset(&k, 0, sizeof k);
        k.vs_hash = d->vs->hash;
        k.vs_len = d->vs->ndw;
        k.ps_hash = d->ps->hash;
        k.ps_len = d->ps->ndw;
        k.vsv = vsv;
        k.psv = psv;
        k.decl_hash = d->decl ? fnv(d->decl->el, d->decl->n * sizeof d->decl->el[0], 2166136261u) : 0;
        if (d->core) core_state(d, &k);
        if (d->decl && (static_stride_on() || d->core))
            for (uint32_t i = 0; i < d->decl->n; i++) {
                uint32_t sn = d->decl->el[i].Stream;
                if (sn < 16) k.stride[sn] = d->streams[sn].stride;
            }
        k.topo_class = tc;
        k.fill = RS(d, D3DRS_FILLMODE);
        if (RS(d, D3DRS_ALPHABLENDENABLE)) {
            k.blend[0] = 1;
            k.blend[1] = RS(d, D3DRS_SRCBLEND);
            k.blend[2] = RS(d, D3DRS_DESTBLEND);
            k.blend[3] = RS(d, D3DRS_BLENDOP);
            if (RS(d, D3DRS_SEPARATEALPHABLENDENABLE)) {
                k.blend[4] = RS(d, D3DRS_SRCBLENDALPHA);
                k.blend[5] = RS(d, D3DRS_DESTBLENDALPHA) | RS(d, D3DRS_BLENDOPALPHA) << 16;
            } else {
                k.blend[4] = k.blend[1];
                k.blend[5] = k.blend[2] | k.blend[3] << 16;
            }
        }
        static const D3DRENDERSTATETYPE wr[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
                                                 D3DRS_COLORWRITEENABLE3};
        for (int i = 0; i < 4; i++) {
            k.color[i] = d->rt[i] ? d->rt[i]->img->vkf : VK_FORMAT_UNDEFINED;
            k.write[i] = RS(d, wr[i]) & 0xF;
        }
        k.depth = d->ds ? d->ds->img->vkf : VK_FORMAT_UNDEFINED;
        k.stencil = d->ds ? d->ds->img->fmt->stencil : 0;
        VkPipeline *pp = pipe_find(&k, key_hash(&k));
        if (!*pp) *pp = pipeline_create(d, &k, vsm, vsi, psm, psi);
        pipe = *pp;
        d->last_pipe = pipe;
        d->last_gen = d->pipe_gen;
        d->last_topo = tc;
        d->last_vsi = vsi;
        d->last_psi = psi;
        d->last_psv = psv;
    }

    /* Coleta samplers ativos neste draw e resolve layouts (ótimo ou GENERAL em conflito anexo+sampler). */
    int gen = re5vk_layout_general();
    vk_image *samp_im[20];
    int samp_dep[20], samp_slot[20], samp_is_vs[20], samp_conflict[20];
    VkImageLayout samp_layout[20];
    uint32_t nsamp = 0;
    for (int s = 0; s < 16; s++) {
        if (!(psi->samplers >> s & 1)) continue;
        int dep;
        vk_image *im = bound_image(d, s, psi->sampler_type[s], &dep);
        int conf = img_is_attached(d, im);
        samp_im[nsamp] = im;
        samp_dep[nsamp] = dep;
        samp_slot[nsamp] = s;
        samp_is_vs[nsamp] = 0;
        samp_conflict[nsamp] = conf;
        samp_layout[nsamp] = (gen || conf) ? VK_IMAGE_LAYOUT_GENERAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        nsamp++;
    }
    for (int s = 0; s < 4; s++) {
        if (!(vsi->samplers >> s & 1)) continue;
        int dep;
        vk_image *im = bound_image(d, 16 + s, vsi->sampler_type[s], &dep);
        int conf = img_is_attached(d, im);
        samp_im[nsamp] = im;
        samp_dep[nsamp] = dep;
        samp_slot[nsamp] = 16 + s;
        samp_is_vs[nsamp] = 1;
        samp_conflict[nsamp] = conf;
        samp_layout[nsamp] = (gen || conf) ? VK_IMAGE_LAYOUT_GENERAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        nsamp++;
    }

    VkImageLayout want_rt_l[4];
    for (int i = 0; i < 4; i++) {
        int conf = 0;
        if (d->rt[i])
            for (uint32_t j = 0; j < nsamp; j++)
                if (samp_im[j] == d->rt[i]->img) { conf = 1; break; }
        want_rt_l[i] = (gen || conf) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkImageLayout want_ds_l = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    {
        int conf = 0;
        if (d->ds)
            for (uint32_t j = 0; j < nsamp; j++)
                if (samp_im[j] == d->ds->img) { conf = 1; break; }
        if (gen || conf) want_ds_l = VK_IMAGE_LAYOUT_GENERAL;
    }

    /* Constantes e descritores ANTES de abrir/dividir o passe: um anel de UBO ou pool cheio
     * chama re5vk_flush() e invalida o VkCommandBuffer e o passe aberto. */
    uint8_t vs_ib = (uint8_t)(vsi->uses_i | vsi->uses_b << 1), ps_ib = (uint8_t)(psi->uses_i | psi->uses_b << 1);
    upload_ubos(d, vsi, psi, vs_ib, ps_ib, 0);

    uint64_t samp_sig = 0xcbf29ce484222325ull;
    for (uint32_t j = 0; j < nsamp; j++) {
        uint64_t b = (uint64_t)(samp_is_vs[j] ? (RE5VK_BIND_VS_SAMP + samp_slot[j] - 16)
                                              : (RE5VK_BIND_PS_SAMP + samp_slot[j]));
        samp_sig = (samp_sig ^ (b | ((uint64_t)samp_layout[j] << 16))) * 0x100000001b3ull;
    }
    if (d->desc_dirty || !d->set || d->bound_vs_buf != d->vs_ubo_buf || d->bound_ps_buf != d->ps_ubo_buf ||
        d->bound_samp_sig != samp_sig) {
        VkDescriptorImageInfo ii[20];
        uint32_t bind[20];
        uint32_t ni = 0;
        for (uint32_t j = 0; j < nsamp; j++) {
            int s = samp_slot[j];
            int cmp = !samp_is_vs[j] && samp_dep[j] && (psv.depth_samplers >> s & 1);
            int multimip = samp_im[j]->levels > 1;
            int nolin = !format_linear(d, samp_im[j]->vkf) || (samp_is_vs[j] && vtx_nearest());
            ii[ni] = (VkDescriptorImageInfo){sampler_cached(d, s, cmp, multimip, nolin),
                                             re5vk_image_sampled_view(d, samp_im[j], d->ss[s][D3DSAMP_SRGBTEXTURE] != 0),
                                             samp_layout[j]};
            bind[ni++] = (samp_is_vs[j] ? RE5VK_BIND_VS_SAMP : RE5VK_BIND_PS_SAMP) +
                         (uint32_t)(samp_is_vs[j] ? (s - 16) : s);
        }
        /* Mesmo conjunto (UBOs, bindings, layouts, views, samplers) já escrito neste envio: reusa o set. */
        uint64_t key[2 + 3 * RE5VK_NBINDINGS];
        uint32_t nk = 0;
        key[nk++] = (uint64_t)(uintptr_t)d->vs_ubo_buf;
        key[nk++] = (uint64_t)(uintptr_t)d->ps_ubo_buf;
        for (uint32_t i = 0; i < ni; i++) {
            key[nk++] = (uint64_t)(uintptr_t)ii[i].imageView;
            key[nk++] = (uint64_t)(uintptr_t)ii[i].sampler;
            key[nk++] = (uint64_t)bind[i] | ((uint64_t)ii[i].imageLayout << 16);
        }
        uint64_t h = set_key_hash(key, nk);
        int found;
        struct vk_set_cache_entry *e = set_cache_slot(d, key, nk, h, &found);
        if (found) {
            d->set = e->set;
        } else {
            uint64_t ser = d->serial;
            d->set = re5vk_alloc_set(d);   /* pode mandar o envio e zerar o cache (gen novo) */
            if (d->serial != ser) {
                /* o envio mudou: sujo ou não, os blocos têm de estar no anel do envio novo */
                upload_ubos(d, vsi, psi, vs_ib, ps_ib, 1);
                key[0] = (uint64_t)(uintptr_t)d->vs_ubo_buf;
                key[1] = (uint64_t)(uintptr_t)d->ps_ubo_buf;
                h = set_key_hash(key, nk);
            }
            VkWriteDescriptorSet w[RE5VK_NBINDINGS];
            VkDescriptorBufferInfo bi[2] = {{d->vs_ubo_buf, 0, RE5VK_VS_UBO_SIZE},
                                            {d->ps_ubo_buf, 0, RE5VK_PS_UBO_SIZE}};
            uint32_t nw = 0;
            for (int i = 0; i < 2; i++)
                w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set,
                                                 .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                                 .pBufferInfo = &bi[i]};
            for (uint32_t i = 0; i < ni; i++)
                w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set,
                                                 .dstBinding = bind[i], .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 .pImageInfo = &ii[i]};
            vkUpdateDescriptorSets(d->dev, nw, w, 0, NULL);
            if (d->set_cache_n < d->set_cache_cap / 2u || !d->set_cache) {
                e = set_cache_slot(d, key, nk, h, &found);
                if (e && !found && d->set_cache_n < d->set_cache_cap / 2u) {
                    memcpy(e->key, key, nk * sizeof key[0]);
                    e->nkey = nk;
                    e->hash = h;
                    e->gen = d->set_cache_gen;
                    e->set = d->set;
                    d->set_cache_n++;
                }
            }
        }
        d->bound_vs_buf = d->vs_ubo_buf;
        d->bound_ps_buf = d->ps_ubo_buf;
        d->bound_samp_sig = samp_sig;
        d->desc_dirty = 0;
    }

    /* Se o passe já estiver aberto, só fecha se algum anexo mudar para GENERAL ou alguma textura
     * precisar de barreira/transição (proibidas dentro do passe). */
    if (d->rendering) {
        int need_split = 0;
        for (int i = 0; i < 4 && !need_split; i++)
            if (d->rt[i] && d->render_rt_layouts[i] != want_rt_l[i]) need_split = 1, d->split_why = "divisao:layout_rt";
        if (!need_split && d->ds && d->render_ds_layout != want_ds_l) need_split = 1, d->split_why = "divisao:layout_ds";
        for (uint32_t j = 0; j < nsamp && !need_split; j++) {
            vk_image *im = samp_im[j];
            if (samp_conflict[j]) {
                if (conflict_subs(d, im, 0)) need_split = 1, d->split_why = "divisao:amostra_alvo";
                continue;
            }
            if (re5vk_img_needs_barrier(im, 0, im->levels, 0, im->layers, samp_layout[j]))
                need_split = 1, d->split_why = img_any_dirty(im) ? "divisao:textura_recem_desenhada" : "divisao:transicao_textura";
        }
        if (need_split) {
            d->pass_was_split = 1;
            re5vk_end_rendering(d);
        }
    }

    /* Marca leitura TBDR das texturas amostradas e aplica transições pendentes fora do passe. */
    for (uint32_t j = 0; j < nsamp; j++) {
        vk_image *im = samp_im[j];
        im->last_use = d->serial;
        if (samp_is_vs[j]) {   /* uma linha por imagem lida no VS: formato, tamanho, mips */
            static const vk_image *seen[32];
            static int nseen;
            int known = 0;
            for (int q = 0; q < nseen; q++) known |= seen[q] == im;
            if (!known && nseen < 32) {
                seen[nseen++] = im;
                RE5_INFO("re5vk", "textura de vertice slot %d: D3DFMT %u vkf %d %ux%u mips %u layout %d rt_views %d dummy %d", samp_slot[j],
                         (unsigned)im->fmt->d3d, (int)im->vkf, im->w, im->h, im->levels, (int)samp_layout[j], im->rt_views != NULL,
                         im == &d->dummy2d);
            }
        }
        if (d->tbdr.enabled && im->tbdr_subs) {   /* desligado (padrão): nem percorre os sub-recursos */
            uint8_t amask = im->fmt->depth ? RE5VK_TBDR_DEPTH : RE5VK_TBDR_COLOR;
            for (uint32_t ly = 0; ly < im->layers; ly++)
                for (uint32_t lv = 0; lv < im->levels; lv++)
                    if (!samp_conflict[j] || !sub_is_attached(d, im, ly, lv))
                        re5vk_tbdr_mark_read(&d->tbdr, &im->tbdr_subs[(size_t)ly * im->levels + lv], amask);
        }
        if (!d->rendering && samp_conflict[j]) {
            conflict_subs(d, im, 1);
            continue;
        }
        if (!d->rendering) {
            VkPipelineStageFlags stg = samp_is_vs[j] ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                                     : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            /* Se a mesma imagem aparece no VS e no PS neste draw, acumula os dois estágios. */
            for (uint32_t m = 0; m < nsamp; m++)
                if (samp_im[m] == im)
                    stg |= samp_is_vs[m] ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            re5vk_img_transition(d, re5vk_cmd(d), im, 0, im->levels, 0, im->layers, samp_layout[j], stg);
        } else if (!samp_conflict[j] && im->sub_stages && stage_note_on()) {
            /* Passe ja aberto e imagem ja em SHADER_READ_ONLY: sem transicao, o estagio que a le (VS!) nao
             * entrava em sub_stages. A barreira de um upload/escrita posterior esperava so o FS, e o VS
             * deste draw (textura de vertice da animacao facial) lia a textura sendo reescrita: lixo no
             * cabelo e no rosto, so quando a cabeca aparece. RE5VK_STAGE_NOTE=0 desliga. */
            VkPipelineStageFlags stg = 0;
            for (uint32_t m = 0; m < nsamp; m++)
                if (samp_im[m] == im)
                    stg |= samp_is_vs[m] ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            for (uint32_t i = 0; i < im->layers * im->levels; i++)
                if (im->sub_layouts[i] == samp_layout[j]) im->sub_stages[i] |= (uint32_t)stg;
        }
    }

    begin_rendering_with_layouts(d, want_rt_l, want_ds_l);
    d->pass_draws++;

    VkCommandBuffer cb = re5vk_cmd(d);
    if (pipe != d->bound_pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        d->bound_pipe = pipe;
        /* Religa os vertex buffers a cada troca de pipeline: a Mali r38 perdia o binding >0 (cabecas com 2
         * streams). RE5VK_REBIND_VB=0 volta ao cache. */
        { static int rb = -1; if (rb < 0) { const char *e = getenv("RE5VK_REBIND_VB"); rb = !(e && *e == '0'); } if (rb) d->vb_valid = 0; }
    }
    set_dynamic(d, pt);

    uint32_t offs[2] = {(uint32_t)d->vs_ubo_off, (uint32_t)d->ps_ubo_off};
    if (d->bound_set != d->set || d->bound_vs_off != offs[0] || d->bound_ps_off != offs[1]) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->layout, 0, 1, &d->set, 2, offs);
        d->bound_set = d->set;
        d->bound_vs_off = offs[0];
        d->bound_ps_off = offs[1];
    }

    /* vértices */
    VkBuffer bufs[16];
    VkDeviceSize vo[16], strides[16];
    for (uint32_t s = 0; s < 15; s++) {
        vk_buffer *vb = d->streams[s].vb;
        bufs[s] = vb ? vb->buf : d->zero_vb;
        vo[s] = vb ? d->streams[s].off : 0;
        strides[s] = vb ? d->streams[s].stride : 0;
        if (vb) vb->last_use = d->serial;
    }
    bufs[15] = d->zero_vb;
    vo[15] = 0;
    strides[15] = 0;
    /* Só se algo mudou desde o último bind neste command buffer. */
    if (!d->vb_valid || memcmp(bufs, d->bound_vb, sizeof bufs) || memcmp(vo, d->bound_vo, sizeof vo) ||
        memcmp(strides, d->bound_vstride, sizeof strides)) {
        if (d->core) vkCmdBindVertexBuffers(cb, 0, 16, bufs, vo);   /* stride no pipeline */
        else vkCmdBindVertexBuffers2(cb, 0, 16, bufs, vo, NULL, strides);
        memcpy(d->bound_vb, bufs, sizeof bufs);
        memcpy(d->bound_vo, vo, sizeof vo);
        memcpy(d->bound_vstride, strides, sizeof strides);
        d->vb_valid = 1;
    }
}

static UINT vertex_count(D3DPRIMITIVETYPE t, UINT n)
{
    switch (t) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
    default: return 0;
    }
}

/* RE5VK_SKIP_REL=1 (diagnóstico): não desenha com VS que lê c[] por endereçamento relativo
 * (skinning). Se um defeito some junto, ele vem desse caminho. */
static int skip_rel_draw(const vk_device *d)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RE5VK_SKIP_REL");
        on = e && *e == '1';
        if (on) RE5_WARN("re5vk", "RE5VK_SKIP_REL=1: draws com endereçamento relativo no VS não são desenhados");
    }
    if (d->last_vsi && d->last_vsi->uses_rel && d->decl && rs_log_on()) {
        static uint32_t seen[16];
        static int nseen;
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < d->decl->n; i++) h = (h ^ (d->decl->el[i].Type | d->decl->el[i].Usage << 8)) * 16777619u;
        int dup = 0;
        for (int i = 0; i < nseen; i++) dup |= seen[i] == h;
        if (!dup && nseen < 16) {
            seen[nseen++] = h;
            char buf[512];
            int n = snprintf(buf, sizeof buf, "RS_LOG decl de skinning:");
            for (uint32_t i = 0; i < d->decl->n && n < 480; i++)
                n += snprintf(buf + n, sizeof buf - (size_t)n, " [s%u+%u tipo %u uso %u.%u]", d->decl->el[i].Stream,
                              d->decl->el[i].Offset, d->decl->el[i].Type, d->decl->el[i].Usage,
                              d->decl->el[i].UsageIndex);
            for (uint32_t s = 0; s < 4 && n < 480; s++)
                n += snprintf(buf + n, sizeof buf - (size_t)n, " {stream%u off %u stride %u vb %p base ?}", s, d->streams[s].off, d->streams[s].stride, (void *)d->streams[s].vb);
            RE5_INFO("re5vk", "%s", buf);
        }
    }
    if (rs_log_on() && d->last_psi && d->last_psi->uses_kill && d->ps) {
        static const D3DRENDERSTATETYPE wr0[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
                                                  D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};
        int w = 0;
        for (int i = 0; i < 4; i++)
            if (d->rt[i] && (d->rs[wr0[i]] & 0xF)) w = 1;
        static int nlog;
        if (!w && nlog < 30) {
            nlog++;
            RE5_INFO("re5vk", "RS_LOG draw sem cor com kill: ps %08X c0(%g %g %g %g) zwrite %u zfunc %u tex0 %p",
                     (unsigned)d->ps->hash, d->psf[0][0], d->psf[0][1], d->psf[0][2], d->psf[0][3],
                     (unsigned)d->rs[D3DRS_ZWRITEENABLE], (unsigned)d->rs[D3DRS_ZFUNC], (void *)d->tex[0]);
        }
    }
    if (on && d->last_vsi && d->last_vsi->uses_rel) return 1;
    if (small_target(d)) return 1;
    /* RE5VK_SKIP_VS=<hash>[,<hash>...] (DIAGNOSTICO): nao desenha com esses VS (hash FNV-1a do bytecode,
     * o mesmo nome de work/shaders/vs_<hash>.bin e do DUMP_HEAD). Isola um shader suspeito. */
    {
        static int nsv = -1;
        static uint32_t sv[8];
        if (nsv < 0) {
            nsv = 0;
            const char *e = getenv("RE5VK_SKIP_VS");
            while (e && *e && nsv < 8) {
                char *end;
                unsigned long h = strtoul(e, &end, 16);
                if (end == e) break;
                sv[nsv++] = (uint32_t)h;
                RE5_WARN("re5vk", "RE5VK_SKIP_VS: draws com o VS %08X nao sao desenhados", (unsigned)h);
                e = *end == ',' ? end + 1 : end;
            }
        }
        for (int i = 0; i < nsv && d->vs; i++)
            if (d->vs->hash == sv[i]) return 1;
    }
    /* RE5VK_SKIP_VTXTEX=1 (DIAGNOSTICO): nao desenha draws cujo VS le textura (cabecas com texldl). Se o
     * lixo some, ele vem desses draws; combine com RE5VK_VTX_ZERO=1 para separar textura de geometria. */
    {
        static int svt = -1;
        if (svt < 0) {
            const char *e = getenv("RE5VK_SKIP_VTXTEX");
            svt = e && *e == '1';
            if (svt) RE5_WARN("re5vk", "RE5VK_SKIP_VTXTEX=1: draws com textura no VS nao sao desenhados");
            const char *z = getenv("RE5VK_VTX_ZERO");
            RE5_INFO("re5vk", "diagnosticos de cabeca: VTX_ZERO=%s SKIP_VTXTEX=%d VTX_NEAREST=%s STAGE_NOTE=%s", z ? z : "(off)", svt,
                     getenv("RE5VK_VTX_NEAREST") ? getenv("RE5VK_VTX_NEAREST") : "(padrao 1)", getenv("RE5VK_STAGE_NOTE") ? getenv("RE5VK_STAGE_NOTE") : "(padrao 1)");
        }
        if (svt && d->last_vsi && d->last_vsi->samplers) return 1;
    }
    /* RE5VK_SKIP_NOCOLOR=1 (diagnóstico): não desenha draws que não escrevem cor (passada só de
     * profundidade). Se os retângulos pretos de folhas/cerca somem, a profundidade vem dali. */
    static int nc = -1, zpre = -1;
    if (nc < 0) {
        const char *e = getenv("RE5VK_SKIP_NOCOLOR");
        nc = e && *e == '1';
        if (nc) RE5_WARN("re5vk", "RE5VK_SKIP_NOCOLOR=1: draws sem escrita de cor não são desenhados");
        /* Passada só de profundidade (Z-prepass) no Mali: com ela as folhas, a cerca e o cabelo saíam
         * com retângulos pretos (a profundidade do quad inteiro ficava gravada); sem ela, certo (A54,
         * 2026-11-18, RE5VK_SKIP_NOCOLOR=1). No Mali ela também não ganha nada: a GPU já descarta os
         * pixels escondidos (FPK). A passada de cor grava a própria profundidade (LESSEQUAL), e o
         * backend não tem consulta de oclusão. RE5VK_ZPREPASS=1 mantém; =0 pula em qualquer GPU. */
        /* 1 = passada inteira; 0 = sem ela (personagens apareciam através das paredes: algumas só
         * gravam profundidade aqui); 2 = sem os draws dela cujo PS descarta pixel (folhas, cerca,
         * cabelo: texkill), que no Mali gravavam o quad inteiro. Padrão 1: RE5VK_KILL_DEPTH ataca a causa. */
        const char *z = getenv("RE5VK_ZPREPASS");
        zpre = z && *z ? (*z - '0') : 2;   /* padrao 2: KILL_DEPTH ficou desligado (suspeito de hang) */
        if (zpre != 1) RE5_INFO("re5vk", "passada só de profundidade: modo %d (RE5VK_ZPREPASS=1 inteira, 2 sem os draws com texkill, 0 nenhuma)", zpre);
    }
    if (!nc && zpre != 1 && (zpre == 0 || (d->last_psi && d->last_psi->uses_kill)) && d->rs[D3DRS_ZWRITEENABLE] &&
        !d->rs[D3DRS_STENCILENABLE]) {
        static const D3DRENDERSTATETYPE wz[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
                                                 D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};
        int writes = 0;
        for (int i = 0; i < 4; i++)
            if (d->rt[i] && (d->rs[wz[i]] & 0xF)) writes = 1;
        if (!writes) return 1;
    }
    if (nc) {
        static const D3DRENDERSTATETYPE wr[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
                                                 D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};
        int writes = 0;
        for (int i = 0; i < 4; i++)
            if (d->rt[i] && (d->rs[wr[i]] & 0xF)) writes = 1;
        if (!writes) {
            static int said;
            if (!said++) RE5_WARN("re5vk", "RE5VK_SKIP_NOCOLOR: primeiro draw sem cor pulado");
            return 1;
        }
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE v_DrawPrimitive(IDirect3DDevice9 *self, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!n) return D3D_OK;
    re5vk_lock(d);
    prepare(d, t);
    if (skip_rel_draw(d)) {
        re5vk_unlock(d);
        return D3D_OK;
    }
    vkCmdDraw(re5vk_cmd(d), vertex_count(t, n), 1, start, 0);
    re5vk_unlock(d);
    return D3D_OK;
}

/* RE5VK_DUMP_HEAD=1 (DIAGNOSTICO): nos primeiros draws de skinning com 2 streams, loga parametros do draw,
 * se o fetch de vertice passa do tamanho do buffer, bytes crus dos primeiros/ultimos vertices e as
 * constantes c0..c3 e c23..c34. Compare com o mesmo quadro no PC. */
static void dump_head(vk_device *d, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT numv, UINT start, UINT n)
{
    static int on = -1, done;
    if (on < 0) { const char *e = getenv("RE5VK_DUMP_HEAD"); on = e && *e == '1'; }
    if (!on || done >= 4 || !d->last_vsi || !d->last_vsi->uses_rel || !d->decl) return;
    int two = 0;
    for (uint32_t i = 0; i < d->decl->n; i++) if (d->decl->el[i].Stream == 1) two = 1;
    if (!two) return;
    done++;
    RE5_INFO("re5vk", "DUMP_HEAD #%d vs %08X ps %08X base %d minv %u numv %u start %u n %u ibfmt %u ibsize %u", done,
             d->vs ? (unsigned)d->vs->hash : 0u, d->ps ? (unsigned)d->ps->hash : 0u, (int)base, minv, numv, start, n,
             (unsigned)d->ib->format, (unsigned)d->ib->size);
    /* maior índice usado pelo draw (o fetch vai até ele, não até minv+numv) */
    if (d->ib->mem.map && (t == D3DPT_TRIANGLELIST || t == D3DPT_TRIANGLESTRIP)) {
        const int i32 = d->ib->format == D3DFMT_INDEX32;
        const uint32_t isz = i32 ? 4u : 2u;
        uint64_t cnt = t == D3DPT_TRIANGLELIST ? (uint64_t)n * 3u : (uint64_t)n + 2u, first = (uint64_t)start;
        uint32_t mx = 0, mn = UINT32_MAX;
        if ((first + cnt) * isz <= d->ib->size) {
            for (uint64_t k = first; k < first + cnt; k++) {
                uint32_t v = i32 ? ((const uint32_t *)d->ib->mem.map)[k] : ((const uint16_t *)d->ib->mem.map)[k];
                if (v > mx) mx = v;
                if (v < mn) mn = v;
            }
            RE5_INFO("re5vk", "DUMP_HEAD indices (tipo %d) min %u max %u (minv+numv-1 = %u)%s", (int)t, mn, mx,
                     minv + numv - 1, mx >= minv + numv ? " INDICE ACIMA DE NUMV" : "");
            for (uint32_t s = 0; s < 2; s++) {
                vk_buffer *vb = d->streams[s].vb;
                if (!vb) continue;
                uint64_t end = (uint64_t)d->streams[s].off + ((uint64_t)((int64_t)base + mx) + 1u) * d->streams[s].stride;
                RE5_INFO("re5vk", "DUMP_HEAD stream%u pelo maior indice: fim %llu vbsize %u %s", s,
                         (unsigned long long)end, vb->size, end > vb->size ? "FORA DO BUFFER" : "dentro");
            }
        }
    }
    for (int r = 4; r < 16; r++) RE5_INFO("re5vk", "DUMP_HEAD c%d: %g %g %g %g", r, d->vsf[r][0], d->vsf[r][1], d->vsf[r][2], d->vsf[r][3]);
    for (uint32_t s = 0; s < 2; s++) {
        vk_buffer *vb = d->streams[s].vb;
        if (!vb) continue;
        uint64_t first = (uint64_t)d->streams[s].off + (uint64_t)((int64_t)base + minv) * d->streams[s].stride;
        uint64_t last = (uint64_t)d->streams[s].off + (uint64_t)((int64_t)base + minv + numv) * d->streams[s].stride;
        RE5_INFO("re5vk", "DUMP_HEAD stream%u off %u stride %u vbsize %u primeiro %llu ultimo_fim %llu %s", s, d->streams[s].off,
                 d->streams[s].stride, vb->size, (unsigned long long)first, (unsigned long long)last,
                 last > vb->size ? "FORA DO BUFFER" : "dentro");
        const uint8_t *m = (const uint8_t *)vb->mem.map;
        if (!m || first + d->streams[s].stride * 4 > vb->size) continue;
        for (int v = 0; v < 4; v++) {
            char buf[200]; int k = 0;
            for (uint32_t b = 0; b < d->streams[s].stride && b < 32; b++) k += snprintf(buf + k, sizeof buf - (size_t)k, "%02X", m[first + (uint64_t)v * d->streams[s].stride + b]);
            RE5_INFO("re5vk", "DUMP_HEAD s%u v%d: %s", s, v, buf);
        }
    }
    for (int r = 0; r < 4; r++) RE5_INFO("re5vk", "DUMP_HEAD c%d: %g %g %g %g", r, d->vsf[r][0], d->vsf[r][1], d->vsf[r][2], d->vsf[r][3]);
    for (int r = 23; r < 35; r++) RE5_INFO("re5vk", "DUMP_HEAD c%d: %g %g %g %g", r, d->vsf[r][0], d->vsf[r][1], d->vsf[r][2], d->vsf[r][3]);
}

static HRESULT STDMETHODCALLTYPE v_DrawIndexedPrimitive(IDirect3DDevice9 *self, D3DPRIMITIVETYPE t, INT base,
                                                        UINT minv, UINT numv, UINT start, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!n) return D3D_OK;
    if (!d->ib) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    prepare(d, t);
    if (skip_rel_draw(d)) {
        re5vk_unlock(d);
        return D3D_OK;
    }
    dump_head(d, t, base, minv, numv, start, n);
    VkCommandBuffer cb = re5vk_cmd(d);
    VkIndexType it = d->ib->format == D3DFMT_INDEX32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (d->ib->buf != d->bound_ib || it != d->bound_ibt) {   /* o VkBuffer muda num Lock com DISCARD */
        vkCmdBindIndexBuffer(cb, d->ib->buf, 0, it);
        d->bound_ib = d->ib->buf;
        d->bound_ibt = it;
    }
    d->ib->last_use = d->serial;
    vkCmdDrawIndexed(cb, vertex_count(t, n), 1, start, base, 0);
    re5vk_unlock(d);
    return D3D_OK;
}

/* ---- StretchRect ---- */

static HRESULT STDMETHODCALLTYPE v_StretchRect(IDirect3DDevice9 *self, IDirect3DSurface9 *src, const RECT *sr,
                                               IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE filter)
{
    vk_device *d = (vk_device *)self;
    vk_surface *s = (vk_surface *)src, *t = (vk_surface *)dst;
    if (!s || !t) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    re5vk_end_rendering(d);
    uint8_t s_amask = s->img->fmt->depth
                          ? (uint8_t)(RE5VK_TBDR_DEPTH | (s->img->fmt->stencil ? RE5VK_TBDR_STENCIL : 0u))
                          : RE5VK_TBDR_COLOR;
    re5vk_tbdr_mark_read(&d->tbdr, &s->img->tbdr_subs[(size_t)s->layer * s->img->levels + s->level], s_amask);
    RECT a = sr ? *sr : (RECT){0, 0, (LONG)s->desc.Width, (LONG)s->desc.Height};
    RECT b = dr ? *dr : (RECT){0, 0, (LONG)t->desc.Width, (LONG)t->desc.Height};
    VkCommandBuffer cb = re5vk_cmd(d);
    int gen = re5vk_layout_general();
    int same_sub = s->img == t->img && s->level == t->level && s->layer == t->layer;
    VkImageLayout sl = (gen || same_sub) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageLayout tl = (gen || same_sub) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    re5vk_img_transition(d, cb, s->img, s->level, 1, s->layer, 1, sl, 0);
    if (!same_sub) re5vk_img_transition(d, cb, t->img, t->level, 1, t->layer, 1, tl, 0);
    VkImageAspectFlags asp = s->img->fmt->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (s->img->fmt->stencil && t->img->fmt->stencil) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    int same = a.right - a.left == b.right - b.left && a.bottom - a.top == b.bottom - b.top &&
               s->img->vkf == t->img->vkf;
    if (same) {
        VkImageCopy c = {{asp, s->level, s->layer, 1}, {a.left, a.top, 0}, {asp, t->level, t->layer, 1},
                         {b.left, b.top, 0}, {(uint32_t)(a.right - a.left), (uint32_t)(a.bottom - a.top), 1}};
        vkCmdCopyImage(cb, s->img->image, sl, t->img->image, tl, 1, &c);
    } else {
        if (s->img->fmt->depth) re5_fatal("re5vk", "StretchRect de profundidade com escala sem implementação");
        VkImageBlit bl = {{asp, s->level, s->layer, 1}, {{a.left, a.top, 0}, {a.right, a.bottom, 1}},
                          {asp, t->level, t->layer, 1}, {{b.left, b.top, 0}, {b.right, b.bottom, 1}}};
        vkCmdBlitImage(cb, s->img->image, sl, t->img->image, tl, 1, &bl,
                       filter == D3DTEXF_POINT || filter == D3DTEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    re5vk_img_mark_written(t->img, t->level, 1, t->layer, 1);
    re5vk_tbdr_mark_external_write(&t->img->tbdr_subs[(size_t)t->layer * t->img->levels + t->level],
                                   RE5VK_TBDR_COLOR | RE5VK_TBDR_DEPTH | RE5VK_TBDR_STENCIL);
    s->img->last_use = t->img->last_use = d->serial;
    re5vk_unlock(d);
    return D3D_OK;
}

void re5vk_draw_destroy(vk_device *d)
{
    free(d->set_cache);
    d->set_cache = NULL;
    d->set_cache_cap = d->set_cache_n = 0;
}

void re5vk_draw_vtable(struct IDirect3DDevice9Vtbl *vt)
{
    vt->SetRenderTarget = v_SetRenderTarget;
    vt->GetRenderTarget = v_GetRenderTarget;
    vt->SetDepthStencilSurface = v_SetDepthStencilSurface;
    vt->GetDepthStencilSurface = v_GetDepthStencilSurface;
    vt->SetViewport = v_SetViewport;
    vt->GetViewport = v_GetViewport;
    vt->SetScissorRect = v_SetScissorRect;
    vt->GetScissorRect = v_GetScissorRect;
    vt->Clear = v_Clear;
    vt->SetRenderState = v_SetRenderState;
    vt->GetRenderState = v_GetRenderState;
    vt->SetSamplerState = v_SetSamplerState;
    vt->GetSamplerState = v_GetSamplerState;
    vt->SetTexture = v_SetTexture;
    vt->GetTexture = v_GetTexture;
    vt->SetStreamSource = v_SetStreamSource;
    vt->GetStreamSource = v_GetStreamSource;
    vt->SetIndices = v_SetIndices;
    vt->SetVertexDeclaration = v_SetVertexDeclaration;
    vt->GetVertexDeclaration = v_GetVertexDeclaration;
    vt->SetVertexShader = v_SetVertexShader;
    vt->GetVertexShader = v_GetVertexShader;
    vt->SetPixelShader = v_SetPixelShader;
    vt->GetPixelShader = v_GetPixelShader;
    vt->SetVertexShaderConstantF = v_SetVSConstF;
    vt->SetPixelShaderConstantF = v_SetPSConstF;
    vt->SetVertexShaderConstantI = v_SetVSConstI;
    vt->SetPixelShaderConstantI = v_SetPSConstI;
    vt->SetVertexShaderConstantB = v_SetVSConstB;
    vt->SetPixelShaderConstantB = v_SetPSConstB;
    vt->SetClipPlane = v_SetClipPlane;
    vt->DrawPrimitive = v_DrawPrimitive;
    vt->DrawIndexedPrimitive = v_DrawIndexedPrimitive;
    vt->StretchRect = v_StretchRect;
}
