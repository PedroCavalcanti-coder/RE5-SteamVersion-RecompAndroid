# disasm64: x86-64 function recovery, scored against Microsoft's symbols

`tools/disasm/disasm64.py` finds the functions in a PE32+ image and writes the
catalog `lift/generate64.py` lifts from. Before it, the 64-bit path started from
an IDA export. `tools/pe/pdb_ref.py` builds the reference it is scored against,
from the vendor's own PDB.

## Where the starts come from

A 64-bit Windows image describes its own code far better than a 32-bit one, so
the seeds are read, not guessed:

| Seed | Source | What it misses |
|---|---|---|
| `.pdata` | every function with a frame | frameless leaves |
| Guard CF table | load config, `/guard:cf` (all of Windows) | functions whose address is never taken |
| exports, entry point | export directory, optional header | |
| DIR64 relocations into code | `.reloc` | |
| EH funclets | C++ FuncInfo, FH3 and FH4 (generate64's `eh_tables`), and `__C_specific_handler` scope tables | |
| `jmp rel32` thunks | int3-padded stubs (generate64's `jmp_thunks`) | |

Recursive descent from those finds the leaves through `call rel32`.

## What the scoring turned up

Each of these was a real defect found by scoring against the PDB. The numbers
are from Windows 11 24H2, build 26100.4349.

- **`.pdata` is a function list once chained entries are removed.** An entry
  whose unwind info has `UNW_FLAG_CHAININFO` (or whose UnwindData has its low
  bit set) is a PGO-split cold chunk of another function. Taking only the
  primary entries is what made `.pdata` usable here. generate64 had rejected it
  as "not a function list".
- **MSVC puts out-of-line blocks after an unreached `int3`.** A function's end
  is the end of the contiguous code from its start, but a gap of
  `int3`/`nop` padding (including `0f 1f` multi-byte nops) between decoded
  blocks is not a gap. Stopping at the first undecoded byte cut 3,306 of
  ntdll's functions short: 11.6% exact ends before, 92.3% after.
- **`__except` bodies are reached only through the scope table.** No jump goes
  to them, so the function stopped before its handler. The scope table follows
  the handler RVA in the unwind info, as `{Begin, End, Handler, JumpTarget}`.
  It is believed only when every scope lies inside the function `.pdata` says
  it covers. The handler bodies became extra entries of the parent, and the
  filter and finally funclets became seeds. ntdll's exact ends went from 92.3%
  to 96.5%.
- **The kernel stuffs the return stack.** ntoskrnl's Spectre mitigations
  `call` over a trap to code that starts with `add rsp, 8`. No function
  discards its own return address, so such a target is followed as a jump.
  That removed 2,304 invented functions.
- **The kernel keeps string literals in code sections** (PAGE, INIT,
  `.text`). Relocated pointers to them are DIR64s into code, and 2,166 of
  them had been catalogued as functions. They are now rejected by
  `looks_like_string`: NUL-terminated ASCII or UTF-16LE, or a leading NUL
  for `""`.
- **C++ code compiled for `__CxxFrameHandler4`** (MSVC 2019 and later) has
  compressed EH tables, and `eh_tables` read only the FH3 layout. FH4's
  unwind map calls destructors directly by RVA, so a leaf
  `std::vector::~vector` with no `.pdata` is reached only from that map.
  Functions with destructors and no `try` carry only `UNW_FLAG_UHANDLER`,
  and those were skipped too. diagtrack.dll's recall went from 79.57% to
  85.78%.

## The reference: stripped PDBs

Microsoft's public PDBs are stripped. `llvm-pdbutil dump --summary` says
`Is stripped: true`, and there are no `S_GPROC32` records, so there are no
function sizes. pdb_ref works from what is left:

- **Publics** with the `function` flag give the start and the name of every
  function with external linkage.
- **Section contributions.** Windows is built `/Gy`, so each function sits in
  its own COMDAT, and that COMDAT's range is the function's range. A COMDAT
  with no public is a static function.
- **Not every unnamed code COMDAT is a function.** Two kinds are dropped:
  - PGO cold chunks, recognised by their chained `.pdata` entry. These were
    414 false misses in kernelbase.
  - String literals in the kernel. These were 3,290 false misses, plus 562
    more past a 64-byte window.
- **COMDAT sizes include the trailing `int3`/`nop` padding.** It is trimmed,
  as score_recovery asks.

What the reference cannot see: a static function inside a non-COMDAT
contribution (an assembler module) has no record. Such a function counts
against precision even when it is real.

The PDB comes from `msdl.microsoft.com`, keyed by the RSDS GUID and age in the
image's debug directory. The CDN stalls mid-file without closing the
connection, so the fetch times out after 30 s and resumes with a `Range`
request. A PDB, and any reference built from one, is derived from a
proprietary binary. Both stay in a work directory outside the repo.

## Current numbers

Windows 11 24H2 26100.4349 (Pro), commands as in the README:

| Image | Reference | Precision | Recall | Exact ends | Time |
|---|---:|---:|---:|---:|---:|
| ntdll.dll | 4,538 | 98.24% | 99.65% | 96.52% | 4 s |
| kernel32.dll | 2,651 | 99.47% | 98.38% | 97.35% | 1 s |
| kernelbase.dll | 6,176 | 99.45% | 99.61% | 96.65% | 4 s |
| ntoskrnl.exe | 30,139 | 97.86% | 99.20% | 96.72% | 25 s |
| win32kfull.sys | 9,684 | 96.73% | 99.99% | 97.42% | 8 s |
| win32kbase.sys | 9,181 | 99.57% | 99.98% | 98.65% | 6 s |
| diagtrack.dll | 18,661 | 99.99% | 85.78% | 88.08% | 9 s |

kernel32's catalog lifts through generate64 with 0 lift errors. The closure
adds 29 entries over 5 rounds.

## Known gaps

- **Unreferenced EH funclets.** 2,299 of diagtrack's 2,654 misses are
  destructor funclets (`lea rcx, [rdx+N]; jmp dtor`) whose address appears
  nowhere in the file, as an RVA or as a rel32. They are left over after the
  compiler merged unwind states, and they never run. Counting only functions
  something refers to, diagtrack's recall is 97.83%.

- **Functions reached only by a conditional tail jump**, such as ntdll's
  `Feature_*__private_IsEnabledFallback` stubs, are absorbed into the
  function that jumps to them. That is most of ntdll's 16 misses.
- **Funclets score as splits.** A SEH filter or finally block is in its
  parent's COMDAT, so it reads as a split. Every split sampled in ntdll was
  one (`push rbp; sub rsp, N` inside the parent). They are real entry points,
  which the unwinder calls, so they stay in the catalog. The splits in the
  other images have not been sampled yet.
- **Decoding past a call to a `noreturn` function** that is not followed by
  `int3` runs into whatever comes next, up to the next known start. That is
  the likely cause of the "long" ends, but it has not been measured.
