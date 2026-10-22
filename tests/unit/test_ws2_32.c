/*
 * ws2_32: WSAStartup (versões e WSADATA do x86), ordem de bytes, inet_addr
 * nas formas que o Windows aceita, inet_ntoa, nomes de uma máquina sem rede,
 * iphlpapi e os sockets (loopback de verdade, resto da rede inexistente,
 * WSAAsyncSelect postando na fila do user32).
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;
static uint32_t g_scratch, g_off;

static uint32_t call(const char *name, int n, ...)
{
    uint32_t args[8];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, re5_import_bind("ws2_32.dll", name, 0), n, args);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

static uint32_t gstr(const char *s)
{
    uint32_t va = g_scratch + g_off;
    memcpy(re5_g2h(va), s, strlen(s) + 1);
    g_off += (uint32_t)strlen(s) + 16u;
    return va;
}

static uint32_t k32(const char *name, int n, const uint32_t *a)
{
    return re5_guest_call(T, re5_import_bind("kernel32.dll", name, 0), n, a);
}

static void put_addr(uint32_t va, uint8_t a, uint8_t b, uint8_t c_, uint8_t d, uint16_t port)
{
    memset(re5_g2h(va), 0, 16);
    wr16(va, 2);                                   /* AF_INET */
    wr8(va + 2, (uint8_t)(port >> 8));
    wr8(va + 3, (uint8_t)port);
    wr8(va + 4, a);
    wr8(va + 5, b);
    wr8(va + 6, c_);
    wr8(va + 7, d);
}

