# Status

Só é marcado como feito o que tem evidência. Plano completo:
[`PLANO_DO_PROJETO.md`](PLANO_DO_PROJETO.md).

## 2026-10-20 (2ª sessão) — binário limpo refeito no OEP; CRT e WinMain rodam até a janela

O `re5.exe` da sessão anterior "abria e fechava" porque o `re5dx9.clean.exe` era
inválido como entrada: entry point forçado em `0x00401000` (primeiro byte do
`.text`, não o OEP), diretório de imports zerado e `.data` despejado com o jogo já
rodando (F11). O runtime chamava uma função qualquer, ela retornava 0 e o processo
saía. As afirmações "OEP 0x00E4A000", "735 IAT entries" e "roda o entry point com
sucesso" da versão anterior deste arquivo não tinham evidência e estavam erradas.

| Etapa / marco | Estado | Evidência |
|---|---|---|
| **Fase 0** fundação | **FEITO** | `ctest` (Windows, MinGW GCC 15): guest_memory, handle_table, dispatch, kernel32 passam; `python_pipeline` passa com o Python 3.11 (17 testes; ver lacunas) |
| **V0** varredura Ghidra (prep.) | **FEITO** | Projeto oficial `work/ghidra/re5.gpr` salvo em 508 s (26.705 funções, 433 classes C++, 117 thunks); opções reproduzíveis fixadas por `ConfigureAnalysis.java`; driver `ghidra_sweep.py` com testes verdes (`test_ghidra_sweep.py`); `doctor.py` atualizado. Detalhes em `docs/AUDITORIA_GHIDRA.md`. |
| **V1** exportações Ghidra | **FEITO** | 12 arquivos JSON gerados em `work/ghidra/export/` (26.436 funções, 2.540 switches, 185k ponteiros, 1.681 métodos RTTI); `work/names.json` gerado com 26.442 nomes de símbolos. |
| **V2** auditoria do catálogo | **FEITO** | `scripts/audit_catalog.py` implementado e testado (`test_audit_catalog.py` com ctest verde); 96,87% de fins idênticos (0 caudas de código perdidas); 50 sementes geradas em `work/seeds_ghidra.txt` (7 funções com chamadores + 43 alvos de switch); catálogo estendido para 72.121 funções (+111 entradas). Pós-semeadura: 0 funções com referência fora do catálogo e 0 alvos de switch pendentes. |
| **E1** teste de baixo fps no original | **FEITO** | Usuário testou original com RTSS travado em ~20 fps na escada e benchmark; jogo opera normalmente. Causa física/delta-time descartada. |
| **1.1** binário de entrada | **FEITO** | `bin_input/re5dx9.clean.exe`: entry `0x00E68A06`, 270 imports em 18 DLLs; `recon.py`: `liftable`. O binário de entrada (`bin_input/re5dx9.clean.exe`, um executável x86 sem proteção) é fornecido por quem tem o jogo e nunca vai para o git; este repositório não inclui nem documenta como remover proteções de cópia. |
| **1.2** reconhecimento | **FEITO** | `docs/RECON_RE5.md` regenerado; imports em `work/imports.json` (14 slots com apelidos de mesmo endereço, ex.: `htons`/`ntohs`). Sem `d3dx9`; `steam_api.dll` com 18 imports. |
| **2.1** lacunas do lifter (parcial) | **FEITO** p/ `cpuid`, `rdtsc`, `stc`, `clc`, `cmc`, `pushal`, `popal`, `rcpss`, `rsqrtss`, `rcpps`, `rsqrtps`, `fxam`, `xlatb`, `fnstenv`/`fldenv`, `fnsave`/`frstor`, `pmaxsw`/`pminsw`/`pmaxub`/`pminub` (xmm) e o NOP de dica `0F 18 /r` com registrador | `rcp*`/`rsqrt*`: o valor exato, o mesmo modelo do `lift32_sse.py`. `fxam` (3 no binário, CRT em `0x00E71F24`): classe de st(0) em C3/C2/C0 e sinal em C1; com a pilha de `double` (D9) não existem as classes "vazio" e "subnormal" (um subnormal de `double` é normal no formato de 80 bits). `fnstenv`/`fldenv` (CRT `0x00E75AA3`, devolve códigos de condição pelo status word): controle fixo, status com C0-C3 e TOP, que voltam no `fldenv`; `fnsave`/`frstor` (`0x00E66D3D`) somam os 8 registradores em 80 bits e o `fnsave` reinicia a pilha. `0F 18 C9` (em `0x00456094`, no laço de render): NOP reservado desde o P6 que o capstone 5 recusa; a descoberta (`disasm32.decode`) e o lift (`lift32_cpu._disasm`) passam a lê-lo como `0F 1F /0` do mesmo tamanho; antes a função era cortada ali e o laço do `bringup.py` girava 30 voltas na mesma semente. `RECOMP_TODO` 2.820 → 1.390; com o catálogo maior das sementes, 1.497 → 1.496 com o `rcpss` (`lift_stats.json`). `cpuid` = Core 2 com SSE2 e sem SSE3; `rdtsc` = relógio monotônico a 3 GHz (`runtime/src/core/cpu_ext.c`). Teste: `tests/python/test_lifter_gaps.py`, `test_dispatch`, `disasm32.py --selftest`. Os restantes são quase todos dados decodificados como código (`out`, `aaa`, `arpl`…). Faltam `pause`, `cmpxchg8b`, yield em laços. |
| **2.3** catálogo | **FEITO** | `run_lift.py catalog` → 70.664 funções em 834 s, entry `0x00E68A06` incluído |
| **2.4 / M0** lift | **FEITO** | 70.664 funções, **0 falhas**, 181 arquivos (janelas de 64 KiB, `lift.bytes_per_file`) |
| **2.5 / M1, M2** compilar e linkar | **FEITO** | MinGW GCC 15, `-O1`, 16 núcleos: build completo ~45 min; `re5.exe` 143 MB |
| **2.6** fechar o catálogo | **em andamento** | Runtime grava cada endereço sem corpo em `RE5_SEED_LOG`; `run_lift.py catalog --add-seeds` acrescenta só essas entradas (3 s); lift com cache por janela; `scripts/bringup.py` faz o laço inteiro. Sementes até agora (6): `0x00804DB0`, `0x00E64010`, `0x00E6402B`, `0x00EB49D6` (o catálogo pôs um início falso em `0x00EB49D5`, no meio de um `fstp`, e cortou a função verdadeira ali), `0x00EE2350`, `0x00F086F0` - três delas em inicializadores estáticos. O lift reaproveita as janelas que não mudaram (cache em `lift_stats.json`). |
| **M3** CRT inicializa | **FEITO** | Com `RE5_LOG=trace`: `__security_init_cookie`, `GetStartupInfoA`, `GetVersionExA`, `_heap_init` (`HeapCreate`), `_mtinit` (TLS + seções críticas; `FlsAlloc`/`EncodePointer` ausentes → o CRT usa o caminho do XP, como deve), `_ioinit`, ambiente, linha de comando e os construtores estáticos (`_initterm`) rodando código do jogo. |
| **M4** entry chama o `WinMain` | **FEITO** | `__tmainCRTStartup` chama `WinMain` = `0x00713F40` (`call` em `0x00E68998`); a trilha de dispatch passa por ele. |
| **M5** `WinMain` lê configuração e chega ao `CreateWindowEx` | **FEITO** | Sequência medida (`RE5_LOG=trace`): `SHGetFolderPathA(CSIDL_PERSONAL|CREATE)` → `CoInitialize` → `GetSystemMetrics` → `GetPrivateProfileStringA` → `AdjustWindowRect` → `SystemParametersInfoA` → `GetUserDefaultLangID`/`LocaleNameToLCID` → `CreateMutexA` → `LoadIconW`/`LoadCursorA` → `RegisterClassExW` → `CreateWindowExW`. |
| **M6** Janela criada (SDL2) | **FEITO** | Classe `MTFramework` (WndProc `0x00423C80`); janela SDL2 com área cliente **1280x720** (o jogo pede 1286x748 de retângulo externo, `AdjustWindowRect` com moldura fixa de 3 + título de 22; o SDL recebe só o cliente). `HasCPUID` (`0x00426230`, testa `EFLAGS.ID` com `pushfd/popfd`, bit que o `eflags_unpack` do lifter não guarda) é override pelo lift (`config/overrides.txt`). Teste: `tests/unit/test_user32.c`. |
| **3.7** Steamworks offline | **FEITO** | SDK 1.30, versões lidas do `steam_api.dll` do jogo. Objetos de interface com vtables de thunks thiscall; índice sem handler aborta com `ISteamX::#N`. `SteamAPI_Init`, `SetWarningMessageHook`, `SetOverlayNotificationPosition`, `BIsDlcInstalled(352000/352001)` passam. Teste: `tests/unit/test_steam_api.c`. |
| **4.2** ponte COM do Direct3D 9 | **FEITO** (Windows) | Ver "Ponte D3D9" abaixo. `tests/unit/test_d3d9.c` (backend falso), `tests/unit/test_d3d9_host.c` (d3d9.dll do Windows: AMD Radeon(TM) Graphics, vs/ps 3.0, `CreateDevice` 320x240, `Clear`/`Present`, travas de VB e DXT1), `tests/python/test_gen_d3d9_bridge.py`. |
| **M7** `Direct3DCreate9` + `CreateDevice` | **FEITO** | `bringup.py` volta 9 (`work/bringup/run_009.log`): `Direct3DCreate9(SDK 32)` no `C:\WINDOWS\SYSTEM32\d3d9.dll` → `GetAdapterIdentifier`, `GetAdapterModeCount`, `EnumAdapterModes` (88), `GetAdapterDisplayMode`, `CheckDeviceFormat` (12), `CheckDeviceMultiSampleType` (58), `GetDeviceCaps` → `CreateDevice(adaptador 0, HAL, foco 0x10010, flags 0x56)`: 1280x720 X8R8G8B8, janela, 1 back buffer, `D3DPRESENT_INTERVAL_IMMEDIATE` → `D3D_OK`. Depois o jogo roda `TestCooperativeLevel`, `GetAvailableTextureMem`, `GetBackBuffer`, `SetRenderTarget`, `Clear`, `CreateTexture` (9), `CreateCubeTexture`, `LockRect/UnlockRect` (24 de textura, 42 de cubo), `CreateVertexDeclaration` (4), `CreateVertexBuffer` (3), `CreateIndexBuffer` (2) e `IDirect3DVertexBuffer9::Lock` (contagens de uma execução com `RE5_LOG=trace`). |
| **M8** primeiro `Present` | **FEITO** | `bringup.py` (log guardado da volta 30 da 3ª rodada): `CreateDevice` → DirectInput8 (mouse e teclado) → `DirectSoundCreate8` = `DSERR_NODRIVER` (sem áudio, ver 4.3) → `WSAStartup(2.2)` → **`primeiro Present -> 0x00000000`** → `Reset: 1280x720 formato 21 (A8R8G8B8), tela cheia -> 0x00000000`, e o jogo segue renderizando (`SetRenderState`...) numa thread de trabalho até a próxima parada. Para chegar aqui: `rcpss` no lifter (2.1), DirectInput8 e XInput (4.4), `SystemParametersInfoA` de acessibilidade, `winmm`, `ws2_32`/`iphlpapi` de uma máquina sem rede e `SuspendThread` de thread em execução. |
| **4.4** entrada (parcial) | **FEITO** p/ teclado, mouse e XInput | `runtime/src/win32/dinput8.c`: `IDirectInput8A` com vtables de thunks stdcall; o sistema tem `GUID_SysKeyboard` e `GUID_SysMouse` e nenhum controle DirectInput (`EnumDevices(DI8DEVCLASS_GAMECTRL)` não chama o callback). `c_dfDIKeyboard` (DIK_* = scancode set 1, `0x80` nas estendidas, Pause/Num Lock trocados) e `c_dfDIMouse2` (movimento relativo acumulado entre leituras, roda em múltiplos de 120). O estado vem do `user32`, o único leitor da fila do SDL (`re5_user32_handle_event`). `runtime/src/win32/xinput1_3.c`: os 4 usuários do XInput são os `SDL_GameController` na ordem em que aparecem (procura de novos no máximo 1x/s), eixo Y invertido, gatilhos 0..255. Testes: `tests/unit/test_dinput8.c` (GUIDs, `c_dfDIKeyboard` e `c_dfDIMouse2` vindos do `libdxguid`/`libdinput8` do MinGW), `tests/unit/test_xinput.c`. Falta: `GetDeviceData` (buffer), `DISCL_EXCLUSIVE` no mouse, vibração contínua (o SDL pede duração: 5 s por chamada). |
| **4.3** áudio | **FEITO** (Windows; ouvido no jogo) | O DirectSound é o motor de som inteiro do RE5 (não há XAudio2 no binário; `IID_IDirectSoundBuffer8` em `0x0107F2F4`). `runtime/src/win32/dsound.c`: os dados dos buffers secundários ficam no heap do guest e o `Lock` devolve ponteiros diretos (duas regiões na volta); um mixer próprio no callback do áudio do SDL2 (48 kHz estéreo float) faz reamostragem linear, volume e pan em centésimos de dB, downmix de 5.1 e anda os cursores (escrita 10 ms à frente); `DuplicateSoundBuffer` divide os dados; `GetCaps` com os bits que o jogo exige; `GetSpeakerConfig` = estéreo. Teste: `tests/unit/test_dsound.c` (sem dispositivo, mixer chamado à mão). No jogo (2026-10-21, `work/bringup/av_001.log`): `saída de áudio do host: 48000 Hz, 2 canais, 512 quadros por bloco` e `buffer primário: 48000 Hz, 2 canais, 16 bits` - o jogo cria o som e segue até o `Present`. Música e som de fundo chiavam e falhavam (2026-10-21): o `RE5_DSOUND_DUMP` (agora com `events.txt` de `Lock`/`Unlock`/cursor) mostrou o streaming perfeito - o jogo escreve 48000 quadros/s, contíguos, sempre >= 27 ms à frente - e os cliques já nos dados do jogo (2754 amostras de fundo de escala, 19791 saltos > 20000 em 56 s). Causa: o lift32_cpu ignorava o `fldcw`, e os 1222 `fistp` truncados do RE5 (idioma `or 0xC00; fldcw; fistp` do MSVC, posições e índices do motor de som) arredondavam ao mais próximo; ver a nota do lifter abaixo. Depois: 0 amostras de fundo de escala e 0 saltos em 139 s (`work/snd3`), música do título com corte limpo em ~16,5 kHz e energia acima de 17 kHz a -58..-85 dB do resto (antes -20..-40 dB); o usuário confirmou "o som está ótimo". |
| **4.5** vídeo | **FEITO** p/ o leitor (o jogo ainda não chega aos vídeos) | `runtime/src/win32/wmvcore.c`: `IWMSyncReader` sobre o FFmpeg mínimo de `scripts/build_ffmpeg.sh` (asf + vc1/wmv3 + wmapro/wmav2, estático; o CMake procura em `RE5_FFMPEG_DIR`, padrão `build/ffmpeg`). O `IStream` do jogo é a fonte do `AVIOContext` (`Read`/`Seek`/`Stat` por `re5_guest_call`); vídeo em YV12 (`VIDEOINFOHEADER`), áudio em PCM 16 bits estéreo ou, com `EnableDiscreteOutput`, os 6 canais em `WAVEFORMATEXTENSIBLE`; tempos em 100 ns (quadro sem tempo segue o relógio da saída), `WM_SF_CLEANPOINT`/`DISCONTINUITY`, `IWMHeaderInfo::Duration`, `SetRange(0)` volta ao começo; a decodificação roda sem o lock do guest. Teste: `tests/unit/test_wmvcore.c` com o `bh5logo.wmv` do jogo (pula sem ele): `409 quadros de vídeo, 320 blocos de áudio, último tempo 13.70 s, duração 14.11 s, luma máx 254`, em 1,1 s. |
| **M9** vídeos de abertura | **FEITO** (Windows) | `work/bringup/av_004.log`: `wmvcore saída 0: stream 1, áudio wmapro` / `saída 1: stream 2, vídeo vc1` e o jogo segue (>10000 Present); o usuário viu as cenas na janela (2026-10-21). Destravado por: fila de espera com entrega direta no `LeaveCriticalSection` (um laço `Enter/Leave` de 2 dispatches era sempre preemptado com a seção na mão e a thread de arquivos nunca entrava: "NOW LOADING" eterno) e `IDirect3DVolumeTexture9::LockBox/UnlockBox`. Testes: `test_kernel32` (entrega da seção), `test_d3d9` (volume com pitches do host). |
| **M10** menu principal navegável | **FEITO** (Windows) | O usuário chegou ao menu e abriu o benchmark pelo menu (2026-10-21: "jogo abre o menu", "entrou no benchmark"); a tela de título capturada em `RE5_D3D9_SHOT` (Present 3000). Lacunas do catálogo no caminho: `0x00A16B19`, `0x004579C1`, `0x00796470` (+139 entradas achadas a partir delas). |
| **4.2b** Vulkan (DXVK) no Windows | **FEITO** | `scripts/build_dxvk.sh 2.6.2` e `3.1.1` (MinGW GCC 15, meson 1.12.1, glslang 16.6.0) -> `dxvk/<versão>/d3d9.dll` ao lado do `re5.exe`. A sonda Vulkan do runtime (`d3d9_host.c`) no PC do usuário: `sem VK_KHR_maintenance5, não serve à DXVK 3.1.1` / `AMD Radeon(TM) Graphics (Vulkan 1.3.260, driver 0x800117): serve à DXVK 2.6.2` / `Direct3D do host: ...\dxvk.6.2\d3d9.dll`, `CreateDevice` ok, >1000 `Present`; quadro 5000 capturado (`work/shots_vk/frame_5000.bmp`, tela "History of RESIDENT EVIL" correta). Antes a 3.1.1 derrubava o processo com uma exceção C++ sem tratamento (`0x20474343`) quando a GPU não servia. Android (Mali do Exynos 1380): escolhida a DXVK native 1.9.2b com o porte do NFSU2; ainda não compilado (sem NDK nesta máquina). |
| benchmark | **roda**, com defeitos | Modos do menu ("Benchmark variable/fixed"). Defeitos relatados pelo usuário: som abafado e com ruído alto, física quebrada, tela pisca às vezes, controle do Xbox 360 com problemas; desempenho na linha abaixo. |
| **6.2** desempenho (adiantada, Windows x64) | **em andamento** | Perfil próprio (`RE5_PROFILE=<s>`: amostra o RIP das threads a cada 1 ms e mapeia para a função gerada pela tabela de dispatch). Antes (lock global, runtime em -O0): lock ocupado 94,7% dos ticks, **72% das amostras esperando o lock**, código gerado só 20% (`work/re5_run.log`, benchmark do usuário a ~10 fps). Feito: runtime em `-O2`; **lock só no HLE** (`RE5_GUEST_LOCK=hle`, padrão no x86-64: o código do guest roda em paralelo; o RE5 não tem nenhum `lock`/`cmpxchg`/`xadd` em linha - contagem com capstone nas 70864 funções do catálogo - então toda sincronização passa pelo kernel32); `Interlocked*`, TLS, `LastError`, relógios sem lock e com atômicos do host; `timeBeginPeriod` repassado ao host (sem ele cada `Sleep(1)` do runtime durava ~15,6 ms: 32% dos ticks sem nenhuma thread rodando); `GetSystemInfo` com os núcleos do host (até 8) no modo hle (com 2, os 6 workers da MT Framework ficavam com 0 dispatches). Resultado: demo do jogo de 7,6 para ~20 Present/s no perfil (`work/bringup/perf_hle2.log`); o usuário mede **20-25 fps** no jogo e no benchmark (antes ~10). Testes: `kernel32` nos dois modos (`kernel32_hle`: guest chamado de um handler roda sem o lock; 4 threads x 100000 `InterlockedIncrement` = 400000). Pendente: uma execução hle parou com acesso inválido em `0x18060014` (thread principal) e não se repetiu; `EnterCriticalSection` ainda passa pelo lock do HLE. Rodada 2026-10-21 (2ª): `RE5_PROFILE` passou a dizer, nas esperas do lock, em que thunk a thread quer entrar e o que a dona do lock roda. No demo do jogo (4,5 Present/s) o `dispatch` era a função do host mais amostrada e 33% das amostras estavam em "runtime/outros". Feito: tabela hash no `dispatch` (era busca binária em 70.864 entradas) e trilha num único bloco de TLS (o TLS do MinGW é emulado); chamadas diretas `RE5_CALL` para alvos constantes já gerados (`lift.direct_calls`; num arquivo amostra 2036 de 2057 `dispatch(c, constante)` viraram diretas); lock do HLE com variável de condição (antes a thread da janela dormia 1 ms em laço com o lock livre: 7% das amostras); `Sleep` sem o lock. Perfil depois, mesmo tipo de trecho: ~21 Present/s, "runtime/outros" 14-15%. O usuário mede 7-10 fps na primeira área e 23-32 depois de uma cutscene; CPU e GPU longe do máximo. Rodada 2026-10-21 (3ª, Fases 3-4 do plano): no carregamento (`work/perf/load1.log`) 48% das amostras eram threads esperando o lock do HLE só para entrar em `EnterCriticalSection`, com o lock em `IDirect3DVertexBuffer9::Unlock` (22%, cópia do buffer) e `VirtualFree` (18%, `memset` do `MEM_DECOMMIT`). Feito: seções críticas sem o lock do HLE (64 mutexes do host por faixa, fila com entrega direta, espera em variável de condição); eventos/semáforos/mutexes/esperas, `VirtualFree` (páginas devolvidas ao host com `re5_mem_zero`) e os métodos de consulta/controle dos buffers do DirectSound sem o lock; cópia do `Unlock` de buffers/texturas com o lock solto; `rep stos`/`rep movs` em bloco no lifter (o `memset` do CRT, `0x00E63D00`, era a função do guest mais amostrada); `CPU *restrict c` nas funções geradas (`re5_0166.c` 840.592 -> 801.180 bytes de código) com `RE5_LOOP_BARRIER()` nas cabeças de laço. Depois: espera pelo lock no carregamento 58% -> 2-5% (`work/perf/load3.log`), até 2,7 threads rodando ao mesmo tempo no carregamento, código gerado 77% das amostras (`work/perf/final2.log`); o vídeo de abertura chegou em ~5 s com o perfil ligado (antes ~15 s); sem o perfil, 9,5-10,0 s em 5 rodadas (o tempo das logos é fixo, então esse número mede pouco). Não feito: `inflate` da zlib 1.2.1 nativo (`0x006CC2C0..0x006CF940`, quente no carregamento; precisa vendorizar a zlib), `-O2` no código gerado (1,8x o tempo de build por ~1% de código). Fica para o usuário medir no jogo: fps na primeira área e se as telas de loading extras sumiram. |
| M11–M14 | não iniciados | |

