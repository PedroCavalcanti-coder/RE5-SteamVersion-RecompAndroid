# Lift report

Driver: `scripts/run_lift.py lift` (pcrecomp `lift32` + patched `simd32`),
`precise_carry=True`, 400 functions per file.

| Metric | Value |
|---|---|
| functions lifted | 27,742 |
| lift failures (exceptions) | 0 |
| files | 70 + `recomp_funcs.h`, `recomp_dispatch.c`, `recomp_imports.c` |
| C lines | 8,935,801 (454 MB, with one address comment per instruction) |
| largest file | `recomp_0034.c`, 1.6 M lines; biggest functions 18–20 k lines (around 0x620AE0) |
| lift time | 86 s |
| `RECOMP_UNIMPL` sites | 2,923 |
| ICALL / ITAIL sites | 13,271 / 3,126 |

## RECOMP_UNIMPL breakdown

| Instruction | sites | Assessment |
|---|---|---|
| 3DNow! (`pf*`, `pi2fd`, `pswapd`) | 2,890 | CPU-dispatched alternatives; avoid by reporting no 3DNow! in CPUID |
| `int` | 17 | 0x55FF54, 0x769DBA… — to inspect (CRT `int 3`/debug traps?) |
| `fnstenv` / `fldenv` | 4 | CRT FPU environment save/restore (0x76A496, 0x76A74E) — must implement |
| `lahf` | 2 | 0x6F5FC5 real; implement |
| `sldt daa das aaa aas bound arpl insb insd lcall` | 10 | all in 0x6D5610, a false function |

## Compile

- `recomp_0010.c` (42 k lines): MSVC x86 `/O1` 1.4 s, OK.
- `recomp_0034.c` (1.6 M lines): MSVC `/O1` 207 s (101 MB object); clang 22 `-O1` 785 s.
- Whole project (CMake + Ninja, -j16): 4 min 20 s, 0 errors, 0 unresolved symbols at link.

After the first run the lift was regenerated with `lahf`, `fnstenv/fldenv`,
backward-branch yields, one override (0x6F5FC5) and one exclusion (0x6D5610):
`RECOMP_UNIMPL` sites 2,907 (2,890 3DNow!, 17 `int` in data/unreachable code).
