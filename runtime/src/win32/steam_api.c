/*
 * steam_api.c - Steamworks offline (Fase 3, etapa 3.7).
 *
 * O re5dx9.exe foi ligado ao steam_api.dll do SDK 1.30 (julho de 2014; versões
 * de interface lidas do próprio DLL despejado no OEP): SteamUser017,
 * SteamFriends014, SteamUtils007, STEAMAPPS_INTERFACE_VERSION006,
 * STEAMUSERSTATS_INTERFACE_VERSION011, STEAMREMOTESTORAGE_INTERFACE_VERSION012,
 * SteamMatchMaking009, SteamNetworking005, SteamClient015.
 *
 * Modelo:
 *   - funções planas (SteamAPI_*, SteamUser()...): cdecl, arg_bytes 0;
 *   - cada interface é um objeto na memória do guest: [vtable] -> 80 thunks
 *     thiscall (this em ecx, quem chama empurra os argumentos, o método limpa).
 *     Índice sem implementação aborta dizendo "ISteamX::#N" (D8);
 *   - o MSVC põe métodos virtuais SOBRECARREGADOS em ordem inversa de
 *     declaração. Onde isso pesa (GetStat/SetStat int32/float) as duas formas
 *     movem 4 bytes crus, então os dois índices usam o mesmo handler;
 *   - CSteamID devolvido por valor usa ponteiro de retorno oculto (a classe tem
 *     construtores): primeiro argumento na pilha, eax = esse ponteiro;
 *   - callbacks: SteamAPI_RegisterCallback marca o CCallbackBase do jogo e o
 *     guarda; o que o "Steam" responde (estatísticas recebidas/gravadas...) entra
 *     numa fila entregue em SteamAPI_RunCallbacks, chamando o Run do objeto.
 *
 * O usuário é um jogador fixo e logado ("Player"), como os emuladores offline
 * fazem: o jogo segue o caminho normal sem rede. Arquivos da nuvem
 * (ISteamRemoteStorage) ficam na pasta do usuário do runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "re5/runtime.h"

#define APP_ID       21690u
#define STEAM_ID_LO  0x00000001u   /* conta 1 */
#define STEAM_ID_HI  0x01100001u   /* universo público, individual, instância 1 */
#define PERSONA_NAME "Player"
#define VT_SIZE      80u

/* ---- memória do guest para objetos, strings e dados de callback ---- */

static uint32_t g_arena, g_arena_used;

static uint32_t galloc(uint32_t n)
{
    if (!g_arena) {
        g_arena = re5_mem_alloc(0x40000u, RE5_PAGE_READWRITE, "steam_api");
        if (!g_arena) re5_fatal("steam_api", "sem memória do guest");
    }
    n = (n + 15u) & ~15u;
    if (g_arena_used + n > 0x40000u) re5_fatal("steam_api", "arena de 256 KiB esgotada");
    uint32_t va = g_arena + g_arena_used;
    g_arena_used += n;
    return va;
}

static uint32_t gstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, va = galloc(n);
    memcpy(re5_g2h(va), s, n);
    return va;
}

/* ---- interfaces ---- */

typedef struct {
    int         index;
    const char *name;
    re5_host_fn fn;
    uint32_t    arg_bytes;   /* bytes de pilha (sem o this, que vai em ecx) */
} method;

typedef struct {
    const char   *iface;
    const method *methods;
    uint32_t      obj;
} interface;

static void unknown_method(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("steam_api", "%s sem implementação (this=0x%08X, retorno 0x%08X)", t ? t->name : "?", c->ecx,
              rd32(c->esp));
}

static uint32_t make_object(interface *it)
{
    if (it->obj) return it->obj;
    uint32_t vt = galloc(4 * VT_SIZE);
    for (uint32_t i = 0; i < VT_SIZE; i++) {
        const method *m = NULL;
        for (const method *p = it->methods; p && p->name; p++)
            if ((uint32_t)p->index == i) m = p;
        char *name = malloc(96);
        if (m) snprintf(name, 96, "%s::%s", it->iface, m->name);
        else snprintf(name, 96, "%s::#%u", it->iface, i);
        wr32(vt + 4 * i, re5_thunk_new(name, m ? m->fn : unknown_method, m ? m->arg_bytes : 0, NULL));
    }
    it->obj = galloc(16);
    wr32(it->obj, vt);
    return it->obj;
}

