# Plano Mestre de Otimização — PC (x86-64) e Android (ARM64)

**Revisão 2 — 27/10/2026.** Substitui a revisão 1 (mesma data), que foi conferida item por item
contra o código, o histórico do git e as medidas do `STATUS.md`.

Como este documento se relaciona com os outros:

| Documento | Papel |
|---|---|
| `STATUS.md` | Evidência. Só recebe "FEITO" com números da própria sessão. Os números de base deste plano vão para lá. |
| `ARQUITETURA.md` | Decisões D1–D9. Uma fase que contraria uma decisão (ex.: D5, o lock global no ARM64) muda esse arquivo **antes** do código. |
| `RELATORIO_ALTERACOES_OTIMIZACAO.md` | Histórico dos Itens 1 e 2 (o que foi tentado, revertido e por quê). Continua valendo como registro. |
| `guia_de_otimiza_o_re5_64_bit_recomp_vulkan_backend.md` | Material de referência genérico. Parte dele não se aplica a uma recompilação estática; a seção 9 diz o quê e por quê. |
| `PLANO_DO_PROJETO.md` (Fase 6) e `PLANO_APP_ANDROID.md` (A6) | Este plano detalha a etapa 6.2 (desempenho) e a A6 (Android a 30 fps). |
| **Este arquivo** | A ordem do trabalho de desempenho, o que cada fase precisa provar e como o usuário testa. |

---

## 1. O que esta revisão mudou

| Revisão 1 | Problema encontrado | Revisão 2 |
|---|---|---|
| 1.1 XMM como vetor nativo: "primeira etapa a executar" | Já está no código: `030b33b1`, reaplicado em `c4fee40d` (`cpu.h`: `typedef xv4f XMM`). O §F do relatório diz "desfeito", mas o commit seguinte o trouxe de volta. | Fase **0.2**: medir o que já está no código, sem reimplementar. |
| 1.2 Flags em locais | Também já no código (`c4fee40d`, `CACHED` inclui `cf..of`), com a causa do fechamento corrigida no catálogo (`--fix-labels`, 120 inícios falsos). Falta medir no jogo depois da correção. | Fase **0.2**, com uma chave de liga/desliga para comparar. |
| 1.3 Dead Flag Elimination "a construir" | Já existe desde `c875e6ce`: liveness reversa por função, `call` mata todas as flags, `ret` com flags mortas abaixo de `0x00E63000`. | Fase **P3**: só o que falta (saltos de cauda, contagem do que sobrou). |
| 1.4 Devirtualização por vtable de Majini | Exige engenharia reversa classe a classe. As chamadas diretas (`RE5_CALL`) e o dispatch por hash já existem. | Fase **P2**: cache por ponto de chamada, gerado mecanicamente pelo lifter para toda chamada indireta, mais `musttail` nos `jmp` entre funções. |
| 1.5 Vulkan nativo na `cRenderDevice` | Seria reescrever a DXVK: traduzir shaders D3D9 SM3 para SPIR-V e todo o estado do D3D9. Contraria D4. O custo da DXVK nunca foi medido. | Fase **P6** mede e ajusta a DXVK. A reescrita fica adiada com condição para voltar (§9). |
| 1.6 Havok → Jolt, física a 60 Hz com interpolação | O Havok 5.5 está compilado dentro do executável, com estruturas que o código da Capcom lê diretamente. A interpolação muda a simulação, o que quebra a paridade com o original e o co-op. | **Rejeitado** (§9). A vetorização da colisão só entra como *override* nativo verificado (P5). |
| 1.7 LOD de animação, IA alternada, skinning na GPU | Muda o comportamento do jogo. O co-op LAN PC×Android passa a ver mundos diferentes. O skinning do RE5 já é feito em vertex shader (D3D9 SM3). | **Rejeitado** como padrão (§9). |
| 1.8 `alignas(64)` nas estruturas da MT Framework, alocador por quadro | O layout da memória do guest é fixo pelo binário: não dá para realinhar nem trocar o alocador do jogo. | Fase **P1** acelera o heap do HLE (o lado do host). |
| 1.8 `-O3 -flto -fstrict-aliasing -march=x86-64-v3`, `/fp:fast` | `-fstrict-aliasing` quebra o código gerado (o runtime força `-fno-strict-aliasing -fwrapv`). `-march` com FMA funde `a*b+c` e muda o arredondamento. LTO em 180 unidades enormes não cabe na memória do build. O projeto não compila com MSVC. | Fase **P4**: PGO, ordenação de funções, `-O2` só nas unidades quentes, `-march=x86-64-v2` com `-ffp-contract=off`. |
| 2.1 Main thread no núcleo Prime (Cortex-X) | O aparelho de teste (Galaxy A54, Exynos 1380) tem 4×A78 + 4×A55, sem núcleo Prime. | Fase **A2**: detecta os clusters pela frequência máxima, sem nomes fixos, e usa a API ADPF do Android. |
| 2.1 item 3, relaxar o lock global (subitem) | É o maior gargalo provável do Android: no ARM64 o lock global (D5) deixa uma só thread do jogo rodar por vez. | Promovido a fase própria e prioritária: **A1**. |
| 2.2 `storeOp`, command buffers secundários, Turnip | Quem monta os render passes é a DXVK, não o runtime. O A54 é Mali, onde o Turnip (Adreno) não se aplica. A swapchain está em `MAILBOX`, que desenha quadros que nunca aparecem. | Fase **A4**: modo de apresentação, escala de resolução e avaliação de uma DXVK mais nova no Mali. |
| 2.3 FTZ + DN no FPCR | FTZ já está feito (MXCSR `0x9FC0` → `FPCR.FZ`). DN muda o NaN que o x86 propagaria. | Fase **A5**: DN **não**; em vez disso, `-ffp-contract=off` e revisão do sse2neon. |
| 2.4 `posix_memalign`, auditoria de `#pragma pack` | Carga desalinhada comum no ARM64 não gera `SIGBUS`. As estruturas do guest não podem mudar. | Absorvido pela **A1** (alinhamento só importa nos atômicos) e pela **A5**. |
| — (ausente) | Mali-G68 não tem BCn: o DXT vira RGBA8 na CPU, com 4–8× a memória e a banda. | Nova fase **A3**: transcodificar DXT para ETC2 na importação. |
| — (ausente) | Não havia medição nem perfil antes de cada fase. As ferramentas já existem (`bench_re5.py`, `RE5_PROFILE`, `RE5_FPS_LOG`). | Nova **Fase 0** obrigatória, e o protocolo exige perfil na entrada e benchmark A/B na saída. |
| Protocolo: "commit só depois da aprovação" | O trabalho não commitado se perde quando o container da sessão é reciclado. | Commit num **branch por fase**; `master` só recebe o merge depois da aprovação (§4). |

