/*
 * cpu.h - runtime for the CPU-struct x86-32 recompilation model
 *         (paired with tools/lift/lift32_cpu.py).
 *
 * Lifted x86 functions operate on an explicit CPU state struct, so several
 * machine states can be live at once - which is what a hybrid build needs when
 * real library code calls back into lifted code while an outer lifted call is
 * still on the stack. See runtime/hybrid/.
 *
 * The recomp runs as a 32-bit program so register values are real 32-bit host
 * pointers: the image's data sections live at their mapped VAs and heap comes
 * from malloc - both inside the 32-bit address space - so memory operands
 * dereference directly.
 *
 * Flags are computed eagerly by helpers after arithmetic/logic ops; Jcc reads
 * the stored bits. This is verbose but mechanical and easy to verify against
 * the hardware. (A lazy-flags optimization can come later.)
 */
#ifndef PCRECOMP_CPU_H
#define PCRECOMP_CPU_H

#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>   /* __readfsdword/__writefsdword: TIB-relative (fs:) access for SEH */
#if defined(__clang__) && defined(_M_IX86)
/* clang-cl has __readfsdword but not __writefsdword on x86: the call links as
 * an undefined ___writefsdword. MSVC has both. */
static __inline void pcrecomp_writefsdword(unsigned long off, unsigned long v)
{
    __asm__ volatile("movl %1, %%fs:(%0)" : : "r"(off), "r"(v) : "memory");
}
#define __writefsdword pcrecomp_writefsdword
#endif
#endif

/* ---- SSE register file ----
 *
 * One storage, several views - which is how the silicon works, and why this is
 * a union rather than eight float[4]. `movaps` moves a bit pattern, `pxor`
 * works on integers and `addss` works on floats, all through the same 128
 * bits; a float-only model corrupts the first two the moment a game stores a
 * mask in an XMM register, which every SSE-era game does.
 *
 * Scalar single precision is `float` here and NOT `double` like the x87 stack
 * below it. That difference is deliberate. An x87 register really is wider
 * than the values it holds, so widening it costs nothing - but SSE single
 * genuinely IS 32-bit, and computing `addss` in double rounds once where the
 * hardware rounds twice. The results differ in the low bits, and in a game
 * that feeds them to a coordinate transform the difference is visible. */
#if defined(__GNUC__) || defined(__clang__)
typedef float    xv4f  __attribute__((vector_size(16)));
typedef double   xv2d  __attribute__((vector_size(16)));
typedef uint8_t  xv16b __attribute__((vector_size(16)));
typedef uint16_t xv8w  __attribute__((vector_size(16)));
typedef uint32_t xv4d  __attribute__((vector_size(16)));
typedef uint64_t xv2q  __attribute__((vector_size(16)));
typedef int32_t  xv4i  __attribute__((vector_size(16)));
typedef int16_t  xv8i  __attribute__((vector_size(16)));
typedef int8_t   xv16i __attribute__((vector_size(16)));
typedef int64_t  xv2qi __attribute__((vector_size(16)));

typedef xv4f XMM;
#else
typedef union {
    float    f32[4];
    double   f64[2];
    uint32_t u32[4];
    uint64_t u64[2];
    int32_t  i32[4];
    uint16_t u16[8];
    int16_t  i16[8];
    uint8_t  u8[16];
    int8_t   i8[16];
    int64_t  i64[2];
} XMM;
#endif

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    /* flags as discrete bits (0/1) */
    uint32_t cf, zf, sf, of, pf, af;
    /* x87: register-stack of doubles. st0 == st[fpu_top]; push pre-decrements. */
    double   st[8];
    int      fpu_top;
    /* SSE: xmm0..xmm7. A 32-bit target has no xmm8-15. */
    XMM      xmm[8];
    uint32_t fpu_sw;   /* status word: only C0/C1/C2/C3 condition bits modelled */
    /* Segment registers: stored, but NOT used in address computation. The
     * memory model below is flat - a register holds a real 32-bit address - so
     * `mov ds, ax` records the selector and does nothing else. That is right
     * for code that only saves and restores them, which is the common case even
     * in segmented 32-bit code, and wrong for code that switches DS to reach
     * another segment's data. The latter needs a selector -> base mapping the
     * runtime does not have; such code should be caught rather than lifted
     * silently. */
    uint16_t cs, ds, es, fs, gs, ss;
    /* Linear base of the TIB that `fs:` points at. Used only by the portable
     * fs: path (FS_* macros below): the host points this at a simulated TIB
     * per thread, and fs:[off] reads ordinary memory at fs_base+off. Left 0 and
     * unused on the MSVC default path, which reads the real segment. */
    uint32_t fs_base;
    /* x87 control word. Only the rounding-control field (bits 10-11) changes
     * what the lifted code computes: fistp and frndint round by it. MSVC's
     * float-to-int idiom is `fnstcw; or 0xC00; fldcw; fistp; fldcw` (truncate),
     * and inline floor/ceil use RC=01/10 the same way; with the field ignored
     * every one of those rounded to nearest instead. The host sets it to the
     * Windows default, 0x027F, when it creates a thread (a zeroed CPU still
     * rounds to nearest, RC=00). */
    uint32_t fpu_cw;
    /* MXCSR as the guest last loaded it. FTZ (bit 15) and DAZ (bit 6) are also
     * applied to the host thread's own SSE state (cpu_ldmxcsr), because the
     * lifted SSE arithmetic runs on it; the rounding bits are only stored. */
    uint32_t mxcsr;
    /* Calls made straight to another lifted function (no dispatch lookup):
     * the host counts them here, per thread and without TLS, to keep its
     * periodic scheduling point (re5: RE5_CALL in cpu_epilogue.h). */
    uint32_t ncalls;
    /* Direction flag (EFLAGS bit 10): std/cld set it, and every string
     * instruction steps ESI/EDI by -size while it is 1 (STR_STEP). MSVC's CRT
     * relies on it: strrchr scans back with `std; repne scasb`, and memmove
     * copies an overlapping buffer downwards with `std; rep movsd`. Modelling
     * it as always-forward made strrchr return NULL and memmove replicate. */
    uint32_t df;
} CPU;

/* ESI/EDI step of one string element: +size, or -size with DF=1. */
#define STR_STEP(c, n) ((c)->df ? (uint32_t)0u - (uint32_t)(n) : (uint32_t)(n))

/* ---- partial register access (preserve unaffected bits, like x86) ---- */
#define R8L(r)        ((uint8_t)(r))
#define R8H(r)        ((uint8_t)((r) >> 8))
#define R16(r)        ((uint16_t)(r))
#define SET8L(r, v)   ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET8H(r, v)   ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET16(r, v)   ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

