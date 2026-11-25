# Release do vk2 (Android)

Regra: a versão marcada "release" só é apagada/substituída quando outra tiver desempenho **significativamente maior**
no mesmo teste. Este arquivo guarda como refazê-la.

## release-1 — `app-vk2-release-1.apk` (2026-11-25)

- **Desempenho (A54, Mali-G68, 360p, `RE5_LEAN=1` + `RE5_GFX=vk2`, ambiente limpo, medido pelo usuário, 90 s):
  pico 59,3 / média 35,6.** Comparação na mesma bateria: A (réplica do `app-vk2-4`) 53,6 / 33; B (mesmo código sem IMMEDIATE) 54,6 / 32,7.
- **Código:** branch `release/vk2-imediato-1`, tag `release-vk2-1`. Base = commit `2657eae6` (vk2 completo) mais:
  1. `vk2_dev.c` `swap_req()`: no Android, quando o jogo pede "sem vsync", usa `VK_PRESENT_MODE_IMMEDIATE_KHR` com FIFO de reserva (nunca MAILBOX).
     `RE5VK2_PRESENT=fifo|mailbox|immediate` força outro modo.
  2. `d3d9.c`: `game_fps_cap_lift()` movida para antes de `query_lag()` (no `2657eae6` ela ficava dentro da função e o Android não compilava).
- **Fora do APK de propósito:** toque nativo e demais edições sem commit da árvore (`thread.c`, `thunks.c`, `overrides.c`, `kernel32*.c`).
  Elas ajudaram o `app-vk2-2..4`, mas na bateria A/B (A com elas, B sem) deram a mesma média; ficam fora do release.
- **Como refazer:** `git worktree add work/limpo release-vk2-1`; copiar `android/local.properties` e `android/app/src/main/assets/re5dx9.clean.exe`;
  `bash work/release/vk2-imediato-1/build_limpo.sh` (CMake + Ninja, NDK r27 da Unity, `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DRE5_GENERATED_OPT=-O1`,
  `-DRE5_GENERATED_DIR=<repo>/generated/src`, FFmpeg de `build/ffmpeg-android`); `llvm-strip --strip-unneeded` do `libre5.so` para
  `android/app/src/main/jniLibs/arm64-v8a/`; `gradlew assembleDebug` com `JAVA_HOME` do Android Studio (JBR).
- **Ambiente no aparelho:** `re5_env.txt` só com `RE5_LEAN=1` e `RE5_GFX=vk2`; caches `cache/vk2` e `cache/re5vk.pcache` vazios antes do teste.
- **Cópia do APK:** `work/release/vk2-imediato-1/app-vk2-release-1.apk` (SHA-256 em `SHA256.txt`; `work/` não vai para o git).
