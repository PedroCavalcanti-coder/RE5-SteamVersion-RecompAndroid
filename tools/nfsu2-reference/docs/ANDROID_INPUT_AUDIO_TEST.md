# Android runtime 0.4.0 — 2026-10-07

For subsequent profile-folder and memory protection work, see
[runtime 0.5.0](ANDROID_PROFILES_MEMORY_TEST.md).

Current status supersedes the earlier bootstrap and 0.3.0 bring-up reports.
The locally generated ARM64 APK has rendered the original NFSU2 initial
splash on Samsung SM-S938B / Adreno 830. This is real guest D3D9 drawing through
DXVK/Vulkan, not a host replacement image. Menu and races are not validated.

## Added

- A separate launcher selects languages present in `nfsu2/LANGUAGES`, defaults
  to Spanish and persists the selection. The real guest's `Language` registry
  query receives the original supported language name (Spanish, English UK,
  etc.). No original files are renamed. Each launch has a fresh `:game` process.
- DirectInput 8 ANSI objects, keyboard/mouse enumeration, supported formats,
  acquisition, immediate state, cooperative level and guest COM reference
  counting. Android physical keys and initial touch buttons update DIK states
  and Windows key messages. Buffered input, action mapping, force feedback,
  full joystick support and mouse motion remain incomplete. Unsupported
  action mapping returns a real unsupported HRESULT; it is not a success stub.
- Software DirectSound PCM buffers, guest lock/unlock, looping and one-shot
  playback, positions, frequency, volume and pan; Android AAudio stereo float
  output. Non-PCM codecs, spatialization, notifications, device disconnect
  recovery and all sound API coverage remain unfinished.
- Guest mutex ownership, recursion, named handles, timeouts and abandoned
  ownership; file seeks; UTC guest time zone; bounded ANSI formatting;
  committed-memory pointer checks; Windows message queue and dispatch.

## Validation

- ARM64 mixer fixtures passed on the physical phone: signed PCM16, unsigned
  PCM8, stereo/mono, fractional resampling, loops, stopping, clipping and gains.
- Guest ABI checks passed on the phone for keyboard press/release, arbitrary
  data offsets, output guard byte, acquired/unacquired state and stdcall cleanup.
- AAudio opened a real 48 kHz stereo float stream. This verifies output
  initialization, not audible correctness of every game sound.
- Launcher selection was confirmed in device logs as `Spanish`, followed by
  the original guest loading `Spanish.bin`. The updated foreground activation
  lets initialization advance to the next explicit boundary:
  `kernel32.dll!VirtualProtect` at guest caller `0x007212e5`.
- Device logs confirm game resource loading, buffers/shaders and original
  initial splash draw/present. The runtime retains a 30-second bring-up limit;
  timeout while waiting for initialization/messages is an unresolved result,
  not a successful menu or race test.
- A touch test exposed JNI lookup failing after NativeActivity-only loading;
  explicit `System.loadLibrary` fixed the failure and subsequent touch
  down/up events reached the native input state.

Parent workspace evidence: `work/android-input-audio-device-test.log`,
`work/android-input-audio-device-test.png`, and
`work/android-runtime-input-host-test.log`. Logs/screenshots and APKs stay
local. Current local artifact: `build/android/NFSU2-AndroidEvolved-runtime.apk`.

Launcher evidence: `work/android-launcher-test.log` and
`work/android-launcher-menu.png` in the parent workspace.

The instance requests Vulkan 1.1; the tested phone driver advertises 1.3.
A Vulkan-1.1-only GPU has not been tested. Full guest pause/resume, native
surface recovery and performance optimization remain open work.
