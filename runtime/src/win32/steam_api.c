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
#include "re5/steam_lan.h"

#define APP_ID       21690u
#define STEAM_ID_LO  0x00000001u   /* conta 1 */
#define STEAM_ID_HI  0x01100001u   /* universo público, individual, instância 1 */
#define PERSONA_NAME "Player"
#define VT_SIZE      80u

static uint64_t g_next_call_id = 1000;
static uint64_t next_call_id(void) { return ++g_next_call_id; }

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
    steam_lan_poll();
    re5_lan_event evt;
    while (steam_lan_pop_event(&evt)) {
        switch (evt.type) {
        case RE5_LAN_EVT_LOBBY_CREATED: {
            uint32_t d[4] = {evt.param1, 0, (uint32_t)evt.lobby_id, (uint32_t)(evt.lobby_id >> 32)};
            post_callback(513, d, sizeof d, evt.call_id);
            break;
        }
        case RE5_LAN_EVT_LOBBY_ENTERED: {
            uint32_t d[5] = {(uint32_t)evt.lobby_id, (uint32_t)(evt.lobby_id >> 32), 0, 0, evt.param1};
            post_callback(504, d, sizeof d, evt.call_id);
            break;
        }
        case RE5_LAN_EVT_LOBBY_MATCH_LIST: {
            uint32_t d[1] = {evt.param1};
            post_callback(510, d, sizeof d, evt.call_id);
            break;
        }
        case RE5_LAN_EVT_LOBBY_CHAT_UPDATE: {
            uint32_t d[8] = {
                (uint32_t)evt.lobby_id, (uint32_t)(evt.lobby_id >> 32),
                (uint32_t)evt.user_id,  (uint32_t)(evt.user_id >> 32),
                (uint32_t)evt.user_id,  (uint32_t)(evt.user_id >> 32),
                evt.param1, 0
            };
            post_callback(506, d, sizeof d, 0);
            break;
        }
        case RE5_LAN_EVT_LOBBY_DATA_UPDATE: {
            uint32_t d[6] = {
                (uint32_t)evt.lobby_id, (uint32_t)(evt.lobby_id >> 32),
                (uint32_t)evt.user_id,  (uint32_t)(evt.user_id >> 32),
                evt.param1 & 0xFF, 0
            };
            post_callback(505, d, sizeof d, 0);
            break;
        }
        default:
            break;
        }
    }

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
    steam_lan_init();
    RE5_INFO("steam_api", "SteamAPI_Init: Steam LAN (app %u, usuario '%s', SteamID %llu, porta %u)",
             APP_ID, steam_lan_get_local_name(), (unsigned long long)steam_lan_get_local_steam_id(), steam_lan_get_p2p_port());
    c->eax = 1;
}

static void s_Shutdown(CPU *c)
{
    (void)c;
    steam_lan_shutdown();
}

static void t_Nothing(CPU *c) { (void)c; }
static void t_False(CPU *c) { ret_bool(c, 0); }
static void t_Zero(CPU *c) { c->eax = 0; }

/* ---- ISteamUser017 ---- */

static void u_GetHSteamUser(CPU *c) { c->eax = 1; }
static void u_BLoggedOn(CPU *c) { ret_bool(c, 1); }
static void u_GetSteamID(CPU *c)
{
    uint64_t id = steam_lan_get_local_steam_id();
    ret_steamid(c, (uint32_t)id, (uint32_t)(id >> 32));
}
static void u_GetPlayerSteamLevel(CPU *c) { c->eax = 0; }

static void u_GetUserDataFolder(CPU *c)
{
    uint32_t buf = re5_arg(c, 0), n = re5_arg(c, 1);
    char path[128];
    snprintf(path, sizeof path, "C:\\Program Files (x86)\\Steam\\userdata\\%u\\%u\\local", (uint32_t)steam_lan_get_local_steam_id(), APP_ID);
    ret_bool(c, 0);
    if (strlen(path) + 1 > n) return;
    memcpy(re5_g2h(buf), path, strlen(path) + 1);
    ret_bool(c, 1);
}

static void u_StartVoiceRecording(CPU *c)
{
    (void)c;
}

static void u_StopVoiceRecording(CPU *c)
{
    (void)c;
}

static void u_GetAvailableVoice(CPU *c)
{
    uint32_t pcbCompressed = re5_arg(c, 0);
    uint32_t pcbUncompressed = re5_arg(c, 1);
    if (pcbCompressed) wr32(pcbCompressed, 0);
    if (pcbUncompressed) wr32(pcbUncompressed, 0);
    c->eax = 2; /* k_EVoiceResultNotRecording */
}