---

## 2. Estado atual (com a fonte de cada afirmação)

### 2.1 Otimizações que já estão no código

| Otimização | Onde / commit | Medida registrada |
|---|---|---|
| Lock só no HLE (`RE5_GUEST_LOCK=hle`, padrão no x86-64) | `thread.c`, D5 | ~10 → 20–25 fps (usuário); espera no lock no carregamento de 58% → 2–5% das amostras |
| Seções críticas, eventos, `VirtualFree`, `Sleep`, consultas do DirectSound fora do lock do HLE | `kernel32_sync.c` e outros, D5 | idem |
| Dispatch por tabela hash; chamadas diretas `RE5_CALL` para alvos constantes | `dispatch.c`, `lift.direct_calls`, D3 | 2036 de 2057 `dispatch(c, const)` viraram diretas num arquivo de amostra |
| `rep stos`/`rep movs` em bloco; `CPU *restrict c`; `RE5_LOOP_BARRIER()` nas cabeças de laço | lifter, D3 | código ~5% menor |
| Flags mortas (liveness por função; `call` mata; `ret` morto abaixo de `0x00E63000`) | `lift32_cpu.py` (`flag_liveness`), `c875e6ce` | — |
| SSE empacotado como operação vetorial; GPR/XMM em variáveis locais | `c875e6ce` | — |
| Base fixa do guest | `c875e6ce` | — |
| FTZ/DAZ por padrão (MXCSR `0x9FC0`; `FPCR.FZ` no ARM64) | D9 (`RE5_FTZ=0` desliga) | não medido isoladamente |
| **Item 1**: XMM como vetor nativo | `030b33b1`, `c4fee40d` | +3 fps (~60 → ~63,3 a 800×600, usuário) |
| **Item 2**: flags em variáveis locais | `c4fee40d` | com o bug do `bt`: −10 fps e fechamento. **Depois da correção: não medido** |
| Catálogo sem inícios falsos em caudas de laço (`--fix-labels`) | `c4fee40d` | elimina o estouro da pilha do host que fechava o jogo; **falta confirmar no jogo** |
| Runtime em `-O2`; código gerado em `-O1` | `generated/CMakeLists.txt` | `-O2` no gerado: sem ganho medível e build de 11 → 19 min |
| DXT decodificado com escrita de 32 bits e paleta pré-computada | `dxt_decode.h`, `b6c0da42` | "−70% dos ciclos" (sem log registrado) |

### 2.2 Números de referência

- `bench_re5.py`, código gerado em `-O1`, 22/10, **antes** dos Itens 1 e 2: mediana **42,8 fps**,
  p10 35,9, em 356 s.
- Usuário, 800×600: ~60 → ~63,3 fps com o Item 1.
- A revisão 1 cita **16,39 ms (61 fps)** no PC e **55,55 ms (18 fps)** no Android, mas sem
  resolução, configuração nem comando registrados, e nenhum dos dois está no `STATUS.md`. O
  valor do PC é suspeito de estar perto de um teto de 60 Hz. A Fase 0.1 refaz os dois com
  roteiro fixo.
- GPU a 200 MHz com 34% de uso indica que o gargalo é a CPU. Mas **qual parte** da CPU não foi
  medida no gameplay desde as últimas mudanças. O último perfil útil (carregamento) deu 77% das
  amostras no código gerado.

### 2.3 Android hoje

- App, launcher e controles prontos (`b6c0da42`). DXVK native **1.9.2b** com o porte do NFSU2.
  Galaxy A54: Exynos 1380 (4×Cortex-A78 + 4×Cortex-A55), Mali-G68, Vulkan 1.3.219.
- **Lock global** (padrão fora do x86-64): uma thread do guest por vez, apesar dos 8 workers da
  MT Framework.
- Sem BCn no Mali: DXT → RGBA8/BGRA8 na CPU.
- Swapchain `VK_PRESENT_MODE_MAILBOX_KHR` (logcat no `STATUS.md`).
- O launcher **não passa variáveis `RE5_*`** ao runtime (nenhum `setenv` no Java nem no
  `main.c`). Sem isso não dá para fazer experimentos no aparelho; ver A0.
- O HUD de telemetria (fps, RAM, CPU, GPU) existe, mas não grava em log.

### 2.4 Ferramentas que já existem (o plano usa estas, não novas)

| Ferramenta | Para quê |
|---|---|
| `scripts/bench_re5.py --label X --env K=V` | Benchmark variável sozinho (entrada roteirizada); imprime média, mediana e p10 |
| `RE5_FPS_LOG=1` | fps por segundo no log |
| `RE5_PROFILE=<s>` (só Windows) | Amostragem de 1 ms por thread, mapeada para `L_<VA>`, com quem espera o lock e onde |
| `scripts/difftest_cpu.py`, `difftest_func.py` | Equivalência instrução a instrução e função a função contra o Unicorn |
| `RE5_CAPTURE`, `snap_diff.py`, `trace_diff.py` | Captura de quadro e diff contra o x86 / o original |
| `config/overrides.txt` + `runtime/src/core/overrides.c` | Corpo nativo para uma função do guest |
| `RE5_GUEST_LOCK`, `RE5_CPUS`, `RE5_FTZ`, `RE5_GENERATED_OPT`, `lift.direct_calls` | Chaves de A/B já disponíveis |

