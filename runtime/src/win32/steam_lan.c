/*
 * steam_lan.c - Subcamada de rede LAN para Steamworks (Matchmaking + Networking P2P).
 *
 * Implementa transporte UDP em broadcast e unicast direto entre nós LAN.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
typedef SOCKET hsock_t;
#define HBAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
typedef int hsock_t;
#define HBAD_SOCK (-1)
#endif

#include "re5/runtime.h"
#include "re5/steam_lan.h"
#include "re5/thread.h"
#include "re5/vfs.h"
#include "../core/host_sync.h"

#define RE5_LAN_MAGIC 0x35455221u /* "!RE5" */

enum {
    PKT_BEACON           = 0x01,
    PKT_PING             = 0x02,
    PKT_JOIN_REQ         = 0x03,
    PKT_JOIN_RESP        = 0x04,
    PKT_LEAVE            = 0x05,
    PKT_DATA_UPDATE      = 0x06,
    PKT_PROFILE          = 0x07,   /* nome/país mudaram no meio da partida */
    PKT_KEEPALIVE        = 0x08,   /* entre membros da sala: queda do parceiro e ping */
    PKT_P2P_DATA         = 0x10,
    PKT_P2P_ACK          = 0x11,
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;
    uint16_t port;
    uint64_t sender_steam_id;
    uint32_t nonce;            /* aleatório por processo: separa o eco do nosso broadcast de outro jogador com o mesmo ID */
} lan_hdr;

typedef struct {
    uint64_t lobby_id;
    uint64_t owner_id;
    uint16_t member_count;
    uint16_t max_members;
    char     owner_name[32];
    uint16_t data_count;
    char     owner_country[4];
} lan_beacon_body;

typedef struct {
    uint64_t lobby_id;
    char     client_name[32];
    char     client_country[4];
} lan_join_req_body;

typedef struct {
    char name[32];
    char country[4];
} lan_profile_body;

/* Ping sem relógio comum: cada um ecoa o último t_send que recebeu e há quanto
 * tempo o segura; quem recebe o eco tira RTT = agora - echo - hold. */
typedef struct {
    uint64_t lobby_id;
    uint32_t t_send;
    uint32_t echo;
    uint32_t echo_hold;
} lan_keepalive_body;

typedef struct {
    uint64_t lobby_id;
    uint32_t response_code;
    uint16_t member_count;
} lan_join_resp_body;

typedef struct {
    uint64_t lobby_id;
    uint64_t member_id; /* 0 se for do lobby */
    char     key[RE5_LAN_MAX_KEY_LEN];
    char     val[RE5_LAN_MAX_VAL_LEN];
} lan_data_update_body;

typedef struct {
    uint16_t channel;
    uint8_t  send_type;
    uint8_t  _pad;
    uint32_t seq;
    uint32_t payload_len;
} lan_p2p_hdr;
#pragma pack(pop)

/* Estruturas internas */

typedef struct {
    char key[RE5_LAN_MAX_KEY_LEN];
    char val[RE5_LAN_MAX_VAL_LEN];
} lan_kv;

typedef struct {
    uint64_t           steam_id;
    char               name[32];
    char               country[4];
    struct sockaddr_in addr;
    uint32_t           last_seen_ms;
    int                p2p_session_requested;
    int32_t            rtt_ms;          /* -1: ainda sem medida */
    uint32_t           ka_their_t;      /* último t_send recebido dele (0: nenhum) */
    uint32_t           ka_their_at;     /* quando chegou, no nosso relógio */
} lan_peer;

typedef struct {
    uint64_t           lobby_id;
    uint64_t           owner_id;
    char               owner_name[32];
    uint16_t           host_port;
    struct in_addr     host_ip;
    uint16_t           member_count;
    uint16_t           max_members;
    uint64_t           members[RE5_LAN_MAX_LOBBY_MEMBERS];
    char               member_names[RE5_LAN_MAX_LOBBY_MEMBERS][32];
    lan_kv             data[RE5_LAN_MAX_DATA_ENTRIES];
    int                data_count;
    lan_kv             member_data[RE5_LAN_MAX_LOBBY_MEMBERS][RE5_LAN_MAX_DATA_ENTRIES];
    int                member_data_count[RE5_LAN_MAX_LOBBY_MEMBERS];
    uint32_t           last_seen_ms;
    int                is_local;
    int                joinable;
} lan_lobby;

typedef struct lan_p2p_msg {
    uint64_t            remote_id;
    uint16_t            channel;
    uint32_t            len;
    uint8_t             data[RE5_LAN_MAX_PACKET_SIZE];
    struct lan_p2p_msg *next;
} lan_p2p_msg;

#define MAX_PEERS 16
#define MAX_EVENTS 64
#define MAX_IFACES 8
#define MAX_MANUAL_HOSTS 4

/* Celular: Wi-Fi que cai, app em segundo plano. Sem notícia de um membro da sala
 * por este tempo, ele "desconectou" (o jogo deixa de esperar por ele). */
#define KEEPALIVE_MS      1000u
#define MEMBER_TIMEOUT_MS 15000u   /* RE5_LAN_TIMEOUT_MS muda (Wi-Fi ruim: mais; testes: menos) */
#define IFACE_REFRESH_MS  5000u

/* EChatRoomEnterResponse */
#define ENTER_SUCCESS      1u
#define ENTER_DOESNT_EXIST 2u
#define ENTER_NOT_ALLOWED  3u
#define ENTER_FULL         4u
#define ENTER_ERROR        5u

/* O pedido de entrada é UDP: repete até a resposta do host, desiste no prazo. */
#define JOIN_RESEND_MS  300u
#define JOIN_TIMEOUT_MS 5000u

/* EChatMemberStateChange */
#define MEMBER_ENTERED      0x01u
#define MEMBER_LEFT         0x02u
#define MEMBER_DISCONNECTED 0x04u

#define P2P_ERROR_TIMEOUT 4u   /* k_EP2PSessionErrorTimeout */

static re5_mutex g_mu;
static int       g_initialized;
static volatile int g_running;

static uint64_t g_local_steam_id;
static char     g_local_name[32];
static uint16_t g_discovery_port = RE5_LAN_DEFAULT_DISCOVERY_PORT;
static uint16_t g_p2p_port = RE5_LAN_DEFAULT_P2P_PORT;

static hsock_t  g_disc_sock = HBAD_SOCK;
static hsock_t  g_p2p_sock  = HBAD_SOCK;

static lan_lobby g_lobbies[RE5_LAN_MAX_LOBBIES];
static int       g_num_lobbies;
static lan_lobby g_active_lobby;
static int       g_has_active_lobby;

static lan_peer  g_peers[MAX_PEERS];
static int       g_num_peers;

static lan_p2p_msg *g_p2p_queue;

static re5_lan_event g_events[MAX_EVENTS];
static int           g_evt_head, g_evt_tail, g_evt_count;

static uint64_t g_pending_search_call;
static uint32_t g_pending_search_expire;

static uint32_t g_nonce;
static int      g_id_clash_warned;

static uint32_t g_member_timeout_ms = MEMBER_TIMEOUT_MS;
static uint32_t g_file_id;                  /* ID gravado no arquivo (sem o deslocamento da instância) */
static char     g_local_country[4] = "US";
static char     g_identity_path[1024];

/* Endereços de broadcast de cada interface (Wi-Fi, hotspot do celular...): no
 * Android o 255.255.255.255 muitas vezes sai pela interface errada ou nem sai. */
static struct in_addr g_iface_bcast[MAX_IFACES];
static int            g_n_iface;
static char           g_local_ip[16] = "?";
static uint32_t       g_iface_at;

/* Hosts digitados pelo jogador (rede sem broadcast): recebem o PING direto. */
static struct in_addr g_manual_hosts[MAX_MANUAL_HOSTS];
static int            g_n_manual;

/* Entrada em andamento (lado do cliente). */
static struct {
    int                active;
    uint64_t           lobby_id;
    uint64_t           call_id;
    struct sockaddr_in host;
    uint32_t           next_send_ms;
    uint32_t           deadline_ms;
} g_join;

static uint32_t get_ticks_ms(void)
{
#if defined(_WIN32)
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void sock_close(hsock_t s)
{
    if (s == HBAD_SOCK) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

static int sock_set_nonblocking(hsock_t s)
{
#if defined(_WIN32)
    u_long v = 1;
    return ioctlsocket(s, FIONBIO, &v);
#else
    int f = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, f | O_NONBLOCK);
#endif
}

static int sock_set_reuse(hsock_t s)
{
    int on = 1;
    int r = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#if defined(SO_REUSEPORT)
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof on);
#endif
    return r;
}

static int sock_set_broadcast(hsock_t s)
{
    int on = 1;
    return setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
}

static void sock_fix_connreset(hsock_t s)
{
#if defined(_WIN32)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
#else
    (void)s;
#endif
}

static void sanitize_name(const char *in, char *out, size_t cap);
static lan_peer *peer_find(uint64_t steam_id);

static void fill_hdr(lan_hdr *h, uint8_t type)
{
    h->magic = RE5_LAN_MAGIC;
    h->type = type;
    h->flags = 0;
    h->port = g_p2p_port;
    h->sender_steam_id = g_local_steam_id;
    h->nonce = g_nonce;
}

static void push_event(re5_lan_event_type type, uint64_t call_id, uint64_t lobby_id, uint64_t user_id, uint32_t p1, uint32_t p2)
{
    if (g_evt_count >= MAX_EVENTS) return;
    g_events[g_evt_tail] = (re5_lan_event){type, call_id, lobby_id, user_id, p1, p2};
    g_evt_tail = (g_evt_tail + 1) % MAX_EVENTS;
    g_evt_count++;
}

int steam_lan_pop_event(re5_lan_event *evt)
{
    re5_mutex_lock(&g_mu);
    if (g_evt_count == 0) {
        re5_mutex_unlock(&g_mu);
        return 0;
    }
    *evt = g_events[g_evt_head];
    g_evt_head = (g_evt_head + 1) % MAX_EVENTS;
    g_evt_count--;
    re5_mutex_unlock(&g_mu);
    return 1;
}

