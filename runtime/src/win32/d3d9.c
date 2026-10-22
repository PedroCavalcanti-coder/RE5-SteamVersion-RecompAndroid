/*
 * d3d9.c - Direct3D 9: ponte COM para o Direct3D do host (Fase 4, etapa 4.2;
 * decisão D4). Ver re5/d3d9_bridge.h para o modelo de objeto.
 *
 *   - as vtables (uma por interface, compartilhada por todos os objetos daquele
 *     tipo) são thunks stdcall gerados por scripts/gen_d3d9_bridge.py a partir
 *     do d3d9.h: os métodos de tipos planos são repassados automaticamente; o
 *     resto tem handler aqui ou aborta com o nome (D8);
 *   - o mesmo objeto do host é sempre o mesmo objeto do guest (GetBackBuffer
 *     duas vezes, GetTexture, GetDevice): um hash host -> objeto. A contagem de
 *     referências é do objeto-sombra, e cada referência do guest é uma do host;
 *     na última Release o objeto-sombra é liberado;
 *   - Lock/Unlock: o guest escreve num buffer-sombra na memória do guest,
 *     copiado para a memória do host no Unlock. Vertex/index buffers têm uma
 *     sombra persistente do tamanho do buffer: o que o guest escreveu fica
 *     nela, então travas sem DISCARD não leem a memória do host (muitas vezes
 *     write-combined). Texturas e superfícies ganham um bloco por trava,
 *     preenchido a partir do host;
 *   - D3DPRESENT_PARAMETERS e D3DDEVICE_CREATION_PARAMETERS levam HWND: o do
 *     guest (falso, do user32) vira a janela do host pelo backend;
 *   - Present solta o lock do guest durante a espera do host (D5).
 *
 * Tudo aqui roda com o lock do guest; o estado da ponte não tem lock próprio.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "re5/d3d9_bridge.h"
#include "re5/user32.h"
#include "kernel32_internal.h"

#if defined(RE5_HOST_WINDOWS)
#include <windows.h>
#endif
#include <d3d9.h>

#define D9_MAGIC    0x39443344u   /* 'D3D9' */
#define D9_OBJ_SIZE 16u

/* Usados pela tabela gerada (definidos mais abaixo). */
static void *d9_self(CPU *c, int kind);
static void *d9_in(uint32_t va, int kind, const char *what);
static void d9_out(uint32_t pp, int ok, void *host, int kind);

static void *d9_ptr(uint32_t va) { return va ? re5_g2h(va) : NULL; }

static float d9_f32(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static HANDLE *d9_no_shared(uint32_t va, const char *what)
{
    if (va) re5_fatal("d3d9", "%s com pSharedHandle (recurso compartilhado) sem implementação", what);
    return NULL;
}

#include "re5_d3d9_methods.inc"

/* ---- objetos ---- */

typedef struct {
    uint32_t sub;        /* texturas: nível + 32 * face; buffers e superfícies: 0 */
    uint32_t guest;      /* o que o guest recebeu */
    uint8_t *host;       /* o que o Direct3D do host deu */
    uint32_t pitch, row_bytes, rows, flags;
    uint32_t slices, slice_pitch;   /* volumes: fatias de `rows` linhas; o resto: 1 */
    int      owned;      /* 1: bloco só desta trava (texturas); 0: dentro da sombra do buffer */
} d9_lock;

typedef struct d9_obj {
    IUnknown      *host;
    uint32_t       guest, handle;
    d9_kind        kind;
    uint32_t       refs;                  /* referências que o guest tem */
    struct d9_obj *hnext;                 /* hash host -> objeto */
    uint32_t       shadow, shadow_size;   /* vertex/index buffer */
    d9_lock       *locks;
    uint32_t       nlocks, caplocks;
    uint32_t       focus_hwnd, behavior;  /* device: o que o guest passou ao CreateDevice */
} d9_obj;

#define D9_HASH 4096u
static d9_obj *g_hash[D9_HASH];
static uint32_t g_vtables[D9K_COUNT];
static uint32_t g_live, g_presents;
static const re5_d3d9_backend *g_backend;

static const re5_d3d9_backend *backend(void)
{
    if (!g_backend) g_backend = re5_d3d9_host_backend();
    return g_backend;
}

void re5_d3d9_set_backend(const re5_d3d9_backend *b) { g_backend = b; }
uint32_t re5_d3d9_present_count(void) { return g_presents; }
uint32_t re5_d3d9_live_objects(void) { return g_live; }

static uint32_t heap(void) { return k32_process_heap(); }

static uint32_t hash_of(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (uint32_t)((v >> 4) ^ (v >> 16) ^ (v >> 28)) & (D9_HASH - 1u);
}

static const char *kind_name(int kind)
{
    for (uint32_t i = 0; i < D9_IFACE_COUNT; i++)
        if ((int)k_d9_ifaces[i].kind == kind) return k_d9_ifaces[i].iface;
    return "?";
}

/* Um parâmetro do tipo `want` aceita um objeto do tipo `have`? */
static int kind_accepts(int want, int have)
{
    if (want == have) return 1;
    int texture = have == D9K_TEXTURE || have == D9K_CUBETEXTURE || have == D9K_VOLUMETEXTURE;
    if (want == D9K_BASETEXTURE) return texture;
    if (want == D9K_RESOURCE)
        return texture || have == D9K_SURFACE || have == D9K_VERTEXBUFFER || have == D9K_INDEXBUFFER;
    return 0;
}

static d9_obj *obj_of(uint32_t va)
{
    if (!va || !re5_mem_is_mapped(va, D9_OBJ_SIZE) || rd32(va + 12) != D9_MAGIC) return NULL;
    d9_obj *o = re5_handle_get(rd32(va + 4), RE5_H_COM);
    return o && o->guest == va ? o : NULL;
}

void *re5_d3d9_host_object(uint32_t guest_obj)
{
    d9_obj *o = obj_of(guest_obj);
    return o ? o->host : NULL;
}

static uint32_t vtable_of(int kind);

/* Objeto do guest para um objeto do host que acabou de dar uma referência. */
static uint32_t d9_wrap(void *host, int kind)
{
    if (!host) return 0;
    if (kind == D9K_BASETEXTURE) {
        IDirect3DBaseTexture9 *t = host;
        switch (t->lpVtbl->GetType(t)) {
        case D3DRTYPE_TEXTURE: kind = D9K_TEXTURE; break;
        case D3DRTYPE_CUBETEXTURE: kind = D9K_CUBETEXTURE; break;
        case D3DRTYPE_VOLUMETEXTURE: kind = D9K_VOLUMETEXTURE; break;
        default: re5_fatal("d3d9", "textura do host %p com tipo de recurso desconhecido", host);
        }
    }
    if (kind < 0 || kind >= D9K_COUNT) re5_fatal("d3d9", "objeto do host %p pedido como %s", host, kind_name(kind));
    uint32_t h = hash_of(host);
    for (d9_obj *o = g_hash[h]; o; o = o->hnext) {
        if (o->host != host) continue;
        if ((int)o->kind != kind)
            re5_fatal("d3d9", "objeto do host %p é %s no guest, agora pedido como %s", host, kind_name(o->kind),
                      kind_name(kind));
        o->refs++;
        return o->guest;
    }
    d9_obj *o = calloc(1, sizeof *o);
    if (!o) re5_fatal("d3d9", "sem memória");
    o->host = host;
    o->kind = (d9_kind)kind;
    o->refs = 1;
    o->guest = k32_heap_alloc(heap(), 0, D9_OBJ_SIZE);
    if (!o->guest) re5_fatal("d3d9", "sem memória do guest para um %s", kind_name(kind));
    o->handle = re5_handle_new(RE5_H_COM, o, NULL);
    wr32(o->guest, vtable_of(kind));
    wr32(o->guest + 4, o->handle);
    wr32(o->guest + 8, (uint32_t)kind);
    wr32(o->guest + 12, D9_MAGIC);
    o->hnext = g_hash[h];
    g_hash[h] = o;
    g_live++;
    return o->guest;
}

/* Última referência do guest: o objeto-sombra some (o do host já recebeu a Release). */
static void d9_retire(d9_obj *o)
{
    if (o->nlocks)
        RE5_WARN("d3d9", "%s 0x%08X liberado com %u trava(s) aberta(s)", kind_name(o->kind), o->guest, o->nlocks);
    for (uint32_t i = 0; i < o->nlocks; i++)
        if (o->locks[i].owned) k32_heap_free(heap(), o->locks[i].guest);
    if (o->shadow) k32_heap_free(heap(), o->shadow);
    for (d9_obj **p = &g_hash[hash_of(o->host)]; *p; p = &(*p)->hnext) {
        if (*p == o) {
            *p = o->hnext;
            break;
        }
    }
    re5_handle_close(o->handle);
    wr32(o->guest + 12, 0);   /* um this velho passa a ser recusado por obj_of */
    k32_heap_free(heap(), o->guest);
    free(o->locks);
    free(o);
    g_live--;
}

static RE5_NORETURN void not_ours(CPU *c, int kind, const d9_obj *o)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("d3d9", "%s: this=0x%08X não é um %s da ponte (é %s)", t ? t->name : "?", re5_arg(c, 0),
              kind_name(kind), o ? kind_name(o->kind) : "desconhecido");
}

