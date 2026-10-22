# Android runtime 0.7.0 — 2026-10-08

Continues the [resolution/input work](ANDROID_RESOLUTION_INPUT_TEST.md)
and [memory/filesystem adaptation](ANDROID_PROFILES_MEMORY_TEST.md).

## Runtime changes

- One persistent WinMM timer worker replaces allocating a native thread and
  guest stack for every timer. One-shot and periodic timers retain cancellation
  and synchronous retirement. Startup checks cover retirement and worker reuse.
- Android's text dialog sends paced keyboard press/release events to the real
  frontend action map. Replacing the default name sends backspaces first.
  The game received the full name `paulo`; the earlier `NOMBREp` result came
  from injecting characters faster than the frontend consumed them.
- `GetFileType` recognizes opened game files as disk files. The original CRT
  queried this immediately after opening a profile for writing; returning an
  invalid-handle error made it close the new file before `WriteFile`.
  The filesystem device test checks disk/console/invalid/closed handles.
- FILETIME conversion supports the runtime's virtual UTC timezone, with
  calendar/millisecond output and invalid input checks.
- A bounded decoder handles the generated x86 callbacks the game's audio
  code generates in its guest heap. It invokes existing lifted functions;
  x86 bytes are never executed by ARM64. Longer routines and 32-bit stack
  cleanup immediates are supported, together with context reloads from the
  shifted stack, signed min/max branches and 32-bit multiply/add/subtract.
  A synthetic long callback checks 300 cdecl calls, signed-clamp outcomes,
  arithmetic overflow, stack cleanup and guard bytes.
  Other generated instruction patterns remain explicit unsupported boundaries.
- Critical-section deletion is deferred while occupied. Waiting threads retain
  ownership of the native mutex object across scheduler handovers. Owner checks
  prevent unlocking from another thread; reinitializing an occupied section
  remains an explicit boundary. Startup checks cover recursion, guards,
  deferred retirement and reinitialization after retirement.
- Interactive Android sessions no longer stop at the five-minute bring-up
  deadline. Explicit lifecycle cancellation still stops the guest. Host smoke
  tests retain their deadline.
- The activity keeps the screen awake while playing and only shows touch
  popups while in the foreground.

## Device evidence

The connected Samsung SM-S938B / Adreno 830 rendered the Spanish title screen,
main menu, car preview and quick-race mode selection at 1170×540. The launcher
also previously created a 2340×1080 backbuffer successfully.

The original failed profile attempt left a zero-byte `paulo` file. That test
file and its empty directory were removed. A later profile is 54,966 bytes at
`/storage/emulated/0/nfsu2/AppData/Local/NFS Underground 2/paulo/paulo`.
A fresh game process opened it for reading and the menu displayed `paulo`.
The user's profile data is outside Git.

Local evidence is in the parent workspace's `work/android-paulo-save-success.log`
and `work/android-profile-step1.png`. Earlier failures are recorded in
`work/android-profile-save-failure.log` and `work/android-resolution-crash.log`.

Circuit selection reached a longer generated callback at `0x10fcfe34`, exceeding
the earlier decoder budget. The extended decoder passed this point and opened
Resort Loop's track selection, race options, Peugeot 206 selection, automatic
transmission and the race loading screen. Loading then reached a context reload,
signed clamp, multiplication and subtraction in generated audio callbacks.
After adding these operations, the connected phone loaded Resort Loop and
rendered the countdown, race HUD and moving opponents. The user confirmed
that the race started. A complete race and player acceleration remain pending
validation; the first unoptimized race measured 2.5–2.7 presentation FPS.
Local evidence: `work/android-first-gameplay.log` and
`work/android-gameplay-drive.png` in the parent workspace.
Changing resolution inside the original
display menu previously stopped at an invalid D3D9 COM object; detailed
object diagnostics are now present, and that path is still under investigation.
The subsequent optimized runtime and controls build fixes the black surface
on Android resume and the driving action-map switch; see the newer
[gameplay and controls report](ANDROID_GAMEPLAY_CONTROLS_TEST.md) for evidence.

Presentation-call FPS logs include movie/frame reuse and are not a measurement
of unique video frames or complete-race performance. The user still reports
lag. Achieving 120 FPS remains a future profiling and optimization goal.
The tested Vulkan driver advertises 1.3; Vulkan-1.1-only hardware remains
unverified. Full widescreen HUD/FOV adaptation and complete input/audio
coverage remain unfinished.