**Onde cada teste roda.** Conferido em 27/10 com um build limpo do `master` (`b6c0da42`) no
Linux: o build passa e o `ctest` fica verde (26 testes). Mas dois são pulados por falta de
arquivos do jogo, que não vão para o git (D7): o `wmvcore` e o **`difftest_cpu`**, que precisa
de `bin_input/re5dx9.clean.exe` e `work/catalog.json`. No container da IA, então, a equivalência
contra o Unicorn **não é verificada**. Ela só roda onde o jogo está: no PC do usuário, ou numa
sessão em que ele forneça `bin_input/`.

---

## 3. Princípios (valem para todas as fases)

1. **Medir antes e depois.** Toda fase entra com um perfil ou contador que mostra o custo que ela
   ataca, e sai com o benchmark A/B. Uma fase sem evidência de entrada não começa.
2. **Toda otimização tem chave.** Uma opção de `re5_config.toml` (lifter) ou uma variável `RE5_*`
   (runtime), no mesmo molde de `lift.direct_calls`. O A/B é feito no mesmo build, sem voltar
   commit. O padrão só muda depois da medida.
3. **Equivalência bit a bit no código do jogo.** Qualquer mudança no lifter, no `cpu.h` ou em
   *override* passa pelo `difftest_cpu` (e pelo `difftest_func` no caso de *override*). Nada de
   `-ffast-math`, `-Ofast`, `-ffinite-math-only`, `-fassociative-math`, `-fno-signed-zeros`.
4. **Não mudar a simulação do jogo** (IA, animação, física, passo de tempo). O jogo original é o
   oráculo dos bugs que ainda estão abertos (escada), e o co-op LAN junta PC e Android: os dois
   lados precisam calcular a mesma coisa.
5. **O layout da memória do guest é fixo.** Alinhamento, `pack`, SoA e alocadores só podem mudar
   do lado do host (runtime, ponte D3D9, HLE).
6. **Semântica de ponto flutuante igual em todo host:** `-ffp-contract=off` no código gerado e no
   runtime (ver 0.4). Proibido no código gerado: `-fstrict-aliasing` (ele lê a mesma memória por
   tipos diferentes) e `-fno-wrapv`.
7. **Uma variável por teste.**

---

## 4. Protocolo de execução

```
 1. Evidência de entrada (perfil/contador)  ──> sem evidência, a fase não começa
 2. Implementação no branch otim/<fase>, com chave de liga/desliga
 3. Testes automáticos: ctest + python -m unittest no container;
    difftest_cpu/difftest_func e relift onde houver bin_input/ (ver §2.4)
    (lifter: relift completo, 0 falhas novas, RECOMP_TODO não sobe)
 4. Commit + push NO BRANCH (nada se perde; master intocado)
 5. Pedido de teste ao usuário com o roteiro da §4.1
 6a. Aprovado  ──> merge em master, padrão da chave ligado, STATUS.md com os números
 6b. Reprovado ──> chave desligada ou branch descartado; anomalia registrada no STATUS.md
```

- **Uma fase por vez no `master`.** Perfil e investigação da fase seguinte podem andar em paralelo,
  porque não mudam o código do jogo.
- A IA não declara uma fase concluída: quem aprova é o usuário, com os números do roteiro.
- Fase que mexe no lifter, no `cpu.h` ou em *override* só vai para o usuário testar o jogo
  **depois** do `difftest_cpu` (e do `difftest_func`) verde na máquina com `bin_input/`. O pedido
  de teste traz esse comando primeiro:
  `python scripts/difftest_cpu.py --per-group 3 --seeds 5 --special 0.35`.
- Qualquer anomalia (queda de fps, travamento, artefato, fechamento) suspende o merge. Volta-se
  ao passo 2, ou a chave fica desligada.

### 4.1 Roteiro de teste do usuário (modelo para cada fase)

**PC (Windows):**
1. Build: anotar o hash do commit e o preset.
2. Configuração fixa por toda a série de testes (definida na 0.1): resolução, *Frame Rate*
   variável no jogo, vsync, `JobThread`, `RE5_CPUS`, DXVK padrão (sem `dxvk.conf`).
3. Rodar alternando, 3 vezes cada:
   `python scripts/bench_re5.py --label <fase>-on --env <CHAVE>=1` e
   `python scripts/bench_re5.py --label <fase>-off --env <CHAVE>=0`.
4. Reportar: mediana e p10 de cada execução (o script imprime), RAM de pico, e se houve
   fechamento. Para fases que tocam o lifter ou as threads: **30 minutos** de jogo no capítulo 1-1
   sem fechar.
5. Visual: uma captura no mesmo ponto do benchmark, ligado e desligado.

**Android:** o mesmo, com o HUD ligado e
`adb logcat -s re5:* | tee <fase>-on.log` (depende da A0, que manda o `RE5_FPS_LOG` para o
logcat). Mais a temperatura e o fps no minuto 20.

**Aprovação:** mediana pelo menos 3% maior e p10 que não piora, ou ganho de estabilidade
comprovado; nenhuma regressão visual ou funcional; 30 minutos sem fechar quando exigido.
Diferença abaixo de 2% é empate: a mudança só fica se simplificar o código.

---

## 5. Metas

| Meta | Plataforma | Critério | Observação |
|---|---|---|---|
| **M-PC1** | PC de referência (Ryzen 7 5700U + Vega) | p10 ≥ 60 fps no benchmark variável, na resolução de referência da 0.1 | Primeiro degrau |
| **M-PC2** | idem | mediana ≥ 90 fps | |
| **M-PC3** | idem | mediana ≥ 144 fps | Ambiciosa: depende do quanto a P6 e a P4 renderem |
| **M-AND1** | Galaxy A54 | **30 fps** estáveis (p10 ≥ 28) por 20 min sem queda térmica forte | É o M14 do `PLANO_DO_PROJETO.md` e o A6 do `PLANO_APP_ANDROID.md` |
| **M-AND2** | SoC topo (Snapdragon 8 Gen 2 ou superior) | 60 fps | Ambiciosa. No A54, 60 fps fixos não é meta realista enquanto o código do guest rodar serializado (A1) numa Mali-G68 |

---

## 6. Grupo 0 — Base comum (antes de qualquer otimização nova)

### Fase 0.1 — Linha de base oficial e tetos artificiais