/* ---- memory access: register holds a real 32-bit address ---- */
static inline uint8_t  rd8 (uint32_t a) { return *(uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16(uint32_t a) { return *(uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32(uint32_t a) { return *(uint32_t *)(uintptr_t)a; }
static inline void wr8 (uint32_t a, uint8_t  v) { *(uint8_t  *)(uintptr_t)a = v; }
static inline void wr16(uint32_t a, uint16_t v) { *(uint16_t *)(uintptr_t)a = v; }
static inline void wr32(uint32_t a, uint32_t v) { *(uint32_t *)(uintptr_t)a = v; }

/* ---- fs:-relative access (TIB / SEH chain) ----
 *
 * The lifter emits these for every `fs:` memory operand (SEH prologues store
 * the exception-registration record through fs:[0]). Two implementations, one
 * chosen at compile time so a single lift builds for either host:
 *
 *   MSVC default        the real segment, via the __readfsN/__writefsN
 *                       intrinsics. Correct when the host is a 1:1 Win32
 *                       process whose own TIB the guest shares.
 *   portable / non-MSVC read ordinary memory at c->fs_base + off. The host
 *                       gives each guest thread a simulated TIB and points
 *                       c->fs_base at it. This is what a non-Windows (Linux)
 *                       host needs; it also works on Windows if the host sets
 *                       c->fs_base to NtCurrentTeb() and defines
 *                       RECOMP_FS_SIMULATED.
 */
#if defined(_MSC_VER) && !defined(RECOMP_FS_SIMULATED)
#define FS_RD8(c, off)      __readfsbyte(off)
#define FS_RD16(c, off)     __readfsword(off)
#define FS_RD32(c, off)     __readfsdword(off)
#define FS_WR8(c, off, v)   __writefsbyte((off), (unsigned char)(v))
#define FS_WR16(c, off, v)  __writefsword((off), (unsigned short)(v))
#define FS_WR32(c, off, v)  __writefsdword((off), (unsigned long)(v))
#else
#define FS_RD8(c, off)      rd8 ((c)->fs_base + (off))
#define FS_RD16(c, off)     rd16((c)->fs_base + (off))
#define FS_RD32(c, off)     rd32((c)->fs_base + (off))
#define FS_WR8(c, off, v)   wr8 ((c)->fs_base + (off), (uint8_t )(v))
#define FS_WR16(c, off, v)  wr16((c)->fs_base + (off), (uint16_t)(v))
#define FS_WR32(c, off, v)  wr32((c)->fs_base + (off), (uint32_t)(v))
#endif
/* movlps/movhps and friends move a lane as bits, not as a number: a double
 * round-trip would be exact for every value except the signalling NaNs a
 * mask-producing compare leaves behind. memcpy, because the address need
 * not be aligned. */
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static inline void wr64(uint32_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

/* ---- EFLAGS pack/unpack (modelled bits only) ---- */
/* `lock`-prefixed read-modify-write.
 *
 * A recompiled game with a worker pool needs these to be real: the guest uses
 * `lock xadd` for every reference count and every queue index, and a
 * non-atomic one is a use-after-free an hour into a session rather than a
 * crash you can find. Nothing else in this header cares about other threads;
 * these do.
 *
 * The x86 semantics are "return the old value" for xadd, and for cmpxchg
 * "return what was there, and the caller compares" - the ZF the instruction
 * sets follows from that. */
#if defined(_MSC_VER)
static inline uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)(uintptr_t)a, (long)v);
}
static inline uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)(uintptr_t)a,
                                                 (long)nv, (long)cmp);
}
static inline uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchange((volatile long *)(uintptr_t)a, (long)v);
}
#else
static inline uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_add((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    return __sync_val_compare_and_swap((volatile uint32_t *)(uintptr_t)a, cmp, nv);
}
static inline uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    return __sync_lock_test_and_set((volatile uint32_t *)(uintptr_t)a, v);
}
#endif

static inline uint32_t eflags_pack(CPU *c) {
    return 0x202u | (c->cf) | (c->pf << 2) | (c->af << 4) |
           (c->zf << 6) | (c->sf << 7) | (c->df << 10) | (c->of << 11);
}
static inline void eflags_unpack(CPU *c, uint32_t v) {
    c->cf = v & 1; c->pf = (v >> 2) & 1; c->af = (v >> 4) & 1;
    c->zf = (v >> 6) & 1; c->sf = (v >> 7) & 1; c->df = (v >> 10) & 1; c->of = (v >> 11) & 1;
}

/* ---- stack ----
 *
 * ESP is an address in a flat build, so the base is zero and these compile to
 * exactly what they did before. A segmented build defines STACK_BASE to its
 * SS base instead, because there ESP is an OFFSET: 16-bit code reaching its
 * arguments does `mov bp, sp` and then `[bp+6]`, and truncating a host address
 * to 16 bits produces a pointer to nothing. */
#ifndef STACK_BASE
#define STACK_BASE(c) 0u
#endif
static inline void push32(CPU *c, uint32_t v) { c->esp -= 4; wr32(STACK_BASE(c) + c->esp, v); }
static inline uint32_t pop32(CPU *c) { uint32_t v = rd32(STACK_BASE(c) + c->esp); c->esp += 4; return v; }
/* The same on a cached ESP (lift32_cpu keeps the GPRs in locals, see cache_registers). */
static inline void push32_r(CPU *c, uint32_t *sp, uint32_t v) { (void)c; *sp -= 4; wr32(STACK_BASE(c) + *sp, v); }
static inline uint32_t pop32_r(CPU *c, uint32_t *sp) { (void)c; uint32_t v = rd32(STACK_BASE(c) + *sp); *sp += 4; return v; }

/* An instruction the lifter could not express. Plain abort() by default, so a
 * project that includes only cpu.h is unchanged; a runtime that can say
 * something useful - which guest address, which mnemonic, how it got there -
 * defines RECOMP_TODO before including this. A bare abort() in two million
 * lines becomes __fastfail in a release build, which no handler sees. */
#ifndef RECOMP_TODO
#define RECOMP_TODO(va, text) abort()
#endif

/* ---- absolute image references: abs VA at preferred base -> live address ---- */
extern uint32_t g_image_delta;   /* live_base - PE ImageBase (0 if loaded where it wanted) */
#define GVA(abs) ((uint32_t)((abs) + g_image_delta))

/* ---- flag helpers ---- */
static inline uint32_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1;
}

/* logical ops (AND/OR/XOR/TEST): CF=OF=0 */
static inline void flags_logic32(CPU *c, uint32_t r) {
    c->cf = 0; c->of = 0; c->sf = r >> 31; c->zf = (r == 0); c->pf = parity8((uint8_t)r);
}
static inline void flags_logic8(CPU *c, uint8_t r) {
    c->cf = 0; c->of = 0; c->sf = (r >> 7) & 1; c->zf = (r == 0); c->pf = parity8(r);
}
/* cmp/sub (a - b) */
static inline uint32_t flags_sub32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->cf = (a < b);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = (((a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* add (a + b) */
static inline uint32_t flags_add32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->cf = (r < a);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* inc/dec: like add/sub by 1 but CF preserved */
static inline uint32_t flags_inc32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_add32(c, a, 1); c->cf = keepcf; return r;
}
static inline uint32_t flags_dec32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_sub32(c, a, 1); c->cf = keepcf; return r;
}

/* ---- width-generic flag helpers (sz in {1,2,4}); used by generated code ---- */
static inline uint32_t mask_sz(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline uint32_t sign_sz(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }

static inline uint32_t flags_sub(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a - b) & m;
    c->cf = (a < b); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_add(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a + b) & m;
    c->cf = (r < a); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_logicz(CPU *c, uint32_t r, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); r &= m;
    c->cf = 0; c->of = 0; c->sf = (r & s) != 0; c->zf = (r == 0);
    c->pf = parity8((uint8_t)r); return r;
}
/* Masked forms: the lifter's flag liveness (lift32_cpu: lift_function) passes the
 * flags something can still read, as EFLAGS bits (CF 0x1, PF 0x4, AF 0x10, ZF 0x40,
 * SF 0x80, OF 0x800), and only those are computed. `lm` is a constant at every call
 * site, so the compiler drops the rest; without it every add/sub/cmp/test spent ~20
 * host instructions on six flags that the next instruction overwrote. */
#define RF_CF 0x001u
#define RF_PF 0x004u
#define RF_AF 0x010u
#define RF_ZF 0x040u
#define RF_SF 0x080u
#define RF_OF 0x800u
static inline uint32_t flags_sub_m(CPU *c, uint32_t lm, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a - b) & m;
    if (lm & RF_CF) c->cf = (a < b);
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & s) != 0;
    if (lm & RF_OF) c->of = (((a ^ b) & (a ^ r)) & s) != 0;
    if (lm & RF_AF) c->af = ((a ^ b ^ r) & 0x10) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t flags_add_m(CPU *c, uint32_t lm, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a + b) & m;
    if (lm & RF_CF) c->cf = (r < a);
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & s) != 0;
    if (lm & RF_OF) c->of = ((~(a ^ b) & (a ^ r)) & s) != 0;
    if (lm & RF_AF) c->af = ((a ^ b ^ r) & 0x10) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t flags_logicz_m(CPU *c, uint32_t lm, uint32_t r, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); r &= m;
    if (lm & RF_CF) c->cf = 0;
    if (lm & RF_OF) c->of = 0;
    if (lm & RF_SF) c->sf = (r & s) != 0;
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t flags_adc_m(CPU *c, uint32_t lm, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), cin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a + b + cin; uint32_t r = (uint32_t)full & m;
    if (lm & RF_CF) c->cf = (full >> (sz * 8)) & 1;
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & s) != 0;
    if (lm & RF_OF) c->of = ((~(a ^ b) & (a ^ r)) & s) != 0;
    if (lm & RF_AF) c->af = ((a ^ b ^ r) & 0x10) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t flags_sbb_m(CPU *c, uint32_t lm, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), bin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a - b - bin; uint32_t r = (uint32_t)full & m;
    if (lm & RF_CF) c->cf = (full >> (sz * 8)) & 1;
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & s) != 0;
    if (lm & RF_OF) c->of = (((a ^ b) & (a ^ r)) & s) != 0;
    if (lm & RF_AF) c->af = ((a ^ b ^ r) & 0x10) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t flags_incs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_add(c, a, 1, sz); c->cf = keep; return r;
}
static inline uint32_t flags_decs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_sub(c, a, 1, sz); c->cf = keep; return r;
}
static inline uint32_t flags_adc(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), cin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a + b + cin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t flags_sbb(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), bin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a - b - bin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}