static void ret_bool(CPU *c, int v) { c->eax = v ? 1u : 0u; }

static void ret_u64(CPU *c, uint64_t v)
{
    c->eax = (uint32_t)v;
    c->edx = (uint32_t)(v >> 32);
}

/* CSteamID por valor: ponteiro oculto no primeiro argumento. */
static void ret_steamid(CPU *c, uint32_t lo, uint32_t hi)
{
    uint32_t p = re5_arg(c, 0);
    wr32(p, lo);
    wr32(p + 4, hi);
    c->eax = p;
}

/* ---- callbacks e chamadas assíncronas ---- */

#define K_FLAG_REGISTERED 0x01u
#define CB_FLAGS  4u
#define CB_ID     8u

#define MAX_CB 128
static uint32_t g_callbacks[MAX_CB];
static uint32_t g_ncallbacks;

typedef struct { uint32_t obj; uint64_t call; } call_result;
static call_result g_results[MAX_CB];
static uint32_t g_nresults;

typedef struct { int id; uint32_t data, size; uint64_t call; } pending;
#define MAX_PENDING 64
static pending g_pending[MAX_PENDING];
static uint32_t g_npending;

/* Enfileira um resultado do "Steam". call != 0: resposta de uma SteamAPICall_t. */
static void post_callback(int id, const void *data, uint32_t size, uint64_t call)
{
    if (g_npending == MAX_PENDING) re5_fatal("steam_api", "fila de callbacks cheia (o jogo não chama RunCallbacks)");
    uint32_t va = galloc(size);
    memcpy(re5_g2h(va), data, size);
    g_pending[g_npending++] = (pending){id, va, size, call};
}

/* thiscall do guest: this em ecx, argumentos na pilha. */
static void call_method(uint32_t obj, uint32_t slot, int argc, const uint32_t *argv)
{
    re5_thread *t = re5_thread_current();
    uint32_t saved_ecx = t->cpu.ecx, saved_eax = t->cpu.eax, saved_edx = t->cpu.edx;
    t->cpu.ecx = obj;
    re5_guest_call(t, rd32(rd32(obj) + 4 * slot), argc, argv);
    t->cpu.ecx = saved_ecx;
    t->cpu.eax = saved_eax;
    t->cpu.edx = saved_edx;
}

/* Vtable do CCallbackBase compilada pelo MSVC: as duas sobrecargas de Run em
 * ordem inversa - [0] Run(void*, bool, SteamAPICall_t), [1] Run(void*),
 * [2] GetCallbackSizeBytes. */
#define CB_RUN_RESULT 0u
#define CB_RUN        1u

static void s_RunCallbacks(CPU *c)
{
    (void)c;
    uint32_t n = g_npending;
    pending batch[MAX_PENDING];
    memcpy(batch, g_pending, n * sizeof batch[0]);
    g_npending = 0;
    for (uint32_t i = 0; i < n; i++) {
        pending *p = &batch[i];
        if (p->call) {
            for (uint32_t k = 0; k < g_nresults; k++) {
                if (g_results[k].call != p->call) continue;
                uint32_t obj = g_results[k].obj;
                g_results[k] = g_results[--g_nresults];
                wr8(obj + CB_FLAGS, rd8(obj + CB_FLAGS) & (uint8_t)~K_FLAG_REGISTERED);
                const uint32_t a[4] = {p->data, 0 /* bIOFailure */, (uint32_t)p->call, (uint32_t)(p->call >> 32)};
                call_method(obj, CB_RUN_RESULT, 4, a);
                break;
            }
        }
        for (uint32_t k = 0; k < g_ncallbacks; k++) {
            uint32_t obj = g_callbacks[k];
            if ((int32_t)rd32(obj + CB_ID) == p->id) call_method(obj, CB_RUN, 1, &p->data);
        }
    }
}

static void s_RegisterCallback(CPU *c)
{
    uint32_t obj = re5_arg(c, 0);
    int id = (int)re5_arg(c, 1);
    if (g_ncallbacks == MAX_CB) re5_fatal("steam_api", "mais de %d callbacks registrados", MAX_CB);
    wr8(obj + CB_FLAGS, rd8(obj + CB_FLAGS) | K_FLAG_REGISTERED);
    wr32(obj + CB_ID, (uint32_t)id);
    g_callbacks[g_ncallbacks++] = obj;
    RE5_DEBUG("steam_api", "callback %d registrado em 0x%08X", id, obj);
}

