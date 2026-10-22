/*
 * ws2_32_sock.c - sockets do Winsock sobre os sockets do host.
 *
 * O resto do ws2_32 (ws2_32.c) descreve uma máquina sem rede, coerente com a
 * Steam offline: o nome do computador resolve para 127.0.0.1 e nada mais.
 * Os sockets seguem a mesma máquina: o loopback funciona de verdade (é um
 * socket do host), e o resto da rede não existe -
 *   - connect/sendto para fora de 127.0.0.0/8: WSAENETUNREACH;
 *   - bind em INADDR_ANY vira 127.0.0.1 (nada fica exposto na rede do host);
 *     bind num endereço específico que não é loopback: WSAEADDRNOTAVAIL.
 * O RE5 tem dois usuários (uma classe de socket em 0x0041C7xx e uma biblioteca
 * em 0x00D1Cxxx, esta com WSAAsyncSelect); os dois são do modo online.
 *
 * SOCKET do guest = handle de 32 bits (RE5_H_SOCKET). Chamadas que podem
 * bloquear soltam o lock do guest. WSAAsyncSelect: uma thread do host olha os
 * sockets registrados a cada 10 ms e posta as mensagens na fila do user32, com
 * a regra de rearme do Winsock (FD_READ volta com recv, FD_ACCEPT com accept,
 * FD_WRITE quando um send dá WSAEWOULDBLOCK; FD_CONNECT e FD_CLOSE uma vez).
 */
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "re5/runtime.h"
#include "re5/user32.h"
#include "../core/host_sync.h"

/* ---- códigos do Winsock ---- */

#define W_SOCKET_ERROR     0xFFFFFFFFu
#define W_INVALID_SOCKET   0xFFFFFFFFu
#define W_EINTR            10004u
#define W_EBADF            10009u
#define W_EACCES           10013u
#define W_EFAULT           10014u
#define W_EINVAL           10022u
#define W_EMFILE           10024u
#define W_EWOULDBLOCK      10035u
#define W_EINPROGRESS      10036u
#define W_EALREADY         10037u
#define W_ENOTSOCK         10038u
#define W_EDESTADDRREQ     10039u
#define W_EMSGSIZE         10040u
#define W_EPROTOTYPE       10041u
#define W_ENOPROTOOPT      10042u
#define W_EPROTONOSUPPORT  10043u
#define W_ESOCKTNOSUPPORT  10044u
#define W_EOPNOTSUPP       10045u
#define W_EAFNOSUPPORT     10047u
#define W_EADDRINUSE       10048u
#define W_EADDRNOTAVAIL    10049u
#define W_ENETDOWN         10050u
#define W_ENETUNREACH      10051u
#define W_ECONNABORTED     10053u
#define W_ECONNRESET       10054u
#define W_ENOBUFS          10055u
#define W_EISCONN          10056u
#define W_ENOTCONN         10057u
#define W_ETIMEDOUT        10060u
#define W_ECONNREFUSED     10061u
#define W_EHOSTUNREACH     10065u

#define W_FD_READ    0x01u
#define W_FD_WRITE   0x02u
#define W_FD_OOB     0x04u
#define W_FD_ACCEPT  0x08u
#define W_FD_CONNECT 0x10u
#define W_FD_CLOSE   0x20u

#define W_FIONBIO  0x8004667Eu
#define W_FIONREAD 0x4004667Fu

#define W_SOL_SOCKET   0xFFFFu
#define W_IPPROTO_TCP  6u
#define W_TCP_NODELAY  1u
#define W_SO_REUSEADDR 0x0004u
#define W_SO_KEEPALIVE 0x0008u
#define W_SO_BROADCAST 0x0020u
#define W_SO_LINGER    0x0080u
#define W_SO_DONTLINGER 0xFF7Fu
#define W_SO_SNDBUF    0x1001u
#define W_SO_RCVBUF    0x1002u
#define W_SO_SNDTIMEO  0x1005u
#define W_SO_RCVTIMEO  0x1006u
#define W_SO_ERROR     0x1007u
#define W_SO_TYPE      0x1008u

/* ---- host ---- */