static inline uint32_t op_shl(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); cnt &= 31; if (!cnt) return v & m;
    /* In 64 bits: a count past the width of an 8/16-bit operand would shift by a
     * negative amount in C (difftest_cpu.py). CF is the last bit out, as on hw. */
    c->cf = (uint32_t)((((uint64_t)(v & m)) << cnt) >> (sz * 8)) & 1; uint32_t r = (v << cnt) & m;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t op_shr(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1; uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r); return r;
}
static inline uint32_t op_sar(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    /* Sign-extend to 32 bits and shift there: an 8/16-bit operand with a count at
     * or past its width fills with the sign, and CF is the sign too - the old
     * form shifted by a negative amount and gave 0 (difftest_cpu.py). */
    int32_t sv = (v & s) ? (int32_t)(v | ~m) : (int32_t)v;
    c->cf = (uint32_t)(sv >> (cnt - 1)) & 1; uint32_t r = (uint32_t)(sv >> cnt) & m;
    c->zf = (r == 0); c->sf = (r & s) != 0; c->pf = parity8((uint8_t)r); return r;
}

/* Masked shifts (see flags_sub_m). A zero count still leaves every flag alone. */
static inline uint32_t op_shl_m(CPU *c, uint32_t lm, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); cnt &= 31; if (!cnt) return v & m;
    uint32_t r = (v << cnt) & m;
    if (lm & RF_CF) c->cf = (uint32_t)((((uint64_t)(v & m)) << cnt) >> (sz * 8)) & 1;
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & sign_sz(sz)) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t op_shr_m(CPU *c, uint32_t lm, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    uint32_t r = v >> cnt;
    if (lm & RF_CF) c->cf = (v >> (cnt - 1)) & 1;
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & sign_sz(sz)) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t op_sar_m(CPU *c, uint32_t lm, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    int32_t sv = (v & s) ? (int32_t)(v | ~m) : (int32_t)v;
    uint32_t r = (uint32_t)(sv >> cnt) & m;
    if (lm & RF_CF) c->cf = (uint32_t)(sv >> (cnt - 1)) & 1;
    if (lm & RF_ZF) c->zf = (r == 0);
    if (lm & RF_SF) c->sf = (r & s) != 0;
    if (lm & RF_PF) c->pf = parity8((uint8_t)r);
    return r;
}

/* ---- rotates. Unlike the shifts these leave ZF/SF/PF alone, which is not an
   omission: x86 does not touch them here, and code that rotates then branches
   on the zero flag is reading the flag the PREVIOUS instruction set. ---- */
static inline uint32_t op_rol(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    /* A masked count that is a multiple of the width rotates nothing but still
     * sets CF from the result (difftest_cpu.py: rol r/m8, cl with cl = 8). */
    cnt &= 31; if (!cnt) return v;
    cnt %= (uint32_t)w;
    uint32_t r = cnt ? ((v << cnt) | (v >> (w - cnt))) & m : v;
    c->cf = r & 1;
    c->of = (((r >> (w - 1)) & 1) ^ (c->cf & 1));
    return r;
}
static inline uint32_t op_ror(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; if (!cnt) return v;   /* multiple of the width: CF still set (see op_rol) */
    cnt %= (uint32_t)w;
    uint32_t r = cnt ? ((v >> cnt) | (v << (w - cnt))) & m : v;
    c->cf = (r >> (w - 1)) & 1;
    c->of = (((r >> (w - 1)) & 1) ^ ((r >> (w - 2)) & 1));
    return r;
}

/* ---- byte order ----
 *
 * bswap reverses the four bytes of a 32-bit register and touches no flags.
 * It is how a compiler writes htonl/ntohl inline, so it turns up in any code
 * that opens a socket, next to a `ror bx, 8` doing the 16-bit half.
 *
 * The 16-bit form is architecturally undefined - Intel documents it as such
 * and real parts leave the register zeroed - so a compiler never emits it and
 * this deliberately does not pretend to implement it. */
static inline uint32_t op_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u)
         | ((v << 8) & 0x00FF0000u) | (v << 24);
}

