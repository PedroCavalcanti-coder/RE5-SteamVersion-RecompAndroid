/*
 * Steamworks offline: funções planas (cdecl), métodos de interface por vtable
 * (thiscall, this em ecx), CSteamID devolvido por ponteiro oculto, callbacks
 * entregues em SteamAPI_RunCallbacks pelo slot que o MSVC usa para Run(void*),
 * estatísticas e a "nuvem" do ISteamRemoteStorage em disco.
 */
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "check.h"
#include "re5/runtime.h"

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

static uint32_t gstr(const char *s)
{
    uint32_t va = gbuf((uint32_t)strlen(s) + 1);
    memcpy(re5_g2h(va), s, strlen(s) + 1);
    return va;
}

static uint32_t flat(const char *name, int n, const uint32_t *args)
{
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, re5_import_bind("steam_api.dll", name, 0), n, args);
    CHECK_EQ_U32(T->cpu.esp, esp0);   /* re5_guest_call restaura; cdecl não mexe */
    return r;
}

/* Método virtual: this em ecx, a vtable do objeto, o slot. */
static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *args)
{
    T->cpu.ecx = obj;
    return re5_guest_call(T, rd32(rd32(obj) + 4 * slot), n, args);
}

static uint32_t g_run_calls, g_run_param;
static void cb_run(CPU *c)
{
    g_run_calls++;
    g_run_param = re5_arg(c, 0);
}
static void cb_run_result(CPU *c) { (void)c; CHECK(0 && "Run(void*, bool, SteamAPICall_t) não devia ser chamado"); }
static void cb_size(CPU *c) { c->eax = 24; }

int main(int argc, char **argv)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");

    const uint32_t appid = 21690;
    CHECK_EQ_U32(flat("SteamAPI_RestartAppIfNecessary", 1, &appid), 0);
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);

    uint32_t user = flat("SteamUser", 0, NULL);
    CHECK(user != 0);
    CHECK_EQ_U32(flat("SteamUser", 0, NULL), user);   /* sempre o mesmo objeto */
    CHECK_EQ_U32(vcall(user, 1, 0, NULL) & 0xFF, 1);  /* BLoggedOn */
    uint32_t sid = gbuf(8);
    CHECK_EQ_U32(vcall(user, 2, 1, &sid), sid);       /* GetSteamID: ponteiro oculto */
    CHECK_EQ_U32(rd32(sid + 4), 0x01100001u);

    uint32_t utils = flat("SteamUtils", 0, NULL);
    CHECK_EQ_U32(vcall(utils, 9, 0, NULL), 21690);     /* GetAppID */

    /* CCallback<UserStatsReceived_t>: vtable [0] Run(p, bool, call), [1] Run(p), [2] tamanho. */
    uint32_t vt = gbuf(12), cb = gbuf(16);
    wr32(vt, re5_thunk_new("teste!RunResult", cb_run_result, 16, NULL));
    wr32(vt + 4, re5_thunk_new("teste!Run", cb_run, 4, NULL));
    wr32(vt + 8, re5_thunk_new("teste!Size", cb_size, 0, NULL));
    wr32(cb, vt);
    const uint32_t reg[2] = {cb, 1101};
    flat("SteamAPI_RegisterCallback", 2, reg);
    CHECK_EQ_U32(rd8(cb + 4) & 1, 1);
    CHECK_EQ_U32(rd32(cb + 8), 1101);

    uint32_t stats = flat("SteamUserStats", 0, NULL);
    CHECK_EQ_U32(vcall(stats, 0, 0, NULL) & 0xFF, 1);   /* RequestCurrentStats */
    CHECK_EQ_U32(g_run_calls, 0);                        /* só no RunCallbacks */
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_run_calls, 1);
    CHECK_EQ_U32(rd32(g_run_param), 21690);              /* m_nGameID */
    CHECK_EQ_U32(rd32(g_run_param + 8), 1);              /* k_EResultOK */
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_run_calls, 1);

    uint32_t name = gstr("KILLS"), out = gbuf(4);
    const uint32_t set[2] = {name, 42}, get[2] = {name, out};
    CHECK_EQ_U32(vcall(stats, 3, 2, set) & 0xFF, 1);
    CHECK_EQ_U32(vcall(stats, 2, 2, get) & 0xFF, 1);
    CHECK_EQ_U32(rd32(out), 42);

    flat("SteamAPI_UnregisterCallback", 1, &cb);
    CHECK_EQ_U32(rd8(cb + 4) & 1, 0);
    vcall(stats, 0, 0, NULL);
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_run_calls, 1);

    if (argc > 1) {   /* nuvem: pasta nova que faz de raiz do VFS */
#if defined(_WIN32)
        _mkdir(argv[1]);
#else
        mkdir(argv[1], 0755);
#endif
        CHECK(re5_vfs_init(argv[1], NULL) == 0);
        uint32_t remote = flat("SteamRemoteStorage", 0, NULL);
        uint32_t file = gstr("save0.bin"), data = gstr("abcdef"), back = gbuf(16);
        const uint32_t w[3] = {file, data, 6}, r[3] = {file, back, 16};
        CHECK_EQ_U32(vcall(remote, 10, 1, &file) & 0xFF, 0);   /* FileExists */
        CHECK_EQ_U32(vcall(remote, 0, 3, w) & 0xFF, 1);        /* FileWrite */
        CHECK_EQ_U32(vcall(remote, 10, 1, &file) & 0xFF, 1);
        CHECK_EQ_U32(vcall(remote, 12, 1, &file), 6);          /* GetFileSize */
        CHECK_EQ_U32(vcall(remote, 1, 3, r), 6);               /* FileRead */
        CHECK(memcmp(re5_g2h(back), "abcdef", 6) == 0);
        CHECK_EQ_U32(vcall(remote, 3, 1, &file) & 0xFF, 1);    /* FileDelete */
        CHECK_EQ_U32(vcall(remote, 10, 1, &file) & 0xFF, 0);
    }
    CHECK_DONE();
}
