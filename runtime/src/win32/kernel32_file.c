/*
 * kernel32_file.c - arquivos, diretórios e INI sobre o VFS (Fase 3, etapa 3.3).
 *
 * Cada HANDLE de arquivo é um FILE* do host (RE5_H_FILE). Os caminhos do guest
 * passam pelo re5_vfs_resolve: a pasta de instalação C:\RE5 vira a pasta do
 * jogo, o resto vai para a pasta do usuário. Os .arc são lidos pelo próprio
 * código do jogo; aqui só existe E/S de bytes.
 *
 * E/S assíncrona (K32_FILE_FLAG_OVERLAPPED, ReadFileEx): a leitura é feita na hora
 * e a conclusão é entregue como manda o Win32 - evento do OVERLAPPED sinalizado
 * e completion routine enfileirada como APC, que roda no próximo SleepEx
 * alertável da mesma thread.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "re5/user32.h"
#include "kernel32_internal.h"

#if defined(RE5_HOST_WINDOWS)
#include <direct.h>
#include <io.h>
#include <sys/utime.h>
#include <windows.h>
#define host_seek _fseeki64
#define host_tell _ftelli64
#define host_mkdir(p) _mkdir(p)
static int host_truncate(FILE *f, int64_t n) { return _chsize_s(_fileno(f), n) == 0 ? 0 : -1; }
#else
#include <sys/statvfs.h>
#include <unistd.h>
#include <utime.h>
#define host_seek fseeko
#define host_tell ftello
#define host_mkdir(p) mkdir((p), 0755)
static int host_truncate(FILE *f, int64_t n) { return ftruncate(fileno(f), (off_t)n); }
#endif

#define K32_GENERIC_READ      0x80000000u
#define K32_GENERIC_WRITE     0x40000000u
#define K32_CREATE_NEW        1u
#define K32_CREATE_ALWAYS     2u
#define K32_OPEN_EXISTING     3u
#define K32_OPEN_ALWAYS       4u
#define K32_TRUNCATE_EXISTING 5u
#define K32_FILE_FLAG_OVERLAPPED 0x40000000u
#define K32_INVALID_HANDLE    0xFFFFFFFFu
#define K32_FILE_ATTRIBUTE_DIRECTORY 0x10u
#define K32_FILE_ATTRIBUTE_ARCHIVE   0x20u
#define K32_FILE_ATTRIBUTE_NORMAL    0x80u
#define K32_ERROR_PATH_NOT_FOUND  3u
#define K32_ERROR_ACCESS_DENIED   5u
#define K32_ERROR_INVALID_HANDLE  6u
#define K32_ERROR_HANDLE_EOF      38u
#define K32_ERROR_FILE_EXISTS     80u
#define K32_ERROR_ALREADY_EXISTS  183u
#define K32_ERROR_NO_MORE_FILES   18u
#define K32_ERROR_NEGATIVE_SEEK   131u
#define EPOCH_DIFF_100NS 116444736000000000ull
#define MAXP 1024

typedef struct {
    FILE    *f;
    uint32_t access;
    uint32_t flags;
    char     path[MAXP];
} host_file;

static void destroy_file(void *p)
{
    host_file *hf = p;
    if (hf->f) fclose(hf->f);
    free(hf);
}

static int resolve(uint32_t guest_va, char *out)
{
    const char *g = re5_gstr(guest_va);
    if (!g || re5_vfs_resolve(g, out, MAXP) != 0) {
        re5_set_last_error(K32_ERROR_PATH_NOT_FOUND);
        return -1;
    }
    return 0;
}

static void set_errno_error(void)
{
    switch (errno) {
    case ENOENT: re5_set_last_error(K32_ERROR_FILE_NOT_FOUND); break;
    case ENOTDIR: re5_set_last_error(K32_ERROR_PATH_NOT_FOUND); break;
    case EEXIST: re5_set_last_error(K32_ERROR_ALREADY_EXISTS); break;
    default: re5_set_last_error(K32_ERROR_ACCESS_DENIED); break;
    }
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static uint64_t to_filetime(time_t t) { return (uint64_t)t * 10000000ull + EPOCH_DIFF_100NS; }

static host_file *get_file(uint32_t h)
{
    host_file *hf = re5_handle_get(h, RE5_H_FILE);
    if (!hf) re5_set_last_error(K32_ERROR_INVALID_HANDLE);
    return hf;
}

/* ---- abrir, ler, escrever ---- */