- **Objetivo:** números confiáveis para comparar tudo o que vem depois.
- **Passos:**
  1. Fixar a configuração de referência (§4.1) e registrar o `config.ini` usado.
  2. Descartar tetos: logar no `CreateDevice` o `PresentationInterval` pedido pelo jogo (o
     `d3d9.c` já lê o campo), o *Frame Rate* do `config.ini` e o modo de apresentação da DXVK.
     Uma linha de base a 60–63 fps precisa provar que não está presa ao vsync.
  3. PC: 3 execuções do `bench_re5.py` no `master` atual. Android: idem, com o roteiro da A0.
- **Saída:** tabela "linha de base" no `STATUS.md`, com hash, configuração, mediana, p10 e RAM.

### Fase 0.2 — Validar o que já está no código (Itens 1 e 2)

- **Objetivo:** saber se o Item 2 corrigido ajuda, atrapalha ou empata, e se o fechamento sumiu.
- **Passos:**
  1. Chave no lifter `lift.cache_flags` (padrão `true`), que tira `cf..of` do `CACHED` quando
     desligada. Relift nos dois modos.
  2. A/B pelo roteiro da §4.1, mais 30 minutos de jogo em cada modo.
- **Critério:** sem fechamento em 30 minutos com `--fix-labels`. O padrão de `cache_flags`
  segue o resultado. O Item 1 não precisa de chave (ganho e estabilidade já medidos), mas entra
  na tabela da 0.1.

### Fase 0.3 — Perfil em todas as plataformas e contadores por quadro

- **Objetivo:** responder "para onde vai o quadro" antes de escolher a próxima fase.
- **Passos:**
  1. **Windows:** `RE5_PROFILE` no gameplay (não só no carregamento), separando código gerado,
     runtime (dispatch, lock, HLE), `d3d9.dll`/DXVK, driver e tempo ocioso.
  2. **Linux:** `perf record -g` (as funções geradas têm símbolo `L_<VA>`). **Android:**
     `simpleperf record --call-graph fp` no processo `:game`, com o `libre5.so` com símbolos.
  3. **Contadores** na linha do `RE5_FPS_LOG`, por segundo: `Present`, chamadas D3D9, *draw
     calls*, thunks do HLE, tempo esperando o lock do HLE, `HeapAlloc`/`HeapFree` e dispatches
     indiretos.
- **Saída:** a fração de cada categoria no `STATUS.md`. A ordem do Grupo PC (§7) é reavaliada
  com esse número: a fase que ataca a maior fatia vai primeiro.

### Fase 0.4 — Mesma semântica de ponto flutuante em todo host

- **Problema:** nenhum alvo passa `-ffp-contract`. O Clang (Android) usa `on` por padrão e funde
  `a*b+c` dentro de uma expressão em `fmadd`, com um arredondamento só. O GCC em modo GNU usa
  `fast`: no x86-64 base não tem FMA (sem efeito), mas com `-march=x86-64-v3` passaria a fundir
  até entre comandos. Nos dois casos o resultado deixa de ser o do SSE/x87 do jogo original, e o
  PC e o Android do co-op calculam números diferentes.
- **Passos:** `-ffp-contract=off` no `re5_cpu` (interface comum ao runtime e ao gerado). Depois,
  rodar o `difftest_cpu` também com o código compilado para ARM64 (cross-compilado com o NDK e
  executado no aparelho, ou com `qemu-aarch64`), na máquina que tem `bin_input/`, para medir a
  diferença antes e depois. Isso pede um modo novo no `difftest_cpu.py`: o compilador e o
  executor do lado C configuráveis (hoje é o `RECOMP_CC` do host).
- **Critério:** `difftest_cpu` igual ao Unicorn no ARM64. O custo em fps no Android é medido, mas
  não decide (é correção, não otimização).

---

## 7. Grupo PC (x86-64)

> A ordem abaixo é a provável. A 0.3 pode trocá-la: a fase que ataca a maior fatia medida vai
> primeiro.

### Fase P1 — Ponte D3D9 e heap do HLE fora do lock

- **Evidência esperada na 0.3:** espera no lock do HLE durante o gameplay, e thunks D3D9 por
  quadro na casa dos milhares.
- **Por que:** hoje **todos** os ~300 métodos D3D9 da ponte e o `HeapAlloc`/`HeapFree` pegam o
  lock do HLE (mutex + variável de condição) a cada chamada. O jogo cria o device com
  `0x56`, que inclui `D3DCREATE_MULTITHREADED` (`0x04`), e a ponte repassa essas flags. Então a
  DXVK já serializa o device por dentro, e o lock do HLE em volta é uma segunda camada.
- **Passos:**
  1. Auditar o estado próprio da ponte (tabela de handles, buffers-sombra, contadores de
     `Present`, captura, coldebug, menu LAN) e protegê-lo com locks finos onde preciso.
  2. ~~Marcar os métodos D3D9 como `RE5_THUNK_NOLOCK` sob a chave `RE5_D3D9_NOLOCK`.~~
     **Adiado (28/10):** a ponte tem estado compartilhado (hash de objetos, travas, sombras);
     um mutex próprio pode travar contra o lock do HLE quando um callback do guest chama o
     D3D9. Só entra com o perfil mostrando o D3D9 esperando o lock.
  3. **Feito (sem medida):** heap do HLE com mutex próprio (`g_hmu`), `HeapAlloc/Free/ReAlloc/Size`
     sem o lock do HLE sob a chave `RE5_HEAP_NOLOCK=1` (padrão desligado até o A/B);
     teste `heap_mt`.
  4. Comparar, só para registro, a DXVK com o `d3d9.dll` do próprio Windows
     (`RE5_D3D9_DLL`): no PC de referência o driver nativo pode ganhar ou perder.
- **Testes:** `test_d3d9` e `kernel32` com várias threads (estresse: criar/destruir texturas
  enquanto outra thread desenha); 30 minutos de jogo.
- **Risco:** baixo a médio (corrida na ponte). **Esforço:** médio.

### Fase P2 — Dispatch, chamadas indiretas e saltos entre funções

