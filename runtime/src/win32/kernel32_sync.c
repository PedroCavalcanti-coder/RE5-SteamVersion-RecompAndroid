/*
 * kernel32_sync.c - seções críticas, TLS, Interlocked e objetos de espera
 * (eventos, mutexes, semáforos) - Fase 3, etapa 3.2.
 *
 * Com o lock global do guest (D5), só uma thread executa código do guest por
 * vez; o que precisa de cuidado é ESPERAR: quem espera solta o lock do guest
 * (RE5_BLOCKING_BEGIN/END), senão o dono do recurso nunca roda.
 *
 *   CRITICAL_SECTION  estado na própria estrutura do guest (OwningThread,
 *                     RecursionCount), protegido por mutexes do host por faixa
 *                     de endereço, sem o lock do HLE; quem espera dorme numa
 *                     fila e o Leave passa a seção direto para o primeiro.
 *   eventos etc.      objetos do host atrás de HANDLEs; um mutex e uma variável
 *                     de condição globais para todas as esperas (acordar todo
 *                     mundo a cada sinal é simples e correto; desempenho é Fase 6).
 *                     Sinalizar e esperar não pegam o lock do HLE: g_mu e o
 *                     mutex da tabela de handles já protegem tudo o que mexem
 *                     (SetEvent segurava o lock global; RE5_PROFILE, Fase 4).
 */
#include <stdlib.h>

#include "kernel32_internal.h"
#include "../core/host_sync.h"

#define K32_WAIT_OBJECT_0  0x00000000u
#define K32_WAIT_ABANDONED 0x00000080u
#define K32_WAIT_TIMEOUT   0x00000102u
#define K32_WAIT_FAILED    0xFFFFFFFFu
#define K32_INFINITE_MS    0xFFFFFFFFu
#define PUMP_SLICE_MS      10u   /* thread da janela: mensagens do Windows a cada 10 ms numa espera */
#define K32_ERROR_ALREADY_EXISTS 183u
#define K32_ERROR_NOT_OWNER      288u
#define K32_ERROR_TOO_MANY_POSTS 298u
#define K32_ERROR_INVALID_HANDLE 6u

static uint32_t cur_tid(void)
{
    re5_thread *t = re5_thread_current();
    return t ? t->tid : 0;
}

/* ---- CRITICAL_SECTION (x86): DebugInfo, LockCount, RecursionCount,
 *      OwningThread, LockSemaphore, SpinCount ----
 *
 * Sem o lock do HLE (RE5_THUNK_NOLOCK): o estado de cada seção é protegido por
 * um de CS_STRIPES mutexes do host, escolhido pelo endereço, e quem espera
 * dorme na variável de condição da faixa até o Leave entregar a seção a ele.
 * Medido no carregamento (RE5_PROFILE): 48% das amostras eram threads esperando
 * o lock global só para entrar no EnterCriticalSection, e quem esperava a seção
 * girava com Sleep(1). */

#define CS_LOCKCOUNT 4u
#define CS_RECURSION 8u
#define CS_OWNER     12u
#define CS_SPIN      20u

/* Filas de espera das seções disputadas, no host (o guest não vê): quem chega
 * primeiro recebe a seção do Leave. Sem isso, uma thread que só faz
 * Enter/Leave em laço nunca deixa a outra entrar. */
#define CS_STRIPES 64
#define CS_QUEUES  8      /* seções disputadas ao mesmo tempo por faixa */
#define CS_QLEN    32
typedef struct {
    uint32_t cs, n;
    uint32_t tid[CS_QLEN];
} cs_queue;
typedef struct {
    re5_mutex mu;
    re5_cond  cv;
    cs_queue  q[CS_QUEUES];
} cs_stripe;
static cs_stripe g_cs[CS_STRIPES];
static volatile int g_cs_ready;
static re5_mutex g_cs_init_mu;

static cs_stripe *stripe_of(uint32_t cs)
{
    if (!g_cs_ready) {
        static volatile int once;
        if (!once) {   /* a primeira chamada vem do CRT, antes de haver outra thread */
            re5_mutex_init(&g_cs_init_mu);
            once = 1;
        }
        re5_mutex_lock(&g_cs_init_mu);
        if (!g_cs_ready) {
            for (int i = 0; i < CS_STRIPES; i++) {
                re5_mutex_init(&g_cs[i].mu);
                re5_cond_init(&g_cs[i].cv);
            }
            g_cs_ready = 1;
        }
        re5_mutex_unlock(&g_cs_init_mu);
    }
    return &g_cs[(cs >> 3) % CS_STRIPES];
}

