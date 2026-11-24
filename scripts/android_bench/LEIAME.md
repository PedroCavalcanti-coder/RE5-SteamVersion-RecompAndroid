# Ferramentas de medição no celular (A54, 2026-11-23)

Todas usam o `adb` do SDK em `%LOCALAPPDATA%\Android\Sdk\platform-tools` e o pacote `com.re5.recompiled`.
Rode com o aparelho livre e o jogo instalado.

| Script | Para quê |
|---|---|
| `bench_android.py <pasta> [s] [CHAVE=VALOR...]` | abre o jogo do zero, roda o benchmark pelo roteiro de teclas (`RE5_INPUT_SCRIPT`) e mede `s` segundos; grava `logcat.txt` e `clocks.txt` (GPU/CPU) |
| `bench_fps.py <logcat.txt>...` | média, mediana, p10, draws por quadro e `present_ms` das linhas de `RE5_FPS_LOG` |
| `matriz.py <pasta> [A,C,D]` | instala vários APKs em sequência e roda o benchmark em cada um (edite a lista `V`) |
| `prof_run.py <pasta> <dxvk\|vkmali> [s] [CHAVE=VALOR]` | benchmark + `simpleperf --app` por `s` segundos; troca e restaura a preferência do renderizador |
| `build_variant.sh <nome> [-D...]` | compila o código atual com o NDK r27 (Unity) numa pasta `build/android-<nome>`, empacota e guarda `work/android/app-<nome>.apk` |

Cuidados aprendidos:
- `RE5_FPS_LOG=1` e `RE5VK_PASS_TIMING=1` custam CPU (até 24% da thread de render no vkmali, por `clock_gettime` a cada chamada).
  Perfil de CPU sem eles; fps com `RE5_FPS_LOG` só quando for ler o fps.
- O roteiro de teclas (`RE5_INPUT_SCRIPT`) faz o jogo abrir o benchmark sozinho. **Depois de qualquer teste, devolva o
  `re5_env.txt` do aparelho ao normal** (os scripts tentam, mas uma interrupção deixa as chaves lá).
- A preferência do app (`shared_prefs/re5_settings.xml`, `gfx_renderer`) vale mais que `RE5_GFX` do `re5_env.txt`.
- O Git Bash converte caminhos `/c/...`: o `adb install` precisa de caminho do Windows (`C:\...`).
- Medir sempre o MESMO trecho do benchmark, por pico e média; uma rodada de aquecimento antes (cache de texturas).
