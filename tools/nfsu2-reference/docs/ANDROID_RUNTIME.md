# Portable guest integration

Current status: [0.4.0 device report](ANDROID_INPUT_AUDIO_TEST.md). Portable
heaps, serialized guest threads, TLS, files/resources, virtual Windows windows,
D3D9/DXVK, initial DirectInput and PCM/AAudio services are implemented. The
original initial splash renders on Android. Menu, races and complete API/lifecycle
coverage remain unfinished. Sections below record the earlier first connection.

The Android target now compiles and links the complete locally generated
`recomp_*.c` sources and dispatch table into `libnfsu2_android.so` for AArch64.
It uses a separate portable host, not Windows `native32` or host x86 execution.

## Implemented connection

- PE32 parser copies headers/sections from Internal storage/nfsu2/SPEED2.EXE
  into the reserved guest address space and validates the expected image layout.
- Guest virtual addresses remain at 0x00400000. `ADDR` adds the host mapping
  offset; RECOMP_FLAT_MEMORY and RECOMP_NATIVE_RANGE are not enabled.
- IAT slots receive synthetic 32-bit addresses for DLL-qualified HLE imports;
  they never contain ARM64 function pointers.
- Generated dispatch resolves guest functions. Unsupported indirect calls,
  instructions and imports produce a named integration boundary.
- Existing documented manual overrides are reused from src/runtime/overrides.c.
- Complete integer, segment, x87, MMX, XMM, MXCSR and published-flags snapshots
  protect explicit guest calls; a recursive machine lock serializes them.
- Initial 16 MiB guest stack and TIB, with SEH-list sentinel/stack bounds/self.
  This is single-thread startup bring-up; guest thread creation, fair yielding
  and guest SEH execution are not implemented yet.
- Android-specific generated-C adapter supplies a monotonic virtual 1 GHz
  RDTSC and a virtual CPUID contract. It advertises only the selected legacy
  x86/MMX/SSE/SSE2 subset and no 3DNow, AVX or SSE3.
- The first HLE implementations answer GetVersionExA (XP guest environment)
  and GetModuleHandleA(NULL). HeapCreate and other services stop explicitly.
- Startup exercises the real lifted sub_00401000 copy routine over seven
  lengths, checking output, buffer boundary, cdecl cleanup and caller state.
  It then calls the actual generated CRT entry 0x0075BCC7 until the first
  unsupported HLE operation. No replacement game loop or mock WinMain.

## Build / validation

`android/gradlew.bat assembleDebug` now requires locally generated game C and
produces version 0.2.0-runtime. The first full ARM64 compilation completed in
3m35s. Host sources compile with warnings treated as errors; generated sources
use -O0, -fno-strict-aliasing, -fexceptions and disabled FP contraction.

`tests/android_runtime` builds the exact same memory/PE/dispatch/callback
implementation as a headless Linux 64-bit executable. It is a useful additional
host check, not a substitute for device ARM64 or Vulkan testing:

```sh
cmake -S tests/android_runtime -B /tmp/nfsu2-runtime-check -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build /tmp/nfsu2-runtime-check -j2
/tmp/nfsu2-runtime-check/runtime_check '/path/to/SPEED2.EXE'
```

The headless test passed on Linux x86-64: 252 imports bound, all 27,742 functions
linked, seven real lifted-copy cases passed, and actual CRT entry called
GetVersionExA and GetModuleHandleA before reaching HeapCreate from 0x007620A1.
This verifies 32-bit guest offsets on a 64-bit host and the initial call/return
path. After the phone was reconnected, version 0.2.0 was installed and executed
on the Samsung SM-S938B / Adreno 830. The ARM64 test reproduced the same 252
imports, seven lifted-copy cases and CRT boundary at HeapCreate / 0x007620A1.
Home/return resumed Vulkan presentation; the resumed segment reached 600 frames
without an observed app-specific crash. The status overlay was visually
verified. Device evidence: workspace work/android-runtime-device-test.log and
work/android-runtime-device.png. The older report describes bootstrap 0.1.0.

Use `bash tests/android_runtime/run.sh [path/to/SPEED2.EXE]` for reproducible
host checks. The script keeps its build in the user's cache directory and runs
the test in the same session. Runtime evidence is stored locally in workspace
`work/android-runtime-host-test.log`.

The current runtime APK includes binary-derived generated game code, unlike
the earlier bootstrap. It is a local artifact from the owner's copy; source
generation and build products remain gitignored. No game data are embedded.

## Next boundaries

Implement guest heaps/handles, TLS, environment and CRT services according to
the measured startup call sequence; retain strict failures instead of no-op
imports. Then add files, timers and guest threads, Android window/input/audio
services, and a COM/D3D9 adapter feeding the Vulkan 1.1 renderer. The Vulkan
backend currently presents its diagnostic clear only; guest draw calls are
not connected. Game execution must move to a controlled worker before it can
run indefinitely alongside Android lifecycle events.
