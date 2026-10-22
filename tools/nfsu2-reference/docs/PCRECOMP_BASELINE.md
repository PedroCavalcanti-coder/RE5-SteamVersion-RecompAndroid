# pcrecomp baseline

pcrecomp `35548b8` (upstream HEAD on 2026-10-06), before any change.
Environment: Windows 11, Python 3.14, capstone 5.0.7, unicorn, MSVC 14.51 x86,
LLVM/clang 22.1.8 (`--target=i686-pc-windows-msvc`, via `scripts/clang32.bat`).

| Test | Result |
|---|---|
| `--selftest` of rtti, vtable_scan, disasm32, score_recovery, seed_from_log, emu_unpack, steamstub, generate, lift32_cpu, lift64_cpu, recover, catalog, debug_symbols, map_names, merge_names, pe_analyze, rsrc | all OK |
| `stdcall_argc --selftest` | **FAIL**: picked the partial SDK 10.0.28000.0 (no `kernel32.lib`) → fixed in patch 0001, now 53/53 |
| test_unlzexe, test_bswap, test_fpu_operands | OK |
| `sse_selftest.py` (needs a C compiler) | 59/59 |
| `difftest.py` (lift32 vs Unicorn) | 202/202 |
| C selftests: recomp32_cpu/cpu_selftest, recomp32/flags_selftest, mmx_selftest, hybrid_selftest, native32_selftest | all pass |
| win32hle_selftest | not linked (my invocation lacked its other sources); not a toolkit failure |

After this project's patches: difftest **242/242** (40 new SIMD cases), all
selftests above still pass.

Notes:
- `difftest.py` only covers `lift32.py`; `lift32_cpu.py` has no differential coverage.
- clang on Windows defaults to x64; the tests need `RECOMP_CC=scripts/clang32.bat` and an MSVC x86 environment (`source scripts/vsenv.sh x86`).