/* Sockets: o loopback é de verdade (sockets do host); o resto da rede não existe. */
static void test_sockets(void)
{
    uint32_t m = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "sockets");
    uint32_t sa = m, len = m + 0x20, buf = m + 0x100, msg = m + 0x200, set = m + 0x300, tv = m + 0x400, v = m + 0x480;
    re5_guest_lock();   /* como código do guest; a thread do WSAAsyncSelect também pega o lock */

    uint32_t ls = call("socket", 3, 2u, 1u, 6u);
    CHECK(ls != 0xFFFFFFFFu);
    wr32(v, 1);
    CHECK_EQ_U32(call("setsockopt", 5, ls, 0xFFFFu, 4u, v, 4u), 0);   /* SO_REUSEADDR */
    put_addr(sa, 0, 0, 0, 0, 0);                                       /* INADDR_ANY:0 */
    CHECK_EQ_U32(call("bind", 3, ls, sa, 16u), 0);
    CHECK_EQ_U32(call("listen", 2, ls, 4u), 0);
    wr32(len, 16);
    CHECK_EQ_U32(call("getsockname", 3, ls, sa, len), 0);
    CHECK_EQ_U32(rd8(sa + 4), 127);                                    /* INADDR_ANY virou loopback */
    CHECK(rd16(sa + 2) != 0);
    CHECK_EQ_U32(call("WSAAsyncSelect", 4, ls, 0x10010u, 0x0401u, 0x08u), 0);   /* FD_ACCEPT */

    uint32_t cs = call("socket", 3, 2u, 1u, 6u);
    CHECK_EQ_U32(call("connect", 3, cs, sa, 16u), 0);
    uint32_t got = 0;
    const uint32_t pk[5] = {msg, 0, 0, 0, 1 /* PM_REMOVE */}, sl[2] = {10, 0};
    for (int i = 0; i < 300 && !got; i++) {
        if (re5_guest_call(T, re5_import_bind("user32.dll", "PeekMessageW", 0), 5, pk) && rd32(msg + 4) == 0x0401u)
            got = 1;
        else
            k32("SleepEx", 2, sl);   /* solta o lock: a thread de eventos anda */
    }
    CHECK(got);
    CHECK_EQ_U32(rd32(msg), 0x10010u);
    CHECK_EQ_U32(rd32(msg + 8), ls);       /* wParam: o socket */
    CHECK_EQ_U32(rd32(msg + 12), 0x08u);   /* lParam: FD_ACCEPT, erro 0 */
    uint32_t as = call("accept", 3, ls, 0u, 0u);
    CHECK(as != 0xFFFFFFFFu);

    memcpy(re5_g2h(buf), "hello", 5);
    CHECK_EQ_U32(call("send", 4, cs, buf, 5u, 0u), 5);
    wr32(set, 1);
    wr32(set + 4, as);
    wr32(tv, 2);
    wr32(tv + 4, 0);
    CHECK_EQ_U32(call("select", 5, 0u, set, 0u, 0u, tv), 1);
    CHECK_EQ_U32(call("__WSAFDIsSet", 2, as, set), 1);
    CHECK_EQ_U32(call("ioctlsocket", 3, as, 0x4004667Fu, v), 0);   /* FIONREAD */
    CHECK_EQ_U32(rd32(v), 5);
    memset(re5_g2h(buf), 0, 16);
    CHECK_EQ_U32(call("recv", 4, as, buf, 16u, 0u), 5);
    CHECK(memcmp(re5_g2h(buf), "hello", 5) == 0);
    wr32(v, 1);
    CHECK_EQ_U32(call("ioctlsocket", 3, as, 0x8004667Eu, v), 0);   /* FIONBIO */
    CHECK_EQ_U32(call("recv", 4, as, buf, 16u, 0u), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10035);                 /* WSAEWOULDBLOCK */
    wr32(len, 4);
    CHECK_EQ_U32(call("getsockopt", 5, cs, 0xFFFFu, 0x1008u, v, len), 0);   /* SO_TYPE */
    CHECK_EQ_U32(rd32(v), 1);                                              /* SOCK_STREAM */
    wr32(len, 16);
    CHECK_EQ_U32(call("getpeername", 3, as, sa + 0x40, len), 0);
    CHECK_EQ_U32(rd8(sa + 0x44), 127);

    /* UDP no loopback. */
    uint32_t u1 = call("socket", 3, 2u, 2u, 17u), u2 = call("socket", 3, 2u, 2u, 17u);
    put_addr(sa, 127, 0, 0, 1, 0);
    CHECK_EQ_U32(call("bind", 3, u1, sa, 16u), 0);
    wr32(len, 16);
    CHECK_EQ_U32(call("getsockname", 3, u1, sa, len), 0);
    memcpy(re5_g2h(buf), "dgram", 5);
    CHECK_EQ_U32(call("sendto", 6, u2, buf, 5u, 0u, sa, 16u), 5);
    wr32(len, 16);
    memset(re5_g2h(buf), 0, 16);
    CHECK_EQ_U32(call("recvfrom", 6, u1, buf, 16u, 0u, sa + 0x40, len), 5);
    CHECK(memcmp(re5_g2h(buf), "dgram", 5) == 0);
    CHECK_EQ_U32(rd8(sa + 0x44), 127);

    /* O resto da rede não existe. */
    uint32_t xs = call("socket", 3, 2u, 1u, 6u);
    put_addr(sa, 8, 8, 8, 8, 53);
    CHECK_EQ_U32(call("connect", 3, xs, sa, 16u), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10051);                  /* WSAENETUNREACH */
    put_addr(sa, 255, 255, 255, 255, 9999);
    CHECK_EQ_U32(call("sendto", 6, u2, buf, 5u, 0u, sa, 16u), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10051);
    put_addr(sa, 10, 0, 0, 1, 0);
    CHECK_EQ_U32(call("bind", 3, xs, sa, 16u), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10049);                  /* WSAEADDRNOTAVAIL */
    CHECK_EQ_U32(call("socket", 3, 23u, 1u, 0u), 0xFFFFFFFFu);        /* AF_INET6 */
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10047);

    const uint32_t all[7] = {ls, cs, as, u1, u2, xs, 0};
    for (int i = 0; all[i]; i++) CHECK_EQ_U32(call("closesocket", 1, all[i]), 0);
    CHECK_EQ_U32(call("closesocket", 1, ls), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10038);                  /* WSAENOTSOCK */
    re5_guest_unlock();
}

