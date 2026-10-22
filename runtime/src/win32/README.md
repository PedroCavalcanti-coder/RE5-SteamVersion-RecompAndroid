# `runtime/src/win32/` — HLE do Win32 (Fase 3)

Um arquivo por DLL (`kernel32.c`, `user32.c`, `gdi32.c`, `advapi32.c`, `winmm.c`,
`ole32.c`, `ws2_32.c`, ...). Cada um exporta `const re5_import_module re5_mod_<dll>`
e é registrado em `runtime/src/core/imports.c`. Padrão do handler: ver o
cabeçalho de `kernel32.c` e o `CLAUDE.md`.

A ordem de implementação vem do censo do build limpo:

```bash
./build/linux-debug/re5 --census --missing --exe bin_input/re5dx9.clean.exe
```

Pontes COM (decisão D4): `steam_api.c` (thiscall, vtables escritas à mão) e
`d3d9.c` (stdcall, vtables geradas por `scripts/gen_d3d9_bridge.py` a partir do
`d3d9.h` da DXVK native; o backend do host fica em `runtime/src/gfx/d3d9_host.c`),
`dinput8.c`, `dsound.c` (mixer próprio no áudio do SDL2) e `wmvcore.c`
(`IWMSyncReader` sobre o FFmpeg de `scripts/build_ffmpeg.sh`), estas com vtables
escritas à mão. Método sem handler aborta com `Interface::Método`.

Referência para comportamento: `tools/pcrecomp/runtime/win32hle/` (um Win32
implementado sobre POSIX/SDL2 para o modelo de 32 bits do pcrecomp) — reaproveite
a lógica, adaptando os acessos à memória do guest (`re5_g2h`, `rd32/wr32`).