static void peer_update(uint64_t steam_id, const char *name, const struct sockaddr_in *addr)
{
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i].steam_id == steam_id) {
            g_peers[i].addr = *addr;
            g_peers[i].last_seen_ms = get_ticks_ms();
            if (name && *name) snprintf(g_peers[i].name, sizeof g_peers[i].name, "%s", name);
            return;
        }
    }
    if (g_num_peers < MAX_PEERS) {
        lan_peer *p = &g_peers[g_num_peers++];
        p->steam_id = steam_id;
        p->addr = *addr;
        p->last_seen_ms = get_ticks_ms();
        if (name && *name) snprintf(p->name, sizeof p->name, "%s", name);
        else p->name[0] = 0;
        p->country[0] = 0;
        p->p2p_session_requested = 0;
        p->rtt_ms = -1;
        p->ka_their_t = 0;
    }
}

static void peer_set_country(uint64_t steam_id, const char *cc)
{
    lan_peer *p = peer_find(steam_id);
    if (!p || !cc || !cc[0]) return;
    snprintf(p->country, sizeof p->country, "%.2s", cc);
}

static lan_peer *peer_find(uint64_t steam_id)
{
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i].steam_id == steam_id) return &g_peers[i];
    }
    return NULL;
}

static void send_raw_udp(hsock_t sock, const struct sockaddr_in *dest, const void *data, size_t len)
{
    sendto(sock, (const char *)data, (int)len, 0, (const struct sockaddr *)dest, sizeof *dest);
}

static void send_broadcast(const void *data, size_t len)
{
    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof bcast);
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(g_discovery_port);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_raw_udp(g_disc_sock, &bcast, data, len);

    /* Envia também explicitamente em 127.0.0.1:27015 para recepção loopback */
    bcast.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    send_raw_udp(g_disc_sock, &bcast, data, len);

    /* Broadcast dirigido de cada interface (192.168.43.255 no hotspot etc.). */
    for (int i = 0; i < g_n_iface; i++) {
        bcast.sin_addr = g_iface_bcast[i];
        send_raw_udp(g_disc_sock, &bcast, data, len);
    }
    /* Hosts digitados: direto na descoberta e nas primeiras portas P2P. */
    for (int i = 0; i < g_n_manual; i++) {
        bcast.sin_addr = g_manual_hosts[i];
        bcast.sin_port = htons(g_discovery_port);
        send_raw_udp(g_p2p_sock, &bcast, data, len);
        for (uint16_t p = RE5_LAN_DEFAULT_P2P_PORT; p < RE5_LAN_DEFAULT_P2P_PORT + 4; p++) {
            bcast.sin_port = htons(p);
            send_raw_udp(g_p2p_sock, &bcast, data, len);
        }
    }
    bcast.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Envia também para as portas P2P locais em loopback (instâncias no mesmo PC) */
    for (uint16_t p = RE5_LAN_DEFAULT_P2P_PORT; p < RE5_LAN_DEFAULT_P2P_PORT + 16; p++) {
        if (p == g_p2p_port) continue;
        bcast.sin_port = htons(p);
        send_raw_udp(g_p2p_sock, &bcast, data, len);
    }
    const char *env_p2p = getenv("RE5_LAN_PORT");
    if (env_p2p && atoi(env_p2p) > 0) {
        uint16_t custom_p2p = (uint16_t)atoi(env_p2p);
        for (uint16_t p = custom_p2p; p < custom_p2p + 16; p++) {
            if (p == g_p2p_port) continue;
            bcast.sin_port = htons(p);
            send_raw_udp(g_p2p_sock, &bcast, data, len);
        }
    }
}

static size_t build_beacon_packet(uint8_t *buf, size_t max_len)
{
    if (!g_has_active_lobby || !g_active_lobby.is_local) return 0;
    if (g_active_lobby.owner_id != g_local_steam_id) return 0;
    if (max_len < sizeof(lan_hdr) + sizeof(lan_beacon_body)) return 0;

    lan_hdr *h = (lan_hdr *)buf;
    fill_hdr(h, PKT_BEACON);

    lan_beacon_body *b = (lan_beacon_body *)(buf + sizeof(lan_hdr));
    b->lobby_id = g_active_lobby.lobby_id;
    b->owner_id = g_active_lobby.owner_id;
    b->member_count = g_active_lobby.member_count;
    b->max_members = g_active_lobby.max_members;
    snprintf(b->owner_name, sizeof b->owner_name, "%s", g_active_lobby.owner_name);
    b->data_count = (uint16_t)g_active_lobby.data_count;
    memset(b->owner_country, 0, sizeof b->owner_country);
    memcpy(b->owner_country, g_local_country, 2);

    size_t off = sizeof(lan_hdr) + sizeof(lan_beacon_body);
    for (int i = 0; i < g_active_lobby.data_count && off + sizeof(lan_kv) <= max_len; i++) {
        memcpy(buf + off, &g_active_lobby.data[i], sizeof(lan_kv));
        off += sizeof(lan_kv);
    }
    return off;
}

static void broadcast_beacon(void)
{
    uint8_t buf[RE5_LAN_MAX_PACKET_SIZE];
    size_t len = build_beacon_packet(buf, sizeof buf);
    if (len > 0) send_broadcast(buf, len);
}

static void handle_beacon(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    if (len < sizeof(lan_beacon_body)) return;
    const lan_beacon_body *b = (const lan_beacon_body *)payload;

    struct sockaddr_in peer_p2p = *from;
    peer_p2p.sin_port = htons(h->port);
    char owner_name[RE5_LAN_MAX_NAME_LEN + 1], raw[sizeof b->owner_name + 1];
    memcpy(raw, b->owner_name, sizeof b->owner_name);
    raw[sizeof b->owner_name] = 0;
    sanitize_name(raw, owner_name, sizeof owner_name);
    peer_update(h->sender_steam_id, owner_name, &peer_p2p);
    char cc[3] = {b->owner_country[0], b->owner_country[1], 0};
    peer_set_country(h->sender_steam_id, cc);

    if (b->owner_id == g_local_steam_id) return; /* ignorar nosso próprio beacon */

    lan_lobby *lb = NULL;
    for (int i = 0; i < g_num_lobbies; i++) {
        if (g_lobbies[i].lobby_id == b->lobby_id) {
            lb = &g_lobbies[i];
            break;
        }
    }
    if (!lb) {
        if (g_num_lobbies < RE5_LAN_MAX_LOBBIES) lb = &g_lobbies[g_num_lobbies++];
        else return;
    }

    lb->lobby_id = b->lobby_id;
    lb->owner_id = b->owner_id;
    snprintf(lb->owner_name, sizeof lb->owner_name, "%s", owner_name);
    lb->host_port = h->port;
    lb->host_ip = from->sin_addr;
    lb->member_count = b->member_count;
    lb->max_members = b->max_members;
    lb->members[0] = b->owner_id;
    snprintf(lb->member_names[0], sizeof lb->member_names[0], "%s", owner_name);
    lb->last_seen_ms = get_ticks_ms();
    lb->is_local = 0;
    lb->joinable = 1;

    /* Extrai chaves e valores do lobby */
    size_t off = sizeof(lan_beacon_body);
    lb->data_count = 0;
    for (int i = 0; i < (int)b->data_count && off + sizeof(lan_kv) <= len; i++) {
        if (lb->data_count < RE5_LAN_MAX_DATA_ENTRIES) {
            memcpy(&lb->data[lb->data_count++], payload + off, sizeof(lan_kv));
        }
        off += sizeof(lan_kv);
    }
    RE5_INFO("steam_lan", "Lobby detectado na LAN: ID %llu, Dono '%s' (membros %u/%u)",
             (unsigned long long)b->lobby_id, owner_name, b->member_count, b->max_members);
}

static void handle_ping(const lan_hdr *h, const struct sockaddr_in *from)
{
    if (g_has_active_lobby && g_active_lobby.owner_id == g_local_steam_id) {
        broadcast_beacon();
        uint8_t buf[RE5_LAN_MAX_PACKET_SIZE];
        size_t len = build_beacon_packet(buf, sizeof buf);
        if (len > 0) {
            send_raw_udp(g_p2p_sock, from, buf, len);
            if (h->port) {
                struct sockaddr_in to_p2p = *from;
                to_p2p.sin_port = htons(h->port);
                send_raw_udp(g_p2p_sock, &to_p2p, buf, len);
            }
        }
    }
}

static int member_index(const lan_lobby *lb, uint64_t steam_id)
{
    for (int i = 0; i < lb->member_count && i < RE5_LAN_MAX_LOBBY_MEMBERS; i++)
        if (lb->members[i] == steam_id) return i;
    return -1;
}

static void send_data_update(const struct sockaddr_in *dest, uint64_t lobby_id, uint64_t member_id, const lan_kv *kv)
{
    uint8_t buf[sizeof(lan_hdr) + sizeof(lan_data_update_body)];
    fill_hdr((lan_hdr *)buf, PKT_DATA_UPDATE);
    lan_data_update_body *du = (lan_data_update_body *)(buf + sizeof(lan_hdr));
    memset(du, 0, sizeof *du);
    du->lobby_id = lobby_id;
    du->member_id = member_id;
    snprintf(du->key, sizeof du->key, "%s", kv->key);
    snprintf(du->val, sizeof du->val, "%s", kv->val);
    send_raw_udp(g_p2p_sock, dest, buf, sizeof buf);
}

static void send_join_req(void)
{
    uint8_t buf[sizeof(lan_hdr) + sizeof(lan_join_req_body)];
    fill_hdr((lan_hdr *)buf, PKT_JOIN_REQ);
    lan_join_req_body *req = (lan_join_req_body *)(buf + sizeof(lan_hdr));
    memset(req, 0, sizeof *req);
    req->lobby_id = g_join.lobby_id;
    snprintf(req->client_name, sizeof req->client_name, "%s", g_local_name);
    memcpy(req->client_country, g_local_country, 2);
    send_raw_udp(g_p2p_sock, &g_join.host, buf, sizeof buf);
}

/* Host: alguém pediu para entrar. O pedido pode chegar repetido (o cliente
 * reenvia até a resposta), então quem já é membro só recebe a confirmação de novo. */
