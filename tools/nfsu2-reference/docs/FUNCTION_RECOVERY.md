# Function recovery

Sources: entry point, recursive descent over direct `call`/`jmp` targets,
prologue scan, jump tables, stored code pointers in data (pcrecomp disasm32),
plus seeds from `vtable_scan.py` (4,432 methods) and `rtti.py` (6 methods).
Input: `work/SPEED2.analysis.exe` (relocations restored).

| Stat | Value |
|---|---|
| detected_functions | 27,742 |
| of which aliases | 4,599 (2,220 labels inside another body, 2,379 C++ EH stubs/funclets) |
| thunks / leaves | 71 / 8,065 |
| functions reached only via data pointers | 2,293 |
| vtable_targets (seeds) | 4,432 |
| RTTI_methods | 6 |
| entries dropped (not instruction boundaries) | 388 |
| data pointers rejected (inside an instruction / not code) | 9,239 / 53 |
| covered_text_bytes | 3,544,709 of 3,676,913 |
| coverage_percent | 96.4 % of `.text` |

## The uncovered 132,204 bytes (`scripts/coverage_gaps.py`)

| Class | gaps | bytes |
|---|---|---|
| padding (int3/nop/zero) | 12,281 | 96,170 |
| pointer tables in `.text` (contain relocation slots) | 425 | 33,914 |
| decodes as code, unreached | 36 | 1,937 |
| unknown | 22 | 183 |

So unrecovered *code* is at most ~2.1 KB: **≈99.9 % of the real code is
catalogued**, without counting data as functions.

## Known problems

- `suspected_false_starts`: 0x6D5610 lifts to BCD/`arpl`/`bound`/`insb`/`lcall`
  — data decoded as code. To be excluded.
- `suspected_missing_starts`: the 36 code-like gaps (e.g. 0x43BD51, 0x44052F,
  0x6945EA) need checking; they may be dead code.
- nfsu2-re's 1,693 named functions have not yet been scored against the
  catalog (`score_recovery.py`).
