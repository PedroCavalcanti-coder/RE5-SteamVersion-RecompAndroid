# Plano do Projeto — `re5-recompiled`

> Recompilação estática do `re5dx9.exe` (Resident Evil 5: Gold Edition, Steam) para
> C portável, rodando nativo em **Android ARM64** com Vulkan — e, no caminho, em
> **Linux x64**. Este documento diz **o que será feito em cada fase, quem faz cada
> parte (você ou a IA), o que sai de cada etapa e como saber que ela terminou.**
>
> Estado vivo dos marcos, com evidências: [`docs/STATUS.md`](STATUS.md).
> Decisões técnicas e o porquê delas: [`docs/ARQUITETURA.md`](ARQUITETURA.md).
> Números medidos do binário: [`docs/RECON_RE5.md`](RECON_RE5.md).
> Auditoria em curso (2026-10-22): [`docs/PLANO_VARREDURA_GHIDRA.md`](PLANO_VARREDURA_GHIDRA.md),
> varredura com o Ghidra e o jogo original como oráculo (etapas V0–V8).

---

## 1. Onde estamos (20/10/2026, 2ª sessão)

| | |
|---|---|
| **Fase 0 — Fundação** | **Concluída.** |
| **Fase 1 — Binário de entrada** | **Concluída.** O binário de entrada (`bin_input/re5dx9.clean.exe`, um executável x86 sem proteção) é fornecido por quem tem o jogo e nunca vai para o git; este repositório não inclui nem documenta como remover proteções de cópia. |
| **Fase 2 — Lifting** | M0–M2 feitos (70.664 funções, 0 falhas, compila e linka). 2.1 parcial (`cpuid`, `rdtsc`, flags de carry, `pushad/popad`); 2.6 em andamento com `scripts/bringup.py`. |
| **Fase 3 — HLE Win32** | M3 e M4 feitos: o CRT inicializa e o `WinMain` roda até pedir o ícone da janela (`LoadIconW`). 136 de 270 imports com HLE. |
| **Fase 4 — Multimídia** | 4.1 janela (SDL2) feita (M6). 4.2 ponte COM do Direct3D 9 feita no Windows: o jogo cria o device no `d3d9.dll` do host (M7). Faltam o M8 (primeiro `Present`), entrada, áudio e vídeo. |

Estado detalhado e evidências: [`STATUS.md`](STATUS.md).

## 2. Como o trabalho se divide entre você e a IA

| A IA faz | Você faz |
|---|---|
| Escreve e mantém todo o código: scripts do pipeline, patches no lifter, runtime, HLE Win32, pontes D3D9/áudio/vídeo/input, projeto Android | Fornece os arquivos do jogo (cópia legítima) e tudo que exige **sua conta Steam** |
| Roda o pipeline inteiro no container: reconhecimento, catálogo, lift, compilação, testes | Roda o que exige **Windows, GPU real ou celular**: testes visuais, desempenho, APK no aparelho |
| Diagnostica crashes a partir de logs e trilhas de dispatch | Envia logs (`RE5_LOG_FILE`, `adb logcat`) e capturas quando algo falha no seu hardware |
| Mede e documenta: cada marco em `STATUS.md` só vira "feito" com evidência | Decide prioridades, questões legais e quando algo está "bom o suficiente" |
| Faz commit e push no branch de trabalho; abre PR quando você pedir | Revisa e faz merge |

### Limites do ambiente da IA (container na nuvem)

- **Sem Windows, sem GPU, sem celular.** O desenvolvimento acontece em **Linux x64
  headless**; gráficos podem ser validados com Vulkan por software (Mesa lavapipe) e
  capturas de quadro, mas desempenho e o Android real dependem de você.
- **Sem Steam.** A IA não baixa nada da sua conta.
- **Sessões efêmeras.** Tudo que importa precisa estar commitado; o código gerado
  é regenerado a cada sessão (minutos), não versionado.
- **4 núcleos, ~30 GiB livres.** O build completo do código gerado (estimativa: 20–35
  milhões de linhas) deve levar **~1–2 h** aqui. Se ficar lento demais, você pode
  compilar localmente com mais núcleos usando os mesmos presets.

### O ciclo de cada sessão da IA