static void k_CreateFileA(CPU *c)
{
    uint32_t access = re5_arg(c, 1), disp = re5_arg(c, 4), flags = re5_arg(c, 5);
    const char *guest = re5_gstr(re5_arg(c, 0));
    char path[MAXP];
    c->eax = K32_INVALID_HANDLE;
    re5_user32_script_file(guest);
    if (resolve(re5_arg(c, 0), path) != 0) return;
    if (is_dir(path)) {
        re5_fatal("kernel32", "CreateFileA(\"%s\"): abrir diretório (FILE_FLAG_BACKUP_SEMANTICS) sem implementação",
                  guest);
    }
    int had = exists(path);
    const char *mode = NULL;
    switch (disp) {
    case K32_CREATE_NEW:
        if (had) { re5_set_last_error(K32_ERROR_FILE_EXISTS); return; }
        mode = "w+b";
        break;
    case K32_CREATE_ALWAYS: mode = "w+b"; break;
    case K32_OPEN_EXISTING:
        if (!had) { re5_set_last_error(K32_ERROR_FILE_NOT_FOUND); return; }
        mode = (access & K32_GENERIC_WRITE) ? "r+b" : "rb";
        break;
    case K32_OPEN_ALWAYS: mode = had ? ((access & K32_GENERIC_WRITE) ? "r+b" : "rb") : "w+b"; break;
    case K32_TRUNCATE_EXISTING:
        if (!had) { re5_set_last_error(K32_ERROR_FILE_NOT_FOUND); return; }
        mode = "w+b";
        break;
    default:
        re5_set_last_error(K32_ERROR_INVALID_PARAMETER);
        return;
    }
    FILE *f = fopen(path, mode);
    if (!f) {
        set_errno_error();
        RE5_DEBUG("kernel32", "CreateFileA(\"%s\" -> %s, %s): falhou", guest, path, mode);
        return;
    }
    host_file *hf = calloc(1, sizeof *hf);
    if (!hf) { fclose(f); re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY); return; }
    hf->f = f;
    hf->access = access;
    hf->flags = flags;
    snprintf(hf->path, sizeof hf->path, "%s", path);
    c->eax = re5_handle_new(RE5_H_FILE, hf, destroy_file);
    /* K32_OPEN_ALWAYS/K32_CREATE_ALWAYS de um arquivo que já existia: ERROR_ALREADY_EXISTS. */
    re5_set_last_error((had && (disp == K32_OPEN_ALWAYS || disp == K32_CREATE_ALWAYS)) ? K32_ERROR_ALREADY_EXISTS
                                                                                : K32_ERROR_SUCCESS);
    RE5_DEBUG("kernel32", "CreateFileA(\"%s\") -> 0x%X", guest, c->eax);
}

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. */
static int64_t overlapped_offset(uint32_t ov) { return (int64_t)((uint64_t)k32_r32(ov + 12) << 32 | k32_r32(ov + 8)); }

static void overlapped_done(uint32_t ov, uint32_t status, uint32_t bytes)
{
    k32_w32(ov, status);
    k32_w32(ov + 4, bytes);
    uint32_t ev = k32_r32(ov + 16);
    if (ev) k32_event_signal(ev);
}