- **Evidência:** `dispatch`/`re5_lookup` e a trilha entre as funções do host mais amostradas.
- **Passos:**
  1. **Cache por ponto de chamada** (gerado pelo lifter em toda chamada indireta: vtables,
     ponteiros de função). Cada ponto guarda o índice do slot da tabela hash que acertou da
     última vez; se `slots[i].va == alvo`, chama `slots[i].fn` direto, senão faz o lookup e
     atualiza. A tabela não muda depois do `re5_runtime_init` e o índice é um `uint32_t`, então
     a escrita concorrente é inofensiva: o índice é só uma dica que sempre é conferida. Isso
     é a "devirtualização" da revisão 1 sem precisar identificar classes. Chave
     `lift.call_site_cache`.
  2. **`jmp` para outra função vira `musttail`** (`__attribute__((musttail)) return L_x(c);`,
     GCC 15 e Clang) em vez de uma chamada C aninhada. Isso tira a classe inteira de crescimento
     de pilha que fechava o jogo (o `--fix-labels` resolveu 120 casos, mas `jmp` de cauda entre
     funções diferentes ainda aninha). Também mantém o par `call`/`ret` do host alinhado com o
     do guest, que é o que o *Return Stack Buffer* precisa. Chave `lift.musttail`.
  3. **Trilha de diagnóstico** (D8): se aparecer no perfil, mover o anel do TLS para a própria
     `CPU` (que já é por thread; no MinGW o TLS é emulado).
- **Testes:** `difftest_cpu`, `test_crash` e um teste novo com laço de espera atravessando
  funções (a pilha do host não pode crescer); relift sem falhas.
- **Risco:** médio (lifter). **Esforço:** médio.

### Fase P3 — Flags que ainda sobram

- **Evidência:** contar no C gerado as escritas de `R_cf..R_of` que restam, e as que são lidas.
  A análise atual não trata saltos de cauda nem `dispatch_jmp` como mortos.
- **Passos:** tratar `jmp` para outra função como `ret` abaixo de `lift.flags_dead_at_ret_below`
  (o mesmo argumento: o código MSVC não lê na entrada flags deixadas pelo chamador), e medir o
  que sobrar de PF/AF (lidos quase só por `jp` depois de `comiss`/`fcomi`).
- **Critério:** queda medida no número de escritas de flag e no tamanho do código; `difftest_cpu`
  igual. Se o código encolher menos de 1%, encerrar a fase sem benchmark.
- **Risco:** baixo. **Esforço:** baixo.

### Fase P4 — Build: PGO, ordenação de funções e unidades quentes

- **Por que:** o executável tem ~112 MB de código de máquina. Com o código quente espalhado, a
  pressão no cache de instruções e no iTLB tende a pesar mais que qualquer instrução isolada. E
  isso se ataca sem mudar semântica nenhuma.
- **Passos:**
  1. **PGO:** GCC com `-fprofile-generate`, depois uma rodada do `bench_re5.py`, depois
     `-fprofile-use -fprofile-partial-training` (Clang: `-fprofile-instr-generate`/`-use`). O
     perfil vale enquanto o código gerado não mudar; um relift pede perfil novo. Chave:
     `RE5_PGO=off|gen|use` no CMake.
  2. **Ordenação:** `-ffunction-sections` mais a seção `.text.hot` do PGO (GCC/ld.bfd) ou
     `--symbol-ordering-file` (lld), para juntar as funções quentes.
  3. **`-O2` só nas unidades quentes:** com `lift.bytes_per_file = 0x10000`, as funções quentes
     caem em poucos arquivos; `-O2` neles e `-O1` no resto mantém o build perto dos 11 minutos.
  4. **`-march=x86-64-v2`** (SSE4.2, POPCNT: qualquer PC da última década) num preset próprio;
     `x86-64-v3` só como build opcional, sempre com `-ffp-contract=off` (0.4).
  5. LTO só no runtime (ThinLTO/`-flto=auto`), nunca nas ~180 unidades do código gerado.
- **Risco:** baixo (sem mudança de semântica). **Esforço:** médio (tempo de build).

> **P7 (branch `otim/p7-spinwait`, sem medida):** `lift.spin_pause` (pausa só em laço de espera
> pura: endereço de leitura fixo, sem escrita nem chamada) e `RE5_THREAD_PRIO=1` (honra
> `SetThreadPriority` do guest na thread do host, só Windows; desligado por padrão).

### Fase P5 — *Overrides* nativos verificados para funções quentes do guest

- **Evidência:** funções do guest no topo do perfil da 0.3 que sejam **folhas puras** (não
  chamam o guest, efeitos colaterais só na memória documentada).
- **Candidatos já conhecidos:** `inflate` da zlib 1.2.1 (`0x006CC2C0..0x006CF940`, quente no
  carregamento, anotado no `STATUS.md`); funções de string/memória do CRT; rotinas matemáticas de
  vetor/matriz da MT Framework, se aparecerem. É a forma realista da "vetorização SBC1" da
  revisão 1: substituir a rotina medida, e não o subsistema.
- **Regra de entrada de cada *override*:** `difftest_func` bit a bit igual ao Unicorn em pelo
  menos 1000 entradas (incluindo NaN, infinito, −0 e subnormais), mais uma captura com
  `snap_diff` em jogo. Registro em `config/overrides.txt` com o motivo, e chave
  `RE5_OVERRIDES=off` para A/B.
- **Fora da regra:** qualquer coisa da colisão de escada enquanto aquele bug estiver aberto.
- **Risco:** médio (por função). **Esforço:** por função.

### Fase P6 — Gráficos no PC (DXVK)

- **Evidência:** a fatia da DXVK e do driver no perfil da 0.3, e o `DXVK_HUD=fps,frametimes,
  submissions,drawcalls,pipelines,gpuload`.
- **Passos:**
  1. Medir *draw calls*, *submissions* e compilação de pipelines por quadro; *stutter* de
     compilação (o state cache já existe; a DXVK 2.x tem *graphics pipeline library*).
  2. Ajustes só via `dxvk.conf`, um por teste: `maxFrameLatency=1` **já foi testado e piorou**,
     e `numCompilerThreads` alto competiu com as threads do jogo no 5700U.
  3. Se a ponte aparecer no perfil (cópia do `Unlock`, buffers-sombra), otimizar a ponte: cópia
     só da faixa travada, e `D3DLOCK_DISCARD`/`NOOVERWRITE` sem sombra quando possível.
