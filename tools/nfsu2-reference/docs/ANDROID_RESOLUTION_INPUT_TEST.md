# Android runtime 0.6.0 — 2026-10-08

Includes [0.5.0 memory protection and profile storage](ANDROID_PROFILES_MEMORY_TEST.md).

## Resolution selection

The launcher now offers the active Android display resolution, 75%, 50%, and
1280×720. Percentage choices preserve the screen aspect ratio, rounded to
even dimensions. The selection persists and is passed with the selected
language to each fresh game process. The scrollable launcher accommodates
the additional controls.

For the pinned owner-supplied PE, the original resolution function at
`0x005bf610` is intercepted at its lifted function entry on Android. This
handles direct calls and indirect calls, retains stdcall stack cleanup and
returns the selected dimensions through the original guest pointers. The
game then creates its own backbuffer and dependent resources at that size.
The WSI exposes the selected mode alongside existing desktop modes. The
surface/display size and render resolution remain separate.

The owner added `fixwidescreen` with its Windows ASI loader, module, INI and
HUD-position data. These were inspected as references, along with upstream
[Resolution.ixx](https://github.com/ThirteenAG/WidescreenFixesPack/blob/master/source/NFSUnderground2.WidescreenFix/Resolution.ixx).
The Android implementation supplies its own resolution selector. It does
not load those x86 DLL/ASI binaries or apply the entire INI. The fix's FOV,
HUD-position, cinematic scaling and other Windows hooks still need native
adaptation. Selecting a wide resolution alone does not certify correct HUD
or camera geometry.

## Keyboard action mapping

- `EnumDevicesBySemantics` invokes the real lifted callback with ANSI device
  information and 32-bit COM pointers. Enumerated system devices have a
  nonexclusive background configuration. Force-feedback and secondary-device
  requests enumerate no unsupported devices.
- Keyboard `BuildActionMap` assigns direct keyboard semantics to scan-code
  objects, retaining application mappings. `SetActionMap` installs the
  mapped DWORD offsets, buffer size and application event values.
- Buffered keyboard input records both press and release, sequence and
  timestamp. `GetDeviceData` supports 16/20-byte records, peek, consume,
  count limits, overflow reporting and application data. Brief touches
  survive between guest polls. Pause/input clearing records releases.
- Buffered mouse data, mouse action maps, genre-specific controller
  mappings, persistent rebinding and force feedback remain unfinished.
  Forced action-map saving returns unsupported.

On the phone, the real game enumerated its 53-action format and built its
52-, 32-, 19- and 9-action keyboard maps. It installed the 128-byte frontend
format. Earlier builds returned unsupported for semantic enumeration and
then for buffered data; those boundaries have now been implemented.

## Validation

- `assembleDebug` passed and the version 6 ARM64 APK was installed.
- The connected SM-S938B created a real D3D9 backbuffer at **2340×1080**,
  with `CreateDevice` result `00000000`; its original movies rendered.
- An explicit **1170×540** launch also created its D3D9 backbuffer successfully.
- Guest resolution checks passed for output values, guard bytes and stdcall
  cleanup. Keyboard action checks passed for Escape/Enter DWORD state,
  unmapped actions, release and guards. Buffered checks passed for offsets,
  application data, peek, consumption, overflow and ordered release.
- API 26–29 now requests read and write storage permissions, as profile
  creation needs both. This permission branch has not been tested on an
  older physical Android device.

Device evidence is local to the parent workspace:
`work/android-resolution-native-test.log`,
`work/android-resolution-native-test.png`,
`work/android-resolution-input-test.log`,
`work/android-resolution-input-test.png` and
`work/android-resolution-build.log`.
APK: `build/android/NFSU2-AndroidEvolved-runtime.apk`.

The runtime still has a 90-second bring-up deadline. A complete playable
menu, races, saving/loading, lifecycle recovery and Vulkan-1.1-only hardware
compatibility are not certified by these checks.