static int do_io(host_file *hf, int write, uint32_t buf, uint32_t n, int64_t at, uint32_t *done)
{
    *done = 0;
    if (write ? !(hf->access & K32_GENERIC_WRITE) : !(hf->access & K32_GENERIC_READ)) {
        re5_set_last_error(K32_ERROR_ACCESS_DENIED);
        return 0;
    }
    if (n && !re5_mem_is_mapped(buf, n)) re5_fatal("kernel32", "%s: buffer 0x%08X+%u fora da memória do guest",
                                                   write ? "WriteFile" : "ReadFile", buf, n);
    RE5_BLOCKING_BEGIN();
    int ok = 1;
    if (at >= 0 && host_seek(hf->f, at, SEEK_SET) != 0) ok = 0;
    if (ok) {
        fflush(hf->f);   /* troca leitura <-> escrita no mesmo FILE* exige um seek ou flush */
        size_t r = n ? (write ? fwrite(re5_g2h(buf), 1, n, hf->f) : fread(re5_g2h(buf), 1, n, hf->f)) : 0;
        *done = (uint32_t)r;
        if (r < n && ferror(hf->f)) ok = 0;
        clearerr(hf->f);
    }
    RE5_BLOCKING_END();
    if (!ok) re5_set_last_error(K32_ERROR_ACCESS_DENIED);
    return ok;
}

static void rw_common(CPU *c, int write)
{
    host_file *hf = get_file(re5_arg(c, 0));
    uint32_t buf = re5_arg(c, 1), n = re5_arg(c, 2), done_va = re5_arg(c, 3), ov = re5_arg(c, 4);
    c->eax = 0;
    if (done_va) k32_w32(done_va, 0);
    if (!hf) return;
    uint32_t done;
    if (!do_io(hf, write, buf, n, ov ? overlapped_offset(ov) : -1, &done)) return;
    if (done_va) k32_w32(done_va, done);
    if (ov) {
        if (!write && n && !done) {      /* leitura sobreposta além do fim */
            overlapped_done(ov, 0xC0000011u, 0);   /* STATUS_END_OF_FILE */
            re5_set_last_error(K32_ERROR_HANDLE_EOF);
            return;
        }
        overlapped_done(ov, 0, done);
    }
    c->eax = 1;
}

static void k_ReadFile(CPU *c) { rw_common(c, 0); }
static void k_WriteFile(CPU *c) { rw_common(c, 1); }