- **Adiado (ver §9):** backend Vulkan próprio na `cRenderDevice`.
- **Risco:** baixo. **Esforço:** baixo a médio.

### Fase P7 — Threads da MT Framework e esperas

- **Passos:**
  1. A/B de `JobThread` e `RE5_CPUS` (4/6/8) no PC de referência: o *job system* faz fork-join
     e espera o worker mais lento.
  2. Laços de espera ocupada: o RE5 não tem `pause`, mas tem espera em flag (por isso existe o
     `RE5_LOOP_BARRIER`). O lifter pode reconhecer o **laço de espera puro** (o corpo só lê
     memória e compara, sem escrita nem chamada) e emitir `_mm_pause()` (x86) ou `yield` (ARM64)
     só nele. **Não** colocar pausa na barreira de todo laço: isso desaceleraria todo laço
     quente.
  3. Prioridade de thread: honrar `SetThreadPriority` do guest no host (thread de render e
     principal acima dos workers), se o perfil mostrar a thread principal esperando worker.
- **Risco:** baixo. **Esforço:** baixo.

---

## 8. Grupo Android (ARM64)

### Fase A0 — Pré-requisitos e medida no aparelho

- **Passos:**
  1. **Variáveis `RE5_*` do launcher para o runtime:** um campo "avançado" no launcher que grava
     `re5_env.txt`, lido pelo `main.c` antes do `re5_runtime_init` (ou `Os.setenv` antes de
     carregar a `.so`). Sem isso, nenhum A/B é possível no celular.
  2. `RE5_FPS_LOG` e os contadores da 0.3 no logcat; o HUD continua só como visualização.
  3. Linha de base (0.1) e perfil `simpleperf` (0.3) no A54.
  4. **Experimentos baratos, sem código novo** (um por execução, só para medir o tamanho do
     prêmio, **não** para jogar):
     - `RE5_GUEST_LOCK=hle`: **inseguro** no ARM64 (ver A1), mas mostra quanto fps o paralelismo
       vale;
     - `RE5_CPUS=4` contra o padrão (7): workers nos A55 seguram o fork-join;
     - resolução 720p contra 540p: separa o limite de CPU do de GPU;
     - *Frame Rate* 30 no jogo.
- **Saída:** no `STATUS.md`, se o Android é limitado por serialização, CPU ou GPU. Isso decide a
  ordem entre A1, A3 e A4.

### Fase A1 — Paralelismo seguro no ARM64 (etapa 6.2, decisão D5)

- **Por que é a principal:** no ARM64 o lock global deixa uma thread do guest rodar por vez. No
  PC, a troca para `hle` dobrou ou triplicou o fps (10 → 20–25). O motivo do lock é o modelo de
  memória: x86 é TSO, o ARM64 é fraco. O RE5 não tem atômicos em linha (zero `lock`, `cmpxchg`,
  `xadd`, `pause` no catálogo): a sincronização passa pelo kernel32 do HLE, que já usa atômicos e
  mutexes do host (barreiras completas). **O risco que sobra** são as esperas em flag de memória
  comum: quem escreve grava os dados e depois a flag; quem lê vê a flag e depois os dados. O x86
  garante essa ordem e o ARM64 não.
- **Estratégia em degraus, cada um com chave (`RE5_GUEST_LOCK=global|hle|hle-tso`):**
  1. **`hle-tso` completo (correto por construção):** no ARM64, cargas e armazenamentos de
     memória do guest em `cpu.h` (`rd*`/`wr*`) viram acquire/release:
     - cargas com `LDAPR` (RCpc; mais barata que `LDAR`), compilando com
       `-march=armv8.2-a+rcpc`;
     - armazenamentos com `STLR`;
     - acessos relativos a `esp`/`ebp` ficam comuns: a pilha é privada da thread, e o lifter
       sabe o operando. É a mesma ideia que os emuladores de x86 em ARM (FEX-Emu, Box64) usam
       para emular a ordem do x86.
     - **Alinhamento:** `LDAPR`/`STLR` em endereço desalinhado gera falta de alinhamento sem
       FEAT_LSE2. Ou o acesso desalinhado cai num caminho com carga comum mais `dmb ish`, ou se
       usa o atômico direto quando o `getauxval(AT_HWCAP)` informa `HWCAP_USCAT` (LSE2). Conferir
       `HWCAP_LRCPC` no aparelho em vez de supor.
  2. **`hle-barreiras` (heurístico, mais rápido):** `dmb ish` no `RE5_LOOP_BARRIER` e na saída de
     laços de espera puros (P7), e `dmb ishst` antes de toda chamada de HLE e de todo `ret` de
     função que escreveu memória. Só vira padrão se passar nos mesmos testes que o completo.
- **Testes obrigatórios (antes do usuário):** testes de litmus *message passing* e *store
  buffering* em `tests/` rodando código no estilo do gerado no aparelho (milhões de iterações,
  com 0 violações exigidas); depois benchmark em laço por 1 hora sem travar; comparação de
  `trace_diff` (relógio determinístico) entre `global` e `hle-tso`.
- **Documentação:** D5 em `ARQUITETURA.md` atualizada antes do merge.
- **Risco:** alto (é a mudança mais delicada do plano). **Esforço:** alto. **Ganho esperado:** o
  maior do Android. A0 mede o tamanho do prêmio antes.

### Fase A2 — Núcleos, energia e térmica

- **Passos:**
  1. **Clusters pelo `cpuinfo_max_freq`** de cada CPU em `/sys/devices/system/cpu/` (sem nomes
     fixos Prime/Gold/Silver: o A54 não tem Prime). Padrão de `RE5_CPUS` = número de núcleos
     grandes; threads do guest com afinidade aos grandes, a thread de CS da DXVK também; áudio e
     E/S livres.
  2. **ADPF** (`PerformanceHintManager`, Android 12+): sessão com as threads principal e de
     render e duração-alvo de 33,3 ms; relatar a duração real a cada quadro (o `Present` da
     ponte é o ponto natural).
  3. **Térmica:** `PowerManager.getThermalHeadroom()` para baixar a escala de resolução (A4)
     antes do *throttling*, não depois.
  4. **Não** usar espera ocupada no limitador de quadros (ver A4).
