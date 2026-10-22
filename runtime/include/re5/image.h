/*
 * image.h - carrega o executável PE32 do jogo no espaço do guest.
 *
 * O re5dx9.exe não tem relocações, então a imagem vai exatamente para
 * ImageBase (0x00400000). O loader copia cabeçalhos e seções, liga cada
 * entrada da IAT a um thunk (re5_import_bind) e devolve o entry point.
 * Nenhum byte do guest é executado: o código está em generated/src.
 */
#ifndef RE5_IMAGE_H
#define RE5_IMAGE_H

#include <stdint.h>

#define RE5_IMAGE_MAX_SECTIONS 16

typedef struct {
    char     name[9];
    uint32_t va;
    uint32_t vsize;
    uint32_t characteristics;
} re5_section;

typedef struct {
    uint32_t    base;
    uint32_t    size;
    uint32_t    entry;
    uint32_t    stack_reserve;
    uint32_t    timestamp;
    uint32_t    resource_va;      /* diretório de recursos (FindResource), 0 se ausente */
    uint32_t    resource_size;
    uint32_t    n_sections;
    re5_section sections[RE5_IMAGE_MAX_SECTIONS];
    uint32_t    n_imports;
} re5_image;

/* 0 = ok. Em erro, *err recebe uma mensagem estática. */
int re5_image_load(const char *path, re5_image *out, const char **err);

/* A imagem principal carregada (para GetModuleHandle(NULL), crash reports). */
const re5_image *re5_image_main(void);

#endif /* RE5_IMAGE_H */