static void k_ReadFileEx(CPU *c)
{
    host_file *hf = get_file(re5_arg(c, 0));
    uint32_t buf = re5_arg(c, 1), n = re5_arg(c, 2), ov = re5_arg(c, 3), routine = re5_arg(c, 4);
    c->eax = 0;
    if (!hf || !ov) { if (hf) re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    uint32_t done;
    if (!do_io(hf, 0, buf, n, overlapped_offset(ov), &done)) return;
    if (n && !done) {
        re5_set_last_error(K32_ERROR_HANDLE_EOF);
        return;
    }
    k32_w32(ov, 0);
    k32_w32(ov + 4, done);
    if (routine) k32_apc_queue(routine, 0 /* dwErrorCode */, done, ov);
    c->eax = 1;
}

static void k_SetFilePointer(CPU *c)
{
    host_file *hf = get_file(re5_arg(c, 0));
    uint32_t lo = re5_arg(c, 1), hi_va = re5_arg(c, 2), method = re5_arg(c, 3);
    c->eax = 0xFFFFFFFFu;   /* INVALID_SET_FILE_POINTER */
    if (!hf) return;
    int64_t dist = hi_va ? (int64_t)((uint64_t)k32_r32(hi_va) << 32 | lo) : (int64_t)(int32_t)lo;
    int64_t base = 0;
    fflush(hf->f);
    if (method == 1) base = host_tell(hf->f);
    else if (method == 2) {
        host_seek(hf->f, 0, SEEK_END);
        base = host_tell(hf->f);
    } else if (method != 0) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    int64_t pos = base + dist;
    if (pos < 0) { re5_set_last_error(K32_ERROR_NEGATIVE_SEEK); return; }
    if (host_seek(hf->f, pos, SEEK_SET) != 0) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    if (hi_va) k32_w32(hi_va, (uint32_t)((uint64_t)pos >> 32));
    else if (pos > 0x7FFFFFFF) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    re5_set_last_error(K32_ERROR_SUCCESS);   /* 0xFFFFFFFF pode ser posição válida com hi */
    c->eax = (uint32_t)pos;
}

static void k_GetFileSize(CPU *c)
{
    host_file *hf = get_file(re5_arg(c, 0));
    uint32_t hi_va = re5_arg(c, 1);
    c->eax = 0xFFFFFFFFu;   /* INVALID_FILE_SIZE */
    if (!hf) return;
    fflush(hf->f);
    int64_t cur = host_tell(hf->f);
    host_seek(hf->f, 0, SEEK_END);
    int64_t size = host_tell(hf->f);
    host_seek(hf->f, cur, SEEK_SET);
    if (hi_va) k32_w32(hi_va, (uint32_t)((uint64_t)size >> 32));
    re5_set_last_error(K32_ERROR_SUCCESS);
    c->eax = (uint32_t)size;
}

static void k_SetEndOfFile(CPU *c)
{
    host_file *hf = get_file(re5_arg(c, 0));
    c->eax = 0;
    if (!hf) return;
    fflush(hf->f);
    if (host_truncate(hf->f, host_tell(hf->f)) != 0) { set_errno_error(); return; }
    c->eax = 1;
}

static void k_FlushFileBuffers(CPU *c)
{
    host_file *hf = get_file(re5_arg(c, 0));
    c->eax = hf && fflush(hf->f) == 0;
}

static void k_SetFileTime(CPU *c)
{
    host_file *hf = get_file(re5_arg(c, 0));
    uint32_t write_va = re5_arg(c, 3), access_va = re5_arg(c, 2);
    c->eax = 0;
    if (!hf) return;
    c->eax = 1;
    if (!write_va && !access_va) return;
    struct stat st;
    if (stat(hf->path, &st) != 0) return;
    struct utimbuf ut = {st.st_atime, st.st_mtime};
    uint64_t ft;
    if (access_va && (ft = (uint64_t)k32_r32(access_va + 4) << 32 | k32_r32(access_va)) >= EPOCH_DIFF_100NS)
        ut.actime = (time_t)((ft - EPOCH_DIFF_100NS) / 10000000ull);
    if (write_va && (ft = (uint64_t)k32_r32(write_va + 4) << 32 | k32_r32(write_va)) >= EPOCH_DIFF_100NS)
        ut.modtime = (time_t)((ft - EPOCH_DIFF_100NS) / 10000000ull);
    fflush(hf->f);
    utime(hf->path, &ut);
}

/* ---- atributos e diretórios ---- */

static void k_GetFileAttributesA(CPU *c)
{
    char path[MAXP];
    struct stat st;
    c->eax = 0xFFFFFFFFu;   /* INVALID_FILE_ATTRIBUTES */
    if (resolve(re5_arg(c, 0), path) != 0) return;
    if (stat(path, &st) != 0) { set_errno_error(); return; }
    c->eax = S_ISDIR(st.st_mode) ? K32_FILE_ATTRIBUTE_DIRECTORY : K32_FILE_ATTRIBUTE_ARCHIVE;
}

static void k_CreateDirectoryA(CPU *c)
{
    char path[MAXP];
    c->eax = 0;
    if (resolve(re5_arg(c, 0), path) != 0) return;
    if (exists(path)) { re5_set_last_error(K32_ERROR_ALREADY_EXISTS); return; }
    if (host_mkdir(path) != 0) {
        re5_set_last_error(errno == ENOENT ? K32_ERROR_PATH_NOT_FOUND : K32_ERROR_ACCESS_DENIED);
        return;
    }
    c->eax = 1;
}

static void k_DeleteFileA(CPU *c)
{
    char path[MAXP];
    c->eax = 0;
    if (resolve(re5_arg(c, 0), path) != 0) return;
    if (remove(path) != 0) { set_errno_error(); return; }
    c->eax = 1;
}

static void k_MoveFileA(CPU *c)
{
    char from[MAXP], to[MAXP];
    c->eax = 0;
    if (resolve(re5_arg(c, 0), from) != 0 || resolve(re5_arg(c, 1), to) != 0) return;
    if (!exists(from)) { re5_set_last_error(K32_ERROR_FILE_NOT_FOUND); return; }
    if (exists(to)) { re5_set_last_error(K32_ERROR_ALREADY_EXISTS); return; }
    if (rename(from, to) != 0) { set_errno_error(); return; }
    c->eax = 1;
}

static void k_CopyFileA(CPU *c)
{
    char from[MAXP], to[MAXP];
    c->eax = 0;
    if (resolve(re5_arg(c, 0), from) != 0 || resolve(re5_arg(c, 1), to) != 0) return;
    if (re5_arg(c, 2) && exists(to)) { re5_set_last_error(K32_ERROR_FILE_EXISTS); return; }
    FILE *in = fopen(from, "rb");
    if (!in) { set_errno_error(); return; }
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); set_errno_error(); return; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) { re5_set_last_error(K32_ERROR_ACCESS_DENIED); return; }
    c->eax = 1;
}

