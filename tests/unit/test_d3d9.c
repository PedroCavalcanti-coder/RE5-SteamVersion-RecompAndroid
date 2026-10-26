/*
 * Ponte COM do Direct3D 9 com um backend falso (sem GPU): objetos do guest
 * com vtables de thunks stdcall (nomes, bytes de pilha, índice sem handler),
 * repassadores automáticos (inteiros, float, ponteiros para estruturas,
 * objetos de entrada e de saída), D3DPRESENT_PARAMETERS 32 -> 64 bits com o
 * HWND traduzido, identidade e contagem de referências, e Lock/Unlock com
 * buffer-sombra na memória do guest.
 */
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/d3d9_bridge.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#include <d3d9.h>

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;
static uint32_t g_scratch, g_off;

static uint32_t gbuf(uint32_t n)
{
    uint32_t va = g_scratch + g_off;
    memset(re5_g2h(va), 0, n);
    g_off = (g_off + n + 15u) & ~15u;
    return va;
}

/* Método COM: stdcall, o this é o primeiro argumento na pilha. */
static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *args)
{
    uint32_t a[16];
    a[0] = obj;
    for (int i = 0; i < n; i++) a[i + 1] = args[i];
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, rd32(rd32(obj) + 4 * slot), n + 1, a);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static const re5_thunk *method(uint32_t obj, uint32_t slot) { return re5_thunk_get(rd32(rd32(obj) + 4 * slot)); }

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static int in_guest(uint32_t va) { return va >= 0x10000u && va < RE5_THUNK_BASE; }

/* ---- backend falso ---- */

#define GUEST_HWND 0x00010010u
#define FAKE_HWND  ((HWND)(uintptr_t)0x0000ABCD00001234ull)

typedef struct {
    const void *lpVtbl;
    ULONG       refs;
    uint8_t     mem[512];
    UINT        size;
    UINT        locks, unlocks;
} fake;

#define FAKE_REFS(T)                                                                         \
    static ULONG STDMETHODCALLTYPE fake_##T##_AddRef(T *This) { return ++((fake *)This)->refs; } \
    static ULONG STDMETHODCALLTYPE fake_##T##_Release(T *This) { return --((fake *)This)->refs; }

FAKE_REFS(IDirect3D9)
FAKE_REFS(IDirect3DDevice9)
FAKE_REFS(IDirect3DSurface9)
FAKE_REFS(IDirect3DVertexBuffer9)
FAKE_REFS(IDirect3DTexture9)
FAKE_REFS(IDirect3DVolumeTexture9)

static fake g_d3d, g_dev, g_bb, g_vb, g_tex, g_vol;

static struct {
    UINT adapter, usage;
    D3DDEVTYPE type;
    D3DFORMAT afmt, cfmt;
    D3DRESOURCETYPE rtype;
    HWND focus, present_hwnd;
    DWORD flags;
    D3DPRESENT_PARAMETERS pp;
    float z;
    D3DCOLOR color;
    D3DRENDERSTATETYPE rs;
    DWORD rs_value;
    IDirect3DBaseTexture9 *tex;
    UINT presents;
} rec;

static UINT STDMETHODCALLTYPE d3d_GetAdapterCount(IDirect3D9 *This) { return 1; }