static d9_obj *self_obj(CPU *c, int kind)
{
    d9_obj *o = obj_of(re5_arg(c, 0));
    if (!o || !kind_accepts(kind, o->kind)) not_ours(c, kind, o);
    return o;
}

static d9_obj *any_obj(CPU *c)
{
    d9_obj *o = obj_of(re5_arg(c, 0));
    if (!o) not_ours(c, -1, NULL);
    return o;
}

static void *d9_self(CPU *c, int kind) { return self_obj(c, kind)->host; }

static void *d9_in(uint32_t va, int kind, const char *what)
{
    if (!va) return NULL;
    d9_obj *o = obj_of(va);
    if (!o) re5_fatal("d3d9", "%s: 0x%08X não é um objeto D3D9 da ponte", what, va);
    if (!kind_accepts(kind, o->kind))
        re5_fatal("d3d9", "%s: 0x%08X é %s, esperado %s", what, va, kind_name(o->kind), kind_name(kind));
    return o->host;
}

static void d9_out(uint32_t pp, int ok, void *host, int kind)
{
    if (pp) wr32(pp, ok && host ? d9_wrap(host, kind) : 0);
}

/* ---- IUnknown (todas as interfaces) ---- */

static const uint32_t k_iid_unknown[11] = {0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};

static int guid_is(const uint8_t g[16], const uint32_t v[11])
{
    uint8_t b[16];
    uint32_t d1 = v[0];
    uint16_t d2 = (uint16_t)v[1], d3 = (uint16_t)v[2];
    memcpy(b, &d1, 4);
    memcpy(b + 4, &d2, 2);
    memcpy(b + 6, &d3, 2);
    for (int i = 0; i < 8; i++) b[8 + i] = (uint8_t)v[3 + i];
    return memcmp(b, g, 16) == 0;
}

static void guid_text(const uint8_t g[16], char *out, size_t n)
{
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    snprintf(out, n, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9], g[10], g[11],
             g[12], g[13], g[14], g[15]);
}

static void i_QueryInterface(CPU *c)
{
    d9_obj *o = any_obj(c);
    uint32_t riid = re5_arg(c, 1), ppv = re5_arg(c, 2);
    if (!ppv || !riid) {
        c->eax = (uint32_t)E_POINTER;
        return;
    }
    const uint8_t *g = re5_g2h(riid);
    int want = -2;
    if (guid_is(g, k_iid_unknown)) want = -1;
    for (uint32_t i = 0; i < D9_IFACE_COUNT && want == -2; i++)
        if (guid_is(g, k_d9_ifaces[i].iid)) want = (int)k_d9_ifaces[i].kind;
    if (want == -2) {
        char s[64];
        guid_text(g, s, sizeof s);
        re5_fatal("d3d9", "%s::QueryInterface(%s) sem implementação", kind_name(o->kind), s);
    }
    if (want == -1 || kind_accepts(want, o->kind)) {   /* a mesma interface ou uma base dela */
        o->host->lpVtbl->AddRef(o->host);
        o->refs++;
        wr32(ppv, o->guest);
        c->eax = (uint32_t)S_OK;
        return;
    }
    /* Outra interface D3D9: quem responde é o host. */
    GUID iid;
    memcpy(&iid, g, sizeof iid);
    void *r = NULL;
    HRESULT hr = o->host->lpVtbl->QueryInterface(o->host, &iid, &r);
    d9_out(ppv, SUCCEEDED(hr), r, want);
    c->eax = (uint32_t)hr;
}

