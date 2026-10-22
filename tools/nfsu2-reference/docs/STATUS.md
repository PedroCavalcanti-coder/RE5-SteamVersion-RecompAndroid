# Status

## Update 2026-10-07 (evidence from this day's runs)

| Milestone | State | Evidence |
|---|---|---|
| M9 EA logo / intro | **DONE** | movies play; the logos are not skippable in the original either (input reaches the pad queue as event 0x26) |
| M10 main menu | **DONE** | menus navigate with Enter (synthetic DirectInput events, `NFSU2_TAP`) and real keys (user) |
| Car rendering | **DONE** | career-menu 350Z renders correctly (window capture of the recompiled build) after patch 0009 |
| Race loading | **DONE** | attract-mode race loads and runs, 5,400+ frames, no crash (was a null read in 0x005E5110) |
| M11 garage / M12 player-driven race | IN PROGRESS | user reports cars visible and racing; an end-to-end scripted race is not yet in the test set |
| Audio | **DONE** | stutter fixed by the fair FIFO machine lock (patch 0008) |
| Performance | open | user reports menu lag at times; profiler available (`NFSU2_PROFILE=1`) |

Root causes fixed today: MSVC /O1-/O2 miscompile of signed tests on
left-aligned 16-bit lazy flags (found and diagnosed by the project owner,
patch 0009); x87 TOP missing from `fnstsw`; unfair machine lock (audio);
missing per-thread xmm/mxcsr/flags in native32 (patch 0011); DirectInput
needing the host HINSTANCE; guest resources (D3DX effects) not found by
Windows. Tools added: windowed mode, `NFSU2_BACKGROUND`, `NFSU2_TAP`,
`NFSU2_NATIVE` (original code as oracle), `NFSU2_NATIVE_RANGE` (hybrid
bisection), real-function difftest, sampling profiler.

## Earlier entries


Last update: 2026-10-06. Only what has evidence is marked DONE.

| Milestone | State | Evidence |
|---|---|---|
| M0 C generated | **DONE** | `run_lift.py lift`: 27,742 functions, 70 files, 8.9 M lines, 0 lift failures |
| M1 C compiles | **DONE** | CMake + Ninja + MSVC 14.51 x86 `/O1`: all 70 TUs, 0 errors, 4 min 20 s with -j16 (largest TU 207 s) |
| M2 executable links | **DONE** | `build/NFSU2-Recompiled.exe`, 115 MB, 0 unresolved symbols |
| M3 CRT initializes | **DONE** | trace: `GetVersionExA`, `HeapCreate`, `TlsAlloc`, `GetStartupInfoA`, environment and locale setup, `_initterm` run through lifted code; `SetUnhandledExceptionFilter(0x766A55)` |
| M4 original entry point runs | **DONE** | `entering original entry point 0x0075BCC7`; 249 imports bound to Windows, 3 shimmed, 0 unresolved |
| M5 WinMain | **DONE** | the game's main (0x580E00 → 0x57ED10 → 0x5B7690) runs: single-instance check (Toolhelp process walk), D3DX `DisablePSGP` registry probe, CPU detection, registry settings (`Install Dir`, `Language`…), the multimedia-timer thread (callbacks Windows → lifted code via `timeSetEvent`, ~257/s), the file-system thread |
| M6 game window | **DONE** | `RegisterClassExA` + `CreateWindowExA("GameFrame", "NFS Underground 2")` + `ShowWindow`; the guest WndProc `0x5CCD60` receives messages through native32's callback path |
| M7 D3D9 initialization | **DONE** | `Direct3DCreate9(32)`, `CreateDevice` → `S_OK` (640x480 X8R8G8B8 fullscreen, D24S8) |
| M8 first frame | **DONE** | `Present` → `S_OK`; 1,320 frames in one run, ~496 D3D calls and 14 draws per frame |
| M9 EA logo / intro | **DONE** | the intro movies play (seen by the user on screen); 34 vertex + 34 pixel shaders compiled from the exe's effect resources |
| M10 main menu | IN PROGRESS | movies cannot be skipped: input issue below |
| M11 garage, M12 race | not started | |

## Resolved: the game asked for Disc 2

