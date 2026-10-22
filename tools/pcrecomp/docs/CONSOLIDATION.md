# pcrecomp and xboxrecomp should share a spine

Written after the Trespasser audit found a defect in `disasm32.py` that
`xboxrecomp` had already solved. The first half is the assessment; the
[port queue](#the-port-queue) at the bottom is the plan of record.

The original Xbox is x86-32 with an NV2A bolted on. Strip the XBE loader, the
kernel shims and the GPU, and what is left is a static recompiler for the same
instruction set, the same compiler (MSVC), the same calling conventions and the
same C++ ABI as every binary in this repo. Two toolboxes solving one problem,
and only one of them has been scoring itself.

## How this came up

The trespasser project (private) scored
`disasm32.py` against a linker map — real ground truth — and found it missing
7,331 functions that IDA found. The cause was that `find_call_targets` seeded
only `call rel32` (E8) and never `jmp rel32` (E9), so any method reached only
by a tail call stayed invisible.

`xboxrecomp/tools/disasm/xrefs.py` already seeds calls, `jmp rel32`, conditional
jumps, memory references *and* address-taken immediates. It had been right about
this the whole time.

Worse, while fixing it I nearly widened pcrecomp's
`abs(target - start_va) < 0x100000` guard, which would have been a bug — that
guard distinguishes internal control flow from a tail call, and widening it
merges tail-called functions into their callers. `xboxrecomp/tools/disasm/
functions.py` bounds functions by **section end** and uses `0x100000` only as a
fallback when the section end is unknown. It separates the two concerns
correctly; we conflate them.

Two toolboxes, the same architecture, one of them quietly better. Nobody was
comparing them.

## What each has

Both target x86-32 static recompilation. Lines of tooling code:

| xboxrecomp | | pcrecomp | |
|---|---:|---|---:|
| `recomp` (lifter/codegen) | 9,819 | `lift` | 5,118 |
| `disasm` | 4,613 | `disasm` | 2,857 |
| **`conformance`** | 2,532 | — | |
| **`func_id`** | 2,406 | `classify` | 1,600 |
| `xbe_parser` *(Xbox)* | 1,155 | `pe` | 1,590 |
| **`ghidra_naming`** | 952 | `ghidra` | 541 |
| **`fusion`** | 902 | — | |
| `kernel_audit` *(Xbox)* | 711 | — | |
| **`symbols`** | 617 | — | |
| **`abi_analysis`** | 544 | — | |
| `xmv` *(Xbox)* | 512 | — | |
| **`debug_symbols`** | 511 | — | |
| **`split`** | 482 | — | |
| `xiso` *(Xbox)* | 404 | — | |
| **`rtti`** | 351 | — | |
| **`seed_from_log`** | 157 | — | |
| — | | `ne` (16-bit NE) | 1,756 |
| — | | `assets` | 960 |
| — | | `cpp` | 541 |
| — | | `ida` | 436 |
| — | | `drm` | 320 |
| **total** | **25,668** | **total** | **15,719** |

Roughly **2,800 lines of xboxrecomp is genuinely Xbox-specific** (`xbe_parser`,
`xiso`, `xmv`, `kernel_audit`). Nearly everything else is portable x86 work.

## What flows which way

**xboxrecomp → pcrecomp.** About 9,400 lines of portable tooling pcrecomp
simply does not have:

- **`conformance` (2,532 lines).** A generated-C-versus-hardware harness. Its
  comments show it already pins the x87 control word to PC=53 so the model and
  the hardware round identically, isolating genuine lifter bugs from precision
  noise. pcrecomp has `lift/difftest.py` (lifted C vs Unicorn), which is the
  same idea at a fraction of the maturity. This is the single most valuable
  item on the list — the Trespasser scorecard has been hand-rolling validation
  that partly exists here.
- **`func_id` (2,406).** `vtable_scanner`, `imm_scanner`, `crt_identifier`,
  `stub_classifier`, `clustering`. Directly relevant: pcrecomp's unaligned
  data-pointer scan accepts any four bytes that look like a code address, and a
  real vtable scanner -- runs of consecutive code pointers, not isolated hits --
  is the principled replacement for it.
- **`rtti` (351).** Trespasser hand-rolled RTTI extraction in its own
  `tools/recon.py` because pcrecomp had none. This already exists.
- **`disasm` improvements.** E9 seeding is now ported (see below). Section-end
  function bounding is not, and should be.
- **`symbols`, `debug_symbols`, `abi_analysis`, `ghidra_naming`, `fusion`,
  `split`, `seed_from_log`** — all portable, all absent from pcrecomp.

**pcrecomp → xboxrecomp.** Less bulk, but real:

- **`ne` (1,756).** 16-bit New Executable support. Irrelevant to Xbox, so this
  one stays put.
- **`pe` (1,590).** Full PE analysis, delay imports, protection detection.
  xboxrecomp parses XBEs; if it ever wants to look at a Windows binary
  alongside (an Xbox title's PC port, say) this is the piece.
- **`ida` (436).** xboxrecomp has Ghidra scripts but no IDA path.
- **`drm` (320)** and **`assets` (960).** SafeDisc dumping, InstallShield/Wise
  extraction — PC-era problems Xbox does not have, but the Trespasser and
  Quake-family work depends on them.
- **The Quake-family classification experience** (`classify`, 1,600) overlaps
  `func_id` and the better parts of each should merge rather than one winning.

## The honest counter-argument

Merging two working toolchains is a real cost and a real risk. Thirty-odd
projects depend on pcrecomp as it stands; the Xbox projects depend on
xboxrecomp. A big-bang consolidation would put all of them in the blast radius
at once for a benefit that is mostly "less duplication later".

So the recommendation is deliberately narrow:

1. **Port individual modules, measured, one at a time.** The E9 seeding fix was
   done this way — measured against ground truth before and after, output
   compared, ~4 lines. That is the template, and the port queue below follows it.
2. **Start with the disassembler, not the big modules.** The first instinct was
   to take `conformance` and `func_id` first, because Trespasser needs both. That
   is backwards: every downstream tool consumes the function catalog, so until
   the catalog is trustworthy every measurement below it is noisy. Fix what
   produces the catalog first — items 0 to 3 — then the modules that consume
   it. Scoring the catalog first is what turned up item 0, which was not on the
   list at all and turned out to be 87% of the measured error.
3. **Do not attempt a shared repo yet.** Prove the modules transplant cleanly
   first. A `pcrecomp-core` extraction is a conversation for after three or
   four successful ports, not before.
4. **Keep the Xbox-specific 2,800 lines where they are.** They are not
   candidates for anything.


## What the first measurement found

Before porting anything, the queue below was scored against ground truth. It
changed the order, and it changed what item 1 even is. The method:

- **Reference**: `trespass.map` — a linker map from a source build of
  Trespasser, so it is symbol-derived truth rather than another tool's opinion.
  34,159 distinct function starts. The map carries no end addresses, so IDA's
  ranges over an md5-identical copy of the same binary were merged onto its
  starts (92.3% coverage) to make the split/invented breakdown possible.
- **Candidate**: the stored `oracle_functions.json`, restricted to the truth's
  own VA range so the two populations are comparable.

```
  true positives      26,808
  false positives      7,981
      split            7,364  (inside a known function)
      invented           617  (outside every known function)
  false negatives      7,351

  precision   77.06%    recall 78.48%    F1 77.76%
```

**Splits are 92% of the error, and invented is 617 — not the 2,905 this
document claimed.** That number came from a different measurement and does not
reproduce against the map. Item 1 as originally written was aimed at the small
half of the problem.

### What E9 seeding actually did

Re-running the current disassembler on the same binary — the E9 fix in, nothing
else changed — makes the point sharper than the split/invented breakdown alone:

| | before E9 | after E9 |
|---|---:|---:|
| recall | 78.48% | **97.72%** |
| false negatives | 7,351 | **780** |
| precision | 77.06% | 71.48% |
| split | 7,364 | 12,695 |
| invented | 617 | 622 |
| F1 | 77.76% | 82.57% |

Recall went from missing one function in five to missing one in forty-four.
That is the largest single recovery improvement in this repo's history, and it
cost four lines.

**And it reads as a precision regression.** 77.06% → 71.48%, because finding
all those tail-called functions also found all the tail *jumps* into other
functions' bodies, and every one of those became another catalog entry claiming
to be a function start. Splits grew by 5,331; invented did not move at all
(617 → 622), which is the clearest possible evidence that the two numbers are
measuring different things and only one of them is a defect.

A maintainer watching precision alone would have seen a 5.6-point drop and
reverted the best change the tool has had. That is the actual cost of the
missing category, and it is why item 0 below is item 0.

Four things were then checked, because "7,364 splits" has several possible
causes and they need different fixes:

| Question | Answer |
|---|---|
| Are the splits real functions the map simply omits? | **No.** 0 of 7,364 are IDA function starts either, and IDA agrees with the map almost perfectly — of 31,512 IDA starts in range, exactly 1 is not a map start. Both authorities say no function starts there. |
| Are they tiny stubs, or real bodies? | **Real bodies.** 5,890 of 7,364 decode to more than 20 instructions. |
| Which scan produces them? | Not the seed scans. 28 come from the `55 8B EC` prologue scan, 362 from the `E8` scan, 547 from the `E9` scan — and **6,427 from none of them.** |
| So where do they come from? | The fixpoint in `find_functions`, from the branch that promotes a jump landing inside another function's body into a function of its own. |

That branch is deliberate, and its comment says so: a shared epilogue or a
switch arm reached from elsewhere "has no label in the jumping function, so the
lifter tail-dispatches to it; make it a real entry point or that dispatch is
unresolved at runtime."

So the dominant finding is not a recovery defect. **It is a category error in
the catalog format.** The disassembler knows perfectly well that these are not
function starts — it creates them precisely because they are *not*, and the
lifter needs a dispatchable body at an address in the middle of something else.
It then writes them into the catalog in a field that says "function", and every
consumer, the scorecard included, has no way to tell the two apart.

Measuring that as 77% precision measures a design decision. The disassembler was
never claiming a function starts at those 6,427 addresses.

xboxrecomp already draws this distinction — `_build_alias_entries` and
`_pass_seed_aliases` record such an address as an alias of its containing
function, with `detection_method="tail_jump_alias"` and `has_prologue=False`,
bounded by the containing function's end. That is the thing worth taking, and
it is a field, not a redesign.

### What changed as a result

1. **`entry_kind` on every catalog entry** — `"start"` where we believe a
   function begins, `"alias"` where we do not and are only making an address
   dispatchable. Set by the branch that creates them, emitted in the JSON.
2. **`score_recovery.py` holds aliases out of precision** and reports them on
   their own line, split by whether they land inside a known function (expected)
   or outside every one (suspect — a body pointing at nothing). An alias landing
   exactly on a reference start is counted separately again, because that is a
   real function demoted, and a demoted start is not reported as found.
3. **`probes_as_function_body` still lands**, but it is worth far less than
   predicted: 283 false positives removed, only 2 of them invented. Forty lines
   at zero measured recall cost is still a good trade. It is just no longer
   item 1, and it never deserved to be.

The rest of the queue keeps its order. The lesson is the one this repo keeps
re-learning: the number a tool reports about itself is not a measurement, and
the first real measurement usually reorders the work.

## The port queue

Ordered by (what it fixes) / (what it costs), not by size. Each item names the
source file, the defect it closes, and **how to know it worked** -- because the
whole reason this document exists is that nobody was measuring.

The rule for every item: port it, measure it with `disasm/score_recovery.py`
against a reference catalog before and after, and keep the diff small enough to
read. The E9 fix was four lines and recovered 6,550 functions. That ratio is
the target, not the exception.

---

### 0. Function start vs. alias entry · ~30 lines · **done**

**Source**: `xboxrecomp/tools/disasm/functions.py` --
`_build_alias_entries`, `_pass_seed_aliases`, and the `detection_method` field
carried on every `Function`.

**The defect.** `disasm32.find_functions` promotes a branch landing inside
another function's body into a function of its own, deliberately: the lifter
tail-dispatches to that address and needs a body there. It then writes the
result into the catalog in a field that says "function", identical to a real
function start. Nothing downstream can tell a genuine start from an address we
only made dispatchable.

Measured: **6,427 of 7,364 false positives were aliases** -- 87% of the reported
error, and the disassembler was never claiming a function started there.

**The fix.** `Function.entry_kind`, `"start"` or `"alias"`, set where the
aliases are created and emitted in the JSON. `score_recovery.py` holds aliases
out of precision, and reports them separately split by whether they land inside
a known function (expected) or outside every one (suspect). An alias landing on
a reference start is counted again on its own line: that is a real function
demoted, and a demoted start will not be reported as found.

The overlap between an alias body and its containing function stays. That is
the same trade xboxrecomp makes, and for the same reason its docstring gives:
the alternative is a stub that returns immediately, silently skipping the
epilogue and leaking the caller's frame.

**Verified, end to end on the oracle binary** (same reference, same scorer,
the new catalog a strict subset of the old -- 0 entries added, 302 removed):

| | baseline | after |
|---|---:|---:|
| function starts | 46,696 | 38,559 |
| split | 12,473 | 4,338 |
| invented | 844 | 842 |
| **precision** | 71.48% | **86.57%** |
| recall | 97.72% | 97.72% |
| **F1** | 82.57% | **91.80%** |

7,845 alias entries, and **every one of them inside a known function** -- zero
landed outside one, which is the result that says the category is real rather
than a convenient place to put the numbers that were embarrassing. Recall does
not move at all, so nothing was reclassified out of existence.

`score_recovery.py --selftest` asserts that an alias inside a known function
does not touch precision, that one outside every function is still counted, and
that an alias landing on a reference start reads as a demoted function rather
than a found one.

---

### 1. Candidate validation -- `engine.probes_as_*` · ~40 lines · **done**

**Source**: `xboxrecomp/tools/disasm/engine.py` -- `probes_as_function_body`,
`probes_as_prologue`, `probes_as_constant_stub`, `probes_as_vcall_thunk`,
`probes_as_returning_body`.

**The defect.** `disasm32.find_data_code_pointers` scans every byte offset of
every data section for a value in the code range and calls each hit a function.
Unaligned on purpose -- GTA1's handler table starts at `0x4B4AD1` and an
aligned-only scan misses it entirely -- but the price is that any four bytes
that happen to look like a code address become a function.
`find_branch_targets` has the same shape: a linear `E8`/`E9` byte scan that
cannot tell an opcode from the middle of an immediate.

**Scale, measured end to end, and smaller than any estimate of it.** The probe
rejects 302 data-pointer candidates on the oracle binary. Scored, that is 283
false positives removed -- of which **2** are invented and the rest are splits.
Invented false positives barely move: 844 before, 842 after.

An earlier draft of this document said 2,905; a later one said 617. Neither
survived the run. The 844 invented starts are real, but they do not come from
the data-pointer scan, so this is not what removes them.

It still lands, because 283 junk entries for forty lines at **zero measured
recall cost** is a good trade. It is simply not, and was never, the largest
thing wrong -- and it took three estimates and one end-to-end run to say that
honestly.

The comment in `find_data_code_pointers` says "false positives just become dead
functions -- harmless". That is true for the *build* and false for everything
else: it poisons the precision number, buries real misses in noise, and spends
codegen on garbage.

**The fix.** xboxrecomp does not guess harder, it *corroborates*. Before
accepting a candidate, decode forward from it and require the stream to reach a
`ret` or a tail jump without hitting bytes that will not decode. Data almost
never does; code almost always does. Read that docstring -- it records why
alignment was tried first and abandoned (a real MSVC function start is only
aligned when the linker had a reason to pad it) and why the instruction cap has
to be generous (one real function runs 537 instructions before its first `ret`,
and a cap of 512 rejected it).

The probe is deliberately *read-only*: it follows instructions the sweep already
decoded where they exist and decodes the rest without recording them, so running
it never changes what the sweep produced.

**Verify**: re-score Trespasser. The invented count should fall with recall
unchanged. If recall moves at all, the probe is too strict. The probe also
prints what it rejected, so its effect is visible without a second full run.
`disasm32.py --selftest` covers the decision boundaries: a prologue reaching
`ret` passes, a tail `jmp` passes, undecodable bytes fail, a clean decode that
runs off the section end without a terminator fails.

---

### 2. Jump-table resync · **not applicable -- measured, then dropped**

xboxrecomp needs this because its `linear_sweep` decodes straight through an
inline switch table and comes out the far side out of phase, eating the
function's epilogue. **pcrecomp cannot have that bug.** `disassemble_function`
is recursive descent from function starts only; it treats an indirect `jmp` as
a block terminator and recovers the arms through `jump_table_targets`, so it
never decodes a table as instructions in the first place. The architectures
differ and the fix does not transfer.

The evidence, since the queue below predicted otherwise:

- 2,417 functions scored a short end. 1,984 (82%) had nothing but padding in
  the gap and were already correct; see the reference-building note in
  `score_recovery.py`.
- Of the 433 real ones, 98% had a jump table at the cut, which looked like
  confirmation. It was not. Sampling 150 of them and asking *what branches into
  the gap* -- direct targets, conditional targets, and resolved table arms --
  the answer was **nothing, in 150 of 150**. The gaps are dword jump tables and
  the MSVC byte index maps that pair with them. Data. The function's own end
  simply excludes trailing switch data, which is the correct reading of "end of
  the last instruction".
- The one place a table could still hurt is `find_branch_targets`, which byte-
  scans for `E8`/`E9` and walks through tables. Gating it on
  `probes_as_function_body` would reject **1 of 844** invented false positives,
  so there is nothing there either.

Worth keeping the finding rather than the code: the short-end number is a
definition difference twice over, and after correcting for both, pcrecomp's
function ends are 95.56% exact against the map.

### 3. Function-end bounding · **attempted, measured worse, reverted**

Real defect, no net-positive fix found yet.

`func.end` is `max(block.end)`, and a function's blocks are not necessarily
contiguous: pass 1 follows an unconditional jump as internal control flow
whenever the target is within `0x100000` of the start -- a distance test
standing in for a structural one. So a **ten-byte thunk** at `0x0043F770`,
whose `jmp` lands 225 KB away, reports an end 943,579 bytes past its own start.
95 functions of 30,731 (0.3%) are wrong this way.

Three fixes were implemented and measured on a 2,000-function sample. All three
are worse than doing nothing:

| approach | exact ends |
|---|---:|
| **leave it alone** | **95.55%** |
| end at the first hole between blocks (strict contiguity) | 93.70% |
| ...allowing a 256-byte hole | 94.00% |
| ...4 KB | 93.95% |
| ...64 KB | 93.95% |
| ...256 KB | 95.25% |

Strict contiguity fixes 94 of the 95 and breaks about twice as many, because
**IDA's `end` is the end of a function's last chunk, not of a contiguous
range** -- IDA models functions as having distant tails, and `max(block.end)`
happens to model that better than any contiguous rule does. No gap threshold
recovers the loss; the sweep above is monotonic in the wrong direction.

The structural alternative -- refuse to follow a `jmp` whose target is itself a
known function start, which is what xboxrecomp's next-known-function bound
amounts to -- separates only **25 of the 95**. The other 70 jump into addresses
nothing else calls a function.

So the distance guard stays for now. A correct fix has to distinguish "a chunk
of this function" from "a tail call into another one", and neither distance,
nor gap size, nor the current candidate set answers that. It is a call-graph
question. Revisit after `rtti` and `vtable_scanner` improve the set of addresses
known to be function starts.

**Do not** simply widen the `0x100000` guard. That was nearly done while fixing
the E9 bug and would have merged every tail-called function into its caller.

### 4. `seed_from_log` · **done** · and the loop was open at both ends

**Source**: `xboxrecomp/tools/seed_from_log/`.

Ported as `tools/disasm/seed_from_log.py`. The `recomp32` runtime already
prints every dispatch it could not resolve, with the calling function:

    ICALL: unresolved VA 0x004A1C30 from 0x0048D4E0
    ITAIL: unresolved VA 0x004B2088 from 0x00401D10

A human was reading those out of a console and hand-editing a seed list.

Gates are xboxrecomp's, and they matter: an address the program *mentioned* is
not automatically a function. A garbage slot points at data just as easily, and
seeding data is worse than the missing target was -- it splits a real function
and can break the build, where the missing target only broke one dispatch. So a
candidate must lie in an executable section **and** pass
`probes_as_function_body`. That is item 1 paying for itself somewhere it was
not built for.

Three outcomes are reported separately, because they mean different things:
a new function, an alternate entry inside a known one, and -- loudly -- an
address the catalog **already contains**, which means the run failed to
dispatch a function that was found all along. That last one is a dispatch-table
bug and this tool cannot fix it; saying so is more useful than seeding it again.

**The other end was missing.** `find_functions` has taken a `seeds` argument all
along, with a docstring explaining that the PE entry point matters most --
nothing calls it, and a CRT startup need not open with `push ebp; mov ebp, esp`.
`main()` never passed any. So the tool wrote a seed file nothing could read, and
the entry point was never seeded.

Measured on charmap.exe, a stock Microsoft binary: **the entry point at
`0x0041D340` was not in the catalog at all.** Seeding it recovered it, and
dropped 48 spurious function starts -- mid-function addresses that had been
promoted to functions only because nothing owned them, every one of which is
still covered by a function in the new catalog. So: one real function gained,
48 false ones gone, nothing lost.

### 5. `rtti` · **done** · a symbol source, not a recovery one

**Source**: `xboxrecomp/tools/rtti/rtti.py`. Ported as `tools/cpp/rtti.py`,
which is where the other C++ RE helpers live.

32-bit MSVC RTTI uses plain VAs with none of the image-relative indirection x64
uses, and the original Xbox is 32-bit MSVC, so the structure parsing transferred
unchanged. Two things did not.

**Vtables are not only in `.rdata`.** The Xbox original scans `.rdata` and
`.data`, which is right for an XBE. On PE a linker may merge read-only data into
`.text`, and a modern MSVC one routinely does: charmap.exe keeps all four of its
vtables there. Restricting the scan to non-code sections found **zero** of them
while cheerfully reporting the four class names -- a failure that looks exactly
like "this binary has no vtables". Scanning every section costs nothing, because
the filter is a word equal to the address of a CompleteObjectLocator the image
actually contains, followed by an array of code pointers.

**Vtable slots point at linker stubs, not at functions.** MSVC's incremental
linker parks a table of five-byte `jmp` stubs at the front of `.text` and puts
*those* addresses in vtables, so relinking after an edit repoints one jump
instead of every reference. Trespasser is built that way: 2,230 of its 2,243
vtable slots are stubs.

That one indirection is the whole difference between a symbol source and noise:

| | agreement with the linker map |
|---|---:|
| vtable slot taken raw | 13 of 2,243 (**0.6%**) |
| followed one hop | 2,243 of 2,243 (**100%**) |

Validated against `trespass.map`, which is symbol-derived truth:

- **2,243 of 2,243** recovered virtual-method addresses are exact map function
  starts. Not "inside a function" -- exactly on the start, every one.
- **2,226 of 2,239** class attributions agree with the map's own mangled names
  (99.4%). The 13 that do not are inherited methods whose implementation lives
  in a base class that is not itself in the RTTI graph -- the case
  `owning_class` documents as unresolvable, behaving as documented.

**What it is worth, honestly.** As a *seed* source on Trespasser: nothing. All
2,243 methods were already in the catalog, because E9 seeding reaches them
through the stub table. As a *symbol* source: 448 classes, a full inheritance
graph, and 2,239 methods that stop being `sub_004A1C30` and start being
`CBloodSplats__004A1C30` -- in under three seconds, at 99.4% verified accuracy.
That is what makes lifted code readable and a crash stack worth reading.

Its seeding value needs a binary with RTTI *and* poor recall.

### A second binary, and the same answer

Star Wars: Force Commander (2000, MSVC 6, 3.9 MB of `.text`, STL-heavy C++ via
MSVCP60) was the obvious candidate for "RTTI finally adds functions": it is
larger than Trespasser in class count, it has no linker map, and its RTTI is
completely intact -- **567 classes, 1,121 vtables, 5,436 virtual methods**.

It does not add functions either.

| | |
|---|---:|
| RTTI virtual methods | 5,436 |
| already in the disassembler's catalog | **5,427** |
| not found by `disasm32` | **9** |
| ...of those, new functions rather than alternate entries | **0** |

All nine land inside a body the sweep had already decoded. On a binary with
34,674 recovered function starts and 98.8% byte coverage, RTTI's contribution
to *recovery* is nine alternate entry points.

This is now measured on two binaries that differ in almost every way that
should matter -- one with a linker map and one without, one 7.8 MB and one 3.9,
different studios, different C++ styles -- and the answer is the same both
times. **Treat `rtti` and `vtable_scan` as symbol sources, not recovery
passes.** The thing that finds the functions is E9 seeding plus the fixpoint,
and it was already finding them.

What Force Commander *did* get is names: 4,832 methods attributed to one of 567
classes, plus 738 functions attributed to a source file, on a stripped retail
binary with no symbols of any kind. 5,570 of 34,674 functions (16%) stop being
`sub_004A1C30`.

Note that Rise of Legends, the original argument for item 6, is explicitly
**not** the binary that would settle this: it has no RTTI at all.

### 6. `func_id/vtable_scanner` · **done** · validated against RTTI

**Source**: `xboxrecomp/tools/func_id/vtable_scanner.py`. Ported as
`tools/cpp/vtable_scan.py`.

Finds vtables structurally -- **runs of three or more consecutive code pointers
in a data section** -- so it works on the binaries `rtti.py` cannot help, which
is most of them. This is the principled version of
`find_data_code_pointers`: a *run* of code addresses is a far stronger signal
than an isolated word, which is why that scan invents so much and this one does
not.

Trespasser is a rare chance to check a heuristic against truth, because RTTI
describes its vtables exactly:

| | |
|---|---:|
| RTTI vtables with 3+ slots (findable) | 359 |
| found by the heuristic | **359 (100%)** |
| slots pointing at real linker-map functions | **12,655 / 12,907 (98.0%)** |

It also finds **216 vtables in `.rdata` that RTTI does not describe** -- classes
compiled without RTTI, and plain function tables -- and 85.3% of *their* slots
are real map functions too. So the extras are mostly real, not noise.

**Section choice did almost all of the damage, and then all of the good.**
Scanning every non-code section gave 1,104 candidates. Measuring what fraction
of each section's slots were real functions sorted them instantly:

| section | candidates | slots that are real functions |
|---|---:|---:|
| `.rdata` | 575 | **85.3%** |
| `.data` | 12 | 0.0% |
| `.idata` | 51 | 0.0% (it is the import thunk table) |
| `.rsrc` | 466 | 1.2% (resource blobs that look like addresses) |

`.rsrc` alone was 42% of all candidates and almost entirely noise. Excluding
sections whose contents are *defined to be something else* drops the count to
587 with no loss of recall.

Note the shape of that fix. The obvious move is to whitelist `.rdata` and
`.data` -- and that is exactly the assumption that made the RTTI port find zero
vtables in charmap.exe, which keeps them in `.text`. So this excludes by known
purpose and scans anything it does not recognise, with `--include-code` for the
merged-`.text` case.

**Known blind spot, measured rather than guessed:** 90 of Trespasser's 449 real
vtables have fewer than three slots and are structurally invisible here. Lowering
the threshold to two would find them and drown the result in ordinary data.

**What it is worth, honestly.** On Trespasser: nothing in recovery. Of 3,489
probe-gated method addresses, 3,462 are already in the catalog, and of the 27
that are not, 26 are alternate entries inside known functions and one is a
false positive. It recovers **0 of the 780** functions the catalog is missing --
because E9 seeding already reaches them.

Its case is the binary this cannot be tested on yet: Rise of Legends, 25,513
functions reachable only through vtables and **no RTTI at all**, which is why
this item was always separate from item 5 rather than superseded by it.

**Second measurement, Force Commander.** Where RTTI is available it can be used
as truth for the heuristic, which is what makes this worth re-running: the
scanner found 4,982 of RTTI's 5,436 methods (**91.6%**), short of the 100% it
scored on Trespasser because that run only counted vtables with three or more
slots and this one counts them all. It also proposed 688 methods RTTI does not
describe -- of which **109** were not in the catalog, so its recovery
contribution on a 34,674-function binary is at most 109 addresses, and it is
the more generous of the two passes.

### 7. `symbols/map_names` · **done** · the port got simpler and the heuristics got worse

**Source**: `xboxrecomp/tools/symbols/map_names.py`. Ported as
`tools/pe/map_names.py`.

`resolve` turns an MSVC linker MAP into `{va: name}`. **Simpler on PC than on
Xbox**: imagebld discards the preferred load address when it emits an XBE, so
the Xbox version reconstructs every address from section arithmetic. A plain PE
keeps it, and MSVC writes the answer into the MAP's own `Rva+Base` column.

Validated against Trespasser's `trespass.map`, cross-checked against the
*independent* parser in that project:

| | |
|---|---:|
| addresses the other parser found | 34,159 |
| agreeing | **34,159 (100%)** |
| names agreeing on those | **34,159 (100%)** |
| addresses only this parser found | 0 |

An earlier version of this entry reported 24 extra addresses as a bonus. They
were not. They were `__ehfuncinfo$` and `__ehhandler$` records whose Rva+Base
the linker never resolved, landing in the PE header -- and the exact-image-base
filter only caught some of them. See `drop_unplaced`: 1,377 such symbols on
Trespasser, 478 of them flagged as functions. With those gone the two parsers
agree exactly, on 34,528 function symbols and 34,159 distinct addresses, which
is what the other parser's own statistics report.

Finding extra results that the reference does not have should have read as a
warning rather than a win. It did not, until the merge step put `sub_00400008`
at the top of a name table and made it obvious.

`port` carries *library* names from a donor with a MAP onto a target without
one, by matching code byte for byte. Round-tripped -- Trespasser as its own
donor and target -- it applied 431 names and **431 of 431 match the MAP
exactly**. Pointed at charmap.exe it recovers 9 CRT functions
(`__SEH_epilog4`, `___report_gsfailure`, `___scrt_fastfail`, ...), all in one
contiguous CRT block.

### Three bugs, two of them the Xbox assumptions not holding

**The flags column is not one flag.** MSVC writes `f` for a function and `f i`
for an inlined one. A regex expecting a single optional `f` parsed 14,023 of
36,074 functions and dropped the other **20,983 -- 58% -- with no error at
all**. The symptom was a smaller number than expected, which is exactly the
kind of thing that gets rationalised rather than investigated.

**"Came from a library" cannot be inferred from `Lib:Object`.** The Xbox
version tests against a list of known XDK section names. The obvious PC
translation is "the column contains a colon", and it is wrong: Trespasser
builds its own engine as static libraries, so its MAP says
`Physics:BioModel.obj` and `AI:AIMain.obj` in the same shape as
`LIBCMT:printf.obj`. That filter accepted 33,054 of 35,006 functions, and
porting them onto charmap.exe named its functions after Trespasser's physics
engine -- `?apply_bc_and_locate_field_tracers@CBioModel@@AAEXH@Z`, applied to a
Windows character map, twice. Now a regex over known runtime names, overridable
with `--lib`, because this is a judgement the tool cannot make.

**A signature matching several target functions identifies none of them.** A
12-byte MSVC prologue is common enough to appear dozens of times in one image.
The donor-side ambiguity check the Xbox version has is necessary and not
sufficient; the same test is now applied on the target side, which dropped 672
further matches on the round trip. Conservative, and the reason precision is
100% rather than nearly so.

The identity check earns its place too. It fired immediately on the first run
-- and it was catching a bug in this port rather than a mismatched pair, since
a MAP's section "Start" column is an offset *within* the section while the PE
puts `.text` at RVA 0x1000. Resolving the entry point the wrong way lands
exactly one section alignment low, which is indistinguishable from a stale MAP.

### What it is worth

Unlike items 5 and 6, this one pays immediately: 34,183 real function names for
Trespasser, from a file that was sitting next to the binary. A MAP is the best
symbol source a binary can have, and games shipped them by accident constantly.

`port`'s yield is low by construction -- 9 functions into charmap -- because it
only claims what it can prove. That is the right trade for a symbol file, where
a wrong name is worse than no name: it is a lie you act on for weeks.

**Not covered: Borland and 16-bit maps.** Operation Neptune ships `ONWIN.MAP`,
but it is a Borland 16-bit map for `ONWIN.EXE` -- 28 segments, four-digit
offsets, already-demangled names with spaces in them (`operator delete(void
far*)`) -- and not for the `ONWIN32.EXE` the project actually lifts. A
different grammar and a different binary, so it is a separate piece of work
rather than a regex tweak.

### 8. `debug_symbols` · **done** · and the expected result was backwards

**Source**: `xboxrecomp/tools/debug_symbols/recover.py`. Ported as
`tools/pe/debug_symbols.py`.

An `assert` bakes `__FILE__` into the binary and references it from the function
containing it, so the strings say which source file each function came from.
Trespasser's *release* build carries 90 of them -- `AIMain.cpp`, `Brain.cpp`,
`WorldDBase.cpp` -- which is the usual story: some subsystem was built with
asserts on and nobody noticed.

The Xbox version consumes a `strings.json` with cross-references already
computed. pcrecomp has no such file, so the references are found here by
scanning each function's bytes for the address of a string, unaligned, because
`push offset aFoo` puts the address one byte into a five-byte instruction.

Result on Trespasser: **4,488 of 46,853 functions attributed (9.6%)** -- 410
observed directly, 4,078 filled by interpolation.

### The two passes are equally trustworthy, which is not what anyone expects

Trespasser's MAP records the object file every function was linked from, so the
attribution can be scored rather than admired. The obvious hypothesis is that a
direct `__FILE__` reference is near-certain and interpolation is the risky
guess. Measured:

| | agreement with the MAP |
|---|---:|
| direct (function references the string itself) | 267 / 276 (**96.7%**) |
| interpolated (bracketed by two agreeing neighbours) | 3,363 / 3,467 (**97.0%**) |

Interpolation is very slightly **better**. Not because it is clever, but
because a direct reference proves less than it appears to: it shows an assert's
`__FILE__` sits in this function's bytes, not that the function was compiled
from that file, and inlining a callee drags its asserts along with it.
Interpolation only fills runs whose two ends already agree, which turns out to
be the stronger filter.

An earlier draft of this entry asserted 99.5% for direct and 96.7% for
interpolated. Those numbers were written before the measurement and were wrong.
They are recorded here because inventing a plausible number for the thing you
have not checked is precisely the habit this whole exercise exists to break.

Both figures are good enough to use and neither is authoritative, so the output
records **how** each attribution was reached -- the same distinction
`entry_kind` draws for function starts, for the same reason: an observation and
an inference that look identical in a file will be treated identically by
whoever reads it.

The 3% that disagree are interpolation bridging an object-file boundary whose
two ends happened to match: `uiwnd.obj` ... `uidlgs.obj` ... `uiwnd.obj` fills
the middle with `uiwnd.cpp`. That is the failure this method has, and it is
bounded by how often a single file's functions are split around another's.

### What it is worth

It does not name a single function. It says which *file* 4,488 of them came
from, which turns an undifferentiated wall of `sub_004A1C30` into something
with a shape -- and tells you which functions are worth reverse-engineering in
one sitting, because they were written in one.

### 9. `conformance` · **assessed, then absorbed rather than ported**

**Source**: `xboxrecomp/tools/conformance/`, 2,532 lines. **Not ported.** The
two ideas worth having were lifted into `tools/lift/difftest.py`, which already
does the harder half -- assembling a snippet, running it through Unicorn,
compiling the lifted C, and comparing state field by field. Porting a second
harness beside it would have been duplication, not capability.

What difftest.py was missing, and now has:

**The x87 stack was never compared at all.** `g_st[8]` and `g_fp_top` existed
only so the generated C would link; nothing read them. Depth is the point --
a handler that pushes or pops the wrong number of times leaves every later
`st(i)` reading its neighbour, the values stay plausible for a long while
afterwards, and a comparison of values alone passes while the model is already
one slot out. Sixteen x87 cases now cover push, pop, the pop that `faddp` and
`fstp` do, load/store round trips, and integer conversion.

Depth on the hardware side comes from the status word's TOP field, which is a
rotating index starting at 0 on an empty stack and counting *down*, so after n
pushes it reads `(0 - n) & 7`. Values are not read from registers at all:
Unicorn's `ST0..ST7` return the 64-bit mantissa with the exponent and sign
dropped, and its `FP0..FP7` return `(0, 0)` on this build, so neither can be
turned back into a number. The cases store what they want compared into guest
memory instead, which difftest already compares byte for byte.

**The control word is pinned to PC=53.** The model holds the stack as C
doubles; the x87 default is 64-bit extended, which carries eleven bits the
model cannot.

### The pin had to be earned

Setting it and asserting it matters would have been the easy version. It does
not, for most cases: with the hardware left at extended precision, all fifteen
original x87 cases still passed, because a single operation followed by a store
to double survives the double rounding.

Searching random divisions, **about 1 in 4,000** does not:

    3855816586396013.0 / 2899998962399734.0
      PC=53  ->  1.3295924020625665
      PC=64  ->  1.3295924020625667

That is `fpu.double-rounding`, and it is the case that makes the control word
load-bearing rather than decorative: with the pin the suite is 16/16, and
reverting only the control word fails exactly that one. Rare enough to look
like a flake, common enough that a growing FPU suite would carry a couple of
permanent unexplained divergences without it -- which is how a suite starts
being scrolled past.

The depth comparison is mutation-tested too: reporting `g_fp_top + 1` from the
lifted side turns 15/15 into 15 failures, each naming the discrepancy.

Whole suite: **63/67 match hardware, 4 known divergences, 0 failures.**

### What was left behind

`xbe_run.py` (670 lines) is Xbox-specific. The `sse` case kind is not ported:
the lifter has MMX but the x87 stack was the gap that mattered, and SSE cases
need the same memory-capture treatment to be worth anything. The corpus runner
assumes a title library this repo does not have.

### 10. `abi_analysis` and `split` · **assessed, both declined**

**`abi_analysis` (544 lines): no consumer, and the answers it would give are
worse than the ones already available.** It infers calling convention, parameter
count and frame type heuristically. Every place pcrecomp needs that, it already
has an exact answer:

| where ABI matters | how pcrecomp answers it |
|---|---|
| lifted -> lifted | it does not. `typedef void (*recomp_func_t)(void)` -- arguments live on the simulated stack and the global-register model never needs a signature |
| import shims | `pe/stdcall_argc.py` reads the decorated `_Name@N` out of the SDK import libraries. That is the compiler's own computation, not an inference |
| the hybrid real -> lifted boundary | measured at runtime. `hybrid.h` returns `arg_bytes_cleaned` as `final_esp - initial_esp - 4`, which is what the callee's `ret N` actually popped |

Nothing in `tools/`, `docs/` or any project driver asks for an estimated
parameter count. Porting this would replace exact answers with guesses in the
one place -- the hybrid boundary -- where a wrong stack purge desynchronises
everything after it.

**`split` (482 lines): a workflow this repo does not have.** It emits one `.s`
per function with the bytes as `db` and the disassembly alongside as a comment,
so an undecompiled function still assembles to the original encoding. That is
the oracle a *matching decompilation* is built on, and pcrecomp does not do
matching decompilation -- it lifts to C that behaves the same, which is a
different goal with a different definition of done. Building the scaffolding
for a workflow nobody has asked for is how a toolbox gets heavy.

Both stay listed so they are not re-invented from scratch by someone who
assumes the gap was an oversight.

### 10b. What the queue actually turned up instead: nothing merged the names

Items 5, 7 and 8 each produced a symbol source, in three different JSON shapes,
and nothing combined them -- so a project would pick one and discard the rest.
Worse, `map_names.py`'s own docstring hands the sanitising job to "the naming
merge", which did not exist.

`tools/pe/merge_names.py` closes it: sources in precedence order, first claim
per address wins, and a report of what each contributed -- which immediately
showed `rtti` contributing **0** names to Trespasser, because the MAP already
covers every address RTTI knows. That is the tool earning its keep on the first
run: a source that adds nothing should be dropped, and now it says so.

The real work is sanitising. A MAP gives `_SmackOpen@12` and
`??0CColour@@QAE@HHH@Z`; none of it is a C identifier. Overloads must stay
apart, so the signature is kept -- `f_QAE_XZ` and `f_QAE_HH_Z` are ugly and
correct. But `@` and `_` both sanitise to `_`, so symbols differing only in
separator collide, and on Trespasser **4,217 of 53,242 names (8%)** needed
their address appended to stay unique. Letting one silently win produces C that
does not compile, or compiles and calls the wrong function.

### 11. Tests

`xboxrecomp/tools/disasm/` ships six test files -- `test_cc_boundary`,
`test_decode_at`, `test_function_end`, `test_gap_prologue`, `test_imm_refs`,
`test_vcall_thunk`. `pcrecomp/tools/disasm/` ships none; the only executable
checks in this repo are two `--selftest` flags and three C self-tests under
`runtime/`.

Port the tests *with* each item above rather than as a batch. A ported heuristic
without its test is a heuristic nobody can safely change later.

---

## What is not on the list

**Not candidates, deliberately:**

- `xbe_parser`, `xiso`, `xmv`, `kernel_audit` (~2,800 lines) -- genuinely
  Xbox-specific. They stay where they are.
- `fusion` and `ghidra_naming` -- assessed later; `fusion` is tied to a corpus
  we do not have.
- pcrecomp's `ne/` (1,756 lines) -- 16-bit New Executable support. Irrelevant to
  Xbox, stays put.

**Flowing the other way (pcrecomp -> xboxrecomp):** `pe/` (1,605) if xboxrecomp
ever wants to look at a title's PC port alongside the XBE; `ida/` (436), since
xboxrecomp has a Ghidra path and no IDA one; `drm/` (320) and `assets/` (924),
which solve PC-era problems Xbox does not have. The Quake-family classification
experience in `classify/` (1,600) overlaps `func_id`, and the better parts of
each should merge rather than one winning.

---

## Status

- [x] `disasm32.find_branch_targets` -- E9 tail-call seeding, ported from the
      behaviour in `xboxrecomp/tools/disasm/xrefs.py`. Measured on an 8.8 MB
      binary with a linker map: recovers 6,550 of 7,331 missed functions.
- [x] 0. `entry_kind` -- function start vs. alias entry, with the scorer
      taught to stop counting a design decision as a defect. 87% of the
      measured "false positives" were this.
- [x] 1. `probes_as_*` candidate validation -- `probes_as_function_body`,
      gating the data-pointer scan, with a self-test.
- [x] Range scoring in `score_recovery.py` -- function ends, not just starts,
      which items 2 and 3 needed and nothing had. Baseline: 95.56% exact.
- [~] 2. `resync_jump_tables` -- **not applicable.** pcrecomp's recursive
      descent never decodes through a table. Measured: nothing branches into
      any of 150 sampled short-end gaps; they are switch data.
- [~] 3. Function-end bounding -- **real defect (95 functions), every fix
      measured worse than leaving it.** Reverted. Needs a call-graph answer,
      not a distance or gap one.
- [x] 4. `seed_from_log` -- ported, plus the `--seed-functions` end that was
      missing. Found that the PE entry point was never seeded: on charmap.exe
      it was absent from the catalog entirely.
- [x] 5. `rtti` -- ported to `tools/cpp/rtti.py`. 100% of recovered method
      addresses are exact linker-map function starts, 99.4% of class
      attributions agree with the map. Worth names, not new functions.
- [x] 6. `func_id/vtable_scanner` -- ported to `tools/cpp/vtable_scan.py`.
      100% recall against RTTI's own vtables, 98.0% of slots are real map
      functions. Section filtering, not the run heuristic, was what mattered.
- [x] 7. `symbols/map_names` -- ported to `tools/pe/map_names.py`. 100%
      agreement with an independent parser on 34,159 addresses and names;
      `port` round-trips at 431/431. Borland/16-bit maps are not covered.
- [x] 8. `debug_symbols` -- ported to `tools/pe/debug_symbols.py`. 4,488 of
      46,853 Trespasser functions attributed to a source file at ~97% accuracy
      against the MAP. Direct and interpolated attribution are equally
      accurate, which contradicted the expectation.
- [x] 9. `conformance` -- assessed, and absorbed into `lift/difftest.py`
      rather than ported. x87 stack depth is now compared (it never was), the
      control word is pinned to PC=53, and 16 x87 cases exist to exercise both.
      SSE cases and the Xbox corpus runner were left behind.
- [~] 10. `abi_analysis` -- **declined.** No consumer, and pcrecomp already
      has exact answers where ABI matters (SDK import libs, runtime esp delta).
- [~] 10. `split` -- **declined.** It is the oracle for a matching decomp, and
      this repo does not do matching decomps.
- [x] 10b. `merge_names.py` -- the gap items 5/7/8 actually left: three symbol
      sources, three shapes, nothing combining or sanitising them.
- [ ] 11. Disassembler tests, ported alongside each item

**Still not a shared repo.** A `pcrecomp-core` extraction is a conversation for
after items 1-6 have landed and held, not before.