static void i_AddRef(CPU *c)
{
    d9_obj *o = any_obj(c);
    o->host->lpVtbl->AddRef(o->host);
    c->eax = ++o->refs;
}

static void i_Release(CPU *c)
{
    d9_obj *o = any_obj(c);
    if (!o->refs) re5_fatal("d3d9", "%s::Release em 0x%08X sem referência", kind_name(o->kind), o->guest);
    o->host->lpVtbl->Release(o->host);
    c->eax = --o->refs;
    if (!o->refs) d9_retire(o);
}

/* ---- D3DPRESENT_PARAMETERS ---- */

/* Layout do guest: 14 DWORDs; hDeviceWindow (32 bits) em +28. */
#define PP_HWND 28u

/* O Direct3D do host apresenta sempre em janela (sem trocar o modo do monitor);
 * o guest continua vendo Windowed/refresh como pediu, e a tela cheia dele vira
 * uma janela sem bordas do tamanho do monitor (re5_user32_set_fullscreen). Tela cheia exclusiva só com RE5_FULLSCREEN=1: com o jogo ainda em
 * bring-up, uma tela cheia exclusiva travada prendeu o desktop do usuário
 * (2026-10-20, reinício forçado). */
static int force_windowed(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RE5_FULLSCREEN");
        v = !(e && *e && *e != '0');
        RE5_INFO("d3d9", v ? "tela cheia do guest apresentada em janela no host (RE5_FULLSCREEN=1 para exclusiva)"
                           : "RE5_FULLSCREEN: tela cheia exclusiva no host");
    }
    return v;
}

int re5_d3d9_pp_from_guest(uint32_t va, void *host)
{
    D3DPRESENT_PARAMETERS *p = host;
    memset(p, 0, sizeof *p);
    p->BackBufferWidth = rd32(va + 0);
    p->BackBufferHeight = rd32(va + 4);
    p->BackBufferFormat = (D3DFORMAT)rd32(va + 8);
    p->BackBufferCount = rd32(va + 12);
    p->MultiSampleType = (D3DMULTISAMPLE_TYPE)rd32(va + 16);
    p->MultiSampleQuality = rd32(va + 20);
    p->SwapEffect = (D3DSWAPEFFECT)rd32(va + 24);
    uint32_t hwnd = rd32(va + PP_HWND);
    p->hDeviceWindow = NULL;
    if (hwnd && !(p->hDeviceWindow = (HWND)backend()->window(hwnd))) return -1;
    p->Windowed = (BOOL)rd32(va + 32);
    p->EnableAutoDepthStencil = (BOOL)rd32(va + 36);
    p->AutoDepthStencilFormat = (D3DFORMAT)rd32(va + 40);
    p->Flags = rd32(va + 44);
    p->FullScreen_RefreshRateInHz = rd32(va + 48);
    p->PresentationInterval = rd32(va + 52);
    if (force_windowed()) {
        p->Windowed = TRUE;
        p->FullScreen_RefreshRateInHz = 0;
    }
    return 0;
}

void re5_d3d9_pp_to_guest(const void *host, uint32_t va)
{
    const D3DPRESENT_PARAMETERS *p = host;
    wr32(va + 0, p->BackBufferWidth);
    wr32(va + 4, p->BackBufferHeight);
    wr32(va + 8, (uint32_t)p->BackBufferFormat);
    wr32(va + 12, p->BackBufferCount);
    wr32(va + 16, (uint32_t)p->MultiSampleType);
    wr32(va + 20, p->MultiSampleQuality);
    wr32(va + 24, (uint32_t)p->SwapEffect);
    if (!force_windowed()) wr32(va + 32, (uint32_t)p->Windowed);
    wr32(va + 36, (uint32_t)p->EnableAutoDepthStencil);
    wr32(va + 40, (uint32_t)p->AutoDepthStencilFormat);
    wr32(va + 44, p->Flags);
    if (!force_windowed()) wr32(va + 48, p->FullScreen_RefreshRateInHz);
    wr32(va + 52, p->PresentationInterval);
}

static void pp_from_guest_or_die(uint32_t va, D3DPRESENT_PARAMETERS *p, const char *what)
{
    if (re5_d3d9_pp_from_guest(va, p) != 0)
        re5_fatal("d3d9", "%s: a janela 0x%X do guest não tem janela no host (RE5_HEADLESS?)", what,
                  rd32(va + PP_HWND));
}

static HWND host_hwnd(uint32_t hwnd, const char *what)
{
    if (!hwnd) return NULL;
    HWND h = (HWND)backend()->window(hwnd);
    if (!h) re5_fatal("d3d9", "%s: a janela 0x%X do guest não tem janela no host (RE5_HEADLESS?)", what, hwnd);
    return h;
}

/* ---- Direct3DCreate9 e IDirect3D9 ---- */

static void d_Direct3DCreate9(CPU *c)
{
    uint32_t sdk = re5_arg(c, 0);
    void *d3d = backend()->create9(sdk);
    RE5_INFO("d3d9", "Direct3DCreate9(SDK %u) pelo backend %s -> %s", sdk, backend()->name, d3d ? "ok" : "NULL");
    c->eax = d9_wrap(d3d, D9K_D3D9);
}

