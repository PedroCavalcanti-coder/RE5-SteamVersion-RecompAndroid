# Runtime 0.3.0 device verification — 2026-10-07

Installed and launched the ARM64 debug APK on authorized ADB device
R5GL70TXWMP, Samsung SM-S938B, Android API 36, Adreno 830.

The game reads the owner's files from `/storage/emulated/0/nfsu2`.
The APK uses the supplied icon. The Vulkan instance requests API 1.1;
the tested driver advertises 1.3. A device limited to Vulkan 1.1 has
not been tested, and additional DXVK feature requirements still apply.

The lifted game executed its initialization through real DXVK D3D9
device creation, embedded shader loading and creation, textures,
vertex declarations, vertex/index buffers and resource locks. This
does not establish successful game scene rendering or gameplay.

Fixed Android initialization failing because DXVK attempted to create
a cache directory from an empty path. Optional DXVK state caching is
disabled on Android and file logging is disabled. Added process affinity
queries reporting the one virtual x86 CPU exposed by the serialized
guest runtime; invalid handles and masks report errors.

Current explicit integration boundary:
`dinput8.dll!DirectInput8Create` from guest address `0x006d7100`.
The game has not reached its menu. Input, audio, further Windows APIs,
and complete gameplay/lifecycle behavior remain unfinished.

Evidence: `work/android-dxvk-device-test.log` and screenshot
`work/android-dxvk-device-test.png` in the parent workspace.
The Linux/Xlib Vulkan test reaches the same DirectInput boundary:
`work/android-runtime-surface-host-test.log`.

APK: `build/android/NFSU2-AndroidEvolved-runtime.apk`.
Earlier bootstrap/runtime reports describe earlier builds and should
not be used as the current integration status.