static void cs_init(uint32_t cs, uint32_t spin)
{
    k32_w32(cs, 0xFFFFFFFFu);
    k32_w32(cs + CS_LOCKCOUNT, 0xFFFFFFFFu);
    k32_w32(cs + CS_RECURSION, 0);
    k32_w32(cs + CS_OWNER, 0);
    k32_w32(cs + 16, 0);
    k32_w32(cs + CS_SPIN, spin);
}

/* Com o mutex da faixa. */
static int cs_try_locked(uint32_t cs, uint32_t tid)
{
    uint32_t owner = k32_r32(cs + CS_OWNER);
    if (owner && owner != tid) return 0;
    k32_w32(cs + CS_OWNER, tid);
    k32_w32(cs + CS_RECURSION, k32_r32(cs + CS_RECURSION) + 1);
    k32_w32(cs + CS_LOCKCOUNT, k32_r32(cs + CS_LOCKCOUNT) + 1);
    return 1;
}

static cs_queue *csq_find(cs_stripe *s, uint32_t cs, int create)
{
    cs_queue *empty = NULL;
    for (int i = 0; i < CS_QUEUES; i++) {
        if (s->q[i].n && s->q[i].cs == cs) return &s->q[i];
        if (!s->q[i].n && !empty) empty = &s->q[i];
    }
    if (!create) return NULL;
    if (!empty) re5_fatal("kernel32", "mais de %d seções críticas disputadas ao mesmo tempo numa faixa", CS_QUEUES);
    empty->cs = cs;
    return empty;
}

static void k_InitializeCriticalSection(CPU *c) { cs_init(re5_arg(c, 0), 0); }

static void k_InitializeCriticalSectionAndSpinCount(CPU *c)
{
    cs_init(re5_arg(c, 0), re5_arg(c, 1));
    c->eax = 1;
}

static void k_DeleteCriticalSection(CPU *c)
{
    uint32_t cs = re5_arg(c, 0);
    memset(re5_g2h(cs), 0, 24);
}

static void k_EnterCriticalSection(CPU *c)
{
    uint32_t cs = re5_arg(c, 0), tid = cur_tid();
    cs_stripe *s = stripe_of(cs);
    re5_mutex_lock(&s->mu);
    if (cs_try_locked(cs, tid)) {
        re5_mutex_unlock(&s->mu);
        return;
    }
    cs_queue *q = csq_find(s, cs, 1);
    if (q->n == CS_QLEN) re5_fatal("kernel32", "mais de %d threads esperando a seção 0x%08X", CS_QLEN, cs);
    q->tid[q->n++] = tid;
    re5_mutex_unlock(&s->mu);
    /* O Leave do dono põe esta thread como dona (recursão 1) quando chegar a vez
     * dela. Sem o lock do guest (modo global) e sem o mutex da faixa na ordem
     * guest -> faixa, nunca o contrário. */
    RE5_BLOCKING_BEGIN();
    int pump = re5_thread_has_pump();
    re5_mutex_lock(&s->mu);
    while (k32_r32(cs + CS_OWNER) != tid) {
        if (re5_cond_wait_ms(&s->cv, &s->mu, pump ? PUMP_SLICE_MS : K32_INFINITE_MS) && pump &&
            k32_r32(cs + CS_OWNER) != tid) {
            re5_mutex_unlock(&s->mu);   /* thread da janela: o Windows não a marca travada */
            re5_thread_pump();
            re5_mutex_lock(&s->mu);
        }
    }
    re5_mutex_unlock(&s->mu);
    RE5_BLOCKING_END();
}

static void k_TryEnterCriticalSection(CPU *c)
{
    uint32_t cs = re5_arg(c, 0);
    cs_stripe *s = stripe_of(cs);
    re5_mutex_lock(&s->mu);
    c->eax = (uint32_t)cs_try_locked(cs, cur_tid());
    re5_mutex_unlock(&s->mu);
}