/* ---- double-precision shifts: shift `d`, feeding in bits from `s` ---- */
static inline uint32_t op_shld(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;      /* count > width is undefined */
    uint32_t r = ((d << cnt) | (s >> (w - cnt))) & m;
    c->cf = (d >> (w - cnt)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    return r;
}
/* BSF/BSR: the index of the lowest (or highest) set bit. The flag is the
 * whole interface - ZF says whether there WAS one - and when the source is
 * zero the destination is architecturally undefined, so it is left alone
 * rather than given a value that code might come to depend on. */
static inline uint32_t op_bsf(CPU *c, uint32_t d, uint32_t s, int sz) {
    uint32_t i, n = (uint32_t)sz * 8;
    c->zf = (s == 0);
    if (s == 0) return d;
    for (i = 0; i < n; i++) if (s & (1u << i)) return i;
    return d;
}

static inline uint32_t op_bsr(CPU *c, uint32_t d, uint32_t s, int sz) {
    uint32_t i, n = (uint32_t)sz * 8;
    c->zf = (s == 0);
    if (s == 0) return d;
    for (i = n; i-- > 0; ) if (s & (1u << i)) return i;
    return d;
}

/* BT/BTS/BTR/BTC: the selected bit into CF, then the register form's own
 * change to it. The count is taken modulo the operand width - which is what
 * makes `bt eax, 33` test bit 1 and not read past the register. Only the
 * register form is expressed here; the memory form of these addresses a bit
 * string that can run past the operand, and no lifted site uses it. */
static inline uint32_t op_bittest(CPU *c, uint32_t d, uint32_t bit, int sz, int op) {
    uint32_t mask = 1u << (bit & ((uint32_t)sz * 8 - 1));
    c->cf = (d & mask) ? 1 : 0;
    switch (op) {
        case 1: return d | mask;      /* bts */
        case 2: return d & ~mask;     /* btr */
        case 3: return d ^ mask;      /* btc */
        default: return d;            /* bt  */
    }
}

/* RCL/RCR rotate through CF, so the rotated quantity is one bit wider than
 * the operand and the count is taken modulo that wider width. Writing this as
 * a bit at a time is slower than the closed form and is the version that is
 * obviously right, which for an instruction this rare is the better trade. */
static inline uint32_t op_rcl(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8, i;
    uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    cnt = (cnt & 31u) % (n + 1);   /* masked to 5 bits first, as the hw does (difftest_cpu.py) */
    if (cnt == 0) return d;        /* a zero count changes no flag at all */
    for (i = 0; i < cnt; i++) {
        uint32_t top = (d >> (n - 1)) & 1u;
        d = ((d << 1) | c->cf) & mask;
        c->cf = top;
    }
    /* OF is defined only for a count of one; hardware leaves something there
     * for the rest and nothing may read it. */
    if (cnt == 1) c->of = (uint32_t)(((d >> (n - 1)) & 1u) ^ c->cf);
    return d;
}

static inline uint32_t op_rcr(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8, i;
    uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    cnt = (cnt & 31u) % (n + 1);   /* masked to 5 bits first, as the hw does */
    if (cnt == 0) return d;
    /* RCR sets OF from the two top bits of the RESULT, and before the rotate
     * rather than after - but only the count-of-one case is defined, and for
     * that one the two are the same thing. */
    if (cnt == 1) c->of = (uint32_t)((((d >> 1) | (c->cf << (n - 1))) >> (n - 1)) & 1u)
                        ^ (uint32_t)((((d >> 1) | (c->cf << (n - 1))) >> (n - 2)) & 1u);
    for (i = 0; i < cnt; i++) {
        uint32_t bot = d & 1u;
        d = ((d >> 1) | (c->cf << (n - 1))) & mask;
        c->cf = bot;
    }
    return d;
}

static inline uint32_t op_shrd(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;
    uint32_t r = ((d >> cnt) | (s << (w - cnt))) & m;
    c->cf = (d >> (cnt - 1)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    return r;
}

/* ---- shifts (set flags like x86; count masked to 5 bits) ---- */
static inline uint32_t shr32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1;
    uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint32_t shl32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (32 - cnt)) & 1;
    uint32_t r = v << cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
static inline uint8_t shr8(CPU *c, uint8_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    if (cnt <= 8) c->cf = (v >> (cnt - 1)) & 1;
    uint8_t r = (cnt < 8) ? (uint8_t)(v >> cnt) : 0;
    c->zf = (r == 0); c->sf = (r >> 7) & 1; c->pf = parity8(r);
    return r;
}

/* ---- x87 FPU ---- */
#include <math.h>
static inline void   fpush(CPU *c, double v) { c->fpu_top = (c->fpu_top - 1) & 7; c->st[c->fpu_top] = v; }
static inline double fpop(CPU *c) { double v = c->st[c->fpu_top]; c->fpu_top = (c->fpu_top + 1) & 7; return v; }
static inline double *fst(CPU *c, int i) { return &c->st[(c->fpu_top + i) & 7]; }

/* float/double/int memory operands (memcpy avoids alignment/aliasing issues) */
static inline double rdf32(uint32_t a) { float  f; memcpy(&f, (void *)(uintptr_t)a, 4); return (double)f; }
static inline double rdf64(uint32_t a) { double d; memcpy(&d, (void *)(uintptr_t)a, 8); return d; }
static inline double rdi16(uint32_t a) { int16_t i; memcpy(&i, (void *)(uintptr_t)a, 2); return (double)i; }
static inline double rdi32(uint32_t a) { int32_t i; memcpy(&i, (void *)(uintptr_t)a, 4); return (double)i; }
static inline double rdi64(uint32_t a) { int64_t i; memcpy(&i, (void *)(uintptr_t)a, 8); return (double)i; }
static inline void   wrf32(uint32_t a, double v) { float  f = (float)v; memcpy((void *)(uintptr_t)a, &f, 4); }
static inline void   wrf64(uint32_t a, double v) { memcpy((void *)(uintptr_t)a, &v, 8); }
/* rep stos / rep movs: one bulk host operation with the result the element
 * loop would give. With DF=1 the elements go from EDI/ESI downwards, so the
 * block is the n elements ending at (and including) the start address. A copy
 * that walks into its own source replicates elements, which memmove would
 * not: forward when the destination starts inside the source, backward when
 * the source starts inside the destination. Those cases go element by element. */
static inline void *rep_ptr(uint32_t a) { return (void *)(uintptr_t)a; }
static inline void rep_stos(CPU *c, uint32_t esz) {
    uint32_t n = c->ecx, v = c->eax;
    if (!n) return;
    uint32_t bytes = n * esz;
    uint32_t d = c->df ? c->edi - (bytes - esz) : c->edi;
    if (esz == 1 || (esz == 2 && (v & 0xFFu) == ((v >> 8) & 0xFFu)) || (esz == 4 && v == (v & 0xFFu) * 0x01010101u)) {
        memset(rep_ptr(d), (int)(v & 0xFFu), bytes);
    } else {
        uint8_t *p = (uint8_t *)rep_ptr(d);
        for (uint32_t i = 0; i < bytes; i += esz) memcpy(p + i, &v, esz);
    }
    c->edi = c->df ? c->edi - bytes : c->edi + bytes;
    c->ecx = 0;
}
static inline void rep_movs(CPU *c, uint32_t esz) {
    uint32_t n = c->ecx;
    if (!n) return;
    uint32_t bytes = n * esz;
    if (!c->df) {
        uint32_t d = c->edi, s = c->esi;
        if (d - s >= bytes) {   /* d below s, or past the end of the source */
            memmove(rep_ptr(d), rep_ptr(s), bytes);
        } else {
            uint8_t *pd = (uint8_t *)rep_ptr(d);
            const uint8_t *ps = (const uint8_t *)rep_ptr(s);
            for (uint32_t i = 0; i < bytes; i += esz) memmove(pd + i, ps + i, esz);
        }
        c->edi = d + bytes;
        c->esi = s + bytes;
    } else {
        uint32_t d = c->edi - (bytes - esz), s = c->esi - (bytes - esz);   /* lowest element */
        if (s - d >= bytes) {   /* s below d, or past the end of the destination */
            memmove(rep_ptr(d), rep_ptr(s), bytes);
        } else {
            uint8_t *pd = (uint8_t *)rep_ptr(d);
            const uint8_t *ps = (const uint8_t *)rep_ptr(s);
            for (uint32_t i = bytes; i > 0; i -= esz) memmove(pd + i - esz, ps + i - esz, esz);
        }
        c->edi -= bytes;
        c->esi -= bytes;
    }
    c->ecx = 0;
}

/* Round st(0) the way the control word says (fistp, frndint). nearbyint is
 * the host's current mode, which the host leaves at round-to-nearest. */
static inline double fpu_rint(const CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3u) {
    case 1:  return floor(v);
    case 2:  return ceil(v);
    case 3:  return trunc(v);
    default: return nearbyint(v);
    }
}
/* fscale: st(0) * 2^trunc(st(1)). The exponent is clamped before the cast -
 * (int)1e300 is undefined in C and gave 0 where the x87 gives infinity
 * (scripts/difftest_cpu.py); a NaN exponent gives NaN. */
static inline double fpu_fscale(double v, double e) {
    if (e != e) return e;
    e = trunc(e);
    if (e > 100000.0) e = 100000.0;
    if (e < -100000.0) e = -100000.0;
    return ldexp(v, (int)e);
}
/* fist/fistp store an already-rounded value (fpu_rint). Out of range or NaN
 * stores the "integer indefinite" (only the sign bit), as the x87 does with
 * the invalid-operation exception masked - C would call the cast undefined. */
