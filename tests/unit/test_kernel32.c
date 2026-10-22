/*
 * HLE do kernel32 que o CRT do MSVC 2005 usa antes do WinMain: heap, memória
 * virtual, seções críticas, TLS, Interlocked, objetos de espera, code pages,
 * ctype, locale, versão do SO, tempo, ambiente e linha de comando. Cada função
 * é chamada como o guest chamaria: pelo thunk do import, com a pilha do guest
 * (o teste confere que o stdcall limpa os argumentos certos).
 */
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;

static re5_thread *T;
static uint32_t g_scratch;

static uint32_t call(const char *name, int n, ...)
{
    uint32_t args[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = T->cpu.esp;
    uint32_t r = re5_guest_call(T, re5_import_bind("kernel32.dll", name, 0), n, args);
    CHECK_EQ_U32(T->cpu.esp, esp0);
    return r;
}

/* Copia para a memória de rascunho do guest e devolve o VA. */
static uint32_t gput(const void *p, uint32_t n)
{
    static uint32_t off;
    uint32_t va = g_scratch + off;
    memcpy(re5_g2h(va), p, n);
    off = (off + n + 15u) & ~15u;
    return va;
}

static uint32_t gstr(const char *s) { return gput(s, (uint32_t)strlen(s) + 1); }
static uint32_t gbuf(uint32_t n) { static const uint8_t z[4096]; return gput(z, n); }
static uint16_t r16(uint32_t va) { uint16_t v; memcpy(&v, re5_g2h(va), 2); return v; }

static void test_heap(void)
{
    uint32_t heap = call("GetProcessHeap", 0);
    CHECK(heap != 0);
    CHECK_EQ_U32(rd32(re5_process_peb() + RE5_PEB_PROCESS_HEAP), heap);

    uint32_t a = call("HeapAlloc", 3, heap, 0u, 10u);
    CHECK(a && a % 8 == 0);
    CHECK_EQ_U32(call("HeapSize", 3, heap, 0u, a), 10);
    memset(re5_g2h(a), 0xAB, 10);

    /* realloc dentro da classe: no lugar; fora: copia */
    CHECK_EQ_U32(call("HeapReAlloc", 4, heap, 0u, a, 12u), a);
    uint32_t b = call("HeapReAlloc", 4, heap, 8u, a, 3000u);   /* HEAP_ZERO_MEMORY */
    CHECK(b && b != a);
    CHECK_EQ_U32(((uint8_t *)re5_g2h(b))[9], 0xAB);
    CHECK_EQ_U32(((uint8_t *)re5_g2h(b))[12], 0);
    CHECK_EQ_U32(((uint8_t *)re5_g2h(b))[2999], 0);
    CHECK_EQ_U32(call("HeapSize", 3, heap, 0u, b), 3000);

    /* bloco liberado volta pela mesma classe, zerado se pedido */
    uint32_t c = call("HeapAlloc", 3, heap, 0u, 10u);
    CHECK_EQ_U32(c, a);
    CHECK_EQ_U32(call("HeapFree", 3, heap, 0u, c), 1);
    uint32_t d = call("HeapAlloc", 3, heap, 8u, 16u);
    CHECK_EQ_U32(d, a);
    CHECK_EQ_U32(((uint8_t *)re5_g2h(d))[0], 0);

    uint32_t big = call("HeapAlloc", 3, heap, 0u, 3u << 20);
    CHECK(big && big % 8 == 0);
    CHECK_EQ_U32(call("HeapSize", 3, heap, 0u, big), 3u << 20);
    CHECK_EQ_U32(call("HeapReAlloc", 4, heap, 0u, big, (3u << 20) + 100u), big);   /* cabe na região */
    CHECK_EQ_U32(call("HeapFree", 3, heap, 0u, big), 1);
    CHECK_EQ_U32(call("HeapFree", 3, heap, 0u, 0u), 1);

    uint32_t h2 = call("HeapCreate", 3, 0u, 0x1000u, 0u);
    CHECK(h2 && h2 != heap);
    uint32_t e = call("HeapAlloc", 3, h2, 0u, 100u);
    CHECK(e != 0);
    CHECK_EQ_U32(call("HeapDestroy", 1, h2), 1);
    CHECK_EQ_U32(call("HeapDestroy", 1, heap), 0);   /* o do processo não */

    uint32_t g = call("GlobalAlloc", 2, 0x40u, 64u);
    CHECK(g != 0);
    CHECK_EQ_U32(call("GlobalLock", 1, g), g);
}

static void test_virtual(void)
{
    uint32_t r = call("VirtualAlloc", 4, 0u, 0x30000u, 0x2000u, 0x04u);   /* reserva */
    CHECK(r && r % 0x10000u == 0);
    CHECK_EQ_U32(call("VirtualAlloc", 4, r + 0x1234u, 0x100u, 0x1000u, 0x04u), r + 0x1000u);
    wr32(r + 0x1234u, 77);
    CHECK_EQ_U32(call("VirtualFree", 3, r + 0x1000u, 0x1000u, 0x4000u), 1);   /* decommit zera */
    CHECK_EQ_U32(rd32(r + 0x1234u), 0);
    CHECK_EQ_U32(call("VirtualFree", 3, r, 0u, 0x8000u), 1);
    CHECK_EQ_U32(call("VirtualFree", 3, r, 0u, 0x8000u), 0);
    uint32_t fixed = call("VirtualAlloc", 4, 0x30000000u, 0x10000u, 0x3000u, 0x04u);
    CHECK_EQ_U32(fixed, 0x30000000u);
    CHECK_EQ_U32(call("VirtualAlloc", 4, 0x30000000u, 0x10000u, 0x3000u, 0x04u), 0);   /* ocupado */
}

static void test_sync(void)
{
    uint32_t cs = gbuf(24);
    CHECK_EQ_U32(call("InitializeCriticalSectionAndSpinCount", 2, cs, 4000u), 1);
    call("EnterCriticalSection", 1, cs);
    call("EnterCriticalSection", 1, cs);
    CHECK_EQ_U32(rd32(cs + 8), 2);               /* RecursionCount */
    CHECK_EQ_U32(rd32(cs + 12), T->tid);         /* OwningThread */
    CHECK_EQ_U32(call("TryEnterCriticalSection", 1, cs), 1);
    call("LeaveCriticalSection", 1, cs);
    call("LeaveCriticalSection", 1, cs);
    call("LeaveCriticalSection", 1, cs);
    CHECK_EQ_U32(rd32(cs + 12), 0);
    call("DeleteCriticalSection", 1, cs);

    uint32_t slot = call("TlsAlloc", 0);
    CHECK(slot < RE5_TLS_SLOTS);
    CHECK_EQ_U32(call("TlsSetValue", 2, slot, 0xCAFEu), 1);
    call("SetLastError", 1, 5u);
    CHECK_EQ_U32(call("TlsGetValue", 1, slot), 0xCAFE);
    CHECK_EQ_U32(call("GetLastError", 0), 0);
    CHECK_EQ_U32(FS_RD32(&T->cpu, RE5_TEB_TLS_SLOTS + 4 * slot), 0xCAFE);
    CHECK_EQ_U32(call("TlsFree", 1, slot), 1);
    CHECK_EQ_U32(call("TlsGetValue", 1, slot), 0);

    uint32_t v = gbuf(4);
    wr32(v, 10);
    CHECK_EQ_U32(call("InterlockedIncrement", 1, v), 11);
    CHECK_EQ_U32(call("InterlockedDecrement", 1, v), 10);
    CHECK_EQ_U32(call("InterlockedExchangeAdd", 2, v, 5u), 10);
    CHECK_EQ_U32(rd32(v), 15);

    uint32_t ev = call("CreateEventA", 4, 0u, 0u, 1u, 0u);   /* auto-reset, sinalizado */
    CHECK(ev != 0);
    CHECK_EQ_U32(call("WaitForSingleObject", 2, ev, 0u), 0);
    CHECK_EQ_U32(call("WaitForSingleObject", 2, ev, 10u), 0x102);   /* WAIT_TIMEOUT */
    CHECK_EQ_U32(call("SetEvent", 1, ev), 1);
    uint32_t sem = call("CreateSemaphoreA", 4, 0u, 0u, 2u, 0u);
    uint32_t hs = gbuf(8);
    wr32(hs, sem);
    wr32(hs + 4, ev);
    CHECK_EQ_U32(call("WaitForMultipleObjects", 4, 2u, hs, 0u, 0u), 1);   /* o semáforo está em 0 */
    CHECK_EQ_U32(call("ReleaseSemaphore", 3, sem, 2u, v), 1);
    CHECK_EQ_U32(rd32(v), 0);
    CHECK_EQ_U32(call("ReleaseSemaphore", 3, sem, 1u, 0u), 0);           /* passaria do máximo */
    CHECK_EQ_U32(call("WaitForMultipleObjects", 4, 2u, hs, 1u, 0u), 0x102);   /* evento já consumido */
    CHECK_EQ_U32(call("CloseHandle", 1, sem), 1);
    CHECK_EQ_U32(call("CloseHandle", 1, ev), 1);
    CHECK_EQ_U32(call("CloseHandle", 1, ev), 0);

    uint32_t name = gstr("RE5_TEST_MUTEX");
    uint32_t m1 = call("CreateMutexA", 3, 0u, 1u, name);
    CHECK_EQ_U32(call("GetLastError", 0), 0);
    uint32_t m2 = call("CreateMutexA", 3, 0u, 0u, name);
    CHECK_EQ_U32(call("GetLastError", 0), 183);   /* ERROR_ALREADY_EXISTS */
    CHECK_EQ_U32(m1, m2);
    CHECK_EQ_U32(call("ReleaseMutex", 1, m1), 1);
    CHECK_EQ_U32(call("ReleaseMutex", 1, m1), 0);
    call("CloseHandle", 1, m1);
    call("CloseHandle", 1, m2);
}

static volatile uint32_t g_thread_param, g_thread_tid;
static void thread_fn(CPU *c)
{
    g_thread_param = re5_arg(c, 0);
    g_thread_tid = re5_thread_current()->tid;
    c->eax = 42;
}

static volatile uint32_t g_spin_count, g_spin_stop;
static void spin_fn(CPU *c)
{
    const uint32_t a[2] = {1, 0};
    while (!g_spin_stop) {
        g_spin_count++;
        re5_guest_call(re5_thread_current(), re5_import_bind("kernel32.dll", "SleepEx", 0), 2, a);
    }
    c->eax = 0;
}

static volatile uint32_t g_cs_va, g_cs_in, g_cs_tid;
static void cs_waiter_fn(CPU *c)
{
    re5_thread *t = re5_thread_current();
    g_cs_tid = t->tid;
    re5_guest_call(t, re5_import_bind("kernel32.dll", "EnterCriticalSection", 0), 1, (const uint32_t *)&g_cs_va);
    g_cs_in = 1;
    re5_guest_call(t, re5_import_bind("kernel32.dll", "LeaveCriticalSection", 0), 1, (const uint32_t *)&g_cs_va);
    c->eax = 0;
}

static void test_threads(void)
{
    uint32_t start = re5_thunk_new("teste!thread", thread_fn, 4, NULL), tid = gbuf(4), code = gbuf(4);
    uint32_t h = call("CreateThread", 6, 0u, 0u, start, 0x1234u, 4u /* CREATE_SUSPENDED */, tid);
    CHECK(h != 0);
    CHECK(rd32(tid) != 0 && rd32(tid) != T->tid);
    CHECK_EQ_U32(call("GetExitCodeThread", 2, h, code), 1);
    CHECK_EQ_U32(rd32(code), 259);                          /* STILL_ACTIVE */
    CHECK_EQ_U32(call("WaitForSingleObject", 2, h, 20u), 0x102);
    CHECK_EQ_U32(call("ResumeThread", 1, h), 1);
    CHECK_EQ_U32(call("WaitForSingleObject", 2, h, 0xFFFFFFFFu), 0);
    CHECK_EQ_U32(call("GetExitCodeThread", 2, h, code), 1);
    CHECK_EQ_U32(rd32(code), 42);
    CHECK_EQ_U32(g_thread_param, 0x1234);
    CHECK_EQ_U32(g_thread_tid, rd32(tid));
    CHECK_EQ_U32(call("CloseHandle", 1, h), 1);

    /* Fechar o handle logo depois de criar não pode derrubar a thread. */
    h = call("CreateThread", 6, 0u, 0u, start, 7u, 0u, 0u);
    CHECK_EQ_U32(call("CloseHandle", 1, h), 1);
    for (int i = 0; i < 1000 && g_thread_param != 7; i++) call("SleepEx", 2, 1u, 0u);
    CHECK_EQ_U32(g_thread_param, 7);

    /* SuspendThread de uma thread em execução: ela para no próximo lock do guest
     * (aqui, a volta de cada SleepEx) e só conta de novo depois do ResumeThread. */
    g_spin_stop = 0;
    g_spin_count = 0;
    uint32_t spin = re5_thunk_new("teste!spin", spin_fn, 4, NULL);
    h = call("CreateThread", 6, 0u, 0u, spin, 0u, 0u, 0u);
    for (int i = 0; i < 1000 && g_spin_count < 3; i++) call("SleepEx", 2, 1u, 0u);
    CHECK(g_spin_count >= 3);
    re5_guest_lock();   /* quem chama SuspendThread está rodando código do guest */
    CHECK_EQ_U32(call("SuspendThread", 1, h), 0);
    CHECK_EQ_U32(call("SuspendThread", 1, h), 1);
    uint32_t frozen = g_spin_count;
    re5_guest_unlock();
    call("SleepEx", 2, 50u, 0u);
    CHECK_EQ_U32(g_spin_count, frozen);
    CHECK_EQ_U32(call("ResumeThread", 1, h), 2);
    call("SleepEx", 2, 30u, 0u);
    CHECK_EQ_U32(g_spin_count, frozen);   /* ainda suspensa uma vez */
    CHECK_EQ_U32(call("ResumeThread", 1, h), 1);
    for (int i = 0; i < 1000 && g_spin_count == frozen; i++) call("SleepEx", 2, 1u, 0u);
    CHECK(g_spin_count > frozen);
    g_spin_stop = 1;
    CHECK_EQ_U32(call("WaitForSingleObject", 2, h, 0xFFFFFFFFu), 0);
    CHECK_EQ_U32(call("CloseHandle", 1, h), 1);

    /* Seção disputada: o Leave passa a seção direto para quem está na fila, senão
     * um laço Enter/Leave (2 dispatches por volta) deixaria a outra thread de fora. */
    uint32_t cs = gbuf(24);
    g_cs_va = cs;
    call("InitializeCriticalSection", 1, cs);
    call("EnterCriticalSection", 1, cs);
    uint32_t waiter = re5_thunk_new("teste!cs_waiter", cs_waiter_fn, 4, NULL);
    h = call("CreateThread", 6, 0u, 0u, waiter, 0u, 0u, 0u);
    call("SleepEx", 2, 30u, 0u);   /* a outra thread chega no Enter e entra na fila */
    CHECK_EQ_U32(g_cs_in, 0);
    call("LeaveCriticalSection", 1, cs);
    CHECK_EQ_U32(rd32(cs + 12), g_cs_tid);   /* OwningThread: já é dela */
    CHECK_EQ_U32(rd32(cs + 8), 1);           /* RecursionCount */
    call("EnterCriticalSection", 1, cs);     /* espera ela sair */
    CHECK_EQ_U32(g_cs_in, 1);
    call("LeaveCriticalSection", 1, cs);
    CHECK_EQ_U32(rd32(cs + 12), 0);
    CHECK_EQ_U32(call("WaitForSingleObject", 2, h, 0xFFFFFFFFu), 0);
    CHECK_EQ_U32(call("CloseHandle", 1, h), 1);
    call("DeleteCriticalSection", 1, cs);

    /* Lock do guest justo + preempção: com o lock em mãos a outra thread não anda;
     * depois de 2 ms, re5_guest_preempt dá a vez a quem está na fila. Só no modo
     * global: no hle o código do guest nem pega o lock (ver test_hle). */
    if (re5_guest_lock_mode_hle()) return;
    g_spin_stop = 0;
    g_spin_count = 0;
    re5_guest_lock();
    re5_guest_preempt();   /* ninguém na fila: não faz nada */
    h = call("CreateThread", 6, 0u, 0u, spin, 0u, 0u, 0u);
    clock_t t0 = clock();   /* ocupado sem soltar o lock e sem dispatch (o dispatch já preempta) */
    while ((clock() - t0) * 1000 / CLOCKS_PER_SEC < 20) {}
    CHECK_EQ_U32(g_spin_count, 0);
    re5_guest_preempt();
    CHECK(g_spin_count > 0);
    re5_guest_unlock();
    g_spin_stop = 1;
    CHECK_EQ_U32(call("WaitForSingleObject", 2, h, 0xFFFFFFFFu), 0);
    CHECK_EQ_U32(call("CloseHandle", 1, h), 1);
}

/* ---- RE5_GUEST_LOCK=hle: o código do guest roda em paralelo, o lock só no HLE ---- */

static volatile uint32_t g_flag, g_seen, g_counter_va, g_done;

/* "Código do guest" (thunk sem lock) que espera a outra thread passar por um handler com lock. */
static void wait_flag_fn(CPU *c)
{
    clock_t t0 = clock();
    while (!g_flag && (clock() - t0) * 1000 / CLOCKS_PER_SEC < 3000) {}
    g_seen = g_flag;
    c->eax = 0;
}

/* Handler com lock que chama o guest: no modo hle o guest roda sem o lock. */
static void callback_fn(CPU *c)
{
    static uint32_t wait;
    if (!wait) {
        wait = re5_thunk_new("teste!espera", wait_flag_fn, 0, NULL);
        re5_thunk_set_flags(wait, RE5_THUNK_NOLOCK);
    }
    re5_guest_call(re5_thread_current(), wait, 0, NULL);
    c->eax = 0;
}

static void set_flag_fn(CPU *c)   /* handler com lock, de outra thread */
{
    g_flag = 1;
    c->eax = 0;
}

static void setter_thread_fn(CPU *c)
{
    static uint32_t set;
    if (!set) set = re5_thunk_new("teste!liga", set_flag_fn, 0, NULL);
    re5_guest_call(re5_thread_current(), set, 0, NULL);
    c->eax = 0;
}

/* Início de thread sem lock: 100000 InterlockedIncrement de verdade em paralelo. */
static void incr_thread_fn(CPU *c)
{
    re5_thread *t = re5_thread_current();
    uint32_t inc = re5_import_bind("kernel32.dll", "InterlockedIncrement", 0), p = g_counter_va;
    for (int i = 0; i < 100000; i++) re5_guest_call(t, inc, 1, &p);
    __atomic_add_fetch(&g_done, 1u, __ATOMIC_SEQ_CST);
    c->eax = 0;
}

static void test_hle(void)
{
    if (!re5_guest_lock_mode_hle()) return;
    /* Handler -> guest -> espera outra thread entrar num handler: sem soltar o lock, isto travaria. */
    g_flag = g_seen = 0;
    uint32_t setter = re5_thunk_new("teste!setter", setter_thread_fn, 4, NULL);
    uint32_t cb = re5_thunk_new("teste!callback", callback_fn, 0, NULL);
    uint32_t h = call("CreateThread", 6, 0u, 0u, setter, 0u, 4u /* CREATE_SUSPENDED */, 0u);
    CHECK_EQ_U32(call("ResumeThread", 1, h), 1);
    re5_guest_call(T, cb, 0, NULL);
    CHECK_EQ_U32(g_seen, 1);
    CHECK_EQ_U32(call("WaitForSingleObject", 2, h, 0xFFFFFFFFu), 0);
    call("CloseHandle", 1, h);

    /* InterlockedIncrement sem lock e atômico: 4 threads x 100000 = 400000 exatos. */
    uint32_t counter = gbuf(4);
    wr32(counter, 0);
    g_counter_va = counter;
    g_done = 0;
    uint32_t incr = re5_thunk_new("teste!incr", incr_thread_fn, 4, NULL);
    re5_thunk_set_flags(incr, RE5_THUNK_NOLOCK);
    uint32_t hs[4];
    for (int i = 0; i < 4; i++) hs[i] = call("CreateThread", 6, 0u, 0u, incr, 0u, 0u, 0u);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_U32(call("WaitForSingleObject", 2, hs[i], 0xFFFFFFFFu), 0);
        call("CloseHandle", 1, hs[i]);
    }
    CHECK_EQ_U32(g_done, 4);
    CHECK_EQ_U32(rd32(counter), 400000);
}