static void s_UnregisterCallback(CPU *c)
{
    uint32_t obj = re5_arg(c, 0);
    for (uint32_t k = 0; k < g_ncallbacks; k++) {
        if (g_callbacks[k] == obj) {
            g_callbacks[k] = g_callbacks[--g_ncallbacks];
            break;
        }
    }
    wr8(obj + CB_FLAGS, rd8(obj + CB_FLAGS) & (uint8_t)~K_FLAG_REGISTERED);
}

static void s_RegisterCallResult(CPU *c)
{
    uint32_t obj = re5_arg(c, 0);
    uint64_t call = (uint64_t)re5_arg(c, 2) << 32 | re5_arg(c, 1);
    if (g_nresults == MAX_CB) re5_fatal("steam_api", "mais de %d call results pendentes", MAX_CB);
    wr8(obj + CB_FLAGS, rd8(obj + CB_FLAGS) | K_FLAG_REGISTERED);
    g_results[g_nresults++] = (call_result){obj, call};
}

static void s_UnregisterCallResult(CPU *c)
{
    uint32_t obj = re5_arg(c, 0);
    uint64_t call = (uint64_t)re5_arg(c, 2) << 32 | re5_arg(c, 1);
    for (uint32_t k = 0; k < g_nresults; k++) {
        if (g_results[k].obj == obj && g_results[k].call == call) {
            g_results[k] = g_results[--g_nresults];
            break;
        }
    }
    wr8(obj + CB_FLAGS, rd8(obj + CB_FLAGS) & (uint8_t)~K_FLAG_REGISTERED);
}

/* ---- ciclo de vida ---- */

static void s_RestartAppIfNecessary(CPU *c) { c->eax = 0; }   /* já estamos "dentro da Steam" */
static void s_IsSteamRunning(CPU *c) { c->eax = 1; }

static void s_Init(CPU *c)
{
    RE5_INFO("steam_api", "SteamAPI_Init: Steam offline (app %u, usuário '%s')", APP_ID, PERSONA_NAME);
    c->eax = 1;
}

static void s_Shutdown(CPU *c) { (void)c; }

/* ---- ISteamUser017 ---- */

static void u_GetHSteamUser(CPU *c) { c->eax = 1; }
static void u_BLoggedOn(CPU *c) { ret_bool(c, 1); }
static void u_GetSteamID(CPU *c) { ret_steamid(c, STEAM_ID_LO, STEAM_ID_HI); }
static void u_GetPlayerSteamLevel(CPU *c) { c->eax = 0; }

static void u_GetUserDataFolder(CPU *c)
{
    uint32_t buf = re5_arg(c, 0), n = re5_arg(c, 1);
    char path[128];
    snprintf(path, sizeof path, "C:\\Program Files (x86)\\Steam\\userdata\\%u\\%u\\local", STEAM_ID_LO, APP_ID);
    ret_bool(c, 0);
    if (strlen(path) + 1 > n) return;
    memcpy(re5_g2h(buf), path, strlen(path) + 1);
    ret_bool(c, 1);
}

static const method k_user[] = {
    {0, "GetHSteamUser", u_GetHSteamUser, 0},
    {1, "BLoggedOn", u_BLoggedOn, 0},
    {2, "GetSteamID", u_GetSteamID, 4},
    {6, "GetUserDataFolder", u_GetUserDataFolder, 8},
    {23, "GetPlayerSteamLevel", u_GetPlayerSteamLevel, 0},
    {0, NULL, NULL, 0},
};

/* ---- ISteamUtils007 ---- */

static uint32_t g_start_time;

static void t_SecondsSinceActive(CPU *c) { c->eax = (uint32_t)time(NULL) - g_start_time; }
static void t_GetConnectedUniverse(CPU *c) { c->eax = 1; }   /* k_EUniversePublic */
static void t_GetServerRealTime(CPU *c) { c->eax = (uint32_t)time(NULL); }

static void t_GetIPCountry(CPU *c)
{
    static uint32_t s;
    if (!s) s = gstr("US");
    c->eax = s;
}