static void handle_join_req(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    if (len < sizeof(lan_join_req_body)) return;
    const lan_join_req_body *req = (const lan_join_req_body *)payload;
    char raw[sizeof req->client_name + 1], name[RE5_LAN_MAX_NAME_LEN + 1];
    memcpy(raw, req->client_name, sizeof req->client_name);
    raw[sizeof req->client_name] = 0;
    sanitize_name(raw, name, sizeof name);

    struct sockaddr_in client_addr = *from;
    client_addr.sin_port = htons(h->port);
    peer_update(h->sender_steam_id, name, &client_addr);
    char cc[3] = {req->client_country[0], req->client_country[1], 0};
    peer_set_country(h->sender_steam_id, cc);

    uint32_t resp_code = ENTER_SUCCESS;
    int is_new = 0;
    if (!g_has_active_lobby || g_active_lobby.lobby_id != req->lobby_id || g_active_lobby.owner_id != g_local_steam_id) {
        resp_code = ENTER_DOESNT_EXIST;
    } else if (member_index(&g_active_lobby, h->sender_steam_id) >= 0) {
        resp_code = ENTER_SUCCESS;
    } else if (!g_active_lobby.joinable) {
        resp_code = ENTER_NOT_ALLOWED;
    } else if (g_active_lobby.member_count >= g_active_lobby.max_members) {
        resp_code = ENTER_FULL;
    } else {
        int idx = g_active_lobby.member_count++;
        g_active_lobby.members[idx] = h->sender_steam_id;
        snprintf(g_active_lobby.member_names[idx], sizeof g_active_lobby.member_names[idx], "%s", name);
        g_active_lobby.member_data_count[idx] = 0;
        is_new = 1;
    }

    uint8_t resp_buf[sizeof(lan_hdr) + sizeof(lan_join_resp_body)];
    fill_hdr((lan_hdr *)resp_buf, PKT_JOIN_RESP);
    lan_join_resp_body *rb = (lan_join_resp_body *)(resp_buf + sizeof(lan_hdr));
    rb->lobby_id = req->lobby_id;
    rb->response_code = resp_code;
    rb->member_count = g_has_active_lobby ? g_active_lobby.member_count : 0;
    send_raw_udp(g_p2p_sock, &client_addr, resp_buf, sizeof resp_buf);

    if (resp_code != ENTER_SUCCESS) {
        RE5_INFO("steam_lan", "Pedido de entrada de '%s #%u' recusado (codigo %u)", name, (uint32_t)h->sender_steam_id,
                 resp_code);
        return;
    }

    /* O cliente já tem os dados da sala (vieram no beacon), mas não os do host como
     * membro (SetLobbyMemberData feito antes de ele chegar): manda tudo de novo. */
    for (int i = 0; i < g_active_lobby.data_count; i++)
        send_data_update(&client_addr, g_active_lobby.lobby_id, 0, &g_active_lobby.data[i]);
    for (int k = 0; k < g_active_lobby.member_data_count[0]; k++)
        send_data_update(&client_addr, g_active_lobby.lobby_id, g_local_steam_id, &g_active_lobby.member_data[0][k]);

    if (!is_new) return;

    /* É isto que avisa o jogo do host: LobbyChatUpdate_t (membro entrou), como a
     * Steam faz para todos os que já estão na sala; e o pedido de sessão P2P. */
    push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, g_active_lobby.lobby_id, h->sender_steam_id, MEMBER_ENTERED, 0);
    lan_peer *p = peer_find(h->sender_steam_id);
    if (p && !p->p2p_session_requested) {
        p->p2p_session_requested = 1;
        push_event(RE5_LAN_EVT_P2P_SESSION_REQUEST, 0, 0, h->sender_steam_id, 0, 0);
    }
    RE5_INFO("steam_lan", "Jogador '%s #%u' entrou no lobby (%u/%u)", name, (uint32_t)h->sender_steam_id,
             g_active_lobby.member_count, g_active_lobby.max_members);
}

static void handle_join_resp(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    (void)from;
    if (len < sizeof(lan_join_resp_body)) return;
    const lan_join_resp_body *resp = (const lan_join_resp_body *)payload;

    /* Respostas repetidas (um por pedido reenviado) depois da primeira: ignora. */
    if (!g_join.active || g_join.lobby_id != resp->lobby_id || h->sender_steam_id != g_active_lobby.owner_id) return;
    g_join.active = 0;

    if (resp->response_code == ENTER_SUCCESS) {
        g_active_lobby.member_count = resp->member_count > 1 ? resp->member_count : 2;
        if (g_active_lobby.member_count > RE5_LAN_MAX_LOBBY_MEMBERS) g_active_lobby.member_count = RE5_LAN_MAX_LOBBY_MEMBERS;
        int me = g_active_lobby.member_count - 1;
        g_active_lobby.members[me] = g_local_steam_id;
        snprintf(g_active_lobby.member_names[me], sizeof g_active_lobby.member_names[me], "%s", g_local_name);
        g_active_lobby.member_data_count[me] = 0;

        /* Quem entra recebe só LobbyEnter_t (a Steam não manda LobbyChatUpdate_t de si mesmo). */
        push_event(RE5_LAN_EVT_LOBBY_ENTERED, g_join.call_id, g_active_lobby.lobby_id, g_local_steam_id, ENTER_SUCCESS, 0);
        lan_peer *p = peer_find(h->sender_steam_id);
        if (p && !p->p2p_session_requested) {
            p->p2p_session_requested = 1;
            push_event(RE5_LAN_EVT_P2P_SESSION_REQUEST, 0, 0, h->sender_steam_id, 0, 0);
        }
        RE5_INFO("steam_lan", "Conectado ao lobby de '%s #%u'", g_active_lobby.owner_name,
                 (uint32_t)g_active_lobby.owner_id);
    } else {
        push_event(RE5_LAN_EVT_LOBBY_ENTERED, g_join.call_id, g_active_lobby.lobby_id, g_local_steam_id,
                   resp->response_code, 0);
        g_has_active_lobby = 0;
        RE5_INFO("steam_lan", "Host recusou a entrada no lobby: codigo %u", resp->response_code);
    }
}

/* Cliente: reenvia o pedido de entrada até a resposta; sem resposta no prazo,
 * LobbyEnter_t com erro (senão o jogo esperaria para sempre). */
static void join_tick(uint32_t now)
{
    if (!g_join.active) return;
    if ((int32_t)(now - g_join.deadline_ms) >= 0) {
        g_join.active = 0;
        g_has_active_lobby = 0;
        push_event(RE5_LAN_EVT_LOBBY_ENTERED, g_join.call_id, g_join.lobby_id, g_local_steam_id, ENTER_ERROR, 0);
        RE5_WARN("steam_lan", "O host %s:%u nao respondeu ao pedido de entrada em %u ms "
                 "(firewall bloqueando UDP de entrada no host?)",
                 inet_ntoa(g_join.host.sin_addr), ntohs(g_join.host.sin_port), JOIN_TIMEOUT_MS);
        return;
    }
    if ((int32_t)(now - g_join.next_send_ms) >= 0) {
        send_join_req();
        g_join.next_send_ms = now + JOIN_RESEND_MS;
    }
}

static void handle_leave(const lan_hdr *h, const uint8_t *payload, size_t len)
{
    if (len < sizeof(uint64_t)) return;
    uint64_t lobby_id = *(const uint64_t *)payload;

    if (!g_has_active_lobby || g_active_lobby.lobby_id != lobby_id) return;

    if (g_active_lobby.owner_id == g_local_steam_id) {
        /* Membro saiu */
        for (int i = 1; i < g_active_lobby.member_count && i < RE5_LAN_MAX_LOBBY_MEMBERS; i++) {
            if (g_active_lobby.members[i] == h->sender_steam_id) {
                for (int k = i; k + 1 < g_active_lobby.member_count && k + 1 < RE5_LAN_MAX_LOBBY_MEMBERS; k++) {
                    g_active_lobby.members[k] = g_active_lobby.members[k + 1];
                    memcpy(g_active_lobby.member_names[k], g_active_lobby.member_names[k + 1], sizeof g_active_lobby.member_names[k]);
                    memcpy(g_active_lobby.member_data[k], g_active_lobby.member_data[k + 1], sizeof g_active_lobby.member_data[k]);
                    g_active_lobby.member_data_count[k] = g_active_lobby.member_data_count[k + 1];
                }
                g_active_lobby.member_count--;
                push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, lobby_id, h->sender_steam_id, MEMBER_LEFT, 0);
                break;
            }
        }
    } else if (h->sender_steam_id == g_active_lobby.owner_id) {
        /* Host fechou o lobby */
        push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, lobby_id, h->sender_steam_id, MEMBER_LEFT, 0);
        g_has_active_lobby = 0;
    }
}

static void handle_data_update(const lan_hdr *h, const uint8_t *payload, size_t len)
{
    (void)h;
    if (len < sizeof(lan_data_update_body)) return;
    const lan_data_update_body *du = (const lan_data_update_body *)payload;

    if (!g_has_active_lobby || g_active_lobby.lobby_id != du->lobby_id) return;

    if (du->member_id == 0) {
        /* Lobby data */
        int found = 0;
        for (int i = 0; i < g_active_lobby.data_count; i++) {
            if (strcmp(g_active_lobby.data[i].key, du->key) == 0) {
                snprintf(g_active_lobby.data[i].val, sizeof g_active_lobby.data[i].val, "%s", du->val);
                found = 1;
                break;
            }
        }
        if (!found && g_active_lobby.data_count < RE5_LAN_MAX_DATA_ENTRIES) {
            int idx = g_active_lobby.data_count++;
            snprintf(g_active_lobby.data[idx].key, sizeof g_active_lobby.data[idx].key, "%s", du->key);
            snprintf(g_active_lobby.data[idx].val, sizeof g_active_lobby.data[idx].val, "%s", du->val);
        }
        push_event(RE5_LAN_EVT_LOBBY_DATA_UPDATE, 0, du->lobby_id, du->lobby_id, 1, 0);
    } else {
        /* Member data */
        int midx = -1;
        for (int i = 0; i < g_active_lobby.member_count; i++) {
            if (g_active_lobby.members[i] == du->member_id) {
                midx = i;
                break;
            }
        }
        if (midx >= 0) {
            int found = 0;
            for (int k = 0; k < g_active_lobby.member_data_count[midx]; k++) {
                if (strcmp(g_active_lobby.member_data[midx][k].key, du->key) == 0) {
                    snprintf(g_active_lobby.member_data[midx][k].val, sizeof g_active_lobby.member_data[midx][k].val, "%s", du->val);
                    found = 1;
                    break;
                }
            }
            if (!found && g_active_lobby.member_data_count[midx] < RE5_LAN_MAX_DATA_ENTRIES) {
                int idx = g_active_lobby.member_data_count[midx]++;
                snprintf(g_active_lobby.member_data[midx][idx].key, sizeof g_active_lobby.member_data[midx][idx].key, "%s", du->key);
                snprintf(g_active_lobby.member_data[midx][idx].val, sizeof g_active_lobby.member_data[midx][idx].val, "%s", du->val);
            }
            push_event(RE5_LAN_EVT_LOBBY_DATA_UPDATE, 0, du->lobby_id, du->member_id, 1, 0);
        }
    }
}