1. `python3 scripts/doctor.py` — o ambiente está pronto?
2. Ler `docs/STATUS.md` e este plano → escolher a próxima etapa não concluída.
3. Implementar com testes (`ctest --preset linux-debug` precisa ficar verde).
4. Atualizar `docs/STATUS.md` com a evidência (números, logs, capturas).
5. Commit + push no branch. Reportar a você o que mudou e o que precisa de você.

---

## 3. As fases

Cada etapa tem um identificador (`1.1`, `2.3`...) usado em commits, no `STATUS.md` e
nas conversas.

### Fase 0 — Fundação do repositório ✅ concluída

**Objetivo:** um esqueleto em que cada fase seguinte só acrescenta, sem refazer.

| Entregável | Onde |
|---|---|
| Reconhecimento e portão de entrada do lifter | `scripts/recon.py`, `config/known_builds.json` |
| Verificação de ambiente | `scripts/doctor.py`, `scripts/bootstrap.sh` |
| Driver de catálogo + lift (pcrecomp `disasm32` + `lift32_cpu`) | `scripts/run_lift.py` |
| `cpu.h` do pcrecomp adaptado à base do guest, sem fork manual | `scripts/gen_cpu_header.py` |
| Runtime: memória de 4 GiB do guest, dispatch, thunks, registro de imports + censo, handles, loader PE, TEB/PEB, threads e lock do guest, VFS, log | `runtime/` |
| HLE inicial do `kernel32` (15 funções, o padrão para as próximas) | `runtime/src/win32/kernel32.c` |
| Executável host `re5` (`--census` já funciona no binário real) | `runtime/src/host/main.c` |
| Build CMake + presets (Linux, Android, Windows) | `CMakeLists.txt`, `CMakePresets.json` |
| Testes: 4 unitários, 1 ponta a ponta (PE32 sintético compilado com clang → recon → catálogo → lift → executado no runtime), 5 testes Python | `tests/` |
| Plano, arquitetura, status, guia para sessões de IA | `docs/`, `CLAUDE.md` |

**Critério de pronto (atingido):** `ctest` 6/6; o PE sintético — com cdecl, stdcall,
tabela de saltos, globais, 4 imports e `fs:[0x18]` — retorna o valor esperado
executando o C gerado.

---

### Fase 1 — Binário limpo e análise estática

**Objetivo:** um `re5dx9.exe` com código legível, e todo o conhecimento sobre ele
que as fases seguintes consomem.

#### 1.1 Binário de entrada

O binário de entrada (`bin_input/re5dx9.clean.exe`, um executável x86 sem proteção) é fornecido por quem tem o jogo e nunca vai para o git; este repositório não inclui nem documenta como remover proteções de cópia.

#### 1.2 Reconhecimento do build limpo

**A IA:** roda `recon.py --all --markdown docs/RECON_RE5.md`; extrai a IAT real
(esperado: centenas de imports em kernel32, user32, gdi32, advapi32, d3d9, d3dx9?,
xinput1_3, dinput8, dsound, winmm, ws2_32, ole32, wmvcore, steam_api?), imports
com carga atrasada (`pcrecomp/tools/pe/delay_imports.py`) e recursos (`rsrc.py`).
Estende o `recon.py` com o **censo de ISA**: x87, SSE/SSE2, MMX, 3DNow!, `lock`,
`cpuid`, `rdtsc`, `pause`, `cmpxchg8b`, acessos `fs:` — isso define o trabalho no
lifter na Fase 2.

**Entregáveis:** `docs/RECON_RE5.md` atualizado; censo de imports; censo de ISA.

#### 1.3 Política de imports

**A IA:** com `re5 --census --missing --exe bin_input/re5dx9.clean.exe`, classifica
cada import em `config/iat_overrides.json`: HLE real, constante justificada, ou
ponte COM. Ordena o trabalho da Fase 3 pelo que o jogo chama primeiro.

#### 1.4 Recuperação de C++ (RTTI e vtables)

**A IA:** roda `pcrecomp/tools/cpp/rtti.py` e `vtable_scan.py` (cada método virtual é
uma entrada de função comprovada — a MT Framework é C++ com RTTI farto),
`pe/debug_symbols.py` (strings `__FILE__`) e `pe/merge_names.py` → sementes e nomes
(`work/seeds_*.json`, `work/names.json`). Nomes de classes da MT Framework ajudam a
ler crashes nas fases seguintes.

