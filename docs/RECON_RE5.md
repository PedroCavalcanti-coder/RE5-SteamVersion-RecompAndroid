# Reconhecimento do binário (gerado por `scripts/recon.py`)

Gerado em 2026-10-20 15:42 UTC. Regenere com `python3 scripts/recon.py --all --markdown docs/RECON_RE5.md`.

## `bin_input/re5dx9.exe`

| Campo | Valor |
|---|---|
| SHA-256 | `751726f0ec8bd01c00b037af8ae64cdf0bc5cc5b9025f3f4d782ef6f66e04e26` |
| Tamanho | 8,195,416 bytes |
| Build conhecido | steam-2023-02-13-original |
| Timestamp PE | 2023-02-13T05:43:28Z |
| Linker | 8.00 (MSVC 2005) |
| Image base / SizeOfImage | 0x00400000 / 0x01929000 |
| Entry point | 0x01CFF310 (seção #7) |
| Relocações | não (RELOCS_STRIPPED=True) |
| Imports declarados | 21 funções em 18 DLLs |
| Veredito | **BLOQUEADO** — camadas presentes: `steamstub`, `enigma` |

| # | Seção | VA | VSize | Raw | Entropia | Flags |
|---|---|---|---|---|---|---|
| 0 | `(sem nome)` | 0x00401000 | 0x00B49000 | 0x0041CA00 | 8.0000 | XW |
| 1 | `(sem nome)` | 0x00F4A000 | 0x00180000 | 0x00063200 | 7.9993 | XW |
| 2 | `(sem nome)` | 0x010CA000 | 0x001E9000 | 0x00022C00 | 7.9979 | XW |
| 3 | `(sem nome)` | 0x012B3000 | 0x000BA000 | 0x00081600 | 7.9997 | XW |
| 4 | `.rsrc` | 0x0136D000 | 0x00012000 | 0x00012000 | 5.9056 | -- |
| 5 | `(sem nome)` | 0x0137F000 | 0x00764000 | 0x00053600 | 7.9956 | XW |
| 6 | `.data` | 0x01AE3000 | 0x0021C000 | 0x0021B400 | 7.9814 | XW |
| 7 | `.bind` | 0x01CFF000 | 0x000298D8 | 0x000298D8 | 7.9732 | X- ← EP |

Evidências:

- seção .bind presente (SteamStub)
- assinatura do Enigma Protector no binário: Enigma Protector, Enigma Protector CA
- 7/7 seções executáveis com entropia >= 7.5 (código comprimido/criptografado; código x86 real fica em ~6.0-6.7)
- entry point na seção #7 '.bind', não na primeira seção de código (stub de loader)
- 5 seções sem nome (típico de protetor)
- tabela de imports mínima: 21 funções em 18 DLLs (o loader resolve a IAT real em tempo de execução)

<details><summary>Imports declarados</summary>

- `kernel32.dll`: GetModuleHandleA, GetProcAddress, ExitProcess, LoadLibraryA
- `user32.dll`: MessageBoxA
- `advapi32.dll`: RegCloseKey
- `oleaut32.dll`: SysFreeString
- `gdi32.dll`: CreateFontA
- `shell32.dll`: ShellExecuteA
- `version.dll`: GetFileVersionInfoA
- `WS2_32.dll`: connect
- `IPHLPAPI.DLL`: GetIfEntry
- `WINMM.dll`: timeBeginPeriod
- `XINPUT1_3.dll`: #3
- `DINPUT8.dll`: DirectInput8Create
- `d3d9.dll`: D3DPERF_GetStatus
- `DSOUND.dll`: #11
- `WMVCore.DLL`: WMCreateSyncReader
- `AVIFIL32.dll`: AVIFileInit
- `MSVFW32.dll`: ICCompressorChoose
- `ole32.dll`: CoSetProxyBlanket

</details>

## `bin_input/re5dx9.exe.unpacked.exe`

| Campo | Valor |
|---|---|
| SHA-256 | `a30b52b96b31d87b8357c99567732750d46644771a865b8ef5f98b382275f0a8` |
| Tamanho | 8,025,216 bytes |
| Build conhecido | steam-2023-02-13-steamless |
| Timestamp PE | 2023-02-13T05:43:28Z |
| Linker | 8.00 (MSVC 2005) |
| Image base / SizeOfImage | 0x00400000 / 0x018FF000 |
| Entry point | 0x01CFC090 (seção #6) |
| Relocações | não (RELOCS_STRIPPED=True) |
| Imports declarados | 21 funções em 18 DLLs |
| Veredito | **BLOQUEADO** — camadas presentes: `enigma` |

| # | Seção | VA | VSize | Raw | Entropia | Flags |
|---|---|---|---|---|---|---|
| 0 | `(sem nome)` | 0x00401000 | 0x00B49000 | 0x0041CA00 | 8.0000 | XW |
| 1 | `(sem nome)` | 0x00F4A000 | 0x00180000 | 0x00063200 | 7.9993 | XW |
| 2 | `(sem nome)` | 0x010CA000 | 0x001E9000 | 0x00022C00 | 7.9979 | XW |
| 3 | `(sem nome)` | 0x012B3000 | 0x000BA000 | 0x00081600 | 7.9997 | XW |
| 4 | `.rsrc` | 0x0136D000 | 0x00012000 | 0x00012000 | 5.9056 | -- |
| 5 | `(sem nome)` | 0x0137F000 | 0x00764000 | 0x00053600 | 7.9956 | XW |
| 6 | `.data` | 0x01AE3000 | 0x0021C000 | 0x0021B400 | 7.9814 | XW ← EP |

Evidências:

- assinatura do Enigma Protector no binário: Enigma Protector, Enigma Protector CA
- 6/6 seções executáveis com entropia >= 7.5 (código comprimido/criptografado; código x86 real fica em ~6.0-6.7)
- entry point na seção #6 '.data', não na primeira seção de código (stub de loader)
- 5 seções sem nome (típico de protetor)
- tabela de imports mínima: 21 funções em 18 DLLs (o loader resolve a IAT real em tempo de execução)

<details><summary>Imports declarados</summary>

- `kernel32.dll`: GetModuleHandleA, GetProcAddress, ExitProcess, LoadLibraryA
- `user32.dll`: MessageBoxA
- `advapi32.dll`: RegCloseKey
- `oleaut32.dll`: SysFreeString
- `gdi32.dll`: CreateFontA
- `shell32.dll`: ShellExecuteA
- `version.dll`: GetFileVersionInfoA
- `WS2_32.dll`: connect
- `IPHLPAPI.DLL`: GetIfEntry
- `WINMM.dll`: timeBeginPeriod
- `XINPUT1_3.dll`: #3
- `DINPUT8.dll`: DirectInput8Create
- `d3d9.dll`: D3DPERF_GetStatus
- `DSOUND.dll`: #11
- `WMVCore.DLL`: WMCreateSyncReader
- `AVIFIL32.dll`: AVIFileInit
- `MSVFW32.dll`: ICCompressorChoose
- `ole32.dll`: CoSetProxyBlanket

</details>

## `bin_input/re5dx9.clean.exe`

| Campo | Valor |
|---|---|
| SHA-256 | `4534003fe7949750c8c2bffd337f9d735eb9d27498a66e77e77cf1a87dfa4036` |
| Tamanho | 16,257,024 bytes |
| Build conhecido | steam-2023-02-13-oep-dump-reconstructed |
| Timestamp PE | 2023-02-13T05:43:28Z |
| Linker | 8.00 (MSVC 2005) |
| Image base / SizeOfImage | 0x00400000 / 0x00F81000 |
| Entry point | 0x00E68A06 (seção #0) |
| Relocações | não (RELOCS_STRIPPED=True) |
| Imports declarados | 270 funções em 18 DLLs |
| Veredito | **PRONTO PARA LIFTING** |

| # | Seção | VA | VSize | Raw | Entropia | Flags |
|---|---|---|---|---|---|---|
| 0 | `.text` | 0x00401000 | 0x00B49000 | 0x00B49000 | 6.4608 | X- ← EP |
| 1 | `.rdata` | 0x00F4A000 | 0x00180000 | 0x00180000 | 6.2128 | -- |
| 2 | `.data` | 0x010CA000 | 0x001E9000 | 0x001E9000 | 1.8661 | -W |
| 3 | `.data1` | 0x012B3000 | 0x000BA000 | 0x000BA000 | 7.4821 | -- |
| 4 | `.rsrc` | 0x0136D000 | 0x00012000 | 0x00012000 | 5.9056 | -- |
| 5 | `.idata` | 0x0137F000 | 0x000019A6 | 0x00001A00 | 5.0170 | -- |

<details><summary>Imports declarados</summary>

- `advapi32.dll`: RegOpenKeyExA, RegQueryValueExA, RegCloseKey
- `avifil32.dll`: AVIFileInit, AVIFileReadData, AVIFileOpenA, AVIFileRelease, AVIStreamRelease, AVIMakeCompressedStream, AVIFileExit, AVIFileCreateStreamA, AVIStreamWrite
- `dinput8.dll`: DirectInput8Create
- `dsound.dll`: DirectSoundCreate8
- `gdi32.dll`: GetStockObject
- `iphlpapi.dll`: GetIfEntry, GetIfTable
- `kernel32.dll`: CreateDirectoryA, SetCurrentDirectoryA, ReadFile, CloseHandle, FindFirstFileA, DeleteFileA, FindClose, lstrcpyA, lstrlenA, InitializeCriticalSection, TlsAlloc, TlsFree, TlsSetValue, TryEnterCriticalSection, InitializeCriticalSectionAndSpinCount, GetCurrentProcess, GetProcAddress, LoadLibraryA, FreeLibrary, CreateThread, GetExitCodeThread, GetCurrentThreadId, FindNextFileA, GetFileAttributesA, InterlockedExchangeAdd, GetStringTypeW, GetStringTypeA, GetLocaleInfoA, GetTimeZoneInformation, GetLastError, SetStdHandle, FlushFileBuffers, GetConsoleMode, GetConsoleCP, WriteConsoleA, GetConsoleOutputCP, WriteConsoleW, GetCurrentProcessId, GetTickCount, EnterCriticalSection, GetSystemDefaultLangID, GetFileType, SetHandleCount, GetEnvironmentStringsW, FreeEnvironmentStringsW, GetEnvironmentStrings, FreeEnvironmentStringsA, LCMapStringW, MultiByteToWideChar, WideCharToMultiByte, LCMapStringA, GetOEMCP, GetACP, GetCPInfo, SetLastError, GetModuleFileNameA, GetStdHandle, HeapSize, TlsGetValue, ExitProcess, HeapReAlloc, GetStartupInfoA, GetProcessHeap, HeapAlloc, GetVersionExA, HeapFree, LeaveCriticalSection, RtlUnwind, RaiseException, IsDebuggerPresent, SetUnhandledExceptionFilter, UnhandledExceptionFilter, TerminateProcess, Sleep, SuspendThread, SetThreadPriority, OutputDebugStringA, GlobalUnlock, CompareStringA, CompareStringW, FileTimeToSystemTime, SetEnvironmentVariableA, CreateSemaphoreA, ReleaseSemaphore, HeapCreate, DeleteCriticalSection, WriteFile, ReadFileEx, GlobalAlloc, GlobalLock, HeapDestroy, LocalFileTimeToFileTime, SetThreadIdealProcessor, SetFileTime, FileTimeToLocalFileTime, GetDiskFreeSpaceA, MoveFileA, CreateMutexA, WaitForSingleObject, ReleaseMutex, GetSystemTimeAsFileTime, GetUserDefaultLangID, LocaleNameToLCID, FatalAppExitA, InterlockedIncrement, QueryPerformanceCounter, SetEvent, CreateEventA, ResetEvent, GetSystemInfo, WaitForMultipleObjects, QueryPerformanceFrequency, ResumeThread, VerSetConditionMask, VerifyVersionInfoA, SleepEx, GetCurrentDirectoryA, DebugBreak, GetCommandLineA, InterlockedDecrement, GetModuleHandleA, GetPrivateProfileStringA, WritePrivateProfileStringA, VirtualFree, VirtualAlloc, CreateFileA, GetFileSize, SetFilePointer, SetEndOfFile, SystemTimeToFileTime, CopyFileA
- `msvfw32.dll`: ICCompressorChoose, ICCompressorFree
- `oleaut32.dll`: SysFreeString, SysAllocString
- `shell32.dll`: DragAcceptFiles, ShellExecuteA, SHGetFolderPathA
- `user32.dll`: CallNextHookEx, GetKeyboardState, SetWindowsHookExA, UnhookWindowsHookEx, DestroyWindow, RegisterClassExA, LoadIconA, CreateWindowExA, DefWindowProcA, SendMessageA, EndPaint, GetSystemMenu, GetWindowRect, GetMessageW, PostQuitMessage, IsZoomed, AttachThreadInput, GetClientRect, BeginPaint, TranslateMessage, ClipCursor, LoadIconW, SetWindowLongA, PeekMessageW, SetWindowPos, LoadAcceleratorsA, ShowWindow, CreateWindowExW, SystemParametersInfoA, GetSystemMetrics, UpdateWindow, AdjustWindowRect, DefWindowProcW, GetWindowThreadProcessId, MoveWindow, DispatchMessageW, ScreenToClient, ClientToScreen, SetClipboardData, EmptyClipboard, GetCursorPos, SetCursorPos, RegisterClassExW, LoadCursorA, SetCursor, DestroyIcon, GetClassLongA, SetClassLongA, SetForegroundWindow, ShowCursor, IsIconic, GetForegroundWindow, GetFocus, FindWindowW, SetWindowTextW, SetActiveWindow, CloseClipboard
- `winmm.dll`: timeBeginPeriod, timeEndPeriod, timeGetTime
- `wmvcore.dll`: WMCreateSyncReader
- `ws2_32.dll`: connect, gethostbyname, inet_addr, htons, WSAAsyncSelect, WSAStartup, gethostname, listen, bind, setsockopt, send, ntohs, accept, __WSAFDIsSet, select, WSACleanup, recv, sendto, getsockname, recvfrom, ioctlsocket, getpeername, WSAGetLastError, socket, closesocket, inet_ntoa, getsockopt
- `xinput1_3.dll`: XInputSetState, XInputGetCapabilities, XInputGetState
- `d3d9.dll`: D3DPERF_GetStatus, Direct3DCreate9
- `ole32.dll`: CoSetProxyBlanket, CoUninitialize, CoInitialize, CoCreateInstance
- `steam_api.dll`: SteamAPI_IsSteamRunning, SteamUtils, SteamAPI_Shutdown, SteamFriends, SteamApps, SteamAPI_UnregisterCallback, SteamAPI_RegisterCallback, SteamRemoteStorage, SteamAPI_RestartAppIfNecessary, SteamAPI_Init, SteamAPI_RegisterCallResult, SteamAPI_UnregisterCallResult, SteamNetworking, SteamClient, SteamMatchmaking, SteamUser, SteamUserStats, SteamAPI_RunCallbacks

</details>