#if defined(_WIN32)
typedef SOCKET hsock;
#define HBAD INVALID_SOCKET
static uint32_t host_err(void) { return (uint32_t)WSAGetLastError(); }   /* já é código do Winsock */
static void host_close(hsock h) { closesocket(h); }
static int host_nonblock(hsock h, int on)
{
    u_long v = (u_long)on;
    return ioctlsocket(h, FIONBIO, &v);
}
static void host_init(void)
{
    static int done;
    if (done) return;
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) re5_fatal("ws2_32", "WSAStartup do host falhou");
    done = 1;
}
#define SEND_FLAGS 0
#else
typedef int hsock;
#define HBAD (-1)
static uint32_t host_err(void)
{
    switch (errno) {
    case EINTR: return W_EINTR;
    case EBADF: return W_EBADF;
    case EACCES: return W_EACCES;
    case EFAULT: return W_EFAULT;
    case EINVAL: return W_EINVAL;
    case EMFILE: return W_EMFILE;
    case EAGAIN: return W_EWOULDBLOCK;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return W_EWOULDBLOCK;
#endif
    case EINPROGRESS: return W_EWOULDBLOCK;   /* connect sem bloqueio: o Winsock diz WSAEWOULDBLOCK */
    case EALREADY: return W_EALREADY;
    case ENOTSOCK: return W_ENOTSOCK;
    case EDESTADDRREQ: return W_EDESTADDRREQ;
    case EMSGSIZE: return W_EMSGSIZE;
    case EPROTOTYPE: return W_EPROTOTYPE;
    case ENOPROTOOPT: return W_ENOPROTOOPT;
    case EPROTONOSUPPORT: return W_EPROTONOSUPPORT;
    case EOPNOTSUPP: return W_EOPNOTSUPP;
    case EAFNOSUPPORT: return W_EAFNOSUPPORT;
    case EADDRINUSE: return W_EADDRINUSE;
    case EADDRNOTAVAIL: return W_EADDRNOTAVAIL;
    case ENETDOWN: return W_ENETDOWN;
    case ENETUNREACH: return W_ENETUNREACH;
    case ECONNABORTED: return W_ECONNABORTED;
    case ECONNRESET: case EPIPE: return W_ECONNRESET;
    case ENOBUFS: return W_ENOBUFS;
    case EISCONN: return W_EISCONN;
    case ENOTCONN: return W_ENOTCONN;
    case ETIMEDOUT: return W_ETIMEDOUT;
    case ECONNREFUSED: return W_ECONNREFUSED;
    case EHOSTUNREACH: return W_EHOSTUNREACH;
    default: re5_fatal("ws2_32", "errno %d do host sem código do Winsock", errno);
    }
}
static void host_close(hsock h) { close(h); }
static int host_nonblock(hsock h, int on)
{
    int f = fcntl(h, F_GETFL, 0);
    return fcntl(h, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
static void host_init(void) {}
#define SEND_FLAGS MSG_NOSIGNAL
#endif

/* ---- sockets do guest ---- */

typedef struct sock {
    hsock    h;
    uint32_t handle, type;
    int      nonblock, listening, connecting, closed_seen;
    uint32_t async_hwnd, async_msg, async_events, armed;
    struct sock *next;   /* lista do WSAAsyncSelect */
} sock;

static sock *g_async;
static int g_poll_started;

static void sock_destroy(void *p)
{
    sock *s = p;
    for (sock **q = &g_async; *q; q = &(*q)->next) {
        if (*q == s) {
            *q = s->next;
            break;
        }
    }
    host_close(s->h);
    free(s);
}

static sock *sock_of(uint32_t h)
{
    sock *s = re5_handle_get(h, RE5_H_SOCKET);
    if (!s) re5_set_last_error(W_ENOTSOCK);
    return s;
}

static uint32_t fail(uint32_t err)
{
    re5_set_last_error(err);
    return W_SOCKET_ERROR;
}

/* sockaddr_in do guest (16 bytes: família, porta, endereço, zeros) -> host. */
static int addr_from(uint32_t va, uint32_t len, struct sockaddr_in *a)
{
    if (!va || len < 16) return -1;
    memset(a, 0, sizeof *a);
    if (rd16(va) != AF_INET) return -2;
    a->sin_family = AF_INET;
    memcpy(&a->sin_port, re5_g2h(va + 2), 2);
    memcpy(&a->sin_addr, re5_g2h(va + 4), 4);
    return 0;
}

static void addr_to(const struct sockaddr_in *a, uint32_t va, uint32_t lenp)
{
    if (!va || !lenp) return;
    if (rd32(lenp) < 16) return;
    memset(re5_g2h(va), 0, 16);
    wr16(va, AF_INET);
    memcpy(re5_g2h(va + 2), &a->sin_port, 2);
    memcpy(re5_g2h(va + 4), &a->sin_addr, 4);
    wr32(lenp, 16);
}

static int is_loopback(const struct sockaddr_in *a) { return ((const uint8_t *)&a->sin_addr)[0] == 127; }
static int is_any(const struct sockaddr_in *a) { return a->sin_addr.s_addr == 0; }

static void w_socket(CPU *c)
{
    uint32_t af = re5_arg(c, 0), type = re5_arg(c, 1), proto = re5_arg(c, 2);
    host_init();
    if (af != AF_INET) { c->eax = fail(W_EAFNOSUPPORT); return; }
    if (type != 1 && type != 2) { c->eax = fail(W_ESOCKTNOSUPPORT); return; }
    hsock h = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, (int)proto);
    if (h == HBAD) { c->eax = fail(host_err()); return; }
    sock *s = calloc(1, sizeof *s);
    if (!s) re5_fatal("ws2_32", "sem memória");
    s->h = h;
    s->type = type;
    s->handle = re5_handle_new(RE5_H_SOCKET, s, sock_destroy);
    c->eax = s->handle;
}

static void w_closesocket(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    if (!sock_of(h)) { c->eax = W_SOCKET_ERROR; return; }
    re5_handle_close(h);
    c->eax = 0;
}

static void w_bind(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    struct sockaddr_in a;
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    int r = addr_from(re5_arg(c, 1), re5_arg(c, 2), &a);
    if (r) { c->eax = fail(r == -2 ? W_EAFNOSUPPORT : W_EFAULT); return; }
    if (is_any(&a)) a.sin_addr.s_addr = htonl(0x7F000001u);   /* INADDR_ANY: só o loopback existe */
    else if (!is_loopback(&a)) { c->eax = fail(W_EADDRNOTAVAIL); return; }
    c->eax = bind(s->h, (struct sockaddr *)&a, sizeof a) == 0 ? 0 : fail(host_err());
}

static void w_listen(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    if (listen(s->h, (int)re5_arg(c, 1)) != 0) { c->eax = fail(host_err()); return; }
    s->listening = 1;
    c->eax = 0;
}

static void w_connect(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    struct sockaddr_in a;
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    int r = addr_from(re5_arg(c, 1), re5_arg(c, 2), &a);
    if (r) { c->eax = fail(r == -2 ? W_EAFNOSUPPORT : W_EFAULT); return; }
    if (!is_loopback(&a)) { c->eax = fail(W_ENETUNREACH); return; }   /* máquina sem rede */
    int rc;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        rc = connect(s->h, (struct sockaddr *)&a, sizeof a);
        if (rc != 0) err = host_err();
        RE5_BLOCKING_END();
    }
    if (rc == 0) { c->eax = 0; return; }
    if (err == W_EWOULDBLOCK) s->connecting = 1;
    c->eax = fail(err);
}

