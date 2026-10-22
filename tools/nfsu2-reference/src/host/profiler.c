/*
 * profiler.c - sampling profiler for the game thread (NFSU2_PROFILE=1).
 *
 * A host thread suspends the game thread ~1000 times a second, reads its EIP,
 * and counts it. Every 10 s (and at exit) the counts are resolved to symbols
 * with DbgHelp -- lifted functions are sub_XXXXXXXX in the PDB, so the report
 * names guest functions directly -- and written to logs/profile_<stamp>.txt.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "nfs_log.h"
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

#define NBUCKET (1 << 20)
static uint32_t *g_pc;          /* sampled EIPs, ring */
static volatile LONG g_npc;
static HANDLE g_target;

typedef struct { char name[96]; unsigned n; } entry;
static int cmp(const void *a, const void *b) { return (int)((const entry *)b)->n - (int)((const entry *)a)->n; }

static void report(void) {
    LONG n = g_npc < NBUCKET ? g_npc : NBUCKET;
    if (!n) return;
    HANDLE proc = GetCurrentProcess();
    static int inited;
    if (!inited) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, NULL, TRUE); inited = 1; }
    entry *e = calloc(8192, sizeof *e);
    int ne = 0;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    for (LONG i = 0; i < n; i++) {
        char name[96];
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, g_pc[i], &disp, si)) _snprintf(name, sizeof name - 1, "%s", si->Name);
        else {
            HMODULE m = NULL; char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)g_pc[i], &m)) GetModuleFileNameA(m, path, MAX_PATH);
            const char *b = strrchr(path, '\\');
            _snprintf(name, sizeof name - 1, "[%s]", b ? b + 1 : path);
        }
        name[95] = 0;
        int k;
        for (k = 0; k < ne; k++) if (!strcmp(e[k].name, name)) break;
        if (k == ne) { if (ne == 8192) continue; strcpy(e[ne].name, name); e[ne].n = 0; ne++; }
        e[k].n++;
    }
    qsort(e, ne, sizeof *e, cmp);
    char path[700];
    snprintf(path, sizeof path, "%s/profile_%s.txt", nfs_log_dir(), nfs_log_stamp());
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "game-thread samples: %ld\n", n);
        for (int k = 0; k < ne && k < 80; k++) fprintf(f, "%6.2f%%  %7u  %s\n", 100.0 * e[k].n / n, e[k].n, e[k].name);
        fclose(f);
    }
    NFS_LOG(CPU, "profile: %ld samples, top %s %.1f%% -> %s", n, ne ? e[0].name : "-", ne ? 100.0 * e[0].n / n : 0.0, path);
    free(e);
}

static DWORD WINAPI sampler(void *p) {
    (void)p;
    timeBeginPeriod(1);
    DWORD last = GetTickCount();
    for (;;) {
        Sleep(1);
        if (SuspendThread(g_target) == (DWORD)-1) break;
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_target, &c)) {
            LONG i = InterlockedIncrement(&g_npc) - 1;
            if (i < NBUCKET) g_pc[i] = c.Eip;
        }
        ResumeThread(g_target);
        if (GetTickCount() - last > 10000) { last = GetTickCount(); report(); }
    }
    return 0;
}

void nfs_profiler_start(HANDLE game_thread) {
    const char *e = getenv("NFSU2_PROFILE");
    if (!e || *e != '1') return;
    g_pc = calloc(NBUCKET, sizeof *g_pc);
    DuplicateHandle(GetCurrentProcess(), game_thread, GetCurrentProcess(), &g_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    NFS_LOG(CPU, "profiler on (NFSU2_PROFILE=1)");
}
