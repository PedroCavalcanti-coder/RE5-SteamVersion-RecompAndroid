/*
 * steam_lan.c - Subcamada de rede LAN para Steamworks (Matchmaking + Networking P2P).
 *
 * Implementa transporte UDP em broadcast e unicast direto entre nós LAN.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <unistd.h>
typedef int hsock_t;
#define HBAD_SOCK (-1)
#endif

#include "re5/runtime.h"
#include "re5/steam_lan.h"
#include "re5/thread.h"
#include "../core/host_sync.h"

#define RE5_LAN_MAGIC 0x35455221u /* "!RE5" */

enum {
    PKT_BEACON           = 0x01,
    PKT_PING             = 0x02,
    PKT_JOIN_REQ         = 0x03,
    PKT_JOIN_RESP        = 0x04,
    PKT_LEAVE            = 0x05,
    PKT_DATA_UPDATE      = 0x06,
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
} lan_hdr;

typedef struct {
    uint64_t lobby_id;
    uint64_t owner_id;
    uint16_t member_count;
    uint16_t max_members;
    char     owner_name[32];
    uint16_t data_count;
} lan_beacon_body;

typedef struct {
    uint64_t lobby_id;
    char     client_name[32];
} lan_join_req_body;

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
    struct sockaddr_in addr;
    uint32_t           last_seen_ms;
    int                p2p_session_requested;
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
static uint64_t g_join_call_id;

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
        else snprintf(p->name, sizeof p->name, "Player_%u", (uint32_t)steam_id);
    }
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
    h->magic = RE5_LAN_MAGIC;
    h->type = PKT_BEACON;
    h->flags = 0;
    h->port = g_p2p_port;
    h->sender_steam_id = g_local_steam_id;

    lan_beacon_body *b = (lan_beacon_body *)(buf + sizeof(lan_hdr));
    b->lobby_id = g_active_lobby.lobby_id;
    b->owner_id = g_active_lobby.owner_id;
    b->member_count = g_active_lobby.member_count;
    b->max_members = g_active_lobby.max_members;
    snprintf(b->owner_name, sizeof b->owner_name, "%s", g_active_lobby.owner_name);
    b->data_count = (uint16_t)g_active_lobby.data_count;

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
    peer_update(h->sender_steam_id, b->owner_name, &peer_p2p);

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
    snprintf(lb->owner_name, sizeof lb->owner_name, "%s", b->owner_name);
    lb->host_port = h->port;
    lb->host_ip = from->sin_addr;
    lb->member_count = b->member_count;
    lb->max_members = b->max_members;
    lb->members[0] = b->owner_id;
    snprintf(lb->member_names[0], sizeof lb->member_names[0], "%s", b->owner_name);
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
             (unsigned long long)b->lobby_id, b->owner_name, b->member_count, b->max_members);
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

static void handle_join_req(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    if (len < sizeof(lan_join_req_body)) return;
    const lan_join_req_body *req = (const lan_join_req_body *)payload;

    struct sockaddr_in client_addr = *from;
    client_addr.sin_port = htons(h->port);
    peer_update(h->sender_steam_id, req->client_name, &client_addr);

    if (!g_has_active_lobby || g_active_lobby.lobby_id != req->lobby_id) return;
    if (g_active_lobby.owner_id != g_local_steam_id) return;

    uint32_t resp_code = 1; /* Success */
    if (g_active_lobby.member_count >= g_active_lobby.max_members) {
        resp_code = 4; /* Full */
    } else {
        int idx = g_active_lobby.member_count++;
        g_active_lobby.members[idx] = h->sender_steam_id;
        snprintf(g_active_lobby.member_names[idx], sizeof g_active_lobby.member_names[idx], "%s", req->client_name);

        push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, g_active_lobby.lobby_id, h->sender_steam_id, 0x01 /* Entered */, 0);
        push_event(RE5_LAN_EVT_P2P_SESSION_REQUEST, 0, 0, h->sender_steam_id, 0, 0);
        lan_peer *p = peer_find(h->sender_steam_id);
        if (p) p->p2p_session_requested = 1;
        RE5_INFO("steam_lan", "Jogador '%s' (%llu) entrou no lobby", req->client_name, (unsigned long long)h->sender_steam_id);
    }

    uint8_t resp_buf[sizeof(lan_hdr) + sizeof(lan_join_resp_body)];
    lan_hdr *rh = (lan_hdr *)resp_buf;
    rh->magic = RE5_LAN_MAGIC;
    rh->type = PKT_JOIN_RESP;
    rh->flags = 0;
    rh->port = g_p2p_port;
    rh->sender_steam_id = g_local_steam_id;

    lan_join_resp_body *rb = (lan_join_resp_body *)(resp_buf + sizeof(lan_hdr));
    rb->lobby_id = g_active_lobby.lobby_id;
    rb->response_code = resp_code;
    rb->member_count = g_active_lobby.member_count;

    send_raw_udp(g_p2p_sock, &client_addr, resp_buf, sizeof resp_buf);
}

