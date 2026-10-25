/*
 * test_steam_lan.c - Teste unitário da subcamada LAN (ISteamMatchmaking009 e ISteamNetworking005).
 *
 * Suporta três modos:
 *  - Padrão (sem argumentos): testa vtable, callbacks, metadados e loopback local;
 *  - --host: inicia como host "Chris", cria sala e aguarda Sheva para troca P2P;
 *  - --client: descobre o lobby do host via UDP, conecta e troca mensagens P2P no canal 0x389.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

static uint32_t g_created_calls, g_enter_calls, g_chat_calls, g_data_calls, g_p2p_req_calls;
static uint64_t g_last_lobby_created;
static uint64_t g_last_p2p_req_user;

static void cb_created_run(CPU *c)
{
    g_created_calls++;
    uint32_t p = re5_arg(c, 0);
    CHECK_EQ_U32(rd32(p), 1); /* k_EResultOK */
    g_last_lobby_created = (uint64_t)rd32(p + 12) << 32 | rd32(p + 8);
}

static void cb_enter_run(CPU *c)
{
    g_enter_calls++;
    uint32_t p = re5_arg(c, 0);
    CHECK_EQ_U32(rd32(p + 16), 1); /* k_EChatRoomEnterResponseSuccess */
}

static void cb_chat_run(CPU *c)
{
    g_chat_calls++;
}

static void cb_data_run(CPU *c)
{
    g_data_calls++;
}

static void cb_p2p_req_run(CPU *c)
{
    g_p2p_req_calls++;
    uint32_t p = re5_arg(c, 0);
    g_last_p2p_req_user = (uint64_t)rd32(p + 4) << 32 | rd32(p);
}

static void cb_dummy(CPU *c) { (void)c; }

static int run_host_mode(void)
{
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);
    uint32_t match = flat("SteamMatchmaking", 0, NULL);
    uint32_t net = flat("SteamNetworking", 0, NULL);

    const uint32_t create_args[2] = {2, 2};
    vcall(match, 13, 2, create_args);
    flat("SteamAPI_RunCallbacks", 0, NULL);

    uint64_t my_id = steam_lan_get_local_steam_id();
    uint64_t lobby_id = ((uint64_t)0x01180000u << 32) | (uint32_t)my_id;

    uint32_t k = gstr("Chapter"), v = gstr("1-1");
    const uint32_t set_args[4] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), k, v};
    vcall(match, 20, 4, set_args);

    /* Espera o cliente conectar (até 5 segundos) */
    uint64_t client_id = 0;
    const uint32_t lobby_args[2] = {(uint32_t)lobby_id, (uint32_t)(lobby_id >> 32)};
    for (int t = 0; t < 500; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
        if (vcall(match, 17, 2, lobby_args) >= 2) {
            uint32_t sid_va = gbuf(8);
            const uint32_t get_mem_args[4] = {sid_va, (uint32_t)lobby_id, (uint32_t)(lobby_id >> 32), 1};
            vcall(match, 18, 4, get_mem_args);
            client_id = (uint64_t)rd32(sid_va + 4) << 32 | rd32(sid_va);
            break;
        }
    }
    CHECK(client_id != 0);

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

    /* Envia resposta P2P */
    const char resp[] = "HELLO_FROM_CHRIS";
    uint32_t send_buf = gbuf((uint32_t)strlen(resp) + 1);
    memcpy(re5_g2h(send_buf), resp, strlen(resp) + 1);
    const uint32_t s_args[6] = {(uint32_t)client_id, (uint32_t)(client_id >> 32), send_buf, (uint32_t)strlen(resp) + 1, 0, 0x389};
    CHECK_EQ_U32(vcall(net, 0, 6, s_args) & 0xFF, 1);

    SDL_Delay(50);
    vcall(match, 15, 2, lobby_args);
    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
    return 0;
}