#### 1.5 Classificação

**A IA:** separa CRT do MSVC 2005 (estático), Havok, D3DX (se estático), zlib/libpng
e código do jogo (`pcrecomp/tools/classify/`). Funções de biblioteca bem conhecidas
(memcpy, memset, strlen, math) viram candidatas a override nativo (mais rápidas e
sem casos de borda do lift).

**Pronto quando (Fase 1):** entrada `liftable`; `RECON_RE5.md`, censo de imports e
de ISA commitados; sementes geradas.

---

### Fase 2 — Lifting x86 → C

**Objetivo:** todo o `.text` do jogo como C que compila e linka (marcos M0–M2).

| Etapa | O que a IA faz |
|---|---|
| **2.1 Lacunas do lifter** | Implementar no `lift32_cpu` o que o censo de ISA pedir. Já se sabe que faltam: **`pause`** (vira yield do lock do guest — spinlocks da MT Framework), **`cpuid`** (modelo de CPU fixo: SSE2 sim, extensões novas não, para o jogo escolher caminhos genéricos), **`rdtsc`** (contador virtual), **`cmpxchg8b`** (atômico de 64 bits), **pontos de yield em laços** (`RECOMP_BACKEDGE`, o mesmo patch que o NFSU2 precisou), **sites de hook** (`config/hooks.txt`). Cada um com teste. |
| **2.2 Teste diferencial** | O `difftest` do pcrecomp (Unicorn) cobre o `lift32`, não o `lift32_cpu`. Portar para o `lift32_cpu` e rodar por classe de instrução e em funções folha reais do RE5. |
| **2.3 Catálogo** | `run_lift.py catalog --seeds work/seeds_rtti.json,work/seeds_vtables.json`. Estimativa: **70–100 mil funções** (o `.text` é ~3× o do NFSU2, que tem 27,7 mil). Medir cobertura dos bytes do `.text`; se você puder rodar o Ghidra/IDA headless, pontuar contra ele (`score_recovery.py`). |
| **2.4 Lift (M0)** | `run_lift.py lift` → `generated/src/`. Meta: **0 falhas de lift**; todo `RECOMP_TODO` contado por mnemônico em `lift_stats.json` e justificado (inalcançável) ou eliminado. |
| **2.5 Compilar e linkar (M1, M2)** | `-O1`, unidades de 400 funções, ccache. Linkar com o HLE incompleto: import ausente aborta com o nome, não impede o link. |
| **2.6 Fechar o catálogo** | Cada `dispatch` sem função gerada em tempo de execução imprime o endereço; esses endereços voltam como sementes (`pcrecomp/tools/disasm/seed_from_log.py`) e o ciclo repete. |

**Você:** nada obrigatório. Opcional: rodar Ghidra/IDA headless para dar uma
referência de pontuação ao catálogo.

**Pronto quando:** M0 (código gerado, 0 falhas), M1 (compila), M2 (linka), difftest verde.

---

### Fase 3 — Runtime HLE Win32 (Linux x64, headless)

**Objetivo:** o código do jogo inicializa o CRT e roda o `WinMain` até tentar criar a
janela (marcos M3–M5), respondendo ao Win32 sem Windows e sem Wine.