static void test_nls(void)
{
    CHECK_EQ_U32(call("GetACP", 0), 1252);
    const uint8_t s1252[] = {'a', 0xE9, 0x80, 0};
    uint32_t src = gput(s1252, sizeof s1252), w = gbuf(64), back = gbuf(16);
    CHECK_EQ_U32(call("MultiByteToWideChar", 6, 0u, 0u, src, 0xFFFFFFFFu, 0u, 0u), 4);
    CHECK_EQ_U32(call("MultiByteToWideChar", 6, 0u, 0u, src, 0xFFFFFFFFu, w, 32u), 4);
    CHECK_EQ_U32(r16(w + 2), 0xE9);
    CHECK_EQ_U32(r16(w + 4), 0x20AC);
    CHECK_EQ_U32(call("WideCharToMultiByte", 8, 0u, 0u, w, 0xFFFFFFFFu, back, 16u, 0u, 0u), 4);
    CHECK(memcmp(re5_g2h(back), s1252, 4) == 0);
    CHECK_EQ_U32(call("MultiByteToWideChar", 6, 0u, 0u, src, 0xFFFFFFFFu, w, 2u), 0);
    CHECK_EQ_U32(call("GetLastError", 0), 122);

    uint32_t u8 = gstr("\xC3\xA9\xF0\x9F\x98\x80");   /* é + U+1F600 */
    CHECK_EQ_U32(call("MultiByteToWideChar", 6, 65001u, 0u, u8, 0xFFFFFFFFu, w, 32u), 4);
    CHECK_EQ_U32(r16(w + 2), 0xD83D);
    CHECK_EQ_U32(call("WideCharToMultiByte", 8, 65001u, 0u, w, 0xFFFFFFFFu, back, 16u, 0u, 0u), 7);
    CHECK(strcmp(re5_g2h(back), "\xC3\xA9\xF0\x9F\x98\x80") == 0);

    const uint16_t chars[] = {'A', 'z', '5', ' ', '!', 0xE9};
    uint32_t wc = gput(chars, sizeof chars), types = gbuf(16);
    CHECK_EQ_U32(call("GetStringTypeW", 4, 1u, wc, 6u, types), 1);
    CHECK_EQ_U32(r16(types + 0), 0x0200 | 0x0100 | 0x0080 | 0x0001);   /* A: hex, alpha, upper */
    CHECK_EQ_U32(r16(types + 2), 0x0200 | 0x0100 | 0x0002);
    CHECK_EQ_U32(r16(types + 4), 0x0200 | 0x0004 | 0x0080);
    CHECK_EQ_U32(r16(types + 6), 0x0200 | 0x0008 | 0x0040);
    CHECK_EQ_U32(r16(types + 8), 0x0200 | 0x0010);
    CHECK_EQ_U32(r16(types + 10), 0x0200 | 0x0100 | 0x0002);

    uint32_t up = gbuf(16);
    CHECK_EQ_U32(call("LCMapStringA", 6, 0x409u, 0x200u, gstr("abc\xE9"), 0xFFFFFFFFu, up, 16u), 5);
    CHECK(strcmp(re5_g2h(up), "ABC\xC9") == 0);
    CHECK_EQ_U32(call("CompareStringA", 6, 0x409u, 1u, gstr("Hello"), 0xFFFFFFFFu, gstr("hELLO"), 0xFFFFFFFFu), 2);
    CHECK_EQ_U32(call("CompareStringA", 6, 0x409u, 0u, gstr("abc"), 0xFFFFFFFFu, gstr("abd"), 0xFFFFFFFFu), 1);

    uint32_t cpinfo = gbuf(20);
    CHECK_EQ_U32(call("GetCPInfo", 2, 1252u, cpinfo), 1);
    CHECK_EQ_U32(rd32(cpinfo), 1);
    uint32_t lbuf = gbuf(16);
    CHECK_EQ_U32(call("GetLocaleInfoA", 4, 0x409u, 0x1004u, lbuf, 16u), 5);
    CHECK(strcmp(re5_g2h(lbuf), "1252") == 0);
}

