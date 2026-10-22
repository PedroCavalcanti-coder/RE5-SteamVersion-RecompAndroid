# Android runtime 0.5.0 — 2026-10-08

This report supersedes the startup boundary in
[the 0.4.0 input/audio report](ANDROID_INPUT_AUDIO_TEST.md).
The new APK continues the original startup movies on the physical Samsung
SM-S938B / Adreno 830. It is still a development build; complete menus,
races and game save/load are not certified.

## Implemented

- `VirtualProtect` updates committed guest pages, reports the previous
  protection and makes host pages read-only, writable or inaccessible.
  Guest execute permissions remain metadata: x86 bytes are never executed
  as ARM64 instructions. `IsBadWritePtr` now checks write permissions.
  Recommitting a heap block preserves protections of existing shared pages.
  Guard/cache modifiers, virtual reservations and decommit remain incomplete.
- `SHGetFolderPathA` redirects LocalAppData to
  `C:\nfsu2\AppData\Local`, mapped to
  `/storage/emulated/0/nfsu2/AppData/Local`. The original game creates its
  `NFS Underground 2` subfolder there. Roaming AppData and Documents have
  separate contained mappings. Unsupported folder IDs return an error.
- `CreateDirectoryA`, `GetFullPathNameA` including its file-part pointer,
  `FindFirstFileA`, `FindNextFileA` and `FindClose`. Searches are
  case-insensitive, support basic `*`/`?` patterns and `*.*`, return the
  32-bit ANSI find-data layout and signal end-of-search/invalid handles.
  DOS wildcard corner cases and short 8.3 names are not implemented.
  Unix ctime substitutes for creation time; access/write timestamps use stat.
- Synchronous `WriteFile`, `SetEndOfFile` and `FlushFileBuffers`, backed
  by real file writes, truncation and fsync. Overlapped I/O stays an explicit
  unsupported boundary. Full Windows sharing/security semantics remain open.
- The bring-up time limit is now 90 seconds so the original movies can
  advance beyond the old 30-second smoke-test cutoff.

All mapped paths remain inside the user's `nfsu2` directory. Traversal and
symlink escapes are rejected. Game data and generated game code are not
included in Git.

## Checks

- ARM64 `tests/android_memory` passed on the phone: unaligned page crossings,
  previous flags, read-only/read-write/no-access transitions, recommit
  preservation, untouched data, invalid flags, wrapping addresses and
  ranges that include uncommitted pages. Device page size: 4096 bytes.
- ARM64 `tests/android_files` passed on the phone in an isolated temporary
  directory: folder mappings, case-insensitive names, wildcard enumeration,
  find-data offsets, directory creation, full-path sizing and file-part
  pointers, write/seek/truncate/flush persistence and path escape rejection.
  Fixtures clean up their own temporary files.
- `assembleDebug` completed successfully and the version 5 APK was installed
  on the connected device. The selected guest language remains Spanish.
- The real guest passed the former `VirtualProtect`, `SHGetFolderPathA`,
  directory creation and full-path boundaries. It opened `ealogo.vp6`,
  `THX_logo.vp6` and `PSA.vp6`; a device screenshot captured Rachel's
  original intro frame through the D3D9/Vulkan bridge.
- The extended 90-second run loaded `FMVOpening.vp6` and rendered its original
  car sequence. It reached the bring-up deadline at guest `0x0043bdf0` while
  still in the intro; this is not evidence of successful menu initialization.

Both test projects build with the Android NDK CMake toolchain, ABI
`arm64-v8a`, platform `android-26` and `ANDROID_STL=c++_static`. Push the
executables to `/data/local/tmp`, run `chmod 755` after each push, then run
them through `adb shell`. The filesystem fixture currently uses
`/data/local/tmp` and is intended to run as the adb shell user.

Local evidence is in the parent workspace's
`work/android-profile-device-test.log`,
`work/android-profile-device-test.png` and `work/android-profile-build.log`.
The locally generated APK is `build/android/NFSU2-AndroidEvolved-runtime.apk`.

Vulkan instance version remains 1.1. The tested driver advertises 1.3;
compatibility on a Vulkan-1.1-only GPU is still unverified. Lifecycle
recovery, complete input/audio coverage and performance remain unfinished.

API contracts used:
[VirtualProtect](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect),
[SHGetFolderPathA](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpatha),
[FindFirstFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilea),
[WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile).
