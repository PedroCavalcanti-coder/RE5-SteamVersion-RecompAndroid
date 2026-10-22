# Threading

## What SPEED2.EXE uses (direct call sites through the IAT)

| API | sites | | API | sites |
|---|---|---|---|---|
| CreateThread | 6 | | WaitForSingleObject | 19 |
| ExitThread / TerminateThread | 1 / 1 | | WaitForMultipleObjects | 1 |
| SuspendThread / ResumeThread | 1 / 2 | | CreateMutexA / ReleaseMutex | 2 / 14 |
| SetThreadPriority | 6 | | Initialize/Enter/LeaveCriticalSection | 5 / 15 / 16 |
| GetExitCodeThread | 1 | | CreateEventA / SetEvent / ResetEvent | 2 / 4 / 2 |
| TlsAlloc / TlsGetValue / TlsSetValue | 1 / 3 / 4 | | InterlockedExchange | 6 |
| timeSetEvent / timeKillEvent | 1 / 1 | | Sleep / SleepEx | 11 / 3 |
| SetProcessAffinityMask | 2 | | `lock`-prefixed instructions in code | 0 |

No `_beginthread(ex)` import: the static CRT's thread wrappers, if linked, end
in CreateThread. Which subsystems own the 6 threads (streaming loader, audio,
online, input) is still to be identified at runtime.

## Model for the Windows bring-up

pcrecomp `native32`, unchanged in principle:

- The register file is **global**, guarded by one machine lock. A thread owns
  it while running lifted code and releases it around every native call
  (`native_bridge` calls `mach_leave`/`mach_enter`), so a guest thread blocked in
  `WaitForSingleObject`, `Sleep` or `EnterCriticalSection` does not block the
  others.
- Every guest thread gets **its own saved register state, guest stack (4 MB)
  and simulated TIB** on first entry (`mstate` in TLS). This is the
  "independent CPU context per thread" requirement, executed one at a time.
- Lifted loops yield the lock every 65,536 back-edges (`RECOMP_BACKEDGE`), so a
  spin-wait on another guest thread's flag cannot starve it.
- `CreateThread` is shimmed to give the host thread a 16 MB stack (each guest
  call is a host C call, so frames are larger than the original).
- Thread starts and `timeSetEvent` callbacks are guest VAs: Windows calls
  them, the fetch faults on the non-executable guest image, and native32's
  vectored handler redirects into the lifted body on that thread.

## Known risks

- **SuspendThread** on a thread that holds the machine lock deadlocks every
  guest thread. One site exists; it must be checked when reached.
- `SetProcessAffinityMask` (2 sites) may pin the process to one core; harmless
  under the machine lock, but it should be logged.
- No `lock` prefixes and only `InterlockedExchange` imports: guest atomics are
  serialised by the machine lock already.

## Toward ARM64

The machine lock makes the guest effectively single-core. For the Android host
the plan is to make the register file thread-local (`_Thread_local` /
per-thread CPU block) and drop the global lock, with guest synchronisation
mapped onto host primitives by the HLE layer. Nothing in the generated C
depends on the lock; only native32 does.