static void d_CreateDevice(CPU *c)
{
    IDirect3D9 *d3d = d9_self(c, D9K_D3D9);
    uint32_t adapter = re5_arg(c, 1), type = re5_arg(c, 2), focus = re5_arg(c, 3), flags = re5_arg(c, 4);
    uint32_t ppva = re5_arg(c, 5), out = re5_arg(c, 6);
    if (!ppva || !out) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest_or_die(ppva, &pp, "IDirect3D9::CreateDevice");
    HWND hfocus = host_hwnd(focus, "IDirect3D9::CreateDevice");
    /* FPU_PRESERVE: sem ele o Direct3D do host muda a precisão do x87 da
     * thread do host. O x87 do guest é emulado (D9) e não sente isso; o pedido
     * do guest é guardado e devolvido em GetCreationParameters. */
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, adapter, (D3DDEVTYPE)type, hfocus, flags | D3DCREATE_FPU_PRESERVE,
                                           &pp, &dev);
    re5_d3d9_pp_to_guest(&pp, ppva);
    RE5_INFO("d3d9",
             "CreateDevice(adaptador %u, tipo %u, foco 0x%X, flags 0x%X): %ux%u formato %u, %u back buffer(s), "
             "%s, swap %u, MSAA %u, profundidade %s/%u, intervalo 0x%X -> 0x%08X",
             adapter, type, focus, flags, pp.BackBufferWidth, pp.BackBufferHeight, (unsigned)pp.BackBufferFormat,
             pp.BackBufferCount,
             rd32(ppva + 32) ? "janela" : force_windowed() ? "tela cheia (no host: janela)" : "tela cheia",
             (unsigned)pp.SwapEffect,
             (unsigned)pp.MultiSampleType, pp.EnableAutoDepthStencil ? "sim" : "não",
             (unsigned)pp.AutoDepthStencilFormat, pp.PresentationInterval, (uint32_t)hr);
    d9_out(out, SUCCEEDED(hr), dev, D9K_DEVICE);
    if (SUCCEEDED(hr) && force_windowed()) re5_user32_set_fullscreen(rd32(ppva + PP_HWND) ? rd32(ppva + PP_HWND) : focus,
                                                                   !rd32(ppva + 32));
    if (SUCCEEDED(hr)) {
        d9_obj *o = obj_of(rd32(out));
        o->focus_hwnd = focus;
        o->behavior = flags;
    }
    c->eax = (uint32_t)hr;
}

/* ---- IDirect3DDevice9 ---- */

static void d_GetCreationParameters(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_DEVICE);
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)o->host;
    uint32_t out = re5_arg(c, 1);
    if (!out) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = dev->lpVtbl->GetCreationParameters(dev, &p);
    if (SUCCEEDED(hr)) {   /* UINT, D3DDEVTYPE, HWND (32 bits no guest), DWORD */
        wr32(out, p.AdapterOrdinal);
        wr32(out + 4, (uint32_t)p.DeviceType);
        wr32(out + 8, o->focus_hwnd);
        wr32(out + 12, o->behavior);
    }
    c->eax = (uint32_t)hr;
}

static void d_Reset(CPU *c)
{
    IDirect3DDevice9 *dev = d9_self(c, D9K_DEVICE);
    uint32_t ppva = re5_arg(c, 1);
    if (!ppva) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest_or_die(ppva, &pp, "IDirect3DDevice9::Reset");
    HRESULT hr = dev->lpVtbl->Reset(dev, &pp);
    re5_d3d9_pp_to_guest(&pp, ppva);
    /* Tela cheia do guest = janela sem bordas do tamanho do monitor no host. */
    if (SUCCEEDED(hr) && force_windowed() && rd32(ppva + PP_HWND)) re5_user32_set_fullscreen(rd32(ppva + PP_HWND), !rd32(ppva + 32));
    RE5_INFO("d3d9", "Reset: %ux%u formato %u, %s -> 0x%08X", pp.BackBufferWidth, pp.BackBufferHeight,
             (unsigned)pp.BackBufferFormat,
             rd32(ppva + 32) ? "janela" : force_windowed() ? "tela cheia (no host: janela)" : "tela cheia",
             (uint32_t)hr);
    c->eax = (uint32_t)hr;
}

/* RE5_FPS_LOG=1: uma linha por segundo com os Present daquele segundo (medir
 * o benchmark sem o perfil, que custa desempenho). */
static void fps_log(void)
{
    static int on = -1;
    static uint32_t t0, n0;
    if (on < 0) on = getenv("RE5_FPS_LOG") && *getenv("RE5_FPS_LOG") && *getenv("RE5_FPS_LOG") != '0';
    if (!on) return;
    uint32_t now = SDL_GetTicks();
    if (!t0) { t0 = now; n0 = g_presents; return; }
    if (now - t0 >= 1000u) {
        RE5_INFO("d3d9", "fps %.1f (Present %u)", (double)(g_presents - n0) * 1000.0 / (double)(now - t0), g_presents);
        t0 = now;
        n0 = g_presents;
    }
}

/* RE5_D3D9_SHOT=300,600: o back buffer desses Present (contados a partir de 1)
 * vai para <RE5_D3D9_SHOT_DIR ou .>/frame_<N>.bmp, antes de ser apresentado.
 * Evidência de quadro e base para comparar quadros sem olhar a tela. */
static int shot_wanted(uint32_t n)
{
    const char *e = getenv("RE5_D3D9_SHOT");
    for (const char *p = e; p && *p;) {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p) break;
        if (v == n) return 1;
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void shot(IDirect3DDevice9 *dev, uint32_t n)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    const char *dir = getenv("RE5_D3D9_SHOT_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%u.bmp", dir && *dir ? dir : ".", n);
    HRESULT hr = dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr)) hr = bb->lpVtbl->GetDesc(bb, &d);
    if (SUCCEEDED(hr) && d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr)) hr = dev->lpVtbl->GetRenderTargetData(dev, bb, sys);
    if (SUCCEEDED(hr)) hr = sys->lpVtbl->LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    FILE *f = SUCCEEDED(hr) ? fopen(path, "wb") : NULL;
    if (f) {   /* BMP de 32 bits de cima para baixo (altura negativa) */
        uint8_t h[54] = {'B', 'M'};
        uint32_t row = d.Width * 4u;
        put_le(h + 2, 54u + row * d.Height, 4);
        put_le(h + 10, 54, 4);
        put_le(h + 14, 40, 4);
        put_le(h + 18, d.Width, 4);
        put_le(h + 22, (uint32_t)-(int32_t)d.Height, 4);
        put_le(h + 26, 1, 2);
        put_le(h + 28, 32, 2);
        fwrite(h, 1, sizeof h, f);
        for (uint32_t y = 0; y < d.Height; y++) fwrite((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, 1, row, f);
        fclose(f);
        RE5_INFO("d3d9", "Present %u: back buffer %ux%u em %s", n, d.Width, d.Height, path);
    } else {
        RE5_WARN("d3d9", "Present %u: captura do back buffer falhou (0x%08X)", n, (uint32_t)hr);
    }
    if (sys && SUCCEEDED(hr)) sys->lpVtbl->UnlockRect(sys);
    if (sys) sys->lpVtbl->Release(sys);
    if (bb) bb->lpVtbl->Release(bb);
}