static void handle_p2p_data(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    peer_update(h->sender_steam_id, NULL, from);
    lan_peer *p = peer_find(h->sender_steam_id);
    if (p && !p->p2p_session_requested) {
        p->p2p_session_requested = 1;
        push_event(RE5_LAN_EVT_P2P_SESSION_REQUEST, 0, 0, h->sender_steam_id, 0, 0);
    }

    if (len < sizeof(lan_p2p_hdr)) return;

    const lan_p2p_hdr *ph = (const lan_p2p_hdr *)payload;
    if (len < sizeof(lan_p2p_hdr) + ph->payload_len) return;
    if (ph->payload_len > RE5_LAN_MAX_PACKET_SIZE) return;

    lan_p2p_msg *msg = malloc(sizeof(lan_p2p_msg));
    if (!msg) return;
    msg->remote_id = h->sender_steam_id;
    msg->channel = ph->channel;
    msg->len = ph->payload_len;
    memcpy(msg->data, payload + sizeof(lan_p2p_hdr), ph->payload_len);
    msg->next = NULL;

    int qcount = 0;
    lan_p2p_msg **q = &g_p2p_queue;
    while (*q) {
        qcount++;
        q = &(*q)->next;
    }
    if (qcount >= 256) {
        lan_p2p_msg *old = g_p2p_queue;
        g_p2p_queue = old->next;
        free(old);
        q = &g_p2p_queue;
        while (*q) q = &(*q)->next;
    }
    *q = msg;
}

/* Um membro da sala mudou de nome ou país no menu LAN. */
static void handle_profile(const lan_hdr *h, const uint8_t *payload, size_t len)
{
    if (len < sizeof(lan_profile_body)) return;
    const lan_profile_body *pb = (const lan_profile_body *)payload;
    lan_peer *p = peer_find(h->sender_steam_id);
    if (!p) return;
    char raw[sizeof pb->name + 1];
    memcpy(raw, pb->name, sizeof pb->name);
    raw[sizeof pb->name] = 0;
    sanitize_name(raw, p->name, sizeof p->name);
    char cc[3] = {pb->country[0], pb->country[1], 0};
    peer_set_country(h->sender_steam_id, cc);
    if (g_has_active_lobby) {
        int idx = member_index(&g_active_lobby, h->sender_steam_id);
        if (idx >= 0) snprintf(g_active_lobby.member_names[idx], sizeof g_active_lobby.member_names[idx], "%s", p->name);
        if (g_active_lobby.owner_id == h->sender_steam_id)
            snprintf(g_active_lobby.owner_name, sizeof g_active_lobby.owner_name, "%s", p->name);
    }
    push_event(RE5_LAN_EVT_PERSONA_CHANGE, 0, 0, h->sender_steam_id, 0x0001 /* k_EPersonaChangeName */, 0);
    RE5_INFO("steam_lan", "Jogador #%u agora se chama '%s' (%s)", (uint32_t)h->sender_steam_id, p->name, p->country);
}

static void handle_keepalive(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    if (len < sizeof(lan_keepalive_body)) return;
    const lan_keepalive_body *ka = (const lan_keepalive_body *)payload;
    if (!g_has_active_lobby || g_active_lobby.lobby_id != ka->lobby_id) return;
    if (member_index(&g_active_lobby, h->sender_steam_id) < 0) return;
    struct sockaddr_in addr = *from;
    addr.sin_port = htons(h->port);
    peer_update(h->sender_steam_id, NULL, &addr);   /* também segue o parceiro que trocou de IP (DHCP do Wi-Fi) */
    lan_peer *p = peer_find(h->sender_steam_id);
    if (!p) return;
    uint32_t now = get_ticks_ms();
    p->ka_their_t = ka->t_send ? ka->t_send : 1;
    p->ka_their_at = now;
    if (ka->echo) {
        int32_t rtt = (int32_t)(now - ka->echo - ka->echo_hold);
        if (rtt >= 0 && rtt < 60000) p->rtt_ms = p->rtt_ms < 0 ? rtt : (p->rtt_ms * 3 + rtt) / 4;
    }
}

static void send_keepalives(uint32_t now)
{
    if (!g_has_active_lobby || g_join.active) return;
    for (int i = 0; i < g_active_lobby.member_count && i < RE5_LAN_MAX_LOBBY_MEMBERS; i++) {
        if (g_active_lobby.members[i] == g_local_steam_id) continue;
        lan_peer *p = peer_find(g_active_lobby.members[i]);
        if (!p) continue;
        uint8_t buf[sizeof(lan_hdr) + sizeof(lan_keepalive_body)];
        fill_hdr((lan_hdr *)buf, PKT_KEEPALIVE);
        lan_keepalive_body *ka = (lan_keepalive_body *)(buf + sizeof(lan_hdr));
        ka->lobby_id = g_active_lobby.lobby_id;
        ka->t_send = now ? now : 1;
        ka->echo = p->ka_their_t;
        ka->echo_hold = p->ka_their_t ? now - p->ka_their_at : 0;
        send_raw_udp(g_p2p_sock, &p->addr, buf, sizeof buf);
    }
}

/* Membro sem notícia há MEMBER_TIMEOUT_MS: para o host, sai da sala; para o
 * cliente, se era o host, a sala acabou. O jogo recebe o mesmo que a Steam manda
 * numa queda: LobbyChatUpdate_t "desconectou" e P2PSessionConnectFail_t. */
static void check_member_timeouts(uint32_t now)
{
    if (!g_has_active_lobby || g_join.active) return;
    for (int i = 0; i < g_active_lobby.member_count && i < RE5_LAN_MAX_LOBBY_MEMBERS; i++) {
        uint64_t id = g_active_lobby.members[i];
        if (id == g_local_steam_id) continue;
        lan_peer *p = peer_find(id);
        if (p && now - p->last_seen_ms < g_member_timeout_ms) continue;
        uint64_t lobby_id = g_active_lobby.lobby_id;
        RE5_WARN("steam_lan", "Sem resposta de '%s #%u' ha %u s: desconectado", p ? p->name : "?", (uint32_t)id,
                 g_member_timeout_ms / 1000u);
        push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, lobby_id, id, MEMBER_DISCONNECTED, 0);
        push_event(RE5_LAN_EVT_P2P_SESSION_FAIL, 0, 0, id, P2P_ERROR_TIMEOUT, 0);
        if (p) p->p2p_session_requested = 0;
        if (id == g_active_lobby.owner_id) {
            g_has_active_lobby = 0;
            return;
        }
        for (int k = i; k + 1 < g_active_lobby.member_count && k + 1 < RE5_LAN_MAX_LOBBY_MEMBERS; k++) {
            g_active_lobby.members[k] = g_active_lobby.members[k + 1];
            memcpy(g_active_lobby.member_names[k], g_active_lobby.member_names[k + 1], sizeof g_active_lobby.member_names[k]);
            memcpy(g_active_lobby.member_data[k], g_active_lobby.member_data[k + 1], sizeof g_active_lobby.member_data[k]);
            g_active_lobby.member_data_count[k] = g_active_lobby.member_data_count[k + 1];
        }
        g_active_lobby.member_count--;
        i--;
    }
}

/* Interfaces IPv4 ativas: o IP para mostrar ao jogador e o broadcast de cada rede. */
static void refresh_interfaces(void)
{
    g_n_iface = 0;
    snprintf(g_local_ip, sizeof g_local_ip, "?");
#if !defined(_WIN32)
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if (strcmp(g_local_ip, "?") == 0) snprintf(g_local_ip, sizeof g_local_ip, "%s", inet_ntoa(a));
        if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr && g_n_iface < MAX_IFACES)
            g_iface_bcast[g_n_iface++] = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
    }
    freeifaddrs(ifs);
#else
    /* Sem getifaddrs: o IP que a rota padrão usaria (connect UDP não envia nada). */
    hsock_t t = socket(AF_INET, SOCK_DGRAM, 0);
    if (t == HBAD_SOCK) return;
    struct sockaddr_in probe;
    memset(&probe, 0, sizeof probe);
    probe.sin_family = AF_INET;
    probe.sin_port = htons(9);
    probe.sin_addr.s_addr = inet_addr("192.0.2.1");
    int len = sizeof probe;
    if (connect(t, (struct sockaddr *)&probe, sizeof probe) == 0 && getsockname(t, (struct sockaddr *)&probe, &len) == 0)
        snprintf(g_local_ip, sizeof g_local_ip, "%s", inet_ntoa(probe.sin_addr));
    sock_close(t);
#endif
}

static void parse_packet(const uint8_t *buf, size_t len, const struct sockaddr_in *from)
{
    if (len < sizeof(lan_hdr)) return;
    const lan_hdr *h = (const lan_hdr *)buf;
    if (h->magic != RE5_LAN_MAGIC) return;
    if (h->sender_steam_id == g_local_steam_id) {
        /* Mesmo nonce: eco do nosso broadcast. Outro nonce: outro jogador com o
         * nosso ID, e os dois se confundiriam em tudo (sala, P2P). */
        if (h->nonce != g_nonce && !g_id_clash_warned) {
            g_id_clash_warned = 1;
            RE5_WARN("steam_lan", "Outro jogador em %s usa o mesmo ID #%u: apague o arquivo de identidade "
                     "de um dos dois (ou defina RE5_STEAM_ID) para gerar um ID novo",
                     inet_ntoa(from->sin_addr), (uint32_t)g_local_steam_id);
        }
        return;
    }

    const uint8_t *payload = buf + sizeof(lan_hdr);
    size_t payload_len = len - sizeof(lan_hdr);
    lan_peer *known = peer_find(h->sender_steam_id);
    if (known) known->last_seen_ms = get_ticks_ms();

    switch (h->type) {
    case PKT_BEACON:
        handle_beacon(h, payload, payload_len, from);
        break;
    case PKT_PING:
        handle_ping(h, from);
        break;
    case PKT_JOIN_REQ:
        handle_join_req(h, payload, payload_len, from);
        break;
    case PKT_JOIN_RESP:
        handle_join_resp(h, payload, payload_len, from);
        break;
    case PKT_LEAVE:
        handle_leave(h, payload, payload_len);
        break;
    case PKT_DATA_UPDATE:
        handle_data_update(h, payload, payload_len);
        break;
    case PKT_PROFILE:
        handle_profile(h, payload, payload_len);
        break;
    case PKT_KEEPALIVE:
        handle_keepalive(h, payload, payload_len, from);
        break;
    case PKT_P2P_DATA:
        handle_p2p_data(h, payload, payload_len, from);
        break;
    default:
        break;
    }
}

