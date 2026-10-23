/*
 * coldebug.c - linhas de colisão do Havok por cima do quadro (ver re5/coldebug.h).
 *
 * Layouts do Havok 5.5 do RE5 (x86, medidos num despejo da memória; scripts/hk_export.py e
 * docs/AUDITORIA_GHIDRA.md):
 *   hkpRigidBody          +0x10 forma, +0xD0 vtable do movimento (hkpFixedRigidMotion = estático),
 *                         transformação: 3 colunas de rotação em +0xE0 (16 B cada), translação em +0x110
 *   hkpSimpleShapePhantom +0x10 forma, transformação em +0xA0 (3 colunas) e translação em +0xD0
 *   hkpMoppBvTreeShape    filho em +0x34
 *   hkpSimpleMeshShape    vértices (vec4): ptr +0x18, n +0x1C; triângulos (16 B): ptr +0x24, n +0x28
 *   hkpConvexVerticesShape  vértices em blocos de 4 (x[4] y[4] z[4]): array +0x40, nº de vértices +0x4C,
 *                         planos (nx ny nz d): array +0x50
 *   hkpCapsuleShape       raio +0x10, ponto A +0x20, ponto B +0x30
 * Os corpos e fantasmas são achados por uma varredura das vtables (a cada ~1,5 s, numa thread do host).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#endif
#include <d3d9.h>

#include "re5/coldebug.h"
#include "re5/log.h"
#include "re5/runtime.h"

#define VT_RIGID    0x0105443Cu
#define VT_FIXED    0x0105476Cu
#define VT_PHANTOM  0x010549FCu
#define VT_MOPP     0x0105C4A8u
#define VT_SIMPLE   0x0105C82Cu
#define VT_CONVEXV  0x0105D22Cu
#define VT_CAPSULE  0x0105BDACu

#define MAX_BODIES   4096
#define MAX_PHANTOMS 128
#define MAX_CACHE    4096
#define MAX_LINES    120000

typedef struct { float x, y, z, rhw; uint32_t color; } line_vtx;

typedef struct {
    uint32_t va;
    int      ok;
    uint32_t nv, ne;
    float   *v;        /* nv x 3 */
    uint32_t *e;       /* ne x 2 (índices de vértice) */
} shape_cache;

static int g_enabled = -1, g_on;
static SDL_mutex *g_mu;
static SDL_Thread *g_thread;
static uint32_t g_bodies[MAX_BODIES], g_nbodies, g_phantoms[MAX_PHANTOMS], g_nphantoms;
static shape_cache g_cache[MAX_CACHE];
static uint32_t g_ncache;

/* ---- leitura da memória do guest ---- */

static const uint8_t *G(uint32_t va, uint32_t n)
{
    return (va >= 0x10000u && n && re5_mem_is_mapped(va, n)) ? (const uint8_t *)re5_g2h(va) : NULL;
}

static int rd(uint32_t va, uint32_t *out)
{
    const uint8_t *p = G(va, 4);
    if (!p) return 0;
    memcpy(out, p, 4);
    return 1;
}

static uint32_t rdu(uint32_t va)
{
    uint32_t v = 0;
    rd(va, &v);
    return v;
}

static int rdf(uint32_t va, float *out, int n)
{
    const uint8_t *p = G(va, 4u * (uint32_t)n);
    if (!p) return 0;
    memcpy(out, p, 4u * (uint32_t)n);
    return 1;
}

/* ---- varredura das vtables ---- */

static void scan_main_once(void)
{
    static re5_mem_region regs[8192];
    uint32_t n = re5_mem_regions(regs, 8192);
    static uint32_t bodies[MAX_BODIES], phantoms[MAX_PHANTOMS];
    uint32_t nb = 0, np = 0;
    /* A região pode ser liberada pelo guest durante a varredura: copia em blocos com ReadProcessMemory
     * (falha sem derrubar o processo) em vez de ler o ponteiro do host direto. */
    static uint32_t buf[(256u << 10) / 4];
    for (uint32_t i = 0; i < n; i++) {
        if (regs[i].base < 0x10000000u) continue;   /* imagem e pilhas: os objetos vivos estão no heap */
        for (uint64_t off = 0; off < regs[i].size; off += sizeof buf) {
            size_t len = regs[i].size - off < sizeof buf ? (size_t)(regs[i].size - off) : sizeof buf;
            uint32_t base = regs[i].base + (uint32_t)off;
#if defined(RE5_HOST_WINDOWS)
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), re5_g2h(base), buf, len, &got) || got != len) continue;
#else
            if (!re5_mem_is_mapped(base, (uint32_t)len)) continue;
            memcpy(buf, re5_g2h(base), len);
