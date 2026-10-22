/*
 * oleaut32.c - BSTR (SysAllocString/SysFreeString).
 *
 * Um BSTR é um ponteiro para a string UTF-16 terminada em 0 precedida pelo
 * tamanho em bytes (DWORD, sem o terminador). O bloco mora no heap do processo
 * do guest; o guest recebe o ponteiro para o primeiro caractere.
 */
#include "kernel32_internal.h"

static void o_SysAllocString(CPU *c)
{
    uint32_t src = re5_arg(c, 0);
    c->eax = 0;
    if (!src) return;
    uint32_t n = k32_wcslen(src);
    uint32_t blk = k32_heap_alloc(k32_process_heap(), 0, 4u + 2u * n + 2u);
    if (!blk) return;   /* sem memória: NULL, como o Windows */
    k32_w32(blk, 2u * n);
    memcpy(re5_g2h(blk + 4), re5_g2h(src), 2u * n);
    k32_w16(blk + 4 + 2u * n, 0);
    c->eax = blk + 4;
}

static void o_SysFreeString(CPU *c)
{
    uint32_t b = re5_arg(c, 0);
    if (b) k32_heap_free(k32_process_heap(), b - 4);
}

static const re5_import_def k_defs[] = {
    {"SysAllocString", o_SysAllocString, 4, 2},
    {"SysFreeString",  o_SysFreeString,  4, 6},
    {0},
};

const re5_import_module re5_mod_oleaut32 = {"oleaut32.dll", k_defs};
