/*
 * gdi32.c - objetos de desenho do Win32 (Fase 4, etapa 4.1).
 *
 * RE5 só importa GetStockObject (para hbrBackground da classe da janela).
 */
#include <stdint.h>
#include "re5/runtime.h"

static void g_GetStockObject(CPU *c)
{
    int32_t i = (int32_t)re5_arg(c, 0);
    if (i < 0 || i > 20) {
        c->eax = 0;
        return;
    }
    c->eax = 0x20000000u | (uint32_t)i;
}

static const re5_import_def k_defs[] = {
    {"GetStockObject", g_GetStockObject, 4, 0},
    {0},
};

const re5_import_module re5_mod_gdi32 = {"gdi32.dll", k_defs};