static void k_GetDiskFreeSpaceA(CPU *c)
{
    uint32_t spc = re5_arg(c, 1), bps = re5_arg(c, 2), freec = re5_arg(c, 3), total = re5_arg(c, 4);
    uint64_t free_bytes = 0, total_bytes = 0;
#if defined(RE5_HOST_WINDOWS)
    ULARGE_INTEGER fb, tb;
    if (GetDiskFreeSpaceExA(re5_vfs_game_root(), &fb, &tb, NULL)) {
        free_bytes = fb.QuadPart;
        total_bytes = tb.QuadPart;
    }
#else
    struct statvfs sv;
    if (statvfs(re5_vfs_game_root(), &sv) == 0) {
        free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
        total_bytes = (uint64_t)sv.f_blocks * sv.f_frsize;
    }
#endif
    /* Clusters de 4 KiB; a API de 32 bits satura, como no Windows. */
    uint64_t fcl = free_bytes / 4096, tcl = total_bytes / 4096;
    if (spc) k32_w32(spc, 8);
    if (bps) k32_w32(bps, 512);
    if (freec) k32_w32(freec, fcl > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)fcl);
    if (total) k32_w32(total, tcl > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)tcl);
    c->eax = 1;
}

/* ---- FindFirstFileA / FindNextFileA ---- */

typedef struct {
    char   (*names)[260];
    uint32_t n, next;
    char     dir[MAXP];
} find_state;

static void destroy_find(void *p)
{
    find_state *fs = p;
    free(fs->names);
    free(fs);
}

