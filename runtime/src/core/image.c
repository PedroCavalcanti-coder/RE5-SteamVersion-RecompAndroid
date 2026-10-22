/*
 * image.c - loader PE32 para o espaço do guest (ver re5/image.h).
 *
 * Só o que o re5dx9.exe precisa: PE32 i386, base fixa, imports por nome e
 * ordinal. Lê os campos por offset em vez de structs do <windows.h> para
 * compilar igual em todo host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/guest_memory.h"
#include "re5/image.h"
#include "re5/imports.h"
#include "re5/log.h"

static re5_image g_main;
static int g_main_loaded;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const re5_image *re5_image_main(void) { return g_main_loaded ? &g_main : NULL; }

static uint32_t section_prot(uint32_t ch)
{
    int x = (ch & 0x20000000u) != 0, w = (ch & 0x80000000u) != 0;
    if (w) return x ? RE5_PAGE_EXECUTE_READWRITE : RE5_PAGE_READWRITE;
    return x ? RE5_PAGE_EXECUTE_READ : RE5_PAGE_READONLY;
}

/* Liga cada entrada da IAT a um thunk. Lê da imagem já mapeada no guest. */
static int bind_imports(re5_image *img, uint32_t dir_rva, uint32_t dir_size, const char **err)
{
    if (!dir_rva || !dir_size) return 0;
    uint32_t base = img->base;
    for (uint32_t d = base + dir_rva; ; d += 20) {
        if (!re5_mem_is_mapped(d, 20)) { *err = "diretório de imports fora da imagem"; return -1; }
        const uint8_t *desc = re5_g2h(d);
        uint32_t ilt = u32(desc), name_rva = u32(desc + 12), iat = u32(desc + 16);
        if (!ilt && !name_rva && !iat) break;
        const char *dll = re5_gstr(base + name_rva);
        uint32_t lookup = base + (ilt ? ilt : iat);
        for (uint32_t i = 0; ; i++) {
            uint32_t entry = u32(re5_g2h(lookup + 4 * i));
            if (!entry) break;
            uint32_t va;
            if (entry & 0x80000000u) {
                va = re5_import_bind(dll, NULL, (uint16_t)entry);
            } else {
                va = re5_import_bind(dll, re5_gstr(base + entry + 2), 0);   /* IMAGE_IMPORT_BY_NAME: hint, nome */
            }
            memcpy(re5_g2h(base + iat + 4 * i), &va, 4);
            img->n_imports++;
        }
    }
    return 0;
}

int re5_image_load(const char *path, re5_image *out, const char **err)
{
    static const char *dummy;
    if (!err) err = &dummy;
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "não foi possível abrir o executável"; return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = len > 0 ? malloc((size_t)len) : NULL;
    if (!file || fread(file, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(file);
        *err = "falha ao ler o executável";
        return -1;
    }
    fclose(f);
    int rc = -1;
    size_t n = (size_t)len;

    if (n < 0x40 || u16(file) != 0x5A4D) { *err = "não é um executável MZ"; goto done; }
    uint32_t pe = u32(file + 0x3C);
    if (pe + 0xF8 > n || u32(file + pe) != 0x00004550) { *err = "cabeçalho PE ausente"; goto done; }
    const uint8_t *fh = file + pe + 4, *oh = fh + 20;
    if (u16(fh) != 0x014C) { *err = "máquina não é i386"; goto done; }
    if (u16(oh) != 0x010B) { *err = "não é PE32"; goto done; }
    uint16_t n_sec = u16(fh + 2), opt_size = u16(fh + 16);
    out->timestamp = u32(fh + 4);
    out->entry = u32(oh + 16);
    out->base = u32(oh + 28);
    out->size = u32(oh + 56);
    uint32_t hdr_size = u32(oh + 60);
    out->stack_reserve = u32(oh + 72);
    uint32_t n_dirs = u32(oh + 92);
    const uint8_t *dirs = oh + 96;
    uint32_t imp_rva = n_dirs > 1 ? u32(dirs + 8) : 0, imp_size = n_dirs > 1 ? u32(dirs + 12) : 0;
    out->resource_va = n_dirs > 2 && u32(dirs + 16) ? out->base + u32(dirs + 16) : 0;
    out->resource_size = n_dirs > 2 ? u32(dirs + 20) : 0;
    out->entry += out->base;

    if (re5_mem_map_fixed(out->base, out->size, RE5_PAGE_READWRITE, "imagem") != 0) {
        *err = "ImageBase ocupado ou inválido no espaço do guest";
        goto done;
    }
    memcpy(re5_g2h(out->base), file, hdr_size < n ? hdr_size : n);

    const uint8_t *sec = oh + opt_size;
    if ((size_t)(sec - file) + 40u * n_sec > n) { *err = "tabela de seções truncada"; goto done; }
    for (uint16_t i = 0; i < n_sec; i++, sec += 40) {
        uint32_t vsize = u32(sec + 8), rva = u32(sec + 12), raw_size = u32(sec + 16), raw = u32(sec + 20);
        uint32_t copy = raw_size < vsize || !vsize ? raw_size : vsize;
        if (rva + (vsize > raw_size ? vsize : raw_size) > out->size) { *err = "seção fora de SizeOfImage"; goto done; }
        if (copy && (uint64_t)raw + copy <= n) memcpy(re5_g2h(out->base + rva), file + raw, copy);
        if (out->n_sections < RE5_IMAGE_MAX_SECTIONS) {
            re5_section *s = &out->sections[out->n_sections++];
            memcpy(s->name, sec, 8);
            s->name[8] = 0;
            s->va = out->base + rva;
            s->vsize = vsize ? vsize : raw_size;
            s->characteristics = u32(sec + 36);
        }
    }

    if (bind_imports(out, imp_rva, imp_size, err) != 0) goto done;

    /* Proteções por seção só depois de escrever a IAT (que costuma morar em .rdata). */
    for (uint32_t i = 0; i < out->n_sections; i++) {
        re5_section *s = &out->sections[i];
        re5_mem_protect(s->va, s->vsize, section_prot(s->characteristics), NULL);
    }
    re5_mem_protect(out->base, hdr_size, RE5_PAGE_READONLY, NULL);

    g_main = *out;
    g_main_loaded = 1;
    RE5_INFO("image", "%s: base 0x%08X, %u seções, entry 0x%08X, %u imports", path, out->base,
             out->n_sections, out->entry, out->n_imports);
    rc = 0;
done:
    free(file);
    return rc;
}
