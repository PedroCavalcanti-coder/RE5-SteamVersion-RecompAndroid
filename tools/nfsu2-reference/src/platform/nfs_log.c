#define _CRT_SECURE_NO_WARNINGS
#include "nfs_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *k_names[NFS_CAT_COUNT] = {
    "BOOT", "CPU", "ICALL", "IMPORT", "WIN32", "D3D9", "THREAD", "FILE", "AUDIO", "CRASH"
};
static FILE *g_file;
static char g_dir[512] = ".";
static char g_stamp[32] = "nostamp";

void nfs_log_init(const char *log_dir) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(g_stamp, sizeof g_stamp, "%Y%m%d_%H%M%S", tm);
    if (!log_dir) return;
    strncpy(g_dir, log_dir, sizeof g_dir - 1);
    char path[600];
    snprintf(path, sizeof path, "%s/run_%s.log", g_dir, g_stamp);
    g_file = fopen(path, "w");
    if (g_file) setvbuf(g_file, NULL, _IOLBF, 1 << 16);
}

const char *nfs_log_dir(void) { return g_dir; }
const char *nfs_log_stamp(void) { return g_stamp; }

void nfs_logv(nfs_cat_t cat, const char *fmt, va_list ap) {
    char line[2048];
    int n = snprintf(line, sizeof line, "[NFSU2:%s] ", k_names[cat]);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    fputs(line, stderr); fputc('\n', stderr);
    if (g_file) { fputs(line, g_file); fputc('\n', g_file); }
}

void nfs_log(nfs_cat_t cat, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nfs_logv(cat, fmt, ap);
    va_end(ap);
}

void nfs_log_flush(void) {
    fflush(stderr);
    if (g_file) fflush(g_file);
}