static int wild_match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (!pat[1]) return 1;
            for (; *s; s++) if (wild_match(pat + 1, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}

/* WIN32_FIND_DATAA: atributos, 3 FILETIME, tamanho (alto, baixo), 2 reservados, nome[260], nome 8.3[14]. */
static void fill_find_data(uint32_t fd, const char *dir, const char *name)
{
    char full[MAXP * 2];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    struct stat st;
    memset(re5_g2h(fd), 0, 320);
    if (stat(full, &st) == 0) {
        uint64_t ft = to_filetime(st.st_mtime);
        k32_w32(fd, S_ISDIR(st.st_mode) ? K32_FILE_ATTRIBUTE_DIRECTORY : K32_FILE_ATTRIBUTE_ARCHIVE);
        for (int i = 0; i < 3; i++) {
            k32_w32(fd + 4 + 8 * (uint32_t)i, (uint32_t)ft);
            k32_w32(fd + 8 + 8 * (uint32_t)i, (uint32_t)(ft >> 32));
        }
        uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        k32_w32(fd + 28, (uint32_t)(size >> 32));
        k32_w32(fd + 32, (uint32_t)size);
    }
    size_t n = strlen(name);
    if (n > 259) n = 259;
    memcpy(re5_g2h(fd + 44), name, n);
}

static void k_FindFirstFileA(CPU *c)
{
    const char *guest = re5_gstr(re5_arg(c, 0));
    uint32_t fd = re5_arg(c, 1);
    c->eax = K32_INVALID_HANDLE;
    if (!guest) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    /* Separa a pasta do padrão: só o último componente pode ter curingas. */
    char gdir[MAXP], pattern[260], dir[MAXP];
    const char *slash = strrchr(guest, '\\');
    const char *fslash = strrchr(guest, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (slash) {
        snprintf(gdir, sizeof gdir, "%.*s", (int)(slash - guest), guest);
        if (!gdir[0] || (gdir[1] == ':' && !gdir[2])) snprintf(gdir + strlen(gdir), sizeof gdir - strlen(gdir), "\\");
        snprintf(pattern, sizeof pattern, "%s", slash + 1);
    } else {
        snprintf(gdir, sizeof gdir, ".");
        snprintf(pattern, sizeof pattern, "%s", guest);
    }
    if (strpbrk(gdir, "*?")) re5_fatal("kernel32", "FindFirstFileA(\"%s\"): curinga fora do último componente", guest);
    if (re5_vfs_resolve(gdir, dir, sizeof dir) != 0 || !is_dir(dir)) {
        re5_set_last_error(K32_ERROR_PATH_NOT_FOUND);
        return;
    }
    if (!strcmp(pattern, "*.*")) snprintf(pattern, sizeof pattern, "*");
    find_state *fs = calloc(1, sizeof *fs);
    if (!fs) { re5_set_last_error(K32_ERROR_NOT_ENOUGH_MEMORY); return; }
    snprintf(fs->dir, sizeof fs->dir, "%s", dir);
    DIR *d = opendir(dir);
    struct dirent *e;
    uint32_t cap = 0;
    while (d && (e = readdir(d))) {
        if (!wild_match(pattern, e->d_name)) continue;
        if (fs->n == cap) {
            cap = cap ? cap * 2 : 32;
            void *nn = realloc(fs->names, cap * sizeof *fs->names);
            if (!nn) break;
            fs->names = nn;
        }
        snprintf(fs->names[fs->n++], 260, "%s", e->d_name);
    }
    if (d) closedir(d);
    if (!fs->n) {
        destroy_find(fs);
        re5_set_last_error(K32_ERROR_FILE_NOT_FOUND);
        return;
    }
    fill_find_data(fd, fs->dir, fs->names[0]);
    fs->next = 1;
    c->eax = re5_handle_new(RE5_H_FIND, fs, destroy_find);
}

static void k_FindNextFileA(CPU *c)
{
    find_state *fs = re5_handle_get(re5_arg(c, 0), RE5_H_FIND);
    c->eax = 0;
    if (!fs) { re5_set_last_error(K32_ERROR_INVALID_HANDLE); return; }
    if (fs->next >= fs->n) { re5_set_last_error(K32_ERROR_NO_MORE_FILES); return; }
    fill_find_data(re5_arg(c, 1), fs->dir, fs->names[fs->next++]);
    c->eax = 1;
}

static void k_FindClose(CPU *c)
{
    uint32_t h = re5_arg(c, 0);
    c->eax = re5_handle_type_of(h) == RE5_H_FIND && re5_handle_close(h) == 0;
    if (!c->eax) re5_set_last_error(K32_ERROR_INVALID_HANDLE);
}

/* ---- INI (GetPrivateProfileStringA / WritePrivateProfileStringA) ---- */

#define INI_MAX_LINES 2048

typedef struct {
    char *line[INI_MAX_LINES];
    int   n;
} ini_file;

static void ini_path(uint32_t file_va, char *out)
{
    const char *f = re5_gstr(file_va);
    char guest[MAXP];
    /* Sem pasta, o Windows procura o INI na pasta do Windows. */
    if (f && !strpbrk(f, "\\/")) snprintf(guest, sizeof guest, "C:\\Windows\\%s", f);
    else snprintf(guest, sizeof guest, "%s", f ? f : "");
    if (re5_vfs_resolve(guest, out, MAXP) != 0) out[0] = 0;
}

static void ini_load(const char *path, ini_file *ini)
{
    ini->n = 0;
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    char buf[1024];
    while (f && ini->n < INI_MAX_LINES && fgets(buf, sizeof buf, f)) {
        size_t k = strlen(buf);
        while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
        ini->line[ini->n++] = strdup(buf);
    }
    if (f) fclose(f);
}

static void ini_free(ini_file *ini)
{
    for (int i = 0; i < ini->n; i++) free(ini->line[i]);
    ini->n = 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t k = strlen(s);
    while (k && (s[k - 1] == ' ' || s[k - 1] == '\t')) s[--k] = 0;
    return s;
}

/* Nome da seção se a linha for "[nome]", senão NULL (escreve em buf). */
static const char *ini_section(const char *line, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", line);
    char *s = trim(buf);
    size_t k = strlen(s);
    if (k < 2 || s[0] != '[' || s[k - 1] != ']') return NULL;
    s[k - 1] = 0;
    return trim(s + 1);
}

/* "chave=valor": devolve a chave (em kbuf) e *val aponta para o valor aparado. */
static const char *ini_key(const char *line, char *kbuf, size_t cap, const char **val)
{
    const char *eq = strchr(line, '=');
    if (!eq || line[0] == ';') return NULL;
    snprintf(kbuf, cap, "%.*s", (int)(eq - line), line);
    *val = eq + 1;
    while (**val == ' ' || **val == '\t') (*val)++;
    return trim(kbuf);
}

static int stricmp_a(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

/* Copia uma lista "a\0b\0\0" para o guest, truncando como o Windows (size-2). */
static uint32_t put_list(uint32_t buf, uint32_t size, char names[][256], int n)
{
    if (size < 2) { if (size) k32_w8(buf, 0); return 0; }
    uint32_t pos = 0;
    for (int i = 0; i < n; i++) {
        uint32_t len = (uint32_t)strlen(names[i]);
        if (pos + len + 2 > size) {
            uint32_t room = size - 2 - pos;
            memcpy(re5_g2h(buf + pos), names[i], room);
            k32_w8(buf + pos + room, 0);
            k32_w8(buf + size - 1, 0);
            return size - 2;
        }
        memcpy(re5_g2h(buf + pos), names[i], len + 1);
        pos += len + 1;
    }
    k32_w8(buf + pos, 0);
    return pos;
}

static void k_GetPrivateProfileStringA(CPU *c)
{
    const char *app = re5_gstr(re5_arg(c, 0)), *key = re5_gstr(re5_arg(c, 1)), *def = re5_gstr(re5_arg(c, 2));
    uint32_t buf = re5_arg(c, 3), size = re5_arg(c, 4);
    char path[MAXP];
    ini_path(re5_arg(c, 5), path);
    ini_file ini;
    ini_load(path, &ini);
    static char names[256][256];
    int nn = 0, in_sec = 0;
    const char *found = NULL;
    char sbuf[1024], kbuf[1024];
    for (int i = 0; i < ini.n && !found; i++) {
        const char *sec = ini_section(ini.line[i], sbuf, sizeof sbuf);
        if (sec) {
            in_sec = app && !stricmp_a(sec, app);
            if (!app && nn < 256) snprintf(names[nn++], 256, "%s", sec);
            continue;
        }
        const char *val, *k;
        if (!in_sec || !(k = ini_key(ini.line[i], kbuf, sizeof kbuf, &val))) continue;
        if (!key) { if (nn < 256) snprintf(names[nn++], 256, "%s", k); }
        else if (!stricmp_a(k, key)) found = val;
    }
    if (!app || !key) {
        c->eax = put_list(buf, size, names, nn);
        ini_free(&ini);
        return;
    }
    char value[1024];
    snprintf(value, sizeof value, "%s", found ? found : (def ? def : ""));
    char *v = found ? trim(value) : value;
    if (!found) { size_t k = strlen(v); while (k && v[k - 1] == ' ') v[--k] = 0; }
    size_t len = strlen(v);
    if (found && len >= 2 && (v[0] == '"' || v[0] == '\'') && v[len - 1] == v[0]) {
        v[len - 1] = 0;
        v++;
        len -= 2;
    }
    ini_free(&ini);
    if (!size) { c->eax = 0; return; }
    if (len >= size) len = size - 1;
    memcpy(re5_g2h(buf), v, len);
    k32_w8(buf + (uint32_t)len, 0);
    c->eax = (uint32_t)len;
}

static void k_WritePrivateProfileStringA(CPU *c)
{
    const char *app = re5_gstr(re5_arg(c, 0)), *key = re5_gstr(re5_arg(c, 1)), *value = re5_gstr(re5_arg(c, 2));
    char path[MAXP];
    ini_path(re5_arg(c, 3), path);
    c->eax = 0;
    if (!app || !path[0]) { re5_set_last_error(K32_ERROR_INVALID_PARAMETER); return; }
    ini_file ini;
    ini_load(path, &ini);
    char sbuf[1024], kbuf[1024];
    int sec_start = -1, sec_end = ini.n, key_line = -1;
    for (int i = 0; i < ini.n; i++) {
        const char *sec = ini_section(ini.line[i], sbuf, sizeof sbuf);
        if (sec) {
            if (sec_start >= 0) { sec_end = i; break; }
            if (!stricmp_a(sec, app)) sec_start = i;
            continue;
        }
        const char *val, *k;
        if (sec_start >= 0 && key && (k = ini_key(ini.line[i], kbuf, sizeof kbuf, &val)) && !stricmp_a(k, key))
            key_line = i;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { set_errno_error(); ini_free(&ini); return; }
    for (int i = 0; i < ini.n; i++) {
        if (!key && sec_start >= 0 && i >= sec_start && i < sec_end) continue;   /* apaga a seção */
        if (i == key_line) {
            if (value) fprintf(f, "%s=%s\r\n", key, value);
            continue;
        }
        fprintf(f, "%s\r\n", ini.line[i]);
        if (key && value && key_line < 0 && sec_start >= 0 && i == sec_end - 1) fprintf(f, "%s=%s\r\n", key, value);
    }
    if (key && value && sec_start < 0) fprintf(f, "[%s]\r\n%s=%s\r\n", app, key, value);
    fclose(f);
    ini_free(&ini);
    c->eax = 1;
}

static const re5_import_def k_defs[] = {
    {"CopyFileA",                  k_CopyFileA,                  12, 0},
    {"CreateDirectoryA",           k_CreateDirectoryA,            8, 0},
    {"CreateFileA",                k_CreateFileA,                28, 0},
    {"DeleteFileA",                k_DeleteFileA,                 4, 0},
    {"FindClose",                  k_FindClose,                   4, 0},
    {"FindFirstFileA",             k_FindFirstFileA,              8, 0},
    {"FindNextFileA",              k_FindNextFileA,               8, 0},
    {"FlushFileBuffers",           k_FlushFileBuffers,            4, 0},
    {"GetDiskFreeSpaceA",          k_GetDiskFreeSpaceA,          20, 0},
    {"GetFileAttributesA",         k_GetFileAttributesA,          4, 0},
    {"GetFileSize",                k_GetFileSize,                 8, 0},
    {"GetPrivateProfileStringA",   k_GetPrivateProfileStringA,   24, 0},
    {"MoveFileA",                  k_MoveFileA,                   8, 0},
    {"ReadFile",                   k_ReadFile,                   20, 0},
    {"ReadFileEx",                 k_ReadFileEx,                 20, 0},
    {"SetEndOfFile",               k_SetEndOfFile,                4, 0},
    {"SetFilePointer",             k_SetFilePointer,             16, 0},
    {"SetFileTime",                k_SetFileTime,                16, 0},
    {"WriteFile",                  k_WriteFile,                  20, 0},
    {"WritePrivateProfileStringA", k_WritePrivateProfileStringA, 16, 0},
    {0},
};

const re5_import_module re5_mod_kernel32_file = {"kernel32.dll", k_defs};