static void t_GetCurrentBatteryPower(CPU *c) { c->eax = 255; }   /* ligado na tomada */
static void t_GetAppID(CPU *c) { c->eax = APP_ID; }
static void t_Nothing(CPU *c) { (void)c; }
static void t_False(CPU *c) { ret_bool(c, 0); }
static void t_Zero(CPU *c) { c->eax = 0; }

/* Toda SteamAPICall_t que o runtime cria já nasce concluída. */
static void t_IsAPICallCompleted(CPU *c)
{
    uint32_t failed = re5_arg(c, 2);
    if (failed) wr8(failed, 0);
    ret_bool(c, 1);
}

static void t_GetAPICallFailureReason(CPU *c) { c->eax = 0xFFFFFFFFu; }   /* k_ESteamAPICallFailureNone */

static void t_GetAPICallResult(CPU *c)
{
    uint64_t call = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    uint32_t out = re5_arg(c, 2), cub = re5_arg(c, 3), failed = re5_arg(c, 5);
    int expected = (int)re5_arg(c, 4);
    for (uint32_t i = 0; i < g_npending; i++) {
        pending *p = &g_pending[i];
        if (p->call != call || p->id != expected) continue;
        memcpy(re5_g2h(out), re5_g2h(p->data), p->size < cub ? p->size : cub);
        if (failed) wr8(failed, 0);
        g_pending[i] = g_pending[--g_npending];
        ret_bool(c, 1);
        return;
    }
    if (failed) wr8(failed, 1);
    ret_bool(c, 0);
}

static void t_GetSteamUILanguage(CPU *c)
{
    static uint32_t s;
    if (!s) s = gstr("english");
    c->eax = s;
}

static const method k_utils[] = {
    {0, "GetSecondsSinceAppActive", t_SecondsSinceActive, 0},
    {1, "GetSecondsSinceComputerActive", t_SecondsSinceActive, 0},
    {2, "GetConnectedUniverse", t_GetConnectedUniverse, 0},
    {3, "GetServerRealTime", t_GetServerRealTime, 0},
    {4, "GetIPCountry", t_GetIPCountry, 0},
    {8, "GetCurrentBatteryPower", t_GetCurrentBatteryPower, 0},
    {9, "GetAppID", t_GetAppID, 0},
    {10, "SetOverlayNotificationPosition", t_Nothing, 4},
    {11, "IsAPICallCompleted", t_IsAPICallCompleted, 12},
    {12, "GetAPICallFailureReason", t_GetAPICallFailureReason, 8},
    {13, "GetAPICallResult", t_GetAPICallResult, 24},
    {15, "GetIPCCallCount", t_Zero, 0},
    {16, "SetWarningMessageHook", t_Nothing, 4},
    {17, "IsOverlayEnabled", t_False, 0},     /* sem overlay da Steam no runtime */
    {18, "BOverlayNeedsPresent", t_False, 0},
    {23, "GetSteamUILanguage", t_GetSteamUILanguage, 0},
    {24, "IsSteamRunningInVR", t_False, 0},
    {0, NULL, NULL, 0},
};

/* ---- ISteamApps006 ---- */

static void a_True(CPU *c) { ret_bool(c, 1); }

static void a_GetCurrentGameLanguage(CPU *c)
{
    static uint32_t s;
    if (!s) s = gstr("english");
    c->eax = s;
}

/* A Gold Edition inclui os dois episódios extras; o jogo confere a posse pelo id. */
static void a_BIsDlcInstalled(CPU *c)
{
    RE5_INFO("steam_api", "ISteamApps::BIsDlcInstalled(%u) -> true", re5_arg(c, 0));
    ret_bool(c, 1);
}

static void a_GetAppInstallDir(CPU *c)
{
    uint32_t buf = re5_arg(c, 1), n = re5_arg(c, 2);
    const char *dir = RE5_VFS_INSTALL_DIR;
    uint32_t len = (uint32_t)strlen(dir);
    if (len + 1 > n) { c->eax = 0; return; }
    memcpy(re5_g2h(buf), dir, len + 1);
    c->eax = len;
}

