/*
 * vfs.h - caminhos do guest (Windows) -> caminhos do host.
 *
 * O jogo abre arquivos como "nativePC\arc\rom\s\s100.arc" (relativo à pasta
 * de instalação) ou "C:\Users\...\Documents\CAPCOM\RESIDENT EVIL 5\..." (saves).
 * No Linux/Android o sistema de arquivos diferencia maiúsculas, então cada
 * componente é resolvido sem diferenciar (como o NTFS faria).
 *
 *   relativo ou dentro de RE5_VFS_INSTALL_DIR  -> <game_root>/...
 *   qualquer outro absoluto "X:\resto"          -> <user_root>/drive_x/resto
 */
#ifndef RE5_VFS_H
#define RE5_VFS_H

#include <stddef.h>

/* Pasta de instalação que o guest enxerga (também vai para o registro). */
#define RE5_VFS_INSTALL_DIR "C:\\RE5"

/* game_root: pasta com os dados do jogo (nativePC/ ...). user_root: onde
 * ficam saves e configurações gravadas pelo jogo (NULL = game_root/user). */
int re5_vfs_init(const char *game_root, const char *user_root);

/* Resolve um caminho do guest. 0 = ok (o arquivo pode não existir: o último
 * componente que não existe é mantido como veio, para CreateFile criar). */
int re5_vfs_resolve(const char *guest_path, char *out, size_t out_size);

const char *re5_vfs_game_root(void);

#endif /* RE5_VFS_H */