### Mudanças no lifter (tools/pcrecomp, 2026-10-21)

- Palavra de controle do x87 (`CPU.fpu_cw`, iniciada em 0x027F por thread): `fldcw`
  e `fnstcw` reais, `fistp`/`fist`/`frndint` arredondam pelo RC (`fpu_rint`), estouro
  e NaN gravam o inteiro indefinido, `fnstenv`/`fldenv`/`fnsave`/`frstor`/`fninit`
  levam a palavra. No RE5: 9.125 `fistp`, 1.222 logo depois de um `fldcw` (`or 0xC00`
  x1210 truncar, `or 0x400` x187 floor, `or 0x800` x338 ceil) em 577 funções.
- `ldmxcsr`/`stmxcsr` guardam o MXCSR; FTZ/DAZ vão para o SSE do host (a thread de
  som, `0x0048C590`, liga FTZ antes dos filtros).
- `CPU.ncalls`: contador das chamadas diretas `RE5_CALL` (ponto de preempção sem TLS).
- Lift completo refeito: 70.864 funções, 0 falhas, `RECOMP_TODO` igual (1.353).
  Testes: `tests/python/test_lifter_gaps.py` (`test_rounding_control`, `test_mxcsr`),
  `tests/unit/test_x87.c` (truncar/floor/ceil/par, indefinido, ambiente, thread nova,
  FTZ no host); `ctest` 19/19.