static void check_pending_search(void)
{
    if (g_pending_search_call == 0) return;
    uint32_t now = get_ticks_ms();
    if (now < g_pending_search_expire) return;

    /* Remove lobbies inativos (sem beacon ha mais de 5 segundos) */
    int active = 0;
    for (int i = 0; i < g_num_lobbies; i++) {
        if (now - g_lobbies[i].last_seen_ms < 5000) {
            if (active != i) g_lobbies[active] = g_lobbies[i];
            active++;
        }
    }
    g_num_lobbies = active;

    push_event(RE5_LAN_EVT_LOBBY_MATCH_LIST, g_pending_search_call, 0, 0, (uint32_t)g_num_lobbies, 0);
    RE5_INFO("steam_lan", "Busca de salas concluida: %d sala(s) encontrada(s)", g_num_lobbies);
    g_pending_search_call = 0;
}

static void poll_sockets_once(void)
{
    uint8_t buf[RE5_LAN_MAX_PACKET_SIZE];
    struct sockaddr_in from;

    /* Drena discovery socket */
    for (;;) {
#if defined(_WIN32)
        int fromlen = sizeof from;
        int n = recvfrom(g_disc_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAECONNRESET) continue;
            break;
        }
#else
        socklen_t fromlen = sizeof from;
        int n = recvfrom(g_disc_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
#endif
        re5_mutex_lock(&g_mu);
        parse_packet(buf, (size_t)n, &from);
        re5_mutex_unlock(&g_mu);
    }

    /* Drena P2P socket */
    for (;;) {
#if defined(_WIN32)
        int fromlen = sizeof from;
        int n = recvfrom(g_p2p_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAECONNRESET) continue;
            break;
        }
#else
        socklen_t fromlen = sizeof from;
        int n = recvfrom(g_p2p_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
#endif
        re5_mutex_lock(&g_mu);
        parse_packet(buf, (size_t)n, &from);
        re5_mutex_unlock(&g_mu);
    }
}

static void lan_worker(void *arg)
{
    (void)arg;
    uint32_t last_beacon_ms = 0;
    while (g_running) {
        poll_sockets_once();

        re5_mutex_lock(&g_mu);
        check_pending_search();

        uint32_t now = get_ticks_ms();
        join_tick(now);
        if (now - g_iface_at >= IFACE_REFRESH_MS) {
            refresh_interfaces();
            g_iface_at = now;
        }
        if (now - last_beacon_ms >= KEEPALIVE_MS) {
            broadcast_beacon();
            send_keepalives(now);
            last_beacon_ms = now;
        }
        check_member_timeouts(now);
        re5_mutex_unlock(&g_mu);

        re5_host_sleep_ms(5);
    }
}

/* ---- identidade: ID único e nome do jogador ---- */

#define LAN_ID_FILE_GUEST "C:\\Users\\Player\\re5_lan.ini"
#define LAN_DEFAULT_NAME  "Jogador"

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Aleatório para o ID e o nonce; não precisa ser criptográfico. */
static uint32_t lan_random(void)
{
    uint32_t r = 0;
#if defined(_WIN32)
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    r = (uint32_t)qpc.QuadPart ^ (uint32_t)(qpc.QuadPart >> 32) ^ (GetCurrentProcessId() << 16);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&r, sizeof r, 1, f) != 1) r = 0;
        fclose(f);
    }
    if (!r) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        r = (uint32_t)ts.tv_nsec ^ ((uint32_t)getpid() << 16);
    }
#endif
    static uint32_t s_ctr;
    return mix32(r ^ (uint32_t)time(NULL) ^ (++s_ctr * 0x9E3779B9u));
}

/* Nome legível em qualquer fonte do jogo: ASCII imprimível, sem '#' (separa o ID),
 * espaços das pontas removidos, no máximo RE5_LAN_MAX_NAME_LEN caracteres. */
static void sanitize_name(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in == ' ' || *in == '\t') in++;
    for (; *in && n + 1 < cap && n < RE5_LAN_MAX_NAME_LEN; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '\r' || ch == '\n') break;
        out[n++] = (ch < 0x20 || ch >= 0x7F || ch == '#') ? '_' : (char)ch;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static int identity_path(char *out, size_t cap)
{
    const char *env = getenv("RE5_LAN_CONFIG");
    if (env && *env) return snprintf(out, cap, "%s", env) < (int)cap ? 0 : -1;
    return re5_vfs_resolve(LAN_ID_FILE_GUEST, out, cap);
}

static void mkdir_parents(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/' && *s != '\\') continue;
        char k = *s;
        *s = 0;
#if defined(_WIN32)
        if (s[-1] != ':') _mkdir(p);
#else
        mkdir(p, 0755);
#endif
        *s = k;
    }
}

/* Código de país (bandeira): duas letras A-Z, maiúsculas. 1 = válido. */
static int sanitize_country(const char *in, char out[4])
{
    if (!in) return 0;
    while (*in == ' ' || *in == '\t') in++;
    if (!isalpha((unsigned char)in[0]) || !isalpha((unsigned char)in[1]) || isalnum((unsigned char)in[2])) return 0;
    out[0] = (char)toupper((unsigned char)in[0]);
    out[1] = (char)toupper((unsigned char)in[1]);
    out[2] = out[3] = 0;
    return 1;
}

/* "1.2.3.4, 5.6.7.8" -> g_manual_hosts. Entradas inválidas são ignoradas. */
static void parse_manual_hosts(const char *list)
{
    g_n_manual = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list ? list : "");
    for (char *tok = strtok(buf, ", ;\t\r\n"); tok && g_n_manual < MAX_MANUAL_HOSTS; tok = strtok(NULL, ", ;\t\r\n")) {
        struct in_addr a;
        a.s_addr = inet_addr(tok);
        if (a.s_addr != INADDR_NONE && a.s_addr != 0) g_manual_hosts[g_n_manual++] = a;
    }
}

static void manual_hosts_text(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < g_n_manual; i++) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", i ? "," : "", inet_ntoa(g_manual_hosts[i]));
    }
}

/* Arquivo "chave=valor": id (6 dígitos), nome, pais, hosts. 1 = lido. */
static int read_identity(const char *path, uint32_t *id, char *name, size_t cap, char country[4], char *hosts,
                         size_t hosts_cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        if (strcmp(key, "id") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v >= RE5_LAN_ID_MIN && v < RE5_LAN_ID_MIN + RE5_LAN_ID_SPAN) *id = (uint32_t)v;
        } else if (strcmp(key, "nome") == 0 || strcmp(key, "name") == 0) {
            sanitize_name(val, name, cap);
        } else if (strcmp(key, "pais") == 0 || strcmp(key, "country") == 0) {
            sanitize_country(val, country);
        } else if (strcmp(key, "hosts") == 0) {
            snprintf(hosts, hosts_cap, "%s", val);
            for (char *e = hosts + strlen(hosts); e > hosts && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        }
    }
    fclose(f);
    return 1;
}

static void write_identity(const char *path)
{
    mkdir_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) {
        RE5_WARN("steam_lan", "Nao foi possivel gravar a identidade LAN em %s: o ID muda a cada execucao", path);
        return;
    }
    char hosts[128];
    manual_hosts_text(hosts, sizeof hosts);
    fprintf(f, "# Identidade do jogador no multiplayer LAN do RE5.\n"
               "# Tambem editavel dentro do jogo: F12 ou toque com tres dedos.\n"
               "# nome:  o que os outros jogadores veem (ate %d caracteres, sem '#').\n"
               "# pais:  codigo de duas letras da bandeira (BR, PT, US...).\n"
               "# hosts: IPs de hosts para procurar direto, separados por virgula (rede sem broadcast).\n"
               "# id:    seu codigo unico na rede; nao copie este arquivo para outro aparelho.\n"
               "#        Apague a linha para gerar um novo.\n"
               "nome=%s\n"
               "pais=%s\n"
               "hosts=%s\n"
               "id=%u\n",
            RE5_LAN_MAX_NAME_LEN, g_local_name, g_local_country, hosts, g_file_id);
    fclose(f);
}

/* País padrão: o da localidade do sistema (pt_BR -> BR), senão US. */
static void default_country(char out[4])
{
    snprintf(out, 4, "US");
#if defined(_WIN32)
    char cc[8];
    if (GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, cc, sizeof cc) > 0) sanitize_country(cc, out);
#else
    const char *envs[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (int i = 0; i < 3; i++) {
        const char *l = getenv(envs[i]);
        const char *u = l ? strchr(l, '_') : NULL;   /* "pt_BR.UTF-8" */
        if (u && u[1] && u[2]) {
            char two[3] = {u[1], u[2], 0};
            if (sanitize_country(two, out)) return;
        }
    }
#endif
}

/* Prioridade: RE5_STEAM_ID / RE5_PLAYER_NAME / RE5_COUNTRY / RE5_LAN_HOSTS >
 * arquivo de identidade > gerado. Instâncias no mesmo aparelho dividem o arquivo,
 * então a N-ésima (porta P2P base + N) soma N ao ID e cada uma tem um ID próprio. */