static const method k_apps[] = {
    {0, "BIsSubscribed", a_True, 0},
    {1, "BIsLowViolence", t_False, 0},
    {2, "BIsCybercafe", t_False, 0},
    {3, "BIsVACBanned", t_False, 0},
    {4, "GetCurrentGameLanguage", a_GetCurrentGameLanguage, 0},
    {5, "GetAvailableGameLanguages", a_GetCurrentGameLanguage, 0},
    {6, "BIsSubscribedApp", a_True, 4},
    {7, "BIsDlcInstalled", a_BIsDlcInstalled, 4},
    {8, "GetEarliestPurchaseUnixTime", t_Zero, 4},
    {9, "BIsSubscribedFromFreeWeekend", t_False, 0},
    {15, "GetCurrentBetaName", t_False, 8},
    {18, "GetAppInstallDir", a_GetAppInstallDir, 12},
    {19, "BIsAppInstalled", a_True, 4},
    {0, NULL, NULL, 0},
};

/* ---- ISteamUserStats011: estatísticas e conquistas guardadas no runtime ---- */

#define MAX_STATS 256
typedef struct { char name[64]; uint32_t value; int achievement; uint32_t unlock_time; } stat_entry;
static stat_entry g_stats[MAX_STATS];
static uint32_t g_nstats;