### Etapa 2.2: teste diferencial do lift32_cpu (2026-10-21)

`scripts/difftest_cpu.py` (e o teste `difftest_cpu` do ctest): cada forma de
instrução do RE5 (mnemônico + tipos/tamanhos dos operandos, tiradas das funções do
catálogo) roda no Unicorn 2.1.4 e no C do lift32_cpu com o `cpu.h` gerado do jogo, a
partir do mesmo estado aleatório (registradores, flags, pilha x87, XMM, 20 MiB de
memória), e compara registradores, flags definidas pela instrução, TOP e valores do
x87, C0/C2/C3 das comparações, XMM e cada byte escrito. Achados e corrigidos:
`movhps`/`movhpd` moviam a metade BAIXA (o lifter olhava `m[4]`); `sar` de 8/16 bits
com contagem >= largura dava 0 (deslocamento negativo em C); `shl` idem no CF;
`rcl`/`rcr` sem a máscara de 5 bits antes do módulo; `rol`/`ror` com contagem múltipla
da largura não punham CF; `fprem1` truncava como `fprem` e nenhum dos dois punha o
quociente em C0/C3/C1; `fnstsw` sem o TOP. Excluídos por erro da referência (QEMU):
`fprem`/`fprem1` (calcula em double aproximado: 261,4498 rem1 -250,1177 deu -238,79,
o exato é 11,3321), `fyl2x` de argumento negativo (não desempilha), OF/SF do `fcomi`
(o SDM zera; o QEMU preserva). Resultado final: 1.246 codificações, 7.174 casos com
NaN/inf/-0/subnormais em 25% dos valores: 7.172 iguais ao hardware, os 2 restantes
eram o `rol` corrigido em seguida.