static void k_LeaveCriticalSection(CPU *c)
{
    uint32_t cs = re5_arg(c, 0), tid = cur_tid();
    cs_stripe *s = stripe_of(cs);
    re5_mutex_lock(&s->mu);
    uint32_t owner = k32_r32(cs + CS_OWNER), rec = k32_r32(cs + CS_RECURSION);
    if (owner != tid || !rec) {
        re5_mutex_unlock(&s->mu);
        re5_fatal("kernel32", "LeaveCriticalSection(0x%08X) pela thread 0x%X, dona 0x%X (recursão %u)", cs, tid,
                  owner, rec);
    }
    k32_w32(cs + CS_RECURSION, --rec);
    k32_w32(cs + CS_LOCKCOUNT, k32_r32(cs + CS_LOCKCOUNT) - 1);
    if (!rec) {
        cs_queue *q = csq_find(s, cs, 0);
        uint32_t next = 0;
        if (q) {
            next = q->tid[0];
            memmove(q->tid, q->tid + 1, (q->n - 1) * sizeof q->tid[0]);
            q->n--;
        }
        k32_w32(cs + CS_OWNER, next);
        if (next) {
            k32_w32(cs + CS_RECURSION, 1);
            k32_w32(cs + CS_LOCKCOUNT, k32_r32(cs + CS_LOCKCOUNT) + 1);
            re5_cond_broadcast(&s->cv);
        }
    }
    re5_mutex_unlock(&s->mu);
}

/* ---- TLS: slots no TEB (fs:[0xE10]) ---- */

static uint64_t g_tls_used;

static void k_TlsAlloc(CPU *c)
{
    for (uint32_t i = 0; i < RE5_TLS_SLOTS; i++) {
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            re5_thread *t = re5_thread_current();
            if (t) k32_w32(t->teb + RE5_TEB_TLS_SLOTS + 4 * i, 0);
            c->eax = i;
            return;
        }
    }
    re5_fatal("kernel32", "TlsAlloc: os %u slots do TEB acabaram", RE5_TLS_SLOTS);
}

static int tls_ok(uint32_t i) { return i < RE5_TLS_SLOTS && (g_tls_used & (1ull << i)); }

static void k_TlsFree(CPU *c)
{
    uint32_t i = re5_arg(c, 0);
    if (!tls_ok(i)) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = 0; return; }
    g_tls_used &= ~(1ull << i);
    c->eax = 1;
}

static void k_TlsGetValue(CPU *c)
{
    uint32_t i = re5_arg(c, 0);
    re5_thread *t = re5_thread_current();
    if (!tls_ok(i) || !t) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = 0; return; }
    c->eax = k32_r32(t->teb + RE5_TEB_TLS_SLOTS + 4 * i);
    re5_set_last_error(K32_ERROR_SUCCESS);   /* o Windows faz isso: 0 vira valor válido */
}

static void k_TlsSetValue(CPU *c)
{
    uint32_t i = re5_arg(c, 0);
    re5_thread *t = re5_thread_current();
    if (!tls_ok(i) || !t) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = 0; return; }
    k32_w32(t->teb + RE5_TEB_TLS_SLOTS + 4 * i, re5_arg(c, 1));
    c->eax = 1;
}

/* ---- Interlocked: atômicos do host (RE5_THUNK_NOLOCK: o guest pode rodar em paralelo) ---- */

static volatile uint32_t *k32_atomic_at(uint32_t va) { return (volatile uint32_t *)re5_g2h(va); }

static void k_InterlockedIncrement(CPU *c) { c->eax = __atomic_add_fetch(k32_atomic_at(re5_arg(c, 0)), 1u, __ATOMIC_SEQ_CST); }

static void k_InterlockedDecrement(CPU *c) { c->eax = __atomic_sub_fetch(k32_atomic_at(re5_arg(c, 0)), 1u, __ATOMIC_SEQ_CST); }

static void k_InterlockedExchangeAdd(CPU *c)
{
    c->eax = __atomic_fetch_add(k32_atomic_at(re5_arg(c, 0)), re5_arg(c, 1), __ATOMIC_SEQ_CST);
}

/* ---- objetos de espera ---- */

typedef struct waitable {
    re5_handle_type type;
    int      manual, signaled;     /* evento; thread: terminou */
    uint32_t owner, recursion;     /* mutex */
    int32_t  count, max;           /* semáforo */
    uint32_t tid, exit_code;       /* thread */
    uint32_t start, param, stack;
    int      suspend, running;     /* suspend: antes de começar; depois vale o da re5_thread */
    re5_thread *thr;               /* vivo enquanto !signaled (o detach vem depois) */
    char    *name;
    uint32_t handle;
    struct waitable *next_named;
} waitable;

