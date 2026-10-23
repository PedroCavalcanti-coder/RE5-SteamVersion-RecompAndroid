/*
 * memdump.c - despejo da memória do guest para análise fora do jogo (Ghidra, scripts).
 *
 * Com RE5_DUMP=1, F10 grava <RE5_DUMP_DIR ou work/dump>/guest_<n>.bin e .map; RE5_DUMP_AT=<Present,...>
 * grava no Present indicado (benchmark sem jogador). O .map tem uma linha "VA tamanho deslocamento"
 * (hexa) por trecho; só as páginas de 4 KiB que não são zero entram (o heap tem arenas enormes,
 * quase todas zeradas). Um script lê o .bin como se fosse a memória do jogo e segue ponteiros
 * (estruturas do Havok, objetos do jogo) sem precisar do jogo rodando.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/log.h"
#include "re5/runtime.h"

#define DUMP_MAX_REGIONS 8192
#define PAGE 4096u

static int dump_enabled(void)
{
    static int e = -1;
    if (e < 0) {
        const char *s = getenv("RE5_DUMP");
        e = s && *s && *s != '0';
    }
    return e;
}

static int page_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (uint32_t i = 0; i < PAGE / 8; i++)
        if (q[i]) return 0;
    return 1;
}

static void dump_to(const char *name, const char *why);

static void do_dump(const char *why)
{
    static int seq;
    char name[64];
    snprintf(name, sizeof name, "guest_%d", ++seq);
    dump_to(name, why);
}

void re5_dump_named(const char *name)
{
    dump_to(name, name);
}

static void dump_to(const char *name, const char *why)
{
    const char *dir = getenv("RE5_DUMP_DIR");
    char bin[512], map[512];
    snprintf(bin, sizeof bin, "%s/%s.bin", dir && *dir ? dir : "work/dump", name);
    snprintf(map, sizeof map, "%s/%s.map", dir && *dir ? dir : "work/dump", name);
    FILE *fb = fopen(bin, "wb"), *fm = fopen(map, "w");
    if (!fb || !fm) {
        RE5_ERROR("dump", "não consegui criar %s (crie a pasta)", bin);
        if (fb) fclose(fb);
        if (fm) fclose(fm);
        return;
    }
    static re5_mem_region regs[DUMP_MAX_REGIONS];
    uint32_t n = re5_mem_regions(regs, DUMP_MAX_REGIONS);
    uint64_t off = 0, pages = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t base = regs[i].base, size = regs[i].size;
        uint32_t run_start = 0;
        int in_run = 0;
        for (uint32_t o = 0; o <= size; o += PAGE) {
            int nz = o < size && !page_zero((const uint8_t *)re5_g2h(base + o));
            if (nz && !in_run) {
                in_run = 1;
                run_start = o;
            } else if (!nz && in_run) {
                in_run = 0;
                uint32_t len = o - run_start;
                fwrite(re5_g2h(base + run_start), 1, len, fb);
                fprintf(fm, "%08X %08X %llX\n", base + run_start, len, (unsigned long long)off);
                off += len;
                pages += len / PAGE;
            }
        }
    }
    fclose(fb);
    fclose(fm);
    RE5_INFO("dump", "%s: %llu MiB em %s (+ .map)", why, (unsigned long long)(off >> 20), bin);
}

void re5_dump_key(int fkey)
{
    if (fkey == 10 && dump_enabled()) do_dump("F10");
}

void re5_dump_at_present(uint32_t n)
{
    static uint32_t list[64], count;
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("RE5_DUMP_AT");
        for (const char *p = e; p && *p && count < 64;) {
            char *end;
            uint32_t v = (uint32_t)strtoul(p, &end, 10);
            if (end == p) break;
            list[count++] = v;
            p = *end == ',' ? end + 1 : end;
        }
    }
    for (uint32_t i = 0; i < count; i++)
        if (list[i] == n) {
            char why[32];
            snprintf(why, sizeof why, "Present %u", n);
            do_dump(why);
        }
}
