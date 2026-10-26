/*
 * log.c - log do runtime (ver re5/log.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#endif

#include "re5/log.h"
#include "re5/runtime.h"

static re5_log_level g_level = RE5_LOG_INFO;
static FILE *g_file;
static const char *const k_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

#ifdef __ANDROID__
static int s_pfd[2];
static pthread_t s_log_thread;

static void *android_log_pipe_thread(void *arg)
{
    (void)arg;
    ssize_t rdsz;
    char buf[2048];
    while ((rdsz = read(s_pfd[0], buf, sizeof(buf) - 1)) > 0) {
        if (buf[rdsz - 1] == '\n') rdsz--;
        buf[rdsz] = 0;
        __android_log_write(ANDROID_LOG_INFO, "RE5Native", buf);
    }
    return NULL;
}
#endif

void re5_log_init(void)
{
#ifdef __ANDROID__
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);
    if (pipe(s_pfd) == 0) {
        dup2(s_pfd[1], 1);
        dup2(s_pfd[1], 2);
        pthread_create(&s_log_thread, NULL, android_log_pipe_thread, NULL);
        pthread_detach(s_log_thread);
    }
#endif

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
#ifdef __ANDROID__
    static const int k_android_prio[] = {
        ANDROID_LOG_ERROR,
        ANDROID_LOG_WARN,
        ANDROID_LOG_INFO,
        ANDROID_LOG_DEBUG,
        ANDROID_LOG_VERBOSE
    };
    int prio = (level >= 0 && level <= RE5_LOG_TRACE) ? k_android_prio[level] : ANDROID_LOG_INFO;
    __android_log_print(prio, "RE5Native", "%s", line);
#endif
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
