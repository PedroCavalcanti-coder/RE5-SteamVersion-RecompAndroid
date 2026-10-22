/*
 * log.c - log do runtime (ver re5/log.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "re5/log.h"
#include "re5/runtime.h"

static re5_log_level g_level = RE5_LOG_INFO;
static FILE *g_file;
static const char *const k_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

void re5_log_init(void)
{
    const char *lv = getenv("RE5_LOG");
    if (lv) {
        for (int i = 0; i <= RE5_LOG_TRACE; i++) {
            const char *n = k_names[i];
            size_t k = 0;
            while (n[k] && lv[k] && (n[k] | 0x20) == (lv[k] | 0x20)) k++;
            if (!n[k] && !lv[k]) g_level = (re5_log_level)i;
        }
    }
    const char *path = getenv("RE5_LOG_FILE");
    if (path && !g_file) g_file = fopen(path, "w");
}

void re5_log_set_level(re5_log_level level) { g_level = level; }
int re5_log_enabled(re5_log_level level) { return level <= g_level; }

static void vlog(re5_log_level level, const char *channel, const char *fmt, va_list ap)
{
    char line[2048];
    int n = snprintf(line, sizeof line, "[%-5s] %-9s ", k_names[level], channel);
    if (n < 0) return;
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    fprintf(stderr, "%s\n", line);
    if (g_file) {
        fprintf(g_file, "%s\n", line);
        fflush(g_file);
    }
}

void re5_log(re5_log_level level, const char *channel, const char *fmt, ...)
{
    if (!re5_log_enabled(level)) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(level, channel, fmt, ap);
    va_end(ap);
}

void re5_fatal(const char *channel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(RE5_LOG_ERROR, channel, fmt, ap);
    va_end(ap);
    re5_dispatch_trail_print();
    fflush(stderr);
    if (g_file) fflush(g_file);
    abort();
}

void re5_recomp_todo(uint32_t va, const char *what)
{
    re5_fatal("lift", "instrução sem tradução em 0x%08X: %s (ver lift_stats.json, recomp_todo_by_mnemonic)",
              va, what);
}