static re5_mutex g_mu;
static re5_cond g_cv;
static int g_ready;
static waitable *g_named;

static void ensure_init(void)
{
    if (!g_ready) {
        re5_mutex_init(&g_mu);
        re5_cond_init(&g_cv);
        g_ready = 1;
    }
}

static void destroy_waitable(void *p)
{
    waitable *w = p;
    re5_mutex_lock(&g_mu);
    for (waitable **pp = &g_named; *pp; pp = &(*pp)->next_named) {
        if (*pp == w) { *pp = w->next_named; break; }
    }
    re5_mutex_unlock(&g_mu);
    free(w->name);
    free(w);
}

/* Cria (ou abre, se o nome já existe) um objeto. *existed = 1 no segundo caso. */
static uint32_t create_waitable(re5_handle_type type, uint32_t name_va, waitable **out, int *existed)
{
    ensure_init();
    const char *name = re5_gstr(name_va);
    *existed = 0;
    if (name && *name) {
        re5_mutex_lock(&g_mu);
        for (waitable *w = g_named; w; w = w->next_named) {
            if (strcmp(w->name, name) == 0) {
                re5_mutex_unlock(&g_mu);
                if (w->type != type) return 0;
                *existed = 1;
                *out = w;
                return re5_handle_dup(w->handle);
            }
        }
        re5_mutex_unlock(&g_mu);
    }
    waitable *w = calloc(1, sizeof *w);
    if (!w) return 0;
    w->type = type;
    w->handle = re5_handle_new(type, w, destroy_waitable);
    if (name && *name) {
        w->name = strdup(name);
        re5_mutex_lock(&g_mu);
        w->next_named = g_named;
        g_named = w;
        re5_mutex_unlock(&g_mu);
    }
    *out = w;
    return w->handle;
}

static waitable *get_waitable(uint32_t h, re5_handle_type type)
{
    waitable *w = re5_handle_get(h, type);
    if (!w) re5_set_last_error(K32_ERROR_INVALID_HANDLE);
    return w;
}

static void k_CreateEventA(CPU *c)
{
    waitable *w;
    int existed;
    c->eax = create_waitable(RE5_H_EVENT, re5_arg(c, 3), &w, &existed);
    if (!c->eax) { re5_set_last_error(K32_ERROR_INVALID_HANDLE); return; }
    if (!existed) {
        w->manual = re5_arg(c, 1) != 0;
        w->signaled = re5_arg(c, 2) != 0;
    }
    re5_set_last_error(existed ? K32_ERROR_ALREADY_EXISTS : K32_ERROR_SUCCESS);
}

int k32_event_signal(uint32_t h)
{
    waitable *w = get_waitable(h, RE5_H_EVENT);
    if (!w) return 0;
    re5_mutex_lock(&g_mu);
    w->signaled = 1;
    re5_cond_broadcast(&g_cv);
    re5_mutex_unlock(&g_mu);
    return 1;
}

static void k_SetEvent(CPU *c) { c->eax = (uint32_t)k32_event_signal(re5_arg(c, 0)); }

static void k_ResetEvent(CPU *c)
{
    waitable *w = get_waitable(re5_arg(c, 0), RE5_H_EVENT);
    c->eax = 0;
    if (!w) return;
    re5_mutex_lock(&g_mu);
    w->signaled = 0;
    re5_mutex_unlock(&g_mu);
    c->eax = 1;
}

static void k_CreateMutexA(CPU *c)
{
    waitable *w;
    int existed;
    c->eax = create_waitable(RE5_H_MUTEX, re5_arg(c, 2), &w, &existed);
    if (!c->eax) { re5_set_last_error(K32_ERROR_INVALID_HANDLE); return; }
    if (!existed && re5_arg(c, 1)) {
        w->owner = cur_tid();
        w->recursion = 1;
    }
    re5_set_last_error(existed ? K32_ERROR_ALREADY_EXISTS : K32_ERROR_SUCCESS);
}

static void k_ReleaseMutex(CPU *c)
{
    waitable *w = get_waitable(re5_arg(c, 0), RE5_H_MUTEX);
    c->eax = 0;
    if (!w) return;
    re5_mutex_lock(&g_mu);
    if (w->owner != cur_tid() || !w->recursion) {
        re5_mutex_unlock(&g_mu);
        re5_set_last_error(K32_ERROR_NOT_OWNER);
        return;
    }
    if (--w->recursion == 0) {
        w->owner = 0;
        re5_cond_broadcast(&g_cv);
    }
    re5_mutex_unlock(&g_mu);
    c->eax = 1;
}