- **Teste:** `adb shell top -H` e `simpleperf` mostrando as threads do jogo nos núcleos grandes;
  20 minutos com fps estável.
- **Risco:** baixo. **Esforço:** médio.

### Fase A3 — Texturas para GPU sem BCn (novo)

- **Por que:** Mali-G68 (e os Adreno com driver proprietário) não têm BC1–BC3. Hoje cada textura
  DXT vira RGBA8 na CPU: DXT1 ocupa 8× mais e DXT5 4× mais, em memória, banda e cache de textura.
  Num GPU por tiles a banda é exatamente o recurso escasso. E a decodificação no carregamento
  gera *stutter*.
- **Passos:**
  1. Transcodificar na **importação** (launcher) ou na primeira carga, com cache em disco:
     DXT1 → ETC2 RGB8/RGB8A1 (4 bpp, mesmo tamanho) e DXT3/5 → ETC2 RGBA8 EAC (8 bpp, mesmo
     tamanho). Todo Mali com Vulkan suporta ETC2 (conferir `textureCompressionETC2` no
     `vulkaninfo` do aparelho).
  2. A ponte cria a textura no formato ETC2 quando há versão em cache; sem cache, cai no caminho
     atual (`dxt_decode.h`).
  3. ASTC como alternativa onde a qualidade do ETC2 decepcionar (decidido pela comparação visual).
- **Equivalência:** recompressão com perda; o teste é visual (capturas lado a lado nos mesmos
  pontos) e de memória (`VmRSS` e VRAM pelo HUD).
- **Risco:** médio (qualidade visual). **Esforço:** médio a alto.

### Fase A4 — Pipeline gráfico no celular

- **Passos:**
  1. **Modo de apresentação:** a swapchain está em `MAILBOX`, que renderiza quadros que nunca
     aparecem e gasta GPU e bateria. Usar `FIFO` no celular, com limite de 30 fps (o *Frame Rate*
     do próprio jogo, ou o `PresentationInterval` repassado), para esquentar menos.
  2. **Escala de resolução** no launcher (resolução do jogo menor, com a swapchain escalando para
     a tela), também usada pela A2 sob pressão térmica.
  3. **DXVK mais nova no Mali:** o A54 informa Vulkan 1.3.219. Verificar com `vulkaninfo` se ele
     atende aos requisitos da DXVK 2.x. Se atender, testar contra a 1.9.2b (pipeline library,
     ajustes para GPU por tiles da série 2.x). Se não atender, documentar o recurso que falta.
  4. **Turnip** (`libadrenotools`): só para aparelhos Snapdragon, como opção no launcher. Não se
     aplica ao A54.
  5. `loadOp`/`storeOp` e command buffers secundários **não** são controlados pelo runtime: só
     entram como ajuste na DXVK se o perfil de GPU (Mali Offline Compiler / Arm Performance
     Studio) mostrar banda desperdiçada.
- **Risco:** baixo. **Esforço:** baixo a médio.

### Fase A5 — Compilação e matemática no ARM64

- **Passos:**
  1. `-ffp-contract=off` (0.4). **Não** ligar `FPCR.DN`: o x86 propaga o NaN do primeiro
     operando, e o *default NaN* muda os bits que o jogo pode comparar. FTZ já está feito.
  2. `-march=armv8.2-a+rcpc+dotprod` (todo celular ARMv8.2 em diante: A55/A76 ou mais novo).
  3. PGO no Android: `-fprofile-instr-generate` com `LLVM_PROFILE_FILE` na pasta do app, uma
     rodada do benchmark no aparelho, depois `-fprofile-instr-use` (a P4 aplicada ao NDK).
  4. Helpers quentes do `cpu.h` no ARM64 (do `simpleperf`): conferir o código que o sse2neon
     gera para eles (`movemask`, shuffles, conversões) e reescrever em NEON direto só os que
     aparecerem, com `difftest_cpu` no ARM64.
- **Risco:** baixo. **Esforço:** médio.

### Fase A6 — Áudio e vídeo

- **Passos:** confirmar no logcat qual driver de áudio o SDL2 escolheu (AAudio no Android 8.1 ou
  mais novo) e a latência; medir o custo do WMV/FFmpeg no aparelho. Pré-transcodificar os vídeos
  na importação (etapa 6.4) só se o perfil mostrar queda durante cutscenes.
- **Risco:** baixo. **Esforço:** baixo.

---

## 9. Itens da revisão 1 rejeitados ou adiados

| Item | Decisão | Motivo | Condição para reabrir |
|---|---|---|---|
| 1.5 Vulkan nativo na `cRenderDevice` | Adiado | É reescrever a DXVK (shaders SM3 → SPIR-V, estado D3D9 completo); contraria D4; ganho nunca medido | Depois de P1 e P6: a DXVK (thread de CS mais driver) acima de 30% do quadro **no caminho crítico** do perfil |
| 1.6 Havok → Jolt, física a 60 Hz com interpolação | Rejeitado | Havok estático no exe, lido diretamente pelo código da Capcom; muda a simulação (princípio 4); o bug aberto da escada perde o oráculo | Nenhuma |
| 1.7 LOD de animação, IA alternada entre quadros | Rejeitado como padrão | Muda o jogo; o co-op PC×Android diverge | Só como opção desligada por padrão, se o perfil mostrar animação ou IA acima de 20% em cenas de horda, e nunca em partida co-op |
| 1.7 Skinning na GPU | Rejeitado | O RE5 já faz skinning em vertex shader | Perfil mostrando skinning na CPU |
| 1.8 `alignas(64)` e alocador por quadro na engine | Rejeitado | Layout do guest fixo (princípio 5) | — (a P1 cobre o heap do host) |
| 1.8 `-O3`, `-flto` global, `-fstrict-aliasing`, `/fp:fast` | Rejeitado | Quebram o código gerado, a semântica (D9) ou o build | — (a P4 é a forma segura) |
| 2.1 Afinidade fixa por nome de núcleo | Substituído | Topologias variam (o A54 não tem Prime) | — (A2) |
| 2.2 Command buffers secundários em paralelo | Fora do alcance | A DXVK grava os comandos (com a thread de CS dela) | Só como mudança na DXVK com perfil de GPU |
| 2.3 `FPCR.DN` | Rejeitado | Muda a propagação de NaN do x86 | — |
| 2.4 `posix_memalign`/auditoria de `pack` | Rejeitado | Carga desalinhada comum não falha no ARM64; o guest é fixo | — (o alinhamento dos atômicos está na A1) |
| Limitador com espera ocupada (`yield`) | Rejeitado | Queima a bateria que a fase quer poupar | — (`FIFO` na A4) |