static void d_Present(CPU *c)
{
    IDirect3DDevice9 *dev = d9_self(c, D9K_DEVICE);
    const RECT *src = d9_ptr(re5_arg(c, 1)), *dst = d9_ptr(re5_arg(c, 2));
    HWND over = host_hwnd(re5_arg(c, 3), "IDirect3DDevice9::Present");
    const RGNDATA *dirty = d9_ptr(re5_arg(c, 4));
    HRESULT hr;
    if (shot_wanted(g_presents + 1)) shot(dev, g_presents + 1);
    {
        RE5_BLOCKING_BEGIN();
        hr = dev->lpVtbl->Present(dev, src, dst, over, dirty);
        RE5_BLOCKING_END();
    }
    if (++g_presents == 1) RE5_INFO("d3d9", "primeiro Present -> 0x%08X", (uint32_t)hr);
    else if (g_presents == 100u || g_presents == 1000u || g_presents == 10000u || g_presents % 100000u == 0)
        RE5_INFO("d3d9", "%u Present (último -> 0x%08X)", g_presents, (uint32_t)hr);
    re5_user32_script_tick(g_presents);
    fps_log();
    c->eax = (uint32_t)hr;
}

/* ---- SetPrivateData (recursos e volumes): o host copia o dado ----
 * O IDirect3DVolume9 não herda de IDirect3DResource9, mas o SetPrivateData
 * está no mesmo slot (4) com a mesma assinatura. */

static void r_SetPrivateData(CPU *c)
{
    IDirect3DResource9 *r = (IDirect3DResource9 *)any_obj(c)->host;
    uint32_t flags = re5_arg(c, 4);
    if (flags & D3DSPD_IUNKNOWN) re5_fatal("d3d9", "SetPrivateData com D3DSPD_IUNKNOWN sem implementação");
    c->eax = (uint32_t)r->lpVtbl->SetPrivateData(r, (REFGUID)d9_ptr(re5_arg(c, 1)), d9_ptr(re5_arg(c, 2)),
                                                 re5_arg(c, 3), flags);
}

/* ---- travas ---- */

static d9_lock *lock_push(d9_obj *o)
{
    if (o->nlocks == o->caplocks) {
        uint32_t cap = o->caplocks ? o->caplocks * 2 : 4;
        d9_lock *n = realloc(o->locks, cap * sizeof *n);
        if (!n) re5_fatal("d3d9", "sem memória");
        o->locks = n;
        o->caplocks = cap;
    }
    d9_lock *l = &o->locks[o->nlocks++];
    memset(l, 0, sizeof *l);
    return l;
}

static int lock_find(const d9_obj *o, uint32_t sub)
{
    for (uint32_t i = o->nlocks; i-- > 0;)
        if (o->locks[i].sub == sub) return (int)i;
    return -1;
}

/* Copia a trava de volta para o host (se o guest podia escrever) e a remove. */
static void lock_finish(d9_obj *o, int idx)
{
    d9_lock l = o->locks[idx];   /* a trava sai da lista antes da cópia, que roda sem o lock */
    o->locks[idx] = o->locks[--o->nlocks];
    if (!(l.flags & D3DLOCK_READONLY)) {
        /* Sem o lock do HLE: a cópia de um buffer dinâmico de MBs com ele na mão
         * era 22% das esperas das outras threads no carregamento (RE5_PROFILE).
         * O recurso continua travado no host até o Unlock de quem chamou. */
        RE5_BLOCKING_BEGIN();
        const uint8_t *g = re5_g2h(l.guest);
        for (uint32_t z = 0; z < (l.slices ? l.slices : 1); z++) {
            size_t base = (size_t)z * l.slice_pitch;
            for (uint32_t r = 0; r < l.rows; r++)
                memcpy(l.host + base + (size_t)r * l.pitch, g + base + (size_t)r * l.pitch, l.row_bytes);
        }
        RE5_BLOCKING_END();
    }
    if (l.owned) k32_heap_free(heap(), l.guest);
}

static void b_Lock(CPU *c)
{
    d9_obj *o = any_obj(c);
    if (o->kind != D9K_VERTEXBUFFER && o->kind != D9K_INDEXBUFFER) not_ours(c, D9K_VERTEXBUFFER, o);
    uint32_t off = re5_arg(c, 1), size = re5_arg(c, 2), pp = re5_arg(c, 3), flags = re5_arg(c, 4);
    if (!pp) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    uint32_t total;
    void *hp = NULL;
    HRESULT hr;
    if (o->kind == D9K_VERTEXBUFFER) {
        IDirect3DVertexBuffer9 *b = (IDirect3DVertexBuffer9 *)o->host;
        D3DVERTEXBUFFER_DESC d;
        b->lpVtbl->GetDesc(b, &d);
        total = d.Size;
        hr = b->lpVtbl->Lock(b, off, size, &hp, flags);
    } else {
        IDirect3DIndexBuffer9 *b = (IDirect3DIndexBuffer9 *)o->host;
        D3DINDEXBUFFER_DESC d;
        b->lpVtbl->GetDesc(b, &d);
        total = d.Size;
        hr = b->lpVtbl->Lock(b, off, size, &hp, flags);
    }
    if (FAILED(hr)) {
        wr32(pp, 0);
        c->eax = (uint32_t)hr;
        return;
    }
    if (!size) size = off < total ? total - off : 0;   /* 0 = até o fim (0, 0 = o buffer inteiro) */
    if (off > total || size > total - off)
        re5_fatal("d3d9", "%s::Lock(%u, %u) fora do buffer de %u bytes, e o host aceitou", kind_name(o->kind), off,
                  re5_arg(c, 2), total);
    if (!o->shadow) {
        o->shadow = k32_heap_alloc(heap(), HEAP_ZERO_MEMORY, total ? total : 1);
        if (!o->shadow) re5_fatal("d3d9", "sem memória do guest para a sombra de um buffer de %u bytes", total);
        o->shadow_size = total;
        /* Primeira trava sem DISCARD: o que o host já tem nessa faixa vale. */
        if (!(flags & D3DLOCK_DISCARD)) memcpy(re5_g2h(o->shadow + off), hp, size);
    }
    d9_lock *l = lock_push(o);
    l->guest = o->shadow + off;
    l->host = hp;
    l->pitch = l->row_bytes = size;
    l->rows = 1;
    l->flags = flags;
    wr32(pp, l->guest);
    c->eax = (uint32_t)hr;
}