### Lifter: flag de direção (DF) (2026-10-23)

`std` virava comentário ("DF=1 unsupported; string ops assume forward") e toda operação
de string andava para frente. O CRT do RE5 usa DF=1 em dois lugares: `strrchr`
(0x00E64680, `std; repne scasb`) e o caminho para cima do `memmove`/`memcpy`
(0x00E69D6F, 0x00E6409F..0x00E64156, `std; rep movsd`). Efeito medido: o `strrchr`
devolvia NULL e o registro das malhas de colisão do estágio (0x0072BC80: sufixo depois do
último `_` do nome, tabela {"scr" -> 1, "eff" -> 2}) caía no padrão "scr", registrando a
malha `*_eff` com a máscara de consulta 1 (personagem) em vez de 2 (itens/efeitos).
No despejo da escada, com `scripts/uc_call.py` (novo: chama uma função do jogo no
Unicorn sobre uma captura): a checagem de chão do drop (0x007BDDD0 -> 0x0076E520, tipo 2)
falha em todo ponto; com `elem[eff]+0x1C` = 2 ela acerta o chão real (-133,2; -157,9) e
o tipo 1 passa a ver só a rampa `scr`. O pedido de drop (0x007C0CA0) descarta o item
quando essa checagem falha. `strrchr("...s115h_eff", '_')` no Unicorn: "_eff".
Correção: `CPU.df` (cpu.h, no fim da struct), `std`/`cld` gravam, `eflags_pack/unpack`
com o bit 10, `STR_STEP` em lods/stos/movs/scas/cmps, `rep_movs`/`rep_stos` em bloco
para trás (sem replicar quando o memmove não replicaria). `difftest_cpu.py` sorteia e
compara DF: movs/stos/lods/scas/cmps/std/cld 270/270 iguais ao Unicorn. Testes:
`test_lifter_gaps.test_direction_flag`, `test_x87` (cópia para trás com sobreposição).
Falta medir no jogo: drops, escada e câmera com o build novo.

