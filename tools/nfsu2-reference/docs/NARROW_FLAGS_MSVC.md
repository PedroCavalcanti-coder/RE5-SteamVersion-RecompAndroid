# Signed narrow flags: MSVC regression

2026-10-07, Windows x86, MSVC 19.51.36257.

The existing crash reports `crash_20261007_113940.txt` and
`crash_20261007_114807.txt` both show a null read in `sub_005E5110`.
Symbolization and host disassembly locate the latter at the translated guest
instruction `0x005E5292` (`mov edx,[ecx+0x10]`), after a failed list lookup.

Earlier in this function, `test bx,bx; jle` at `0x005E5223`/`0x005E5226`
must skip zero and negative 16-bit identifiers. The generated C captures
each operand as `(uint32_t)(uint16_t)value << 16` and uses a signed test of
the AND result. MSVC `/O1` instead emits a zero-extended word test followed
by `je`, omitting the negative case. The crash has `BX=0xFFFF`; the invalid
identifier proceeds into the list lookup and reaches the null read.

A minimal independent reproducer also misclassifies `0x8000` and `0xFFFF`
with this compiler. This is an observed optimization discrepancy, not an
inference based solely on the crash address.

The runtime workaround tests the sign bit explicitly and uses unsigned,
sign-biased ordering for signed comparisons. It changes no game data and
does not bypass the failed lookup or substitute game functions.

## Reproduction and verification

`tests/narrow_flags.c` contains no game code. Compile in an MSVC x86 environment:

```sh
source scripts/vsenv.sh x86
cl -nologo -O1 -I../pcrecomp/runtime/recomp32 \
  -Fe../work/narrow_flags.exe -Fo../work/narrow_flags.obj tests/narrow_flags.c
../work/narrow_flags.exe
```

The baseline pcrecomp header produces 32,768 failures with `/O1`. After
`patches/pcrecomp/0009-recomp_types-explicit-sign-tests-for-signed-conditio.patch`, the
test produces zero failures with `/Od`, `/O1` and `/O2`, covering every
16-bit value for sign/zero tests and 458,752 signed comparisons per run.
Separate non-inlined condition functions matter: combining all flag results
inside one function can hide this optimization discrepancy.

The patch is part of the `git am` series in `patches/pcrecomp`.

Independent re-check (2026-10-07, MSVC 19.51, same test): original header
0 / 32,768 / 32,768 failures at /Od / /O1 / /O2; patched header 0 / 0 / 0.
After the fix the recompiled build reaches a race (the attract-mode demo)
without the 0x005E5110 crash and renders the career-menu car correctly,
both verified on window captures of the recompiled build.

## Game smoke test after the fix

- Full CMake/Ninja x86 build completed and linked successfully.
- `logs/run_20261007_120642.log`: background test with Enter actions at 25,
  32 and 40 seconds. The process stayed alive for the 55-second test interval
  and was then stopped deliberately by the test harness.
- No crash report or NaN/Inf shader-constant diagnostic appeared in that run.
  The game reached 4,462 presented frames, with later frames containing up to
  2,048 draws and about 60,521 D3D9 calls per frame. Additional real input
  events appear in the log during the user's interaction.
- The user confirmed that this build looks correct, while reporting some lag.
  This supports visual improvement; a full race and separate professional/quick
  race regression still need explicit end-to-end verification.

No texture/material patch was needed for the visual improvement observed by
the user. Startup BeginStateBlock failures still occur in the log; they were
not eliminated by this fix. Frame call volume makes bridge/locking overhead
worth profiling, but it is not yet a measured attribution of the lag.
