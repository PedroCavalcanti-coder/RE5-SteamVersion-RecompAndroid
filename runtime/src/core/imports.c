/*
 * imports.c - registro de imports do HLE e censo (ver re5/imports.h).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/imports.h"
#include "re5/log.h"
#include "host_sync.h"

/* Cada runtime/src/win32/<dll>.c define um destes. Novo módulo: declare aqui
 * e acrescente em k_modules. Uma DLL grande pode ter várias tabelas (uma por
 * arquivo, ex.: kernel32_heap.c): todas com o mesmo nome de DLL entram. */
extern const re5_import_module re5_mod_advapi32;
extern const re5_import_module re5_mod_avifil32;
extern const re5_import_module re5_mod_d3d9;
extern const re5_import_module re5_mod_dinput8;
extern const re5_import_module re5_mod_dsound;
extern const re5_import_module re5_mod_gdi32;
extern const re5_import_module re5_mod_iphlpapi;
extern const re5_import_module re5_mod_kernel32;
extern const re5_import_module re5_mod_kernel32_env;
extern const re5_import_module re5_mod_kernel32_file;
extern const re5_import_module re5_mod_kernel32_heap;
extern const re5_import_module re5_mod_kernel32_nls;
extern const re5_import_module re5_mod_kernel32_sync;
extern const re5_import_module re5_mod_msvfw32;
extern const re5_import_module re5_mod_ole32;
extern const re5_import_module re5_mod_oleaut32;
extern const re5_import_module re5_mod_shell32;
extern const re5_import_module re5_mod_steam_api;
extern const re5_import_module re5_mod_user32;
extern const re5_import_module re5_mod_winmm;
extern const re5_import_module re5_mod_wmvcore;
extern const re5_import_module re5_mod_ws2_32;
extern const re5_import_module re5_mod_ws2_32_sock;
extern const re5_import_module re5_mod_xinput1_3;

static const re5_import_module *const k_modules[] = {
    &re5_mod_advapi32,
    &re5_mod_avifil32,
    &re5_mod_d3d9,
    &re5_mod_dinput8,
    &re5_mod_dsound,
    &re5_mod_gdi32,
    &re5_mod_iphlpapi,
    &re5_mod_kernel32,
    &re5_mod_kernel32_env,
    &re5_mod_kernel32_file,
    &re5_mod_kernel32_heap,
    &re5_mod_kernel32_nls,
    &re5_mod_kernel32_sync,
    &re5_mod_msvfw32,
    &re5_mod_ole32,
    &re5_mod_oleaut32,
    &re5_mod_shell32,
    &re5_mod_steam_api,
    &re5_mod_user32,
    &re5_mod_winmm,
    &re5_mod_wmvcore,
    &re5_mod_ws2_32,
    &re5_mod_ws2_32_sock,
    &re5_mod_xinput1_3,
    NULL,
};

typedef struct {
    char     dll[40];       /* normalizado: minúsculas, sem ".dll" */
    char    *name;          /* NULL se por ordinal */
    uint16_t ordinal;
    uint32_t va;
    int      implemented;
    int      static_bound;  /* veio da IAT do executável (entra no censo) */
    char    *label;         /* "dll!nome" para o thunk e o censo */
} binding;

static binding *g_bind;
static uint32_t g_nbind, g_capbind;
static re5_mutex g_mu;
static int g_mu_ready;

static void normalize_dll(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; in && in[k] && k + 1 < n; k++) out[k] = (char)tolower((unsigned char)in[k]);
    out[k] = 0;
    if (k > 4 && strcmp(out + k - 4, ".dll") == 0) out[k - 4] = 0;
}

static const re5_import_module *find_module(const char *norm)
{
    for (const re5_import_module *const *m = k_modules; *m; m++) {
        char mod[40];
        normalize_dll((*m)->dll, mod, sizeof mod);
        if (strcmp(mod, norm) == 0) return *m;
    }
    return NULL;
}

static const re5_import_def *find_def(const char *norm, const char *name, uint16_t ordinal)
{
    for (const re5_import_module *const *m = k_modules; *m; m++) {
        char mod[40];
        normalize_dll((*m)->dll, mod, sizeof mod);
        if (strcmp(mod, norm) != 0) continue;
        for (const re5_import_def *d = (*m)->defs; d->name; d++) {
            if (name ? strcmp(d->name, name) == 0 : (d->ordinal && d->ordinal == ordinal)) return d;
        }
    }
    return NULL;
}

static RE5_NORETURN void unimplemented(CPU *c)
{
    const re5_thunk *t = re5_thunk_current();
    re5_fatal("import", "import não implementado: %s (esp=0x%08X). Implemente em runtime/src/win32/ "
                        "e registre a tabela em imports.c", t ? t->name : "?", c->esp);
}