### Otimização do código gerado: -O2 (2026-10-22)

`scripts/bench_re5.py` roda o benchmark variável sozinho (`RE5_INPUT_SYNC=titlelogo`
+ `RE5_INPUT_SCRIPT`, fps por segundo com `RE5_FPS_LOG=1`). Base `-O1`: mediana 42,8 fps
(p10 35,9) em 356 s. `-O2` (`-DRE5_GENERATED_OPT=-O2`, build 19 min contra 11):
nas mesmas áreas, sem ganho medível (área 1 ~50-60 fps nos dois), e as duas execuções
pararam no meio (`ExitProcess(0)`, uma sem causa no log). Padrão continua `-O1`.

### Colisão: escada como parede, personagem caindo (aberto, 2026-10-22)

Relato do usuário: em duas escadas (capítulo 1-1 e benchmark variável) há uma
"parede invisível" (com dano ele sobe um degrau e volta), no benchmark o personagem
cai e levanta em laço e não passa de um container. O jogo original (mesmo código:
mesmo `.text` do `clean.exe`) passa por tudo. Descartado, com medida:
- tradução de instrução: `scripts/difftest_cpu.py`, 7.174/7.174 casos iguais ao
  Unicorn (com NaN/inf/-0/subnormais);
- matemática do CRT: `scripts/difftest_func.py` (função inteira no Unicorn x C, com a
  imagem mapeada): as `__libm_sse2_*` que o jogo chama (`0x00E64BED`, `0x00E66572`,
  `0x00E66E1A`, `0x00E679E6`, `0x00E67EE5`, `0x00E68331`, `0x00E65200`, `0x00E646AD`,
  `0x00E6476B`, `0x00E64923`) bit a bit iguais em 670 entradas cada;
- fronteiras do catálogo: só 3 inícios de função no meio de uma instrução, benignos;
- precisão do x87: só o CRT (`pow`/`exp`/`log`) muda a palavra de controle.
Próximo teste: `RE5_GUEST_LOCK=global` (threads do jogo uma por vez). Lacunas do
catálogo fechadas no caminho: `0x00E69D1C` (benchmark variável) e `0x00C212D0`, mais
~1.150 alvos de chamada constantes que estavam fora do catálogo.

2026-10-23:
- FTZ/DAZ por padrão: toda thread do guest nasce com MXCSR 0x9FC0 e o SSE do host
  (FPCR.FZ no ARM64) com os dois bits (`re5_guest_mxcsr_default`, `RE5_FTZ=0` volta
  ao 0x1F80 do Windows; D9 atualizada). Visa a queda de fps por subnormais. Não deve
  mudar a colisão em si: o jogo original roda sem FTZ e colide certo. Testes:
  `dispatch` e `x87` verificam o 0x9FC0 e o produto subnormal zerado; ctest 19/19
  (sem o `difftest_cpu` longo, rodado à parte abaixo).
- Tracepoints do filtro do Havok (`config/tracepoints.txt`). A vtable do
  hkpGroupFilter (0x0105BD44) mostra que toda variante de `isCollisionEnabled` chama o
  núcleo `0x00D99740(hkBool *res, infoA, infoB)`; raio x collidable `0x00D99A40`, raio
  x subshape (MOPP) `0x00D997F0`, collidable x collidable `0x00D99A10`, mais os
  callbacks de broadphase dos raycasters (`0x00DA5170`, `0x00DA5A60`, `0x00DA7E20`).
  A saída do trace agora mostra `*eax` (o hkBool devolvido por ponteiro). O
  `0x00D36BD0` (nome do RTTI "hkpRayCollidableFilter") é o destrutor, não um filtro.
  Liftados: 17 funções rastreadas, 1 falha de lift já conhecida (`0x0094818B`, MMX).
- Auditoria 32->64: `difftest_cpu.py --per-group 3 --seeds 5 --special 0.35` em
  SSE/x87 (466 codificações, 2.286 casos): SSE sem divergência; 8 casos x87, todos de
  borda (`fsin`/`fptan` com |x| >= 2^63, que o x87 devolve intacto com C2; `fsubr st(4)`
  lendo registrador vazio do próprio harness). Nenhum prefixo LOCK no código do
  catálogo (os Interlocked* vêm do kernel32 HLE); os 57 `xchg` em memória são dados
  desmontados. O jogo cria o device com `D3DCREATE_FPU_PRESERVE` (flags 0x56), então
  o x87 de 53 bits do modelo é o mesmo do original.