static void w_accept(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_INVALID_SOCKET; return; }
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    hsock h;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        h = accept(s->h, (struct sockaddr *)&a, &al);
        if (h == HBAD) err = host_err();
        RE5_BLOCKING_END();
    }
    s->armed |= W_FD_ACCEPT;
    if (h == HBAD) { c->eax = fail(err); return; }
    sock *n = calloc(1, sizeof *n);
    if (!n) re5_fatal("ws2_32", "sem memória");
    n->h = h;
    n->type = s->type;
    n->handle = re5_handle_new(RE5_H_SOCKET, n, sock_destroy);
    if (s->async_events) {   /* o socket aceito herda o WSAAsyncSelect */
        host_nonblock(h, 1);
        n->nonblock = 1;
        n->async_hwnd = s->async_hwnd;
        n->async_msg = s->async_msg;
        n->async_events = s->async_events;
        n->armed = s->async_events & (W_FD_READ | W_FD_WRITE | W_FD_CLOSE | W_FD_OOB);
        n->next = g_async;
        g_async = n;
    }
    addr_to(&a, re5_arg(c, 1), re5_arg(c, 2));
    c->eax = n->handle;
}

static void w_send(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    uint32_t buf = re5_arg(c, 1), len = re5_arg(c, 2), flags = re5_arg(c, 3);
    long n;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        n = (long)send(s->h, re5_g2h(buf), (int)len, (int)flags | SEND_FLAGS);
        if (n < 0) err = host_err();
        RE5_BLOCKING_END();
    }
    if (n < 0) {
        if (err == W_EWOULDBLOCK) s->armed |= W_FD_WRITE;
        c->eax = fail(err);
        return;
    }
    c->eax = (uint32_t)n;
}