static void unimplemented_fn(CPU *c) { unimplemented(c); }

static binding *find_binding(const char *norm, const char *name, uint16_t ordinal)
{
    for (uint32_t i = 0; i < g_nbind; i++) {
        binding *b = &g_bind[i];
        if (strcmp(b->dll, norm) != 0) continue;
        if (name ? (b->name && strcmp(b->name, name) == 0) : (!b->name && b->ordinal == ordinal)) return b;
    }
    return NULL;
}

/* Acha ou cria a ligação e devolve o VA do thunk (0 se não existe e
 * create_missing == 0). Tudo sob o lock: g_bind pode ser realocado. */
static uint32_t get_binding(const char *dll, const char *name, uint16_t ordinal, int create_missing,
                            int mark_static)
{
    if (!g_mu_ready) { re5_mutex_init(&g_mu); g_mu_ready = 1; }
    char norm[40];
    normalize_dll(dll, norm, sizeof norm);
    re5_mutex_lock(&g_mu);
    binding *b = find_binding(norm, name, ordinal);
    if (!b) {
        const re5_import_def *d = find_def(norm, name, ordinal);
        if (!d && !create_missing) { re5_mutex_unlock(&g_mu); return 0; }
        if (g_nbind == g_capbind) {
            uint32_t cap = g_capbind ? g_capbind * 2 : 256;
            binding *nb = realloc(g_bind, cap * sizeof *nb);
            if (!nb) re5_fatal("import", "sem memória");
            g_bind = nb;
            g_capbind = cap;
        }
        b = &g_bind[g_nbind++];
        memset(b, 0, sizeof *b);
        snprintf(b->dll, sizeof b->dll, "%s", norm);
        b->name = name ? strdup(name) : NULL;
        b->ordinal = ordinal;
        b->implemented = d != NULL;
        char label[160];
        if (name) snprintf(label, sizeof label, "%s.dll!%s", norm, name);
        else snprintf(label, sizeof label, "%s.dll!#%u", norm, ordinal);
        b->label = strdup(label);
        b->va = re5_thunk_new(b->label, d ? d->fn : unimplemented_fn, d ? d->arg_bytes : 0, NULL);
        if (d && d->flags) re5_thunk_set_flags(b->va, d->flags);
    }
    if (mark_static) {
        b->static_bound = 1;
        if (!b->implemented) RE5_DEBUG("import", "sem implementação: %s", b->label);
    }
    uint32_t va = b->va;
    re5_mutex_unlock(&g_mu);
    return va;
}

uint32_t re5_import_bind(const char *dll, const char *name, uint16_t ordinal)
{
    return get_binding(dll, name, ordinal, 1, 1);
}

uint32_t re5_import_lookup(const char *dll, const char *name, uint16_t ordinal)
{
    return get_binding(dll, name, ordinal, 0, 0);
}

int re5_import_module_known(const char *dll)
{
    char norm[40];
    normalize_dll(dll, norm, sizeof norm);
    return find_module(norm) != NULL;
}

int re5_import_dll_bound(const char *dll)
{
    if (!g_mu_ready) return 0;
    char norm[40];
    normalize_dll(dll, norm, sizeof norm);
    int found = 0;
    re5_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nbind && !found; i++)
        found = g_bind[i].static_bound && strcmp(g_bind[i].dll, norm) == 0;
    re5_mutex_unlock(&g_mu);
    return found;
}

re5_census re5_import_census(void)
{
    re5_census c = {0, 0};
    if (!g_mu_ready) return c;
    re5_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nbind; i++) {
        if (!g_bind[i].static_bound) continue;
        c.bound++;
        c.implemented += (uint32_t)g_bind[i].implemented;
    }
    re5_mutex_unlock(&g_mu);
    return c;
}

static int by_label(const void *a, const void *b)
{
    return strcmp((*(const binding *const *)a)->label, (*(const binding *const *)b)->label);
}

void re5_import_census_print(FILE *out, int list_missing)
{
    re5_census c = re5_import_census();
    fprintf(out, "imports: %u ligados, %u implementados (%.1f%%), %u faltando\n", c.bound, c.implemented,
            c.bound ? 100.0 * c.implemented / c.bound : 0.0, c.bound - c.implemented);
    if (!list_missing || c.bound == c.implemented) return;
    re5_mutex_lock(&g_mu);
    const binding **miss = malloc(g_nbind * sizeof *miss);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nbind; i++)
        if (g_bind[i].static_bound && !g_bind[i].implemented) miss[n++] = &g_bind[i];
    qsort(miss, n, sizeof *miss, by_label);
    for (uint32_t i = 0; i < n; i++) fprintf(out, "  falta  %s\n", miss[i]->label);
    re5_mutex_unlock(&g_mu);
    free(miss);
}
