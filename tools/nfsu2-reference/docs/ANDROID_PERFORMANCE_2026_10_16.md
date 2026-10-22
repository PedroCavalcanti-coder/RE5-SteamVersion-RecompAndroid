# Android race performance, 16 October 2026

Testers with recent phones (Dimensity 9300/8500, Snapdragon 8 Gen 3) reported
about 20 FPS. Issue logs showed 38k–75k Direct3D 9 calls per race frame and a
bridge cost of 5–12 ms per frame, so neither the GPU nor the bridge alone
explained the frame time.

## Measurement

`simpleperf` (system binary, `--app` on the debuggable APK) on the Redmi Note 8
(Snapdragon 665) during a race, symbolized with the archived unstripped
library of the installed build.

On-CPU, render thread:

| Share | Code |
|---|---|
| ~25% | statically linked D3DX effect code (`sub_00669286`, `sub_00660A42`, `sub_00663A32`, ...) applying pass states |
| ~18% | bridge: `dispatchGuestD3D9`, `d3dDispatch`, `recomp_lookup*`, PLT stubs, `checkGuestProgress` |
| ~5% | DXVK D3D9 front end |

The render thread was on CPU only ~67% of the time. An off-CPU profile
(`--trace-offcpu --call-graph dwarf`) put 56% of its wall time in
`MachineMutex::lock`, reached from the time-slice yield in
`checkGuestProgress`, called from `recomp_lookup_import` inside the D3DX code.

The guest audio thread used ~14% of a CPU, a third of it in `callGuest`
(state save/restore of all integer, x87, MMX and SSE registers, plus a
`std::vector` malloc/free per call) on behalf of `dynamicThunk`, which runs the
game's generated mixing code: ~100 `push esi; call f; mov [esi+n],eax` blocks
per mix.

## Causes and changes

- Every guest thread yielded the machine after 500 µs if another one waited.
  The render thread handed over about 100 times per frame, each handover
  waking the queued threads in turn. The deadline was a shared static, so the
  incoming thread inherited an expired slice and handed the machine straight
  back (ping-pong). Slices now count from `MachineMutex::acquired`; the render
  thread (the one presenting) gets 3 ms and yields once after each Present.
- `dynamicThunk` performs its calls as plain cdecl calls, as the x86 code does;
  the lifted callee preserves its callee-saved registers.
- `callGuest` takes braced argument lists without heap allocation.
- `d3dDispatch` reads arguments in place on the guest stack; dispatch keeps a
  one-entry object cache, invalidated whenever an object is erased.
- `libnfsu2_android.so` links with `-Bsymbolic`, removing PLT stubs from the
  lookups taken on every indirect call.
- Every 120 frames the log reports the costliest D3D9 methods and the render
  thread's handovers and wait per frame.

## Result

Same race on the Redmi Note 8, compared at similar call counts (the scene is
not an identical replay):

| Calls/frame | v0.14.6 | v0.14.7 candidate |
|---|---|---|
| ~19k | 27.7 FPS | 34.3–36.1 FPS |
| ~25k | 20.3 FPS | 27.8 FPS |

Render-thread wait dropped from 56% of wall time to roughly 5–7 ms of a
28–36 ms frame (7–9 handovers per frame). Audio health reported no underruns.
No high-end MediaTek device was available; the handover cost affected every
device, but the gain there is not measured.

## Next

The D3DX effect state application remains the largest single cost. The
audio thread's `dynamicThunk` still decodes the generated code on every call
(~10% of that thread); caching the decoded sequence per address is the next
inexpensive step.