static void k_CreateSemaphoreA(CPU *c)
{
    waitable *w;
    int existed;
    int32_t initial = (int32_t)re5_arg(c, 1), max = (int32_t)re5_arg(c, 2);
    if (max <= 0 || initial < 0 || initial > max) {
        re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
        c->eax = 0;
        return;
    }
    c->eax = create_waitable(RE5_H_SEMAPHORE, re5_arg(c, 3), &w, &existed);
    if (!c->eax) { re5_set_last_error(K32_ERROR_INVALID_HANDLE); return; }
    if (!existed) {
        w->count = initial;
        w->max = max;
    }
    re5_set_last_error(existed ? K32_ERROR_ALREADY_EXISTS : K32_ERROR_SUCCESS);
}

static void k_ReleaseSemaphore(CPU *c)
{
    waitable *w = get_waitable(re5_arg(c, 0), RE5_H_SEMAPHORE);
    int32_t n = (int32_t)re5_arg(c, 1);
    uint32_t prev_va = re5_arg(c, 2);
    c->eax = 0;
    if (!w) return;
    re5_mutex_lock(&g_mu);
    if (n <= 0 || w->count + n > w->max) {
        re5_mutex_unlock(&g_mu);
        re5_set_last_error(K32_ERROR_TOO_MANY_POSTS);
        return;
    }
    if (prev_va) k32_w32(prev_va, (uint32_t)w->count);
    w->count += n;
    re5_cond_broadcast(&g_cv);
    re5_mutex_unlock(&g_mu);
    c->eax = 1;
}

/* Sob g_mu. 1 se o objeto pode ser adquirido agora pela thread `tid`. */
static int ready(const waitable *w, uint32_t tid)
{
    switch (w->type) {
    case RE5_H_EVENT: return w->signaled;
    case RE5_H_THREAD: return w->signaled;
    case RE5_H_MUTEX: return !w->owner || w->owner == tid;
    case RE5_H_SEMAPHORE: return w->count > 0;
    default: return 0;
    }
}

static void acquire(waitable *w, uint32_t tid)
{
    switch (w->type) {
    case RE5_H_EVENT: if (!w->manual) w->signaled = 0; break;
    case RE5_H_MUTEX: w->owner = tid; w->recursion++; break;
    case RE5_H_SEMAPHORE: w->count--; break;
    default: break;
    }
}

static uint64_t now_ms(void)
{
#if defined(RE5_HOST_WINDOWS)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

#define MAX_WAIT 64u

static uint32_t wait_objects(uint32_t n, const uint32_t *handles, int all, uint32_t ms)
{
    ensure_init();
    if (!n || n > MAX_WAIT) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return K32_WAIT_FAILED; }
    waitable *w[MAX_WAIT];
    for (uint32_t i = 0; i < n; i++) {
        w[i] = re5_handle_get(handles[i], RE5_H_ANY);
        re5_handle_type t = re5_handle_type_of(handles[i]);
        if (!w[i] || (t != RE5_H_EVENT && t != RE5_H_MUTEX && t != RE5_H_SEMAPHORE && t != RE5_H_THREAD)) {
            re5_set_last_error(K32_ERROR_INVALID_HANDLE);
            return K32_WAIT_FAILED;
        }
    }
    uint32_t tid = cur_tid(), result = K32_WAIT_TIMEOUT;
    uint64_t deadline = ms == K32_INFINITE_MS ? 0 : now_ms() + ms;
    int pump = re5_thread_has_pump();
    RE5_BLOCKING_BEGIN();
    re5_mutex_lock(&g_mu);
    for (;;) {
        if (all) {
            uint32_t ok = 0;
            for (uint32_t i = 0; i < n; i++) ok += (uint32_t)ready(w[i], tid);
            if (ok == n) {
                for (uint32_t i = 0; i < n; i++) acquire(w[i], tid);
                result = K32_WAIT_OBJECT_0;
                break;
            }
        } else {
            uint32_t i = 0;
            while (i < n && !ready(w[i], tid)) i++;
            if (i < n) {
                acquire(w[i], tid);
                result = K32_WAIT_OBJECT_0 + i;
                break;
            }
        }
        uint32_t left = K32_INFINITE_MS;
        if (ms != K32_INFINITE_MS) {
            uint64_t now = now_ms();
            if (now >= deadline) break;
            left = (uint32_t)(deadline - now);
        }
        if (pump && left > PUMP_SLICE_MS) left = PUMP_SLICE_MS;
        re5_cond_wait_ms(&g_cv, &g_mu, left);
        if (pump) {   /* a thread da janela trata as mensagens do Windows durante a espera */
            re5_mutex_unlock(&g_mu);
            re5_thread_pump();
            re5_mutex_lock(&g_mu);
        }
    }
    re5_mutex_unlock(&g_mu);
    RE5_BLOCKING_END();
    return result;
}