static inline void   wri32(uint32_t a, double v) { v = nearbyint(v); int32_t i = (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : INT32_MIN; memcpy((void *)(uintptr_t)a, &i, 4); }
static inline void   wri16(uint32_t a, double v) { v = nearbyint(v); int16_t i = (v >= -32768.0 && v < 32768.0) ? (int16_t)v : INT16_MIN; memcpy((void *)(uintptr_t)a, &i, 2); }
static inline void   wri64(uint32_t a, double v) { v = nearbyint(v); int64_t i = (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (int64_t)v : INT64_MIN; memcpy((void *)(uintptr_t)a, &i, 8); }

/* ldmxcsr. The value is kept for stmxcsr; FTZ and DAZ go to the host thread
 * too, since lifted SSE arithmetic is host SSE (NEON on ARM64) arithmetic, and
 * code that asks for flush-to-zero (audio filters, reverb tails) otherwise
 * computes on denormals, which is slow on most hosts. The rounding field is
 * not applied: the host's own code assumes round-to-nearest. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <emmintrin.h>
static inline void cpu_ldmxcsr(CPU *c, uint32_t v) {
    c->mxcsr = v;
    _mm_setcsr((_mm_getcsr() & ~0x8040u) | (v & 0x8040u));
}
#elif defined(__aarch64__)
static inline void cpu_ldmxcsr(CPU *c, uint32_t v) {
    uint64_t fpcr;
    c->mxcsr = v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (v & 0x8040u) ? (fpcr | (1ull << 24)) : (fpcr & ~(1ull << 24));   /* FZ */
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
}
#else
static inline void cpu_ldmxcsr(CPU *c, uint32_t v) { c->mxcsr = v; }
#endif


/* fcom/fcomp: set C3/C2/C0 (st0 vs v). Cleared C1. */
static inline void fcompare(CPU *c, double a, double b) {
    uint32_t sw = c->fpu_sw & ~0x4700u;
    if      (a > b)  { /* 000 */ }
    else if (a < b)  sw |= 0x0100u;          /* C0 */
    else if (a == b) sw |= 0x4000u;          /* C3 */
    else             sw |= 0x4700u;          /* unordered: C3|C2|C0 */
    c->fpu_sw = sw;
}
/* fcomi/fucomi: the same comparison, into EFLAGS instead of the status word.
 * The P6 forms exist so a compiler can branch on a float compare without
 * going through `fnstsw ax; test ah`. Note the mapping is not the integer
 * one: "unordered" is ZF=PF=CF=1, which is why `jp` after an fcomi is the
 * NaN test, and OF/SF/AF are cleared. */
static inline void fcompare_eflags(CPU *c, double a, double b) {
    c->of = c->sf = c->af = 0;
    if      (a > b)  { c->zf = 0; c->pf = 0; c->cf = 0; }
    else if (a < b)  { c->zf = 0; c->pf = 0; c->cf = 1; }
    else if (a == b) { c->zf = 1; c->pf = 0; c->cf = 0; }
    else             { c->zf = 1; c->pf = 1; c->cf = 1; }
}
/* sahf: load AH into CF,PF,AF,ZF,SF */
static inline void do_sahf(CPU *c, uint8_t ah) {
    c->cf = ah & 1; c->pf = (ah >> 2) & 1; c->af = (ah >> 4) & 1;
    c->zf = (ah >> 6) & 1; c->sf = (ah >> 7) & 1;
}

/* ---- SSE ----
 *
 * Enough of SSE/SSE2 for a Pentium 4-era game: scalar float and double, the
 * packed moves and bitwise ops, and the compares. Packed arithmetic beyond
 * that is not here yet - it lifts to a TODO rather than silently wrong code.
 */

/* Memory operands. Separate from the x87 rdf32/rdf64 above because those
 * widen to double for the 80-bit stack, and these must not. memcpy because an
 * SSE memory operand is frequently unaligned (movups exists for a reason). */
static inline float  rdss(uint32_t a) { float  f; memcpy(&f, (void *)(uintptr_t)a, 4); return f; }
static inline double rdsd(uint32_t a) { double d; memcpy(&d, (void *)(uintptr_t)a, 8); return d; }
static inline XMM    rdxm(uint32_t a) { XMM x;    memcpy(&x, (void *)(uintptr_t)a, 16); return x; }
static inline void   wrss(uint32_t a, float  v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void   wrsd(uint32_t a, double v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline void   wrxm(uint32_t a, XMM    v) { memcpy((void *)(uintptr_t)a, &v, 16); }

/* MOVSS/MOVSD loading from memory zero the rest of the register; the
 * register-to-register forms leave the high lanes alone. Getting that
 * backwards is invisible until something reads the high lanes, which is why
 * it is a named helper and not an open-coded assignment. */
/* Packed SSE as one vector operation. Lane by lane (`_d.f32[i] * _s.f32[i]` four
 * times) the compiler emitted four mulss for each mulps; with GCC/Clang vector
 * types it is one mulps on x86 and one fmul on ARM64 NEON, with the same IEEE result
 * per lane. Only the operations whose lanes are independent and exact go here
 * (+ - * /, wrapping integer +/-, bitwise, shuffles); min/max keep their own NaN
 * rule elsewhere. memcpy in and out: no alignment or aliasing assumption on XMM. */
#if defined(__GNUC__) || defined(__clang__)
static inline float  sse_minf(float  d, float  s) { return (d < s) ? d : s; }
static inline float  sse_maxf(float  d, float  s) { return (d > s) ? d : s; }
static inline double sse_mind(double d, double s) { return (d < s) ? d : s; }
static inline double sse_maxd(double d, double s) { return (d > s) ? d : s; }

static inline XMM sse_load_ss(float v) { return (XMM){v, 0.0f, 0.0f, 0.0f}; }
static inline XMM sse_load_sd(double v) { return (XMM)(xv2d){v, 0.0}; }
static inline double xmm_get_sd(XMM v) { return ((xv2d)v)[0]; }
static inline XMM xmm_movsd_reg(XMM d, XMM s) { xv2d vd = (xv2d)d, vs = (xv2d)s; vd[0] = vs[0]; return (XMM)vd; }

static inline XMM xmm_addsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] += s; return (XMM)v; }
static inline XMM xmm_subsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] -= s; return (XMM)v; }
static inline XMM xmm_mulsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] *= s; return (XMM)v; }
static inline XMM xmm_divsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] /= s; return (XMM)v; }
static inline XMM xmm_sqrtsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] = sqrt(s); return (XMM)v; }
static inline XMM xmm_minsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] = sse_mind(v[0], s); return (XMM)v; }
static inline XMM xmm_maxsd(XMM d, double s) { xv2d v = (xv2d)d; v[0] = sse_maxd(v[0], s); return (XMM)v; }
static inline XMM xmm_cvtsi2sd(XMM d, int32_t s) { xv2d v = (xv2d)d; v[0] = (double)s; return (XMM)v; }
static inline XMM xmm_cvtss2sd(XMM d, float s) { xv2d v = (xv2d)d; v[0] = (double)s; return (XMM)v; }

static inline XMM xmm_from_u32(uint32_t v) { return (XMM)(xv4d){v, 0, 0, 0}; }
static inline uint32_t xmm_to_u32(XMM v) { return ((xv4d)v)[0]; }
static inline XMM xmm_from_u64(uint64_t v) { return (XMM)(xv2q){v, 0}; }
static inline uint64_t xmm_to_u64(XMM v) { return ((xv2q)v)[0]; }
static inline uint64_t xmm_get_qword(XMM v, int lane) { return ((xv2q)v)[lane]; }
static inline XMM xmm_set_qword(XMM d, int lane, uint64_t v) { xv2q q = (xv2q)d; q[lane] = v; return (XMM)q; }
static inline XMM xmm_pinsrw(XMM d, int i, uint16_t v) { xv8w w = (xv8w)d; w[i & 7] = v; return (XMM)w; }

