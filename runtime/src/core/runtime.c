/*
 * runtime.c - inicialização do runtime (ver re5/runtime.h).
 */
#include "re5/runtime.h"

int re5_runtime_init(void)
{
    re5_log_init();
    re5_crash_handler_install();
    if (re5_mem_init() != 0) return -1;
    if (re5_thunks_init() != 0) {
        RE5_ERROR("runtime", "falha ao criar a faixa de thunks");
        return -1;
    }
    re5_names_init();
    re5_lookup(0);   /* monta a tabela hash do dispatch antes de haver threads do guest */
    re5_watchdog_start();
    re5_profile_start();
    return 0;
}