static void k_WaitForSingleObject(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    c->eax = wait_objects(1, &h, 0, re5_arg(c, 1));
}

static void k_WaitForMultipleObjects(CPU *c)
{
    uint32_t n = re5_arg(c, 0), arr = re5_arg(c, 1), handles[MAX_WAIT];
    if (!n || n > MAX_WAIT) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); c->eax = K32_WAIT_FAILED; return; }
    for (uint32_t i = 0; i < n; i++) handles[i] = k32_r32(arr + 4 * i);
    c->eax = wait_objects(n, handles, re5_arg(c, 2) != 0, re5_arg(c, 3));
}

/* ---- threads do guest ----
 *
 * Cada CreateThread vira uma thread do host com CPU, pilha e TEB próprios no
 * guest (re5_thread_attach). O objeto de espera é sinalizado quando a função
 * da thread retorna. A thread guarda uma referência ao próprio HANDLE para o
 * objeto sobreviver a um CloseHandle feito pelo jogo logo depois de criá-la. */

#define K32_CREATE_SUSPENDED 0x00000004u
#define K32_STILL_ACTIVE     259u
#define K32_DEFAULT_STACK    (2u << 20)   /* SizeOfStackReserve do re5dx9.exe */
#define HOST_THREAD_STACK    ((size_t)64 << 20)

static void guest_thread_main(void *p)
{
    waitable *w = p;
    re5_thread *t = re5_thread_attach(w->stack);
    if (!t) re5_fatal("kernel32", "CreateThread: sem memória do guest para pilha/TEB");
    re5_mutex_lock(&g_mu);
    w->tid = t->tid;
    w->thr = t;
    re5_cond_broadcast(&g_cv);
    while (w->suspend > 0) re5_cond_wait_ms(&g_cv, &g_mu, K32_INFINITE_MS);
    w->running = 1;
    re5_mutex_unlock(&g_mu);

    re5_guest_lock();
    uint32_t code = re5_guest_call(t, w->start, 1, &w->param);
    re5_guest_unlock();
    RE5_DEBUG("kernel32", "thread 0x%X (início 0x%08X) terminou com %u", t->tid, w->start, code);

    re5_mutex_lock(&g_mu);
    w->exit_code = code;
    w->signaled = 1;
    re5_cond_broadcast(&g_cv);
    re5_mutex_unlock(&g_mu);
    uint32_t h = w->handle;
    re5_thread_detach();
    re5_handle_close(h);   /* a referência da própria thread */
}

static void k_CreateThread(CPU *c)
{
    ensure_init();
    uint32_t stack = re5_arg(c, 1), start = re5_arg(c, 2), param = re5_arg(c, 3), flags = re5_arg(c, 4);
    uint32_t tid_va = re5_arg(c, 5);
    waitable *w = calloc(1, sizeof *w);
    if (!w) { re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY); c->eax = 0; return; }
    w->type = RE5_H_THREAD;
    w->start = start;
    w->param = param;
    w->stack = stack ? (stack < (256u << 10) ? (256u << 10) : stack) : K32_DEFAULT_STACK;
    w->suspend = (flags & K32_CREATE_SUSPENDED) ? 1 : 0;
    w->handle = re5_handle_new(RE5_H_THREAD, w, destroy_waitable);
    re5_handle_dup(w->handle);   /* referência da thread */
    if (re5_host_thread_start(guest_thread_main, w, HOST_THREAD_STACK) != 0) {
        re5_handle_close(w->handle);
        re5_handle_close(w->handle);
        re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY);
        c->eax = 0;
        return;
    }
    /* O TID só existe depois do attach na thread nova (que não precisa do lock do guest). */
    re5_mutex_lock(&g_mu);
    while (!w->tid) re5_cond_wait_ms(&g_cv, &g_mu, K32_INFINITE_MS);
    uint32_t tid = w->tid;
    re5_mutex_unlock(&g_mu);
    if (tid_va) k32_w32(tid_va, tid);
    RE5_DEBUG("kernel32", "CreateThread(início 0x%08X, param 0x%08X, flags 0x%X) -> 0x%X, tid 0x%X", start, param,
              flags, w->handle, tid);
    c->eax = w->handle;
}

