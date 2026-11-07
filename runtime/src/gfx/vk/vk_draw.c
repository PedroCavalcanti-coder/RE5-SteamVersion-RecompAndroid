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
    d->pending_clear.on = 0;
    d->pipe_gen++;
}

/* ---- passes de render ---- */

static void begin_rendering(vk_device *d)
{
    if (d->rendering) return;
    VkRenderingAttachmentInfo col[4];
    uint32_t ncol = 0;
    VkExtent2D area = {0xFFFFFFFFu, 0xFFFFFFFFu};
    for (uint32_t i = 0; i < 4; i++) {
        memset(&col[i], 0, sizeof col[i]);
        col[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        col[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        col[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        vk_surface *s = d->rt[i];
        if (!s) continue;
        col[i].imageView = re5vk_image_rt_view(d, s->img, s->layer, s->level);
        if (d->pending_clear.on && (d->pending_clear.flags & D3DCLEAR_TARGET)) {
            col[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            memcpy(col[i].clearValue.color.float32, d->pending_clear.color[0], 16);
        }
        if (s->desc.Width < area.width) area.width = s->desc.Width;
        if (s->desc.Height < area.height) area.height = s->desc.Height;
        s->img->last_use = d->serial;
        ncol = i + 1;
    }
    VkRenderingAttachmentInfo dep = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sten = dep;
    vk_surface *ds = d->ds;
    if (ds) {
        dep.imageView = re5vk_image_rt_view(d, ds->img, ds->layer, ds->level);
        dep.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sten = dep;
        if (d->pending_clear.on && (d->pending_clear.flags & D3DCLEAR_ZBUFFER)) {
            dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            dep.clearValue.depthStencil.depth = d->pending_clear.z;
        }
        if (d->pending_clear.on && (d->pending_clear.flags & D3DCLEAR_STENCIL)) {
            sten.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            sten.clearValue.depthStencil.stencil = d->pending_clear.stencil;
        }
        if (ds->desc.Width < area.width) area.width = ds->desc.Width;
        if (ds->desc.Height < area.height) area.height = ds->desc.Height;
        ds->img->last_use = d->serial;
    }
    if (area.width == 0xFFFFFFFFu) re5_fatal("re5vk", "draw sem render target nem profundidade");
    d->pending_clear.on = 0;
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, area},
        .layerCount = 1,
        .colorAttachmentCount = ncol,
        .pColorAttachments = col,
        .pDepthAttachment = ds ? &dep : NULL,
        .pStencilAttachment = ds && ds->img->fmt->stencil ? &sten : NULL,
    };
    vkCmdBeginRendering(re5vk_cmd(d), &ri);
    d->rendering = 1;
    d->render_area = area;
    d->dyn_dirty = 1;
}

void re5vk_end_rendering(vk_device *d)
{
    if (!d->rendering && d->pending_clear.on) begin_rendering(d);   /* um Clear sem draw: aplica */
    if (!d->rendering) return;
    vkCmdEndRendering(re5vk_cmd(d));
    re5vk_barrier_all(re5vk_cmd(d));   /* o que o passe escreveu fica visível para amostrar e copiar */
    d->rendering = 0;
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
        re5vk_end_rendering(d);
        set_obj((vk_obj **)&d->rt[i], &s->o);
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
        re5vk_end_rendering(d);
        set_obj((vk_obj **)&d->ds, s ? &s->o : NULL);
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

static HRESULT STDMETHODCALLTYPE v_Clear(IDirect3DDevice9 *self, DWORD count, const D3DRECT *rects, DWORD flags,
                                         D3DCOLOR color, float z, DWORD stencil)
{
    vk_device *d = (vk_device *)self;
    if (!d->ds) flags &= ~(DWORD)(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (d->ds && !d->ds->img->fmt->stencil) flags &= ~(DWORD)D3DCLEAR_STENCIL;
    if (!flags) return D3D_OK;
    re5vk_lock(d);
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

static HRESULT STDMETHODCALLTYPE v_SetRenderState(IDirect3DDevice9 *self, D3DRENDERSTATETYPE s, DWORD v)
{
    vk_device *d = (vk_device *)self;
    if ((uint32_t)s >= 256) return D3DERR_INVALIDCALL;
    if (d->rs[s] == v) return D3D_OK;
    d->rs[s] = v;
    d->dyn_dirty = 1;
    d->pipe_gen++;
    if (s == D3DRS_ALPHAREF) d->ps_dirty = 1;
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

static VkSampler sampler_get(vk_device *d, int s, int compare)
{
    uint32_t key[14];
    memcpy(key, d->ss[s], 13 * 4);
    key[13] = (uint32_t)compare;
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
        .magFilter = filter_of(st[D3DSAMP_MAGFILTER]),
        .minFilter = filter_of(st[D3DSAMP_MINFILTER]),
        .mipmapMode = st[D3DSAMP_MIPFILTER] == D3DTEXF_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                              : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address_of(st[D3DSAMP_ADDRESSU]),
        .addressModeV = address_of(st[D3DSAMP_ADDRESSV]),
        .addressModeW = address_of(st[D3DSAMP_ADDRESSW]),
        .mipLodBias = f_of(st[D3DSAMP_MIPMAPLODBIAS]),
        .anisotropyEnable = aniso && maxa > 1.0f,
        .maxAnisotropy = maxa < 1.0f ? 1.0f : maxa,
        .compareEnable = compare ? VK_TRUE : VK_FALSE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .minLod = minlod,
        .maxLod = st[D3DSAMP_MIPFILTER] == D3DTEXF_NONE ? minlod + 0.25f : VK_LOD_CLAMP_NONE,
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

/* ---- pipelines ---- */

static VkFormat vertex_format(BYTE type);

/* Formato do atributo para a GPU. Os *SCALED (inteiro lido como float) são opcionais no Vulkan e
 * o Adreno não os tem: nesse caso o atributo vai como UINT/SINT e o VS converte (variante
 * in_uint/in_sint). *intk: 0 float, 1 sem sinal, 2 com sinal. */
static VkFormat vertex_format_dev(vk_device *d, BYTE type, int *intk)
{
    VkFormat vf = vertex_format(type), alt = VK_FORMAT_UNDEFINED;
    int k = 0;
    *intk = 0;
    switch (type) {
    case D3DDECLTYPE_UBYTE4: alt = VK_FORMAT_R8G8B8A8_UINT; k = 1; break;
    case D3DDECLTYPE_SHORT2: alt = VK_FORMAT_R16G16_SINT; k = 2; break;
    case D3DDECLTYPE_SHORT4: alt = VK_FORMAT_R16G16B16A16_SINT; k = 2; break;
    case D3DDECLTYPE_UDEC3: alt = VK_FORMAT_A2B10G10R10_UINT_PACK32; k = 1; break;
    default: return vf;
    }
    static int8_t scaled_ok[32];   /* por D3DDECLTYPE: 0 não sabido, 1 sim, -1 não (um device só) */
    if (!scaled_ok[type]) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(d->phys, vf, &fp);
        scaled_ok[type] = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : -1;
        if (scaled_ok[type] < 0)
            RE5_INFO("re5vk", "D3DDECLTYPE %u: sem o formato SCALED %d, vai como inteiro (%d) e o VS converte",
                     (unsigned)type, (int)vf, (int)alt);
    }
    if (scaled_ok[type] > 0) return vf;
    *intk = k;
    return alt;
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
    uint32_t topo_class;        /* 0 pontos, 1 linhas, 2 triângulos */
    uint32_t fill;
    uint32_t blend[6];          /* enable, src, dst, op, srca, dsta/opa empacotados */
    uint32_t write[4];
    VkFormat color[4], depth;
    uint8_t stencil;
} pipe_key;

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

static VkPipeline pipeline_create(vk_device *d, const pipe_key *k, VkShaderModule vsm, const re5vk_shader *vsi,
                                  VkShaderModule psm)
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
        if (intk != ((k->vsv.in_uint >> r & 1) ? 1 : (k->vsv.in_sint >> r & 1) ? 2 : 0))
            re5_fatal("re5vk", "v%u: variante do VS não bate com o formato de vértice", r);
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
        if (used_streams >> s & 1) bind[nb++] = (VkVertexInputBindingDescription){s, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    if (need_zero) bind[nb++] = (VkVertexInputBindingDescription){15, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = bind,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = attr};
    static const VkPrimitiveTopology cls[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = cls[k->topo_class]};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = k->fill == D3DFILL_WIREFRAME ? VK_POLYGON_MODE_LINE
                     : k->fill == D3DFILL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
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
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof dyn / sizeof dyn[0], .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                           .colorAttachmentCount = ncol, .pColorAttachmentFormats = k->color,
                                           .depthAttachmentFormat = k->depth,
                                           .stencilAttachmentFormat = k->stencil ? k->depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rinfo,
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
    vkCmdSetPrimitiveTopology(cb, topology(pt));
    if (!d->dyn_dirty) return;
    d->dyn_dirty = 0;
    VkViewport v = {(float)d->vp.X + 0.5f, (float)(d->vp.Y + d->vp.Height) + 0.5f, (float)d->vp.Width,
                    -(float)d->vp.Height, d->vp.MinZ, d->vp.MaxZ};
    vkCmdSetViewport(cb, 0, 1, &v);
    VkRect2D sc = {{0, 0}, d->render_area};
    if (RS(d, D3DRS_SCISSORTESTENABLE)) {
        sc = (VkRect2D){{d->scissor.left, d->scissor.top},
                        {(uint32_t)(d->scissor.right > d->scissor.left ? d->scissor.right - d->scissor.left : 0),
                         (uint32_t)(d->scissor.bottom > d->scissor.top ? d->scissor.bottom - d->scissor.top : 0)}};
        clamp_rect(&sc, d->render_area);
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    /* cull: na tela (y para baixo nos dois) a ordem é a do D3D9 */
    DWORD cull = RS(d, D3DRS_CULLMODE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetCullMode(cb, cull == D3DCULL_CCW ? VK_CULL_MODE_BACK_BIT : cull == D3DCULL_CW ? VK_CULL_MODE_FRONT_BIT
                                                                                         : VK_CULL_MODE_NONE);
    int has_depth = d->ds != NULL;
    vkCmdSetDepthTestEnable(cb, has_depth && RS(d, D3DRS_ZENABLE) != D3DZB_FALSE);
    vkCmdSetDepthWriteEnable(cb, has_depth && RS(d, D3DRS_ZENABLE) != D3DZB_FALSE && RS(d, D3DRS_ZWRITEENABLE));
    vkCmdSetDepthCompareOp(cb, cmp_op(RS(d, D3DRS_ZFUNC)));
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    int stencil = has_depth && d->ds->img->fmt->stencil && RS(d, D3DRS_STENCILENABLE);
    vkCmdSetStencilTestEnable(cb, stencil);
    VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
    vkCmdSetStencilOp(cb, RS(d, D3DRS_TWOSIDEDSTENCILMODE) ? VK_STENCIL_FACE_FRONT_BIT : both,
                      stencil_op(RS(d, D3DRS_STENCILFAIL)), stencil_op(RS(d, D3DRS_STENCILPASS)),
                      stencil_op(RS(d, D3DRS_STENCILZFAIL)), cmp_op(RS(d, D3DRS_STENCILFUNC)));
    if (RS(d, D3DRS_TWOSIDEDSTENCILMODE))
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op(RS(d, D3DRS_CCW_STENCILFAIL)),
                          stencil_op(RS(d, D3DRS_CCW_STENCILPASS)), stencil_op(RS(d, D3DRS_CCW_STENCILZFAIL)),
                          cmp_op(RS(d, D3DRS_CCW_STENCILFUNC)));
    vkCmdSetStencilCompareMask(cb, both, RS(d, D3DRS_STENCILMASK));
    vkCmdSetStencilWriteMask(cb, both, RS(d, D3DRS_STENCILWRITEMASK));
    vkCmdSetStencilReference(cb, both, RS(d, D3DRS_STENCILREF));
    /* depth bias: o do D3D9 é em unidades de profundidade; o do Vulkan, no menor degrau do formato */
    float bias = f_of(RS(d, D3DRS_DEPTHBIAS)), slope = f_of(RS(d, D3DRS_SLOPESCALEDEPTHBIAS));
    int bias_on = has_depth && (bias != 0.0f || slope != 0.0f);
    vkCmdSetDepthBiasEnable(cb, bias_on);
    if (bias_on) {
        VkFormat df = d->ds->img->vkf;
        float r = df == VK_FORMAT_D16_UNORM ? 65535.0f : (df == VK_FORMAT_D24_UNORM_S8_UINT ? 16777215.0f : 8388608.0f);
        vkCmdSetDepthBias(cb, bias * r, 0.0f, slope);
    }
    DWORD bf = RS(d, D3DRS_BLENDFACTOR);
    float bc[4] = {(float)((bf >> 16) & 0xFF) / 255.0f, (float)((bf >> 8) & 0xFF) / 255.0f, (float)(bf & 0xFF) / 255.0f,
                   (float)(bf >> 24) / 255.0f};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
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

static void prepare(vk_device *d, D3DPRIMITIVETYPE pt)
{
    if (!d->vs || !d->ps) re5_fatal("re5vk", "draw sem vertex/pixel shader (fixed function sem implementação)");
    if (RS(d, D3DRS_CLIPPLANEENABLE) && !d->warned_clip) {
        RE5_WARN("re5vk", "D3DRS_CLIPPLANEENABLE=0x%X: planos de recorte não aplicados (como a DXVK no Mali)",
                 (unsigned)RS(d, D3DRS_CLIPPLANEENABLE));
        d->warned_clip = 1;
    }
    if (RS(d, D3DRS_SRGBWRITEENABLE)) re5_fatal("re5vk", "D3DRS_SRGBWRITEENABLE sem implementação");
    begin_rendering(d);
    /* Mesmo estado de pipeline do draw anterior (pipe_gen e topologia): sem variantes, chave
     * nem busca (eram ~90 mil por segundo na thread de render). */
    re5vk_dxso_variant psv;
    const re5vk_shader *vsi, *psi;
    VkPipeline pipe;
    uint32_t tc = topo_class(pt);
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
        }
        if (vsv.in_uint || vsv.in_sint) re5vk_shader_variant(d, d->vs, &vsv, &vsm, &vsi);
        /* primeiro sem as profundidades, para saber os samplers do PS */
        psv.alpha_func = RS(d, D3DRS_ALPHATESTENABLE) ? (uint8_t)RS(d, D3DRS_ALPHAFUNC) : 0;
        if (psv.alpha_func == D3DCMP_ALWAYS) psv.alpha_func = 0;
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
        if (!*pp) *pp = pipeline_create(d, &k, vsm, vsi, psm);
        pipe = *pp;
        d->last_pipe = pipe;
        d->last_gen = d->pipe_gen;
        d->last_topo = tc;
        d->last_vsi = vsi;
        d->last_psi = psi;
        d->last_psv = psv;
    }
    VkCommandBuffer cb = re5vk_cmd(d);
    if (pipe != d->bound_pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        d->bound_pipe = pipe;
    }
    set_dynamic(d, pt);

    /* constantes */
    /* Só o que o shader lê (vsi/psi->const_f, uses_i, uses_b): antes eram os bancos inteiros,
     * ~8 KB por draw com ~1000 draws por quadro, na thread de render que é o caminho crítico
     * (RE5_PROFILE, 2026-11-03). A região nova é refeita também quando o shader atual lê mais do
     * que a região corrente recebeu (troca de shader sem constante nova). */
    uint8_t vs_ib = (uint8_t)(vsi->uses_i | vsi->uses_b << 1), ps_ib = (uint8_t)(psi->uses_i | psi->uses_b << 1);
    if (d->vs_dirty || vsi->const_f > d->vs_ubo_f || (vs_ib & ~d->vs_ubo_ib)) {
        uint8_t *u = re5vk_ubo(d, RE5VK_VS_UBO_SIZE, &d->vs_ubo_buf, &d->vs_ubo_off);
        memcpy(u, d->vsf, (size_t)vsi->const_f * 16u);
        if (vsi->uses_i) memcpy(u + RE5VK_VS_UBO_I, d->vsi, sizeof d->vsi);
        if (vsi->uses_b) memcpy(u + RE5VK_VS_UBO_B, &d->vsb, 4);
        d->vs_ubo_f = vsi->const_f;
        d->vs_ubo_ib = vs_ib;
        d->vs_dirty = 0;
    }
    if (d->ps_dirty || psi->const_f > d->ps_ubo_f || (ps_ib & ~d->ps_ubo_ib)) {
        uint8_t *u = re5vk_ubo(d, RE5VK_PS_UBO_SIZE, &d->ps_ubo_buf, &d->ps_ubo_off);
        memcpy(u, d->psf, (size_t)psi->const_f * 16u);
        if (psi->uses_i) memcpy(u + RE5VK_PS_UBO_I, d->psi, sizeof d->psi);
        if (psi->uses_b) memcpy(u + RE5VK_PS_UBO_B, &d->psb, 4);
        float aref = (float)(RS(d, D3DRS_ALPHAREF) & 0xFF) / 255.0f;
        memcpy(u + RE5VK_PS_UBO_AREF, &aref, 4);
        d->ps_ubo_f = psi->const_f;
        d->ps_ubo_ib = ps_ib;
        d->ps_dirty = 0;
    }
    /* descritores: reaproveitados enquanto texturas, samplers e shaders não mudam */
    static VkBuffer last_vs_buf, last_ps_buf;
    if (d->desc_dirty || !d->set || last_vs_buf != d->vs_ubo_buf || last_ps_buf != d->ps_ubo_buf) {
        d->set = re5vk_alloc_set(d);
        VkWriteDescriptorSet w[RE5VK_NBINDINGS];
        VkDescriptorBufferInfo bi[2] = {{d->vs_ubo_buf, 0, RE5VK_VS_UBO_SIZE}, {d->ps_ubo_buf, 0, RE5VK_PS_UBO_SIZE}};
        VkDescriptorImageInfo ii[20];
        uint32_t nw = 0;
        for (int i = 0; i < 2; i++)
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set,
                                             .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                             .pBufferInfo = &bi[i]};
        uint32_t ni = 0;
        for (int s = 0; s < 16; s++) {
            if (!(psi->samplers >> s & 1)) continue;
            int dep;
            vk_image *im = bound_image(d, s, psi->sampler_type[s], &dep);
            ii[ni] = (VkDescriptorImageInfo){sampler_get(d, s, dep && (psv.depth_samplers >> s & 1)),
                                             re5vk_image_sampled_view(d, im, d->ss[s][D3DSAMP_SRGBTEXTURE] != 0),
                                             VK_IMAGE_LAYOUT_GENERAL};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set,
                                             .dstBinding = RE5VK_BIND_PS_SAMP + (uint32_t)s, .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &ii[ni++]};
        }
        for (int s = 0; s < 4; s++) {
            if (!(vsi->samplers >> s & 1)) continue;
            int dep;
            vk_image *im = bound_image(d, 16 + s, vsi->sampler_type[s], &dep);
            ii[ni] = (VkDescriptorImageInfo){sampler_get(d, 16 + s, 0),
                                             re5vk_image_sampled_view(d, im, d->ss[16 + s][D3DSAMP_SRGBTEXTURE] != 0),
                                             VK_IMAGE_LAYOUT_GENERAL};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set,
                                             .dstBinding = RE5VK_BIND_VS_SAMP + (uint32_t)s, .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &ii[ni++]};
        }
        vkUpdateDescriptorSets(d->dev, nw, w, 0, NULL);
        last_vs_buf = d->vs_ubo_buf;
        last_ps_buf = d->ps_ubo_buf;
        d->desc_dirty = 0;
    } else {
        /* os mesmos buffers e texturas: só os offsets mudam; as texturas continuam em uso */
        for (int s = 0; s < 16; s++)
            if (d->tex[s] && d->tex[s]->kind != VKO_SURFACE) ((vk_texture *)d->tex[s])->img.last_use = d->serial;
    }
    uint32_t offs[2] = {(uint32_t)d->vs_ubo_off, (uint32_t)d->ps_ubo_off};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->layout, 0, 1, &d->set, 2, offs);

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
        vkCmdBindVertexBuffers2(cb, 0, 16, bufs, vo, NULL, strides);
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

static HRESULT STDMETHODCALLTYPE v_DrawPrimitive(IDirect3DDevice9 *self, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!n) return D3D_OK;
    re5vk_lock(d);
    prepare(d, t);
    vkCmdDraw(re5vk_cmd(d), vertex_count(t, n), 1, start, 0);
    re5vk_unlock(d);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE v_DrawIndexedPrimitive(IDirect3DDevice9 *self, D3DPRIMITIVETYPE t, INT base,
                                                        UINT minv, UINT numv, UINT start, UINT n)
{
    vk_device *d = (vk_device *)self;
    if (!n) return D3D_OK;
    if (!d->ib) return D3DERR_INVALIDCALL;
    re5vk_lock(d);
    prepare(d, t);
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
    RECT a = sr ? *sr : (RECT){0, 0, (LONG)s->desc.Width, (LONG)s->desc.Height};
    RECT b = dr ? *dr : (RECT){0, 0, (LONG)t->desc.Width, (LONG)t->desc.Height};
    VkCommandBuffer cb = re5vk_cmd(d);
    re5vk_barrier_all(cb);
    VkImageAspectFlags asp = s->img->fmt->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (s->img->fmt->stencil && t->img->fmt->stencil) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    int same = a.right - a.left == b.right - b.left && a.bottom - a.top == b.bottom - b.top &&
               s->img->vkf == t->img->vkf;
    if (same) {
        VkImageCopy c = {{asp, s->level, s->layer, 1}, {a.left, a.top, 0}, {asp, t->level, t->layer, 1},
                         {b.left, b.top, 0}, {(uint32_t)(a.right - a.left), (uint32_t)(a.bottom - a.top), 1}};
        vkCmdCopyImage(cb, s->img->image, VK_IMAGE_LAYOUT_GENERAL, t->img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        if (s->img->fmt->depth) re5_fatal("re5vk", "StretchRect de profundidade com escala sem implementação");
        VkImageBlit bl = {{asp, s->level, s->layer, 1}, {{a.left, a.top, 0}, {a.right, a.bottom, 1}},
                          {asp, t->level, t->layer, 1}, {{b.left, b.top, 0}, {b.right, b.bottom, 1}}};
        vkCmdBlitImage(cb, s->img->image, VK_IMAGE_LAYOUT_GENERAL, t->img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       filter == D3DTEXF_POINT || filter == D3DTEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    re5vk_barrier_all(cb);
    s->img->last_use = t->img->last_use = d->serial;
    re5vk_unlock(d);
    return D3D_OK;
}

void re5vk_draw_destroy(vk_device *d) { (void)d; }

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