static void b_Unlock(CPU *c)
{
    d9_obj *o = any_obj(c);
    if (o->kind != D9K_VERTEXBUFFER && o->kind != D9K_INDEXBUFFER) not_ours(c, D9K_VERTEXBUFFER, o);
    if (o->nlocks) lock_finish(o, (int)o->nlocks - 1);   /* travas aninhadas: a última aberta */
    if (o->kind == D9K_VERTEXBUFFER) {
        IDirect3DVertexBuffer9 *b = (IDirect3DVertexBuffer9 *)o->host;
        c->eax = (uint32_t)b->lpVtbl->Unlock(b);
    } else {
        IDirect3DIndexBuffer9 *b = (IDirect3DIndexBuffer9 *)o->host;
        c->eax = (uint32_t)b->lpVtbl->Unlock(b);
    }
}

/* Bloco (largura x altura em pixels, bytes) de cada formato que dá para travar. */
static int format_block(D3DFORMAT f, uint32_t *bw, uint32_t *bh, uint32_t *bytes)
{
    *bw = *bh = 1;
    switch ((uint32_t)f) {
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4: *bytes = 1; return 0;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_D16_LOCKABLE: case D3DFMT_L16: case D3DFMT_R16F:
        *bytes = 2; return 0;
    case D3DFMT_R8G8B8: *bytes = 3; return 0;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_A2W10V10U10: case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_R32F: case D3DFMT_G16R16F:
        *bytes = 4; return 0;
    case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:
        *bytes = 8; return 0;
    case D3DFMT_A32B32G32R32F: *bytes = 16; return 0;
    case D3DFMT_DXT1: *bw = *bh = 4; *bytes = 8; return 0;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *bw = *bh = 4; *bytes = 16; return 0;
    case MAKEFOURCC('A', 'T', 'I', '1'): *bw = *bh = 4; *bytes = 8; return 0;
    case MAKEFOURCC('A', 'T', 'I', '2'): *bw = *bh = 4; *bytes = 16; return 0;
    default: return -1;
    }
}

/* Trava de pixels já aberta no host: bloco no guest com o mesmo pitch, cheio
 * do conteúdo do host (exceto com DISCARD), e D3DLOCKED_RECT do guest. */
static void lock_pixels(d9_obj *o, uint32_t sub, const D3DSURFACE_DESC *desc, const D3DLOCKED_RECT *lr,
                        const RECT *rect, uint32_t flags, uint32_t out)
{
    uint32_t bw, bh, bytes;
    if (format_block(desc->Format, &bw, &bh, &bytes) != 0)
        re5_fatal("d3d9", "%s::LockRect com formato %u sem implementação", kind_name(o->kind), (unsigned)desc->Format);
    uint32_t w = rect ? (uint32_t)(rect->right - rect->left) : desc->Width;
    uint32_t h = rect ? (uint32_t)(rect->bottom - rect->top) : desc->Height;
    uint32_t rows = (h + bh - 1) / bh, row_bytes = (w + bw - 1) / bw * bytes;
    if (lr->Pitch <= 0 || (uint32_t)lr->Pitch < row_bytes || !rows)
        re5_fatal("d3d9", "%s::LockRect: pitch %d do host para %u bytes por linha", kind_name(o->kind), lr->Pitch,
                  row_bytes);
    uint32_t pitch = (uint32_t)lr->Pitch;
    uint64_t size = (uint64_t)pitch * (rows - 1) + row_bytes;
    if (size > 0x40000000u) re5_fatal("d3d9", "trava de %llu bytes", (unsigned long long)size);
    uint32_t g = k32_heap_alloc(heap(), 0, (uint32_t)size);
    if (!g) re5_fatal("d3d9", "sem memória do guest para travar %llu bytes", (unsigned long long)size);
    if (!(flags & D3DLOCK_DISCARD))
        for (uint32_t r = 0; r < rows; r++)
            memcpy((uint8_t *)re5_g2h(g) + (size_t)r * pitch, (const uint8_t *)lr->pBits + (size_t)r * pitch, row_bytes);
    d9_lock *l = lock_push(o);
    l->sub = sub;
    l->guest = g;
    l->host = lr->pBits;
    l->pitch = pitch;
    l->row_bytes = row_bytes;
    l->rows = rows;
    l->flags = flags;
    l->owned = 1;
    wr32(out, pitch);      /* D3DLOCKED_RECT do guest: INT Pitch; void *pBits (32 bits) */
    wr32(out + 4, g);
}

static const RECT *rect_from(uint32_t va, RECT *tmp)
{
    if (!va) return NULL;
    memcpy(tmp, re5_g2h(va), sizeof *tmp);   /* 4 LONGs de 32 bits nos dois lados */
    return tmp;
}