static void w_sendto(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    uint32_t buf = re5_arg(c, 1), len = re5_arg(c, 2), flags = re5_arg(c, 3), to = re5_arg(c, 4);
    struct sockaddr_in a;
    int has_to = to != 0;
    if (has_to) {
        int r = addr_from(to, re5_arg(c, 5), &a);
        if (r) { c->eax = fail(r == -2 ? W_EAFNOSUPPORT : W_EFAULT); return; }
        if (!is_loopback(&a)) { c->eax = fail(W_ENETUNREACH); return; }   /* broadcast inclusive */
    }
    long n;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        n = (long)sendto(s->h, re5_g2h(buf), (int)len, (int)flags | SEND_FLAGS,
                         has_to ? (struct sockaddr *)&a : NULL, has_to ? (socklen_t)sizeof a : 0);
        if (n < 0) err = host_err();
        RE5_BLOCKING_END();
    }
    if (n < 0) {
        if (err == W_EWOULDBLOCK) s->armed |= W_FD_WRITE;
        c->eax = fail(err);
        return;
    }
    c->eax = (uint32_t)n;
}

static void w_recv(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    uint32_t buf = re5_arg(c, 1), len = re5_arg(c, 2), flags = re5_arg(c, 3);
    long n;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        n = (long)recv(s->h, re5_g2h(buf), (int)len, (int)flags);
        if (n < 0) err = host_err();
        RE5_BLOCKING_END();
    }
    s->armed |= W_FD_READ;
    c->eax = n < 0 ? fail(err) : (uint32_t)n;
}

static void w_recvfrom(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    uint32_t buf = re5_arg(c, 1), len = re5_arg(c, 2), flags = re5_arg(c, 3);
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    long n;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        n = (long)recvfrom(s->h, re5_g2h(buf), (int)len, (int)flags, (struct sockaddr *)&a, &al);
        if (n < 0) err = host_err();
        RE5_BLOCKING_END();
    }
    s->armed |= W_FD_READ;
    if (n < 0) { c->eax = fail(err); return; }
    addr_to(&a, re5_arg(c, 4), re5_arg(c, 5));
    c->eax = (uint32_t)n;
}

/* fd_set do guest: u_int fd_count; SOCKET fd_array[64] (SOCKET de 32 bits). */
#define W_FD_SETSIZE 64u

typedef struct {
    sock *s[W_FD_SETSIZE];
    uint32_t n;
} gset;

static int gset_read(uint32_t va, gset *g)
{
    g->n = 0;
    if (!va) return 0;
    uint32_t n = rd32(va);
    if (n > W_FD_SETSIZE) return -1;
    for (uint32_t i = 0; i < n; i++) {
        sock *s = re5_handle_get(rd32(va + 4 + 4 * i), RE5_H_SOCKET);
        if (!s) return -1;
        g->s[g->n++] = s;
    }
    return 0;
}

static void w_select(CPU *c)
{
    uint32_t rv = re5_arg(c, 1), wv = re5_arg(c, 2), ev = re5_arg(c, 3), tv = re5_arg(c, 4);
    gset g[3];
    if (gset_read(rv, &g[0]) || gset_read(wv, &g[1]) || gset_read(ev, &g[2])) {
        c->eax = fail(W_ENOTSOCK);
        return;
    }
    if (!g[0].n && !g[1].n && !g[2].n) { c->eax = fail(W_EINVAL); return; }   /* como o Winsock */
    fd_set hs[3];
    int maxfd = 0;
    for (int k = 0; k < 3; k++) {
        FD_ZERO(&hs[k]);
        for (uint32_t i = 0; i < g[k].n; i++) {
            FD_SET(g[k].s[i]->h, &hs[k]);
#if !defined(_WIN32)
            if (g[k].s[i]->h > maxfd) maxfd = g[k].s[i]->h;
#endif
        }
    }
    struct timeval t, *pt = NULL;
    if (tv) {
        t.tv_sec = (long)(int32_t)rd32(tv);
        t.tv_usec = (long)(int32_t)rd32(tv + 4);
        pt = &t;
    }
    int r;
    uint32_t err = 0;
    {
        RE5_BLOCKING_BEGIN();
        r = select(maxfd + 1, &hs[0], &hs[1], &hs[2], pt);
        if (r < 0) err = host_err();
        RE5_BLOCKING_END();
    }
    if (r < 0) { c->eax = fail(err); return; }
    uint32_t total = 0, va[3] = {rv, wv, ev};
    for (int k = 0; k < 3; k++) {   /* o Winsock deixa no conjunto só os prontos */
        if (!va[k]) continue;
        uint32_t m = 0;
        for (uint32_t i = 0; i < g[k].n; i++)
            if (FD_ISSET(g[k].s[i]->h, &hs[k])) wr32(va[k] + 4 + 4 * m++, g[k].s[i]->handle);
        wr32(va[k], m);
        total += m;
    }
    c->eax = total;
}