#endif
            for (size_t k = 0; k < len / 16u; k++) {
                uint32_t v = buf[k * 4];
                if (v == VT_RIGID) {
                    if (nb < MAX_BODIES) bodies[nb++] = base + (uint32_t)k * 16u;
                } else if (v == VT_PHANTOM) {
                    if (np < MAX_PHANTOMS) phantoms[np++] = base + (uint32_t)k * 16u;
                }
            }
        }
    }
    SDL_LockMutex(g_mu);
    memcpy(g_bodies, bodies, nb * sizeof bodies[0]);
    memcpy(g_phantoms, phantoms, np * sizeof phantoms[0]);
    g_nbodies = nb;
    g_nphantoms = np;
    SDL_UnlockMutex(g_mu);
}

static int scan_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (g_on) scan_main_once();
        SDL_Delay(1500);
    }
    return 0;
}

/* ---- formas ---- */

static shape_cache *cache_find(uint32_t va)
{
    for (uint32_t i = 0; i < g_ncache; i++)
        if (g_cache[i].va == va) return &g_cache[i];
    return NULL;
}

static void build_convex(shape_cache *sc, uint32_t va)
{
    uint32_t vp = rdu(va + 0x40), nblk = rdu(va + 0x44) & 0x3FFFFFFFu, nv = rdu(va + 0x4C);
    uint32_t pp = rdu(va + 0x50), np = rdu(va + 0x54) & 0x3FFFFFFFu;
    if (!nv || nv > 512 || !nblk || nv > nblk * 4u || !np || np > 1024) return;
    const float *vb = (const float *)G(vp, nblk * 48u), *pl = (const float *)G(pp, np * 16u);
    if (!vb || !pl) return;
    sc->v = malloc(nv * 12u);
    for (uint32_t k = 0; k < nv; k++) {
        const float *blk = vb + (k / 4) * 12;
        sc->v[3 * k + 0] = blk[0 + k % 4];
        sc->v[3 * k + 1] = blk[4 + k % 4];
        sc->v[3 * k + 2] = blk[8 + k % 4];
    }
    sc->nv = nv;
    uint32_t cap = 256;
    sc->e = malloc(cap * 8u);
    for (uint32_t i = 0; i < nv; i++)
        for (uint32_t j = i + 1; j < nv; j++) {
            uint32_t shared = 0;
            for (uint32_t q = 0; q < np && shared < 2; q++) {
                const float *n = pl + 4 * q;
                const float *a = sc->v + 3 * i, *b = sc->v + 3 * j;
                float da = n[0] * a[0] + n[1] * a[1] + n[2] * a[2] + n[3];
                float db = n[0] * b[0] + n[1] * b[1] + n[2] * b[2] + n[3];
                if (fabsf(da) < 2e-3f && fabsf(db) < 2e-3f) shared++;
            }
            if (shared >= 2) {
                if (sc->ne == cap) {
                    cap *= 2;
                    sc->e = realloc(sc->e, cap * 8u);
                }
                sc->e[2 * sc->ne] = i;
                sc->e[2 * sc->ne + 1] = j;
                sc->ne++;
            }
        }
    sc->ok = 1;
}

static void build_mesh(shape_cache *sc, uint32_t va)
{
    uint32_t vp = rdu(va + 0x18), vn = rdu(va + 0x1C), tp = rdu(va + 0x24), tn = rdu(va + 0x28);
    if (!vn || vn > 2000000u || !tn || tn > 4000000u) return;
    const uint8_t *vb = G(vp, vn * 16u), *tb = G(tp, tn * 16u);
    if (!vb || !tb) return;
    sc->v = malloc(vn * 12u);
    for (uint32_t i = 0; i < vn; i++) memcpy(sc->v + 3 * i, vb + 16u * i, 12);
    sc->nv = vn;
    sc->e = malloc(tn * 6u * 4u);
    for (uint32_t t = 0; t < tn; t++) {
        uint32_t idx[3];
        memcpy(idx, tb + 16u * t, 12);
        if (idx[0] >= vn || idx[1] >= vn || idx[2] >= vn) continue;
        for (int k = 0; k < 3; k++) {
            sc->e[2 * sc->ne] = idx[k];
            sc->e[2 * sc->ne + 1] = idx[(k + 1) % 3];
            sc->ne++;
        }
    }
    sc->ok = 1;
}

