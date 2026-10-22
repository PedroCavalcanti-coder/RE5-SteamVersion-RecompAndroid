# Differential test report

Two levels, both "compare against a reference x86, not against expectations".

## 1. pcrecomp instruction-level difftest (lift32 + simd32 vs Unicorn)

`python ../pcrecomp/tools/lift/difftest.py` (patched): **246 / 246** cases
match, including 40 MMX/SSE/SSE2 cases and 4 `lahf` cases added for this
project.

## 2. Real SPEED2.EXE functions (`scripts/difftest_real.py`)

Each selected function is a leaf (no calls, no indirect jumps, no `fs:`, no
3DNow!, no tail jumps out of its body). The original bytes run in Unicorn
with the real image mapped; the same function, lifted by our driver, runs as C
compiled for an **x64** host with the portable `ADDR(va) = va + g_mem_base`
memory model. Registers, x87 depth/st(0), and every byte written to .data,
scratch and stack are compared. Cases where both machines fault on random
pointers are skipped.

| Batch | functions | cases tested | passed | failed | skipped |
|---|---|---|---|---|---|
| random (seed 2) | 600 | 947 | 940 | 7 | 853 |
| x87-using (seed 3) | 300 | 505 | 492 | 13 | 395 |
| SSE-using (seed 4) | 72 | 23 | 23 | 0 | 193 |

Failure reasons:

| Function | What | Verdict |
|---|---|---|
| (many, first run) | `fnstsw ax` lacked the TOP field (bits 11-13) | **lifter bug, fixed** (lift32: TOP = -depth mod 8) |
| 0x0063E88B (D3DXQuaternionRotationYawPitchRoll) | one float's sign: +0.0 vs -0.0 | numerical: 80-bit `fsincos` vs double, value at zero |
| 0x007699FB, 0x0076A496 (CRT math/exception) | inf/NaN vs finite on extreme random inputs | CRT edge cases; not reached in normal play |
| 0x00416905, 0x00768A48, 0x00769DBF, 0x005E1250, 0x00769DCA | Unicorn faults, lifted does not | harness: the lifted code reads guest memory through a 4 GB reservation where Unicorn maps only three regions; not a semantic difference |

Harness bugs found on the way (fixed): Unicorn's `FPn` registers are
physical (st(0) is `FP[TOP]`), Unicorn's MXCSR starts at 0 (all SSE exceptions
unmasked), the x87 depth of the lift32 model is `g_fp_top` itself.

## 3. Whole-program oracle (`NFSU2_NATIVE=1`)

The host can run the ORIGINAL x86 code natively, with the same image
mapping, import shims and windowed mode (vtable hooks). Same input script
(`NFSU2_TAP`, `NFSU2_BACKGROUND`): the original reaches a race with the car
rendered correctly; the recompiled build shows black cars and crashes in
0x005E5110 when the race loads. The divergence is therefore in the
recompilation, and is being bisected.
