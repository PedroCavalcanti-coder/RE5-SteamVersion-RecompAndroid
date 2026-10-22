/*
 * shell32.c - pastas especiais do usuário (Fase 3, etapa 3.3).
 *
 * O jogo pergunta onde ficam Documentos/AppData para gravar saves e
 * configurações. A resposta é um caminho do guest num perfil fixo
 * (C:\Users\Player\...), que o VFS leva para a pasta do usuário do runtime
 * (<user_root>/drive_c/Users/Player/...). CSIDL fora da tabela aborta (D8).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>

#include "re5/runtime.h"

#if defined(RE5_HOST_WINDOWS)
#include <direct.h>
#define host_mkdir(p) _mkdir(p)
#else
#define host_mkdir(p) mkdir((p), 0755)
#endif

#define CSIDL_FLAG_CREATE 0x8000u
#define CSIDL_MASK        0x00FFu
#define S_OK              0u
#define E_FAIL            0x80004005u

static const struct { uint32_t csidl; const char *path; } k_folders[] = {
    {0x0005, "C:\\Users\\Player\\Documents"},             /* CSIDL_PERSONAL */
    {0x001A, "C:\\Users\\Player\\AppData\\Roaming"},      /* CSIDL_APPDATA */
    {0x001C, "C:\\Users\\Player\\AppData\\Local"},        /* CSIDL_LOCAL_APPDATA */
    {0x0023, "C:\\ProgramData"},                          /* CSIDL_COMMON_APPDATA */
    {0x002E, "C:\\Users\\Public\\Documents"},             /* CSIDL_COMMON_DOCUMENTS */
    {0x0028, "C:\\Users\\Player"},                        /* CSIDL_PROFILE */
    {0x0024, "C:\\Windows"},                              /* CSIDL_WINDOWS */
    {0x0025, "C:\\Windows\\system32"},                    /* CSIDL_SYSTEM */
};

/* mkdir -p do caminho do host, componente por componente. */
static int mkdirs(const char *host)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", host);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/' && *s != '\\') continue;
        char save = *s;
        *s = 0;
        if (s[-1] != ':') host_mkdir(p);
        *s = save;
    }
    struct stat st;
    if (host_mkdir(p) != 0 && errno != EEXIST) return -1;
    return stat(p, &st) == 0 ? 0 : -1;
}

static void s_SHGetFolderPathA(CPU *c)
{
    uint32_t csidl = re5_arg(c, 1), out = re5_arg(c, 4);
    const char *path = NULL;
    for (size_t i = 0; i < sizeof k_folders / sizeof k_folders[0]; i++)
        if (k_folders[i].csidl == (csidl & CSIDL_MASK)) path = k_folders[i].path;
    if (!path) re5_fatal("shell32", "SHGetFolderPathA: CSIDL 0x%X sem implementação", csidl);
    if (csidl & CSIDL_FLAG_CREATE) {
        char host[1024];
        if (re5_vfs_resolve(path, host, sizeof host) != 0 || mkdirs(host) != 0) {
            RE5_WARN("shell32", "SHGetFolderPathA: não consegui criar %s", path);
            c->eax = E_FAIL;
            return;
        }
    }
    memcpy(re5_g2h(out), path, strlen(path) + 1);
    RE5_DEBUG("shell32", "SHGetFolderPathA(0x%X) -> %s", csidl, path);
    c->eax = S_OK;
}

/* Sem janela de verdade (Fase 4), arrastar arquivos não existe: aceitar ou
 * recusar o registro não muda nada observável para o jogo. */
static void s_DragAcceptFiles(CPU *c) { (void)c; }

/* O RE5 só abre a página de privacidade da Capcom (opção de menu): URL http(s)
 * com "open" vai para o navegador do host. Outro uso aborta (D8). Valor > 32 é
 * sucesso; SE_ERR_NOASSOC (31) se o host não abriu. */
static void s_ShellExecuteA(CPU *c)
{
    const char *op = re5_gstr(re5_arg(c, 1)), *file = re5_gstr(re5_arg(c, 2));
    int url = file && (!strncmp(file, "http://", 7) || !strncmp(file, "https://", 8));
    if (!url || (op && strcmp(op, "open") != 0))
        re5_fatal("shell32", "ShellExecuteA(\"%s\", \"%s\") sem implementação: só abre URL http(s)",
                  op ? op : "(null)", file ? file : "(null)");
    RE5_INFO("shell32", "ShellExecuteA: abrindo %s no navegador do host", file);
    c->eax = SDL_OpenURL(file) == 0 ? 42u : 31u;
}

static const re5_import_def k_defs[] = {
    {"DragAcceptFiles",  s_DragAcceptFiles,   8, 0},
    {"ShellExecuteA",    s_ShellExecuteA,    24, 0},
    {"SHGetFolderPathA", s_SHGetFolderPathA, 20, 0},
    {0},
};

const re5_import_module re5_mod_shell32 = {"shell32.dll", k_defs};