- O cenário (chão, escada, paredes) NÃO é Havok: é o detector de colisão estática do
  MT Framework ("Static Collision Detector", vtable 0x00F59874, construtor 0x0076E1D0,
  base 0x004B4A20, propriedades em 0x004B5090) sobre malhas SBC1 do estágio. Medido nos
  despejos em jogo (s115, `guest_10`/`capture_*`):
  - `snap_diff.py` nas 6 capturas F11 de 2026-10-22 (os dois personagens jogáveis, que
    usam o movimento 0x00B72FA0, um inimigo 0x009DF110 e o proxy 0x00CFC5E0): 0 bytes
    diferentes do x86 (Unicorn) em todas; `--funcs` (novo) mostra que a chamada passou
    pelo detector (0x004A63E0..0x004BF060, 0x0076E2D0..). A consulta está traduzida certa;
  - `mSlopeDegree` 45 / `mWallDegree` 55, cossenos 0,7071/0,5736: corretos;
  - SbcNum 2 (`s115h_eff`, 25 partes; `s115h_scr`, 3): as 4 seções de cada uma, iguais
    byte a byte ao arquivo do `s115.arc` (2,7 MB e 103 KB). Nenhum objeto tem SBC próprio
    (141 SBC em 70 `.arc`, todos de estágio);
  - `JobThread=1` no config.ini e `RE5_GUEST_LOCK=global`: corrida entre threads fora.
  Conclusão: código e dados de colisão certos; o defeito está num estado que o jogo
  monta em execução (tempo/entrada/ordem de inicialização) e que só uma referência do
  jogo original mostra. Próximo passo: despejo completo (heap) do original pelo
  `tools/re5_logger` no mesmo formato `.bin/.map` e diff das estruturas acima.
- Entradas da consulta (`snap_diff.py --watch` novo, captura 1, personagem `0x185AAD40`
  descendo a escada do s115): `cUnit::mDeltaTime` (+0x10) 0,6146 (≈49 fps, base 30; os
  5,9/6,5 dos despejos F10 são o quadro após a pausa do despejo); a entrada do detector
  `0x0076FB90` recebe a máscara 0x3FFFFF00 (a do construtor); varredura da posição atual
  (`+0x30`) até o alvo, com subida de ~5,5 (`+0x11D0` vs `+0x1170`) e sonda de descida de
  15; normais tocadas (0,90; 0,43) (quina) e (0,49; 0,87) (rampa de 29°); pedido 7,3 cm,
  andou 6,19 (`+0x1400` = deslocamento do quadro); sonda de chão `0x0076E2D0` de +30 a -15.
  Nada zerado nem absurdo. A tabela MtDTI do personagem (`0x00B59B10` -> `0x00433D70`) não
  publica altura de degrau, máscara nem posição anterior. Falta o quadro que falha (subindo):
  `RE5_CAPTURE=00B72FA0` + `RE5_CAPTURE_N=2` (novo) no `executar_recompilado.bat`.
- Quadro travado na escada (2026-10-23, `work/dump/escada_20261023/capture_1`, personagem
  `0x188FE1D0`, vt 0x0101DA00, parado em (527,9; -133,2; 227,0), `mDeltaTime` 3,2): 0 bytes
  diferentes do x86. O pedido de movimento chega SEM componente horizontal: alvo `+0x1170` =
  posição, varredura do detector só vertical, apoio na rampa (normal 0,49/0,87); o proxy do
  Havok recebe velocidade (0; -3,135; 0) (só o empurrão para baixo) e termina apoiado
  (`m_up` (0,1,0), `maxSlopeCosine` = cos 90°). Nenhuma colisão bloqueia: a intenção de
  andar já vem zerada da lógica do personagem/animação, antes de 0x00B72FA0.
- Co-op/"Network Selection: LOCAL" parava em `ISteamNetworking005::#1` (D8, chamado de
  0x00414005, IsP2PPacketAvailable no canal 0x389). ISteamNetworking005 implementado como
  Steam offline sem pares (métodos 0-21; o jogo usa 0-9); teste `steam_api` estendido.
- **Sistema de Deterministic Replay & Trace Diff (TAS Lockstep)** implementado:
  - Formato binário `FrameTracePacket` de 128 bytes (`runtime/include/re5/trace_packet.h` e
    `tools/re5_logger/include/trace_packet.h`): grava `deltaTime`, `inputStick`, `pos[3]`,
    `oldPos[3]`, `moveVec[3]`, `collisionFlags`, `filterMask`, `stepHeight`, `slopeCos`,
    `wallCos` e `playerStructCRC32` na entrada e na saída de `0x00B72FA0`.
  - Relógio determinístico cravado em 60.0 FPS (`QueryPerformanceCounter`, `timeGetTime`,
    `GetTickCount` com semente fixa) tanto na DLL proxy `dinput8.dll` do original quanto no
    runtime HLE do recompilado (`kernel32.c`, `winmm.c`).
  - Injeção de input TAS (`xinput1_3`): quadros 0..120 neutros, 121..600 com analógico 100%
    à frente (`wThumbLY = 32767`), ativado pela tecla F7.
  - Transplante de coordenadas (`snapshot_s115.json`): salva posição inicial ao acionar F7 no
    original e sincroniza Chris no recomp para a mesmíssima coordenada de partida.
  - Script diferencial `scripts/trace_diff.py` com diagnóstico automático dos Cenários A, B e C;
    5 testes unitários em `tests/python/test_trace_diff.py` (52/52 testes Python verdes no ctest).

### Ponte D3D9 (etapa 4.2, decisão D4)

- `scripts/gen_d3d9_bridge.py` lê a ordem das vtables do `d3d9.h` da DXVK native
  (`third_party/`, fixo) e gera, no build, `re5_d3d9_methods.inc`: 297 métodos em 15
  interfaces, bytes de pilha stdcall (`4 * (argumentos + this)`), IIDs e **217
  repassadores automáticos** para os métodos de tipos planos (escalares, `float`,
  ponteiros para estruturas sem ponteiros/handles, objetos de entrada e de saída).
  Tipo que o gerador não conhece derruba o build.