static void handle_join_resp(const lan_hdr *h, const uint8_t *payload, size_t len, const struct sockaddr_in *from)
{
    (void)from;
    if (len < sizeof(lan_join_resp_body)) return;
    const lan_join_resp_body *resp = (const lan_join_resp_body *)payload;

    if (!g_has_active_lobby || g_active_lobby.lobby_id != resp->lobby_id) return;

    if (resp->response_code == 1) {
        g_active_lobby.member_count = resp->member_count;
        g_active_lobby.members[1] = g_local_steam_id;
        snprintf(g_active_lobby.member_names[1], sizeof g_active_lobby.member_names[1], "%s", g_local_name);

        push_event(RE5_LAN_EVT_LOBBY_ENTERED, g_join_call_id, g_active_lobby.lobby_id, g_local_steam_id, 1 /* Success */, 0);
        push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, g_active_lobby.lobby_id, g_local_steam_id, 0x01 /* Entered */, 0);
        push_event(RE5_LAN_EVT_P2P_SESSION_REQUEST, 0, 0, h->sender_steam_id, 0, 0);
        lan_peer *p = peer_find(h->sender_steam_id);
        if (p) p->p2p_session_requested = 1;
        RE5_INFO("steam_lan", "Conectado ao lobby %llu com sucesso", (unsigned long long)resp->lobby_id);
    } else {
        push_event(RE5_LAN_EVT_LOBBY_ENTERED, g_join_call_id, g_active_lobby.lobby_id, g_local_steam_id, resp->response_code, 0);
        g_has_active_lobby = 0;
        RE5_INFO("steam_lan", "Falha ao entrar no lobby: codigo %u", resp->response_code);
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
                    snprintf(g_active_lobby.member_names[k], sizeof g_active_lobby.member_names[k], "%s", g_active_lobby.member_names[k + 1]);
                }
                g_active_lobby.member_count--;
                push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, lobby_id, h->sender_steam_id, 0x02 /* Left */, 0);
                break;
            }
        }
    } else if (h->sender_steam_id == g_active_lobby.owner_id) {
        /* Host fechou o lobby */
        push_event(RE5_LAN_EVT_LOBBY_CHAT_UPDATE, 0, lobby_id, h->sender_steam_id, 0x02 /* Left */, 0);
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

static void parse_packet(const uint8_t *buf, size_t len, const struct sockaddr_in *from)
{
    if (len < sizeof(lan_hdr)) return;
    const lan_hdr *h = (const lan_hdr *)buf;
    if (h->magic != RE5_LAN_MAGIC) return;
    if (h->sender_steam_id == g_local_steam_id) return; /* nosso próprio pacote */

    const uint8_t *payload = buf + sizeof(lan_hdr);
    size_t payload_len = len - sizeof(lan_hdr);

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
        if (now - last_beacon_ms >= 1000) {
            broadcast_beacon();
            last_beacon_ms = now;
        }
        re5_mutex_unlock(&g_mu);

        re5_host_sleep_ms(5);
    }
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

    /* Identidade SteamID */
    uint32_t account_id = 1;
    const char *env_id = getenv("RE5_STEAM_ID");
    if (!env_id || !*env_id) env_id = getenv("RE5_LAN_STEAM_ID");
    if (env_id && atoi(env_id) > 0) {
        account_id = (uint32_t)atoi(env_id);
    } else if (g_p2p_port >= RE5_LAN_DEFAULT_P2P_PORT) {
        account_id = 1 + (g_p2p_port - RE5_LAN_DEFAULT_P2P_PORT);
    } else {
        account_id = (uint32_t)g_p2p_port;
    }
    g_local_steam_id = ((uint64_t)0x01100001u << 32) | account_id;

    /* Nome do jogador */
    const char *env_name = getenv("RE5_PLAYER_NAME");
    if (!env_name || !*env_name) env_name = getenv("RE5_LAN_NAME");
    if (env_name && *env_name) {
        snprintf(g_local_name, sizeof g_local_name, "%s", env_name);
    } else if (account_id == 1) {
        snprintf(g_local_name, sizeof g_local_name, "Player 1");
    } else {
        snprintf(g_local_name, sizeof g_local_name, "Player %u", account_id);
    }

    g_running = 1;
    g_initialized = 1;

    RE5_INFO("steam_lan", "Subcamada LAN iniciada: SteamID %llu, Nome '%s', Porta P2P %u, Descoberta %u",
             (unsigned long long)g_local_steam_id, g_local_name, g_p2p_port, g_discovery_port);

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

const char *steam_lan_get_peer_name(uint64_t steam_id)
{
    re5_mutex_lock(&g_mu);
    lan_peer *p = peer_find(steam_id);
    const char *name = p ? p->name : "Player";
    re5_mutex_unlock(&g_mu);
    return name;
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

    uint64_t lobby_id = ((uint64_t)0x01180000u << 32) | (uint32_t)g_local_steam_id;
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

    push_event(RE5_LAN_EVT_LOBBY_CREATED, call_id, lobby_id, g_local_steam_id, 1 /* k_EResultOK */, 0);

    broadcast_beacon();
    re5_mutex_unlock(&g_mu);

    RE5_INFO("steam_lan", "Lobby %llu criado pelo host '%s' (max %u membros)", (unsigned long long)lobby_id, g_local_name, g_active_lobby.max_members);
    return lobby_id;
}

uint64_t steam_lan_request_lobby_list(uint64_t call_id)
{
    re5_mutex_lock(&g_mu);

    /* Envia PING broadcast para que hosts respondam imediatamente */
    lan_hdr ping;
    ping.magic = RE5_LAN_MAGIC;
    ping.type = PKT_PING;
    ping.flags = 0;
    ping.port = g_p2p_port;
    ping.sender_steam_id = g_local_steam_id;
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
    h->magic = RE5_LAN_MAGIC;
    h->type = PKT_DATA_UPDATE;
    h->flags = 0;
    h->port = g_p2p_port;
    h->sender_steam_id = g_local_steam_id;

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
        h->magic = RE5_LAN_MAGIC;
        h->type = PKT_DATA_UPDATE;
        h->flags = 0;
        h->port = g_p2p_port;
        h->sender_steam_id = g_local_steam_id;

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
    g_join_call_id = call_id;
    lan_lobby *lb = NULL;
    for (int i = 0; i < g_num_lobbies; i++) {
        if (g_lobbies[i].lobby_id == lobby_id) {
            lb = &g_lobbies[i];
            break;
        }
    }
    if (!lb) {
        push_event(RE5_LAN_EVT_LOBBY_ENTERED, call_id, lobby_id, g_local_steam_id, 2 /* DoesntExist */, 0);
        re5_mutex_unlock(&g_mu);
        return 0;
    }

    g_active_lobby = *lb;
    g_active_lobby.is_local = 0;
    g_has_active_lobby = 1;

    struct sockaddr_in host_addr;
    memset(&host_addr, 0, sizeof host_addr);
    host_addr.sin_family = AF_INET;
    host_addr.sin_port = htons(lb->host_port);
    host_addr.sin_addr = lb->host_ip;

    peer_update(lb->owner_id, lb->owner_name, &host_addr);

    uint8_t buf[sizeof(lan_hdr) + sizeof(lan_join_req_body)];
    lan_hdr *h = (lan_hdr *)buf;
    h->magic = RE5_LAN_MAGIC;
    h->type = PKT_JOIN_REQ;
    h->flags = 0;
    h->port = g_p2p_port;
    h->sender_steam_id = g_local_steam_id;

    lan_join_req_body *req = (lan_join_req_body *)(buf + sizeof(lan_hdr));
    req->lobby_id = lobby_id;
    snprintf(req->client_name, sizeof req->client_name, "%s", g_local_name);

    send_raw_udp(g_p2p_sock, &host_addr, buf, sizeof buf);
    re5_mutex_unlock(&g_mu);

    RE5_INFO("steam_lan", "Enviando requisicao de entrada para lobby %llu no host porta %u", (unsigned long long)lobby_id, lb->host_port);
    return 1;
}

void steam_lan_leave_lobby(uint64_t lobby_id)
{
    re5_mutex_lock(&g_mu);
    if (!g_has_active_lobby || g_active_lobby.lobby_id != lobby_id) {
        re5_mutex_unlock(&g_mu);
        return;
    }

    uint8_t buf[sizeof(lan_hdr) + sizeof(uint64_t)];
    lan_hdr *h = (lan_hdr *)buf;
    h->magic = RE5_LAN_MAGIC;
    h->type = PKT_LEAVE;
    h->flags = 0;
    h->port = g_p2p_port;
    h->sender_steam_id = g_local_steam_id;
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
    h->magic = RE5_LAN_MAGIC;
    h->type = PKT_P2P_DATA;
    h->flags = 0;
    h->port = g_p2p_port;
    h->sender_steam_id = g_local_steam_id;

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
