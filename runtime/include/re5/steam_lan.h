/*
 * steam_lan.h - Subcamada de rede LAN para Steamworks (Matchmaking + Networking P2P).
 *
 * Suporta emulação de ISteamMatchmaking009 e ISteamNetworking005 sobre UDP:
 *  - Descoberta via UDP Broadcast em LAN (porta padrão 27015).
 *  - Transporte P2P de datagramas UDP direto entre pares (porta padrão 27016+).
 *  - Suporte completo a loopback local (duas instâncias na mesma máquina com
 *    portas diferentes), LAN PC x PC, PC x Android e Android x Android.
 */
#ifndef RE5_STEAM_LAN_H
#define RE5_STEAM_LAN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RE5_LAN_DEFAULT_DISCOVERY_PORT 27015
#define RE5_LAN_DEFAULT_P2P_PORT       27016
#define RE5_LAN_MAX_LOBBY_MEMBERS      2
#define RE5_LAN_MAX_LOBBIES            16
#define RE5_LAN_MAX_KEY_LEN            64
#define RE5_LAN_MAX_VAL_LEN            128
#define RE5_LAN_MAX_DATA_ENTRIES       32
#define RE5_LAN_MAX_PACKET_SIZE        1400
#define RE5_LAN_MAX_NAME_LEN           24   /* nome escolhido pelo jogador, sem o ID */
#define RE5_LAN_MAX_DISPLAY_LEN        40   /* "Nome #123456" */

/* Parte alta dos CSteamID: individual (universo público, tipo 1, instância 1) e
 * lobby (tipo 8 = chat, instância com k_EChatInstanceFlagLobby|MMSLobby). */
#define RE5_LAN_STEAMID_USER_HI        0x01100001u
#define RE5_LAN_STEAMID_LOBBY_HI       0x01860000u

/* Faixa do ID de jogador (account id do CSteamID): seis dígitos, fácil de ler
 * na tela e de conferir entre dois aparelhos. */
#define RE5_LAN_ID_MIN                 100000u
#define RE5_LAN_ID_SPAN                900000u

/* Tipos de eventos postados para o steam_api despachar em RunCallbacks */
typedef enum {
    RE5_LAN_EVT_NONE = 0,
    RE5_LAN_EVT_LOBBY_CREATED,
    RE5_LAN_EVT_LOBBY_ENTERED,
    RE5_LAN_EVT_LOBBY_MATCH_LIST,
    RE5_LAN_EVT_LOBBY_CHAT_UPDATE,
    RE5_LAN_EVT_LOBBY_DATA_UPDATE,
    RE5_LAN_EVT_P2P_SESSION_REQUEST,
} re5_lan_event_type;

typedef struct {
    re5_lan_event_type type;
    uint64_t           call_id;
    uint64_t           lobby_id;
    uint64_t           user_id;
    uint32_t           param1;
    uint32_t           param2;
} re5_lan_event;

/* Inicialização e ciclo de vida */
int  steam_lan_init(void);
void steam_lan_shutdown(void);
void steam_lan_poll(void);

/* Identidade da instância.
 * O ID (account id do CSteamID) vem de RE5_STEAM_ID ou do arquivo de identidade
 * (RE5_LAN_CONFIG, ou C:\\Users\\Player\\re5_lan.ini no VFS), gerado aleatório e
 * gravado na primeira execução; uma 2ª instância no mesmo aparelho soma 1.
 * O nome vem de RE5_PLAYER_NAME ou da chave "nome" do mesmo arquivo. */
uint64_t    steam_lan_get_local_steam_id(void);
const char *steam_lan_get_local_name(void);
uint16_t    steam_lan_get_p2p_port(void);

/* Nome como os jogadores o veem: "Nome #ID". Para um par ainda desconhecido,
 * "Jogador #ID". Copia para `out` (a tabela de pares muda na thread de rede). */
void        steam_lan_get_display_name(uint64_t steam_id, char *out, size_t cap);

/* ISteamMatchmaking009 */
uint64_t    steam_lan_create_lobby(uint32_t lobby_type, int max_members, uint64_t call_id);
uint64_t    steam_lan_request_lobby_list(uint64_t call_id);
int         steam_lan_get_lobby_count(void);
uint64_t    steam_lan_get_lobby_by_index(int idx);
int         steam_lan_get_num_lobby_members(uint64_t lobby_id);
uint64_t    steam_lan_get_lobby_member_by_index(uint64_t lobby_id, int idx);
uint64_t    steam_lan_get_lobby_owner(uint64_t lobby_id);
const char *steam_lan_get_lobby_data(uint64_t lobby_id, const char *key);
int         steam_lan_set_lobby_data(uint64_t lobby_id, const char *key, const char *val);
const char *steam_lan_get_lobby_member_data(uint64_t lobby_id, uint64_t member_id, const char *key);
void        steam_lan_set_lobby_member_data(uint64_t lobby_id, const char *key, const char *val);
int         steam_lan_join_lobby(uint64_t lobby_id, uint64_t call_id);
void        steam_lan_leave_lobby(uint64_t lobby_id);
void        steam_lan_set_lobby_joinable(uint64_t lobby_id, int joinable);
int         steam_lan_get_lobby_member_limit(uint64_t lobby_id);

/* ISteamNetworking005 */
int         steam_lan_send_p2p(uint64_t remote_id, const void *data, uint32_t len, int send_type, int channel);
int         steam_lan_is_p2p_available(uint32_t *msg_size, int channel);
int         steam_lan_read_p2p(void *dest, uint32_t dest_len, uint32_t *msg_size, uint64_t *remote_id, int channel);
int         steam_lan_accept_p2p(uint64_t remote_id);
int         steam_lan_close_p2p(uint64_t remote_id);
int         steam_lan_get_p2p_state(uint64_t remote_id, void *out_state);

/* Consumo de eventos na thread principal para disparo de callbacks */
int         steam_lan_pop_event(re5_lan_event *evt);

#ifdef __cplusplus
}
#endif

#endif /* RE5_STEAM_LAN_H */