| Etapa | O que a IA implementa |
|---|---|
| 3.1 Memória | `HeapCreate/HeapAlloc/HeapFree/HeapReAlloc/HeapSize`, `VirtualAlloc/Free/Protect/Query`, `GlobalAlloc`/`LocalAlloc` — heap do guest sobre `re5_mem_alloc`. |
| 3.2 Threads e sincronização | `CreateThread` (thread do host + CPU/TEB/pilha do guest), eventos, mutexes, semáforos, `CRITICAL_SECTION`, `WaitForSingle/MultipleObjects`, TLS (`TlsAlloc` nos slots do TEB), `Interlocked*`. `GetSystemInfo` começa reportando poucos núcleos para a MT Framework criar poucos workers. |
| 3.3 Arquivos | `CreateFileA`, `ReadFile`, `WriteFile`, `SetFilePointer`, `FindFirstFile`, `GetFileAttributes`... sobre o VFS (os `.arc` são lidos pelo próprio código do jogo). Pasta de saves no diretório do usuário. |
| 3.4 Registro | `advapi32` com registro em memória, semeado com os valores do instalador (`BH5AE_install.vdf`) e do `re5_config.toml`. |
| 3.5 Ambiente do CRT | Linha de comando, ambiente, NLS/locale (`GetCPInfo`, `MultiByteToWideChar`, `LCMapString`...), tempo, versão do SO. |
| 3.6 Exceções | Cadeia SEH no TEB e exceções C++ do guest (`_CxxThrowException` → `RaiseException`), portando o `eh32` do pcrecomp. |
| 3.7 Steamworks | Se o build limpo usar `steam_api.dll`: HLE offline (`SteamAPI_Init` OK, sem conquistas, sem rede). |
| 3.8 Diagnóstico | Handler de `SIGSEGV` que traduz o endereço de falha do host para VA do guest e imprime a trilha de dispatch. |

**Métrica de progresso:** o censo (`re5 --census`) — "N de M imports implementados"
— registrado no `STATUS.md` a cada sessão.

**Você:** nada obrigatório. Se tiver Windows, rodar o jogo original com um logger de
API ajuda a desempatar comportamentos duvidosos.

**Pronto quando:** M5 — o `WinMain` do jogo roda, lê configuração e chega ao `CreateWindowEx`.

---

### Fase 4 — Multimídia no PC (Linux; Windows opcional)

**Objetivo:** do primeiro quadro ao gameplay (M6–M11).

| Etapa | O que a IA implementa |
|---|---|
| 4.1 Janela | `user32`/`gdi32` sobre SDL2: classe de janela, laço de mensagens, `WndProc` do guest chamado por `re5_guest_call`. |
| 4.2 Direct3D 9 | Ponte COM: objetos da DXVK Native (64 bits, no host) expostos ao guest como objetos na memória do guest com vtables de thunks, gerados a partir da lista de métodos do `d3d9.h` (como `generate_device_bridge.py` do NFSU2). `Lock/Unlock` com buffers-sombra na memória do guest. **D3DX9:** se o jogo importar `d3dx9_*.dll`, implementar o subconjunto que o censo mostrar. |
| 4.3 Áudio | O RE5 só usa **DirectSound 8** (não há XAudio2 no binário): buffers na memória do guest, mixer próprio na saída de áudio do **SDL2**. |
| 4.4 Input | XInput → `SDL_GameController`; DirectInput8 → teclado/mouse do SDL2. |
| 4.5 Vídeo | `WMCreateSyncReader` (WMVCore) → **FFmpeg** mínimo (`scripts/build_ffmpeg.sh`: asf + VC-1/WMV3 + WMA Pro/WMA2). |
| 4.6 Validação headless | Mesa lavapipe + SDL offscreen + captura de quadros: capturas e hashes de quadro commitados como evidência e teste de regressão. |

**Você:** rodar no seu PC Linux (ou Windows) com GPU real, jogar o capítulo 1-1 e
reportar o que estiver errado (com `RE5_LOG_FILE`).

**Pronto quando:** M11 — gameplay do capítulo 1-1 no Linux x64, validado por você.

---

### Fase 5 — Android ARM64

**Objetivo:** APK que roda o jogo num aparelho Snapdragon (M12–M13).

| Etapa | O que a IA faz |
|---|---|
| 5.1 Build NDK | Runtime + código gerado com o preset `android-arm64` (Clang do NDK). |
| 5.2 Memória | A mesma reserva de 4 GiB (`mmap` sem commit funciona no espaço de 39/48 bits do ARM64). |
| 5.3 Ordenação de memória | Com o lock global do guest, o TSO do x86 só importa entre threads do host (áudio/render) e dados do guest — tratado com atômicos explícitos nas pontes. |
| 5.4 Ponto flutuante | x87 modelado em `double` (como no PC); se o jogo ligar FTZ/DAZ via `ldmxcsr`, espelhar no `FPCR`. |
| 5.5 Vulkan no Android | WSI do `ANativeWindow` para a DXVK (portando `ports/dxvk_android` do NFSU2); `libadrenotools` para drivers Turnip. |
| 5.6 App | Projeto Gradle, Activity com SDL2, launcher que aponta a pasta de dados do jogo (SAF), áudio AAudio. |

