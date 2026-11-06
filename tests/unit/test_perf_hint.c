/* Núcleos grandes pelo cpuinfo_max_freq (re5/perf_hint.h), num sysfs falso montado aqui. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "check.h"
#include "re5/perf_hint.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

static void fake_cpus(const char *root, const unsigned long *freq, int n)
{
    char p[512];
    MKDIR(root);
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/cpu%d", root, i);
        MKDIR(p);
        snprintf(p, sizeof p, "%s/cpu%d/cpufreq", root, i);
        MKDIR(p);
        snprintf(p, sizeof p, "%s/cpu%d/cpufreq/cpuinfo_max_freq", root, i);
        FILE *f = fopen(p, "w");
        CHECK(f != NULL);
        if (f) {
            fprintf(f, "%lu\n", freq[i]);
            fclose(f);
        }
    }
}

int main(void)
{
    /* Galaxy A54 (Exynos 1380): cpu0-3 A55 a 2,0 GHz, cpu4-7 A78 a 2,4 GHz. */
    const unsigned long a54[8] = {2002000, 2002000, 2002000, 2002000, 2400000, 2400000, 2400000, 2400000};
    fake_cpus("perf_sys_a54", a54, 8);
    CHECK(re5_perf_big_mask("perf_sys_a54") == 0xF0u);

    /* 1 + 3 + 4 (Snapdragon com Prime): Prime e médios (fora do cluster lento). */
    const unsigned long sd[8] = {1800000, 1800000, 1800000, 1800000, 2500000, 2500000, 2500000, 3200000};
    fake_cpus("perf_sys_sd", sd, 8);
    CHECK(re5_perf_big_mask("perf_sys_sd") == 0xF0u);

    /* Todos iguais (PC, emulador): nada a separar. */
    const unsigned long eq[4] = {3000000, 3000000, 3000000, 3000000};
    fake_cpus("perf_sys_eq", eq, 4);
    CHECK(re5_perf_big_mask("perf_sys_eq") == 0);

    /* Sem sysfs. */
    CHECK(re5_perf_big_mask("perf_sys_nao_existe") == 0);

    /* Alvo do ADPF: sem limite pede 60 fps (antes 30, que fazia o sistema baixar o clock). */
    CHECK(re5_perf_target_ns(NULL) == 1000000000LL / 60);
    CHECK(re5_perf_target_ns("") == 1000000000LL / 60);
    CHECK(re5_perf_target_ns("30") == 1000000000LL / 30);
    CHECK(re5_perf_target_ns("45") == 1000000000LL / 45);
    CHECK(re5_perf_target_ns("10") == 1000000000LL / 60);    /* fora de 30..240: como sem limite */
    CHECK(re5_perf_target_ns("1000") == 1000000000LL / 60);
    /* A espera do limitador não é trabalho. */
    CHECK(re5_perf_work_ns(33333333, 13333333) == 20000000);
    CHECK(re5_perf_work_ns(20000000, 0) == 20000000);
    CHECK(re5_perf_work_ns(10, 50) == 1);

    /* Fora do Android os ganchos não fazem nada (nem falham). */
    re5_perf_note_render();
    re5_perf_note_present(0);
    CHECK_DONE();
}
