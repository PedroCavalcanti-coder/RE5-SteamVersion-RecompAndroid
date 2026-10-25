/*
 * test_steam_lan.c - Teste unitário da subcamada LAN (ISteamMatchmaking009 e ISteamNetworking005).
 *
 * Modos:
 *  - Padrão (sem argumentos): identidade lida do arquivo (RE5_LAN_CONFIG), vtable,
 *    callbacks, metadados, loopback local e o pedido de entrada reenviado a um
 *    host que perde o primeiro pacote;
 *  - --identity <arquivo>: arquivo de identidade ausente é criado com um ID novo;
 *  - --host: host "Chris" cria a sala e tem que ser AVISADO (LobbyChatUpdate_t) da
 *    entrada de Sheva, com nome e ID dela; depois troca P2P;
 *  - --client: descobre o lobby do host via UDP, vê "Chris #101", entra e troca P2P no canal 0x389.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/steam_lan.h"

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
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *args)
{
    T->cpu.ecx = obj;
    return re5_guest_call(T, rd32(rd32(obj) + 4 * slot), n, args);
}

/* ISteamFriends014::GetFriendPersonaName (slot 7). */
static const char *friend_name(uint64_t id)
{
    uint32_t friends = flat("SteamFriends", 0, NULL);
    const uint32_t a[2] = {(uint32_t)id, (uint32_t)(id >> 32)};
    return re5_gstr(vcall(friends, 7, 2, a));
}

static uint32_t g_created_calls, g_enter_calls, g_chat_calls, g_data_calls, g_p2p_req_calls, g_persona_calls, g_fail_calls;
static uint64_t g_last_persona_user, g_last_fail_user;
static uint32_t g_last_fail_err;
static uint64_t g_last_lobby_created, g_last_enter_lobby;
static uint32_t g_last_enter_resp;
static uint64_t g_last_chat_user;
static uint32_t g_last_chat_state;
static uint64_t g_last_p2p_req_user;

static void cb_created_run(CPU *c)
{
    g_created_calls++;
    uint32_t p = re5_arg(c, 0);
    CHECK_EQ_U32(rd32(p), 1); /* k_EResultOK */
    g_last_lobby_created = (uint64_t)rd32(p + 12) << 32 | rd32(p + 8);
}

/* LobbyEnter_t: lobby +0, resposta +16. */
static void cb_enter_run(CPU *c)
{
    g_enter_calls++;
    uint32_t p = re5_arg(c, 0);
    g_last_enter_lobby = (uint64_t)rd32(p + 4) << 32 | rd32(p);
    g_last_enter_resp = rd32(p + 16);
}

/* LobbyChatUpdate_t: lobby +0, quem mudou +8, quem causou +16, estado +24. */
static void cb_chat_run(CPU *c)
{
    g_chat_calls++;
    uint32_t p = re5_arg(c, 0);
    g_last_chat_user = (uint64_t)rd32(p + 12) << 32 | rd32(p + 8);
    g_last_chat_state = rd32(p + 24);
}

static void cb_data_run(CPU *c)
{
    (void)c;
    g_data_calls++;
}

static void cb_p2p_req_run(CPU *c)
{
    g_p2p_req_calls++;
    uint32_t p = re5_arg(c, 0);
    g_last_p2p_req_user = (uint64_t)rd32(p + 4) << 32 | rd32(p);
}

/* PersonaStateChange_t: CSteamID +0, flags +8. */
static void cb_persona_run(CPU *c)
{
    g_persona_calls++;
    uint32_t p = re5_arg(c, 0);
    g_last_persona_user = (uint64_t)rd32(p + 4) << 32 | rd32(p);
    CHECK_EQ_U32(rd32(p + 8) & 1u, 1u); /* k_EPersonaChangeName */
}

/* P2PSessionConnectFail_t: CSteamID +0, erro (uint8) +8. */
static void cb_fail_run(CPU *c)
{
    g_fail_calls++;
    uint32_t p = re5_arg(c, 0);
    g_last_fail_user = (uint64_t)rd32(p + 4) << 32 | rd32(p);
    g_last_fail_err = rd8(p + 8);
}

static void cb_dummy(CPU *c) { (void)c; }

