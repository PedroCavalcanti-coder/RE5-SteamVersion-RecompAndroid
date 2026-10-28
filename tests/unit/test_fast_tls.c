/* re5/fast_tls.h: cada thread vê o próprio valor; slot novo começa em zero em toda thread. */
#include <pthread.h>
#include <stdint.h>

#include "check.h"
#include "re5/runtime.h"
#include "re5/fast_tls.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

RE5_TLS_PTR(uintptr_t, t_val)
RE5_TLS_PTR(void *, t_other)

#define NT 8
static int g_bad[NT];

static void *worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    if (t_val_get() != 0 || t_other_get() != NULL) g_bad[id]++;
    for (uintptr_t i = 0; i < 100000; i++) {
        t_val_set(id * 1000003u + i);
        if (t_val_get() != id * 1000003u + i) g_bad[id]++;
    }
    return NULL;
}

int main(void)
{
    CHECK(t_val_get() == 0);
    t_val_set(42);
    pthread_t th[NT];
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)i);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < NT; i++) CHECK_EQ_U32((uint32_t)g_bad[i], 0u);
    CHECK(t_val_get() == 42);   /* as outras threads não mexeram no valor desta */
    return 0;
}