static void load_identity(uint16_t base_p2p)
{
    uint32_t slot = (uint32_t)(g_p2p_port - base_p2p);
    uint32_t id = 0;
    char name[RE5_LAN_MAX_NAME_LEN + 1] = "";
    char country[4] = "";
    char hosts[256] = "";

    int have_path = identity_path(g_identity_path, sizeof g_identity_path) == 0;
    if (!have_path) g_identity_path[0] = 0;
    int read_ok = have_path && read_identity(g_identity_path, &id, name, sizeof name, country, hosts, sizeof hosts);
    int need_write = have_path && (!read_ok || !id || !name[0] || !country[0]);
    if (!id) id = RE5_LAN_ID_MIN + lan_random() % RE5_LAN_ID_SPAN;
    if (!name[0]) snprintf(name, sizeof name, "%s", LAN_DEFAULT_NAME);
    if (!country[0]) default_country(country);
    g_file_id = id;
    snprintf(g_local_name, sizeof g_local_name, "%s", name);
    snprintf(g_local_country, sizeof g_local_country, "%s", country);
    parse_manual_hosts(hosts);
    if (need_write) {
        write_identity(g_identity_path);
        RE5_INFO("steam_lan", "Identidade LAN criada em %s (F12 ou toque com tres dedos no jogo para editar)",
                 g_identity_path);
    } else if (have_path) {
        RE5_INFO("steam_lan", "Identidade LAN lida de %s", g_identity_path);
    }
    id = RE5_LAN_ID_MIN + (id - RE5_LAN_ID_MIN + slot) % RE5_LAN_ID_SPAN;

    const char *env_id = getenv("RE5_STEAM_ID");
    if (!env_id || !*env_id) env_id = getenv("RE5_LAN_STEAM_ID");
    if (env_id && strtoul(env_id, NULL, 10) > 0) id = (uint32_t)strtoul(env_id, NULL, 10);

    const char *env_name = getenv("RE5_PLAYER_NAME");
    if (!env_name || !*env_name) env_name = getenv("RE5_LAN_NAME");
    if (env_name && *env_name) sanitize_name(env_name, g_local_name, sizeof g_local_name);
    if (g_local_name[0] == 0) snprintf(g_local_name, sizeof g_local_name, "%s", LAN_DEFAULT_NAME);
    sanitize_country(getenv("RE5_COUNTRY"), g_local_country);
    if (getenv("RE5_LAN_HOSTS")) parse_manual_hosts(getenv("RE5_LAN_HOSTS"));

    g_local_steam_id = ((uint64_t)RE5_LAN_STEAMID_USER_HI << 32) | id;
    g_nonce = lan_random();
}

int steam_lan_init(void)
{
    if (g_initialized) return 0;
    re5_mutex_init(&g_mu);

#if defined(_WIN32)
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        re5_fatal("steam_lan", "WSAStartup falhou");
        return -1;
    }
#endif

    /* Configuração de portas e identidades */
    const char *env_disc = getenv("RE5_LAN_DISCOVERY_PORT");
    if (env_disc && atoi(env_disc) > 0) g_discovery_port = (uint16_t)atoi(env_disc);

    const char *env_p2p = getenv("RE5_LAN_PORT");
    uint16_t base_p2p = RE5_LAN_DEFAULT_P2P_PORT;
    if (env_p2p && atoi(env_p2p) > 0) base_p2p = (uint16_t)atoi(env_p2p);

    /* Socket de descoberta (Broadcast) */
    g_disc_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_disc_sock == HBAD_SOCK) {
        re5_fatal("steam_lan", "Falha ao criar socket de descoberta");
        return -1;
    }
    sock_set_reuse(g_disc_sock);
    sock_set_broadcast(g_disc_sock);
    sock_set_nonblocking(g_disc_sock);
    sock_fix_connreset(g_disc_sock);

    struct sockaddr_in disc_addr;
    memset(&disc_addr, 0, sizeof disc_addr);
    disc_addr.sin_family = AF_INET;
    disc_addr.sin_port = htons(g_discovery_port);
    disc_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_disc_sock, (struct sockaddr *)&disc_addr, sizeof disc_addr) != 0) {
        RE5_WARN("steam_lan", "Aviso: bind na porta de descoberta %u falhou (outra instancia ativa); operando em modo cliente", g_discovery_port);
    }

    /* Socket P2P Unicast: encontra uma porta disponivel a partir de base_p2p */
    g_p2p_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_p2p_sock == HBAD_SOCK) {
        re5_fatal("steam_lan", "Falha ao criar socket P2P");
        return -1;
    }
    /* NAO usar sock_set_reuse em g_p2p_sock para que o bind falhe se a porta ja estiver
     * ocupada por outra instancia no mesmo computador, permitindo auto-alocar 27017, 27018... */
    sock_set_nonblocking(g_p2p_sock);
    sock_fix_connreset(g_p2p_sock);

    uint16_t bound_port = 0;
    for (uint16_t p = base_p2p; p < base_p2p + 100; p++) {
        struct sockaddr_in p2p_addr;
        memset(&p2p_addr, 0, sizeof p2p_addr);
        p2p_addr.sin_family = AF_INET;
        p2p_addr.sin_port = htons(p);
        p2p_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_p2p_sock, (struct sockaddr *)&p2p_addr, sizeof p2p_addr) == 0) {
            bound_port = p;
            break;
        }
    }
    if (!bound_port) {
        re5_fatal("steam_lan", "Nao foi possivel alocar uma porta UDP P2P livre entre %u e %u", base_p2p, base_p2p + 100);
        return -1;
    }
    g_p2p_port = bound_port;

    load_identity(base_p2p);
    const char *env_to = getenv("RE5_LAN_TIMEOUT_MS");
    if (env_to && strtoul(env_to, NULL, 10) >= 500) g_member_timeout_ms = (uint32_t)strtoul(env_to, NULL, 10);
    refresh_interfaces();
    g_iface_at = get_ticks_ms();

    g_running = 1;
    g_initialized = 1;

    RE5_INFO("steam_lan", "Subcamada LAN iniciada: jogador '%s #%u' (SteamID %llu), porta P2P %u, descoberta %u",
             g_local_name, (uint32_t)g_local_steam_id, (unsigned long long)g_local_steam_id, g_p2p_port, g_discovery_port);

    if (re5_host_thread_start(lan_worker, NULL, 1u << 20) != 0) {
        re5_fatal("steam_lan", "Falha ao iniciar thread de rede LAN");
        return -1;
    }

    return 0;
}

void steam_lan_shutdown(void)
{
    if (!g_initialized) return;
    g_running = 0;
    re5_host_sleep_ms(20);

    re5_mutex_lock(&g_mu);
    if (g_has_active_lobby) {
        steam_lan_leave_lobby(g_active_lobby.lobby_id);
    }

    sock_close(g_disc_sock);
    sock_close(g_p2p_sock);
    g_disc_sock = HBAD_SOCK;
    g_p2p_sock  = HBAD_SOCK;

    lan_p2p_msg *msg = g_p2p_queue;
    while (msg) {
        lan_p2p_msg *next = msg->next;
        free(msg);
        msg = next;
    }
    g_p2p_queue = NULL;

    g_initialized = 0;
    re5_mutex_unlock(&g_mu);
}

void steam_lan_poll(void)
{
    poll_sockets_once();
    re5_mutex_lock(&g_mu);
    check_pending_search();
    join_tick(get_ticks_ms());
    re5_mutex_unlock(&g_mu);
}

uint64_t steam_lan_get_local_steam_id(void)
{
    return g_local_steam_id;
}

const char *steam_lan_get_local_name(void)
{
    return g_local_name;
}

void steam_lan_get_display_name(uint64_t steam_id, char *out, size_t cap)
{
    if (!out || !cap) return;
    re5_mutex_lock(&g_mu);
    const char *name = NULL;
    if (steam_id == g_local_steam_id) {
        name = g_local_name;
    } else {
        lan_peer *p = peer_find(steam_id);
        if (p && p->name[0]) name = p->name;
    }
    snprintf(out, cap, "%s #%u", name ? name : "Jogador", (uint32_t)steam_id);
    re5_mutex_unlock(&g_mu);
}

uint16_t steam_lan_get_p2p_port(void)
{
    return g_p2p_port;
}

/* Matchmaking */

uint64_t steam_lan_create_lobby(uint32_t lobby_type, int max_members, uint64_t call_id)
{
    (void)lobby_type;
    re5_mutex_lock(&g_mu);

    uint64_t lobby_id = ((uint64_t)RE5_LAN_STEAMID_LOBBY_HI << 32) | (uint32_t)g_local_steam_id;
    memset(&g_active_lobby, 0, sizeof g_active_lobby);
    g_active_lobby.lobby_id = lobby_id;
    g_active_lobby.owner_id = g_local_steam_id;
    snprintf(g_active_lobby.owner_name, sizeof g_active_lobby.owner_name, "%s", g_local_name);
    g_active_lobby.host_port = g_p2p_port;
    g_active_lobby.member_count = 1;
    g_active_lobby.max_members = max_members > 0 && max_members <= RE5_LAN_MAX_LOBBY_MEMBERS ? (uint16_t)max_members : RE5_LAN_MAX_LOBBY_MEMBERS;
    g_active_lobby.members[0] = g_local_steam_id;
    snprintf(g_active_lobby.member_names[0], sizeof g_active_lobby.member_names[0], "%s", g_local_name);
    g_active_lobby.is_local = 1;
    g_active_lobby.joinable = 1;
    g_active_lobby.last_seen_ms = get_ticks_ms();
    g_has_active_lobby = 1;

    /* Como a Steam: LobbyCreated_t (call result) e, para o próprio criador, LobbyEnter_t
     * e LobbyDataUpdate_t. Sem o LobbyEnter_t o jogo do host não se considera dentro da
     * sala e ignora o LobbyChatUpdate_t de quem entra depois. */
    push_event(RE5_LAN_EVT_LOBBY_CREATED, call_id, lobby_id, g_local_steam_id, 1 /* k_EResultOK */, 0);
    push_event(RE5_LAN_EVT_LOBBY_ENTERED, 0, lobby_id, g_local_steam_id, ENTER_SUCCESS, 0);
    push_event(RE5_LAN_EVT_LOBBY_DATA_UPDATE, 0, lobby_id, lobby_id, 1, 0);

    broadcast_beacon();
    re5_mutex_unlock(&g_mu);

    RE5_INFO("steam_lan", "Lobby %llu criado pelo host '%s #%u' (max %u membros)", (unsigned long long)lobby_id, g_local_name,
             (uint32_t)g_local_steam_id, g_active_lobby.max_members);
    return lobby_id;
}