static inline XMM xmm_addps(XMM a, XMM b) { return a + b; }
static inline XMM xmm_subps(XMM a, XMM b) { return a - b; }
static inline XMM xmm_mulps(XMM a, XMM b) { return a * b; }
static inline XMM xmm_divps(XMM a, XMM b) { return a / b; }

static inline XMM xmm_addpd(XMM a, XMM b) { return (XMM)((xv2d)a + (xv2d)b); }
static inline XMM xmm_subpd(XMM a, XMM b) { return (XMM)((xv2d)a - (xv2d)b); }
static inline XMM xmm_mulpd(XMM a, XMM b) { return (XMM)((xv2d)a * (xv2d)b); }
static inline XMM xmm_divpd(XMM a, XMM b) { return (XMM)((xv2d)a / (xv2d)b); }

static inline XMM xmm_paddb(XMM a, XMM b) { return (XMM)((xv16b)a + (xv16b)b); }
static inline XMM xmm_psubb(XMM a, XMM b) { return (XMM)((xv16b)a - (xv16b)b); }
static inline XMM xmm_paddw(XMM a, XMM b) { return (XMM)((xv8w)a + (xv8w)b); }
static inline XMM xmm_psubw(XMM a, XMM b) { return (XMM)((xv8w)a - (xv8w)b); }
static inline XMM xmm_paddd(XMM a, XMM b) { return (XMM)((xv4d)a + (xv4d)b); }
static inline XMM xmm_psubd(XMM a, XMM b) { return (XMM)((xv4d)a - (xv4d)b); }
static inline XMM xmm_paddq(XMM a, XMM b) { return (XMM)((xv2q)a + (xv2q)b); }
static inline XMM xmm_psubq(XMM a, XMM b) { return (XMM)((xv2q)a - (xv2q)b); }

static inline XMM xmm_and(XMM a, XMM b) { return (XMM)((xv2q)a & (xv2q)b); }
static inline XMM xmm_or(XMM a, XMM b) { return (XMM)((xv2q)a | (xv2q)b); }
static inline XMM xmm_xor(XMM a, XMM b) { return (XMM)((xv2q)a ^ (xv2q)b); }
static inline XMM xmm_andn(XMM a, XMM b) { return (XMM)(~(xv2q)a & (xv2q)b); }

#if defined(__clang__)
#define XMM_SHUF4(d, s, i0, i1, i2, i3) \
    ((XMM)__builtin_shufflevector((xv4d)(d), (xv4d)(s), (i0), (i1), (i2), (i3)))
#else
#define XMM_SHUF4(d, s, i0, i1, i2, i3) \
    ((XMM)__builtin_shuffle((xv4d)(d), (xv4d)(s), (xv4d){(i0), (i1), (i2), (i3)}))
#endif

static inline XMM xmm_shufpd(XMM d, XMM s, int imm) {
    xv2d vd = (xv2d)d, vs = (xv2d)s;
    return (XMM)(xv2d){ vd[imm & 1], vs[(imm >> 1) & 1] };
}
static inline XMM xmm_unpcklpd(XMM d, XMM s) { xv2q qd = (xv2q)d, qs = (xv2q)s; return (XMM)(xv2q){qd[0], qs[0]}; }
static inline XMM xmm_unpckhpd(XMM d, XMM s) { xv2q qd = (xv2q)d, qs = (xv2q)s; return (XMM)(xv2q){qd[1], qs[1]}; }

static inline XMM xmm_minps(XMM a, XMM b) { return (XMM)_mm_min_ps((__m128)a, (__m128)b); }
static inline XMM xmm_maxps(XMM a, XMM b) { return (XMM)_mm_max_ps((__m128)a, (__m128)b); }
static inline XMM xmm_minpd(XMM a, XMM b) { return (XMM)_mm_min_pd((__m128d)a, (__m128d)b); }
static inline XMM xmm_maxpd(XMM a, XMM b) { return (XMM)_mm_max_pd((__m128d)a, (__m128d)b); }

