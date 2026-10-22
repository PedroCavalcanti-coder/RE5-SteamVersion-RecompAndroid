/* HANDLEs de 32 bits: valores, tipos, duplicação e destruição na última referência. */
#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

static int destroyed;
static void destroy(void *obj) { destroyed += *(int *)obj; }

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

    CHECK_DONE();
}
