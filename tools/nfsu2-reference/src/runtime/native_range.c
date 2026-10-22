/*
 * native_range.c - hybrid bisection: run a range of guest functions as the
 * ORIGINAL machine code, the rest lifted.
 *
 *   NFSU2_NATIVE_RANGE=0x5D0000-0x5F0000   (hex VAs, [lo, hi))
 *
 * The range's pages are made executable. A lifted function whose VA is in the
 * range diverts at RECOMP_ENTER to recomp_run_native, which runs the original
 * code on the guest stack (pcrecomp hybrid_call_machine). When that native code
 * calls a function OUTSIDE the range, the page is still non-executable, the
 * fetch faults, and native32's vectored handler runs the lifted body instead.
 * So exactly the range is swapped. If the symptom disappears, the faulty
 * function is in the range; halve and repeat.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "hybrid.h"
#include "nfs_log.h"

uint32_t g_native_lo, g_native_hi;

void recomp_run_native(uint32_t va) {
    hybrid_regs r = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    /* the lifted x87 stack onto the real one, deepest first, so st(i) match */
    double st[8];
    int depth = g_fp_top < 0 ? 0 : g_fp_top > 8 ? 8 : g_fp_top;
    for (int i = 0; i < depth; i++) st[i] = g_st[i];
    hybrid_fpu_clear();
    if (depth) hybrid_fpu_push(st, depth);
    uint16_t cw = g_fpu_cw;
    __asm fldcw cw
    /* Callbacks from the native code (shims, lifted callees) run on the guest
     * stack too: move the global esp well below the native frames. */
    uint32_t saved = g_esp;
    g_esp -= 0x80000;
    hybrid_call_machine(&r, va);
    (void)saved;
    __asm fnstcw cw
    g_fpu_cw = cw;
    int nd = hybrid_fpu_depth();
    for (int i = 0; i < nd && i < 8; i++) g_st[i] = hybrid_fpu_pop();
    for (int i = nd; i < 8; i++) g_st[i] = 0.0;
    g_fp_top = nd;
    g_eax = r.eax; g_edx = r.edx; g_esp = r.esp;
    g_flag_k = FK_NONE;
}

void nfs_native_range_init(void) {
    const char *e = getenv("NFSU2_NATIVE_RANGE");
    if (!e || !*e) { g_native_lo = g_native_hi = 0; return; }
    char *end;
    g_native_lo = strtoul(e, &end, 16);
    g_native_hi = *end == '-' ? strtoul(end + 1, NULL, 16) : 0;
    if (g_native_hi <= g_native_lo) { g_native_lo = g_native_hi = 0; return; }
    DWORD old;
    VirtualProtect((void *)(uintptr_t)g_native_lo, g_native_hi - g_native_lo, PAGE_EXECUTE_READWRITE, &old);
    NFS_LOG(BOOT, "NFSU2_NATIVE_RANGE: guest code 0x%08X-0x%08X runs as the ORIGINAL machine code", g_native_lo, g_native_hi);
}