static HRESULT STDMETHODCALLTYPE d3d_GetAdapterIdentifier(IDirect3D9 *This, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id)
{
    memset(id, 0, sizeof *id);
    strcpy(id->Description, "Fake GPU");
    id->VendorId = 0x10DE;
    id->DeviceId = 0x1234;
    id->DriverVersion.QuadPart = 0x0006000E000A0001ll;
    id->WHQLLevel = 1;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormat(IDirect3D9 *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u,
                                                       D3DRESOURCETYPE r, D3DFORMAT cf)
{
    rec.adapter = a;
    rec.type = t;
    rec.afmt = af;
    rec.usage = u;
    rec.rtype = r;
    rec.cfmt = cf;
    return cf == D3DFMT_A2R10G10B10 ? D3DERR_NOTAVAILABLE : D3D_OK;
}

static HRESULT STDMETHODCALLTYPE d3d_GetDeviceCaps(IDirect3D9 *This, UINT a, D3DDEVTYPE t, D3DCAPS9 *caps)
{
    memset(caps, 0, sizeof *caps);
    caps->DeviceType = t;
    caps->MaxTextureWidth = 8192;
    caps->MaxVertexShaderConst = 256;
    caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
    caps->MaxPointSize = 8192.0f;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE d3d_CreateDevice(IDirect3D9 *This, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags,
                                                  D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    rec.adapter = a;
    rec.type = t;
    rec.focus = focus;
    rec.flags = flags;
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;   /* como o runtime de verdade */
    rec.pp = *pp;
    g_dev.refs++;
    *out = (IDirect3DDevice9 *)&g_dev;
    return D3D_OK;
}

static struct IDirect3D9Vtbl k_d3d_vt = {
    .AddRef = fake_IDirect3D9_AddRef,
    .Release = fake_IDirect3D9_Release,
    .GetAdapterCount = d3d_GetAdapterCount,
    .GetAdapterIdentifier = d3d_GetAdapterIdentifier,
    .CheckDeviceFormat = d3d_CheckDeviceFormat,
    .GetDeviceCaps = d3d_GetDeviceCaps,
    .CreateDevice = d3d_CreateDevice,
};

static HRESULT STDMETHODCALLTYPE dev_QueryInterface(IDirect3DDevice9 *This, REFIID riid, void **out)
{
    return E_NOINTERFACE;   /* a ponte não deve perguntar ao host pela própria interface */
}

static HRESULT STDMETHODCALLTYPE dev_GetCreationParameters(IDirect3DDevice9 *This, D3DDEVICE_CREATION_PARAMETERS *p)
{
    p->AdapterOrdinal = rec.adapter;
    p->DeviceType = rec.type;
    p->hFocusWindow = rec.focus;
    p->BehaviorFlags = rec.flags;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_Present(IDirect3DDevice9 *This, const RECT *s, const RECT *d, HWND w,
                                             const RGNDATA *r)
{
    rec.presents++;
    rec.present_hwnd = w;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_GetBackBuffer(IDirect3DDevice9 *This, UINT sc, UINT i, D3DBACKBUFFER_TYPE t,
                                                   IDirect3DSurface9 **out)
{
    g_bb.refs++;
    *out = (IDirect3DSurface9 *)&g_bb;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateTexture(IDirect3DDevice9 *This, UINT w, UINT h, UINT l, DWORD u,
                                                   D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **out, HANDLE *sh)
{
    if (sh || w != 8 || h != 8 || (f != D3DFMT_A8R8G8B8 && f != D3DFMT_A8B8G8R8)) return D3DERR_INVALIDCALL;
    g_tex.refs++;
    *out = (IDirect3DTexture9 *)&g_tex;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateVolumeTexture(IDirect3DDevice9 *This, UINT w, UINT h, UINT d, UINT l,
                                                         DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9 **out,
                                                         HANDLE *sh)
{
    if (sh || w != 4 || h != 4 || d != 2 || f != D3DFMT_A8R8G8B8) return D3DERR_INVALIDCALL;
    g_vol.refs++;
    *out = (IDirect3DVolumeTexture9 *)&g_vol;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateVertexBuffer(IDirect3DDevice9 *This, UINT len, DWORD u, DWORD fvf,
                                                        D3DPOOL p, IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    if (sh || len > sizeof g_vb.mem) return D3DERR_INVALIDCALL;
    g_vb.size = len;
    g_vb.refs++;
    *out = (IDirect3DVertexBuffer9 *)&g_vb;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_Clear(IDirect3DDevice9 *This, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c,
                                           float z, DWORD s)
{
    rec.color = c;
    rec.z = z;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_SetRenderState(IDirect3DDevice9 *This, D3DRENDERSTATETYPE s, DWORD v)
{
    rec.rs = s;
    rec.rs_value = v;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_SetTexture(IDirect3DDevice9 *This, DWORD stage, IDirect3DBaseTexture9 *t)
{
    rec.tex = t;
    return D3D_OK;
}

static struct IDirect3DDevice9Vtbl k_dev_vt = {
    .QueryInterface = dev_QueryInterface,
    .AddRef = fake_IDirect3DDevice9_AddRef,
    .Release = fake_IDirect3DDevice9_Release,
    .GetCreationParameters = dev_GetCreationParameters,
    .Present = dev_Present,
    .GetBackBuffer = dev_GetBackBuffer,
    .CreateTexture = dev_CreateTexture,
    .CreateVolumeTexture = dev_CreateVolumeTexture,
    .CreateVertexBuffer = dev_CreateVertexBuffer,
    .Clear = dev_Clear,
    .SetRenderState = dev_SetRenderState,
    .SetTexture = dev_SetTexture,
};

static HRESULT STDMETHODCALLTYPE surf_GetDesc(IDirect3DSurface9 *This, D3DSURFACE_DESC *d)
{
    memset(d, 0, sizeof *d);
    d->Format = D3DFMT_A8B8G8R8;
    d->Width = d->Height = 8;
    return D3D_OK;
}

static struct IDirect3DSurface9Vtbl k_bb_vt = {
    .AddRef = fake_IDirect3DSurface9_AddRef,
    .Release = fake_IDirect3DSurface9_Release,
    .GetDesc = surf_GetDesc,
};

static HRESULT STDMETHODCALLTYPE vb_GetDesc(IDirect3DVertexBuffer9 *This, D3DVERTEXBUFFER_DESC *d)
{
    memset(d, 0, sizeof *d);
    d->Size = g_vb.size;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE vb_Lock(IDirect3DVertexBuffer9 *This, UINT off, UINT size, void **p, DWORD f)
{
    g_vb.locks++;
    *p = g_vb.mem + off;
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE vb_Unlock(IDirect3DVertexBuffer9 *This)
{
    g_vb.unlocks++;
    return D3D_OK;
}

static struct IDirect3DVertexBuffer9Vtbl k_vb_vt = {
    .AddRef = fake_IDirect3DVertexBuffer9_AddRef,
    .Release = fake_IDirect3DVertexBuffer9_Release,
    .GetDesc = vb_GetDesc,
    .Lock = vb_Lock,
    .Unlock = vb_Unlock,
};

#define TEX_PITCH 40u   /* maior que 8 * 4: a ponte tem de manter o pitch do host */

static DWORD STDMETHODCALLTYPE tex_GetLevelCount(IDirect3DTexture9 *This) { return 1; }

static HRESULT STDMETHODCALLTYPE tex_GetSurfaceLevel(IDirect3DTexture9 *This, UINT l, IDirect3DSurface9 **out)
{
    g_bb.refs++;
    *out = (IDirect3DSurface9 *)&g_bb;
    return D3D_OK;
}
static D3DRESOURCETYPE STDMETHODCALLTYPE tex_GetType(IDirect3DTexture9 *This) { return D3DRTYPE_TEXTURE; }

static HRESULT STDMETHODCALLTYPE tex_GetLevelDesc(IDirect3DTexture9 *This, UINT l, D3DSURFACE_DESC *d)
{
    memset(d, 0, sizeof *d);
    d->Format = D3DFMT_A8R8G8B8;
    d->Type = D3DRTYPE_SURFACE;
    d->Width = d->Height = 8;
    return l == 0 ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE tex_LockRect(IDirect3DTexture9 *This, UINT l, D3DLOCKED_RECT *lr, const RECT *r,
                                              DWORD f)
{
    g_tex.locks++;
    lr->Pitch = TEX_PITCH;
    lr->pBits = g_tex.mem + (r ? (UINT)r->top * TEX_PITCH + (UINT)r->left * 4u : 0);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE tex_UnlockRect(IDirect3DTexture9 *This, UINT l)
{
    g_tex.unlocks++;
    return D3D_OK;
}

static struct IDirect3DTexture9Vtbl k_tex_vt = {
    .AddRef = fake_IDirect3DTexture9_AddRef,
    .Release = fake_IDirect3DTexture9_Release,
    .GetType = tex_GetType,
    .GetLevelCount = tex_GetLevelCount,
    .GetLevelDesc = tex_GetLevelDesc,
    .GetSurfaceLevel = tex_GetSurfaceLevel,
    .LockRect = tex_LockRect,
    .UnlockRect = tex_UnlockRect,
};

#define VOL_ROW   20u    /* 4 * 4 + 4 */
#define VOL_SLICE 100u   /* 4 linhas de 20 + 20 */

static D3DRESOURCETYPE STDMETHODCALLTYPE vol_GetType(IDirect3DVolumeTexture9 *This) { return D3DRTYPE_VOLUMETEXTURE; }

static HRESULT STDMETHODCALLTYPE vol_GetLevelDesc(IDirect3DVolumeTexture9 *This, UINT l, D3DVOLUME_DESC *d)
{
    memset(d, 0, sizeof *d);
    d->Format = D3DFMT_A8R8G8B8;
    d->Type = D3DRTYPE_VOLUME;
    d->Width = d->Height = 4;
    d->Depth = 2;
    return l == 0 ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE vol_LockBox(IDirect3DVolumeTexture9 *This, UINT l, D3DLOCKED_BOX *lb,
                                             const D3DBOX *b, DWORD f)
{
    g_vol.locks++;
    lb->RowPitch = VOL_ROW;
    lb->SlicePitch = VOL_SLICE;
    lb->pBits = g_vol.mem + (b ? b->Front * VOL_SLICE + b->Top * VOL_ROW + b->Left * 4u : 0);
    return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE vol_UnlockBox(IDirect3DVolumeTexture9 *This, UINT l)
{
    g_vol.unlocks++;
    return D3D_OK;
}

static struct IDirect3DVolumeTexture9Vtbl k_vol_vt = {
    .AddRef = fake_IDirect3DVolumeTexture9_AddRef,
    .Release = fake_IDirect3DVolumeTexture9_Release,
    .GetType = vol_GetType,
    .GetLevelDesc = vol_GetLevelDesc,
    .LockBox = vol_LockBox,
    .UnlockBox = vol_UnlockBox,
};

static uint32_t g_create_sdk;

static void *fake_create9(uint32_t sdk)
{
    g_create_sdk = sdk;
    g_d3d.refs++;
    return &g_d3d;
}

static void *fake_window(uint32_t hwnd) { return hwnd == GUEST_HWND ? FAKE_HWND : NULL; }

static const re5_d3d9_backend k_fake = {"falso", fake_create9, fake_window};

/* ---- teste ---- */

static void put_pp(uint32_t pp, uint32_t hwnd)
{
    const uint32_t v[14] = {1280, 720, D3DFMT_X8R8G8B8, 0, D3DMULTISAMPLE_NONE, 0, D3DSWAPEFFECT_DISCARD, hwnd,
                            1, 1, D3DFMT_D24S8, D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL, 0, D3DPRESENT_INTERVAL_IMMEDIATE};
    for (int i = 0; i < 14; i++) wr32(pp + 4u * (uint32_t)i, v[i]);
}

int main(void)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    g_d3d.lpVtbl = &k_d3d_vt;
    g_dev.lpVtbl = &k_dev_vt;
    g_bb.lpVtbl = &k_bb_vt;
    g_bb.refs = 1;   /* o device guarda a sua */
    g_vb.lpVtbl = &k_vb_vt;
    g_tex.lpVtbl = &k_tex_vt;
    g_vol.lpVtbl = &k_vol_vt;
    re5_d3d9_set_backend(&k_fake);

    /* Layouts que a ponte repassa por ponteiro: iguais no x86 (MSVC) e no host. */
    CHECK_EQ_U32(sizeof(D3DCAPS9), 304);
    CHECK_EQ_U32(sizeof(D3DADAPTER_IDENTIFIER9), 1104);
    CHECK_EQ_U32(offsetof(D3DADAPTER_IDENTIFIER9, VendorId), 1064);
    CHECK_EQ_U32(sizeof(D3DSURFACE_DESC), 32);
    CHECK_EQ_U32(sizeof(D3DVIEWPORT9), 24);
    CHECK_EQ_U32(sizeof(D3DVERTEXELEMENT9), 8);
    CHECK_EQ_U32(sizeof(RECT), 16);

    /* Direct3DCreate9: stdcall, 4 bytes. */
    CHECK_EQ_U32(re5_thunk_get(re5_import_bind("d3d9.dll", "Direct3DCreate9", 0))->arg_bytes, 4);
    const uint32_t sdk = D3D_SDK_VERSION;
    uint32_t d3d = re5_guest_call(T, re5_import_bind("d3d9.dll", "Direct3DCreate9", 0), 1, &sdk);
    CHECK(in_guest(d3d));
    CHECK_EQ_U32(g_create_sdk, 32);
    CHECK(re5_d3d9_host_object(d3d) == &g_d3d);
    CHECK_EQ_U32(rd32(d3d + 12), 0x39443344u);   /* 'D3D9' */

    /* vtable: nomes e bytes de pilha (this incluído) vindos do d3d9.h. */
    CHECK(strcmp(method(d3d, 0)->name, "IDirect3D9::QueryInterface") == 0);
    CHECK(strcmp(method(d3d, 4)->name, "IDirect3D9::GetAdapterCount") == 0);
    CHECK_EQ_U32(method(d3d, 4)->arg_bytes, 4);
    CHECK(strcmp(method(d3d, 16)->name, "IDirect3D9::CreateDevice") == 0);
    CHECK_EQ_U32(method(d3d, 16)->arg_bytes, 28);

    CHECK_EQ_U32(vcall(d3d, 4, 0, NULL), 1);   /* GetAdapterCount */

    uint32_t id = gbuf(1104);
    const uint32_t gai[3] = {0, 0, id};
    CHECK_EQ_U32(vcall(d3d, 5, 3, gai), D3D_OK);
    CHECK(strcmp((const char *)re5_g2h(id + 512), "Fake GPU") == 0);
    CHECK_EQ_U32(rd32(id + 1064), 0x10DE);
    CHECK_EQ_U32(rd32(id + 1056), 0x000A0001u);   /* DriverVersion, LARGE_INTEGER em +1056 */

    const uint32_t cdf[6] = {0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE,
                             D3DFMT_A2R10G10B10};
    CHECK_EQ_U32(vcall(d3d, 10, 6, cdf), (uint32_t)D3DERR_NOTAVAILABLE);
    CHECK_EQ_U32(rec.usage, D3DUSAGE_RENDERTARGET);
    CHECK_EQ_U32(rec.rtype, D3DRTYPE_SURFACE);
    CHECK_EQ_U32(rec.afmt, D3DFMT_X8R8G8B8);

    uint32_t caps = gbuf(304);
    const uint32_t gdc[3] = {0, D3DDEVTYPE_HAL, caps};
    CHECK_EQ_U32(vcall(d3d, 14, 3, gdc), D3D_OK);
    CHECK_EQ_U32(rd32(caps + offsetof(D3DCAPS9, MaxTextureWidth)), 8192);
    CHECK_EQ_U32(rd32(caps + offsetof(D3DCAPS9, PixelShaderVersion)), D3DPS_VERSION(3, 0));
    CHECK_EQ_U32(rd32(caps + offsetof(D3DCAPS9, MaxPointSize)), fbits(8192.0f));

    /* D3DPRESENT_PARAMETERS: 56 bytes no guest, HWND de 32 bits traduzido. */
    uint32_t pp = gbuf(56);
    D3DPRESENT_PARAMETERS hp;
    put_pp(pp, GUEST_HWND);
    CHECK(re5_d3d9_pp_from_guest(pp, &hp) == 0);
    CHECK(hp.hDeviceWindow == FAKE_HWND);
    CHECK_EQ_U32(hp.BackBufferWidth, 1280);
    CHECK_EQ_U32(hp.SwapEffect, D3DSWAPEFFECT_DISCARD);
    CHECK_EQ_U32(hp.Windowed, 1);
    CHECK_EQ_U32(hp.AutoDepthStencilFormat, D3DFMT_D24S8);
    CHECK_EQ_U32(hp.Flags, D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL);
    CHECK_EQ_U32(hp.PresentationInterval, D3DPRESENT_INTERVAL_IMMEDIATE);
    put_pp(pp, 0);
    CHECK(re5_d3d9_pp_from_guest(pp, &hp) == 0);
    CHECK(hp.hDeviceWindow == NULL);
    put_pp(pp, 0x00010099u);   /* HWND que o host não conhece */
    CHECK(re5_d3d9_pp_from_guest(pp, &hp) == -1);
    hp.BackBufferCount = 2;
    hp.hDeviceWindow = FAKE_HWND;
    re5_d3d9_pp_to_guest(&hp, pp);
    CHECK_EQ_U32(rd32(pp + 12), 2);
    CHECK_EQ_U32(rd32(pp + 28), 0x00010099u);   /* o HWND do guest não é sobrescrito */

    /* CreateDevice */
    put_pp(pp, GUEST_HWND);
    uint32_t pdev = gbuf(4);
    const uint32_t cd[6] = {0, D3DDEVTYPE_HAL, GUEST_HWND, D3DCREATE_HARDWARE_VERTEXPROCESSING, pp, pdev};
    CHECK_EQ_U32(vcall(d3d, 16, 6, cd), D3D_OK);
    uint32_t dev = rd32(pdev);
    CHECK(in_guest(dev));
    CHECK(re5_d3d9_host_object(dev) == &g_dev);
    CHECK(rec.focus == FAKE_HWND);
    CHECK(rec.pp.hDeviceWindow == FAKE_HWND);
    CHECK_EQ_U32(rec.flags, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE);
    CHECK_EQ_U32(rec.pp.BackBufferHeight, 720);
    CHECK_EQ_U32(rec.pp.EnableAutoDepthStencil, 1);
    CHECK_EQ_U32(rd32(pp + 12), 1);             /* BackBufferCount 0 -> 1 volta ao guest */
    CHECK_EQ_U32(rd32(pp + 28), GUEST_HWND);

    CHECK(strcmp(method(dev, 17)->name, "IDirect3DDevice9::Present") == 0);
    CHECK_EQ_U32(method(dev, 17)->arg_bytes, 20);
    CHECK(strcmp(method(dev, 43)->name, "IDirect3DDevice9::Clear") == 0);
    CHECK_EQ_U32(method(dev, 43)->arg_bytes, 28);
    CHECK(strcmp(method(dev, 118)->name, "IDirect3DDevice9::CreateQuery") == 0);
    /* Sem handler (retorno float no x87): o mesmo thunk-que-aborta do RegisterSoftwareDevice. */
    CHECK(strcmp(method(dev, 80)->name, "IDirect3DDevice9::GetNPatchMode") == 0);
    CHECK(method(dev, 80)->fn == method(d3d, 3)->fn);
    CHECK(method(dev, 80)->fn != method(dev, 43)->fn);

    uint32_t cp = gbuf(16);
    CHECK_EQ_U32(vcall(dev, 9, 1, &cp), D3D_OK);   /* GetCreationParameters */
    CHECK_EQ_U32(rd32(cp + 8), GUEST_HWND);
    CHECK_EQ_U32(rd32(cp + 12), D3DCREATE_HARDWARE_VERTEXPROCESSING);

    const uint32_t clr[6] = {0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF00FF00u, fbits(1.0f), 0};
    CHECK_EQ_U32(vcall(dev, 43, 6, clr), D3D_OK);
    CHECK(rec.z == 1.0f);
    CHECK_EQ_U32(rec.color, 0xFF00FF00u);
    const uint32_t srs[2] = {D3DRS_ZENABLE, 1};
    CHECK_EQ_U32(vcall(dev, 57, 2, srs), D3D_OK);
    CHECK_EQ_U32(rec.rs, D3DRS_ZENABLE);

    /* Identidade: o mesmo objeto do host é o mesmo objeto do guest. */
    uint32_t live0 = re5_d3d9_live_objects(), pbb = gbuf(4);
    const uint32_t gbb[4] = {0, 0, D3DBACKBUFFER_TYPE_MONO, pbb};
    CHECK_EQ_U32(vcall(dev, 18, 4, gbb), D3D_OK);
    uint32_t bb = rd32(pbb);
    CHECK_EQ_U32(vcall(dev, 18, 4, gbb), D3D_OK);
    CHECK_EQ_U32(rd32(pbb), bb);
    CHECK_EQ_U32(re5_d3d9_live_objects(), live0 + 1);
    CHECK_EQ_U32(g_bb.refs, 3);
    CHECK_EQ_U32(vcall(bb, 2, 0, NULL), 1);   /* Release: contagem do guest */
    CHECK_EQ_U32(vcall(bb, 2, 0, NULL), 0);
    CHECK_EQ_U32(g_bb.refs, 1);
    CHECK(re5_d3d9_host_object(bb) == NULL);   /* objeto-sombra liberado */
    CHECK_EQ_U32(re5_d3d9_live_objects(), live0);

    /* QueryInterface da própria interface: o mesmo objeto, sem perguntar ao host. */
    uint32_t iid = gbuf(16), pq = gbuf(4);
    const uint8_t iid_dev[16] = {0x96, 0x3b, 0x22, 0xd0, 0x7a, 0xbf, 0xfd, 0x43,
                                 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb};
    memcpy(re5_g2h(iid), iid_dev, 16);
    const uint32_t qi[2] = {iid, pq};
    CHECK_EQ_U32(vcall(dev, 0, 2, qi), S_OK);
    CHECK_EQ_U32(rd32(pq), dev);
    CHECK_EQ_U32(vcall(dev, 1, 0, NULL), 3);   /* AddRef: 1 (CreateDevice) + 1 (QI) + 1 */
    CHECK_EQ_U32(vcall(dev, 2, 0, NULL), 2);
    CHECK_EQ_U32(vcall(dev, 2, 0, NULL), 1);

    /* Vertex buffer: sombra persistente na memória do guest. */
    uint32_t pvb = gbuf(4);
    const uint32_t cvb[6] = {256, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, pvb, 0};
    CHECK_EQ_U32(vcall(dev, 26, 6, cvb), D3D_OK);
    uint32_t vb = rd32(pvb), pdata = gbuf(4);
    const uint32_t lk[4] = {0, 0, pdata, D3DLOCK_DISCARD};
    CHECK_EQ_U32(vcall(vb, 11, 4, lk), D3D_OK);
    uint32_t data = rd32(pdata);
    CHECK(in_guest(data));
    memset(re5_g2h(data), 0xAB, 256);
    CHECK_EQ_U32(g_vb.mem[0], 0);              /* nada chega ao host antes do Unlock */
    CHECK_EQ_U32(vcall(vb, 12, 0, NULL), D3D_OK);
    CHECK_EQ_U32(g_vb.mem[0], 0xAB);
    CHECK_EQ_U32(g_vb.mem[255], 0xAB);
    const uint32_t lk2[4] = {64, 16, pdata, D3DLOCK_NOOVERWRITE};
    CHECK_EQ_U32(vcall(vb, 11, 4, lk2), D3D_OK);
    CHECK_EQ_U32(rd32(pdata), data + 64);
    CHECK_EQ_U32(rd8(data + 64), 0xAB);        /* a sombra guarda o que o guest escreveu */
    memset(re5_g2h(data + 64), 0xCD, 16);
    CHECK_EQ_U32(vcall(vb, 12, 0, NULL), D3D_OK);
    CHECK_EQ_U32(g_vb.mem[64], 0xCD);
    CHECK_EQ_U32(g_vb.mem[79], 0xCD);
    CHECK_EQ_U32(g_vb.mem[80], 0xAB);
    CHECK_EQ_U32(g_vb.locks, 2);
    CHECK_EQ_U32(g_vb.unlocks, 2);

    /* Textura: bloco por trava, com o pitch do host, cheio do conteúdo do host. */
    for (uint32_t i = 0; i < sizeof g_tex.mem; i++) g_tex.mem[i] = (uint8_t)i;
    uint32_t ptex = gbuf(4);
    const uint32_t ctx[8] = {8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, ptex, 0};
    CHECK_EQ_U32(vcall(dev, 23, 8, ctx), D3D_OK);
    uint32_t tex = rd32(ptex), lr = gbuf(8);
    const uint32_t ltx[4] = {0, lr, 0, 0};
    CHECK_EQ_U32(vcall(tex, 19, 4, ltx), D3D_OK);
    CHECK_EQ_U32(rd32(lr), TEX_PITCH);
    uint32_t bits = rd32(lr + 4);
    CHECK(in_guest(bits));
    CHECK_EQ_U32(rd8(bits + TEX_PITCH + 3), TEX_PITCH + 3);
    wr32(bits + 7 * TEX_PITCH, 0x11223344u);
    const uint32_t utx = 0;
    CHECK_EQ_U32(vcall(tex, 20, 1, &utx), D3D_OK);
    uint32_t host_px;
    memcpy(&host_px, g_tex.mem + 7 * TEX_PITCH, 4);
    CHECK_EQ_U32(host_px, 0x11223344u);
    uint32_t rc = gbuf(16);
    const int32_t r[4] = {2, 2, 4, 4};   /* left, top, right, bottom */
    memcpy(re5_g2h(rc), r, 16);
    const uint32_t ltx2[4] = {0, lr, rc, 0};
    CHECK_EQ_U32(vcall(tex, 19, 4, ltx2), D3D_OK);
    bits = rd32(lr + 4);
    CHECK_EQ_U32(rd8(bits), (2 * TEX_PITCH + 8) & 0xFF);   /* (2, 2) do host */
    memset(re5_g2h(bits), 0xEE, 8);
    memset(re5_g2h(bits + TEX_PITCH), 0xEE, 8);
    CHECK_EQ_U32(vcall(tex, 20, 1, &utx), D3D_OK);
    CHECK_EQ_U32(g_tex.mem[2 * TEX_PITCH + 8], 0xEE);
    CHECK_EQ_U32(g_tex.mem[3 * TEX_PITCH + 15], 0xEE);
    CHECK_EQ_U32(g_tex.mem[3 * TEX_PITCH + 16], (3 * TEX_PITCH + 16) & 0xFF);   /* fora do retângulo */
    CHECK_EQ_U32(g_tex.unlocks, 2);

    /* Textura com fallback DXT (CPU) para GPUs móveis sem textureCompressionBC (Mali) */
#if defined(_WIN32)
    _putenv("RE5_D3D9_CPU_DXT=1");
#else
    setenv("RE5_D3D9_CPU_DXT", "1", 1);
#endif
    uint32_t pdxt_tex = gbuf(4);
    const uint32_t ctx_dxt[8] = {8, 8, 1, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, pdxt_tex, 0};
    CHECK_EQ_U32(vcall(dev, 23, 8, ctx_dxt), D3D_OK);
    uint32_t dxt_tex = rd32(pdxt_tex);
    CHECK(dxt_tex != 0);

    /* GetLevelDesc deve reportar o formato original DXT5 ao guest */
    uint32_t pdesc = gbuf(32);
    const uint32_t gld[2] = {0, pdesc};
    CHECK_EQ_U32(vcall(dxt_tex, 17, 2, gld), D3D_OK);
    CHECK_EQ_U32(rd32(pdesc), D3DFMT_DXT5);

    /* GetSurfaceLevel propaga o formato DXT5 */
    uint32_t psurf = gbuf(4);
    const uint32_t gsl[2] = {0, psurf};
    CHECK_EQ_U32(vcall(dxt_tex, 18, 2, gsl), D3D_OK);
    uint32_t surf = rd32(psurf);
    CHECK(surf != 0);
    const uint32_t gsd[1] = {pdesc};
    CHECK_EQ_U32(vcall(surf, 12, 1, gsd), D3D_OK);
    CHECK_EQ_U32(rd32(pdesc), D3DFMT_DXT5);

    /* LockRect deve devolver pitch do DXT5 ((8/4)*16 = 32 bytes por linha) */
    uint32_t dxt_lr = gbuf(8);
    const uint32_t dxt_ltx[4] = {0, dxt_lr, 0, 0};
    CHECK_EQ_U32(vcall(dxt_tex, 19, 4, dxt_ltx), D3D_OK);
    CHECK_EQ_U32(rd32(dxt_lr), 32);
    uint32_t dxt_bits = rd32(dxt_lr + 4);
    CHECK(in_guest(dxt_bits));

    /* Preenche o 1º bloco (4x4) com DXT5 vermelho opaco */
    uint8_t *raw_dxt = (uint8_t *)re5_g2h(dxt_bits);
    raw_dxt[0] = 255; raw_dxt[1] = 0;
    memset(raw_dxt + 2, 0, 6);
    uint16_t red = 0xF800;
    memcpy(raw_dxt + 8, &red, 2);
    memcpy(raw_dxt + 10, &red, 2);
    memset(raw_dxt + 12, 0, 4);

    CHECK_EQ_U32(vcall(dxt_tex, 20, 1, &utx), D3D_OK);
    CHECK_EQ_U32(g_tex.mem[3], 255);
#if defined(_WIN32)
    _putenv("RE5_D3D9_CPU_DXT=0");
#else
    setenv("RE5_D3D9_CPU_DXT", "0", 1);
#endif

    /* Volume: fatias com os pitches do host; o que fica entre as fatias não é tocado. */
    for (uint32_t i = 0; i < sizeof g_vol.mem; i++) g_vol.mem[i] = (uint8_t)i;
    uint32_t pvol = gbuf(4);
    const uint32_t cvt[9] = {4, 4, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, pvol, 0};
    CHECK_EQ_U32(vcall(dev, 24, 9, cvt), D3D_OK);
    uint32_t vol = rd32(pvol), lb = gbuf(12);
    const uint32_t lbx[4] = {0, lb, 0, 0};
    CHECK_EQ_U32(vcall(vol, 19, 4, lbx), D3D_OK);
    CHECK_EQ_U32(rd32(lb), VOL_ROW);
    CHECK_EQ_U32(rd32(lb + 4), VOL_SLICE);
    uint32_t vbits = rd32(lb + 8);
    CHECK(in_guest(vbits));
    CHECK_EQ_U32(rd8(vbits + VOL_SLICE + VOL_ROW + 5), (VOL_SLICE + VOL_ROW + 5) & 0xFF);   /* conteúdo do host */
    wr32(vbits + VOL_SLICE + 3 * VOL_ROW + 12, 0xA1B2C3D4u);   /* fatia 1, linha 3, pixel 3 */
    wr8(vbits + 3 * VOL_ROW + 16, 0x55);                       /* entre linhas: não é do volume */
    CHECK_EQ_U32(vcall(vol, 20, 1, &utx), D3D_OK);
    uint32_t vpx;
    memcpy(&vpx, g_vol.mem + VOL_SLICE + 3 * VOL_ROW + 12, 4);
    CHECK_EQ_U32(vpx, 0xA1B2C3D4u);
    CHECK_EQ_U32(g_vol.mem[3 * VOL_ROW + 16], (3 * VOL_ROW + 16) & 0xFF);
    uint32_t pbox = gbuf(24);
    const uint32_t bx[6] = {1, 1, 3, 2, 1, 2};   /* left, top, right, bottom, front, back */
    memcpy(re5_g2h(pbox), bx, 24);
    const uint32_t lbx2[4] = {0, lb, pbox, 0};
    CHECK_EQ_U32(vcall(vol, 19, 4, lbx2), D3D_OK);
    CHECK_EQ_U32(rd8(rd32(lb + 8)), (VOL_SLICE + VOL_ROW + 4) & 0xFF);
    CHECK_EQ_U32(vcall(vol, 20, 1, &utx), D3D_OK);
    CHECK_EQ_U32(g_vol.locks, 2);
    CHECK_EQ_U32(g_vol.unlocks, 2);

    /* Objetos de entrada: o objeto do host (BaseTexture aceita a textura); NULL passa NULL. */
    const uint32_t st[2] = {0, tex};
    CHECK_EQ_U32(vcall(dev, 65, 2, st), D3D_OK);
    CHECK(rec.tex == (IDirect3DBaseTexture9 *)&g_tex);
    const uint32_t st0[2] = {0, 0};
    CHECK_EQ_U32(vcall(dev, 65, 2, st0), D3D_OK);
    CHECK(rec.tex == NULL);

    /* Present: HWND 0 = a janela do device; contagem para o marco M8. */
    const uint32_t pr[4] = {0, 0, 0, 0};
    CHECK_EQ_U32(vcall(dev, 17, 4, pr), D3D_OK);
    CHECK_EQ_U32(rec.presents, 1);
    CHECK(rec.present_hwnd == NULL);
    CHECK_EQ_U32(re5_d3d9_present_count(), 1);
    CHECK_DONE();
}