static shape_cache *shape_get(uint32_t shape)
{
    uint32_t vt = rdu(shape);
    if (vt == VT_MOPP) {
        shape = rdu(shape + 0x34);
        vt = rdu(shape);
    }
    if (vt != VT_SIMPLE && vt != VT_CONVEXV) return NULL;
    shape_cache *sc = cache_find(shape);
    if (sc) return sc->ok ? sc : NULL;
    if (g_ncache == MAX_CACHE) return NULL;
    sc = &g_cache[g_ncache++];
    memset(sc, 0, sizeof *sc);
    sc->va = shape;
    if (vt == VT_SIMPLE) build_mesh(sc, shape); else build_convex(sc, shape);
    return sc->ok ? sc : NULL;
}

/* ---- matrizes ---- */

typedef struct { float r[4][4]; int transposed; uint32_t start; } cand;   /* r[k] = registrador k */

static int project(const cand *m, const float *p, float *clip)
{
    for (int k = 0; k < 4; k++) {
        if (!m->transposed)
            clip[k] = m->r[k][0] * p[0] + m->r[k][1] * p[1] + m->r[k][2] * p[2] + m->r[k][3];
        else
            clip[k] = m->r[0][k] * p[0] + m->r[1][k] * p[1] + m->r[2][k] * p[2] + m->r[3][k];
    }
    return clip[3] > 1e-3f;
}

static int inside(const cand *m, const float *p)
{
    float c[4];
    if (!project(m, p, c)) return 0;
    float ix = c[0] / c[3], iy = c[1] / c[3], iz = c[2] / c[3];
    return fabsf(ix) <= 1.0f && fabsf(iy) <= 1.0f && iz >= 0.0f && iz <= 1.0f;
}

/* Constantes de vertex shader do quadro atual (cópia das chamadas do guest). */
#define MAX_CALLS 6000
typedef struct { uint32_t start, n; float d[16 * 4]; } const_call;
static const_call g_calls[MAX_CALLS];
static uint32_t g_ncalls;
static SDL_mutex *g_cmu;

void re5_coldebug_vs_const(uint32_t start, const float *data, uint32_t count4)
{
    if (!g_on || !data || !count4 || !g_cmu) return;
    SDL_LockMutex(g_cmu);
    if (g_ncalls < MAX_CALLS) {
        const_call *c = &g_calls[g_ncalls++];
        c->start = start;
        c->n = count4 > 16 ? 16 : count4;
        memcpy(c->d, data, c->n * 16u);
    }
    SDL_UnlockMutex(g_cmu);
}

static uint64_t hash16(const float *f)
{
    uint64_t h = 1469598103934665603ull;
    const uint32_t *u = (const uint32_t *)f;
    for (int i = 0; i < 16; i++) h = (h ^ u[i]) * 1099511628211ull;
    return h;
}

static cand g_best;
static int g_have_best;