**Você:** instalar o APK no aparelho, copiar os dados do jogo e mandar `adb logcat`.

**Pronto quando:** M13 — o APK chega ao menu principal num aparelho real.

---

### Fase 6 — Polimento e desempenho

| Etapa | Conteúdo |
|---|---|
| 6.1 Controles touch | Overlay ImGui com editor de layout e presets (andar, mirar, atirar, recarregar, giro rápido, inventário, comandos à parceira); gamepad. |
| 6.2 Desempenho | Perfil com `simpleperf`; overrides nativos nas funções quentes; `-O2` nas unidades quentes; **relaxar o lock global do guest** (decisão D5) com estratégia de TSO e testes de estresse. |
| 6.3 Gráficos | Escala de resolução, limite de FPS, cache de pipelines, ajustes para Turnip. |
| 6.4 Cutscenes | Custo do WMV no celular; opção de pré-transcodificar. |
| 6.5 Ciclo de vida | Saves, configurações, pausa/retomada sem perder a superfície. |

**Pronto quando:** 30 FPS estáveis com controles touch num Snapdragon 8 Gen 2/3
(60 FPS é meta estendida — o NFSU2 chegou a 35–51 FPS no S24 Ultra).

---

## 4. Marcos

| Marco | Significa | Fase |
|---|---|:---:|
| M0 | C gerado para todo o `.text`, 0 falhas de lift | 2 |
| M1 | Todo o C gerado compila | 2 |
| M2 | O executável `re5` linka com o código gerado | 2 |
| M3 | O CRT do jogo inicializa (heap, TLS, locale) | 3 |
| M4 | O entry point original chama o `WinMain` | 3 |
| M5 | O `WinMain` lê configuração e chega ao `CreateWindowEx` | 3 |
| M6 | Janela criada (SDL2) | 4 |
| M7 | `Direct3DCreate9` + `CreateDevice` OK (DXVK) | 4 |
| M8 | Primeiro `Present` | 4 |
| M9 | Vídeos de abertura tocam | 4 |
| M10 | Menu principal navegável | 4 |
| M11 | Gameplay do capítulo 1-1 no PC | 4 |
| M12 | `libre5_recomp.so` roda no Android | 5 |
| M13 | APK chega ao menu num aparelho real | 5 |
| M14 | Jogável no celular: 30 FPS estáveis com controles touch | 6 |

---

## 5. Riscos principais

| Risco | Impacto | Mitigação |
|---|---|---|
| Build antigo incompatível com os dados atuais | Crashes ao carregar `.arc` | Baixar também o depot de dados do mesmo período |
| Lacunas e bugs do `lift32_cpu` (menos testado que o `lift32`) | Comportamento errado silencioso | Censo de ISA primeiro (1.2), difftest portado (2.2), falhar alto (`RECOMP_TODO`) |
| Escala do build (20–35 M linhas, 4 núcleos) | Ciclos lentos | `-O1`, TUs pequenas, ccache; build local opcional com mais núcleos |
| Dependência de D3DX9 | Muito trabalho de HLE | Censo na 1.2; implementar só o subconjunto usado |
| Spinlocks da MT Framework + lock global | Deadlock | `pause` e laços viram yield (2.1); chamadas bloqueantes soltam o lock |
| Desempenho no ARM64 com lock global | FPS baixo | Medir cedo; relaxar o lock na 6.2 com testes de estresse |
| Validação gráfica sem GPU no container | Bugs só aparecem no seu hardware | lavapipe + hashes de quadro; seus testes nos marcos M8+ |
| Jurídico | Distribuir material da Capcom | Nenhum arquivo do jogo no git (`bin_input/`, `generated/`, `work/` ignorados); sem ferramentas de remoção de proteção no repositório |

---

## 6. O que preciso de você agora

1. Nada bloqueia a IA agora. Depois do M7 o bring-up para numa lacuna do lifter
   (`rcpss` em `0x00E64C4F`, etapa 2.1); fechada ela, o alvo é o M8 (primeiro
   `Present`).
2. Nenhum arquivo do jogo (executáveis, dados, saves, despejos) vai para o git.
