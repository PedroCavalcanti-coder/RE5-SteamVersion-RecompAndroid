# Audio and frame pacing follow-up (2026-10-15)

Tester reports affect all three PC ports, including Dimensity 9300/8400 and
Snapdragon 675. Those models have not been tested locally; the connected
Redmi Note 8 is a separate device. Do not treat these changes as a confirmed
fix for every reported audio or frame-rate problem.

The frame limiter previously slept while owning the serialized guest machine.
It now saves the current thread context, releases the machine until its frame
deadline, then reacquires it and restores the context. Guest audio, streaming
and network workers can progress during that idle time. The startup scheduler
regression exercises timed handovers alongside ordinary yields and checks
integer, FS, x87, XMM, TLS and last-error isolation.

AAudio requests low-latency shared output at the device-selected sample rate.
The initial buffer is two hardware bursts. After observed output underruns,
the guest thread grows the buffer one burst at a time, up to eight bursts and
never beyond the stream capacity. Devices may decline the low-latency request.
Audio health logs report actual performance mode, rate, burst, buffer size,
underrun count and peak callback duration every 120 successful game Presents.
No logging or buffer resizing was added inside the audio callback.

The mixer still uses a mutex while rendering and publishing PCM. Callback
starvation, stale guest streaming data and graphics stalls remain distinct
possible causes; the new diagnostics help distinguish them. Stream recovery
following audio-device disconnects is not implemented by this change.

Build validation is recorded in work/audio-pacing-build.log (ignored).
Physical testing of this final audio revision on the affected tester devices
is pending. No release assets are replaced by this source commit.

AAudio buffer tuning reference:
https://developer.android.com/ndk/guides/audio/aaudio/aaudio


## Resampling and CPU follow-up

The user's remaining audio report is static-like background noise during
**gameplay**, rather than delayed or interrupted playback. A native PCM ring
snapshot from MW on the Redmi was stereo, signed 16-bit, 22050 Hz. AAudio
selected 48000 Hz. This is a real rate conversion, not a changed playback speed.

Upsampling now uses a normalized 16-tap Blackman-windowed sinc with 256 phases
and interpolation between phases. Tables are prepared before stream playback;
the callback does not allocate memory or calculate trigonometric functions.
Equal-rate playback and downsampling retain the previous path. This change
suppresses spectral images from the former two-point linear upsampler; it is
not a noise gate and does not remove intentional game ambience or noise already
present in decoded source PCM. Subjective confirmation of the reported static
remains pending. Filter overshoot and source mixing can still reach the output
clamp; clipping counters report this instead of concealing it.

The deterministic PCM test measures a 6 kHz source tone at 22050 Hz converted
to 48000 Hz: its unwanted 16.05 kHz image is about 68 dB lower than with the
previous interpolator, while the source amplitude remains 0.25. It also checks
unsigned 8-bit silence through upsampling, signed stereo, mute, loop boundaries,
fractional pitch and clipping detection. These are synthetic signal results,
not a measurement of the user's audible background noise.

DirectSound GetCaps previously returned 0x147, omitting primary 16-bit and
secondary stereo/16-bit formats. It now reports the supported PCM formats and
continuous rate range with 0xF1F, matching a Windows DirectSound format-capability
probe. Hardware voice counts remain zero because the adapter mixes in software.
A startup regression calls the guest COM methods, checks the 96-byte output
and stack ABI, then releases its temporary device.

FramePacer drops missed deadlines rather than accumulating lateness and
periodically adding another full frame of sleep. Its deterministic clock test
covers 8000 fast/overloaded frames at 30/60/90/120 FPS, pauses, uncapped mode and
cap changes. The scheduler also avoids save/unlock/relock handovers when no
other guest thread has queued for the machine; queued work is still checked
on the existing 500-microsecond scheduling interval. Native COM/import stubs
skip the lifted-function hash table, with a startup guard against overlapping
lifted modules. No controlled FPS percentage is claimed for these CPU changes.

Run the standalone tests from a Visual Studio developer prompt:

```
cl /EHsc /std:c++17 /O2 tests/audio_mixer.cpp /Fework/audio_mixer.exe /Fowork/audio_mixer.obj
work\audio_mixer.exe
cl /EHsc /std:c++17 /O2 tests/frame_pacer.cpp /Fework/frame_pacer.exe /Fowork/frame_pacer.obj
work\frame_pacer.exe
```

References: [DirectSound DSCAPS](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416821(v=vs.85)),
[Julius O. Smith, windowed sinc interpolation](https://www.dsprelated.com/freebooks/pasp/Windowed_Sinc_Interpolation.html),
[linear interpolation and spectral imaging](https://www.dsprelated.com/freebooks/pasp/Linear_Interpolation_Resampling.html).


## Persistent pipeline states and release validation

Android now places DXVK's state cache in the app-private cache/dxvk directory
before creating the device. Directory creation failure leaves caching disabled
without aborting startup. The bridge no longer unconditionally disables the
cache. This records shader/state combinations for DXVK's background pipeline
compilation on later starts; it is not a promise that every first-use shader
stall disappears, and it is not a persistent Vulkan driver-binary cache.

The MW ARM64 build reaches a quick race on the Redmi Note 8 with the CPU and
cache changes. A bounded sample at 780x360 and a 30 FPS cap showed roughly
27-30 FPS in gameplay, with AAudio reporting no output underruns in the captured
session. Source PCM is 22050 Hz and output is 48000 Hz. The user confirmed that
an intermediate audio/pacing build felt more fluid. Subjective static-noise
confirmation for the final filter is still pending.

MW also caches the validated Device handle for ordinary D3D methods, avoiding
repeated registry lookups. QueryInterface/AddRef/Release keep the normal path;
retiring that handle or shutting down invalidates the cache. This bridge cache
is MW-only for now. Both common standalone tests pass on MSVC; all three
projects build ARM64 and ARMv7 releases. Physical ARMv7 testing is pending:
the Redmi was disconnected during release preparation and the connected S25
supports only ARM64. APK ABI, signatures and versions are checked separately.
Dimensity 9300/8400 and Snapdragon 675 testing remains outstanding.


The final release set contains one arm64-v8a and one armeabi-v7a APK per game.
All six APK signatures match the previous releases' certificate; manifest ABI,
version and every native ELF class/machine were inspected. ARM64 release APKs
install over the S25 apps without clearing game data. The user confirmed that
the updated apps work. The S25 cannot install ARMv7 APKs, so those downloads
remain physically untested in this session. They are not universal APKs.

On the Redmi, MW's DXVK v10 state-cache file contained 39 entries whose SHA-1
checksums were independently verified. This verifies persisted cache contents,
not a quantified warm-start FPS gain. A complete race, long-session stability
and validation on the reported Dimensity/Snapdragon 675 devices remain pending.
