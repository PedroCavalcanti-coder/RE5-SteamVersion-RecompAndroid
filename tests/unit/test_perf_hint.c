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

    /* Dimensity 7030 (6x A55 2.0 GHz + 2x A78 2.5 GHz): isola apenas os 2 núcleos A78. */
    const unsigned long d7030[8] = {2000000, 2000000, 2000000, 2000000, 2000000, 2000000, 2500000, 2500000};
    fake_cpus("perf_sys_d7030", d7030, 8);
    CHECK(re5_perf_big_mask("perf_sys_d7030") == 0xC0u);
    /* render presa só com 3+ núcleos grandes, salvo RE5_PIN_RENDER */
    CHECK(re5_perf_pin_render(0xF0u, NULL) == 1);   /* A54: 4 A78 */
    CHECK(re5_perf_pin_render(0xC0u, NULL) == 0);   /* 7030: 2 A78, ficam para a principal */
    CHECK(re5_perf_pin_render(0xC0u, "1") == 1);
    CHECK(re5_perf_pin_render(0xF0u, "0") == 0);
    CHECK(re5_perf_pin_render(0, "1") == 0);

    /* Snapdragon 8 Gen 4 / 8 Elite (6x Oryon Perf 3.53 GHz + 2x Oryon Prime 4.32 GHz):
     * sem núcleos LITTLE -> não deve confinar o jogo em apenas 2 Prime (retorna 0). */
    const unsigned long sd8g4[8] = {3530000, 3530000, 3530000, 3530000, 3530000, 3530000, 4320000, 4320000};
    fake_cpus("perf_sys_sd8g4", sd8g4, 8);
    CHECK(re5_perf_big_mask("perf_sys_sd8g4") == 0);

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

    /* ADPF com GPU (Android 15+): a espera no Present é a GPU; a CPU é o resto do quadro. */
    {
        int64_t t, c, g;
        re5_perf_split(70000000, 0, 45000000, &t, &c, &g);   /* quadro de 70 ms, 45 ms no Present */
        CHECK(t == 70000000 && g == 45000000 && c == 25000000);
        re5_perf_split(33333333, 13333333, 5000000, &t, &c, &g);   /* limitador fora do total */
        CHECK(t == 20000000 && g == 5000000 && c == 15000000);
        re5_perf_split(20000000, 0, 30000000, &t, &c, &g);   /* Present maior que o quadro: GPU = total */
        CHECK(t == 20000000 && g == 20000000 && c == 1);
        re5_perf_split(10, 50, 0, &t, &c, &g);   /* tudo limitador: nunca 0 (o ADPF exige > 0) */
        CHECK(t == 1 && g == 0 && c == 1);
    }

    /* Fora do Android os ganchos não fazem nada (nem falham). */
    re5_perf_note_render();
    re5_perf_note_present(0, 0);
    CHECK_DONE();
}