static void w_WSAFDIsSet(CPU *c)
{
    uint32_t h = re5_arg(c, 0), set = re5_arg(c, 1), n = set ? rd32(set) : 0;
    c->eax = 0;
    for (uint32_t i = 0; i < n && i < W_FD_SETSIZE; i++)
        if (rd32(set + 4 + 4 * i) == h) c->eax = 1;
}

static void w_ioctlsocket(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    uint32_t cmd = re5_arg(c, 1), argp = re5_arg(c, 2);
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    if (!argp) { c->eax = fail(W_EFAULT); return; }
    if (cmd == W_FIONBIO) {
        int on = rd32(argp) != 0;
        if (!on && s->async_events) { c->eax = fail(W_EINVAL); return; }   /* WSAAsyncSelect exige sem bloqueio */
        if (host_nonblock(s->h, on) != 0) { c->eax = fail(host_err()); return; }
        s->nonblock = on;
        c->eax = 0;
        return;
    }
    if (cmd == W_FIONREAD) {
#if defined(_WIN32)
        u_long v = 0;
        int r = ioctlsocket(s->h, FIONREAD, &v);
#else
        int v = 0;
        int r = ioctl(s->h, FIONREAD, &v);
#endif
        if (r != 0) { c->eax = fail(host_err()); return; }
        wr32(argp, (uint32_t)v);
        c->eax = 0;
        return;
    }
    re5_fatal("ws2_32", "ioctlsocket(0x%08X) sem implementação", cmd);
}

/* Opções: no Windows os valores e formatos são os do guest; no POSIX traduzidos. */
static int opt_map(uint32_t level, uint32_t name, int *hl, int *hn, int *kind)
{
    *kind = 0;   /* 0 = inteiro/BOOL, 1 = linger, 2 = tempo em ms, 3 = SO_DONTLINGER */
    if (level == W_IPPROTO_TCP && name == W_TCP_NODELAY) { *hl = IPPROTO_TCP; *hn = TCP_NODELAY; return 0; }
    if (level != W_SOL_SOCKET) return -1;
    *hl = SOL_SOCKET;
    switch (name) {
    case W_SO_REUSEADDR: *hn = SO_REUSEADDR; return 0;
    case W_SO_KEEPALIVE: *hn = SO_KEEPALIVE; return 0;
    case W_SO_BROADCAST: *hn = SO_BROADCAST; return 0;
    case W_SO_SNDBUF: *hn = SO_SNDBUF; return 0;
    case W_SO_RCVBUF: *hn = SO_RCVBUF; return 0;
    case W_SO_ERROR: *hn = SO_ERROR; return 0;
    case W_SO_TYPE: *hn = SO_TYPE; return 0;
    case W_SO_LINGER: *hn = SO_LINGER; *kind = 1; return 0;
    case W_SO_DONTLINGER: *hn = SO_LINGER; *kind = 3; return 0;
    case W_SO_SNDTIMEO: *hn = SO_SNDTIMEO; *kind = 2; return 0;
    case W_SO_RCVTIMEO: *hn = SO_RCVTIMEO; *kind = 2; return 0;
    default: return -1;
    }
}