---

## 10. Matriz de sequenciamento

| Fase | Alvo | Pré-requisito | Evidência de entrada | Teste automático | Risco | Esforço | Validação do usuário |
|:---:|---|---|---|---|:---:|:---:|:---:|
| **0.1** Linha de base e tetos | PC + Android | — | — | — | — | baixo | [pendente] |
| **0.2** Itens 1/2 (A/B, fechamento) | PC | 0.1 | — | ctest, unittest, relift | baixo | baixo | [pendente] |
| **0.3** Perfil e contadores | PC + Android | 0.1 | — | ctest | — | médio | [pendente] |
| **0.4** `-ffp-contract=off` | PC + Android | — | difftest no ARM64 | difftest_cpu (x86 e ARM64) | baixo | baixo | [pendente] |
| **P1** D3D9 e heap fora do lock | PC (e Android, se A1) | 0.3 | espera no lock do HLE | test_d3d9, kernel32 com estresse | médio | médio | [pendente] |
| **P2** Cache por ponto de chamada, `musttail` | PC + Android | 0.3 | dispatch no perfil | difftest_cpu, test_crash, laço entre funções | médio | médio | [pendente] |
| **P3** Flags restantes | PC + Android | 0.2 | contagem de escritas de flag | difftest_cpu, test_lifter_gaps | baixo | baixo | [pendente] |
| **P4** PGO, ordenação, `-O2` quente | PC | 0.3 | iTLB/i-cache ou "espalhado" no perfil | ctest | baixo | médio | [pendente] |
| **P5** *Overrides* verificados | PC + Android | 0.3 | função folha no topo do perfil | difftest_func ≥ 1000 entradas, snap_diff | médio | por função | [pendente] |
| **P6** DXVK no PC | PC | 0.3 | fatia da DXVK, DXVK_HUD | — | baixo | baixo | [pendente] |
| **P7** Threads e esperas | PC + Android | 0.3 | workers ociosos ou espera no perfil | test_lifter_gaps (laço de espera) | baixo | baixo | [pendente] |
| **A0** Variáveis, logcat, experimentos | Android | 0.1 | — | — | baixo | baixo | [pendente] |
| **A1** Paralelismo (`hle-tso`) | Android | A0, 0.4 | ganho de `hle` no experimento da A0 | litmus no aparelho, laço de 1 h, trace_diff | **alto** | alto | [pendente] |
| **A2** Núcleos, ADPF, térmica | Android | A0 | threads em núcleos pequenos | — | baixo | médio | [pendente] |
| **A3** DXT → ETC2 | Android | A0 | memória/banda ou *stutter* de textura | test_dxt_decode, capturas | médio | médio-alto | [pendente] |
| **A4** FIFO, escala, DXVK nova | Android | A0 | GPU no limite ou térmica | — | baixo | baixo-médio | [pendente] |
| **A5** ARM64: march, PGO, NEON | Android | 0.4, A1 | helpers do cpu.h no perfil | difftest_cpu no ARM64 | baixo | médio | [pendente] |
| **A6** Áudio e vídeo | Android | A0 | queda em cutscene | — | baixo | baixo | [pendente] |

O Grupo Android **não** espera o Grupo PC terminar (na revisão 1 esperava). P2, P3 e P5 valem
para os dois hosts, porque mudam o código gerado comum, e a A1 é independente das fases do PC.

---

## 11. Pendências fora do desempenho que afetam este plano

1. **Arquivos do jogo no git.** O commit `b6c0da42` pôs no `master`, via LFS,
   `android/app/src/main/assets/re5dx9.clean.exe` (16 MB, o executável do jogo) e
   `android/app/src/main/jniLibs/arm64-v8a/libre5.so` (102 MB, o código do jogo traduzido). Isso
   contraria a D7 ("nunca, nem por LFS") e o modelo legal do `PLANO_APP_ANDROID.md` (o código
   traduzido é produzido na máquina do usuário). Tirar isso do histórico exige reescrevê-lo e
   limpar o LFS do GitHub: **decisão do dono do repositório**. Até lá, nenhuma fase deste plano
   deve gerar novos binários derivados do jogo dentro do repositório.
2. **`RE5_PROFILE` só existe no Windows.** A 0.3 usa `perf`/`simpleperf` no Linux e no Android
   em vez de portá-lo.
3. **Colisão da escada (aberta).** Enquanto ela não fechar, nenhuma fase mexe na lógica de
   movimento ou de colisão (P5 a exclui), para não misturar causas.

---

## 12. Próximo passo

O primeiro trabalho é o **Grupo 0**, e não a revisão 1 do Item 1 (que já está no código):

- **IA:** chave `lift.cache_flags` (0.2); contadores por segundo no `RE5_FPS_LOG` (0.3);
  `re5_env.txt` lido pelo runtime e campo no launcher (A0.1); `-ffp-contract=off` e o modo ARM64
  do `difftest_cpu.py` (0.4); log do `PresentationInterval` e do modo de apresentação (0.1).
  Tudo num branch `otim/grupo-0`, com `ctest` verde no container.
- **Usuário, antes do merge:** `difftest_cpu` no PC (o container não tem o jogo) e relift com
  `lift.cache_flags` nos dois valores.
- **Usuário:** depois do merge, a linha de base pelo roteiro da §4.1 (PC e A54), a A/B da 0.2,
  30 minutos de jogo para o fechamento, e os quatro experimentos baratos da A0 no celular.

Com esses números, a ordem do Grupo PC e a escolha entre A1, A3 e A4 no Android são revistas
**neste arquivo**, antes de começar a próxima fase.