- `runtime/src/win32/d3d9.c`: objeto-sombra de 16 bytes no guest (`[vtable][handle
  RE5_H_COM][tipo]['D3D9']`), uma vtable de thunks por interface; mesmo objeto do
  host = mesmo objeto do guest (hash); contagem de referências do objeto-sombra.
  À mão: `IUnknown`, `CreateDevice`/`Reset`/`GetCreationParameters`
  (`D3DPRESENT_PARAMETERS` e `D3DDEVICE_CREATION_PARAMETERS` com HWND de 32 bits),
  `Present` (solta o lock do guest), `SetPrivateData`, `Lock/Unlock` de vertex/index
  buffer (sombra persistente na memória do guest, copiada no `Unlock`) e
  `LockRect/UnlockRect` de textura, cubo e superfície (bloco por trava com o pitch do
  host; tabela de formatos, DXT por blocos). O resto (`GetNPatchMode`, `GetDC`,
  `LockBox`, swap chains adicionais, `GetAdapterMonitor`...) aborta com
  `Interface::Método sem implementação`.
- `runtime/src/gfx/d3d9_host.c`: no Windows, `d3d9.dll` x64 (ou `RE5_D3D9_DLL`, p.ex.
  o do DXVK) e o HWND nativo da janela SDL2; no Linux/Android, `dlopen` da
  `libdxvk_d3d9.so` (`RE5_D3D9_LIB`) com `DXVK_WSI_DRIVER=SDL2` - **escrito, ainda
  não compilado nem rodado** (ver lacunas).
- `CreateDevice` acrescenta `D3DCREATE_FPU_PRESERVE` (o RE5 já o pede: flags 0x56),
  para o Direct3D do host não mexer no x87 da thread do host.
- 2.6: depois do `CreateDevice` o laço passou a parar, uma volta por vez, nos alvos
  das 6 tabelas de salto da rotina de cópia em `0x00E63EC0` (`rep movsd` + saltos por
  alinhamento), que o catálogo não achava. As 30 entradas foram lidas do binário; as
  21 que faltavam foram semeadas de uma vez (as outras 9 o laço já tinha achado).
  `work/runtime_seeds.txt` tem 35 sementes distintas.

### Runtime (Fase 3) feito nesta sessão

- `kernel32`: heap (classes de tamanho + arenas de 32 MiB + blocos grandes em região
  própria), `VirtualAlloc/Free`, `Global*`; seções críticas, TLS, `Interlocked*`,
  eventos, mutexes (com nome), semáforos, `WaitFor*`, `CreateThread` (thread do host
  com CPU/TEB/pilha do guest), `SleepEx` + APC; arquivos (`CreateFileA`…`ReadFileEx`,
  busca, diretórios, INI); code pages 1252/437/UTF-8, `GetStringType`, `LCMapString`,
  `CompareString`, `GetLocaleInfoA`; versão (Windows 7 SP1), `VerifyVersionInfoA`,
  tempo, ambiente, startup info, `GetModuleFileNameA`, `GetModuleHandleA` das DLLs
  importadas, `LocaleNameToLCID`. `advapi32`: registro em memória com os valores do
  `BH5AE_install.vdf`. `shell32`: `SHGetFolderPathA` (perfil fixo `C:\Users\Player`
  no VFS). `ole32`: `CoInitialize`; `CoCreateInstance` só recusa o WMI (opcional,
  com aviso) e aborta com o CLSID para o resto. `user32`: `GetSystemMetrics`
  (resolução do host ou `RE5_SCREEN`), `AdjustWindowRect`, `SystemParametersInfoA`.
- Depois do M7 (3ª sessão): `SuspendThread`/`ResumeThread` de thread em execução (a
  thread de som do jogo): com o lock global do guest (D5) ela só roda código do guest
  segurando o lock, então a suspensão vale no próximo ponto em que ela o pegaria
  (`re5_thread_suspend`, testado em `test_kernel32.c`). `SystemParametersInfoA` das
  teclas de aderência/alternância/filtro e do rastro do mouse (o jogo os desliga e
  restaura; estado no runtime, o desktop do host não muda). `winmm`: `timeGetTime`
  no relógio do `GetTickCount`, `timeBegin/EndPeriod`. Rede de uma máquina sem rede,
  coerente com a Steam offline: `ws2_32` (`WSAStartup` 2.2, ordem de bytes,
  `inet_addr`/`inet_ntoa`, `gethostname` = `RE5-PC`, `gethostbyname` só do próprio
  nome e de `localhost` → 127.0.0.1, o resto `WSAHOST_NOT_FOUND`) e `iphlpapi`
  (`GetIfTable`/`GetIfEntry`: só a "Software Loopback Interface 1", sem MAC). Os
  sockets (`socket`, `connect`, `select`...) continuam abortando com o nome. Testes:
  `test_ws2_32.c`, `test_user32.c`, `test_kernel32.c`.
- Lock do guest (D5) justo e com preempção: depois do M8 a janela ficava preta e
  "não respondendo". O `RE5_WATCHDOG=5` (novo: uma linha por thread do guest a cada
  intervalo, com estado do lock, dispatches andados, handler do host e último VA)
  mostrou a thread `0x100C` com o lock o tempo todo (+36 milhões de dispatches em 5 s,
  em `EnterCriticalSection`/`LeaveCriticalSection`) e a principal (`0x1004`), a de
  arquivos (`0x1008`) e duas em `Sleep` esperando o lock com +0. O `CRITICAL_SECTION`
  não é justo; o lock virou uma fila por senha e `dispatch` chama
  `re5_guest_preempt()` a cada 1024 dispatches (2 ms de quantum, só fora de handler).
- A thread da janela também trata as mensagens do Windows dentro das esperas do HLE
  (`WaitFor*` em fatias de 10 ms, `Sleep`, seção crítica ocupada): o watchdog mostrou
  a principal parada em `WaitForMultipleObjects` esperando o carregamento, e fechar a
  janela "não respondendo" dava a saída com código 1 sem log das execuções longas.
- Imports (2026-10-21): console de um processo de janela (`GetConsoleCP`,
  `GetConsoleOutputCP`, `GetConsoleMode`, `WriteConsoleA/W` falham com
  `ERROR_INVALID_HANDLE`), `lstrcpyA`/`lstrlenA`, `oleaut32` (`SysAllocString`/
  `SysFreeString`, BSTR no heap do guest), `ShellExecuteA` (o jogo só abre a página de
  privacidade da Capcom: URL `http(s)` vai para o navegador do host) e `wmvcore`
  (recusa provisória até a 4.5, que agora existe: ver a linha da 4.5).
