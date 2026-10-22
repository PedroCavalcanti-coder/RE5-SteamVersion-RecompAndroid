/*
 * imports.h - registro das funções do Win32 que o runtime implementa (HLE).
 *
 * Cada módulo do HLE (runtime/src/win32/<dll>.c) exporta uma tabela
 * re5_import_module. O loader resolve cada entrada da IAT do jogo por nome
 * (ou ordinal) aqui:
 *   - implementada  -> thunk com o handler;
 *   - não implementada -> thunk que aborta dizendo "dll!nome" na primeira
 *     chamada. Falhar alto é o objetivo: um stub que "retorna 0" esconde o
 *     bug a mil funções de distância.
 * GetProcAddress usa o mesmo registro, mas devolve 0 para o que não existe,
 * porque o jogo testa NULL para detectar APIs opcionais.
 *
 * O censo (re5_import_census) diz quantos imports do jogo já têm
 * implementação: é a métrica de progresso da Fase 3.
 */
#ifndef RE5_IMPORTS_H
#define RE5_IMPORTS_H

#include <stdio.h>
#include "re5/thunks.h"

typedef struct {
    const char *name;
    re5_host_fn fn;
    uint32_t    arg_bytes;
    uint16_t    ordinal;     /* 0 = só por nome */
    uint16_t    flags;       /* RE5_THUNK_* do thunk (ex.: RE5_THUNK_NOLOCK) */
} re5_import_def;

typedef struct {
    const char           *dll;    /* comparado sem diferenciar maiúsculas, com ou sem ".dll" */
    const re5_import_def *defs;   /* terminada por {0} */
} re5_import_module;

/* Thunk para o import (sempre retorna um VA: o de "não implementado" se for o caso). */
uint32_t re5_import_bind(const char *dll, const char *name, uint16_t ordinal);

/* Para GetProcAddress: 0 se não houver implementação. */
uint32_t re5_import_lookup(const char *dll, const char *name, uint16_t ordinal);

/* 1 se o runtime conhece a DLL (LoadLibrary/GetModuleHandle de módulos de sistema). */
int re5_import_module_known(const char *dll);

/* 1 se a DLL está na tabela de imports do executável (já "carregada" no início). */
int re5_import_dll_bound(const char *dll);

typedef struct {
    uint32_t bound;          /* imports estáticos ligados pelo loader */
    uint32_t implemented;    /* ...dos quais têm handler */
} re5_census;

re5_census re5_import_census(void);
void re5_import_census_print(FILE *out, int list_missing);

#endif /* RE5_IMPORTS_H */