/* Antes de começar, a contagem é do objeto (CREATE_SUSPENDED); em execução, a
 * suspensão para a thread no próximo lock do guest (re5_thread_suspend). */
static void k_ResumeThread(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    if (h == RE5_PSEUDO_THREAD) { c->eax = re5_thread_resume(re5_thread_current()); return; }
    waitable *w = get_waitable(h, RE5_H_THREAD);
    if (!w) { c->eax = 0xFFFFFFFFu; return; }
    re5_mutex_lock(&g_mu);
    if (w->running && !w->signaled) {
        c->eax = re5_thread_resume(w->thr);
    } else {
        c->eax = (uint32_t)w->suspend;
        if (w->suspend > 0 && --w->suspend == 0) re5_cond_broadcast(&g_cv);
    }
    re5_mutex_unlock(&g_mu);
}

static void k_SuspendThread(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    re5_thread *self = re5_thread_current();
    if (h == RE5_PSEUDO_THREAD) { c->eax = re5_thread_suspend(self); return; }
    waitable *w = get_waitable(h, RE5_H_THREAD);
    if (!w) { c->eax = 0xFFFFFFFFu; return; }
    re5_mutex_lock(&g_mu);
    int running = w->running && !w->signaled;
    re5_thread *t = w->thr;
    if (!running) c->eax = (uint32_t)w->suspend++;
    else if (t != self) c->eax = re5_thread_suspend(t);   /* não bloqueia; t vivo sob g_mu */
    re5_mutex_unlock(&g_mu);
    if (running && t == self) c->eax = re5_thread_suspend(t);   /* para aqui, sem g_mu */
}

static void k_GetExitCodeThread(CPU *c)
{
    waitable *w = get_waitable(re5_arg(c, 0), RE5_H_THREAD);
    c->eax = 0;
    if (!w) return;
    re5_mutex_lock(&g_mu);
    k32_w32(re5_arg(c, 1), w->signaled ? w->exit_code : K32_STILL_ACTIVE);
    re5_mutex_unlock(&g_mu);
    c->eax = 1;
}

/* Prioridade só vale no host com RE5_THREAD_PRIO=1 (re5_thread_apply_priority); afinidade nunca. */
static void k_SetThreadPriority(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    waitable *w = get_waitable(h, RE5_H_THREAD);
    uint32_t tid = 0;
    if (w) {
        re5_mutex_lock(&g_mu);
        tid = w->tid;
        re5_mutex_unlock(&g_mu);
    } else if (h == RE5_PSEUDO_THREAD) {
        re5_thread *t = re5_thread_current();
        tid = t ? t->tid : 0;
    }
    if (tid) re5_thread_apply_priority(tid, (int)re5_arg(c, 1));
    c->eax = w || h == RE5_PSEUDO_THREAD;
}
static void k_SetThreadIdealProcessor(CPU *c) { c->eax = 0; }

/* ---- APCs de usuário: só as que a própria thread enfileira (ReadFileEx) ---- */

#define MAX_APC 64u
typedef struct { uint32_t fn, a[3]; } apc;
static RE5_THREAD_LOCAL apc t_apc[MAX_APC];
static RE5_THREAD_LOCAL uint32_t t_napc;

void k32_apc_queue(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2)
{
    if (t_napc == MAX_APC) re5_fatal("kernel32", "fila de APC cheia (%u): o jogo não entra em espera alertável", MAX_APC);
    t_apc[t_napc++] = (apc){fn, {a0, a1, a2}};
}

int k32_apc_run(void)
{
    int n = 0;
    re5_thread *t = re5_thread_current();
    while (t_napc) {
        apc a = t_apc[0];
        memmove(&t_apc[0], &t_apc[1], (t_napc - 1) * sizeof t_apc[0]);
        t_napc--;
        re5_guest_call(t, a.fn, 3, a.a);   /* a completion routine pode enfileirar outra */
        n++;
    }
    return n;
}