int main(void)
{
#if defined(_WIN32)
    _putenv("RE5_HEADLESS=1");
#else
    setenv("RE5_HEADLESS", "1", 1);
#endif
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    g_scratch = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    uint32_t wsa = g_scratch + 0x8000u;

    CHECK_EQ_U32(call("WSAStartup", 2, 0x0202u, wsa), 0);
    CHECK_EQ_U32(rd16(wsa), 0x0202);
    CHECK_EQ_U32(rd16(wsa + 2), 0x0202);
    CHECK(strcmp((const char *)re5_g2h(wsa + 4), "WinSock 2.0") == 0);
    CHECK(strcmp((const char *)re5_g2h(wsa + 261), "Running") == 0);
    CHECK_EQ_U32(call("WSAStartup", 2, 0x0101u, wsa), 0);
    CHECK_EQ_U32(rd16(wsa), 0x0101);
    CHECK_EQ_U32(call("WSAStartup", 2, 0x0303u, wsa), 0);
    CHECK_EQ_U32(rd16(wsa), 0x0202);                         /* o mais alto que existe */
    CHECK_EQ_U32(call("WSAStartup", 2, 0x0000u, wsa), 10092); /* WSAVERNOTSUPPORTED */
    CHECK_EQ_U32(call("WSACleanup", 0), 0);

    CHECK_EQ_U32(call("htons", 1, 0x1234u), 0x3412);
    CHECK_EQ_U32(call("ntohs", 1, 0xABCDu), 0xCDAB);

    CHECK_EQ_U32(call("inet_addr", 1, gstr("192.168.0.1")), 0x0100A8C0u);
    CHECK_EQ_U32(call("inet_addr", 1, gstr("10.1")), 0x0100000Au);        /* a.b: b nos 24 bits baixos */
    CHECK_EQ_U32(call("inet_addr", 1, gstr("0x7f.0.0.01")), 0x0100007Fu); /* hex e octal */
    CHECK_EQ_U32(call("inet_addr", 1, gstr("1.2.3.256")), 0xFFFFFFFFu);   /* INADDR_NONE */
    CHECK_EQ_U32(call("inet_addr", 1, gstr("abc")), 0xFFFFFFFFu);
    uint32_t s = call("inet_ntoa", 1, 0x0100A8C0u);
    CHECK(strcmp((const char *)re5_g2h(s), "192.168.0.1") == 0);

    /* Máquina sem rede: o próprio nome resolve para 127.0.0.1, o resto não. */
    uint32_t hn = g_scratch + 0x9000u;
    CHECK_EQ_U32(call("gethostname", 2, hn, 256u), 0);
    CHECK(strcmp((const char *)re5_g2h(hn), "RE5-PC") == 0);
    CHECK_EQ_U32(call("gethostname", 2, hn, 3u), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 10014);       /* WSAEFAULT */
    uint32_t he = call("gethostbyname", 1, gstr("re5-pc"));
    CHECK(he != 0);
    CHECK_EQ_U32(rd16(he + 8), 2);                          /* AF_INET */
    CHECK_EQ_U32(rd16(he + 10), 4);
    CHECK_EQ_U32(rd32(rd32(rd32(he + 12))), 0x0100007Fu);   /* h_addr_list[0] = 127.0.0.1 */
    CHECK_EQ_U32(rd32(rd32(he + 12) + 4), 0);
    CHECK_EQ_U32(call("gethostbyname", 1, gstr("www.capcom.com")), 0);
    CHECK_EQ_U32(call("WSAGetLastError", 0), 11001);       /* WSAHOST_NOT_FOUND */

    /* iphlpapi: só o loopback, sem MAC (o RE5 pula interfaces sem endereço físico). */
    uint32_t psize = g_scratch + 0x9100u, table = g_scratch + 0xA000u;
    wr32(psize, 0);
    const uint32_t q[3] = {0, psize, 0};
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("iphlpapi.dll", "GetIfTable", 0), 3, q), 122);
    CHECK_EQ_U32(rd32(psize), 864);   /* dwNumEntries + 1 MIB_IFROW de 860 bytes */
    const uint32_t g[3] = {table, psize, 0};
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("iphlpapi.dll", "GetIfTable", 0), 3, g), 0);
    CHECK_EQ_U32(rd32(table), 1);
    CHECK_EQ_U32(rd32(table + 4 + 516), 24);   /* loopback */
    CHECK_EQ_U32(rd32(table + 4 + 528), 0);    /* dwPhysAddrLen */
    CHECK_EQ_U32(rd16(table + 4), 'S');
    wr32(table + 4 + 512, 7);
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("iphlpapi.dll", "GetIfEntry", 0), 1, &(uint32_t){table + 4}), 13);

    test_sockets();
    CHECK_DONE();
}