static inline XMM xmm_pmaxsw(XMM a, XMM b) { return (XMM)_mm_max_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_pminsw(XMM a, XMM b) { return (XMM)_mm_min_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_pmaxub(XMM a, XMM b) { return (XMM)_mm_max_epu8((__m128i)a, (__m128i)b); }
static inline XMM xmm_pminub(XMM a, XMM b) { return (XMM)_mm_min_epu8((__m128i)a, (__m128i)b); }

static inline XMM xmm_paddsb(XMM a, XMM b) { return (XMM)_mm_adds_epi8((__m128i)a, (__m128i)b); }
static inline XMM xmm_paddsw(XMM a, XMM b) { return (XMM)_mm_adds_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_psubsb(XMM a, XMM b) { return (XMM)_mm_subs_epi8((__m128i)a, (__m128i)b); }
static inline XMM xmm_psubsw(XMM a, XMM b) { return (XMM)_mm_subs_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_paddusb(XMM a, XMM b) { return (XMM)_mm_adds_epu8((__m128i)a, (__m128i)b); }
static inline XMM xmm_paddusw(XMM a, XMM b) { return (XMM)_mm_adds_epu16((__m128i)a, (__m128i)b); }
static inline XMM xmm_psubusb(XMM a, XMM b) { return (XMM)_mm_subs_epu8((__m128i)a, (__m128i)b); }
static inline XMM xmm_psubusw(XMM a, XMM b) { return (XMM)_mm_subs_epu16((__m128i)a, (__m128i)b); }

static inline XMM xmm_pcmpeqb(XMM a, XMM b) { return (XMM)((xv16i)a == (xv16i)b); }
static inline XMM xmm_pcmpeqw(XMM a, XMM b) { return (XMM)((xv8i)a == (xv8i)b); }
static inline XMM xmm_pcmpeqd(XMM a, XMM b) { return (XMM)((xv4i)a == (xv4i)b); }
static inline XMM xmm_pcmpgtb(XMM a, XMM b) { return (XMM)((xv16i)a > (xv16i)b); }
static inline XMM xmm_pcmpgtw(XMM a, XMM b) { return (XMM)((xv8i)a > (xv8i)b); }
static inline XMM xmm_pcmpgtd(XMM a, XMM b) { return (XMM)((xv4i)a > (xv4i)b); }

static inline XMM xmm_pmullw(XMM a, XMM b) { return (XMM)_mm_mullo_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_pmaddwd(XMM a, XMM b) { return (XMM)_mm_madd_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_packsswb(XMM a, XMM b) { return (XMM)_mm_packs_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_packuswb(XMM a, XMM b) { return (XMM)_mm_packus_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_packssdw(XMM a, XMM b) { return (XMM)_mm_packs_epi32((__m128i)a, (__m128i)b); }

static inline XMM xmm_punpcklbw(XMM a, XMM b) { return (XMM)_mm_unpacklo_epi8((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpcklwd(XMM a, XMM b) { return (XMM)_mm_unpacklo_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpckldq(XMM a, XMM b) { return (XMM)_mm_unpacklo_epi32((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpcklqdq(XMM a, XMM b) { return (XMM)_mm_unpacklo_epi64((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpckhbw(XMM a, XMM b) { return (XMM)_mm_unpackhi_epi8((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpckhwd(XMM a, XMM b) { return (XMM)_mm_unpackhi_epi16((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpckhdq(XMM a, XMM b) { return (XMM)_mm_unpackhi_epi32((__m128i)a, (__m128i)b); }
static inline XMM xmm_punpckhqdq(XMM a, XMM b) { return (XMM)_mm_unpackhi_epi64((__m128i)a, (__m128i)b); }

static inline XMM xmm_cvtdq2ps(XMM a) { return __builtin_convertvector((xv4i)a, xv4f); }
static inline XMM xmm_cvttps2dq(XMM a) { return (XMM)__builtin_convertvector(a, xv4i); }
static inline XMM xmm_cvtps2dq(XMM a) { return (XMM)_mm_cvtps_epi32((__m128)a); }
static inline XMM xmm_cvtdq2pd(XMM a) { return (XMM)_mm_cvtepi32_pd((__m128i)a); }
static inline XMM xmm_cvtps2pd(float a, float b) { return (XMM)(xv2d){(double)a, (double)b}; }
static inline XMM xmm_cvtpd2ps(XMM a) { xv2d d = (xv2d)a; return (XMM){(float)d[0], (float)d[1], 0.0f, 0.0f}; }

static inline XMM xmm_psllw(XMM a, XMM c) { return (XMM)_mm_sll_epi16((__m128i)a, (__m128i)c); }
static inline XMM xmm_pslld(XMM a, XMM c) { return (XMM)_mm_sll_epi32((__m128i)a, (__m128i)c); }
static inline XMM xmm_psllq(XMM a, XMM c) { return (XMM)_mm_sll_epi64((__m128i)a, (__m128i)c); }
static inline XMM xmm_psraw(XMM a, XMM c) { return (XMM)_mm_sra_epi16((__m128i)a, (__m128i)c); }
static inline XMM xmm_psrad(XMM a, XMM c) { return (XMM)_mm_sra_epi32((__m128i)a, (__m128i)c); }
static inline XMM xmm_psrlw(XMM a, XMM c) { return (XMM)_mm_srl_epi16((__m128i)a, (__m128i)c); }
static inline XMM xmm_psrld(XMM a, XMM c) { return (XMM)_mm_srl_epi32((__m128i)a, (__m128i)c); }
static inline XMM xmm_psrlq(XMM a, XMM c) { return (XMM)_mm_srl_epi64((__m128i)a, (__m128i)c); }

static inline XMM xmm_psllw_imm(XMM a, unsigned n) { return (XMM)_mm_slli_epi16((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_pslld_imm(XMM a, unsigned n) { return (XMM)_mm_slli_epi32((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_psllq_imm(XMM a, unsigned n) { return (XMM)_mm_slli_epi64((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_psraw_imm(XMM a, unsigned n) { return (XMM)_mm_srai_epi16((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_psrad_imm(XMM a, unsigned n) { return (XMM)_mm_srai_epi32((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_psrlw_imm(XMM a, unsigned n) { return (XMM)_mm_srli_epi16((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_psrld_imm(XMM a, unsigned n) { return (XMM)_mm_srli_epi32((__m128i)a, n > 255 ? 255 : n); }
static inline XMM xmm_psrlq_imm(XMM a, unsigned n) { return (XMM)_mm_srli_epi64((__m128i)a, n > 255 ? 255 : n); }

static inline XMM xmm_pslldq(XMM a, unsigned k) {
    if (k >= 16) return (XMM)(xv4d){0, 0, 0, 0};
    uint8_t t[16] = {0};
    memcpy(t + k, &a, 16 - k);
    XMM r; memcpy(&r, t, 16); return r;
}
static inline XMM xmm_psrldq(XMM a, unsigned k) {
    if (k >= 16) return (XMM)(xv4d){0, 0, 0, 0};
    uint8_t t[16] = {0};
    memcpy(t, (const uint8_t *)&a + k, 16 - k);
    XMM r; memcpy(&r, t, 16); return r;
}

static inline XMM xmm_cmp_ss(XMM d, float a, float b, int pred) {
    int cond = 0;
    switch (pred) {
        case 0: cond = (a == b); break;
        case 1: cond = (a < b); break;
        case 2: cond = (a <= b); break;
        case 3: cond = (a != a || b != b); break;
        case 4: cond = (a != b); break;
        case 5: cond = !(a < b); break;
        case 6: cond = !(a <= b); break;
        case 7: cond = !(a != a || b != b); break;
    }
    xv4d v = (xv4d)d;
    v[0] = cond ? 0xFFFFFFFFu : 0u;
    return (XMM)v;
}

static inline XMM xmm_cmp_sd(XMM d, double a, double b, int pred) {
    int cond = 0;
    switch (pred) {
        case 0: cond = (a == b); break;
        case 1: cond = (a < b); break;
        case 2: cond = (a <= b); break;
        case 3: cond = (a != a || b != b); break;
        case 4: cond = (a != b); break;
        case 5: cond = !(a < b); break;
        case 6: cond = !(a <= b); break;
        case 7: cond = !(a != a || b != b); break;
    }
    xv2q v = (xv2q)d;
    v[0] = cond ? ~(uint64_t)0 : 0u;
    return (XMM)v;
}

static inline XMM xmm_cmp_ps(XMM a, XMM b, int pred) {
    return (XMM)_mm_cmp_ps((__m128)a, (__m128)b, pred);
}

static inline XMM xmm_cmp_pd(XMM a, XMM b, int pred) {
    return (XMM)_mm_cmp_pd((__m128d)a, (__m128d)b, pred);
}
#else
#define XMM_LOP(name, fld, n, op) \
    static inline XMM name(XMM a, XMM b) { for (int i = 0; i < n; i++) a.fld[i] = a.fld[i] op b.fld[i]; return a; }
XMM_LOP(xmm_addps, f32, 4, +) XMM_LOP(xmm_subps, f32, 4, -) XMM_LOP(xmm_mulps, f32, 4, *) XMM_LOP(xmm_divps, f32, 4, /)
XMM_LOP(xmm_addpd, f64, 2, +) XMM_LOP(xmm_subpd, f64, 2, -) XMM_LOP(xmm_mulpd, f64, 2, *) XMM_LOP(xmm_divpd, f64, 2, /)
XMM_LOP(xmm_paddb, u8, 16, +) XMM_LOP(xmm_psubb, u8, 16, -) XMM_LOP(xmm_paddw, u16, 8, +) XMM_LOP(xmm_psubw, u16, 8, -)
XMM_LOP(xmm_paddd, u32, 4, +) XMM_LOP(xmm_psubd, u32, 4, -) XMM_LOP(xmm_paddq, u64, 2, +) XMM_LOP(xmm_psubq, u64, 2, -)
XMM_LOP(xmm_and, u64, 2, &) XMM_LOP(xmm_or, u64, 2, |) XMM_LOP(xmm_xor, u64, 2, ^)
static inline XMM xmm_andn(XMM a, XMM b) { a.u64[0] = ~a.u64[0] & b.u64[0]; a.u64[1] = ~a.u64[1] & b.u64[1]; return a; }
static inline XMM xmm_shuf4(XMM d, XMM s, int i0, int i1, int i2, int i3) {
    XMM r; const int k[4] = {i0, i1, i2, i3};
    for (int i = 0; i < 4; i++) r.u32[i] = k[i] < 4 ? d.u32[k[i]] : s.u32[k[i] - 4];
    return r;
}
#define XMM_SHUF4(d, s, i0, i1, i2, i3) xmm_shuf4((d), (s), i0, i1, i2, i3)
static inline void sse_load_ss(XMM *d, float  v) { d->u64[0] = 0; d->u64[1] = 0; d->f32[0] = v; }
static inline void sse_load_sd(XMM *d, double v) { d->u64[0] = 0; d->u64[1] = 0; d->f64[0] = v; }
#endif

/* UCOMISS/COMISS/UCOMISD/COMISD: the comparison lands in ZF/PF/CF, and clears
 * OF/SF/AF. An unordered compare - either operand NaN - sets all three, which
 * is exactly what makes `ucomiss; jp` a NaN test and `ucomiss; jbe` mean
 * "below or equal, or unordered". Note the flags are the UNSIGNED set: the
 * compiler emits `ja`/`jbe` after a float compare, never `jg`/`jle`. */
static inline void sse_compare(CPU *c, double a, double b) {
    c->of = 0; c->sf = 0; c->af = 0;
    if (a != a || b != b) { c->zf = 1; c->pf = 1; c->cf = 1; }   /* unordered */
    else                  { c->zf = (a == b); c->pf = 0; c->cf = (a < b); }
}

/* MINSS/MAXSS are not fmin/fmax. The manual defines them as
 *     if (dst OP src) then dst else src
 * and that "else src" is load-bearing in two cases fmin gets the other way
 * round: when the operands are equal - which includes +0.0 against -0.0, so
 * min(+0,-0) is -0 but min(-0,+0) is +0 - and when either is NaN, where the
 * comparison is false and the SECOND operand wins. Written in that exact
 * shape, C's own "any compare with NaN is false" rule delivers both for free.
 * Writing it the other way round, `(src < dst) ? src : dst`, looks identical
 * and is wrong on both. */
#if !defined(__GNUC__) && !defined(__clang__)
static inline float  sse_minf(float  d, float  s) { return (d < s) ? d : s; }
static inline float  sse_maxf(float  d, float  s) { return (d > s) ? d : s; }
static inline double sse_mind(double d, double s) { return (d < s) ? d : s; }
static inline double sse_maxd(double d, double s) { return (d > s) ? d : s; }
#endif

/* CVTTSS2SI and friends yield the "integer indefinite" value when the source
 * does not fit in 32 bits, where the equivalent C cast is undefined behaviour
 * and may trap. One comparison buys a defined answer that matches hardware. */
static inline int32_t sse_cvtt_i32(double v) {
    return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : (int32_t)0x80000000;
}

/* ---- BT / BTS / BTR / BTC ----
 *
 * With a memory destination and a register bit index, the index is NOT masked
 * to the operand size: it selects a dword away from the base address, which is
 * how a bitmap of arbitrary size gets addressed. Masking it the way the
 * register form does would quietly read and write the wrong word - the kind of
 * wrong that shows up as unrelated corruption much later.
 *
 * op: 0 test, 1 set, 2 reset, 3 complement. CF receives the bit as it was. */
static inline uint32_t bit_string_op(CPU *c, uint32_t addr, int32_t idx, int op)
{
    uint32_t a = addr + 4u * (uint32_t)(idx >> 5);
    unsigned b = (unsigned)(idx & 31);
    uint32_t w = rd32(a);
    c->cf = (w >> b) & 1u;
    if      (op == 1) w |=  (1u << b);
    else if (op == 2) w &= ~(1u << b);
    else if (op == 3) w ^=  (1u << b);
    if (op) wr32(a, w);
    return c->cf;
}

/* The register form, where the index really is taken modulo the width. */
static inline uint32_t bit_reg_op(CPU *c, uint32_t *r, uint32_t idx, int op)
{
    unsigned b = idx & 31u;
    c->cf = (*r >> b) & 1u;
    if      (op == 1) *r |=  (1u << b);
    else if (op == 2) *r &= ~(1u << b);
    else if (op == 3) *r ^=  (1u << b);
    return c->cf;
}

/* ---- 80-bit extended precision ----
 *
 * The x87 stack here is modelled as doubles, which is right for almost
 * everything - but `fld tbyte` and `fstp tbyte` name the hardware's own
 * 80-bit format directly, and a compiler emits them to spill a register
 * without losing precision. Reading one as anything else returns nonsense.
 *
 * The format is a sign bit, a 15-bit exponent biased by 16383, and a 64-bit
 * mantissa whose integer bit is EXPLICIT - unlike float and double, where it
 * is implied. Converting to double loses the extra bits, which is the same
 * loss the rest of this model already accepts.
 */
static inline double rdf80(uint32_t a)
{
    unsigned char b[10];
    memcpy(b, (void *)(uintptr_t)a, 10);

    uint64_t mant;
    uint16_t se;
    memcpy(&mant, b, 8);
    memcpy(&se, b + 8, 2);

    int sign = (se >> 15) & 1;
    int exp  = se & 0x7FFF;

    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) {
        if (mant << 1) return (double)NAN;
        return sign ? -(double)INFINITY : (double)INFINITY;
    }
    double m = (double)mant * 5.4210108624275222e-20;   /* 2^-64 */
    return ldexp(sign ? -m : m, exp - 16383 + 1);
}

static inline void wrf80(uint32_t a, double v)
{
    unsigned char b[10];
    uint64_t mant = 0;
    uint16_t se = 0;

    memset(b, 0, sizeof b);
    if (v != v) {                                   /* NaN */
        se = 0x7FFF; mant = 0xC000000000000000ull;
    } else if (v == (double)INFINITY || v == -(double)INFINITY) {
        se = 0x7FFF; mant = 0x8000000000000000ull;
        if (v < 0) se |= 0x8000;
    } else if (v != 0.0) {
        int e;
        double m = frexp(signbit(v) ? -v : v, &e);   /* m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);
        se = (uint16_t)((e - 1 + 16383) & 0x7FFF);
        if (signbit(v)) se |= 0x8000;
    } else if (signbit(v)) {
        se = 0x8000;                                 /* -0.0: mantissa 0, sign set */
    }
    memcpy(b, &mant, 8);
    memcpy(b + 8, &se, 2);
    memcpy((void *)(uintptr_t)a, b, 10);
}

/* FPREM: the remainder of st0/st1 with the quotient truncated toward zero,
 * which is what fmod computes. The hardware may reduce only partially and set
 * C2 to say "call me again"; computing the whole thing at once means C2 is
 * always clear, and the loop the compiler wrote around it runs once. */
/* fprem (nearest = 0: quotient truncated, fmod) and fprem1 (nearest = 1:
 * rounded, remainder). C2 = 0 (reduction complete: a double never needs the
 * partial steps of the 80-bit unit); the low three quotient bits go to
 * C0 (Q2), C3 (Q1), C1 (Q0). */
static inline double fprem_op(CPU *c, double a, double b, int nearest)
{
    double r = nearest ? remainder(a, b) : fmod(a, b);
    uint32_t sw = c->fpu_sw & ~0x4700u;
    if (r == r && b == b && b != 0.0 && !isinf(a)) {
        double q = nearest ? nearbyint((a - r) / b) : trunc((a - r) / b);
        uint32_t qb = (uint32_t)(uint64_t)fmod(fabs(q), 8.0);
        sw |= ((qb >> 2) & 1u) << 8 | ((qb >> 1) & 1u) << 14 | (qb & 1u) << 9;
    }
    c->fpu_sw = sw;
    return r;
}
/* The packed integer ops that saturate rather than wrap. PADDSW and friends
 * clamp to the lane's own range instead of carrying into the next lane, and
 * the arithmetic has to be done a width up for the clamp to see the overflow
 * at all - which is the whole reason these are functions and not expressions
 * in the generated code. */
static inline int8_t   sse_sat_i8 (int32_t v) { return v >  127 ?  127 : v < -128 ? -128 : (int8_t)v; }
static inline int16_t  sse_sat_i16(int32_t v) { return v >  32767 ?  32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t  sse_sat_u8 (int32_t v) { return v >  255 ?  255 : v < 0 ? 0 : (uint8_t)v; }
static inline uint16_t sse_sat_u16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

#endif /* PCRECOMP_CPU_H */
