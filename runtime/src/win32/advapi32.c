/*
 * advapi32.c - registro do Windows em memória (Fase 3, etapa 3.4).
 *
 * O jogo só lê: RegOpenKeyExA, RegQueryValueExA, RegCloseKey. O conteúdo é o
 * que o instalador da Steam grava (bin_input/BH5AE_install.vdf), com a pasta de
 * instalação virtual do VFS. Chave ou valor que não estão aqui respondem
 * ERROR_FILE_NOT_FOUND, exatamente como num Windows sem eles - o jogo trata.
 * Cada consulta que falha vai para o log (canal "advapi32") para a próxima
 * semente ser fácil de achar.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re5/runtime.h"

#define HKEY_CLASSES_ROOT  0x80000000u
#define HKEY_CURRENT_USER  0x80000001u
#define HKEY_LOCAL_MACHINE 0x80000002u
#define HKEY_USERS         0x80000003u

#define REG_SZ    1u
#define REG_DWORD 4u

#define A32_ERROR_SUCCESS        0u
#define A32_ERROR_FILE_NOT_FOUND 2u
#define A32_ERROR_INVALID_HANDLE 6u
#define A32_ERROR_MORE_DATA      234u

typedef struct {
    const char *key;     /* caminho completo, raiz incluída */
    const char *name;
    uint32_t    type;
    const char *sz;
    uint32_t    dword;
} reg_value;

static const reg_value k_values[] = {
    {"HKLM\\Software\\CAPCOM\\RESIDENT EVIL 5", "installdir", REG_SZ, RE5_VFS_INSTALL_DIR, 0},
    {"HKLM\\Software\\CAPCOM\\RESIDENT EVIL 5", "language", REG_SZ, "English", 0},
    {"HKLM\\Software\\CAPCOM\\RESIDENT EVIL 5", "GUID", REG_SZ, "", 0},
    {"HKLM\\Software\\CAPCOM\\RESIDENT EVIL 5", "launch", REG_DWORD, NULL, 1},
    {"HKCU\\Software\\CAPCOM\\RESIDENT EVIL 5", "language", REG_SZ, "1033", 0},
    {"HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", REG_SZ, "Intel(R) Core(TM)2 Duo CPU     E6850  @ 3.00GHz", 0},
    {"HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", REG_DWORD, NULL, 3000},
    {"HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "VendorIdentifier", REG_SZ, "GenuineIntel", 0},
    {"HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "Identifier", REG_SZ, "x86 Family 6 Model 15 Stepping 11", 0},
};

typedef struct {
    char path[512];
} reg_key;

static void destroy_key(void *p) { free(p); }

static int ieq_prefix(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static int key_exists(const char *path)
{
    size_t n = strlen(path);
    for (size_t i = 0; i < sizeof k_values / sizeof k_values[0]; i++) {
        const char *k = k_values[i].key;
        if (strlen(k) >= n && ieq_prefix(k, path, n) && (k[n] == 0 || k[n] == '\\')) return 1;
    }
    return 0;
}

/* Caminho completo de uma chave aberta (ou raiz predefinida). 0 se inválida. */
static int key_path(uint32_t h, char *out, size_t cap)
{
    const char *root = h == HKEY_LOCAL_MACHINE ? "HKLM" : h == HKEY_CURRENT_USER ? "HKCU"
                     : h == HKEY_CLASSES_ROOT ? "HKCR" : h == HKEY_USERS ? "HKU" : NULL;
    if (root) { snprintf(out, cap, "%s", root); return 1; }
    reg_key *k = re5_handle_get(h, RE5_H_HKEY);
    if (!k) return 0;
    snprintf(out, cap, "%s", k->path);
    return 1;
}

static void a_RegOpenKeyExA(CPU *c)
{
    uint32_t h = re5_arg(c, 0), out = re5_arg(c, 4);
    const char *sub = re5_gstr(re5_arg(c, 1));
    char path[512];
    if (!key_path(h, path, sizeof path)) { c->eax = A32_ERROR_INVALID_HANDLE; return; }
    if (sub && *sub) {
        size_t n = strlen(path);
        snprintf(path + n, sizeof path - n, "\\%s", sub);
        n = strlen(path);
        while (n && path[n - 1] == '\\') path[--n] = 0;
    }
    if (!key_exists(path)) {
        RE5_INFO("advapi32", "RegOpenKeyExA(%s): chave ausente", path);
        c->eax = A32_ERROR_FILE_NOT_FOUND;
        return;
    }
    reg_key *k = calloc(1, sizeof *k);
    if (!k) { c->eax = 8u; return; }
    snprintf(k->path, sizeof k->path, "%s", path);
    uint32_t hk = re5_handle_new(RE5_H_HKEY, k, destroy_key);
    memcpy(re5_g2h(out), &hk, 4);
    c->eax = A32_ERROR_SUCCESS;
}

static void a_RegQueryValueExA(CPU *c)
{
    uint32_t h = re5_arg(c, 0), type_va = re5_arg(c, 3), data = re5_arg(c, 4), size_va = re5_arg(c, 5);
    const char *name = re5_gstr(re5_arg(c, 1));
    char path[512];
    if (!key_path(h, path, sizeof path)) { c->eax = A32_ERROR_INVALID_HANDLE; return; }
    const reg_value *v = NULL;
    for (size_t i = 0; i < sizeof k_values / sizeof k_values[0]; i++) {
        const reg_value *r = &k_values[i];
        if (strlen(r->key) == strlen(path) && ieq_prefix(r->key, path, strlen(path)) &&
            strlen(r->name) == strlen(name ? name : "") && ieq_prefix(r->name, name ? name : "", strlen(r->name)))
            v = r;
    }
    if (!v) {
        RE5_INFO("advapi32", "RegQueryValueExA(%s, \"%s\"): valor ausente", path, name ? name : "");
        c->eax = A32_ERROR_FILE_NOT_FOUND;
        return;
    }
    uint32_t need = v->type == REG_SZ ? (uint32_t)strlen(v->sz) + 1 : 4;
    if (type_va) memcpy(re5_g2h(type_va), &v->type, 4);
    uint32_t have = 0;
    if (size_va) memcpy(&have, re5_g2h(size_va), 4);
    c->eax = A32_ERROR_SUCCESS;
    if (data) {
        if (!size_va || have < need) c->eax = A32_ERROR_MORE_DATA;
        else if (v->type == REG_SZ) memcpy(re5_g2h(data), v->sz, need);
        else memcpy(re5_g2h(data), &v->dword, 4);
    }
    if (size_va) memcpy(re5_g2h(size_va), &need, 4);
}

static void a_RegCloseKey(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    if (h >= HKEY_CLASSES_ROOT && h <= HKEY_USERS) { c->eax = A32_ERROR_SUCCESS; return; }
    c->eax = re5_handle_type_of(h) == RE5_H_HKEY && re5_handle_close(h) == 0 ? A32_ERROR_SUCCESS
                                                                              : A32_ERROR_INVALID_HANDLE;
}

static const re5_import_def k_defs[] = {
    {"RegCloseKey",      a_RegCloseKey,       4, 0},
    {"RegOpenKeyExA",    a_RegOpenKeyExA,    20, 0},
    {"RegQueryValueExA", a_RegQueryValueExA, 24, 0},
    {0},
};

const re5_import_module re5_mod_advapi32 = {"advapi32.dll", k_defs};