static int run_client_mode(void)
{
    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);
    uint32_t match = flat("SteamMatchmaking", 0, NULL);
    uint32_t net = flat("SteamNetworking", 0, NULL);

    /* Descobre o lobby via broadcast */
    uint64_t target_lobby = 0;
    for (int t = 0; t < 500; t++) {
        vcall(match, 4, 0, NULL); /* RequestLobbyList */
        SDL_Delay(20);
        flat("SteamAPI_RunCallbacks", 0, NULL);
        if (steam_lan_get_lobby_count() > 0) {
            target_lobby = steam_lan_get_lobby_by_index(0);
            break;
        }
    }
    CHECK(target_lobby != 0);

    /* Verifica metadados */
    uint32_t k = gstr("Chapter");
    const uint32_t get_args[3] = {(uint32_t)target_lobby, (uint32_t)(target_lobby >> 32), k};
    uint32_t res = vcall(match, 19, 3, get_args);
    CHECK(strcmp(re5_gstr(res), "1-1") == 0);

    /* Entra no lobby */
    const uint32_t join_args[2] = {(uint32_t)target_lobby, (uint32_t)(target_lobby >> 32)};
    vcall(match, 14, 2, join_args);

    /* Aguarda confirmacao de entrada */
    int joined = 0;
    for (int t = 0; t < 500; t++) {
        SDL_Delay(10);
        flat("SteamAPI_RunCallbacks", 0, NULL);
        if (vcall(match, 17, 2, join_args) >= 2) {
            joined = 1;
            break;
        }
    }
    CHECK(joined);

    /* Envia mensagem P2P no canal 0x389 para o host */
    uint32_t host_sid_va = gbuf(8);
    const uint32_t get_owner_args[3] = {host_sid_va, (uint32_t)target_lobby, (uint32_t)(target_lobby >> 32)};
    vcall(match, 35, 3, get_owner_args);
    uint64_t host_id = (uint64_t)rd32(host_sid_va + 4) << 32 | rd32(host_sid_va);

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

    SDL_Delay(20);
    vcall(match, 15, 2, join_args);
    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x20000u, RE5_PAGE_READWRITE, "teste_lan");

    if (argc > 1 && strcmp(argv[1], "--host") == 0) return run_host_mode();
    if (argc > 1 && strcmp(argv[1], "--client") == 0) return run_client_mode();

    CHECK_EQ_U32(flat("SteamAPI_Init", 0, NULL) & 0xFF, 1);

    /* 1. Identidade */
    uint64_t my_id = steam_lan_get_local_steam_id();
    CHECK((my_id >> 32) == 0x01100001u);
    CHECK(steam_lan_get_p2p_port() >= 27016);
    CHECK(strlen(steam_lan_get_local_name()) > 0);

    uint32_t friends = flat("SteamFriends", 0, NULL);
    CHECK(friends != 0);
    uint32_t name_va = vcall(friends, 0, 0, NULL);
    CHECK(strcmp(re5_gstr(name_va), steam_lan_get_local_name()) == 0);

    /* 2. Registro de callbacks do Steamworks:
     *    LobbyCreated_t (513), LobbyEnter_t (504), LobbyChatUpdate_t (506), LobbyDataUpdate_t (505) */
    uint32_t vt_created = gbuf(12), cb_created = gbuf(16);
    wr32(vt_created, re5_thunk_new("teste!CreatedResult", cb_dummy, 16, NULL));
    wr32(vt_created + 4, re5_thunk_new("teste!CreatedRun", cb_created_run, 4, NULL));
    wr32(vt_created + 8, re5_thunk_new("teste!CreatedSize", cb_dummy, 0, NULL));
    wr32(cb_created, vt_created);
    const uint32_t reg_created[2] = {cb_created, 513};
    flat("SteamAPI_RegisterCallback", 2, reg_created);

    uint32_t vt_enter = gbuf(12), cb_enter = gbuf(16);
    wr32(vt_enter, re5_thunk_new("teste!EnterResult", cb_dummy, 16, NULL));
    wr32(vt_enter + 4, re5_thunk_new("teste!EnterRun", cb_enter_run, 4, NULL));
    wr32(vt_enter + 8, re5_thunk_new("teste!EnterSize", cb_dummy, 0, NULL));
    wr32(cb_enter, vt_enter);
    const uint32_t reg_enter[2] = {cb_enter, 504};
    flat("SteamAPI_RegisterCallback", 2, reg_enter);

    uint32_t vt_chat = gbuf(12), cb_chat = gbuf(16);
    wr32(vt_chat, re5_thunk_new("teste!ChatResult", cb_dummy, 16, NULL));
    wr32(vt_chat + 4, re5_thunk_new("teste!ChatRun", cb_chat_run, 4, NULL));
    wr32(vt_chat + 8, re5_thunk_new("teste!ChatSize", cb_dummy, 0, NULL));
    wr32(cb_chat, vt_chat);
    const uint32_t reg_chat[2] = {cb_chat, 506};
    flat("SteamAPI_RegisterCallback", 2, reg_chat);

    uint32_t vt_data = gbuf(12), cb_data = gbuf(16);
    wr32(vt_data, re5_thunk_new("teste!DataResult", cb_dummy, 16, NULL));
    wr32(vt_data + 4, re5_thunk_new("teste!DataRun", cb_data_run, 4, NULL));
    wr32(vt_data + 8, re5_thunk_new("teste!DataSize", cb_dummy, 0, NULL));
    wr32(cb_data, vt_data);
    const uint32_t reg_data[2] = {cb_data, 505};
    flat("SteamAPI_RegisterCallback", 2, reg_data);

    uint32_t vt_p2p = gbuf(12), cb_p2p = gbuf(16);
    wr32(vt_p2p, re5_thunk_new("teste!P2PReqResult", cb_dummy, 16, NULL));
    wr32(vt_p2p + 4, re5_thunk_new("teste!P2PReqRun", cb_p2p_req_run, 4, NULL));
    wr32(vt_p2p + 8, re5_thunk_new("teste!P2PReqSize", cb_dummy, 0, NULL));
    wr32(cb_p2p, vt_p2p);
    const uint32_t reg_p2p[2] = {cb_p2p, 1202};
    flat("SteamAPI_RegisterCallback", 2, reg_p2p);

    /* 3. Criação de Lobby via ISteamMatchmaking009 (slot 13) */
    uint32_t match = flat("SteamMatchmaking", 0, NULL);
    CHECK(match != 0);

    const uint32_t create_args[2] = {2 /* k_ELobbyTypePublic */, 2 /* max 2 players */};
    uint32_t call_id = vcall(match, 13, 2, create_args);
    CHECK(call_id != 0);

    /* Despacha os callbacks pendentes */
    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_created_calls, 1);
    CHECK_EQ_U32(g_enter_calls, 0); /* Host não recebe LobbyEnter_t na criação */
    CHECK(g_last_lobby_created != 0);
    uint64_t lobby_id = g_last_lobby_created;

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

    flat("SteamAPI_RunCallbacks", 0, NULL);
    CHECK_EQ_U32(g_data_calls, 1);

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

    const char test_payload[] = "RE5_LAN_COOP_PACKET_TEST";
    uint32_t payload_len = (uint32_t)strlen(test_payload) + 1;

    /* Envia pacote P2P para si mesmo (loopback) */
    uint32_t send_buf = gbuf(payload_len);
    memcpy(re5_g2h(send_buf), test_payload, payload_len);
    const uint32_t send_args[6] = {(uint32_t)my_id, (uint32_t)(my_id >> 32), send_buf, payload_len, 0, 0x389};
    (void)vcall(net, 0, 6, send_args);

    /* Aguarda até 100 ms para recepção loopback pelo worker */
    for (int t = 0; t < 20; t++) {
        SDL_Delay(5);
        steam_lan_poll();
        if (steam_lan_is_p2p_available(NULL, 0x389)) break;
    }

    /* Se o loopback UDP recebeu o pacote, lê com ReadP2PPacket */
    if (vcall(net, 1, 2, avail_args) & 0xFF) {
        CHECK_EQ_U32(rd32(sz_out), payload_len);

        uint32_t dest_buf = gbuf(1200);
        uint32_t read_sz = gbuf(4);
        uint32_t remote_sid = gbuf(8);
        const uint32_t read_args[5] = {dest_buf, 1200, read_sz, remote_sid, 0x389};

        CHECK_EQ_U32(vcall(net, 2, 5, read_args) & 0xFF, 1);
        CHECK_EQ_U32(rd32(read_sz), payload_len);
        CHECK(strcmp(re5_gstr(dest_buf), test_payload) == 0);

        flat("SteamAPI_RunCallbacks", 0, NULL);
        CHECK_EQ_U32(g_p2p_req_calls, 1);
        CHECK(g_last_p2p_req_user == my_id);
    }

    /* 8. Saída do Lobby */
    vcall(match, 15, 2, lobby_args);
    CHECK_EQ_U32(vcall(match, 17, 2, lobby_args), 0);

    /* 9. Desregistro e Shutdown */
    flat("SteamAPI_UnregisterCallback", 1, &cb_created);
    flat("SteamAPI_UnregisterCallback", 1, &cb_enter);
    flat("SteamAPI_UnregisterCallback", 1, &cb_chat);
    flat("SteamAPI_UnregisterCallback", 1, &cb_data);
    flat("SteamAPI_UnregisterCallback", 1, &cb_p2p);

    flat("SteamAPI_Shutdown", 0, NULL);
    re5_thread_detach();
    CHECK_DONE();
    return 0;
}