static void t_LockRect(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_TEXTURE);
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)o->host;
    uint32_t level = re5_arg(c, 1), out = re5_arg(c, 2), flags = re5_arg(c, 4);
    RECT tmp;
    const RECT *rect = rect_from(re5_arg(c, 3), &tmp);
    if (!out || lock_find(o, level) >= 0) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    HRESULT hr = t->lpVtbl->GetLevelDesc(t, level, &desc);
    if (SUCCEEDED(hr)) hr = t->lpVtbl->LockRect(t, level, &lr, rect, flags);
    if (SUCCEEDED(hr)) lock_pixels(o, level, &desc, &lr, rect, flags, out);
    c->eax = (uint32_t)hr;
}

static void t_UnlockRect(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_TEXTURE);
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)o->host;
    uint32_t level = re5_arg(c, 1);
    int i = lock_find(o, level);
    if (i >= 0) lock_finish(o, i);
    c->eax = (uint32_t)t->lpVtbl->UnlockRect(t, level);
}

/* Volume: o mesmo que lock_pixels, fatia a fatia (D3DLOCKED_BOX do guest: RowPitch, SlicePitch, pBits). */
static void lock_box(d9_obj *o, uint32_t sub, const D3DVOLUME_DESC *desc, const D3DLOCKED_BOX *lb, const D3DBOX *box,
                     uint32_t flags, uint32_t out)
{
    uint32_t bw, bh, bytes;
    if (format_block(desc->Format, &bw, &bh, &bytes) != 0)
        re5_fatal("d3d9", "%s::LockBox com formato %u sem implementação", kind_name(o->kind), (unsigned)desc->Format);
    uint32_t w = box ? box->Right - box->Left : desc->Width;
    uint32_t h = box ? box->Bottom - box->Top : desc->Height;
    uint32_t d = box ? box->Back - box->Front : desc->Depth;
    uint32_t rows = (h + bh - 1) / bh, row_bytes = (w + bw - 1) / bw * bytes;
    if (lb->RowPitch <= 0 || (uint32_t)lb->RowPitch < row_bytes || !rows || !d ||
        lb->SlicePitch < lb->RowPitch * (int)(rows - 1) + (int)row_bytes)
        re5_fatal("d3d9", "%s::LockBox: pitch %d/%d do host para %u bytes x %u linhas", kind_name(o->kind),
                  lb->RowPitch, lb->SlicePitch, row_bytes, rows);
    uint32_t pitch = (uint32_t)lb->RowPitch, spitch = (uint32_t)lb->SlicePitch;
    uint64_t size = (uint64_t)spitch * (d - 1) + (uint64_t)pitch * (rows - 1) + row_bytes;
    if (size > 0x40000000u) re5_fatal("d3d9", "trava de %llu bytes", (unsigned long long)size);
    uint32_t g = k32_heap_alloc(heap(), 0, (uint32_t)size);
    if (!g) re5_fatal("d3d9", "sem memória do guest para travar %llu bytes", (unsigned long long)size);
    if (!(flags & D3DLOCK_DISCARD))
        for (uint32_t z = 0; z < d; z++)
            for (uint32_t r = 0; r < rows; r++) {
                size_t at = (size_t)z * spitch + (size_t)r * pitch;
                memcpy((uint8_t *)re5_g2h(g) + at, (const uint8_t *)lb->pBits + at, row_bytes);
            }
    d9_lock *l = lock_push(o);
    l->sub = sub;
    l->guest = g;
    l->host = lb->pBits;
    l->pitch = pitch;
    l->row_bytes = row_bytes;
    l->rows = rows;
    l->slices = d;
    l->slice_pitch = spitch;
    l->flags = flags;
    l->owned = 1;
    wr32(out, pitch);
    wr32(out + 4, spitch);
    wr32(out + 8, g);
}

static void v_LockBox(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_VOLUMETEXTURE);
    IDirect3DVolumeTexture9 *t = (IDirect3DVolumeTexture9 *)o->host;
    uint32_t level = re5_arg(c, 1), out = re5_arg(c, 2), pbox = re5_arg(c, 3), flags = re5_arg(c, 4);
    D3DBOX box;
    if (pbox) memcpy(&box, re5_g2h(pbox), sizeof box);   /* 6 UINTs nos dois lados */
    if (!out || lock_find(o, level) >= 0) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DVOLUME_DESC desc;
    D3DLOCKED_BOX lb;
    HRESULT hr = t->lpVtbl->GetLevelDesc(t, level, &desc);
    if (SUCCEEDED(hr)) hr = t->lpVtbl->LockBox(t, level, &lb, pbox ? &box : NULL, flags);
    if (SUCCEEDED(hr)) lock_box(o, level, &desc, &lb, pbox ? &box : NULL, flags, out);
    c->eax = (uint32_t)hr;
}

static void v_UnlockBox(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_VOLUMETEXTURE);
    IDirect3DVolumeTexture9 *t = (IDirect3DVolumeTexture9 *)o->host;
    uint32_t level = re5_arg(c, 1);
    int i = lock_find(o, level);
    if (i >= 0) lock_finish(o, i);
    c->eax = (uint32_t)t->lpVtbl->UnlockBox(t, level);
}

static void x_LockRect(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_CUBETEXTURE);
    IDirect3DCubeTexture9 *t = (IDirect3DCubeTexture9 *)o->host;
    uint32_t face = re5_arg(c, 1), level = re5_arg(c, 2), out = re5_arg(c, 3), flags = re5_arg(c, 5);
    uint32_t sub = level + 32u * face;
    RECT tmp;
    const RECT *rect = rect_from(re5_arg(c, 4), &tmp);
    if (!out || lock_find(o, sub) >= 0) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    HRESULT hr = t->lpVtbl->GetLevelDesc(t, level, &desc);
    if (SUCCEEDED(hr)) hr = t->lpVtbl->LockRect(t, (D3DCUBEMAP_FACES)face, level, &lr, rect, flags);
    if (SUCCEEDED(hr)) lock_pixels(o, sub, &desc, &lr, rect, flags, out);
    c->eax = (uint32_t)hr;
}

