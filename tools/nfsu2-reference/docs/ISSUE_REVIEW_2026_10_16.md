# Android issue review, 16 October 2026

Reviewed 662 available Underground 2 issues including comments, alongside
38 Underground 1 issues. Many contain only a phone-local ZIP path and an
empty technical summary. Downloaded available recent attachments; the
attachment in #507 returned HTTP 404. Reports and device logs stay outside
Git, and no issues or comments were modified.

## Back-buffer reset failure

[#629](https://github.com/codepdbh/nfsu2-recompiled/issues/629) reports
v0.14.5 on a moto e40 with `ClearContainer` at the top of the crash stack,
called from `CreateBackBuffers` during `Reset`, after a prior DXVK error.
The same path appears in #530, #506, #486, #451, #420 and #361 on earlier
builds. The #530 ZIP was downloaded; its crash records include older
builds, so its report-version field alone is not sufficient attribution.

The exact v0.14.5 ARM64 library, Build ID
`d88fcb81c4153db9eeefabb3a3d19d7418fa54d4`, has an unconditional
`ClearContainer` call inside the cleanup loop at `0x50abb8`. The source
preparation recipe already contains null-entry cleanup and exception
handling for partial allocation. Regenerating the prepared DXVK source
and rebuilding includes these protections in the actual binaries.

Added a SHA-256 recipe marker checked by CMake, including the shared
Underground 1 build, to reject stale or unverified preparation. Added
guards for absent Vulkan swapchain/surface handles and repeated surface
teardown. `ports/dxvk_android/test_reset_cleanup.py` extracts the actual
prepared method bodies into a host fixture. Fault injection at each of
four buffer slots, successful retry, empty cleanup, missing extension
function pointers and repeated handle destruction pass with MSVC C++17.
This verifies cleanup behavior, not universal recovery from GPU memory
exhaustion or driver faults on unconnected devices.

## Orientation and log overhead

[#633](https://github.com/codepdbh/nfsu2-recompiled/issues/633) reports
that a controller attached in the opposite horizontal orientation leaves
the game upside down. Launcher, GPU probe and game now use
`sensorLandscape`, permitting either horizontal orientation.

[#672](https://github.com/codepdbh/nfsu2-recompiled/issues/672) includes
large runs of repeated module lookups. Both runtimes now log a module
name only on its first lookup, reducing unnecessary logging work while
preserving the first diagnostic. Reports include the actual installed
APK ABI to distinguish 32-bit and 64-bit failures.

## Still open

The #672 native fault resolves to `sub_005C5F70` in the matching v0.14.5
library. Other reports resolve to `sub_0043DBB0` (a string scan). These
need additional state and reproduction before a behavioral change is
justified. The `RaiseException` report in #656 lacks exception arguments
and cannot be classified as fixed. No claim of fixing every crash,
every MediaTek device, audio complaint, or performance report is made.

## Validation and candidate status

Both architectures compile and were installed on the connected Redmi
Note 8. ARM64 reached menus; ARMv7 was verified running through
`/system/bin/app_process32` and presented the intro. The initial ARM64
test, after an app background/resume cycle, stalled while loading a quick
race. The diagnostic recorded guest function `0x00669286`, without the
ClearContainer native crash. This separate stall is not claimed fixed.
The build remains a test candidate, not universal gameplay validation.

Version, signing certificate, ELF architecture and sensorLandscape
manifest values were verified for both APKs. Native symbols matching
each packaged Build ID were archived locally. No affected MediaTek or
Unisoc device is connected for direct reproduction of the reported GPU
failures; injected cleanup tests cover the diagnosed failure paths.

## White screen recovery during this validation

The user confirmed a white screen when entering a race. A second fresh
ARM64 session stalled too, without an app-switch cycle. A native debugger
capture, resolved against the matching APK symbols, shows the guest render
thread in D3D9SwapChainEx::SynchronizePresent, waiting for submission, and
the submission worker inside Vulkan queue presentation. The earlier guest
function in the watchdog is not proof of where the native wait occurs.

Stopped the game and renamed only the app-private cache/dxvk directory to
a backup. With a new graphics cache, the same APK reached a four-car race
(lap 1/3, frames presenting) and the user confirmed it worked. This points
to the old graphics cache as a trigger; it does not isolate which cache
file or driver behavior caused the wait.

Both apps now use cache epoch dxvk-v2, leaving older cache files intact
and rebuilding shaders on the first run after this renderer update.
The launcher also offers a graphics-cache reset with confirmation that
the current game session closes. It targets only the game process with
the same UID and exact package process name, then renames the graphics
cache within getCacheDir(). Game files, profiles and settings are not
changed. The old cache is retained as a private-cache backup.
