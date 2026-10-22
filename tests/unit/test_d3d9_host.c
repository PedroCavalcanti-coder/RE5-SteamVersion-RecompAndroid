/*
 * Ponte do Direct3D 9 sobre o Direct3D do host de verdade (Windows: d3d9.dll
 * x64, ou o de RE5_D3D9_DLL): Direct3DCreate9, GetAdapterCount,
 * GetAdapterIdentifier, GetDeviceCaps, CheckDeviceFormat, CreateDevice numa
 * janela SDL2 escondida, Clear/Present e Lock/Unlock de buffer e textura.
 * Sai com 77 (pulado) se não houver vídeo ou adaptador.
 */
#include <stdio.h>
#include <string.h>

#define SDL_MAIN_HANDLED   /* main() é nosso, não o SDL_main */
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/d3d9_bridge.h"

#include <windows.h>
#include <d3d9.h>

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

#define SKIP 77
#define GUEST_HWND 0x00010010u

static re5_thread *T;
static uint32_t g_scratch, g_off;
static HWND g_hwnd;

static uint32_t gbuf(uint32_t n)
{
    uint32_t va = g_scratch + g_off;
    memset(re5_g2h(va), 0, n);
    g_off = (g_off + n + 15u) & ~15u;
    return va;
}

static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *args)
{
    uint32_t a[16];
    a[0] = obj;
    for (int i = 0; i < n; i++) a[i + 1] = args[i];
    return re5_guest_call(T, rd32(rd32(obj) + 4 * slot), n + 1, a);
}