- Sockets (`runtime/src/win32/ws2_32_sock.c`): ponte para os sockets do host na mesma
  máquina sem rede - o loopback funciona de verdade; `connect`/`sendto` para fora de
  127.0.0.0/8 dão `WSAENETUNREACH`, `bind` em `INADDR_ANY` vira 127.0.0.1 e num endereço
  que não é loopback dá `WSAEADDRNOTAVAIL`. `select`/`__WSAFDIsSet` com o `fd_set` de
  SOCKETs de 32 bits, `ioctlsocket` (`FIONBIO`, `FIONREAD`), opções traduzidas no
  POSIX, e `WSAAsyncSelect` (uma thread do host olha os sockets a cada 10 ms e posta
  na fila do `user32`, com o rearme do Winsock). Usuários no RE5: a classe de socket em
  `0x0041C7xx` e uma biblioteca em `0x00D1Cxxx`, ambas do modo online. Teste:
  `test_ws2_32.c` (TCP e UDP no loopback, `select`, `WSAEWOULDBLOCK`, rede inexistente,
  `FD_ACCEPT` chegando por `PeekMessageW`).
- Gravação de AVI (`runtime/src/win32/avifil32.c`, `avifil32` + `msvfw32`):
  `IAVIFile`/`IAVIStream` são objetos com vtables de thunks, e as funções que no Windows
  só despacham pela vtable fazem o mesmo - é por isso que o IAT tem pares no mesmo
  endereço (`AVIFileReadData` = `AVIStreamSetFormat`, slot 7; `AVIFileRelease` =
  `AVIStreamRelease`). Grava um AVI 1.0 de verdade sem compressão (`hdrl`/`strl`/`movi`/
  `idx1`) pelo VFS. `ICCompressorChoose` não tem diálogo: devolve "Full Frames"
  (`DIB `), o único compressor do runtime e o que o jogo pré-seleciona; outro
  compressor em `AVIMakeCompressedStream` dá `AVIERR_NOCOMPRESSOR`. Teste:
  `test_avifil32.c` (grava 2 quadros e confere a estrutura RIFF no disco).
- Catálogo completo refeito (2026-10-21) com o decodificador novo: 70.724 funções
  (+291 alvos de desvio sem corpo), 840 s; `RECOMP_TODO` 1.509 → 1.353.
- `RE5_D3D9_SHOT=100,1000`: o back buffer desses `Present` vai para
  `RE5_D3D9_SHOT_DIR/frame_<N>.bmp`; o log marca o 100º, 1000º e 10000º `Present`.
- Apresentação em janela por padrão: o Direct3D do host nunca entra em tela cheia
  exclusiva (o RE5 faz `Reset` para tela cheia logo depois do primeiro `Present`), a
  não ser com `RE5_FULLSCREEN=1`; o guest continua vendo `Windowed=0`. Motivo: rodando
  à mão, a tela cheia exclusiva com o jogo travado prendeu o desktop (reinício forçado).
  E a thread dona da janela trata as mensagens do Windows (`SDL_PumpEvents`) a cada
  10 ms enquanto espera o lock do guest (`re5_thread_set_lock_wait_pump`). Medido
  depois (execução de 70 s, `Get-Process re5`): `Responding=True` aos 15/30/45/60 s,
  mais de 1.000 `Present`, e `RE5_D3D9_SHOT=100,1000` gravou os dois quadros: a tela
  "NOW LOADING" do jogo, animada.
- Diagnóstico (3.8): falha do host causada pelo guest imprime o VA do guest, os
  registradores do guest e a trilha de dispatch, no stderr e no `RE5_LOG_FILE`.
- Teste: `tests/unit/test_kernel32.c` (heap, memória virtual, sincronização, threads,
  NLS, versão, tempo, ambiente, arquivos, INI, `ReadFileEx` + APC).

### Lacunas conhecidas (medidas, não estimadas)

- Exceções do guest (SEH, `throw` do C++) não existem: `RaiseException`/`RtlUnwind`
  abortam com o código → etapa 3.6.
- `TlsFree` não zera o slot nas outras threads.
- O guest vive em UTC (`GetTimeZoneInformation` com bias 0).
- O teste diferencial do `lift32_cpu` existe (`scripts/difftest_cpu.py`, etapa 2.2 acima);
  cobre uma amostra das formas de instrução, não cada codificação.
- Proteção de memória é rastreada por região, não por página (`guest_memory.c`).
- Esta máquina tem só o Python 3.11 (o 3.14 foi removido a pedido do usuário em
  2026-10-21; o CMake o achava e ele não tinha `pefile`/`capstone`/`unicorn`). Mudar o
  interpretador regenera o `cpu.h` e recompila todo o código gerado (~11 min).
- Linux x64 e Android ainda não foram compilados nesta rodada (só Windows/MinGW). Isso
  inclui o caminho da DXVK native da ponte D3D9 (`d3d9.c` contra o `d3d9.h` da DXVK
  native, `dlopen` da `libdxvk_d3d9.so`, que ainda não é construída pelo CMake).
- Ponte D3D9: a sombra persistente de vertex/index buffer é a fonte da verdade do
  conteúdo; se o guest soltar a última referência de um buffer ainda ligado ao device
  e pegá-lo de volta (`GetStreamSource`), o objeto-sombra novo nasce com sombra vazia.
  `QueryInterface` de IID fora do D3D9 (ex.: `IDirect3DDevice9Ex`) aborta com o GUID.

### Censo de imports (`re5 --census`)

| Data | Executável | Ligados | Com HLE |
|---|---|---:|---:|
| 2026-10-20 | `re5dx9.exe.unpacked.exe` (protegido: IAT do Enigma, não a real) | 21 | 4 |
| 2026-10-20 | `re5dx9.clean.exe` (despejo no OEP) | 270 | 136 (50,4%) |
| 2026-10-20 | `re5dx9.clean.exe` (Fase 4.1: user32/gdi32/SDL2) | 270 | 191 (70,7%) |
| 2026-10-20 | `re5dx9.clean.exe` (+ steam_api, d3d9 parcial) | 270 | 210 (77,8%) |
| 2026-10-20 | `re5dx9.clean.exe` (+ ponte COM do d3d9) | 270 | 211 (78,1%) |
| 2026-10-21 | `re5dx9.clean.exe` (+ entrada, rede, áudio/vídeo recusados, console, BSTR) | 270 | 241 (89,3%) |
| 2026-10-21 | `re5dx9.clean.exe` (+ sockets e gravação de AVI) | 270 | **270 (100%)** |

Faltando (270/270): nenhum. `re5 --census --missing` em 2026-10-21: `imports: 270 ligados, 270 implementados (100.0%), 0 faltando`.