static void w_setsockopt(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    uint32_t level = re5_arg(c, 1), name = re5_arg(c, 2), val = re5_arg(c, 3), len = re5_arg(c, 4);
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    int hl, hn, kind;
    if (opt_map(level, name, &hl, &hn, &kind) != 0)
        re5_fatal("ws2_32", "setsockopt(nível 0x%X, opção 0x%X) sem implementação", level, name);
    if (!val || len < (kind == 1 ? 4u : kind == 3 ? 1u : 1u)) { c->eax = fail(W_EFAULT); return; }
    int r;
#if defined(_WIN32)
    (void)kind;
    r = setsockopt(s->h, hl, hn, re5_g2h(val), (int)len);
#else
    if (kind == 1) {
        struct linger l = {rd16(val), rd16(val + 2)};
        r = setsockopt(s->h, hl, hn, &l, sizeof l);
    } else if (kind == 3) {
        struct linger l = {rd32(val) ? 0 : 1, 0};
        r = setsockopt(s->h, hl, hn, &l, sizeof l);
    } else if (kind == 2) {
        uint32_t ms = rd32(val);
        struct timeval t = {(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
        r = setsockopt(s->h, hl, hn, &t, sizeof t);
    } else {
        int v = len >= 4 ? (int)rd32(val) : rd8(val);
        r = setsockopt(s->h, hl, hn, &v, sizeof v);
    }
#endif
    c->eax = r == 0 ? 0 : fail(host_err());
}

static void w_getsockopt(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    uint32_t level = re5_arg(c, 1), name = re5_arg(c, 2), val = re5_arg(c, 3), lenp = re5_arg(c, 4);
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    int hl, hn, kind;
    if (opt_map(level, name, &hl, &hn, &kind) != 0)
        re5_fatal("ws2_32", "getsockopt(nível 0x%X, opção 0x%X) sem implementação", level, name);
    if (!val || !lenp || rd32(lenp) < 4) { c->eax = fail(W_EFAULT); return; }
#if defined(_WIN32)
    int l = (int)rd32(lenp);
    int r = getsockopt(s->h, hl, hn, re5_g2h(val), &l);
    if (r == 0) wr32(lenp, (uint32_t)l);
#else
    int r;
    if (kind == 1 || kind == 3) {
        struct linger lg;
        socklen_t l = sizeof lg;
        r = getsockopt(s->h, hl, hn, &lg, &l);
        if (r == 0 && kind == 1) { wr16(val, (uint16_t)lg.l_onoff); wr16(val + 2, (uint16_t)lg.l_linger); }
        if (r == 0 && kind == 3) wr32(val, lg.l_onoff ? 0 : 1);
    } else if (kind == 2) {
        struct timeval t;
        socklen_t l = sizeof t;
        r = getsockopt(s->h, hl, hn, &t, &l);
        if (r == 0) wr32(val, (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000));
    } else {
        int v = 0;
        socklen_t l = sizeof v;
        r = getsockopt(s->h, hl, hn, &v, &l);
        if (r == 0 && hn == SO_ERROR && v) {   /* errno do host -> código do Winsock */
            int saved = errno;
            errno = v;
            v = (int)host_err();
            errno = saved;
        }
        if (r == 0 && hn == SO_TYPE) v = v == SOCK_STREAM ? 1 : 2;
        if (r == 0) wr32(val, (uint32_t)v);
    }
    if (r == 0) wr32(lenp, 4);
#endif
    c->eax = r == 0 ? 0 : fail(host_err());
}

static void name_of(CPU *c, int peer)
{
    sock *s = sock_of(re5_arg(c, 0));
    uint32_t va = re5_arg(c, 1), lenp = re5_arg(c, 2);
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    if (!va || !lenp || rd32(lenp) < 16) { c->eax = fail(W_EFAULT); return; }
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int r = peer ? getpeername(s->h, (struct sockaddr *)&a, &al) : getsockname(s->h, (struct sockaddr *)&a, &al);
    if (r != 0) { c->eax = fail(host_err()); return; }
    addr_to(&a, va, lenp);
    c->eax = 0;
}

static void w_getsockname(CPU *c) { name_of(c, 0); }
static void w_getpeername(CPU *c) { name_of(c, 1); }

/* ---- WSAAsyncSelect ---- */

static void post(sock *s, uint32_t ev, uint32_t err)
{
    s->armed &= ~ev;
    re5_user32_post(s->async_hwnd, s->async_msg, s->handle, (err << 16) | ev);
}

/* Uma volta: o que cada socket registrado tem de novo (com o lock do guest). */
static void async_poll_once(void)
{
    for (sock *s = g_async; s; s = s->next) {
        if (!s->async_events) continue;
        fd_set r, w, e;
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s->h, &r);
        FD_SET(s->h, &w);
        FD_SET(s->h, &e);
        struct timeval t = {0, 0};
#if defined(_WIN32)
        int n = select(0, &r, &w, &e, &t);
#else
        int n = select(s->h + 1, &r, &w, &e, &t);
#endif
        if (n <= 0) continue;
        uint32_t want = s->async_events & s->armed;
        if (s->connecting && (FD_ISSET(s->h, &w) || FD_ISSET(s->h, &e))) {
            int v = 0;
            socklen_t l = sizeof v;
            getsockopt(s->h, SOL_SOCKET, SO_ERROR, (char *)&v, &l);
#if !defined(_WIN32)
            if (v) { errno = v; v = (int)host_err(); }
#endif
            s->connecting = 0;
            if (want & W_FD_CONNECT) post(s, W_FD_CONNECT, (uint32_t)v);
            if (v) continue;
            want = s->async_events & s->armed;
        }
        if (s->listening) {
            if ((want & W_FD_ACCEPT) && FD_ISSET(s->h, &r)) post(s, W_FD_ACCEPT, 0);
            continue;
        }
        if (FD_ISSET(s->h, &r)) {
            char b;
            long k = s->type == 1 ? (long)recv(s->h, &b, 1, MSG_PEEK) : 1;
            if (k > 0) {
                if (want & W_FD_READ) post(s, W_FD_READ, 0);
            } else if (!s->closed_seen) {   /* fim do fluxo ou erro: FD_CLOSE uma vez */
                s->closed_seen = 1;
                if (s->async_events & W_FD_CLOSE) post(s, W_FD_CLOSE, k == 0 ? 0 : host_err());
            }
        }
        if ((want & W_FD_WRITE) && FD_ISSET(s->h, &w) && !s->connecting) post(s, W_FD_WRITE, 0);
    }
}

static void async_thread(void *arg)
{
    (void)arg;
    for (;;) {
        re5_host_sleep_ms(10);
        re5_guest_lock();
        async_poll_once();
        re5_guest_unlock();
    }
}

static void w_WSAAsyncSelect(CPU *c)
{
    sock *s = sock_of(re5_arg(c, 0));
    if (!s) { c->eax = W_SOCKET_ERROR; return; }
    uint32_t hwnd = re5_arg(c, 1), msg = re5_arg(c, 2), ev = re5_arg(c, 3);
    if (host_nonblock(s->h, 1) != 0) { c->eax = fail(host_err()); return; }
    s->nonblock = 1;
    int listed = 0;
    for (sock *q = g_async; q; q = q->next) listed |= q == s;
    s->async_hwnd = hwnd;
    s->async_msg = msg;
    s->async_events = ev;
    s->armed = ev;   /* novo registro: tudo armado (FD_WRITE sai já se der para escrever) */
    if (!listed && ev) {
        s->next = g_async;
        g_async = s;
    }
    if (ev && !g_poll_started) {
        if (re5_host_thread_start(async_thread, NULL, 1u << 20) != 0)
            re5_fatal("ws2_32", "WSAAsyncSelect: não consegui criar a thread de eventos");
        g_poll_started = 1;
    }
    c->eax = 0;
}

static const re5_import_def k_defs[] = {
    {"WSAAsyncSelect", w_WSAAsyncSelect, 16, 0},
    {"__WSAFDIsSet",   w_WSAFDIsSet,      8, 0},
    {"accept",         w_accept,         12, 0},
    {"bind",           w_bind,           12, 0},
    {"closesocket",    w_closesocket,     4, 0},
    {"connect",        w_connect,        12, 0},
    {"getpeername",    w_getpeername,    12, 0},
    {"getsockname",    w_getsockname,    12, 0},
    {"getsockopt",     w_getsockopt,     20, 0},
    {"ioctlsocket",    w_ioctlsocket,    12, 0},
    {"listen",         w_listen,          8, 0},
    {"recv",           w_recv,           16, 0},
    {"recvfrom",       w_recvfrom,       24, 0},
    {"select",         w_select,         20, 0},
    {"send",           w_send,           16, 0},
    {"sendto",         w_sendto,         24, 0},
    {"setsockopt",     w_setsockopt,     20, 0},
    {"socket",         w_socket,         12, 0},
    {0},
};

const re5_import_module re5_mod_ws2_32_sock = {"ws2_32.dll", k_defs};
