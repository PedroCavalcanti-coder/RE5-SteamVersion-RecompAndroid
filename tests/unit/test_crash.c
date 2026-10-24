/*
 * Estouro da pilha do host (Windows): o filtro do crash.c precisa de pilha de
 * reserva (SetThreadStackGuarantee nas threads do runtime) e mostra o ciclo da
 * recursão como funções do guest. Sem a reserva o processo sumia sem log (RE5,
 * 2026-10-24: 0xC0000005 em ntdll!__chkstk).
 *
 * O teste roda a si mesmo como filho ("filho"), que estoura a pilha numa thread
 * de 1 MiB criada pelo runtime; o pai lê o RE5_LOG_FILE do filho.
 */
#include <process.h>
#include <string.h>
#include <windows.h>

#include "check.h"
#include "re5/runtime.h"

static void L_00402000(CPU *c);
static void (*volatile g_next)(CPU *) = L_00402000;
static CPU g_cpu;

/* Recursão sem fim no formato do código gerado: uma chamada C por call do guest. */
static void L_00402000(CPU *c)
{
    volatile char pad[256];
    pad[0] = (char)c->eax;
    c->eax++;
    g_next(c);
    pad[1] = pad[0];
}

const re5_func_entry re5_lifted_table[] = {
    {0x00402000u, L_00402000},
};
const uint32_t re5_lifted_count = 1;

static void deep(void *arg)
{
    (void)arg;
    L_00402000(&g_cpu);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "filho") == 0) {
        re5_log_init();
        re5_crash_handler_install();
        re5_host_thread_run(deep, NULL, (size_t)1 << 20);
        return 0;                                   /* não chega aqui */
    }

    char dir[MAX_PATH], log[MAX_PATH], env[MAX_PATH + 16];
    CHECK(GetTempPathA(sizeof dir, dir) > 0);
    snprintf(log, sizeof log, "%sre5_test_crash_%lu.log", dir, (unsigned long)GetCurrentProcessId());
    snprintf(env, sizeof env, "RE5_LOG_FILE=%s", log);
    _putenv(env);
    intptr_t rc = _spawnl(_P_WAIT, argv[0], argv[0], "filho", NULL);
    CHECK(rc != 0);                                 /* o filho morre pela exceção */

    static char text[1 << 16];
    FILE *f = fopen(log, "r");
    CHECK(f != NULL);
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    remove(log);
    CHECK(strstr(text, "estouro da pilha do host") != NULL);
    CHECK(strstr(text, "pilha do host (topo primeiro") != NULL);
    CHECK(strstr(text, "L_00402000") != NULL);
    CHECK(strstr(text, "(mais ") != NULL);          /* o ciclo aparece resumido */
    if (check_failures) fprintf(stderr, "log do filho:\n%s\n", text);
    CHECK_DONE();
}
