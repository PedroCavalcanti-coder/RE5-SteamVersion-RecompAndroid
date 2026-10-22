/*
 * vfs.c - caminhos do guest -> host (ver re5/vfs.h).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "re5/log.h"
#include "re5/platform.h"
#include "re5/vfs.h"

#if !defined(RE5_HOST_WINDOWS)
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif

#define MAXP 1024

static char g_game[MAXP];
static char g_user[MAXP];

static void strip_trailing_slash(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = 0;
}

int re5_vfs_init(const char *game_root, const char *user_root)
{
    if (!game_root || snprintf(g_game, sizeof g_game, "%s", game_root) >= (int)sizeof g_game) return -1;
    strip_trailing_slash(g_game);
    int n = user_root ? snprintf(g_user, sizeof g_user, "%s", user_root)
                      : snprintf(g_user, sizeof g_user, "%s/user", g_game);
    if (n < 0 || n >= (int)sizeof g_user) return -1;
    strip_trailing_slash(g_user);
    RE5_DEBUG("vfs", "jogo em %s, dados do usuário em %s", g_game, g_user);
    return 0;
}

const char *re5_vfs_game_root(void) { return g_game; }

/* Compara o começo de `path` (já com '/') com `prefix`, sem diferenciar maiúsculas. */
static size_t prefix_ci(const char *path, const char *prefix)
{
    size_t i = 0;
    for (; prefix[i]; i++) {
        char a = path[i] == '\\' ? '/' : path[i], b = prefix[i] == '\\' ? '/' : prefix[i];
        if (!a || tolower((unsigned char)a) != tolower((unsigned char)b)) return 0;
    }
    return (path[i] == 0 || path[i] == '/' || path[i] == '\\') ? i : 0;
}

#if !defined(RE5_HOST_WINDOWS)
/* Acrescenta `comp` a `dir`, trocando pelo nome real se existir com outra caixa. */
static void append_component(char *dir, size_t cap, const char *comp, int *exists)
{
    size_t n = strlen(dir);
    char cand[MAXP];
    snprintf(cand, sizeof cand, "%s/%s", dir, comp);
    struct stat st;
    if (*exists && stat(cand, &st) != 0) {
        DIR *d = opendir(dir);
        struct dirent *e;
        int found = 0;
        while (d && (e = readdir(d))) {
            if (strcasecmp(e->d_name, comp) == 0) {
                snprintf(dir + n, cap - n, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        if (d) closedir(d);
        if (found) return;
        *exists = 0;   /* daqui para baixo nada existe: mantém os nomes como vieram */
    }
    snprintf(dir + n, cap - n, "/%s", comp);
}
#endif

int re5_vfs_resolve(const char *guest_path, char *out, size_t out_size)
{
    if (!guest_path || !g_game[0] || out_size < 2) return -1;
    const char *p = guest_path;
    if (strncmp(p, "\\\\?\\", 4) == 0) p += 4;

    char base[MAXP];
    size_t skip;
    int n;
    if ((skip = prefix_ci(p, RE5_VFS_INSTALL_DIR)) != 0) {
        n = snprintf(base, sizeof base, "%s", g_game);
        p += skip;
    } else if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        n = snprintf(base, sizeof base, "%s/drive_%c", g_user, tolower((unsigned char)p[0]));
        p += 2;
    } else if (p[0] == '\\' || p[0] == '/') {
        n = snprintf(base, sizeof base, "%s/drive_c", g_user);
    } else {
        n = snprintf(base, sizeof base, "%s", g_game);   /* relativo: CWD do guest = pasta de instalação */
    }
    if (n < 0 || n >= (int)sizeof base) return -1;

    /* Componentes, com "." e ".." resolvidos sem subir acima da base. */
    char comps[64][256];
    int nc = 0;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        size_t k = 0;
        while (p[k] && p[k] != '\\' && p[k] != '/') k++;
        if (k >= sizeof comps[0]) return -1;
        if (k == 1 && p[0] == '.') {
            /* nada */
        } else if (k == 2 && p[0] == '.' && p[1] == '.') {
            if (nc > 0) nc--;
        } else {
            if (nc >= 64) return -1;
            memcpy(comps[nc], p, k);
            comps[nc++][k] = 0;
        }
        p += k;
    }

    char res[MAXP];
    snprintf(res, sizeof res, "%s", base);
#if defined(RE5_HOST_WINDOWS)
    for (int i = 0; i < nc; i++) {
        size_t n = strlen(res);
        snprintf(res + n, sizeof res - n, "\\%s", comps[i]);
    }
#else
    int exists = 1;
    for (int i = 0; i < nc; i++) append_component(res, sizeof res, comps[i], &exists);
#endif
    if (strlen(res) + 1 > out_size) return -1;
    memcpy(out, res, strlen(res) + 1);
    return 0;
}