uint64_t steam_lan_request_lobby_list(uint64_t call_id)
{
    re5_mutex_lock(&g_mu);

    /* Envia PING broadcast para que hosts respondam imediatamente */
    lan_hdr ping;
    fill_hdr(&ping, PKT_PING);
    send_broadcast(&ping, sizeof ping);

    g_pending_search_call = call_id;
    g_pending_search_expire = get_ticks_ms() + 200; /* Janela de 200ms para resposta de pings */

    re5_mutex_unlock(&g_mu);
    return 1;
}

int steam_lan_get_lobby_count(void)
{
    re5_mutex_lock(&g_mu);
    int n = g_num_lobbies;
    re5_mutex_unlock(&g_mu);
    return n;
}

uint64_t steam_lan_get_lobby_by_index(int idx)
{
    re5_mutex_lock(&g_mu);
    uint64_t id = (idx >= 0 && idx < g_num_lobbies) ? g_lobbies[idx].lobby_id : 0;
    re5_mutex_unlock(&g_mu);
    return id;
}

int steam_lan_get_num_lobby_members(uint64_t lobby_id)
{
    re5_mutex_lock(&g_mu);
    int n = 0;
    if (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) {
        n = g_active_lobby.member_count;
    } else {
        for (int i = 0; i < g_num_lobbies; i++) {
            if (g_lobbies[i].lobby_id == lobby_id) {
                n = g_lobbies[i].member_count;
                break;
            }
        }
    }
    re5_mutex_unlock(&g_mu);
    return n;
}

int steam_lan_get_lobby_member_limit(uint64_t lobby_id)
{
    re5_mutex_lock(&g_mu);
    int max = RE5_LAN_MAX_LOBBY_MEMBERS;
    if (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) {
        max = g_active_lobby.max_members ? g_active_lobby.max_members : RE5_LAN_MAX_LOBBY_MEMBERS;
    } else {
        for (int i = 0; i < g_num_lobbies; i++) {
            if (g_lobbies[i].lobby_id == lobby_id) {
                max = g_lobbies[i].max_members ? g_lobbies[i].max_members : RE5_LAN_MAX_LOBBY_MEMBERS;
                break;
            }
        }
    }
    re5_mutex_unlock(&g_mu);
    return max;
}

uint64_t steam_lan_get_lobby_member_by_index(uint64_t lobby_id, int idx)
{
    re5_mutex_lock(&g_mu);
    uint64_t id = 0;
    if (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) {
        if (idx >= 0 && idx < g_active_lobby.member_count) id = g_active_lobby.members[idx];
    } else {
        for (int i = 0; i < g_num_lobbies; i++) {
            if (g_lobbies[i].lobby_id == lobby_id) {
                if (idx >= 0 && idx < g_lobbies[i].member_count) id = g_lobbies[i].members[idx];
                break;
            }
        }
    }
    re5_mutex_unlock(&g_mu);
    return id;
}

uint64_t steam_lan_get_lobby_owner(uint64_t lobby_id)
{
    re5_mutex_lock(&g_mu);
    uint64_t id = 0;
    if (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) {
        id = g_active_lobby.owner_id;
    } else {
        for (int i = 0; i < g_num_lobbies; i++) {
            if (g_lobbies[i].lobby_id == lobby_id) {
                id = g_lobbies[i].owner_id;
                break;
            }
        }
    }
    re5_mutex_unlock(&g_mu);
    return id;
}

const char *steam_lan_get_lobby_data(uint64_t lobby_id, const char *key)
{
    if (!key || !*key) return "";
    re5_mutex_lock(&g_mu);
    const char *val = "";
    lan_lobby *lb = (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) ? &g_active_lobby : NULL;
    if (!lb) {
        for (int i = 0; i < g_num_lobbies; i++) {
            if (g_lobbies[i].lobby_id == lobby_id) { lb = &g_lobbies[i]; break; }
        }
    }
    if (lb) {
        for (int i = 0; i < lb->data_count; i++) {
            if (strcmp(lb->data[i].key, key) == 0) {
                val = lb->data[i].val;
                break;
            }
        }
    }
    re5_mutex_unlock(&g_mu);
    return val;
}

int steam_lan_set_lobby_data(uint64_t lobby_id, const char *key, const char *val)
{
    if (!key || !*key) return 0;
    re5_mutex_lock(&g_mu);
    if (!g_has_active_lobby || g_active_lobby.lobby_id != lobby_id || g_active_lobby.owner_id != g_local_steam_id) {
        re5_mutex_unlock(&g_mu);
        return 0;
    }

    int found = 0;
    for (int i = 0; i < g_active_lobby.data_count; i++) {
        if (strcmp(g_active_lobby.data[i].key, key) == 0) {
            snprintf(g_active_lobby.data[i].val, sizeof g_active_lobby.data[i].val, "%s", val ? val : "");
            found = 1;
            break;
        }
    }
    if (!found && g_active_lobby.data_count < RE5_LAN_MAX_DATA_ENTRIES) {
        int idx = g_active_lobby.data_count++;
        snprintf(g_active_lobby.data[idx].key, sizeof g_active_lobby.data[idx].key, "%s", key);
        snprintf(g_active_lobby.data[idx].val, sizeof g_active_lobby.data[idx].val, "%s", val ? val : "");
    }

    /* Envia atualizacao unicast para outros membros */
    uint8_t buf[sizeof(lan_hdr) + sizeof(lan_data_update_body)];
    lan_hdr *h = (lan_hdr *)buf;
    fill_hdr(h, PKT_DATA_UPDATE);

    lan_data_update_body *du = (lan_data_update_body *)(buf + sizeof(lan_hdr));
    du->lobby_id = lobby_id;
    du->member_id = 0;
    snprintf(du->key, sizeof du->key, "%s", key);
    snprintf(du->val, sizeof du->val, "%s", val ? val : "");

    for (int i = 1; i < g_active_lobby.member_count; i++) {
        lan_peer *p = peer_find(g_active_lobby.members[i]);
        if (p) send_raw_udp(g_p2p_sock, &p->addr, buf, sizeof buf);
    }

    push_event(RE5_LAN_EVT_LOBBY_DATA_UPDATE, 0, lobby_id, lobby_id, 1, 0);
    re5_mutex_unlock(&g_mu);
    return 1;
}

const char *steam_lan_get_lobby_member_data(uint64_t lobby_id, uint64_t member_id, const char *key)
{
    if (!key || !*key) return "";
    re5_mutex_lock(&g_mu);
    const char *val = "";
    if (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) {
        for (int i = 0; i < g_active_lobby.member_count; i++) {
            if (g_active_lobby.members[i] == member_id) {
                for (int k = 0; k < g_active_lobby.member_data_count[i]; k++) {
                    if (strcmp(g_active_lobby.member_data[i][k].key, key) == 0) {
                        val = g_active_lobby.member_data[i][k].val;
                        break;
                    }
                }
                break;
            }
        }
    }
    re5_mutex_unlock(&g_mu);
    return val;
}

void steam_lan_set_lobby_member_data(uint64_t lobby_id, const char *key, const char *val)
{
    if (!key || !*key) return;
    re5_mutex_lock(&g_mu);
    if (!g_has_active_lobby || g_active_lobby.lobby_id != lobby_id) {
        re5_mutex_unlock(&g_mu);
        return;
    }

    int midx = -1;
    for (int i = 0; i < g_active_lobby.member_count; i++) {
        if (g_active_lobby.members[i] == g_local_steam_id) { midx = i; break; }
    }
    if (midx >= 0) {
        int found = 0;
        for (int k = 0; k < g_active_lobby.member_data_count[midx]; k++) {
            if (strcmp(g_active_lobby.member_data[midx][k].key, key) == 0) {
                snprintf(g_active_lobby.member_data[midx][k].val, sizeof g_active_lobby.member_data[midx][k].val, "%s", val ? val : "");
                found = 1;
                break;
            }
        }
        if (!found && g_active_lobby.member_data_count[midx] < RE5_LAN_MAX_DATA_ENTRIES) {
            int idx = g_active_lobby.member_data_count[midx]++;
            snprintf(g_active_lobby.member_data[midx][idx].key, sizeof g_active_lobby.member_data[midx][idx].key, "%s", key);
            snprintf(g_active_lobby.member_data[midx][idx].val, sizeof g_active_lobby.member_data[midx][idx].val, "%s", val ? val : "");
        }

        uint8_t buf[sizeof(lan_hdr) + sizeof(lan_data_update_body)];
        lan_hdr *h = (lan_hdr *)buf;
        fill_hdr(h, PKT_DATA_UPDATE);

        lan_data_update_body *du = (lan_data_update_body *)(buf + sizeof(lan_hdr));
        du->lobby_id = lobby_id;
        du->member_id = g_local_steam_id;
        snprintf(du->key, sizeof du->key, "%s", key);
        snprintf(du->val, sizeof du->val, "%s", val ? val : "");

        for (int i = 0; i < g_active_lobby.member_count; i++) {
            if (g_active_lobby.members[i] != g_local_steam_id) {
                lan_peer *p = peer_find(g_active_lobby.members[i]);
                if (p) send_raw_udp(g_p2p_sock, &p->addr, buf, sizeof buf);
            }
        }
        push_event(RE5_LAN_EVT_LOBBY_DATA_UPDATE, 0, lobby_id, g_local_steam_id, 1, 0);
    }
    re5_mutex_unlock(&g_mu);
}

int steam_lan_join_lobby(uint64_t lobby_id, uint64_t call_id)
{
    re5_mutex_lock(&g_mu);
    lan_lobby *lb = NULL;
    for (int i = 0; i < g_num_lobbies; i++) {
        if (g_lobbies[i].lobby_id == lobby_id) {
            lb = &g_lobbies[i];
            break;
        }
    }
    if (!lb) {
        push_event(RE5_LAN_EVT_LOBBY_ENTERED, call_id, lobby_id, g_local_steam_id, ENTER_DOESNT_EXIST, 0);
        re5_mutex_unlock(&g_mu);
        return 0;
    }

    g_active_lobby = *lb;
    g_active_lobby.is_local = 0;
    memset(g_active_lobby.member_data_count, 0, sizeof g_active_lobby.member_data_count);
    g_has_active_lobby = 1;

    memset(&g_join.host, 0, sizeof g_join.host);
    g_join.host.sin_family = AF_INET;
    g_join.host.sin_port = htons(lb->host_port);
    g_join.host.sin_addr = lb->host_ip;
    peer_update(lb->owner_id, lb->owner_name, &g_join.host);

    uint32_t now = get_ticks_ms();
    g_join.active = 1;
    g_join.lobby_id = lobby_id;
    g_join.call_id = call_id;
    g_join.deadline_ms = now + JOIN_TIMEOUT_MS;
    g_join.next_send_ms = now + JOIN_RESEND_MS;
    send_join_req();

    RE5_INFO("steam_lan", "Pedindo para entrar no lobby de '%s #%u' (%s:%u)", lb->owner_name, (uint32_t)lb->owner_id,
             inet_ntoa(lb->host_ip), lb->host_port);
    re5_mutex_unlock(&g_mu);
    return 1;
}

