/*
 * bt_link.c - enlace Bluetooth/TCP ponto a ponto (ver re5/bt_link.h).
 *
 * Duas threads: uma conecta/aceita e le quadros; outra escreve a fila de saida. Cai o enlace, as duas
 * voltam a tentar a cada 2 s. Os quadros entram e saem por filas pequenas: o jogo nunca espera o radio.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2bth.h>
typedef SOCKET hsock_t;
#define HBAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int hsock_t;
#define HBAD_SOCK (-1)
#define sock_close close
#endif
#if defined(__ANDROID__)
#include <SDL2/SDL.h>
#include <jni.h>
#endif

#include "re5/bt_link.h"
#include "re5/log.h"
#include "re5/runtime.h"
#include "re5/thread.h"
#include "../core/host_sync.h"

#define QN 96   /* quadros por fila */

typedef struct {
    uint16_t len;
    uint8_t  d[RE5_BT_MAX_FRAME];
} frame_t;

static re5_mutex  g_mu;
static frame_t    g_rx[QN], g_tx[QN];
static int        g_rx_h, g_rx_n, g_tx_h, g_tx_n;
static volatile int g_run, g_up;
static int        g_started;
static char       g_cfg[128];

/* ---- backend de sockets (TCP e Winsock Bluetooth): quadro = u16 big-endian + dados ---- */

static hsock_t g_conn = HBAD_SOCK;
static hsock_t g_listen = HBAD_SOCK;