static uint32_t register_cb(const char *name, re5_host_fn run, int id)
{
    char n[64];
    uint32_t vt = gbuf(12), cb = gbuf(16);
    snprintf(n, sizeof n, "teste!%sResult", name);
    wr32(vt, re5_thunk_new(strdup(n), cb_dummy, 16, NULL));
    snprintf(n, sizeof n, "teste!%sRun", name);
    wr32(vt + 4, re5_thunk_new(strdup(n), run, 4, NULL));
    snprintf(n, sizeof n, "teste!%sSize", name);
    wr32(vt + 8, re5_thunk_new(strdup(n), cb_dummy, 0, NULL));
    wr32(cb, vt);
    const uint32_t reg[2] = {cb, (uint32_t)id};
    flat("SteamAPI_RegisterCallback", 2, reg);
    return cb;
}

static void register_all(void)
{
    register_cb("Created", cb_created_run, 513);
    register_cb("Enter", cb_enter_run, 504);
    register_cb("Chat", cb_chat_run, 506);
    register_cb("Data", cb_data_run, 505);
    register_cb("P2PReq", cb_p2p_req_run, 1202);
    register_cb("Persona", cb_persona_run, 304);
    register_cb("P2PFail", cb_fail_run, 1203);
}

static int run_host_mode(void)
{
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);
    register_all();
    uint32_t match = flat("SteamMatchmaking", 0, NULL);
    uint32_t net = flat("SteamNetworking", 0, NULL);

    uint64_t my_id = steam_lan_get_local_steam_id();
    CHECK_EQ_U32((uint32_t)my_id, 101);
    CHECK(strcmp(friend_name(my_id), "Chris #101") == 0);

    const uint32_t create_args[2] = {2, 2};
    vcall(match, 13, 2, create_args);
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_created_calls, 1);
    /* Como na Steam, o criador também recebe LobbyEnter_t: sem ele o jogo do host
     * não se considera na sala e não reage a quem entra. */
    CHECK_EQ_U32(g_enter_calls, 1);
    CHECK_EQ_U32(g_last_enter_resp, 1);
    uint64_t lobby_id = g_last_lobby_created;
    CHECK(g_last_enter_lobby == lobby_id);
    CHECK_EQ_U32((uint32_t)(lobby_id >> 32), RE5_LAN_STEAMID_LOBBY_HI);

    uint32_t k = gstr("Chapter"), v = gstr("1-1");
    const uint32_t set_args[4] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), k, v};
    vcall(match, 20, 4, set_args);
    /* Dado de membro do host antes de alguém entrar: tem que chegar ao cliente. */
    uint32_t kc = gstr("Character"), vc = gstr("Chris");
    const uint32_t set_mem_args[4] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), kc, vc};
    vcall(match, 25, 4, set_mem_args);

    /* O aviso de entrada: LobbyChatUpdate_t com o ID de Sheva e "entrou". */
    for (int t = 0; t < 500 && !g_chat_calls; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    CHECK_EQ_U32(g_chat_calls, 1);
    CHECK_EQ_U32(g_last_chat_state, 0x01); /* k_EChatMemberStateChangeEntered */
    uint64_t client_id = g_last_chat_user;
    CHECK_EQ_U32((uint32_t)client_id, 102);
    CHECK_EQ_U32((uint32_t)(client_id >> 32), RE5_LAN_STEAMID_USER_HI);
    CHECK(strcmp(friend_name(client_id), "Sheva #102") == 0);
    CHECK_EQ_U32(g_p2p_req_calls, 1);
    CHECK(g_last_p2p_req_user == client_id);

    const uint32_t lobby_args[2] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32)};
    CHECK_EQ_U32(vcall(match, 17, 2, lobby_args), 2);
    uint32_t sid_va = gbuf(8);
    const uint32_t get_mem_args[4] = {sid_va, (uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), 1};
    vcall(match, 18, 4, get_mem_args);
    CHECK(((uint64_t)rd32(sid_va + 4) << 32 | rd32(sid_va)) == client_id);

    /* Espera mensagem P2P do cliente */
    int got_msg = 0;
    uint32_t sz_out = gbuf(4);
    const uint32_t avail_args[2] = {sz_out, 0x389};
    for (int t = 0; t < 500; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
        if (vcall(net, 1, 2, avail_args) & 0xFF) {
            uint32_t dest = gbuf(1200), read_sz = gbuf(4), r_sid = gbuf(8);
            const uint32_t r_args[5] = {dest, 1200, read_sz, r_sid, 0x389};
            vcall(net, 2, 5, r_args);
            CHECK(strcmp(re5_gstr(dest), "HELLO_FROM_SHEVA") == 0);
            got_msg = 1;
            break;
        }
    }
    CHECK(got_msg);
    /* Pedidos de entrada repetidos não duplicam o membro nem o aviso. */
    CHECK_EQ_U32(g_chat_calls, 1);
    CHECK_EQ_U32(vcall(match, 17, 2, lobby_args), 2);

    /* Envia resposta P2P */
    const char resp[] = "HELLO_FROM_CHRIS";
    uint32_t send_buf = gbuf((uint32_t)strlen(resp) + 1);
    memcpy(re5_g2h(send_buf), resp, strlen(resp) + 1);
    const uint32_t s_args[6] = {(uint32_t)client_id, (uint32_t)(client_id >> 32), send_buf, (uint32_t)strlen(resp) + 1, 0, 0x389};
    CHECK_EQ_U32(vcall(net, 0, 6, s_args) & 0xFF, 1);

    /* Sheva troca nome e bandeira no menu LAN no meio da sala: o host recebe
     * PersonaStateChange_t e passa a vê-la com o nome novo e o mesmo ID. */
    for (int t = 0; t < 500 && !g_persona_calls; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    CHECK_EQ_U32(g_persona_calls, 1);
    CHECK(g_last_persona_user == client_id);
    CHECK(strcmp(friend_name(client_id), "Sheva Alomar #102") == 0);

    /* Ping medido pelos keepalives e país dela no estado da sala. */
    re5_lan_status st;
    for (int t = 0; t < 300; t++) {
        steam_lan_get_status(&st);
        if (st.member_count == 2 && st.members[1].rtt_ms >= 0) break;
        SDL_Delay(10);
    }
    CHECK(st.in_lobby && st.is_host);
    CHECK_EQ_U32(st.member_count, 2);
    CHECK(st.members[1].rtt_ms >= 0 && st.members[1].rtt_ms < 1000);
    CHECK(strcmp(st.members[1].country, "ZA") == 0);
    CHECK(strcmp(st.members[1].name, "Sheva Alomar") == 0);

    /* Ela some sem sair da sala (app fechado, Wi-Fi caiu): depois do prazo, o jogo
     * do host recebe "desconectou" e a falha da sessão P2P, como na Steam. */
    for (int t = 0; t < 800 && !g_fail_calls; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    CHECK_EQ_U32(g_fail_calls, 1);
    CHECK(g_last_fail_user == client_id);
    CHECK_EQ_U32(g_last_fail_err, 4); /* k_EP2PSessionErrorTimeout */
    CHECK_EQ_U32(g_chat_calls, 2);
    CHECK(g_last_chat_user == client_id);
    CHECK_EQ_U32(g_last_chat_state, 0x04); /* k_EChatMemberStateChangeDisconnected */
    CHECK_EQ_U32(vcall(match, 17, 2, lobby_args), 1);

    vcall(match, 15, 2, lobby_args);
    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
}

static int run_client_mode(void)
{
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);
    register_all();
    uint32_t match = flat("SteamMatchmaking", 0, NULL);
    uint32_t net = flat("SteamNetworking", 0, NULL);

    /* Descobre o lobby via broadcast; espera os metadados do host também. */
    uint64_t target_lobby = 0;
    uint32_t k = gstr("Chapter");
    for (int t = 0; t < 500; t++) {
        vcall(match, 4, 0, NULL); /* RequestLobbyList */
        SDL_Delay(20);
        flat("SteamAPI_RunCallbacks", 0, NULL);
        if (steam_lan_get_lobby_count() > 0) {
            uint64_t l = steam_lan_get_lobby_by_index(0);
            const uint32_t get_args[3] = {(uint32_t)l, (uint32_t)(l >> 32), k};
            if (strcmp(re5_gstr(vcall(match, 19, 3, get_args)), "1-1") == 0) {
                target_lobby = l;
                break;
            }
        }
    }
    CHECK(target_lobby != 0);
    CHECK_EQ_U32((uint32_t)(target_lobby >> 32), RE5_LAN_STEAMID_LOBBY_HI);

    /* Na lista, o dono da sala aparece com o nome que escolheu e o ID dele. */
    uint32_t owner_va = gbuf(8);
    const uint32_t get_owner_args[3] = {owner_va, (uint32_t)target_lobby, (uint32_t)(target_lobby >> 32)};
    vcall(match, 35, 3, get_owner_args);
    uint64_t host_id = (uint64_t)rd32(owner_va + 4) << 32 | rd32(owner_va);
    CHECK_EQ_U32((uint32_t)host_id, 101);
    CHECK(strcmp(friend_name(host_id), "Chris #101") == 0);

    /* Entra no lobby */
    const uint32_t join_args[2] = {(uint32_t)target_lobby, (uint32_t)(target_lobby >> 32)};
    vcall(match, 14, 2, join_args);

    for (int t = 0; t < 500 && !g_enter_calls; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    CHECK_EQ_U32(g_enter_calls, 1);
    CHECK_EQ_U32(g_last_enter_resp, 1); /* k_EChatRoomEnterResponseSuccess */
    CHECK(g_last_enter_lobby == target_lobby);
    CHECK_EQ_U32(vcall(match, 17, 2, join_args), 2);
    /* Quem entra não recebe LobbyChatUpdate_t de si mesmo. */
    CHECK_EQ_U32(g_chat_calls, 0);

    /* Dado de membro que o host gravou antes da entrada. */
    uint32_t kc = gstr("Character");
    const char *hero = "";
    for (int t = 0; t < 100; t++) {
        const uint32_t a[5] = {(uint32_t)target_lobby, (uint32_t)(target_lobby >> 32), (uint32_t)host_id,
                               (uint32_t)(host_id >> 32), kc};
        hero = re5_gstr(vcall(match, 24, 5, a));
        if (*hero) break;
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    CHECK(strcmp(hero, "Chris") == 0);

    const char msg[] = "HELLO_FROM_SHEVA";
    uint32_t send_buf = gbuf((uint32_t)strlen(msg) + 1);
    memcpy(re5_g2h(send_buf), msg, strlen(msg) + 1);
    const uint32_t s_args[6] = {(uint32_t)host_id, (uint32_t)(host_id >> 32), send_buf, (uint32_t)strlen(msg) + 1, 0, 0x389};
    CHECK_EQ_U32(vcall(net, 0, 6, s_args) & 0xFF, 1);

    /* Aguarda resposta P2P do host */
    int got_resp = 0;
    uint32_t sz_out = gbuf(4);
    const uint32_t avail_args[2] = {sz_out, 0x389};
    for (int t = 0; t < 500; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
        if (vcall(net, 1, 2, avail_args) & 0xFF) {
            uint32_t dest = gbuf(1200), read_sz = gbuf(4), r_sid = gbuf(8);
            const uint32_t r_args[5] = {dest, 1200, read_sz, r_sid, 0x389};
            vcall(net, 2, 5, r_args);
            CHECK(strcmp(re5_gstr(dest), "HELLO_FROM_CHRIS") == 0);
            got_resp = 1;
            break;
        }
    }
    CHECK(got_resp);

    CHECK(steam_lan_set_profile("Sheva Alomar", "za"));
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_persona_calls, 1); /* o próprio jogo também é avisado */
    CHECK(g_last_persona_user == steam_lan_get_local_steam_id());
    CHECK(strcmp(friend_name(steam_lan_get_local_steam_id()), "Sheva Alomar #102") == 0);

    /* Fica na sala o bastante para o host medir o ping e então some sem LeaveLobby
     * nem Shutdown: é o app do celular fechado à força. */
    for (int t = 0; t < 250; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    CHECK_EQ_U32(g_fail_calls, 0);
    CHECK_DONE();
}

/* Arquivo de identidade ausente: criado com nome padrão e um ID de seis dígitos,
 * que é o account id do CSteamID. */
static int run_identity_mode(const char *path)
{
    remove(path);
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);
    uint64_t id = steam_lan_get_local_steam_id();
    CHECK_EQ_U32((uint32_t)(id >> 32), RE5_LAN_STEAMID_USER_HI);
    CHECK((uint32_t)id >= RE5_LAN_ID_MIN && (uint32_t)id < RE5_LAN_ID_MIN + RE5_LAN_ID_SPAN);

    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    char text[1024] = "";
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
    }
    /* Outra instância na porta base (testes em paralelo) desloca o ID pelo slot. */
    uint32_t slot = steam_lan_get_p2p_port() - 27016u;
    char want[32];
    snprintf(want, sizeof want, "\nid=%u\n", (uint32_t)id - slot);
    CHECK(strstr(text, want) != NULL);
    CHECK(strstr(text, "\nnome=Jogador\n") != NULL);
    CHECK(strstr(text, "\npais=BR\n") != NULL);   /* da localidade (LANG=pt_BR.UTF-8 no CTest) */

    char display[64];
    snprintf(display, sizeof display, "Jogador #%u", (uint32_t)id);
    uint32_t friends = flat("SteamFriends", 0, NULL);
    CHECK(strcmp(re5_gstr(vcall(friends, 0, 0, NULL)), display) == 0);

    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
}