static stat_entry *stat_find(const char *name, int create)
{
    for (uint32_t i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return &g_stats[i];
    if (!create) return NULL;
    if (g_nstats == MAX_STATS) re5_fatal("steam_api", "mais de %d estatísticas", MAX_STATS);
    stat_entry *s = &g_stats[g_nstats++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

static uint64_t game_id(void) { return APP_ID; }

/* UserStatsReceived_t (1101): uint64 m_nGameID, EResult m_eResult, CSteamID m_steamIDUser. */
static void s_RequestCurrentStats(CPU *c)
{
    uint32_t d[6] = {(uint32_t)game_id(), 0, 1 /* k_EResultOK */, 0, STEAM_ID_LO, STEAM_ID_HI};
    post_callback(1101, d, sizeof d, 0);
    ret_bool(c, 1);
}

static void s_GetStat(CPU *c)
{
    stat_entry *s = stat_find(re5_gstr(re5_arg(c, 0)), 0);
    wr32(re5_arg(c, 1), s ? s->value : 0);
    ret_bool(c, 1);
}

static void s_SetStat(CPU *c)
{
    stat_find(re5_gstr(re5_arg(c, 0)), 1)->value = re5_arg(c, 1);
    ret_bool(c, 1);
}

static void s_GetAchievement(CPU *c)
{
    stat_entry *s = stat_find(re5_gstr(re5_arg(c, 0)), 0);
    wr8(re5_arg(c, 1), s && s->achievement ? 1 : 0);
    ret_bool(c, 1);
}

static void s_SetAchievement(CPU *c)
{
    stat_entry *s = stat_find(re5_gstr(re5_arg(c, 0)), 1);
    if (!s->achievement) RE5_INFO("steam_api", "conquista desbloqueada: %s", s->name);
    s->achievement = 1;
    s->unlock_time = (uint32_t)time(NULL);
    ret_bool(c, 1);
}

static void s_ClearAchievement(CPU *c)
{
    stat_entry *s = stat_find(re5_gstr(re5_arg(c, 0)), 1);
    s->achievement = 0;
    s->unlock_time = 0;
    ret_bool(c, 1);
}

static void s_GetAchievementAndUnlockTime(CPU *c)
{
    stat_entry *s = stat_find(re5_gstr(re5_arg(c, 0)), 0);
    wr8(re5_arg(c, 1), s && s->achievement ? 1 : 0);
    wr32(re5_arg(c, 2), s ? s->unlock_time : 0);
    ret_bool(c, 1);
}

/* UserStatsStored_t (1102): uint64 m_nGameID, EResult m_eResult. */
static void s_StoreStats(CPU *c)
{
    uint32_t d[4] = {(uint32_t)game_id(), 0, 1, 0};
    post_callback(1102, d, sizeof d, 0);
    ret_bool(c, 1);
}

static const method k_stats[] = {
    {0, "RequestCurrentStats", s_RequestCurrentStats, 0},
    {1, "GetStat", s_GetStat, 8},   /* float* e int32* (ordem do MSVC): 4 bytes crus */
    {2, "GetStat", s_GetStat, 8},
    {3, "SetStat", s_SetStat, 8},
    {4, "SetStat", s_SetStat, 8},
    {6, "GetAchievement", s_GetAchievement, 8},
    {7, "SetAchievement", s_SetAchievement, 4},
    {8, "ClearAchievement", s_ClearAchievement, 4},
    {9, "GetAchievementAndUnlockTime", s_GetAchievementAndUnlockTime, 12},
    {10, "StoreStats", s_StoreStats, 0},
    {13, "IndicateAchievementProgress", a_True, 12},
    {0, NULL, NULL, 0},
};

/* ---- ISteamFriends014 ---- */

static void f_GetPersonaName(CPU *c)
{
    static uint32_t s;
    if (!s) s = gstr(PERSONA_NAME);
    c->eax = s;
}

static void f_GetPersonaState(CPU *c) { c->eax = 1; }   /* k_EPersonaStateOnline */

static const method k_friends[] = {
    {0, "GetPersonaName", f_GetPersonaName, 0},
    {2, "GetPersonaState", f_GetPersonaState, 0},
    {3, "GetFriendCount", t_Zero, 4},
    {22, "ActivateGameOverlay", t_Nothing, 4},
    {37, "SetRichPresence", a_True, 8},
    {38, "ClearRichPresence", t_Nothing, 0},
    {0, NULL, NULL, 0},
};

/* ---- ISteamRemoteStorage012: "nuvem" numa pasta do usuário do runtime ---- */

#define REMOTE_DIR "C:\\Users\\Player\\SteamCloud\\21690"

static int remote_path(uint32_t name_va, char *out, size_t cap)
{
    const char *name = re5_gstr(name_va);
    if (!name || !*name || strstr(name, "..")) return -1;
    char guest[512];
    snprintf(guest, sizeof guest, "%s\\%s", REMOTE_DIR, name);
    return re5_vfs_resolve(guest, out, cap);
}

static void mkdir_p(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/' && *s != '\\') continue;
        char k = *s;
        *s = 0;
#if defined(RE5_HOST_WINDOWS)
        if (s[-1] != ':') _mkdir(p);
#else
        mkdir(p, 0755);
#endif
        *s = k;
    }
}

static void r_FileWrite(CPU *c)
{
    char path[1024];
    uint32_t data = re5_arg(c, 1), n = re5_arg(c, 2);
    ret_bool(c, 0);
    if (remote_path(re5_arg(c, 0), path, sizeof path) != 0) return;
    mkdir_p(path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int ok = fwrite(re5_g2h(data), 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    ret_bool(c, ok);
}

static void r_FileRead(CPU *c)
{
    char path[1024];
    uint32_t data = re5_arg(c, 1), n = re5_arg(c, 2);
    c->eax = 0;
    if (remote_path(re5_arg(c, 0), path, sizeof path) != 0) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    c->eax = (uint32_t)fread(re5_g2h(data), 1, n, f);
    fclose(f);
}

static int remote_stat(uint32_t name_va, struct stat *st)
{
    char path[1024];
    return remote_path(name_va, path, sizeof path) == 0 && stat(path, st) == 0 && !S_ISDIR(st->st_mode);
}

static void r_FileExists(CPU *c)
{
    struct stat st;
    ret_bool(c, remote_stat(re5_arg(c, 0), &st));
}

static void r_GetFileSize(CPU *c)
{
    struct stat st;
    c->eax = remote_stat(re5_arg(c, 0), &st) ? (uint32_t)st.st_size : 0;
}

static void r_GetFileTimestamp(CPU *c)
{
    struct stat st;
    ret_u64(c, remote_stat(re5_arg(c, 0), &st) ? (uint64_t)st.st_mtime : 0);
}

static void r_FileDelete(CPU *c)
{
    char path[1024];
    ret_bool(c, remote_path(re5_arg(c, 0), path, sizeof path) == 0 && remove(path) == 0);
}

static void r_GetQuota(CPU *c)
{
    wr32(re5_arg(c, 0), 100u << 20);
    wr32(re5_arg(c, 1), 100u << 20);
    ret_bool(c, 1);
}

static const method k_remote[] = {
    {0, "FileWrite", r_FileWrite, 12},
    {1, "FileRead", r_FileRead, 12},
    {2, "FileForget", a_True, 4},
    {3, "FileDelete", r_FileDelete, 4},
    {10, "FileExists", r_FileExists, 4},
    {11, "FilePersisted", r_FileExists, 4},
    {12, "GetFileSize", r_GetFileSize, 4},
    {13, "GetFileTimestamp", r_GetFileTimestamp, 4},
    {17, "GetQuota", r_GetQuota, 8},
    {18, "IsCloudEnabledForAccount", a_True, 0},
    {19, "IsCloudEnabledForApp", a_True, 0},
    {20, "SetCloudEnabledForApp", t_Nothing, 4},
    {0, NULL, NULL, 0},
};

/* ---- interfaces sem nada implementado ainda (co-op online) ---- */

static const method k_none[] = {{0, NULL, NULL, 0}};

/* ISteamClient015: o jogo só instala o gancho de avisos (como o exemplo do SDK);
 * a Steam offline não tem avisos para mandar. */
static const method k_client[] = {
    {20, "GetIPCCallCount", t_Zero, 0},
    {21, "SetWarningMessageHook", t_Nothing, 4},
    {0, NULL, NULL, 0},
};

static interface g_user = {"ISteamUser017", k_user, 0};
static interface g_utils = {"ISteamUtils007", k_utils, 0};
static interface g_apps = {"ISteamApps006", k_apps, 0};
static interface g_stats_if = {"ISteamUserStats011", k_stats, 0};
static interface g_friends = {"ISteamFriends014", k_friends, 0};
static interface g_remote = {"ISteamRemoteStorage012", k_remote, 0};
static interface g_matchmaking = {"ISteamMatchmaking009", k_none, 0};
static interface g_networking = {"ISteamNetworking005", k_none, 0};
static interface g_client = {"ISteamClient015", k_client, 0};

static void s_SteamUser(CPU *c) { c->eax = make_object(&g_user); }
static void s_SteamUtils(CPU *c)
{
    if (!g_start_time) g_start_time = (uint32_t)time(NULL);
    c->eax = make_object(&g_utils);
}
static void s_SteamApps(CPU *c) { c->eax = make_object(&g_apps); }
static void s_SteamUserStats(CPU *c) { c->eax = make_object(&g_stats_if); }
static void s_SteamFriends(CPU *c) { c->eax = make_object(&g_friends); }
static void s_SteamRemoteStorage(CPU *c) { c->eax = make_object(&g_remote); }
static void s_SteamMatchmaking(CPU *c) { c->eax = make_object(&g_matchmaking); }
static void s_SteamNetworking(CPU *c) { c->eax = make_object(&g_networking); }
static void s_SteamClient(CPU *c) { c->eax = make_object(&g_client); }

/* Funções planas: cdecl (S_CALLTYPE), quem chama limpa a pilha. */
static const re5_import_def k_defs[] = {
    {"SteamAPI_Init",                  s_Init,                   0, 0},
    {"SteamAPI_IsSteamRunning",        s_IsSteamRunning,         0, 0},
    {"SteamAPI_RegisterCallResult",    s_RegisterCallResult,     0, 0},
    {"SteamAPI_RegisterCallback",      s_RegisterCallback,       0, 0},
    {"SteamAPI_RestartAppIfNecessary", s_RestartAppIfNecessary,  0, 0},
    {"SteamAPI_RunCallbacks",          s_RunCallbacks,           0, 0},
    {"SteamAPI_Shutdown",              s_Shutdown,               0, 0},
    {"SteamAPI_UnregisterCallResult",  s_UnregisterCallResult,   0, 0},
    {"SteamAPI_UnregisterCallback",    s_UnregisterCallback,     0, 0},
    {"SteamApps",                      s_SteamApps,              0, 0},
    {"SteamClient",                    s_SteamClient,            0, 0},
    {"SteamFriends",                   s_SteamFriends,           0, 0},
    {"SteamMatchmaking",               s_SteamMatchmaking,       0, 0},
    {"SteamNetworking",                s_SteamNetworking,        0, 0},
    {"SteamRemoteStorage",             s_SteamRemoteStorage,     0, 0},
    {"SteamUser",                      s_SteamUser,              0, 0},
    {"SteamUserStats",                 s_SteamUserStats,         0, 0},
    {"SteamUtils",                     s_SteamUtils,             0, 0},
    {0},
};

const re5_import_module re5_mod_steam_api = {"steam_api.dll", k_defs};
