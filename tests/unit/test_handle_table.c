/* HANDLEs de 32 bits: valores, tipos, duplicação e destruição na última referência. */
#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

static int destroyed;
static void destroy(void *obj) { destroyed += *(int *)obj; }

/* Leitura sem mutex: uma thread cria e fecha handles alternando tipo e objeto enquanto outra
 * lê o mesmo índice; o leitor nunca pode ver o tipo de um estado com o objeto de outro. */
static int g_tag[RE5_H_THREAD + 1];
static volatile int g_stop, g_bad, g_reads;
static volatile uint32_t g_h;

static void writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200000; i++) {
        re5_handle_type t = (i & 1) ? RE5_H_EVENT : RE5_H_MUTEX;
        uint32_t h = re5_handle_new(t, &g_tag[t], NULL);
        g_h = h;
        re5_handle_close(h);
    }
    g_stop = 1;
}

static void reader(void *arg)
{
    (void)arg;
    while (!g_stop) {
        uint32_t h = g_h;
        void *e = re5_handle_get(h, RE5_H_EVENT), *m = re5_handle_get(h, RE5_H_MUTEX);
        if ((e && e != &g_tag[RE5_H_EVENT]) || (m && m != &g_tag[RE5_H_MUTEX])) g_bad++;
        g_reads++;
    }
    g_stop = 2;
}

int main(void)
{
    int x = 1, y = 10;
    uint32_t h1 = re5_handle_new(RE5_H_FILE, &x, destroy);
    uint32_t h2 = re5_handle_new(RE5_H_EVENT, &y, destroy);
    CHECK(h1 && h2 && h1 != h2);
    CHECK((h1 & 3u) == 0 && h1 >= 0x100u);
    CHECK(h1 != RE5_PSEUDO_PROCESS && h1 != RE5_PSEUDO_THREAD);
    CHECK(re5_handle_get(h1, RE5_H_FILE) == &x);
    CHECK(re5_handle_get(h1, RE5_H_EVENT) == NULL);   /* tipo errado */
    CHECK(re5_handle_get(h1, RE5_H_ANY) == &x);
    CHECK(re5_handle_get(0, RE5_H_ANY) == NULL);
    CHECK(re5_handle_get(h1 + 1, RE5_H_ANY) == NULL);

    CHECK_EQ_U32(re5_handle_dup(h1), h1);
    CHECK(re5_handle_close(h1) == 0);
    CHECK_EQ_U32(destroyed, 0);                       /* ainda há uma referência */
    CHECK(re5_handle_close(h1) == 0);
    CHECK_EQ_U32(destroyed, 1);
    CHECK(re5_handle_close(h1) != 0);                 /* já fechado */
    CHECK(re5_handle_get(h1, RE5_H_ANY) == NULL);

    uint32_t h3 = re5_handle_new(RE5_H_MUTEX, &x, NULL);
    CHECK_EQ_U32(h3, h1);                             /* slot reaproveitado, como no Windows */
    CHECK_EQ_U32(re5_handle_live_count(), 2);
    CHECK(re5_handle_close(h2) == 0);
    CHECK_EQ_U32(destroyed, 11);

    /* Mais de um bloco de slots (1024): handles antigos continuam válidos quando a tabela cresce. */
    static uint32_t many[3000];
    for (int i = 0; i < 3000; i++) many[i] = re5_handle_new(RE5_H_FILE, &g_tag[RE5_H_FILE], NULL);
    for (int i = 0; i < 3000; i++) CHECK(re5_handle_get(many[i], RE5_H_FILE) == &g_tag[RE5_H_FILE]);
    for (int i = 0; i < 3000; i++) CHECK(re5_handle_close(many[i]) == 0);
    CHECK_EQ_U32(re5_handle_live_count(), 1);

    CHECK(re5_host_thread_start(reader, NULL, 1u << 20) == 0);
    CHECK(re5_host_thread_start(writer, NULL, 1u << 20) == 0);
    while (g_stop != 2) re5_thread_yield();
    CHECK(g_reads > 0);
    CHECK_EQ_U32((uint32_t)g_bad, 0);

    CHECK_DONE();
}
