/*
 * re5_server.c - servidor de retransmissao (relay) do multiplayer LAN do RE5.
 *
 * Roda num prompt de comando no PC (nao precisa do jogo). Os jogadores apontam para ele com
 *     RE5_LAN_SERVER=IP:PORTA          (no re5_env.txt, no ambiente, ou "servidor=" no re5_lan.ini)
 * e passam a achar salas e a trocar pacotes P2P por ele, sem broadcast e sem abrir porta em cada
 * aparelho: so o servidor precisa de uma porta UDP alcancavel. Fechar o prompt derruba o servidor;
 * as salas em andamento perdem o parceiro (o jogo trata como queda do jogador).
 *
 * Protocolo (o mesmo cabecalho do steam_lan.c, runtime/src/win32/steam_lan.c):
 *   HELLO (0x20)  cliente -> servidor, 1x por segundo: registra o endereco publico do cliente.
 *   ACK   (0x21)  servidor -> cliente.
 *   RELAY (0x22)  cabecalho + u64 destino + pacote interno completo (cabecalho + corpo).
 *                 destino != 0: repassado ao cliente de destino; 0: e para o servidor, que guarda
 *                 os beacons de sala (inner BEACON 0x01) e responde aos PING (inner 0x02) com os
 *                 beacons vivos, tambem em RELAY.
 *
 * Uso: re5_server [--port N] [--quiet]        (porta padrao 27040)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#endif

#define MAGIC          0x35455221u   /* "!RE5" */
#define PKT_BEACON     0x01
#define PKT_PING       0x02
#define PKT_SRV_HELLO  0x20
#define PKT_SRV_ACK    0x21
#define PKT_RELAY      0x22
#define MAX_PKT        1600
#define MAX_CLIENTS    128
#define MAX_LOBBIES    32
#define CLIENT_TTL_MS  30000u
#define LOBBY_TTL_MS   4000u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type, flags;
    uint16_t port;
    uint64_t sender;
    uint32_t nonce;
} hdr_t;
#pragma pack(pop)

typedef struct {
    int                used;
    uint64_t           id;
    struct sockaddr_in addr;
    uint32_t           last_ms;
} client_t;

typedef struct {
    int      used;
    uint64_t host, lobby;
    int      len;
    uint8_t  pkt[MAX_PKT];
    uint32_t at_ms;
} lobby_t;

static client_t g_cl[MAX_CLIENTS];
static lobby_t  g_lb[MAX_LOBBIES];
static sock_t   g_sock = BAD_SOCK;
static int      g_quiet;
static uint64_t g_relayed, g_dropped;