/* ---- host falso: perde o primeiro pedido de entrada ---- */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type, flags;
    uint16_t port;
    uint64_t sender;
    uint32_t nonce;
} fake_hdr;
typedef struct {
    uint64_t lobby_id, owner_id;
    uint16_t member_count, max_members;
    char     owner_name[32];
    uint16_t data_count;
    char     owner_country[4];
} fake_beacon;
typedef struct {
    uint64_t lobby_id;
    uint32_t response_code;
    uint16_t member_count;
} fake_join_resp;
#pragma pack(pop)

#define FAKE_HOST_ID    (((uint64_t)RE5_LAN_STEAMID_USER_HI << 32) | 555555u)
#define FAKE_LOBBY_ID   (((uint64_t)RE5_LAN_STEAMID_LOBBY_HI << 32) | 555555u)

static void fake_host_join(uint32_t match)
{
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    struct sockaddr_in me, client;
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, (struct sockaddr *)&me, sizeof me) == 0);
    socklen_t ml = sizeof me;
    getsockname(s, (struct sockaddr *)&me, &ml);
    client = me;
    client.sin_port = htons(steam_lan_get_p2p_port());

    uint8_t buf[256];
    fake_hdr *h = (fake_hdr *)buf;
    *h = (fake_hdr){0x35455221u, 0x01 /* BEACON */, 0, ntohs(me.sin_port), FAKE_HOST_ID, 0xABCD};
    fake_beacon *b = (fake_beacon *)(buf + sizeof *h);
    memset(b, 0, sizeof *b);
    b->lobby_id = FAKE_LOBBY_ID;
    b->owner_id = FAKE_HOST_ID;
    b->member_count = 1;
    b->max_members = 2;
    snprintf(b->owner_name, sizeof b->owner_name, "Josh");
    memcpy(b->owner_country, "ZA", 2);
    sendto(s, (const char *)buf, sizeof *h + sizeof *b, 0, (struct sockaddr *)&client, sizeof client);

    uint64_t lobby = 0;
    for (int t = 0; t < 100 && !lobby; t++) {
        SDL_Delay(5);
        steam_lan_poll();
        for (int i = 0; i < steam_lan_get_lobby_count(); i++)
            if (steam_lan_get_lobby_by_index(i) == FAKE_LOBBY_ID) lobby = FAKE_LOBBY_ID;
    }
    CHECK(lobby == FAKE_LOBBY_ID);
    CHECK(strcmp(friend_name(FAKE_HOST_ID), "Josh #555555") == 0);

    g_enter_calls = 0;
    const uint32_t join_args[2] = {(uint32_t)FAKE_LOBBY_ID, (uint32_t)(FAKE_LOBBY_ID >> 32)};
    vcall(match, 14, 2, join_args);

    /* Ignora o 1º pedido (pacote "perdido") e responde ao reenvio. */
    int reqs = 0;
    for (int t = 0; t < 400 && reqs < 2; t++) {
        SDL_Delay(5);
        int n = (int)recv(s, (char *)buf, sizeof buf, 0);
        if (n >= (int)sizeof(fake_hdr) && ((fake_hdr *)buf)->type == 0x03 /* JOIN_REQ */) {
            reqs++;
            CHECK(((fake_hdr *)buf)->sender == steam_lan_get_local_steam_id());
        }
    }
    CHECK_EQ_U32(reqs, 2);
    CHECK_EQ_U32(g_enter_calls, 0);

    *h = (fake_hdr){0x35455221u, 0x04 /* JOIN_RESP */, 0, ntohs(me.sin_port), FAKE_HOST_ID, 0xABCD};
    fake_join_resp *r = (fake_join_resp *)(buf + sizeof *h);
    *r = (fake_join_resp){FAKE_LOBBY_ID, 1, 2};
    sendto(s, (const char *)buf, sizeof *h + sizeof *r, 0, (struct sockaddr *)&client, sizeof client);
    /* Resposta atrasada de outro pedido: tem que ser ignorada. */
    r->response_code = 4; /* Full */
    sendto(s, (const char *)buf, sizeof *h + sizeof *r, 0, (struct sockaddr *)&client, sizeof client);

    for (int t = 0; t < 100 && !g_enter_calls; t++) {
        SDL_Delay(5);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }
    SDL_Delay(20);
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_enter_calls, 1);
    CHECK_EQ_U32(g_last_enter_resp, 1);
    CHECK_EQ_U32(vcall(match, 17, 2, join_args), 2);

    vcall(match, 15, 2, join_args);
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