void steam_lan_leave_lobby(uint64_t lobby_id)
{
    re5_mutex_lock(&g_mu);
    if (!g_has_active_lobby || g_active_lobby.lobby_id != lobby_id) {
        re5_mutex_unlock(&g_mu);
        return;
    }
    g_join.active = 0;

    uint8_t buf[sizeof(lan_hdr) + sizeof(uint64_t)];
    lan_hdr *h = (lan_hdr *)buf;
    fill_hdr(h, PKT_LEAVE);
    memcpy(buf + sizeof(lan_hdr), &lobby_id, sizeof lobby_id);

    for (int i = 0; i < g_active_lobby.member_count; i++) {
        if (g_active_lobby.members[i] != g_local_steam_id) {
            lan_peer *p = peer_find(g_active_lobby.members[i]);
            if (p) send_raw_udp(g_p2p_sock, &p->addr, buf, sizeof buf);
        }
    }

    g_has_active_lobby = 0;
    re5_mutex_unlock(&g_mu);
}

void steam_lan_set_lobby_joinable(uint64_t lobby_id, int joinable)
{
    re5_mutex_lock(&g_mu);
    if (g_has_active_lobby && g_active_lobby.lobby_id == lobby_id) {
        g_active_lobby.joinable = joinable;
    }
    re5_mutex_unlock(&g_mu);
}

/* Networking P2P */

int steam_lan_send_p2p(uint64_t remote_id, const void *data, uint32_t len, int send_type, int channel)
{
    if (!data || len > (RE5_LAN_MAX_PACKET_SIZE - sizeof(lan_hdr) - sizeof(lan_p2p_hdr))) return 0;

    re5_mutex_lock(&g_mu);
    lan_peer *p = peer_find(remote_id);
    if (!p) {
        re5_mutex_unlock(&g_mu);
        return 0;
    }

    uint8_t buf[RE5_LAN_MAX_PACKET_SIZE];
    lan_hdr *h = (lan_hdr *)buf;
    fill_hdr(h, PKT_P2P_DATA);

    lan_p2p_hdr *ph = (lan_p2p_hdr *)(buf + sizeof(lan_hdr));
    ph->channel = (uint16_t)channel;
    ph->send_type = (uint8_t)send_type;
    ph->_pad = 0;
    static uint32_t s_seq = 0;
    ph->seq = ++s_seq;
    ph->payload_len = len;

    memcpy(buf + sizeof(lan_hdr) + sizeof(lan_p2p_hdr), data, len);
    size_t total_len = sizeof(lan_hdr) + sizeof(lan_p2p_hdr) + len;

    send_raw_udp(g_p2p_sock, &p->addr, buf, total_len);
    re5_mutex_unlock(&g_mu);
    return 1;
}

int steam_lan_is_p2p_available(uint32_t *msg_size, int channel)
{
    re5_mutex_lock(&g_mu);
    lan_p2p_msg *msg = g_p2p_queue;
    while (msg) {
        if (msg->channel == (uint16_t)channel) {
            if (msg_size) *msg_size = msg->len;
            re5_mutex_unlock(&g_mu);
            return 1;
        }
        msg = msg->next;
    }
    if (msg_size) *msg_size = 0;
    re5_mutex_unlock(&g_mu);
    return 0;
}

int steam_lan_read_p2p(void *dest, uint32_t dest_len, uint32_t *msg_size, uint64_t *remote_id, int channel)
{
    if (!dest || dest_len == 0) return 0;

    re5_mutex_lock(&g_mu);
    lan_p2p_msg **prev = &g_p2p_queue;
    lan_p2p_msg *msg = g_p2p_queue;
    while (msg) {
        if (msg->channel == (uint16_t)channel) {
            uint32_t copy_len = msg->len < dest_len ? msg->len : dest_len;
            memcpy(dest, msg->data, copy_len);
            if (msg_size) *msg_size = copy_len;
            if (remote_id) *remote_id = msg->remote_id;

            *prev = msg->next;
            free(msg);
            re5_mutex_unlock(&g_mu);
            return 1;
        }
        prev = &msg->next;
        msg = msg->next;
    }
    re5_mutex_unlock(&g_mu);
    return 0;
}

int steam_lan_accept_p2p(uint64_t remote_id)
{
    (void)remote_id;
    return 1;
}

int steam_lan_close_p2p(uint64_t remote_id)
{
    (void)remote_id;
    return 1;
}

int steam_lan_get_p2p_state(uint64_t remote_id, void *out_state)
{
    if (!out_state) return 0;
    re5_mutex_lock(&g_mu);
    lan_peer *p = peer_find(remote_id);
    re5_mutex_unlock(&g_mu);

    uint8_t *st = (uint8_t *)out_state;
    memset(st, 0, 20);
    st[0] = p ? 1 : 0; /* m_bConnectionActive */
    st[1] = 0;         /* m_bConnecting */
    st[2] = 0;         /* m_eP2PSessionError */
    st[3] = 0;         /* m_bUsingRelay */
    return p ? 1 : 0;
}

/* ---- perfil editável no jogo (menu LAN) ---- */

void steam_lan_get_local_country(char out[4])
{
    re5_mutex_lock(&g_mu);
    memcpy(out, g_local_country, 4);
    re5_mutex_unlock(&g_mu);
}

int steam_lan_set_profile(const char *name, const char *country)
{
    char n[RE5_LAN_MAX_NAME_LEN + 1], cc[4];
    sanitize_name(name ? name : "", n, sizeof n);
    if (!n[0] || !sanitize_country(country, cc)) return 0;

    re5_mutex_lock(&g_mu);
    snprintf(g_local_name, sizeof g_local_name, "%s", n);
    memcpy(g_local_country, cc, sizeof cc);
    if (g_identity_path[0]) write_identity(g_identity_path);

    if (g_has_active_lobby) {
        int me = member_index(&g_active_lobby, g_local_steam_id);
        if (me >= 0) snprintf(g_active_lobby.member_names[me], sizeof g_active_lobby.member_names[me], "%s", n);
        if (g_active_lobby.owner_id == g_local_steam_id)
            snprintf(g_active_lobby.owner_name, sizeof g_active_lobby.owner_name, "%s", n);

        uint8_t buf[sizeof(lan_hdr) + sizeof(lan_profile_body)];
        fill_hdr((lan_hdr *)buf, PKT_PROFILE);
        lan_profile_body *pb = (lan_profile_body *)(buf + sizeof(lan_hdr));
        memset(pb, 0, sizeof *pb);
        snprintf(pb->name, sizeof pb->name, "%s", n);
        memcpy(pb->country, cc, 2);
        for (int i = 0; i < g_active_lobby.member_count && i < RE5_LAN_MAX_LOBBY_MEMBERS; i++) {
            if (g_active_lobby.members[i] == g_local_steam_id) continue;
            lan_peer *p = peer_find(g_active_lobby.members[i]);
            if (p) send_raw_udp(g_p2p_sock, &p->addr, buf, sizeof buf);
        }
        broadcast_beacon();   /* quem está procurando salas já vê o nome novo */
    }
    push_event(RE5_LAN_EVT_PERSONA_CHANGE, 0, 0, g_local_steam_id, 0x0001 /* k_EPersonaChangeName */, 0);
    re5_mutex_unlock(&g_mu);
    RE5_INFO("steam_lan", "Perfil LAN: '%s #%u', pais %s", n, (uint32_t)g_local_steam_id, cc);
    return 1;
}

int steam_lan_set_manual_hosts(const char *list)
{
    re5_mutex_lock(&g_mu);
    parse_manual_hosts(list);
    int n = g_n_manual;
    if (g_identity_path[0]) write_identity(g_identity_path);
    re5_mutex_unlock(&g_mu);
    return n;
}

void steam_lan_get_manual_hosts(char *out, size_t cap)
{
    re5_mutex_lock(&g_mu);
    manual_hosts_text(out, cap);
    re5_mutex_unlock(&g_mu);
}

void steam_lan_get_status(re5_lan_status *st)
{
    memset(st, 0, sizeof *st);
    re5_mutex_lock(&g_mu);
    st->local_id = g_local_steam_id;
    snprintf(st->local_ip, sizeof st->local_ip, "%s", g_local_ip);
    st->p2p_port = g_p2p_port;
    st->lobbies_seen = g_num_lobbies;
    st->joining = g_join.active;
    st->in_lobby = g_has_active_lobby && !g_join.active;
    if (st->in_lobby) {
        uint32_t now = get_ticks_ms();
        st->is_host = g_active_lobby.owner_id == g_local_steam_id;
        st->max_members = g_active_lobby.max_members;
        for (int i = 0; i < g_active_lobby.member_count && i < RE5_LAN_MAX_LOBBY_MEMBERS; i++) {
            re5_lan_member_info *m = &st->members[st->member_count++];
            uint64_t id = g_active_lobby.members[i];
            m->steam_id = id;
            m->is_owner = id == g_active_lobby.owner_id;
            m->is_local = id == g_local_steam_id;
            m->rtt_ms = -1;
            if (m->is_local) {
                snprintf(m->name, sizeof m->name, "%.24s", g_local_name);
                memcpy(m->country, g_local_country, sizeof m->country);
            } else {
                lan_peer *p = peer_find(id);
                snprintf(m->name, sizeof m->name, "%.24s", p && p->name[0] ? p->name : g_active_lobby.member_names[i]);
                if (p) {
                    memcpy(m->country, p->country, sizeof m->country);
                    m->rtt_ms = p->rtt_ms;
                    m->silent_ms = now - p->last_seen_ms;
                }
            }
        }
    }
    re5_mutex_unlock(&g_mu);
}
