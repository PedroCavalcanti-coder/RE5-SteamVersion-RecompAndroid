/*
 * nfs_log.h - categorised logging for the NFSU2 recompilation host.
 *
 *   NFS_LOG(BOOT, "mapped %s at 0x%08X", path, base);
 *   -> [NFSU2:BOOT] mapped ... (stderr and logs/run_<timestamp>.log)
 *
 * Portable C (no Windows types), so the same interface serves the future
 * Android host; only nfs_log.c knows where the lines go.
 */
#ifndef NFS_LOG_H
#define NFS_LOG_H
#include <stdarg.h>

typedef enum {
    NFS_BOOT, NFS_CPU, NFS_ICALL, NFS_IMPORT, NFS_WIN32, NFS_D3D9, NFS_THREAD,
    NFS_FILE, NFS_AUDIO, NFS_CRASH, NFS_CAT_COUNT
} nfs_cat_t;

/* Open logs/run_<timestamp>.log under `log_dir` (NULL: stderr only). */
void nfs_log_init(const char *log_dir);
void nfs_log(nfs_cat_t cat, const char *fmt, ...);
void nfs_logv(nfs_cat_t cat, const char *fmt, va_list ap);
void nfs_log_flush(void);
/* Directory the run log lives in (for crash files), or "." */
const char *nfs_log_dir(void);
const char *nfs_log_stamp(void);

#define NFS_LOG(cat, ...) nfs_log(NFS_##cat, __VA_ARGS__)

#endif