static void u_GetVoice(CPU *c)
{
    uint32_t pnWritten = re5_arg(c, 3);
    uint32_t pnUncompWritten = re5_arg(c, 7);
    if (pnWritten) wr32(pnWritten, 0);
    if (pnUncompWritten) wr32(pnUncompWritten, 0);
    c->eax = 2; /* k_EVoiceResultNotRecording */
}

static void u_DecompressVoice(CPU *c)
{
    uint32_t pnWritten = re5_arg(c, 4);
    if (pnWritten) wr32(pnWritten, 0);
    c->eax = 3; /* k_EVoiceResultNoData */
}

static void u_GetVoiceOptimalSampleRate(CPU *c)
{
    c->eax = 44100;
}

static void u_GetAuthSessionTicket(CPU *c)
{
    uint32_t pTicket = re5_arg(c, 0);
    uint32_t cbMax = re5_arg(c, 1);
    uint32_t pcbTicket = re5_arg(c, 2);
    if (pTicket && cbMax >= 4) wr32(pTicket, 0x12345678);
    if (pcbTicket) wr32(pcbTicket, 4);
    c->eax = 1; /* HAuthTicket válido */
}

static void u_BeginAuthSession(CPU *c)
{
    c->eax = 0; /* k_EBeginAuthSessionResultOK */
}

static void u_UserHasLicenseForApp(CPU *c)
{
    c->eax = 0; /* k_EUserHasLicenseResultHasLicense */
}

static const method k_user[] = {
    {0, "GetHSteamUser", u_GetHSteamUser, 0},
    {1, "BLoggedOn", u_BLoggedOn, 0},
    {2, "GetSteamID", u_GetSteamID, 4},
    {3, "InitiateGameConnection", t_Zero, 28},
    {4, "TerminateGameConnection", t_Nothing, 8},
    {5, "TrackAppUsageEvent", t_Nothing, 16},
    {6, "GetUserDataFolder", u_GetUserDataFolder, 8},
    {7, "StartVoiceRecording", u_StartVoiceRecording, 0},
    {8, "StopVoiceRecording", u_StopVoiceRecording, 0},
    {9, "GetAvailableVoice", u_GetAvailableVoice, 12},
    {10, "GetVoice", u_GetVoice, 36},
    {11, "DecompressVoice", u_DecompressVoice, 24},
    {12, "GetVoiceOptimalSampleRate", u_GetVoiceOptimalSampleRate, 0},
    {13, "GetAuthSessionTicket", u_GetAuthSessionTicket, 12},
    {14, "BeginAuthSession", u_BeginAuthSession, 16},
    {15, "EndAuthSession", t_Nothing, 8},
    {16, "CancelAuthTicket", t_Nothing, 4},
    {17, "UserHasLicenseForApp", u_UserHasLicenseForApp, 12},
    {18, "BIsBehindNAT", t_False, 0},
    {19, "AdvertiseGame", t_Nothing, 16},
    {20, "RequestEncryptedAppTicket", t_Zero, 8},
    {21, "GetEncryptedAppTicket", t_False, 12},
    {22, "GetGameBadgeLevel", t_Zero, 8},
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
    c->eax = gstr(steam_lan_get_local_name());
}

static void f_GetFriendPersonaName(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    c->eax = gstr(steam_lan_get_peer_name(id));
}

static void f_GetPersonaState(CPU *c) { c->eax = 1; }   /* k_EPersonaStateOnline */

