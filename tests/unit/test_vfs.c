/* VFS: caminhos do Windows resolvidos sem diferenciar maiúsculas sobre um diretório real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "check.h"
#include "re5/runtime.h"

const re5_func_entry re5_lifted_table[1] = {{0u, 0}};
const uint32_t re5_lifted_count = 0;

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

int main(int argc, char **argv)
{
    const char *tmp = argc > 1 ? argv[1] : "vfs_fixture";
    char root[512], p[1024], out[1024];
    snprintf(root, sizeof root, "%s/game", tmp);
    mkdir(tmp, 0755);
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/nativePC", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/nativePC/Arc", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/nativePC/Arc/s100.ARC", root); touch(p);

    CHECK(re5_vfs_init(root, NULL) == 0);

    /* Relativo, com outra caixa e barras invertidas. */
    CHECK(re5_vfs_resolve("NATIVEPC\\arc\\S100.arc", out, sizeof out) == 0);
    snprintf(p, sizeof p, "%s/nativePC/Arc/s100.ARC", root);
    CHECK(strcmp(out, p) == 0);

    /* Absoluto dentro da pasta de instalação virtual, com "." e "..". */
    CHECK(re5_vfs_resolve("c:\\re5\\nativepc\\.\\x\\..\\ARC\\s100.arc", out, sizeof out) == 0);
    CHECK(strcmp(out, p) == 0);

    /* Arquivo novo: diretórios existentes corrigidos, nome novo mantido. */
    CHECK(re5_vfs_resolve("nativepc\\arc\\Novo.sav", out, sizeof out) == 0);
    snprintf(p, sizeof p, "%s/nativePC/Arc/Novo.sav", root);
    CHECK(strcmp(out, p) == 0);

    /* Fora da instalação: vai para a pasta do usuário, por drive. */
    CHECK(re5_vfs_resolve("D:\\Users\\x\\Documents\\CAPCOM\\save.bin", out, sizeof out) == 0);
    snprintf(p, sizeof p, "%s/user/drive_d/Users/x/Documents/CAPCOM/save.bin", root);
    CHECK(strcmp(out, p) == 0);

    /* ".." não sobe acima da raiz do jogo. */
    CHECK(re5_vfs_resolve("..\\..\\etc\\passwd", out, sizeof out) == 0);
    snprintf(p, sizeof p, "%s/etc/passwd", root);
    CHECK(strcmp(out, p) == 0);

    CHECK_DONE();
}
