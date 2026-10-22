/*
 * iphlpapi.c - interfaces de rede de uma máquina sem rede (coerente com a
 * Steam offline e o ws2_32): só a "Software Loopback Interface 1" que o
 * Windows 7 lista, sem endereço físico. O RE5 guarda as interfaces com MAC
 * para medir tráfego; sem nenhuma, ele nunca chama GetIfEntry.
 */
#include <string.h>

#include "re5/runtime.h"

#define ERROR_SUCCESS_             0u
#define ERROR_INVALID_DATA_        13u
#define ERROR_INVALID_PARAMETER_   87u
#define ERROR_INSUFFICIENT_BUFFER_ 122u

/* MIB_IFROW (x86): wszName[256], dwIndex, dwType, dwMtu, dwSpeed, dwPhysAddrLen,
 * bPhysAddr[8], dwAdminStatus, dwOperStatus, dwLastChange, 12 contadores,
 * dwDescrLen, bDescr[256] = 860 bytes. MIB_IFTABLE = dwNumEntries + linhas. */
#define IFROW_SIZE 860u
#define LOOPBACK_INDEX 1u

static void fill_loopback(uint32_t row)
{
    static const char name[] = "Software Loopback Interface 1";
    memset(re5_g2h(row), 0, IFROW_SIZE);
    for (uint32_t i = 0; i < sizeof name - 1; i++) wr16(row + 2 * i, (uint8_t)name[i]);
    wr32(row + 512, LOOPBACK_INDEX);
    wr32(row + 516, 24);           /* IF_TYPE_SOFTWARE_LOOPBACK */
    wr32(row + 520, 1500);         /* dwMtu */
    wr32(row + 524, 1073741824);   /* dwSpeed: 1 Gb/s, o que o Windows informa */
    wr32(row + 528, 0);            /* dwPhysAddrLen: sem MAC */
    wr32(row + 540, 1);            /* MIB_IF_ADMIN_STATUS_UP */
    wr32(row + 544, 5);            /* MIB_IF_OPER_STATUS_OPERATIONAL */
    wr32(row + 600, sizeof name - 1);   /* dwDescrLen */
    memcpy(re5_g2h(row + 604), name, sizeof name - 1);
}

static void i_GetIfTable(CPU *c)
{
    uint32_t table = re5_arg(c, 0), psize = re5_arg(c, 1);
    if (!psize) {
        c->eax = ERROR_INVALID_PARAMETER_;
        return;
    }
    uint32_t need = 4u + IFROW_SIZE;
    if (!table || rd32(psize) < need) {
        wr32(psize, need);
        c->eax = ERROR_INSUFFICIENT_BUFFER_;
        return;
    }
    wr32(table, 1);
    fill_loopback(table + 4);
    c->eax = ERROR_SUCCESS_;
}

static void i_GetIfEntry(CPU *c)
{
    uint32_t row = re5_arg(c, 0);
    if (!row) {
        c->eax = ERROR_INVALID_PARAMETER_;
        return;
    }
    if (rd32(row + 512) != LOOPBACK_INDEX) {
        c->eax = ERROR_INVALID_DATA_;
        return;
    }
    fill_loopback(row);
    c->eax = ERROR_SUCCESS_;
}

static const re5_import_def k_defs[] = {
    {"GetIfEntry", i_GetIfEntry, 4, 0},
    {"GetIfTable", i_GetIfTable, 12, 0},
    {0},
};

const re5_import_module re5_mod_iphlpapi = {"iphlpapi.dll", k_defs};