static const method k_friends[] = {
    {0, "GetPersonaName", f_GetPersonaName, 0},
    {2, "GetPersonaState", f_GetPersonaState, 0},
    {3, "GetFriendCount", t_Zero, 4},
    {4, "GetFriendByIndex", t_Nothing, 12},
    {7, "GetFriendPersonaName", f_GetFriendPersonaName, 8},
    {22, "ActivateGameOverlay", t_Nothing, 4},
    {23, "ActivateGameOverlayToUser", t_Nothing, 12},
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

/* ---- ISteamNetworking005: P2P e sockets em LAN ---- */

static void n_SendP2PPacket(CPU *c)
{
    uint64_t remote_id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    uint32_t data_va = re5_arg(c, 2);
    uint32_t data_len = re5_arg(c, 3);
    int send_type = (int)re5_arg(c, 4);
    int channel = (int)re5_arg(c, 5);
    ret_bool(c, steam_lan_send_p2p(remote_id, re5_g2h(data_va), data_len, send_type, channel));
}

static void n_IsP2PPacketAvailable(CPU *c)
{
    uint32_t size_va = re5_arg(c, 0);
    int channel = (int)re5_arg(c, 1);
    uint32_t msg_sz = 0;
    int ok = steam_lan_is_p2p_available(&msg_sz, channel);
    if (size_va) wr32(size_va, msg_sz);
    ret_bool(c, ok);
}

static void n_ReadP2PPacket(CPU *c)
{
    uint32_t dest_va = re5_arg(c, 0);
    uint32_t dest_len = re5_arg(c, 1);
    uint32_t size_va = re5_arg(c, 2);
    uint32_t remote_va = re5_arg(c, 3);
    int channel = (int)re5_arg(c, 4);

    uint32_t msg_sz = 0;
    uint64_t remote_id = 0;
    int ok = steam_lan_read_p2p(re5_g2h(dest_va), dest_len, &msg_sz, &remote_id, channel);
    if (size_va) wr32(size_va, msg_sz);
    if (remote_va) {
        wr32(remote_va, (uint32_t)remote_id);
        wr32(remote_va + 4, (uint32_t)(remote_id >> 32));
    }
    ret_bool(c, ok);
}

static void n_AcceptP2PSessionWithUser(CPU *c)
{
    uint64_t remote_id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    ret_bool(c, steam_lan_accept_p2p(remote_id));
}

static void n_CloseP2PSessionWithUser(CPU *c)
{
    uint64_t remote_id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    ret_bool(c, steam_lan_close_p2p(remote_id));
}

static void n_GetP2PSessionState(CPU *c)
{
    uint64_t remote_id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    uint32_t st = re5_arg(c, 2);
    ret_bool(c, steam_lan_get_p2p_state(remote_id, st ? re5_g2h(st) : NULL));
}

static const method k_networking[] = {
    {0, "SendP2PPacket", n_SendP2PPacket, 24},
    {1, "IsP2PPacketAvailable", n_IsP2PPacketAvailable, 8},
    {2, "ReadP2PPacket", n_ReadP2PPacket, 20},
    {3, "AcceptP2PSessionWithUser", n_AcceptP2PSessionWithUser, 8},
    {4, "CloseP2PSessionWithUser", n_CloseP2PSessionWithUser, 8},
    {5, "CloseP2PChannelWithUser", a_True, 12},
    {6, "GetP2PSessionState", n_GetP2PSessionState, 12},
    {7, "AllowP2PPacketRelay", a_True, 4},
    {8, "CreateListenSocket", t_Zero, 16},         /* 0 = k_HSteamListenSocket_Invalid */
    {9, "CreateP2PConnectionSocket", t_Zero, 20},
    {10, "CreateConnectionSocket", t_Zero, 12},
    {11, "DestroySocket", t_False, 8},
    {12, "DestroyListenSocket", t_False, 8},
    {13, "SendDataOnSocket", t_False, 16},
    {14, "IsDataAvailableOnSocket", t_False, 8},
    {15, "RetrieveDataFromSocket", t_False, 16},
    {16, "IsDataAvailable", t_False, 12},
    {17, "RetrieveData", t_False, 20},
    {18, "GetSocketInfo", t_False, 20},
    {19, "GetListenSocketInfo", t_False, 12},
    {20, "GetSocketConnectionType", t_Zero, 4},    /* k_ESNetSocketConnectionTypeNotConnected */
    {21, "GetMaxPacketSize", t_Zero, 4},
    {0, NULL, NULL, 0},
};

/* ---- ISteamMatchmaking009 ---- */

static void m_RequestLobbyList(CPU *c)
{
    uint64_t call = next_call_id();
    steam_lan_request_lobby_list(call);
    ret_u64(c, call);
}

static void m_GetLobbyByIndex(CPU *c)
{
    int idx = (int)re5_arg(c, 1);
    uint64_t id = steam_lan_get_lobby_by_index(idx);
    ret_steamid(c, (uint32_t)id, (uint32_t)(id >> 32));
}

static void m_CreateLobby(CPU *c)
{
    uint32_t type = re5_arg(c, 0);
    int max_members = (int)re5_arg(c, 1);
    uint64_t call = next_call_id();
    steam_lan_create_lobby(type, max_members, call);
    ret_u64(c, call);
}

static void m_JoinLobby(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    uint64_t call = next_call_id();
    steam_lan_join_lobby(id, call);
    ret_u64(c, call);
}

static void m_LeaveLobby(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    steam_lan_leave_lobby(id);
}

static void m_GetNumLobbyMembers(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    c->eax = (uint32_t)steam_lan_get_num_lobby_members(id);
}

static void m_GetLobbyMemberByIndex(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 2) << 32 | re5_arg(c, 1);
    int idx = (int)re5_arg(c, 3);
    uint64_t member = steam_lan_get_lobby_member_by_index(id, idx);
    ret_steamid(c, (uint32_t)member, (uint32_t)(member >> 32));
}