- **Where:** `0x005C0D30` (called from `0x005B76B3` in the game's startup at `0x005B7690`) shows `MessageBoxA("Please insert Disc 2", "NFS Underground 2")`; cancelling calls `exit(0)` (`0x0075D5A4`).
- **Why:** `0x005BF450` walks drive letters and accepts only `GetDriveTypeA == DRIVE_CDROM` holding the game files; a global at `0x0079DC60` that would also accept other drive types is 0 in the file and is never written by any code. The remaining bypass is the existence of a file named `foobar` in the game folder (`0x0057CAC0("foobar")`), a developer switch.
- **Evidence that this is the game, not the recompilation:** the **original** `SPEED2.EXE` of this installation, started on the same machine, shows the same `#32770 "NFS Underground 2"` dialog after 10 s (oracle run on 2026-10-06; nothing was modified). The recompiled and original binaries agree up to this point.
- **Resolution (owner's decision):** the owner's installed copy has no disc drive available, and the owner asked to bypass the check. The game's own developer switch is used: an empty file `foobar` was added to GAME_ROOT. Nothing in the game or in the recompilation was patched.

## Current issue: keys do not skip the movies

- Mouse: `IDirectInput8::CreateDevice(GUID_SysMouse)`, `FOREGROUND|EXCLUSIVE`; works while the window has focus, `DIERR_INPUTLOST` / `E_ACCESSDENIED` otherwise (expected).
- Keyboard/pads come from `IDirectInput8::EnumDevicesBySemantics` (DirectInput action mapping): the device is handed to the guest callback `0x5CA660`. `BuildActionMap` succeeds for 4 action formats and fails with `E_INVALIDARG` for 3; `SetActionMap` returns `DI_SETTINGSNOTSAVED`; `Acquire` and `GetDeviceData` then succeed. Whether `GetDeviceData` ever returns events, and what the 3 failing action maps are, is the next thing to measure.
- `WM_KEYDOWN` reaches the guest WndProc, which only uses it for the debug console (keys 0x23-0x7B), so window messages are not the game's input path.

## Viability (lifter)

| Metric | Result |
|---|---|
| `.text` bytes | 3,676,913 |
| Functions recovered | 27,742 (incl. 4,599 aliases) |
| Real code coverage | ≈ 99.9 % (uncovered bytes are padding and pointer tables; ~2.1 KB unexplained) |
| Lifted functions | 27,742 (1 replaced by a documented override, 1 excluded as data) |
| `RECOMP_UNIMPL` sites | 2,907: 2,890 3DNow! (unreachable: CPUID reports no 3DNow!), 17 `int` in data/unreachable FDIV-workaround tables |
| Compile errors | 0 |
| Link errors | 0 |
| ICALL / ITAIL sites | 13,271 / 3,126; **0 unresolved at runtime so far** |
| difftest (lift32 + simd32 vs Unicorn) | 246 / 246 |

**Classification: GREEN** for the lifter: complete coverage, everything lifts
and compiles, differential tests pass, and the recompiled code reproduces the
original's behaviour as far as it has been run.

## Runtime issues found and fixed during bring-up

1. The guest range 0x400000–0x932000 was taken before `main` (first by a 64 MB
   main-thread stack, then by NLS/locale mappings and heaps). Fix: the host
   relaunches itself suspended and reserves the range in the child with
   `VirtualAllocEx` before the child's loader runs; the game runs on its own
   256 MB-stack thread.
2. lift32 emitted no `RECOMP_BACKEDGE`, so a lifted spin-wait could starve other
   guest threads under the machine lock (pcrecomp patch 0005).
3. The Cyrix probe at 0x6F5FC5 depends on DIV's undefined flags → documented
   override returning "not Cyrix".

## Known open issues

- `LoadIconA(0x400000, …)` returns NULL: Windows does not know the manually
  mapped guest module, so resource APIs on it need a shim (icon, and the 23
  RT_RCDATA resources).
- C++ exceptions thrown by guest code reach the native `RaiseException`, and
  Windows will not find the guest's handlers (they live in the simulated TIB).
  Not hit yet.

## Next steps

1. Pass the disc check legitimately (owner's disc), then reach `CreateWindowExA` (M6).
2. Shim `LoadIconA`/`FindResourceA`/`LoadResource`/`SizeofResource`/`LockResource` for the guest HMODULE.
3. D3D9 trace (`D3D_TRACE=summary|full`) around `Direct3DCreate9` and the device vtable → `docs/D3D9_USAGE.md`.