static int recv_all(hsock_t s, uint8_t *p, size_t n)
{
    while (n) {
        int r = (int)recv(s, (char *)p, (int)n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int send_all(hsock_t s, const uint8_t *p, size_t n)
{
    while (n) {
        int r = (int)send(s, (const char *)p, (int)n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int wait_readable(hsock_t s, int ms)
{
    fd_set f;
    FD_ZERO(&f);
    FD_SET(s, &f);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select((int)s + 1, &f, NULL, NULL, &tv) > 0;
}

#if defined(_WIN32)
static const GUID k_svc = {0x5e5b7c1a, 0x52e5, 0x4c4f, {0x9a, 0x3b, 0x1d, 0x5e, 0x0a, 0x5e, 0x50, 0x01}};

/* "AA:BB:CC:DD:EE:FF" -> BTH_ADDR. */
static int parse_mac(const char *s, BTH_ADDR *out)
{
    unsigned b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return 0;
    BTH_ADDR a = 0;
    for (int i = 0; i < 6; i++) a = (a << 8) | (b[i] & 0xFF);
    *out = a;
    return 1;
}

/* Aparelho pareado/conhecido cujo nome contem `want` (sem diferenciar maiusculas). */
static int find_device(const char *want, BTH_ADDR *out)
{
    WSAQUERYSETW q;
    memset(&q, 0, sizeof q);
    q.dwSize = sizeof q;
    q.dwNameSpace = NS_BTH;
    HANDLE h;
    if (WSALookupServiceBeginW(&q, LUP_CONTAINERS, &h) != 0) return 0;
    int found = 0;
    char lw[128];
    snprintf(lw, sizeof lw, "%s", want);
    for (char *c = lw; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (;;) {
        union { WSAQUERYSETW r; char raw[4096]; } u;
        DWORD len = sizeof u;
        memset(&u, 0, sizeof u);
        if (WSALookupServiceNextW(h, LUP_RETURN_NAME | LUP_RETURN_ADDR, &len, &u.r) != 0) break;
        if (!u.r.lpszServiceInstanceName || !u.r.lpcsaBuffer) continue;
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, u.r.lpszServiceInstanceName, -1, name, sizeof name, NULL, NULL);
        for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(name, lw)) {
            *out = ((SOCKADDR_BTH *)u.r.lpcsaBuffer->RemoteAddr.lpSockaddr)->btAddr;
            found = 1;
            break;
        }
    }
    WSALookupServiceEnd(h);
    return found;
}

static int bth_host_open(void)
{
    if (g_listen == HBAD_SOCK) {
        g_listen = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (g_listen == HBAD_SOCK) {
            RE5_WARN("bt", "Bluetooth indisponivel neste PC (socket RFCOMM: erro %d)", WSAGetLastError());
            return 0;
        }
        SOCKADDR_BTH sa;
        memset(&sa, 0, sizeof sa);
        sa.addressFamily = AF_BTH;
        sa.port = BT_PORT_ANY;
        int sl = sizeof sa;
        if (bind(g_listen, (SOCKADDR *)&sa, sizeof sa) != 0 || getsockname(g_listen, (SOCKADDR *)&sa, &sl) != 0 ||
            listen(g_listen, 1) != 0) {
            RE5_WARN("bt", "Bluetooth: bind/listen RFCOMM falhou (erro %d)", WSAGetLastError());
            sock_close(g_listen);
            g_listen = HBAD_SOCK;
            return 0;
        }
        CSADDR_INFO ci;
        memset(&ci, 0, sizeof ci);
        ci.LocalAddr.iSockaddrLength = sizeof sa;
        ci.LocalAddr.lpSockaddr = (LPSOCKADDR)&sa;
        ci.RemoteAddr = ci.LocalAddr;
        ci.iSocketType = SOCK_STREAM;
        ci.iProtocol = BTHPROTO_RFCOMM;
        WSAQUERYSETW qs;
        memset(&qs, 0, sizeof qs);
        qs.dwSize = sizeof qs;
        qs.lpszServiceInstanceName = L"RE5 Co-op";
        qs.lpServiceClassId = (LPGUID)&k_svc;
        qs.dwNameSpace = NS_BTH;
        qs.lpcsaBuffer = &ci;
        qs.dwNumberOfCsAddrs = 1;
        if (WSASetServiceW(&qs, RNRSERVICE_REGISTER, 0) != 0)
            RE5_WARN("bt", "Bluetooth: registro do servico RE5 falhou (erro %d): o outro aparelho pode nao achar este PC",
                     WSAGetLastError());
        RE5_INFO("bt", "Bluetooth: aguardando o outro aparelho (canal RFCOMM %lu)", (unsigned long)sa.port);
    }
    if (!wait_readable(g_listen, 2000)) return 0;
    g_conn = accept(g_listen, NULL, NULL);
    return g_conn != HBAD_SOCK;
}

static int bth_client_open(const char *want)
{
    BTH_ADDR addr = 0;
    if (!parse_mac(want, &addr) && !find_device(want, &addr)) {
        RE5_WARN("bt", "Bluetooth: aparelho '%s' nao encontrado entre os pareados (pareie nas configuracoes do Windows)", want);
        return 0;
    }
    hsock_t s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == HBAD_SOCK) return 0;
    SOCKADDR_BTH sa;
    memset(&sa, 0, sizeof sa);
    sa.addressFamily = AF_BTH;
    sa.btAddr = addr;
    sa.serviceClassId = k_svc;
    if (connect(s, (SOCKADDR *)&sa, sizeof sa) != 0) {
        sock_close(s);
        return 0;
    }
    g_conn = s;
    return 1;
}
#endif

static int tcp_open(const char *cfg)
{
    int host = strncmp(cfg, "tcp-host:", 9) == 0;
    const char *arg = cfg + (host ? 9 : 4);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    if (host) {
        a.sin_port = htons((uint16_t)atoi(arg));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (g_listen == HBAD_SOCK) {
            g_listen = socket(AF_INET, SOCK_STREAM, 0);
            int on = 1;
            setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (g_listen == HBAD_SOCK || bind(g_listen, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_listen, 1) != 0) {
                if (g_listen != HBAD_SOCK) sock_close(g_listen);
                g_listen = HBAD_SOCK;
                return 0;
            }
        }
        if (!wait_readable(g_listen, 500)) return 0;
        g_conn = accept(g_listen, NULL, NULL);
        return g_conn != HBAD_SOCK;
    }
    char ip[64];
    snprintf(ip, sizeof ip, "%s", arg);
    char *colon = strrchr(ip, ':');
    if (!colon) return 0;
    *colon = 0;
    a.sin_port = htons((uint16_t)atoi(colon + 1));
    a.sin_addr.s_addr = inet_addr(ip);
    hsock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == HBAD_SOCK) return 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        sock_close(s);
        return 0;
    }
    g_conn = s;
    return 1;
}

/* ---- backend Android: GameActivity.btOpen/btRead/btWrite/btClose (BluetoothSocket em Java) ---- */

#if defined(__ANDROID__)
static int jni_open(const char *cfg)
{
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject act = env ? SDL_AndroidGetActivity() : NULL;
    if (!act) return 0;
    jclass k = (*env)->GetObjectClass(env, act);
    jmethodID m = k ? (*env)->GetMethodID(env, k, "btOpen", "(Ljava/lang/String;)Z") : NULL;
    int ok = 0;
    if (m) {
        jstring js = (*env)->NewStringUTF(env, cfg);
        ok = (*env)->CallBooleanMethod(env, act, m, js) ? 1 : 0;
        (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (k) (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, act);
    return ok;
}

static int jni_read(uint8_t *buf, size_t cap)
{
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject act = env ? SDL_AndroidGetActivity() : NULL;
    if (!act) return -1;
    jclass k = (*env)->GetObjectClass(env, act);
    jmethodID m = k ? (*env)->GetMethodID(env, k, "btRead", "()[B") : NULL;
    int n = -1;
    if (m) {
        jbyteArray a = (jbyteArray)(*env)->CallObjectMethod(env, act, m);
        if (a) {
            jsize len = (*env)->GetArrayLength(env, a);
            if ((size_t)len <= cap) {
                (*env)->GetByteArrayRegion(env, a, 0, len, (jbyte *)buf);
                n = (int)len;
            }
            (*env)->DeleteLocalRef(env, a);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (k) (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, act);
    return n;
}

static int jni_write(const uint8_t *d, size_t len)
{
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject act = env ? SDL_AndroidGetActivity() : NULL;
    if (!act) return -1;
    jclass k = (*env)->GetObjectClass(env, act);
    jmethodID m = k ? (*env)->GetMethodID(env, k, "btWrite", "([B)Z") : NULL;
    int ok = -1;
    if (m) {
        jbyteArray a = (*env)->NewByteArray(env, (jsize)len);
        (*env)->SetByteArrayRegion(env, a, 0, (jsize)len, (const jbyte *)d);
        ok = (*env)->CallBooleanMethod(env, act, m, a) ? 0 : -1;
        (*env)->DeleteLocalRef(env, a);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (k) (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, act);
    return ok;
}

static void jni_close(void)
{
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject act = env ? SDL_AndroidGetActivity() : NULL;
    if (!act) return;
    jclass k = (*env)->GetObjectClass(env, act);
    jmethodID m = k ? (*env)->GetMethodID(env, k, "btClose", "()V") : NULL;
    if (m) (*env)->CallVoidMethod(env, act, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (k) (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, act);
}
#endif

/* ---- ciclo do enlace ---- */

static int is_tcp(void) { return strncmp(g_cfg, "tcp", 3) == 0; }

static int link_open(void)
{
    if (is_tcp()) return tcp_open(g_cfg);
#if defined(__ANDROID__)
    return jni_open(g_cfg);
#elif defined(_WIN32)
    return strcmp(g_cfg, "host") == 0 ? bth_host_open() : bth_client_open(g_cfg);
#else
    return 0;
#endif
}

static int link_read(uint8_t *buf)
{
#if defined(__ANDROID__)
    if (!is_tcp()) return jni_read(buf, RE5_BT_MAX_FRAME);
#endif
    uint8_t h[2];
    if (recv_all(g_conn, h, 2) != 0) return -1;
    size_t n = ((size_t)h[0] << 8) | h[1];
    if (n == 0 || n > RE5_BT_MAX_FRAME) return -1;
    if (recv_all(g_conn, buf, n) != 0) return -1;
    return (int)n;
}

static int link_write(const uint8_t *d, size_t len)
{
#if defined(__ANDROID__)
    if (!is_tcp()) return jni_write(d, len);
#endif
    uint8_t buf[RE5_BT_MAX_FRAME + 2];
    buf[0] = (uint8_t)(len >> 8);
    buf[1] = (uint8_t)len;
    memcpy(buf + 2, d, len);
    return send_all(g_conn, buf, len + 2);
}

static void link_close(void)
{
#if defined(__ANDROID__)
    if (!is_tcp()) jni_close();
#endif
    if (g_conn != HBAD_SOCK) {
        sock_close(g_conn);
        g_conn = HBAD_SOCK;
    }
}

static void reader_thread(void *arg)
{
    (void)arg;
    re5_thread_set_host_name("re5-bt");
    static uint8_t buf[RE5_BT_MAX_FRAME];
    while (g_run) {
        if (!g_up) {
            if (!link_open()) {
                for (int i = 0; i < 20 && g_run; i++) re5_host_sleep_ms(100);   /* 2 s entre tentativas */
                continue;
            }
            re5_mutex_lock(&g_mu);
            g_rx_h = g_rx_n = g_tx_h = g_tx_n = 0;
            re5_mutex_unlock(&g_mu);
            g_up = 1;
            RE5_INFO("bt", "Enlace conectado (%s)", g_cfg);
        }
        int n = link_read(buf);
        if (n <= 0) {
            g_up = 0;
            link_close();
            RE5_WARN("bt", "Enlace caiu: tentando de novo");
            continue;
        }
        re5_mutex_lock(&g_mu);
        if (g_rx_n < QN) {
            frame_t *f = &g_rx[(g_rx_h + g_rx_n) % QN];
            f->len = (uint16_t)n;
            memcpy(f->d, buf, (size_t)n);
            g_rx_n++;
        }   /* cheia: o canal confiavel reenvia, o resto era tempo real mesmo */
        re5_mutex_unlock(&g_mu);
    }
}

static void writer_thread(void *arg)
{
    (void)arg;
    re5_thread_set_host_name("re5-bt-tx");
    static frame_t f;
    while (g_run) {
        int have = 0;
        if (g_up) {
            re5_mutex_lock(&g_mu);
            if (g_tx_n > 0) {
                f = g_tx[g_tx_h];
                g_tx_h = (g_tx_h + 1) % QN;
                g_tx_n--;
                have = 1;
            }
            re5_mutex_unlock(&g_mu);
        }
        if (!have) {
            re5_host_sleep_ms(2);
            continue;
        }
        if (link_write(f.d, f.len) != 0 && g_up) {
            g_up = 0;
            link_close();   /* destrava a leitura; o reader reabre */
        }
    }
}

int re5_bt_start(const char *cfg)
{
    if (g_started || !cfg || !*cfg) return g_started ? 0 : -1;
    re5_mutex_init(&g_mu);
#if defined(_WIN32)
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    snprintf(g_cfg, sizeof g_cfg, "%s", cfg);
    g_run = 1;
    g_started = 1;
    if (re5_host_thread_start(reader_thread, NULL, 1u << 18) != 0 || re5_host_thread_start(writer_thread, NULL, 1u << 18) != 0) {
        g_run = 0;
        g_started = 0;
        return -1;
    }
    RE5_INFO("bt", "Enlace Bluetooth iniciado (%s)", g_cfg);
    return 0;
}

void re5_bt_stop(void)
{
    if (!g_started) return;
    g_run = 0;
    g_up = 0;
    link_close();
    re5_host_sleep_ms(30);
    if (g_listen != HBAD_SOCK) {
        sock_close(g_listen);
        g_listen = HBAD_SOCK;
    }
    g_started = 0;
}

int re5_bt_connected(void) { return g_started && g_up; }

int re5_bt_send(const void *data, size_t len)
{
    if (!g_started || !g_up || !data || !len || len > RE5_BT_MAX_FRAME) return 0;
    int ok = 0;
    re5_mutex_lock(&g_mu);
    if (g_tx_n < QN) {
        frame_t *f = &g_tx[(g_tx_h + g_tx_n) % QN];
        f->len = (uint16_t)len;
        memcpy(f->d, data, len);
        g_tx_n++;
        ok = 1;
    }
    re5_mutex_unlock(&g_mu);
    return ok;
}

size_t re5_bt_recv(void *buf, size_t cap)
{
    size_t n = 0;
    if (!g_started) return 0;
    re5_mutex_lock(&g_mu);
    if (g_rx_n > 0) {
        frame_t *f = &g_rx[g_rx_h];
        n = f->len <= cap ? f->len : 0;
        if (n) memcpy(buf, f->d, n);
        g_rx_h = (g_rx_h + 1) % QN;
        g_rx_n--;
    }
    re5_mutex_unlock(&g_mu);
    return n;
}