/* A matriz (de 4 registradores seguidos, nas duas orientações) que põe mais pontos dentro da tela. */
static int pick_camera(const float *pts, uint32_t npts, uint32_t nplayers, cand *out, uint32_t *score_out)
{
    static uint64_t seen[16384];   /* conjunto (endereçamento aberto) dos 4 registradores já avaliados */
    uint32_t nseen = 0, best_score = 0;
    memset(seen, 0, sizeof seen);
    cand best;
    int found = 0;
    SDL_LockMutex(g_cmu);
    for (uint32_t ci = 0; ci < g_ncalls; ci++) {
        const const_call *c = &g_calls[ci];
        if (c->n < 4) continue;
        for (uint32_t i = 0; i + 4 <= c->n; i += (c->n <= 8 ? 1u : 4u)) {
            uint64_t h = hash16(c->d + 4 * i) | 1u;
            uint32_t slot = (uint32_t)(h >> 7) & 16383u;
            int dup = 0;
            while (seen[slot]) {
                if (seen[slot] == h) { dup = 1; break; }
                slot = (slot + 1) & 16383u;
            }
            if (dup) continue;
            if (nseen >= 8000) continue;
            seen[slot] = h;
            nseen++;
            for (int tr = 0; tr < 2; tr++) {
                cand m;
                memcpy(m.r, c->d + 4 * i, sizeof m.r);
                m.transposed = tr;
                m.start = c->start + i;
                uint32_t score = 0, pl = 0;
                double sx = 0, sxx = 0, sy = 0, syy = 0;
                for (uint32_t p = 0; p < npts; p++) {
                    float cl[4];
                    if (!project(&m, pts + 3 * p, cl)) continue;
                    float ix = cl[0] / cl[3], iy = cl[1] / cl[3], iz = cl[2] / cl[3];
                    if (fabsf(ix) <= 1.0f && fabsf(iy) <= 1.0f && iz >= 0.0f && iz <= 1.0f) {
                        score++;
                        if (p < nplayers) pl++;
                        sx += ix; sxx += (double)ix * ix; sy += iy; syy += (double)iy * iy;
                    }
                }
                if (score < 3) continue;
                /* Matriz degenerada (tudo vira o mesmo ponto) ou que "vê" todos os pontos do mundo não é câmera. */
                double mx = sx / score, my = sy / score;
                double vx = sxx / score - mx * mx, vy = syy / score - my * my;
                if (vx < 0.003 || vy < 0.003 || score * 10u > npts * 9u) continue;
                score += pl * 20u;   /* os personagens pesam mais: estão sempre no centro da cena */
                if (score > best_score) {
                    best_score = score;
                    best = m;
                    found = 1;
                }
            }
        }
    }
    g_ncalls = 0;
    SDL_UnlockMutex(g_cmu);
    if (found && best_score >= 25) {
        *out = best;
        *score_out = best_score;
        return 1;
    }
    return 0;
}

/* ---- desenho ---- */

static line_vtx *g_lines;
static uint32_t g_nl;
static float g_vw, g_vh;
static cand g_cam;

static void emit_line(const float *a, const float *b, uint32_t color)
{
    if (g_nl + 2 > MAX_LINES * 2u) return;
    float ca[4], cb[4];
    project(&g_cam, a, ca);
    project(&g_cam, b, cb);
    const float eps = 1e-2f;
    if (ca[3] <= eps && cb[3] <= eps) return;
    if ((ca[0] > ca[3] && cb[0] > cb[3]) || (ca[0] < -ca[3] && cb[0] < -cb[3]) ||
        (ca[1] > ca[3] && cb[1] > cb[3]) || (ca[1] < -ca[3] && cb[1] < -cb[3]))
        return;
    if (ca[3] <= eps || cb[3] <= eps) {
        float *in = ca[3] > eps ? ca : cb, *out = ca[3] > eps ? cb : ca;
        float t = (eps - out[3]) / (in[3] - out[3]);
        for (int k = 0; k < 4; k++) out[k] += (in[k] - out[k]) * t;
    }
    float *c[2] = {ca, cb};
    for (int k = 0; k < 2; k++) {
        line_vtx *v = &g_lines[g_nl++];
        float w = c[k][3];
        v->x = (c[k][0] / w * 0.5f + 0.5f) * g_vw;
        v->y = (1.0f - (c[k][1] / w * 0.5f + 0.5f)) * g_vh;
        v->z = c[k][2] / w < 0 ? 0.0f : (c[k][2] / w > 1 ? 1.0f : c[k][2] / w);
        v->rhw = 1.0f / w;
        v->color = color;
    }
}

static void to_world(const float rot[3][3], const float *t, const float *p, float *o)
{
    for (int i = 0; i < 3; i++) o[i] = rot[0][i] * p[0] + rot[1][i] * p[1] + rot[2][i] * p[2] + t[i];
}

static int read_transform(uint32_t va, uint32_t rot_off, uint32_t t_off, float rot[3][3], float *t)
{
    for (int c = 0; c < 3; c++) {
        float col[4];
        if (!rdf(va + rot_off + 16u * (uint32_t)c, col, 4)) return 0;
        memcpy(rot[c], col, 12);
    }
    float tv[4];
    if (!rdf(va + t_off, tv, 4)) return 0;
    memcpy(t, tv, 12);
    for (int c = 0; c < 3; c++) {
        float n = rot[c][0] * rot[c][0] + rot[c][1] * rot[c][1] + rot[c][2] * rot[c][2];
        if (!(n > 0.9f && n < 1.1f)) return 0;
    }
    return 1;
}