static void test_env(void)
{
    uint32_t osvi = gbuf(156);
    wr32(osvi, 156);
    CHECK_EQ_U32(call("GetVersionExA", 1, osvi), 1);
    CHECK_EQ_U32(rd32(osvi + 4), 6);
    CHECK_EQ_U32(rd32(osvi + 8), 1);
    CHECK_EQ_U32(rd32(osvi + 16), 2);

    /* "Windows XP SP2 ou mais novo?" sim; "Windows 8 ou mais novo?" não. */
    CPU *c = &T->cpu;
    uint32_t mlo = 0, mhi = 0;
    const uint32_t types[3] = {2u, 1u, 0x20u};
    for (int i = 0; i < 3; i++) {
        uint32_t a[4] = {mlo, mhi, types[i], 3u};
        mlo = re5_guest_call(T, re5_import_bind("kernel32.dll", "VerSetConditionMask", 0), 4, a);
        mhi = c->edx;
    }
    uint32_t req = gbuf(156);
    wr32(req, 156);
    wr32(req + 4, 5);
    wr32(req + 8, 1);
    wr16(req + 148, 2);
    CHECK_EQ_U32(call("VerifyVersionInfoA", 4, req, 0x23u, mlo, mhi), 1);
    wr32(req + 4, 6);
    wr32(req + 8, 2);
    wr16(req + 148, 0);
    CHECK_EQ_U32(call("VerifyVersionInfoA", 4, req, 0x23u, mlo, mhi), 0);
    CHECK_EQ_U32(call("GetLastError", 0), 1150);

    /* 2009-03-05 12:34:56.789 (quinta-feira) ida e volta */
    uint32_t st = gbuf(16), ft = gbuf(8), st2 = gbuf(16);
    const uint16_t date[8] = {2009, 3, 0, 5, 12, 34, 56, 789};
    memcpy(re5_g2h(st), date, 16);
    CHECK_EQ_U32(call("SystemTimeToFileTime", 2, st, ft), 1);
    CHECK_EQ_U32(call("FileTimeToSystemTime", 2, ft, st2), 1);
    CHECK_EQ_U32(r16(st2 + 0), 2009);
    CHECK_EQ_U32(r16(st2 + 2), 3);
    CHECK_EQ_U32(r16(st2 + 4), 4);
    CHECK_EQ_U32(r16(st2 + 6), 5);
    CHECK_EQ_U32(r16(st2 + 8), 12);
    CHECK_EQ_U32(r16(st2 + 14), 789);
    call("GetSystemTimeAsFileTime", 1, ft);
    CHECK(rd32(ft + 4) > 0x01C00000u);   /* depois de ~2001 */

    uint32_t mod = gbuf(64);
    CHECK_EQ_U32(call("GetModuleFileNameA", 3, 0u, mod, 64u), strlen("C:\\RE5\\re5dx9.exe"));
    CHECK(strcmp(re5_g2h(mod), "C:\\RE5\\re5dx9.exe") == 0);
    CHECK_EQ_U32(call("GetModuleFileNameA", 3, 0u, mod, 4u), 4);
    CHECK(strcmp(re5_g2h(mod), "C:\\") == 0);
    CHECK(strcmp(re5_gstr(call("GetCommandLineA", 0)), "\"C:\\RE5\\re5dx9.exe\"") == 0 ||
          getenv("RE5_GUEST_ARGS") != NULL);

    CHECK_EQ_U32(call("SetEnvironmentVariableA", 2, gstr("RE5_X"), gstr("1")), 1);
    uint32_t env = call("GetEnvironmentStringsW", 0);
    int found_os = 0, found_x = 0;
    for (uint32_t p = env; r16(p);) {
        char line[128];
        uint32_t n = 0;
        while (r16(p) && n < sizeof line - 1) { line[n++] = (char)r16(p); p += 2; }
        line[n] = 0;
        p += 2;
        found_os |= strcmp(line, "OS=Windows_NT") == 0;
        found_x |= strcmp(line, "RE5_X=1") == 0;
    }
    CHECK(found_os && found_x);
    CHECK_EQ_U32(call("FreeEnvironmentStringsW", 1, env), 1);

    uint32_t si = gbuf(36);
    call("GetSystemInfo", 1, si);
    CHECK_EQ_U32(rd32(si + 4), 4096);
    CHECK_EQ_U32(rd32(si + 28), 0x10000);
    /* Núcleos: 2 com o lock global; no hle, os do host (1..8). Máscara coerente. */
    uint32_t ncpu = rd32(si + 20);
    if (re5_guest_lock_mode_hle()) CHECK(ncpu >= 1 && ncpu <= 8);
    else CHECK_EQ_U32(ncpu, 2);
    CHECK_EQ_U32(rd32(si + 16), (1u << ncpu) - 1);
    uint32_t sti = gbuf(68);
    call("GetStartupInfoA", 1, sti);
    CHECK_EQ_U32(rd32(sti), 68);
    CHECK_EQ_U32(call("GetStdHandle", 1, 0xFFFFFFF5u), 0);   /* GUI: sem console */
}