static void *test_window(uint32_t hwnd) { return hwnd == GUEST_HWND ? g_hwnd : NULL; }

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("pulado: SDL sem vídeo (%s)\n", SDL_GetError());
        return SKIP;
    }
    SDL_Window *w = SDL_CreateWindow("test_d3d9_host", 0, 0, 320, 240, SDL_WINDOW_HIDDEN);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!w || !SDL_GetWindowWMInfo(w, &info)) {
        printf("pulado: sem janela SDL (%s)\n", SDL_GetError());
        return SKIP;
    }
    g_hwnd = info.info.win.window;

    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    re5_d3d9_backend b = *re5_d3d9_host_backend();
    b.window = test_window;
    re5_d3d9_set_backend(&b);

    const uint32_t sdk = D3D_SDK_VERSION;
    uint32_t d3d = re5_guest_call(T, re5_import_bind("d3d9.dll", "Direct3DCreate9", 0), 1, &sdk);
    if (!d3d) {
        printf("pulado: Direct3DCreate9 do host devolveu NULL\n");
        return SKIP;
    }
    uint32_t adapters = vcall(d3d, 4, 0, NULL);   /* GetAdapterCount */
    if (!adapters) {
        printf("pulado: nenhum adaptador\n");
        return SKIP;
    }
    uint32_t id = gbuf(1104);
    const uint32_t gai[3] = {0, 0, id};
    CHECK_EQ_U32(vcall(d3d, 5, 3, gai), D3D_OK);
    printf("adaptadores: %u; 0 = '%s' (vendor 0x%04X)\n", adapters, (const char *)re5_g2h(id + 512), rd32(id + 1064));
    CHECK(rd32(id + 1064) != 0);

    uint32_t caps = gbuf(304);
    const uint32_t gdc[3] = {0, D3DDEVTYPE_HAL, caps};
    CHECK_EQ_U32(vcall(d3d, 14, 3, gdc), D3D_OK);
    uint32_t vs = rd32(caps + offsetof(D3DCAPS9, VertexShaderVersion));
    uint32_t ps = rd32(caps + offsetof(D3DCAPS9, PixelShaderVersion));
    printf("caps: vs %u.%u ps %u.%u, textura máx %ux%u\n", (vs >> 8) & 0xFF, vs & 0xFF, (ps >> 8) & 0xFF, ps & 0xFF,
           rd32(caps + offsetof(D3DCAPS9, MaxTextureWidth)), rd32(caps + offsetof(D3DCAPS9, MaxTextureHeight)));
    CHECK(((ps >> 8) & 0xFF) >= 2);

    const uint32_t cdf[6] = {0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT5};
    CHECK_EQ_U32(vcall(d3d, 10, 6, cdf), D3D_OK);

    uint32_t pp = gbuf(56), pdev = gbuf(4);
    const uint32_t v[14] = {320, 240, D3DFMT_X8R8G8B8, 1, 0, 0, D3DSWAPEFFECT_DISCARD, GUEST_HWND,
                            1, 1, D3DFMT_D24S8, 0, 0, D3DPRESENT_INTERVAL_IMMEDIATE};
    for (int i = 0; i < 14; i++) wr32(pp + 4u * (uint32_t)i, v[i]);
    const uint32_t cd[6] = {0, D3DDEVTYPE_HAL, GUEST_HWND, D3DCREATE_HARDWARE_VERTEXPROCESSING, pp, pdev};
    uint32_t hr = vcall(d3d, 16, 6, cd);
    if (hr != D3D_OK) {
        printf("pulado: CreateDevice do host -> 0x%08X\n", hr);
        return SKIP;
    }
    uint32_t dev = rd32(pdev);
    CHECK(dev != 0);

    const uint32_t clr[6] = {0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF336699u, 0x3F800000u /* 1.0f */, 0};
    CHECK_EQ_U32(vcall(dev, 43, 6, clr), D3D_OK);
    CHECK_EQ_U32(vcall(dev, 41, 0, NULL), D3D_OK);   /* BeginScene */
    CHECK_EQ_U32(vcall(dev, 42, 0, NULL), D3D_OK);   /* EndScene */
    const uint32_t pr[4] = {0, 0, 0, 0};
    uint32_t prs = vcall(dev, 17, 4, pr);   /* S_PRESENT_OCCLUDED (janela coberta) também é sucesso */
    CHECK(prs == D3D_OK || prs == 0x08760878u);
    CHECK_EQ_U32(re5_d3d9_present_count(), 1);

    /* Vertex buffer: escreve pela sombra, lê de volta por uma trava READONLY. */
    uint32_t pvb = gbuf(4), pdata = gbuf(4);
    const uint32_t cvb[6] = {1024, 0, 0, D3DPOOL_MANAGED, pvb, 0};
    CHECK_EQ_U32(vcall(dev, 26, 6, cvb), D3D_OK);
    uint32_t vb = rd32(pvb);
    const uint32_t lk[4] = {0, 0, pdata, 0};
    CHECK_EQ_U32(vcall(vb, 11, 4, lk), D3D_OK);
    memset(re5_g2h(rd32(pdata)), 0x5A, 1024);
    CHECK_EQ_U32(vcall(vb, 12, 0, NULL), D3D_OK);
    IDirect3DVertexBuffer9 *hvb = re5_d3d9_host_object(vb);
    void *hp = NULL;
    CHECK(hvb->lpVtbl->Lock(hvb, 0, 0, &hp, D3DLOCK_READONLY) == D3D_OK);
    CHECK(hp && ((uint8_t *)hp)[1023] == 0x5A);
    hvb->lpVtbl->Unlock(hvb);
    CHECK_EQ_U32(vcall(vb, 2, 0, NULL), 0);

    /* Textura gerenciada 64x64 DXT1: blocos de 4x4, 8 bytes. */
    uint32_t ptex = gbuf(4), lr = gbuf(8);
    const uint32_t ctx[8] = {64, 64, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, ptex, 0};
    CHECK_EQ_U32(vcall(dev, 23, 8, ctx), D3D_OK);
    uint32_t tex = rd32(ptex);
    const uint32_t ltx[4] = {0, lr, 0, 0};
    CHECK_EQ_U32(vcall(tex, 19, 4, ltx), D3D_OK);
    CHECK(rd32(lr) >= 16 * 8);   /* pitch: 16 blocos por linha */
    memset(re5_g2h(rd32(lr + 4)), 0x77, 8);
    const uint32_t lvl = 0;
    CHECK_EQ_U32(vcall(tex, 20, 1, &lvl), D3D_OK);
    CHECK_EQ_U32(vcall(tex, 2, 0, NULL), 0);

    CHECK_EQ_U32(vcall(dev, 2, 0, NULL), 0);
    CHECK_EQ_U32(vcall(d3d, 2, 0, NULL), 0);
    CHECK_EQ_U32(re5_d3d9_live_objects(), 0);
    SDL_DestroyWindow(w);
    CHECK_DONE();
}