static void m_GetLobbyData(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    const char *key = re5_gstr(re5_arg(c, 2));
    c->eax = gstr(steam_lan_get_lobby_data(id, key));
}

static void m_SetLobbyData(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    const char *key = re5_gstr(re5_arg(c, 2));
    const char *val = re5_gstr(re5_arg(c, 3));
    ret_bool(c, steam_lan_set_lobby_data(id, key, val));
}

static void m_GetLobbyMemberData(CPU *c)
{
    uint64_t lobby_id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    uint64_t user_id  = (uint64_t)re5_arg(c, 3) << 32 | re5_arg(c, 2);
    const char *key = re5_gstr(re5_arg(c, 4));
    c->eax = gstr(steam_lan_get_lobby_member_data(lobby_id, user_id, key));
}

static void m_SetLobbyMemberData(CPU *c)
{
    uint64_t lobby_id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    const char *key = re5_gstr(re5_arg(c, 2));
    const char *val = re5_gstr(re5_arg(c, 3));
    steam_lan_set_lobby_member_data(lobby_id, key, val);
}

static void m_SetLobbyJoinable(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    int joinable = (int)re5_arg(c, 2);
    steam_lan_set_lobby_joinable(id, joinable);
    ret_bool(c, 1);
}

static void m_GetLobbyOwner(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 2) << 32 | re5_arg(c, 1);
    uint64_t owner = steam_lan_get_lobby_owner(id);
    ret_steamid(c, (uint32_t)owner, (uint32_t)(owner >> 32));
}

static void m_GetLobbyMemberLimit(CPU *c)
{
    uint64_t id = (uint64_t)re5_arg(c, 1) << 32 | re5_arg(c, 0);
    c->eax = (uint32_t)steam_lan_get_lobby_member_limit(id);
}

static const method k_matchmaking[] = {
    {0, "GetFavoriteGameCount", t_Zero, 0},
    {4, "RequestLobbyList", m_RequestLobbyList, 0},
    {5, "AddRequestLobbyListStringFilter", t_Nothing, 12},
    {6, "AddRequestLobbyListNumericalFilter", t_Nothing, 12},
    {7, "AddRequestLobbyListNearValueFilter", t_Nothing, 8},
    {8, "AddRequestLobbyListFilterSlotsAvailable", t_Nothing, 4},
    {9, "AddRequestLobbyListDistanceFilter", t_Nothing, 4},
    {10, "AddRequestLobbyListResultCountFilter", t_Nothing, 4},
    {11, "AddRequestLobbyListCompatibleMembersFilter", t_Nothing, 8},
    {12, "GetLobbyByIndex", m_GetLobbyByIndex, 8},
    {13, "CreateLobby", m_CreateLobby, 8},
    {14, "JoinLobby", m_JoinLobby, 8},
    {15, "LeaveLobby", m_LeaveLobby, 8},
    {16, "InviteUserToLobby", a_True, 16},
    {17, "GetNumLobbyMembers", m_GetNumLobbyMembers, 8},
    {18, "GetLobbyMemberByIndex", m_GetLobbyMemberByIndex, 16},
    {19, "GetLobbyData", m_GetLobbyData, 12},
    {20, "SetLobbyData", m_SetLobbyData, 16},
    {21, "GetLobbyDataCount", t_Zero, 8},
    {24, "GetLobbyMemberData", m_GetLobbyMemberData, 20},
    {25, "SetLobbyMemberData", m_SetLobbyMemberData, 16},
    {26, "SendLobbyChatMsg", a_True, 16},
    {28, "RequestLobbyData", a_True, 8},
    {31, "SetLobbyMemberLimit", a_True, 12},
    {32, "GetLobbyMemberLimit", m_GetLobbyMemberLimit, 8},
    {33, "SetLobbyType", a_True, 12},
    {34, "SetLobbyJoinable", m_SetLobbyJoinable, 12},
    {35, "GetLobbyOwner", m_GetLobbyOwner, 12},
    {0, NULL, NULL, 0},
};

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
static interface g_matchmaking = {"ISteamMatchmaking009", k_matchmaking, 0};
static interface g_networking = {"ISteamNetworking005", k_networking, 0};
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
