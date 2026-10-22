/*
 * ws2_32.c - Winsock 2 (o que não depende de socket).
 *
 * O RE5 chama WSAStartup no início. Por enquanto estão aqui o estado do
 * Winsock, as funções puras (ordem de bytes, conversão de endereço) e a
 * resolução de nomes de uma máquina sem rede, coerente com a Steam offline
 * (etapa 3.7): o nome do computador é fixo e só ele e "localhost" resolvem
 * (127.0.0.1); qualquer outro nome dá WSAHOST_NOT_FOUND, como num PC
 * desconectado. Os sockets (socket, connect, select...) continuam sem handler:
 * abortam com o nome na primeira chamada (D8) e viram uma ponte para os
 * sockets do host quando o jogo pedir.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/runtime.h"

#define WSAVERNOTSUPPORTED 10092u
#define WSAEFAULT          10014u
#define WSAHOST_NOT_FOUND  11001u
#define INADDR_NONE_       0xFFFFFFFFu
#define SOCKET_ERROR_      0xFFFFFFFFu
#define HOSTNAME           "RE5-PC"

/* WSADATA (x86): wVersion, wHighVersion, szDescription[257], szSystemStatus[129],
 * iMaxSockets, iMaxUdpDg, lpVendorInfo (alinhado em 396) = 400 bytes. */
#define WSADATA_SIZE 400u

static int g_started;

static void w_WSAStartup(CPU *c)
{
    uint32_t req = re5_arg(c, 0) & 0xFFFFu, data = re5_arg(c, 1);
    uint8_t major = (uint8_t)req, minor = (uint8_t)(req >> 8);
    if (!data) {
        c->eax = WSAEFAULT;
        return;
    }
    if (major < 1 || (major == 1 && minor < 1)) {
        c->eax = WSAVERNOTSUPPORTED;
        return;
    }
    uint16_t ver = major > 2 || (major == 2 && minor >= 2) ? 0x0202 : (uint16_t)req;
    memset(re5_g2h(data), 0, WSADATA_SIZE);
    wr16(data, ver);
    wr16(data + 2, 0x0202);
    memcpy(re5_g2h(data + 4), "WinSock 2.0", 12);
    memcpy(re5_g2h(data + 261), "Running", 8);
    g_started++;
    RE5_INFO("ws2_32", "WSAStartup(%u.%u) -> %u.%u", major, minor, ver & 0xFF, ver >> 8);
    c->eax = 0;
}

static void w_WSACleanup(CPU *c)
{
    if (g_started) g_started--;
    c->eax = 0;
}

/* No Windows o WSAGetLastError é o GetLastError da thread. */
static void w_WSAGetLastError(CPU *c) { c->eax = re5_get_last_error(); }

static void w_htons(CPU *c)
{
    uint32_t v = re5_arg(c, 0) & 0xFFFFu;
    c->eax = (v >> 8) | ((v & 0xFFu) << 8);
}

/* inet_addr: "a.b.c.d", "a.b.c", "a.b" ou "a", cada parte em decimal, octal
 * (0...) ou hexadecimal (0x...), como o do Windows. Resultado em ordem de rede. */
static void w_inet_addr(CPU *c)
{
    const char *s = re5_gstr(re5_arg(c, 0));
    c->eax = INADDR_NONE_;
    if (!s) return;
    uint32_t part[4];
    int n = 0;
    while (*s == ' ') s++;
    for (;;) {
        if (!isdigit((unsigned char)*s) || n == 4) return;
        char *end;
        unsigned long v = strtoul(s, &end, 0);
        if (end == s || v > 0xFFFFFFFFul) return;
        part[n++] = (uint32_t)v;
        s = end;
        if (*s != '.') break;
        s++;
    }
    if (*s && *s != ' ') return;
    uint32_t host;
    switch (n) {
    case 1: host = part[0]; break;
    case 2: if (part[0] > 255 || part[1] > 0xFFFFFF) return; host = part[0] << 24 | part[1]; break;
    case 3:
        if (part[0] > 255 || part[1] > 255 || part[2] > 0xFFFF) return;
        host = part[0] << 24 | part[1] << 16 | part[2];
        break;
    default:
        if (part[0] > 255 || part[1] > 255 || part[2] > 255 || part[3] > 255) return;
        host = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
        break;
    }
    c->eax = (host >> 24) | ((host >> 8) & 0xFF00u) | ((host << 8) & 0xFF0000u) | (host << 24);
}

/* inet_ntoa(struct in_addr) - o endereço vai por valor (4 bytes, ordem de rede);
 * a string fica num buffer do runtime no guest, como o buffer por thread do Windows. */
static uint32_t scratch(void);

static void w_inet_ntoa(CPU *c)
{
    uint32_t buf = scratch(), a = re5_arg(c, 0);
    snprintf(re5_g2h(buf), 16, "%u.%u.%u.%u", a & 0xFFu, (a >> 8) & 0xFFu, (a >> 16) & 0xFFu, a >> 24);
    c->eax = buf;
}

/* Buffer do runtime no guest para inet_ntoa/gethostbyname (por thread no Windows). */
static uint32_t scratch(void)
{
    static uint32_t buf;
    if (!buf) buf = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "ws2_32");
    if (!buf) re5_fatal("ws2_32", "sem memória do guest");
    return buf;
}

static void w_gethostname(CPU *c)
{
    uint32_t name = re5_arg(c, 0), len = re5_arg(c, 1);
    if (!name || len < sizeof HOSTNAME) {
        re5_set_last_error(WSAEFAULT);
        c->eax = SOCKET_ERROR_;
        return;
    }
    memcpy(re5_g2h(name), HOSTNAME, sizeof HOSTNAME);
    c->eax = 0;
}

static int streq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* hostent (x86): h_name, h_aliases, h_addrtype (AF_INET), h_length (4), h_addr_list. */
static void w_gethostbyname(CPU *c)
{
    const char *name = re5_gstr(re5_arg(c, 0));
    if (!name || !(streq_ci(name, HOSTNAME) || streq_ci(name, "localhost"))) {
        RE5_INFO("ws2_32", "gethostbyname(\"%s\"): WSAHOST_NOT_FOUND (sem rede)", name ? name : "");
        re5_set_last_error(WSAHOST_NOT_FOUND);
        c->eax = 0;
        return;
    }
    uint32_t b = scratch() + 0x100u, he = b, aliases = b + 16, list = b + 20, addr = b + 28, hname = b + 32;
    memcpy(re5_g2h(hname), name, strlen(name) + 1);
    wr32(he, hname);
    wr32(he + 4, aliases);
    wr16(he + 8, 2);   /* AF_INET */
    wr16(he + 10, 4);
    wr32(he + 12, list);
    wr32(aliases, 0);
    wr32(list, addr);
    wr32(list + 4, 0);
    wr32(addr, 0x0100007Fu);   /* 127.0.0.1 em ordem de rede */
    c->eax = he;
}

static const re5_import_def k_defs[] = {
    {"WSACleanup",      w_WSACleanup,      0, 0},
    {"WSAGetLastError", w_WSAGetLastError, 0, 0},
    {"WSAStartup",      w_WSAStartup,      8, 0},
    {"gethostbyname",   w_gethostbyname,   4, 0},
    {"gethostname",     w_gethostname,     8, 0},
    {"htons",           w_htons,           4, 0},
    {"inet_addr",       w_inet_addr,       4, 0},
    {"inet_ntoa",       w_inet_ntoa,       4, 0},
    {"ntohs",           w_htons,           4, 0},
    {0},
};

const re5_import_module re5_mod_ws2_32 = {"ws2_32.dll", k_defs};
