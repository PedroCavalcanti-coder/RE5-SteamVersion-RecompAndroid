/*
 * ole32.c - COM do guest (Fase 3; as pontes COM de verdade são a Fase 4, D4).
 *
 * CoInitialize/CoUninitialize só contam o apartamento. CoCreateInstance:
 *   - classe da lista k_absent: responde REGDB_E_CLASSNOTREG, como um Windows
 *     sem aquele componente. Só entra na lista o que o jogo comprovadamente
 *     trata como opcional (com o motivo ao lado);
 *   - qualquer outra classe aborta dizendo o CLSID (D8): é uma ponte a escrever
 *     (XAudio2, por exemplo) e não pode virar um "falhou, segue".
 */
#include <stdio.h>
#include <string.h>

#include "re5/runtime.h"

#define S_OK                 0x00000000u
#define S_FALSE              0x00000001u
#define REGDB_E_CLASSNOTREG  0x80040154u

static int g_init_count;

static void o_CoInitialize(CPU *c)
{
    c->eax = g_init_count++ ? S_FALSE : S_OK;
}

static void o_CoUninitialize(CPU *c)
{
    (void)c;
    if (g_init_count > 0) g_init_count--;
}

static void guid_str(uint32_t va, char *out, size_t cap)
{
    const uint8_t *g = re5_g2h(va);
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    snprintf(out, cap, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9], g[10], g[11],
             g[12], g[13], g[14], g[15]);
}

static const struct { const char *clsid, *why; } k_absent[] = {
    /* WbemLocator (WMI): o código do DirectX SDK que separa controles XInput de
     * DirectInput (IsXInputDevice) cai no caminho "sem WMI" e segue. */
    {"{4590F811-1D3A-11D0-891F-00AA004B2E24}", "WMI ausente: detecção de XInput por WMI desligada"},
};

static void o_CoCreateInstance(CPU *c)
{
    char clsid[48], iid[48];
    guid_str(re5_arg(c, 0), clsid, sizeof clsid);
    guid_str(re5_arg(c, 3), iid, sizeof iid);
    uint32_t out = re5_arg(c, 4);
    for (size_t i = 0; i < sizeof k_absent / sizeof k_absent[0]; i++) {
        if (strcmp(k_absent[i].clsid, clsid) == 0) {
            RE5_WARN("ole32", "CoCreateInstance(%s): REGDB_E_CLASSNOTREG (%s)", clsid, k_absent[i].why);
            if (out) memset(re5_g2h(out), 0, 4);
            c->eax = REGDB_E_CLASSNOTREG;
            return;
        }
    }
    re5_fatal("ole32", "CoCreateInstance(clsid %s, iid %s): classe COM sem ponte (Fase 4, decisão D4)", clsid, iid);
}

static void o_CoSetProxyBlanket(CPU *c)
{
    re5_fatal("ole32", "CoSetProxyBlanket(0x%08X): só existe com um proxy COM, e nenhum é criado", re5_arg(c, 0));
}

static const re5_import_def k_defs[] = {
    {"CoCreateInstance",  o_CoCreateInstance,  20, 0},
    {"CoInitialize",      o_CoInitialize,       4, 0},
    {"CoSetProxyBlanket", o_CoSetProxyBlanket, 32, 0},
    {"CoUninitialize",    o_CoUninitialize,     0, 0},
    {0},
};

const re5_import_module re5_mod_ole32 = {"ole32.dll", k_defs};