int main(int argc, char **argv)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x20000u, RE5_PAGE_READWRITE, "teste_lan");

    if (argc > 1 && strcmp(argv[1], "--host") == 0) return run_host_mode();
    if (argc > 1 && strcmp(argv[1], "--client") == 0) return run_client_mode();
    if (argc > 2 && strcmp(argv[1], "--identity") == 0) return run_identity_mode(argv[2]);

    /* Identidade vinda do arquivo: nome saneado ('#' separa o ID), ID de seis dígitos. */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "w");
        CHECK(f != NULL);
        if (f) {
            fputs("# comentario\nnome =  Leon#Kennedy  \nid=424242\npais= pt\nhosts=10.0.0.9\n", f);
            fclose(f);
        }
    }

    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);

    /* 1. Identidade */
    uint64_t my_id = steam_lan_get_local_steam_id();
    CHECK((my_id >> 32) == RE5_LAN_STEAMID_USER_HI);
    CHECK(steam_lan_get_p2p_port() >= 27016);
    uint32_t slot = steam_lan_get_p2p_port() - (getenv("RE5_LAN_PORT") ? (uint32_t)atoi(getenv("RE5_LAN_PORT")) : 27016u);
    if (argc > 1) {
        CHECK_EQ_U32((uint32_t)my_id, 424242u + slot);
        CHECK(strcmp(steam_lan_get_local_name(), "Leon_Kennedy") == 0);
        char cc[4];
        steam_lan_get_local_country(cc);
        CHECK(strcmp(cc, "PT") == 0);
        /* A bandeira que o jogo mostra: ISteamUtils007::GetIPCountry (slot 4). */
        uint32_t utils = flat("SteamUtils", 0, NULL);
        CHECK(strcmp(re5_gstr(vcall(utils, 4, 0, NULL)), "PT") == 0);
        char hosts[64];
        steam_lan_get_manual_hosts(hosts, sizeof hosts);
        CHECK(strcmp(hosts, "10.0.0.9") == 0);

        /* IPs digitados: os inválidos ficam de fora; tudo volta ao arquivo. */
        CHECK_EQ_U32(steam_lan_set_manual_hosts("192.168.43.1, 999.1.1.1 ,abc,10.0.0.5"), 2);
        steam_lan_get_manual_hosts(hosts, sizeof hosts);
        CHECK(strcmp(hosts, "192.168.43.1,10.0.0.5") == 0);
        CHECK(!steam_lan_set_profile("", "BR"));     /* nome vazio */
        CHECK(!steam_lan_set_profile("Leon", "B1"));  /* país inválido */
        CHECK(steam_lan_set_profile("Leon S. Kennedy", "us"));
        CHECK(strcmp(re5_gstr(vcall(utils, 4, 0, NULL)), "US") == 0);
        FILE *rf = fopen(argv[1], "r");
        char text[1024] = "";
        if (rf) {
            text[fread(text, 1, sizeof text - 1, rf)] = 0;
            fclose(rf);
        }
        CHECK(strstr(text, "\nnome=Leon S. Kennedy\n") != NULL);
        CHECK(strstr(text, "\npais=US\n") != NULL);
        CHECK(strstr(text, "\nhosts=192.168.43.1,10.0.0.5\n") != NULL);
        CHECK(strstr(text, "\nid=424242\n") != NULL);
        flat("SteamAPI_RunCallbacks", 0, NULL);
    }

    uint32_t friends = flat("SteamFriends", 0, NULL);
    CHECK(friends != 0);
    char display[64];
    steam_lan_get_display_name(my_id, display, sizeof display);
    uint32_t name_va = vcall(friends, 0, 0, NULL);
    CHECK(strcmp(re5_gstr(name_va), display) == 0);
    /* Mesmo texto, mesmo ponteiro: o nome não gasta a arena a cada chamada. */
    CHECK_EQ_U32(vcall(friends, 0, 0, NULL), name_va);
    CHECK(strcmp(friend_name(my_id), display) == 0);
    /* Par desconhecido: nome genérico com o ID. */
    CHECK(strcmp(friend_name(((uint64_t)RE5_LAN_STEAMID_USER_HI << 32) | 777777u), "Jogador #777777") == 0);

    /* 2. Callbacks: LobbyCreated_t (513), LobbyEnter_t (504), LobbyChatUpdate_t (506),
     *    LobbyDataUpdate_t (505), P2PSessionRequest_t (1202) */
    register_all();

    /* 3. Criação de Lobby via ISteamMatchmaking009 (slot 13) */
    uint32_t match = flat("SteamMatchmaking", 0, NULL);
    CHECK(match != 0);

    const uint32_t create_args[2] = {2 /* k_ELobbyTypePublic */, 2 /* max 2 players */};
    uint32_t call_id = vcall(match, 13, 2, create_args);
    CHECK(call_id != 0);

    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_created_calls, 1);
    CHECK_EQ_U32(g_enter_calls, 1); /* o criador também entra na sala */
    CHECK_EQ_U32(g_data_calls, 1);
    CHECK(g_last_lobby_created != 0);
    uint64_t lobby_id = g_last_lobby_created;
    CHECK(g_last_enter_lobby == lobby_id);
    CHECK_EQ_U32((uint32_t)(lobby_id >> 32), RE5_LAN_STEAMID_LOBBY_HI);

    /* 4. Consulta ao estado do Lobby */
    const uint32_t lobby_args[2] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32)};
    uint32_t members = vcall(match, 17, 2, lobby_args);
    CHECK_EQ_U32(members, 1);

    uint32_t owner_sid = gbuf(8);
    const uint32_t get_owner_args[3] = {owner_sid, (uint32_t)lobby_id, (uint32_t)(lobby_id >> 32)};
    vcall(match, 35, 3, get_owner_args);
    uint64_t owner = (uint64_t)rd32(owner_sid + 4) << 32 | rd32(owner_sid);
    CHECK(owner == my_id);

    /* 5. Metadados do Lobby (SetLobbyData / GetLobbyData) */
    uint32_t k_chap = gstr("Chapter"), v_chap = gstr("1-1");
    const uint32_t set_data_args[4] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), k_chap, v_chap};
    CHECK_EQ_U32(vcall(match, 20, 4, set_data_args) & 0xFF, 1);

    const uint32_t get_data_args[3] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), k_chap};
    uint32_t res_val = vcall(match, 19, 3, get_data_args);
    CHECK(strcmp(re5_gstr(res_val), "1-1") == 0);
    CHECK_EQ_U32(vcall(match, 19, 3, get_data_args), res_val);

    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_data_calls, 2);

    /* 6. Metadados de Membro (SetLobbyMemberData / GetLobbyMemberData) */
    uint32_t k_hero = gstr("Character"), v_hero = gstr("Chris");
    const uint32_t set_mem_args[4] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), k_hero, v_hero};
    vcall(match, 25, 4, set_mem_args);

    const uint32_t get_mem_args[5] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32),
                                      (uint32_t)my_id, (uint32_t)(my_id >> 32), k_hero};
    uint32_t res_hero = vcall(match, 24, 5, get_mem_args);
    CHECK(strcmp(re5_gstr(res_hero), "Chris") == 0);

    /* 7. Transporte P2P do Canal 0x389 (ISteamNetworking005) */
    uint32_t net = flat("SteamNetworking", 0, NULL);
    CHECK(net != 0);

    uint32_t sz_out = gbuf(4);
    const uint32_t avail_args[2] = {sz_out, 0x389};
    CHECK_EQ_U32(vcall(net, 1, 2, avail_args) & 0xFF, 0); /* Inicialmente vazia */

    /* 8. Saída do Lobby */
    vcall(match, 15, 2, lobby_args);
    CHECK_EQ_U32(vcall(match, 17, 2, lobby_args), 0);

    /* 9. Entrada com o 1º pedido perdido: o cliente reenvia e só a 1ª resposta vale. */
    fake_host_join(match);

    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
}