static void draw_capsule(uint32_t ph, uint32_t shape, uint32_t color)
{
    float rot[3][3], t[3], r, ab[8];
    if (!read_transform(ph, 0xA0, 0xD0, rot, t) || !rdf(shape + 0x10, &r, 1) || !rdf(shape + 0x20, ab, 8)) return;
    if (!(r > 0.0f && r < 5.0f)) return;
    float a[3], b[3];
    to_world(rot, t, ab, a);
    to_world(rot, t, ab + 4, b);
    const int seg = 12;
    float prev[2][3];
    for (int s = 0; s <= seg; s++) {
        float th = 6.2831853f * (float)s / (float)seg;
        float off[3];
        for (int i = 0; i < 3; i++) off[i] = r * (cosf(th) * rot[0][i] + sinf(th) * rot[2][i]);
        float pa[3] = {a[0] + off[0], a[1] + off[1], a[2] + off[2]}, pb[3] = {b[0] + off[0], b[1] + off[1], b[2] + off[2]};
        if (s) {
            emit_line(prev[0], pa, color);
            emit_line(prev[1], pb, color);
        }
        if (s % 3 == 0) emit_line(pa, pb, color);
        memcpy(prev[0], pa, 12);
        memcpy(prev[1], pb, 12);
    }
}

static int g_logged;

void re5_coldebug_key(void)
{
    if (g_enabled < 0) {
        const char *e = getenv("RE5_COLDEBUG");
        g_enabled = e && *e && *e != '0';
        if (e && *e == '2') g_on = 1;
    }
    if (!g_enabled) return;
    g_on = !g_on;
    g_logged = 0;
    RE5_INFO("coldebug", "linhas de colisão %s (F6)", g_on ? "ligadas" : "desligadas");
}

