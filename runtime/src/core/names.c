/*
 * names.c - resolução de nomes de funções e símbolos do guest (work/names.tsv).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/log.h"
#include "re5/runtime.h"

typedef struct {
    uint32_t va;
    const char *name;
} name_entry;

static name_entry *g_entries;
static uint32_t g_count;
static char *g_buf;

void re5_names_init(void)
{
    const char *paths[] = {
        getenv("RE5_NAMES_FILE"),
        "work/names.tsv",
        "names.tsv",
        NULL
    };

    FILE *f = NULL;
    for (int i = 0; paths[i]; i++) {
        if (!paths[i] || !*paths[i]) continue;
        f = fopen(paths[i], "rb");
        if (f) break;
    }
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return;
    }

    g_buf = malloc((size_t)sz + 1);
    if (!g_buf) {
        fclose(f);
        return;
    }
    size_t read_bytes = fread(g_buf, 1, (size_t)sz, f);
    fclose(f);
    g_buf[read_bytes] = '\0';

    uint32_t lines = 0;
    for (size_t i = 0; i < read_bytes; i++) {
        if (g_buf[i] == '\n') lines++;
    }

    g_entries = calloc(lines + 1, sizeof *g_entries);
    if (!g_entries) {
        free(g_buf);
        g_buf = NULL;
        return;
    }

    char *cur = g_buf;
    uint32_t count = 0;
    while (*cur) {
        char *line_end = strchr(cur, '\n');
        if (line_end) *line_end = '\0';
        char *tab = strchr(cur, '\t');
        if (tab) {
            *tab = '\0';
            char *endptr = NULL;
            uint32_t va = (uint32_t)strtoul(cur, &endptr, 16);
            if (endptr != cur) {
                g_entries[count].va = va;
                g_entries[count].name = tab + 1;
                char *r = strchr(tab + 1, '\r');
                if (r) *r = '\0';
                count++;
            }
        }
        if (!line_end) break;
        cur = line_end + 1;
    }
    g_count = count;
    if (g_count > 0) {
        RE5_INFO("runtime", "nomes de funções carregados: %u entradas", g_count);
    }
}

static int name_cmp(const void *key, const void *elem)
{
    uint32_t k = *(const uint32_t *)key;
    uint32_t va = ((const name_entry *)elem)->va;
    return (k < va) ? -1 : (k > va) ? 1 : 0;
}

const char *re5_name_lookup(uint32_t va)
{
    if (!g_count || !g_entries) return NULL;
    name_entry *res = bsearch(&va, g_entries, g_count, sizeof *g_entries, name_cmp);
    return res ? res->name : NULL;
}
