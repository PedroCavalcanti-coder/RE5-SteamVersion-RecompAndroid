/*
 * Teste de ponta a ponta: o fx.exe sintético passou por recon.py ->
 * run_lift.py catalog -> run_lift.py lift; aqui o código gerado roda no
 * runtime real (memória do guest com base deslocada, loader PE, IAT ligada a
 * thunks do HLE, TEB via fs:, thread do host com pilha grande).
 */
#include <stdio.h>

#include "../unit/check.h"
#include "re5/runtime.h"

#define EXPECTED 555u   /* ver tests/e2e/mini_pe/fx.c */

typedef struct { uint32_t entry, result, esp_before, esp_after; } run_args;

static void run_guest(void *p)
{
    run_args *a = p;
    re5_thread *t = re5_thread_attach(1u << 20);
    if (!t) return;
    a->esp_before = t->cpu.esp;
    re5_guest_lock();
    a->result = re5_guest_call(t, a->entry, 0, NULL);
    re5_guest_unlock();
    a->esp_after = t->cpu.esp;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: test_mini_pe <fx.exe>\n");
        return 2;
    }
    CHECK(re5_runtime_init() == 0);
    re5_image img;
    const char *err = NULL;
    int rc = re5_image_load(argv[1], &img, &err);
    if (rc != 0) fprintf(stderr, "load: %s\n", err);
    CHECK(rc == 0);
    CHECK_EQ_U32(img.base, 0x00400000u);
    CHECK(re5_lookup(img.entry) != NULL);

    re5_census cs = re5_import_census();
    CHECK_EQ_U32(cs.bound, 4);
    CHECK_EQ_U32(cs.implemented, 4);
    CHECK(re5_process_init(img.base) == 0);

    run_args a = {img.entry, 0, 0, 0};
    CHECK(re5_host_thread_run(run_guest, &a, (size_t)64 << 20) == 0);
    CHECK_EQ_U32(a.result, EXPECTED);
    CHECK_EQ_U32(a.esp_after, a.esp_before);

    CHECK_DONE();
}