static void x_UnlockRect(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_CUBETEXTURE);
    IDirect3DCubeTexture9 *t = (IDirect3DCubeTexture9 *)o->host;
    uint32_t face = re5_arg(c, 1), level = re5_arg(c, 2);
    int i = lock_find(o, level + 32u * face);
    if (i >= 0) lock_finish(o, i);
    c->eax = (uint32_t)t->lpVtbl->UnlockRect(t, (D3DCUBEMAP_FACES)face, level);
}

static void s_LockRect(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_SURFACE);
    IDirect3DSurface9 *s = (IDirect3DSurface9 *)o->host;
    uint32_t out = re5_arg(c, 1), flags = re5_arg(c, 3);
    RECT tmp;
    const RECT *rect = rect_from(re5_arg(c, 2), &tmp);
    if (!out || lock_find(o, 0) >= 0) {
        c->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    HRESULT hr = s->lpVtbl->GetDesc(s, &desc);
    if (SUCCEEDED(hr)) hr = s->lpVtbl->LockRect(s, &lr, rect, flags);
    if (SUCCEEDED(hr)) lock_pixels(o, 0, &desc, &lr, rect, flags, out);
    c->eax = (uint32_t)hr;
}

static void s_UnlockRect(CPU *c)
{
    d9_obj *o = self_obj(c, D9K_SURFACE);
    IDirect3DSurface9 *s = (IDirect3DSurface9 *)o->host;
    int i = lock_find(o, 0);
    if (i >= 0) lock_finish(o, i);
    c->eax = (uint32_t)s->lpVtbl->UnlockRect(s);
}

/* ---- vtables ---- */

static void unknown_method(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("d3d9", "%s sem implementação (this=0x%08X, retorno 0x%08X)", t ? t->name : "?", re5_arg(c, 0),
              rd32(c->esp));
}

typedef struct {
    d9_kind     kind;
    const char *method;
    re5_host_fn fn;
} d9_manual;

static const d9_manual k_manual[] = {
    {D9K_D3D9, "CreateDevice", d_CreateDevice},
    {D9K_DEVICE, "GetCreationParameters", d_GetCreationParameters},
    {D9K_DEVICE, "Reset", d_Reset},
    {D9K_DEVICE, "Present", d_Present},
    {D9K_SURFACE, "LockRect", s_LockRect},
    {D9K_SURFACE, "UnlockRect", s_UnlockRect},
    {D9K_TEXTURE, "LockRect", t_LockRect},
    {D9K_TEXTURE, "UnlockRect", t_UnlockRect},
    {D9K_CUBETEXTURE, "LockRect", x_LockRect},
    {D9K_CUBETEXTURE, "UnlockRect", x_UnlockRect},
    {D9K_VOLUMETEXTURE, "LockBox", v_LockBox},
    {D9K_VOLUMETEXTURE, "UnlockBox", v_UnlockBox},
    {D9K_VERTEXBUFFER, "Lock", b_Lock},
    {D9K_VERTEXBUFFER, "Unlock", b_Unlock},
    {D9K_INDEXBUFFER, "Lock", b_Lock},
    {D9K_INDEXBUFFER, "Unlock", b_Unlock},
    {D9K_SURFACE, "SetPrivateData", r_SetPrivateData},
    {D9K_VOLUME, "SetPrivateData", r_SetPrivateData},
    {D9K_TEXTURE, "SetPrivateData", r_SetPrivateData},
    {D9K_CUBETEXTURE, "SetPrivateData", r_SetPrivateData},
    {D9K_VOLUMETEXTURE, "SetPrivateData", r_SetPrivateData},
    {D9K_VERTEXBUFFER, "SetPrivateData", r_SetPrivateData},
    {D9K_INDEXBUFFER, "SetPrivateData", r_SetPrivateData},
};

static uint32_t vtable_of(int kind)
{
    if (g_vtables[kind]) return g_vtables[kind];
    const d9_iface *it = NULL;
    for (uint32_t i = 0; i < D9_IFACE_COUNT; i++)
        if ((int)k_d9_ifaces[i].kind == kind) it = &k_d9_ifaces[i];
    if (!it || !it->methods) re5_fatal("d3d9", "tipo %d sem vtable", kind);
    uint32_t vt = k32_heap_alloc(heap(), 0, 4u * it->count);
    if (!vt) re5_fatal("d3d9", "sem memória do guest para a vtable de %s", it->iface);
    for (uint32_t s = 0; s < it->count; s++) {
        const d9_method *m = &it->methods[s];
        re5_host_fn fn = s == 0 ? i_QueryInterface : s == 1 ? i_AddRef : s == 2 ? i_Release : m->fn;
        const char *short_name = strstr(m->name, "::") + 2;
        for (size_t k = 0; k < sizeof k_manual / sizeof k_manual[0]; k++)
            if ((int)k_manual[k].kind == kind && !strcmp(k_manual[k].method, short_name)) fn = k_manual[k].fn;
        wr32(vt + 4u * s, re5_thunk_new(m->name, fn ? fn : unknown_method, m->arg_bytes, NULL));
    }
    /* Um handler escrito à mão com nome que não existe na interface é erro de digitação. */
    for (size_t k = 0; k < sizeof k_manual / sizeof k_manual[0]; k++) {
        if ((int)k_manual[k].kind != kind) continue;
        int found = 0;
        for (uint32_t s = 0; s < it->count && !found; s++)
            found = !strcmp(strstr(it->methods[s].name, "::") + 2, k_manual[k].method);
        if (!found) re5_fatal("d3d9", "handler para %s::%s, que não existe", it->iface, k_manual[k].method);
    }
    RE5_DEBUG("d3d9", "vtable de %s em 0x%08X (%u métodos)", it->iface, vt, it->count);
    return g_vtables[kind] = vt;
}

/* ---- exports do d3d9.dll ---- */

/* "Uma ferramenta de captura (PIX) está ligada?" - não há nenhuma. */
static void d_D3DPERF_GetStatus(CPU *c) { c->eax = 0; }

static const re5_import_def k_defs[] = {
    {"D3DPERF_GetStatus", d_D3DPERF_GetStatus, 0, 0},
    {"Direct3DCreate9", d_Direct3DCreate9, 4, 0},
    {0},
};

const re5_import_module re5_mod_d3d9 = {"d3d9.dll", k_defs};