static uint32_t now_ms(void)
{
#if defined(_WIN32)
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

static void stamp(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(out, cap, "%H:%M:%S", tm);
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void logf_(const char *fmt, ...)
{
    char ts[16];
    stamp(ts, sizeof ts);
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static client_t *client_find(uint64_t id)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_cl[i].used && g_cl[i].id == id) return &g_cl[i];
    return NULL;
}

static void client_touch(uint64_t id, const struct sockaddr_in *from, uint32_t now)
{
    client_t *c = client_find(id);
    if (!c) {
        for (int i = 0; i < MAX_CLIENTS && !c; i++)
            if (!g_cl[i].used) c = &g_cl[i];
        if (!c) return;   /* cheio: ignora */
        memset(c, 0, sizeof *c);
        c->used = 1;
        c->id = id;
        logf_("jogador #%u conectou (%s:%u)", (uint32_t)id, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    } else if (c->addr.sin_addr.s_addr != from->sin_addr.s_addr || c->addr.sin_port != from->sin_port) {
        logf_("jogador #%u mudou de endereco: %s:%u", (uint32_t)id, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    }
    c->addr = *from;
    c->last_ms = now;
}

static void send_to(const struct sockaddr_in *to, const void *data, size_t len)
{
    sendto(g_sock, (const char *)data, (int)len, 0, (const struct sockaddr *)to, sizeof *to);
}

static void send_ack(const struct sockaddr_in *to)
{
    hdr_t h = {MAGIC, PKT_SRV_ACK, 0, 0, 0, 0};
    send_to(to, &h, sizeof h);
}

/* Beacon de sala vindo de um host (RELAY com destino 0). */
static void lobby_store(uint64_t host, const uint8_t *inner, int len, uint32_t now)
{
    if (len < (int)sizeof(hdr_t) + 8 || len > MAX_PKT) return;
    uint64_t lobby;
    memcpy(&lobby, inner + sizeof(hdr_t), 8);
    lobby_t *l = NULL;
    for (int i = 0; i < MAX_LOBBIES; i++)
        if (g_lb[i].used && g_lb[i].host == host) l = &g_lb[i];
    if (!l) {
        for (int i = 0; i < MAX_LOBBIES && !l; i++)
            if (!g_lb[i].used) l = &g_lb[i];
        if (!l) return;
        l->used = 1;
        l->host = host;
        logf_("sala %llu aberta pelo host #%u", (unsigned long long)lobby, (uint32_t)host);
    }
    l->lobby = lobby;
    l->len = len;
    memcpy(l->pkt, inner, (size_t)len);
    l->at_ms = now;
}

static void answer_ping(const client_t *to, uint32_t now)
{
    for (int i = 0; i < MAX_LOBBIES; i++) {
        lobby_t *l = &g_lb[i];
        if (!l->used || l->host == to->id || now - l->at_ms > LOBBY_TTL_MS) continue;
        uint8_t buf[MAX_PKT + 64];
        hdr_t h = {MAGIC, PKT_RELAY, 0, 0, 0, 0};
        memcpy(buf, &h, sizeof h);
        memcpy(buf + sizeof h, &to->id, 8);
        memcpy(buf + sizeof h + 8, l->pkt, (size_t)l->len);
        send_to(&to->addr, buf, sizeof h + 8 + (size_t)l->len);
    }
}

static void handle(const uint8_t *buf, int n, const struct sockaddr_in *from, uint32_t now)
{
    if (n < (int)sizeof(hdr_t)) return;
    hdr_t h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != MAGIC || h.sender == 0) return;

    if (h.type == PKT_SRV_HELLO) {
        client_touch(h.sender, from, now);
        send_ack(from);
        return;
    }
    if (h.type != PKT_RELAY || n < (int)sizeof(hdr_t) + 8 + (int)sizeof(hdr_t)) return;
    client_touch(h.sender, from, now);

    uint64_t dest;
    memcpy(&dest, buf + sizeof h, 8);
    const uint8_t *inner = buf + sizeof h + 8;
    int ilen = n - (int)sizeof h - 8;
    hdr_t ih;
    memcpy(&ih, inner, sizeof ih);
    if (ih.magic != MAGIC || ih.type == PKT_RELAY || ih.sender != h.sender) {
        g_dropped++;
        return;
    }

    if (dest == 0) {   /* para o servidor */
        if (ih.type == PKT_BEACON) lobby_store(h.sender, inner, ilen, now);
        else if (ih.type == PKT_PING) {
            client_t *me = client_find(h.sender);
            if (me) answer_ping(me, now);
        }
        return;
    }
    client_t *to = client_find(dest);
    if (!to || now - to->last_ms > CLIENT_TTL_MS) {
        g_dropped++;
        return;
    }
    send_to(&to->addr, buf, (size_t)n);   /* o datagrama inteiro: o destino abre o RELAY */
    g_relayed++;
}

static void expire(uint32_t now)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_cl[i].used && now - g_cl[i].last_ms > CLIENT_TTL_MS) {
            logf_("jogador #%u saiu (sem sinal)", (uint32_t)g_cl[i].id);
            g_cl[i].used = 0;
        }
    for (int i = 0; i < MAX_LOBBIES; i++)
        if (g_lb[i].used && now - g_lb[i].at_ms > LOBBY_TTL_MS * 2u) {
            logf_("sala %llu fechada (host #%u)", (unsigned long long)g_lb[i].lobby, (uint32_t)g_lb[i].host);
            g_lb[i].used = 0;
        }
}

int main(int argc, char **argv)
{
    int port = 27040;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else {
            fprintf(stderr, "uso: re5_server [--port N] [--quiet]   (porta padrao 27040)\n");
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "porta invalida\n");
        return 2;
    }
#if defined(_WIN32)
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        fprintf(stderr, "WSAStartup falhou\n");
        return 1;
    }
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == BAD_SOCK) {
        fprintf(stderr, "nao foi possivel criar o socket UDP\n");
        return 1;
    }
#if defined(_WIN32)
    {   /* ICMP "porta inalcancavel" de um cliente que sumiu nao pode derrubar o recvfrom */
        DWORD ret = 0;
        BOOL off = FALSE;
        WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    }
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "a porta UDP %d esta em uso\n", port);
        return 1;
    }

    printf("RE5 - servidor de retransmissao do multiplayer\n");
    printf("Porta UDP %d. Nos jogadores, no re5_env.txt:\n    RE5_LAN_SERVER=<IP deste PC>:%d\n", port, port);
    printf("Se o Windows perguntar sobre o firewall, permita redes privadas. Feche esta janela para parar.\n\n");
    fflush(stdout);

    uint32_t last_expire = now_ms(), last_stat = last_expire;
    uint8_t buf[MAX_PKT + 64];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_sock, &fds);
        struct timeval tv = {0, 200000};
        int r = select((int)g_sock + 1, &fds, NULL, NULL, &tv);
        uint32_t now = now_ms();
        if (r > 0) {
            for (;;) {   /* esvazia o que chegou */
                struct sockaddr_in from;
#if defined(_WIN32)
                int fl = sizeof from;
#else
                socklen_t fl = sizeof from;
#endif
                fd_set one;
                struct timeval z = {0, 0};
                FD_ZERO(&one);
                FD_SET(g_sock, &one);
                if (select((int)g_sock + 1, &one, NULL, NULL, &z) <= 0) break;
                int n = (int)recvfrom(g_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0) continue;
                handle(buf, n, &from, now);
            }
        }
        if (now - last_expire > 1000u) {
            expire(now);
            last_expire = now;
        }
        if (!g_quiet && now - last_stat > 30000u) {
            int nc = 0, nl = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) nc += g_cl[i].used;
            for (int i = 0; i < MAX_LOBBIES; i++) nl += g_lb[i].used;
            logf_("%d jogador(es), %d sala(s), %llu pacotes repassados, %llu descartados", nc, nl,
                  (unsigned long long)g_relayed, (unsigned long long)g_dropped);
            last_stat = now;
        }
    }
}