static uint32_t g_apc_bytes, g_apc_calls;
static void apc_routine(CPU *c)
{
    CHECK_EQ_U32(re5_arg(c, 0), 0);   /* dwErrorCode */
    g_apc_bytes = re5_arg(c, 1);
    g_apc_calls++;
}

static void test_files(const char *root)
{
#if defined(_WIN32)
    _mkdir(root);
#else
    mkdir(root, 0755);
#endif
    CHECK(re5_vfs_init(root, NULL) == 0);
    uint32_t name = gstr("C:\\RE5\\Teste.bin");
    uint32_t h = call("CreateFileA", 7, name, 0xC0000000u, 0u, 0u, 2u /* CREATE_ALWAYS */, 0x80u, 0u);
    CHECK(h != 0xFFFFFFFFu);
    uint32_t data = gstr("0123456789"), done = gbuf(4), back = gbuf(16);
    CHECK_EQ_U32(call("WriteFile", 5, h, data, 10u, done, 0u), 1);
    CHECK_EQ_U32(rd32(done), 10);
    CHECK_EQ_U32(call("SetFilePointer", 4, h, 2u, 0u, 0u), 2);
    CHECK_EQ_U32(call("ReadFile", 5, h, back, 3u, done, 0u), 1);
    CHECK(memcmp(re5_g2h(back), "234", 3) == 0);
    CHECK_EQ_U32(call("SetFilePointer", 4, h, 0xFFFFFFFEu, 0u, 2u), 8);   /* -2 do fim */
    CHECK_EQ_U32(call("SetEndOfFile", 1, h), 1);
    CHECK_EQ_U32(call("GetFileSize", 2, h, 0u), 8);
    CHECK_EQ_U32(call("CloseHandle", 1, h), 1);

    /* Abrir sem diferenciar maiúsculas, ler com OVERLAPPED no deslocamento. */
    h = call("CreateFileA", 7, gstr("teste.BIN"), 0x80000000u, 1u, 0u, 3u /* OPEN_EXISTING */, 0x40000000u, 0u);
    CHECK(h != 0xFFFFFFFFu);
    uint32_t ov = gbuf(20);
    wr32(ov + 8, 5);
    CHECK_EQ_U32(call("ReadFile", 5, h, back, 2u, 0u, ov), 1);
    CHECK(memcmp(re5_g2h(back), "56", 2) == 0);
    CHECK_EQ_U32(rd32(ov + 4), 2);
    wr32(ov + 8, 1);
    uint32_t routine = re5_thunk_new("teste!apc", apc_routine, 12, NULL);
    CHECK_EQ_U32(call("ReadFileEx", 5, h, back, 4u, ov, routine), 1);
    CHECK_EQ_U32(g_apc_calls, 0);                        /* só na espera alertável */
    CHECK_EQ_U32(call("SleepEx", 2, 0u, 0u), 0);
    CHECK_EQ_U32(g_apc_calls, 0);
    CHECK_EQ_U32(call("SleepEx", 2, 1000u, 1u), 0xC0);   /* WAIT_IO_COMPLETION, sem dormir */
    CHECK_EQ_U32(g_apc_calls, 1);
    CHECK_EQ_U32(g_apc_bytes, 4);
    CHECK(memcmp(re5_g2h(back), "1234", 4) == 0);
    call("CloseHandle", 1, h);
    CHECK_EQ_U32(call("CreateFileA", 7, gstr("nao_existe.bin"), 0x80000000u, 0u, 0u, 3u, 0u, 0u), 0xFFFFFFFFu);
    CHECK_EQ_U32(call("GetLastError", 0), 2);

    CHECK_EQ_U32(call("CreateDirectoryA", 2, gstr("C:\\RE5\\sub"), 0u), 1);
    CHECK_EQ_U32(call("CreateDirectoryA", 2, gstr("C:\\RE5\\sub"), 0u), 0);
    CHECK_EQ_U32(call("GetLastError", 0), 183);
    CHECK_EQ_U32(call("GetFileAttributesA", 1, gstr("C:\\RE5\\SUB")), 0x10);
    CHECK_EQ_U32(call("GetFileAttributesA", 1, gstr("C:\\RE5\\teste.bin")), 0x20);
    CHECK_EQ_U32(call("GetFileAttributesA", 1, gstr("C:\\RE5\\nada")), 0xFFFFFFFFu);

    uint32_t fd = gbuf(320);
    uint32_t fh = call("FindFirstFileA", 2, gstr("C:\\RE5\\*.BIN"), fd);
    CHECK(fh != 0xFFFFFFFFu);
    CHECK(strcmp(re5_gstr(fd + 44), "Teste.bin") == 0);
    CHECK_EQ_U32(rd32(fd + 32), 8);
    CHECK_EQ_U32(call("FindNextFileA", 2, fh, fd), 0);
    CHECK_EQ_U32(call("GetLastError", 0), 18);   /* ERROR_NO_MORE_FILES */
    CHECK_EQ_U32(call("FindClose", 1, fh), 1);

    uint32_t ini = gstr("C:\\RE5\\config.ini"), out = gbuf(64);
    CHECK_EQ_U32(call("WritePrivateProfileStringA", 4, gstr("Video"), gstr("Width"), gstr("1280"), ini), 1);
    CHECK_EQ_U32(call("WritePrivateProfileStringA", 4, gstr("Video"), gstr("Height"), gstr("720"), ini), 1);
    CHECK_EQ_U32(call("WritePrivateProfileStringA", 4, gstr("Audio"), gstr("Vol"), gstr("8"), ini), 1);
    CHECK_EQ_U32(call("WritePrivateProfileStringA", 4, gstr("video"), gstr("WIDTH"), gstr("1920"), ini), 1);
    CHECK_EQ_U32(call("GetPrivateProfileStringA", 6, gstr("VIDEO"), gstr("width"), gstr("x"), out, 64u, ini), 4);
    CHECK(strcmp(re5_gstr(out), "1920") == 0);
    CHECK_EQ_U32(call("GetPrivateProfileStringA", 6, gstr("Video"), gstr("Height"), gstr("x"), out, 64u, ini), 3);
    CHECK_EQ_U32(call("GetPrivateProfileStringA", 6, gstr("Video"), gstr("Nada"), gstr("padrao"), out, 64u, ini), 6);
    CHECK(strcmp(re5_gstr(out), "padrao") == 0);
    CHECK_EQ_U32(call("GetPrivateProfileStringA", 6, 0u, 0u, 0u, out, 64u, ini), 12);   /* "Video\0Audio\0\0" */
    CHECK(strcmp(re5_gstr(out), "Video") == 0 && strcmp(re5_gstr(out + 6), "Audio") == 0);
    CHECK_EQ_U32(call("WritePrivateProfileStringA", 4, gstr("Video"), gstr("Height"), 0u, ini), 1);
    CHECK_EQ_U32(call("GetPrivateProfileStringA", 6, gstr("Video"), gstr("Height"), gstr("-"), out, 64u, ini), 1);

    CHECK_EQ_U32(call("DeleteFileA", 1, gstr("C:\\RE5\\teste.bin")), 1);
    CHECK_EQ_U32(call("GetFileAttributesA", 1, gstr("C:\\RE5\\teste.bin")), 0xFFFFFFFFu);
}