#define K32_WAIT_IO_COMPLETION 0xC0u

static void k_SleepEx(CPU *c)
{
    uint32_t ms = re5_arg(c, 0), alertable = re5_arg(c, 1);
    if (alertable && k32_apc_run()) { c->eax = K32_WAIT_IO_COMPLETION; return; }
    RE5_BLOCKING_BEGIN();
    if (!ms) {
        re5_host_yield();
    } else if (re5_thread_has_pump()) {   /* em fatias, tratando as mensagens da janela */
        uint64_t end = ms == K32_INFINITE_MS ? UINT64_MAX : now_ms() + ms;
        for (uint64_t now = now_ms(); now < end; now = now_ms()) {
            re5_host_sleep_ms((uint32_t)(end - now < PUMP_SLICE_MS ? end - now : PUMP_SLICE_MS));
            re5_thread_pump();
        }
    } else {
        re5_host_sleep_ms(ms == K32_INFINITE_MS ? 0x7FFFFFFFu : ms);
    }
    RE5_BLOCKING_END();
    c->eax = 0;
}

static const re5_import_def k_defs[] = {
    {"CreateEventA",                          k_CreateEventA,                          16, 0},
    {"CreateMutexA",                          k_CreateMutexA,                          12, 0},
    {"CreateSemaphoreA",                      k_CreateSemaphoreA,                      16, 0},
    {"CreateThread",                          k_CreateThread,                          24, 0},
    {"GetExitCodeThread",                     k_GetExitCodeThread,                      8, 0},
    {"ResumeThread",                          k_ResumeThread,                           4, 0},
    {"SetThreadIdealProcessor",               k_SetThreadIdealProcessor,                8, 0},
    {"SetThreadPriority",                     k_SetThreadPriority,                      8, 0},
    {"SuspendThread",                         k_SuspendThread,                          4, 0},
    {"DeleteCriticalSection",                 k_DeleteCriticalSection,                  4, 0, RE5_THUNK_NOLOCK},
    {"EnterCriticalSection",                  k_EnterCriticalSection,                   4, 0, RE5_THUNK_NOLOCK},
    {"InitializeCriticalSection",             k_InitializeCriticalSection,              4, 0, RE5_THUNK_NOLOCK},
    {"InitializeCriticalSectionAndSpinCount", k_InitializeCriticalSectionAndSpinCount,  8, 0, RE5_THUNK_NOLOCK},
    {"InterlockedDecrement",                  k_InterlockedDecrement,                   4, 0, RE5_THUNK_NOLOCK},
    {"InterlockedExchangeAdd",                k_InterlockedExchangeAdd,                 8, 0, RE5_THUNK_NOLOCK},
    {"InterlockedIncrement",                  k_InterlockedIncrement,                   4, 0, RE5_THUNK_NOLOCK},
    {"LeaveCriticalSection",                  k_LeaveCriticalSection,                   4, 0, RE5_THUNK_NOLOCK},
    {"ReleaseMutex",                          k_ReleaseMutex,                           4, 0, RE5_THUNK_NOLOCK},
    {"ReleaseSemaphore",                      k_ReleaseSemaphore,                      12, 0, RE5_THUNK_NOLOCK},
    {"ResetEvent",                            k_ResetEvent,                             4, 0, RE5_THUNK_NOLOCK},
    {"SetEvent",                              k_SetEvent,                               4, 0, RE5_THUNK_NOLOCK},
    {"SleepEx",                               k_SleepEx,                                8, 0},
    {"TlsAlloc",                              k_TlsAlloc,                               0, 0},
    {"TlsFree",                               k_TlsFree,                                4, 0},
    {"TlsGetValue",                           k_TlsGetValue,                            4, 0, RE5_THUNK_NOLOCK},
    {"TlsSetValue",                           k_TlsSetValue,                            8, 0, RE5_THUNK_NOLOCK},
    {"TryEnterCriticalSection",               k_TryEnterCriticalSection,                4, 0, RE5_THUNK_NOLOCK},
    {"WaitForMultipleObjects",                k_WaitForMultipleObjects,                16, 0, RE5_THUNK_NOLOCK},
    {"WaitForSingleObject",                   k_WaitForSingleObject,                    8, 0, RE5_THUNK_NOLOCK},
    {0},
};

const re5_import_module re5_mod_kernel32_sync = {"kernel32.dll", k_defs};