void re5_coldebug_draw(IDirect3DDevice9 *dev)
{
    if (g_enabled < 0) {
        const char *e = getenv("RE5_COLDEBUG");
        g_enabled = e && *e && *e != '0';
        if (e && *e == '2') g_on = 1;
    }
    if (!g_enabled || !dev) return;
    if (!g_mu) {
        g_mu = SDL_CreateMutex();
        g_cmu = SDL_CreateMutex();
        g_lines = malloc(sizeof(line_vtx) * MAX_LINES * 2u);
        g_thread = SDL_CreateThread(scan_thread, "coldebug-scan", NULL);
    }
    if (!g_on) {
        SDL_LockMutex(g_cmu);
        g_ncalls = 0;
        SDL_UnlockMutex(g_cmu);
        return;
    }
    uint32_t bodies[MAX_BODIES], phantoms[MAX_PHANTOMS], nb, np;
    SDL_LockMutex(g_mu);
    nb = g_nbodies;
    np = g_nphantoms;
    memcpy(bodies, g_bodies, nb * sizeof bodies[0]);
    memcpy(phantoms, g_phantoms, np * sizeof phantoms[0]);
    SDL_UnlockMutex(g_mu);

    /* Pontos para escolher a câmera: os personagens primeiro, depois amostras dos vértices do mundo. */
    static float pts[3 * 600];
    uint32_t npts = 0, nplayers = 0;
    for (uint32_t i = 0; i < np && npts < 40; i++) {
        float rot[3][3], t[3], ab[8];
        uint32_t shape = rdu(phantoms[i] + 0x10);
        if (rdu(shape) != VT_CAPSULE || !read_transform(phantoms[i], 0xA0, 0xD0, rot, t) || !rdf(shape + 0x20, ab, 8))
            continue;
        float mid[3] = {(ab[0] + ab[4]) * 0.5f, (ab[1] + ab[5]) * 0.5f, (ab[2] + ab[6]) * 0.5f};
        to_world(rot, t, mid, pts + 3 * npts);
        npts++;
        nplayers++;
    }
    for (uint32_t i = 0; i < nb && npts < 600; i++) {
        float rot[3][3], t[3];
        uint32_t vt = rdu(bodies[i] + 0xD0);
        if (vt != VT_FIXED || !read_transform(bodies[i], 0xE0, 0x110, rot, t)) continue;
        shape_cache *sc = shape_get(rdu(bodies[i] + 0x10));
        if (!sc || sc->nv < 8) continue;
        uint32_t step = sc->nv / 24u + 1u;
        for (uint32_t k = 0; k < sc->nv && npts < 600; k += step) {
            to_world(rot, t, sc->v + 3 * k, pts + 3 * npts);
            npts++;
        }
    }
    uint32_t score = 0;
    cand cam;
    if (!pick_camera(pts, npts, nplayers, &cam, &score)) {
        if (!g_logged) {
            RE5_INFO("coldebug", "sem matriz de câmera (%u corpos, %u fantasmas): nada a desenhar neste quadro", nb, np);
            g_logged = 1;
        }
        return;
    }
    g_cam = cam;
    if (!g_have_best || g_best.start != cam.start || g_best.transposed != cam.transposed) {
        RE5_INFO("coldebug", "câmera: registradores c%u..c%u (%s), %u pontos; %u corpos, %u fantasmas", cam.start,
                 cam.start + 3, cam.transposed ? "colunas" : "linhas", score, nb, np);
        g_best = cam;
        g_have_best = 1;
    }

    IDirect3DSurface9 *bb = NULL, *rt0 = NULL, *ds = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    bb->lpVtbl->GetDesc(bb, &desc);
    g_vw = (float)desc.Width;
    g_vh = (float)desc.Height;
    g_nl = 0;

    for (uint32_t i = 0; i < nb; i++) {
        float rot[3][3], t[3];
        uint32_t body = bodies[i];
        if (!read_transform(body, 0xE0, 0x110, rot, t)) continue;
        shape_cache *sc = shape_get(rdu(body + 0x10));
        if (!sc) continue;
        uint32_t color = rdu(body + 0xD0) == VT_FIXED ? (rdu(rdu(body + 0x10)) == VT_MOPP ? 0xB000E000u : 0xB0FF8000u)
                                                       : 0xC0FFE020u;
        static float wv[3 * 8192];
        float *w = sc->nv <= 8192 ? wv : malloc(sc->nv * 12u);
        for (uint32_t k = 0; k < sc->nv; k++) to_world(rot, t, sc->v + 3 * k, w + 3 * k);
        for (uint32_t e = 0; e < sc->ne; e++) emit_line(w + 3 * sc->e[2 * e], w + 3 * sc->e[2 * e + 1], color);
        if (w != wv) free(w);
    }
    for (uint32_t i = 0; i < np; i++) draw_capsule(phantoms[i], rdu(phantoms[i] + 0x10), 0xFF00FFFFu);

    static uint32_t frame;
    if (++frame % 180 == 0)
        RE5_INFO("coldebug", "quadro %u: %u corpos, %u fantasmas, %u formas, %u linhas, câmera c%u (%s) %u pontos de %u",
                 frame, nb, np, g_ncache, g_nl / 2, cam.start, cam.transposed ? "colunas" : "linhas", score, npts);
    if (g_nl >= 2) {
        IDirect3DStateBlock9 *sb = NULL;
        dev->lpVtbl->GetRenderTarget(dev, 0, &rt0);
        dev->lpVtbl->GetDepthStencilSurface(dev, &ds);
        if (SUCCEEDED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &sb)) && sb) {
            D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
            dev->lpVtbl->SetRenderTarget(dev, 0, bb);
            dev->lpVtbl->SetDepthStencilSurface(dev, NULL);
            dev->lpVtbl->SetViewport(dev, &vp);
            dev->lpVtbl->SetVertexShader(dev, NULL);
            dev->lpVtbl->SetPixelShader(dev, NULL);
            dev->lpVtbl->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTexture(dev, 1, NULL);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
            dev->lpVtbl->SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            for (uint32_t off = 0; off + 2 <= g_nl;) {
                uint32_t n = g_nl - off;
                if (n > 40000u) n = 40000u;
                dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_LINELIST, n / 2, g_lines + off, sizeof(line_vtx));
                off += n;
            }
            sb->lpVtbl->Apply(sb);
            dev->lpVtbl->SetRenderTarget(dev, 0, rt0);
            dev->lpVtbl->SetDepthStencilSurface(dev, ds);
            sb->lpVtbl->Release(sb);
        }
    }
    if (rt0) rt0->lpVtbl->Release(rt0);
    if (ds) ds->lpVtbl->Release(ds);
    bb->lpVtbl->Release(bb);
}