int main(int argc, char **argv)
{
    CHECK(re5_runtime_init() == 0);
    CHECK(re5_process_init(0x00400000u) == 0);
    T = re5_thread_attach(1u << 20);
    CHECK(T != NULL);
    g_scratch = re5_mem_alloc(0x10000u, RE5_PAGE_READWRITE, "teste");
    CHECK(g_scratch != 0);

    test_heap();
    test_virtual();
    test_sync();
    test_threads();
    test_hle();
    test_nls();
    test_env();
    if (argc > 1) test_files(argv[1]);

    /* winmm: timeGetTime anda com o GetTickCount; timeBeginPeriod valida 1..1000 ms. */
    uint32_t tick = call("GetTickCount", 0);
    uint32_t mm = re5_guest_call(T, re5_import_bind("winmm.dll", "timeGetTime", 0), 0, NULL);
    CHECK(mm - tick < 1000u);
    const uint32_t p1 = 1, p0 = 0;
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("winmm.dll", "timeBeginPeriod", 0), 1, &p1), 0);
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("winmm.dll", "timeBeginPeriod", 0), 1, &p0), 97);
    CHECK_EQ_U32(re5_guest_call(T, re5_import_bind("winmm.dll", "timeEndPeriod", 0), 1, &p1), 0);

    /* Processo de janela sem console; lstr*; BSTR do oleaut32. */
    CHECK_EQ_U32(call("GetConsoleCP", 0), 0);
    CHECK_EQ_U32(call("GetLastError", 0), 6);
    uint32_t wr = gbuf(4);
    wr32(wr, 77);
    CHECK_EQ_U32(call("WriteConsoleA", 5, 0x10u, gstr("x"), 1u, wr, 0u), 0);
    CHECK_EQ_U32(rd32(wr), 0);
    uint32_t src = gstr("abc"), dst = gbuf(8);
    CHECK_EQ_U32(call("lstrlenA", 1, src), 3);
    CHECK_EQ_U32(call("lstrlenA", 1, 0u), 0);
    CHECK_EQ_U32(call("lstrcpyA", 2, dst, src), dst);
    CHECK(strcmp(re5_g2h(dst), "abc") == 0);
    const uint16_t wide[] = {'R', 'E', '5', 0};
    uint32_t ws = gput(wide, sizeof wide);
    uint32_t bstr = re5_guest_call(T, re5_import_bind("oleaut32.dll", "SysAllocString", 0), 1, &ws);
    CHECK(bstr != 0);
    CHECK_EQ_U32(rd32(bstr - 4), 6);   /* tamanho em bytes, sem o terminador */
    CHECK_EQ_U32(r16(bstr + 4), '5');
    CHECK_EQ_U32(r16(bstr + 6), 0);
    re5_guest_call(T, re5_import_bind("oleaut32.dll", "SysFreeString", 0), 1, &bstr);

    re5_census cs = re5_import_census();
    CHECK_EQ_U32(cs.bound, cs.implemented);   /* nenhum import chamado aqui está sem handler */
    CHECK_DONE();
}
