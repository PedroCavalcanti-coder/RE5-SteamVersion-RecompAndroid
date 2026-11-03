# Projeto: backend gráfico Vulkan nativo (PC e Android)

O mesmo backend serve aos dois hosts. O ganho que pode justificá-lo está no Android; o PC é
onde ele é desenvolvido e comparado com a DXVK (§4.3).

Hoje o vídeo sai pela ponte COM do Direct3D 9 (decisão D4: `runtime/src/win32/d3d9.c`) para a
DXVK: 2.6.2 no PC do usuário e a DXVK native 1.9.2b com o porte do NFSU2 no Android. Este
documento decide se vale trocar a DXVK por um backend Vulkan próprio, onde cortar se valer, e
como chegar lá em etapas que nunca quebram o caminho atual. Ele responde ao item 1.5 do
`PLANO_MESTRE_OTIMIZACAO.md` (§9: "adiado até a DXVK passar de 30% do quadro no caminho
crítico") e precisa da medida da fase A0.

## 1. Decisão

**Atualização (03/11): o backend está guardado na branch `re5vk-nativo`; o `master` volta à DXVK.**
Na branch: tradutor SM3 -> SPIR-V (os 164 shaders do benchmark passam no `spirv-val`) e o device
inteiro (`runtime/src/gfx/vk/`, `RE5_GFX=re5vk`). Resultado no A54 em 720p: ~16 fps, contra 15,4
da DXVK. Menus e vídeo saem certos, mas a cena 3D do benchmark sai quase toda preta (causa não
achada). No PC, o primeiro teste rodou 2.408 Presents. Decisão do usuário: seguir otimizando a
DXVK; o backend pode voltar da branch depois. No `master` ficam a medida (A0), o censo
(`RE5_D3D9_CENSUS`), o tempo por método D3D9 e o despejo de shaders (`RE5_D3D9_SHADER_DUMP`).

**DXVK 2.6.2 no A54 (A4.3, 03/11): não roda sem um porte grande.** Medido com `tools/vkprobe`
(Mali-G68, Vulkan 1.3.219, driver 0x9801000):
- **Bloqueio principal:** sem `VK_EXT_robustness2`. A DXVK 2.x exige a extensão e usa o
  `nullDescriptor` dela em todo descritor não ligado.
- **Ligados sem condição pela 2.6.2 e ausentes no Mali:** `multiDrawIndirect`, `fillModeNonSolid`,
  `multiViewport`, `textureCompressionBC`, `shaderClipDistance`/`shaderCullDistance`, `wideLines`.
- **Sem `VK_EXT_graphics_pipeline_library`:** a principal melhoria da 2.x contra travadas de
  compilação não existe aqui.
- **Presentes:** o núcleo do 1.3 (`dynamicRendering`, `synchronization2`, `timelineSemaphore`,
  `maintenance4`), ETC2/ASTC, `transform_feedback`, `custom_border_color`,
  `swapchain_maintenance1`.

**Atualização (31/10, com a medida do aparelho, §3.4): o usuário decidiu seguir com o backend,
e a medida mostra onde ele pode ganhar.** No A54 o quadro não é limitado pela CPU nem pela
resolução. Ele se perde no pipeline da DXVK: só 2 quadros em voo, o `Present` com 53% do
quadro e a GPU a 25–40% do clock máximo. Quadros em voo, apresentação e sincronização são
exatamente o que um backend próprio controla. Antes de cada etapa, o mesmo benchmark mede o
ganho (E1 primeiro: 3 quadros em voo na própria DXVK). A V2 (`RE5_GFX=re5vk`) está no
`15469e27`.

Decisão original (30/10, antes da medida no aparelho): **não vale agora.** O backend próprio fica parado até a medida do Android mostrar o critério
do §6. Os motivos, em ordem de peso:

1. **No PC a DXVK não é o gargalo, e isso está medido.** O fps é limitado pela thread
   principal do jogo, e a DXVK roda em threads próprias (~0,3 núcleo). No mesmo dia
   (28/10, benchmark variável), a DXVK 2.6.2 deu mediana de **63,7 fps** e o `d3d9.dll` do
   sistema, que é o driver D3D9 nativo da AMD, sem nenhuma tradução, deu **68,7 fps**
   (`work/bench/cpu-dxvk-262` e `cpu-native-d3d9`). O driver do fabricante é um teto otimista
   para qualquer backend que a gente escreva, e ele ganha 5 fps (8%).
2. **No Android ainda não há medida.** Este trabalho deixou a medida pronta (§3), mas não
   havia celular no `adb` nesta sessão. O que o código já diz é que, com o lock global do
   ARM64 (D5), a CPU que importa é a do lock: as chamadas D3D9 rodam com ele, e um ms de
   front-end da DXVK é um ms que nenhuma outra thread do jogo roda. Esse é o número que decide
   (`d3d9_ms` por quadro), e ele agora é medido.
3. **O que o celular mais precisa não exige trocar a DXVK.** ETC2 no lugar do DXT
   decodificado (A3), FIFO em vez de MAILBOX, escala de resolução (A4) e paralelismo no ARM64
   (A1) entram com a DXVK como está. A1 sozinha valeu 2 a 3 vezes o fps no PC
   (`RE5_GUEST_LOCK=hle`).
4. **O custo é de meses.** O front-end D3D9 da DXVK 1.9.2b tem ~23 mil linhas (`src/d3d9`) e
   o compilador de shaders ~6,4 mil (`src/dxso`). Mesmo um "DXVK enxuto" só com o que o RE5 usa
   fica na casa de 8 a 15 mil linhas de C/C++ novo, mais o ciclo de correção visual (§8).

O que muda a decisão está no §6. Se a medida passar do critério, o caminho é o do §4.3: um
backend no nível da API D3D9, atrás de `RE5_GFX`, reaproveitando o compilador de shaders da DXVK.

## 2. O que já está medido

| Medida | Valor | Fonte |
|---|---|---|
| PC, variant benchmark, build `re5_x87b` | 74 fps sem crash | `STATUS.md`, 30/10 |
| PC, DXVK 2.6.2 x D3D9 nativo da AMD (mesmo dia) | mediana 63,7 x 68,7 fps; p10 57,8 x 63,2 | `work/bench/cpu-dxvk-262`, `cpu-native-d3d9`, 28/10 |
| PC, chamadas D3D9 | ~380 a 530 mil/s, ~29 a 55 mil `Draw*`/s a 61 a 72 fps: ~7 mil chamadas e ~600 draws por quadro | linha `fps` do `RE5_FPS_LOG`, mesmo log |
| PC, os mais chamados | `SetStreamSource`, `SetTexture`, `SetVertexShaderConstantF`, `SetPixelShaderConstantF`, `SetRenderState` (50 a 150 mil/s cada) | `thunks/s` do mesmo log |
| PC, CPU da DXVK | ~0,3 núcleo em threads próprias; ~3 a 5% da thread do jogo | sessão que abriu este trabalho |
| Android | GPU Mali-G68 (Galaxy A54), Vulkan 1.3.219, MAILBOX, DXT decodificado na CPU, lock global | `STATUS.md` 4.2c |

O perfil do PC é coerente: ~7 mil chamadas por quadro são muitas para D3D9, mas cada uma
custa dezenas de ns no front-end da DXVK (gravar estado, empurrar um comando para a thread
`dxvk-cs`). Quem paga caro é o código do jogo que as gera, não a DXVK.

## 3. A medida do Android (fase A0)

### 3.1 O que foi acrescentado

- **`d3d9_ms` e `present_ms`** na linha `fps` do `RE5_FPS_LOG` (ms por segundo, somados das
  threads). `d3d9_ms` é o tempo dentro dos métodos do Direct3D 9 do host, sem o Present: a
  ponte mais o front-end da DXVK, na thread do jogo que chama. `present_ms` é só a chamada
  `Present` da DXVK, sem a espera para retomar o lock: é ali que a DXVK espera a GPU quando
  está à frente dela (latência de quadros). Custo: dois `clock_gettime` por chamada D3D9, e só
  com `RE5_FPS_LOG` ligado (`runtime/src/core/thunks.c`, `d3d9.c`).
- **CPU por thread no Linux/Android** (`re5_log_process_cpu`, a cada 30 s com `RE5_FPS_LOG`):
  `/proc/self/task/*/stat`, agrupado em `jogo` (threads do guest, pelo `gettid`), `dxvk`
  (`dxvk-*`), `driver` (`mali-*`, `kgsl*`, Adreno) e o resto pelo nome, com o núcleo onde cada
  thread rodou por último. Antes só existia no Windows.
- **Nome nas threads da DXVK native** (`android/dxvk/prepare.py`): na 1.9.2b o
  `setThreadName` do Linux era vazio, e `dxvk-cs`, `dxvk-submit`, `dxvk-queue` herdavam o nome
  da thread do jogo que criou o device: impossível separar uma da outra.
- **Linha duplicada no logcat** (`runtime/src/core/log.c`): no Android cada linha do log saía
  duas vezes (direto e pelo pipe do stderr); o fps contado pelo logcat dobrava.
- **`scripts/android_perf.py`**: o `thread_cpu.py` do Android. Pelo `adb`, sem root: CPU por
  thread e por grupo do processo `com.re5.recompiled:game`, ms de CPU por quadro, núcleo
  grande ou pequeno, frequência, RAM, uso da GPU quando o kernel expõe (`/sys/kernel/gpu/gpu_busy`
  na Samsung, `kgsl` no Adreno, `ged` na MediaTek), o fps e os ms do `RE5_FPS_LOG` no mesmo
  intervalo. No fim imprime um veredito (§3.3). Teste: `tests/python/test_android_perf.py`.

### 3.2 Roteiro (usuário, com o celular no USB e depuração ativada)

1. Instalar o APK com o `libre5.so` e a `libdxvk_d3d9.so` deste commit.
2. Launcher, **Avançado**: `RE5_FPS_LOG=1`. Entrar numa área de gameplay (a primeira do
   capítulo 1-1, parado 10 s, depois andando).
3. No PC: `python scripts/android_perf.py --secs 60 --label base --out work/android/base.txt`.
4. Repetir com uma mudança por vez (a A0 pede): resolução menor no jogo (separa GPU de CPU),
   `RE5_GUEST_LOCK=hle` (inseguro no ARM64, só para medir o prêmio da A1), `RE5_CPUS=4`.
5. Colar os relatórios em `docs/STATUS.md` (seção da A0) e preencher a tabela do §3.4.

### 3.3 Como ler

| Sinal | Quer dizer | Prêmio |
|---|---|---|
| GPU ≥ 90% ocupada, ou `present_ms` ≥ 25% do quadro | GPU no limite: a CPU espera a GPU no Present | A3 (ETC2: banda), A4 (resolução, FIFO). Um backend próprio só ajudaria pela GPU (§4.4) |
| thread `jogo` principal ≥ 85% de um núcleo grande | CPU da thread do jogo no limite | `d3d9_ms` por quadro é o **teto** do que um backend tira dela; o resto é código do jogo (A1, A5, P5) |
| `lock_ms` alto (≥ metade do quadro, somado) | serialização no lock global | A1 |
| `dxvk-cs` ≥ 85% de um núcleo grande | a thread de comandos da DXVK é o gargalo | backend próprio ou DXVK mais nova (A4.3) |

### 3.4 Resultado no aparelho

Medido em 31/10 no Galaxy A54 (SM-A546E, Exynos 1380, Mali-G68), APK com o commit `15469e27`,
DXVK native 1.9.2b, lock global. A cena é o benchmark variável, disparado sozinho pelo
roteiro do `re5_env.txt` (`RE5_INPUT_SYNC=titlelogo` + `RE5_INPUT_SCRIPT`). Os logs estão em
`work/android/a0_dxvk*` (fora do git, D7).

| Resolução do jogo | benchmark (tela) | mediana `RE5_FPS_LOG` | `d3d9_ms`/quadro | `present_ms`/quadro | thread principal | `dxvk-cs` | driver (`mali-cmar-backe`) | GPU ocupada | clock médio da GPU |
|---|---|---|---|---|---|---|---|---|---|
| 1920x1080 | 14,3 fps | 15,8 fps | 6,7 ms (16%) | 23,4 ms (53%) | 35–42% de um núcleo | 4–9% | 12–26% | 75% (pico 98%) | ~387 MHz |
| 1280x720 | 15,4 fps | 15,6 fps | 5,4 ms (15%) | 18,4 ms (53%) | ~35% | ~7% | ~23% | 76% (pico 89%) | ~239 MHz |

O que isso diz:

- **Não é CPU.** O processo inteiro usa 1,2 a 1,6 núcleo, e nenhuma thread chega perto de 100%.
  A thread principal passa mais da metade do quadro parada dentro do `Present`.
- **Não é resolução.** Com 2,25× menos pixels, o fps sobe 8%.
- **A GPU não está no máximo:** 75% ocupada a 240–390 MHz, num Mali-G68 que vai a ~950 MHz.
  CPU e GPU revezam em vez de trabalhar ao mesmo tempo, e com 75% de ocupação o governador
  de frequência não sobe o clock. É um problema de **pipeline** (quadros em voo, sincronização),
  não de força bruta.
- **Na DXVK 1.9, os quadros em voo são `min(3, BackBufferCount + 1)`** (`d3d9_swapchain.cpp`,
  `GetActualFrameLatency`), e o RE5 pede 1 back buffer: são **2 quadros**. A swapchain em si
  já tem 5 a 6 imagens (MAILBOX, 2340x1080).
- **A DXVK 1.9 cai no driver Mali:** SIGSEGV em `vkCmdBindPipeline`, chamado por
  `DxvkContext::updateGraphicsPipelineState` na `dxvk-cs` (processo 7010, logo depois do
  primeiro `SetVertexShaderConstantB`).
- O teste E1 (`d3d9.numBackBuffers = 3` num `dxvk.conf` pelo `DXVK_CONFIG_FILE`, 3 quadros
  em voo) ainda não tem uma rodada limpa: a primeira foi interrompida por fora e voltou
  para 800x600.

## 4. Onde cortar

### 4.1 As três opções

| | A. API D3D9 ("DXVK enxuto") | B. `cRenderDevice` da MT Framework | C. Ajustar a DXVK |
|---|---|---|---|
| O que é | Um `re5_d3d9_backend` novo: objetos COM do host em C/C++ com só os métodos que o RE5 chama, traduzidos direto para Vulkan | Overrides nativos (P5) das funções do dispositivo de render da engine: o jogo nunca chama D3D9 | Patches na DXVK que já usamos (`prepare.py`), com dicas específicas do RE5 |
| Encaixe | Entra pelo `re5_d3d9_backend` (`d3d9_bridge.h`): a ponte, as sombras e o `Lock/Unlock` não mudam | Precisa da engenharia reversa da `cRenderDevice`, do formato das listas de comando e dos materiais | Nenhum código novo no runtime |
| Shaders | Bytecode D3D9 SM3 (`vs_3_0`/`ps_3_0`) -> SPIR-V | Os mesmos bytecodes D3D9 (os `.mfx` da engine trazem SM3 compilado) | Como hoje |
| Chamadas por quadro | As mesmas ~7 mil | Poucas centenas (a engine sabe o que muda) | As mesmas |
| A/B e oráculo | Chave em tempo de execução; DXVK como oráculo, quadro a quadro (`RE5_D3D9_SHOT`, `snap_diff.py`) | Difícil: o oráculo é o jogo inteiro | `dxvk.conf` / build alternativo |
| Risco | Médio: semântica do D3D9 é conhecida e documentada | Alto: estruturas sem nome, contraria D4, e todo bug vira "a engine fez o quê?" | Baixo |
| Esforço | Meses | Meses, mais RE | Dias por ajuste |

**B fica descartada.** O único ganho que ela tem a mais, menos chamadas, só importa se a
thread do jogo gastar tempo nas chamadas, e isso é exatamente o `d3d9_ms` que a A0 mede. Se o
`d3d9_ms` for pequeno, B não ganha nada; se for grande, A já tira a maior parte dele com risco
menor. Além disso, B troca um contrato documentado (D3D9) por estruturas da Capcom sem nome.

**C vem antes de A.** Tudo que é específico do RE5 e do Mali (load/store dos render passes,
profundidade transitória, formatos) dá para pôr na DXVK como patch, do mesmo jeito que o porte
do NFSU2 já faz com os recursos opcionais do Mali. Só quando o critério do §6 apontar o custo
de CPU do front-end ou da `dxvk-cs` é que A se paga.

### 4.2 Por que não "reescrever a DXVK"

A DXVK é um tradutor genérico: aceita qualquer jogo D3D9, então guarda e valida estado que o
RE5 nunca usa, mantém o modelo de recursos inteiro do D3D9 (pools, `Lock` com qualquer flag,
`GetRenderTargetData`...) e grava tudo numa fila para a `dxvk-cs`. Um backend próprio ganha
porque é **fechado no RE5**: o conjunto de formatos, estados, tipos de shader e padrões de
`Lock` é finito e medível. A regra de D8 vale aqui também: método ou estado fora do conjunto
aborta com o nome, nunca faz de conta.

### 4.3 Desenho de A, se a medida mandar

```
 código do guest ──> ponte (d3d9.c: objetos-sombra, sombras de VB/IB/textura)
                        │  re5_d3d9_backend (d3d9_bridge.h)
                        ├── "dxvk"  : Direct3DCreate9 da DXVK (como hoje, padrão)
                        └── "re5vk" : runtime/src/gfx/vk/ (novo)
                               ├─ device.c     IDirect3DDevice9 do host: estado sujo em bits
                               ├─ resources.c  VB/IB em anel por quadro, texturas, RTs
                               ├─ shaders.cpp  dxso da DXVK (vendorizado) -> SPIR-V, cache em disco
                               ├─ pipelines.c  estado -> VkPipeline (hash), cache em disco
                               └─ present.c    swapchain FIFO, escala de resolução
```

- **Chave:** `RE5_GFX=dxvk|re5vk` (padrão `dxvk`), lida no `d3d9_host.c`, que já escolhe a
  DLL/`.so` da DXVK por variável. No launcher, o campo Avançado já grava `re5_env.txt`.
- **A ponte não muda.** O backend entrega um `IDirect3D9*` do host com a mesma ABI COM; os
  217 repassadores gerados e os handlers à mão continuam iguais. Por isso as duas pontas
  podem ser comparadas no mesmo build.
- **Um backend só, PC e Android.** O mesmo `runtime/src/gfx/vk/` compila no Windows x64 e no
  Android ARM64 (pedido do usuário, 30/10): a mesma chave `RE5_GFX` nos dois. O que muda por
  host fica em três pontos: a superfície (`SDL_Vulkan_CreateSurface` com o HWND da janela SDL no
  PC; o `ANativeWindow` do `android_wsi.cpp` no celular), o carregador (`vulkan-1.dll` /
  `libvulkan.so`, ou o driver do `libadrenotools`) e os formatos (BC no PC; ETC2 ou RGBA8 no
  Mali). Requisito mínimo: Vulkan 1.1 com o resto opcional, para servir o Mali-G68
  (1.3.219) e a Radeon Vega do usuário (1.3.260, que recusa a DXVK 3.x).
- **O PC é a plataforma de desenvolvimento.** V2 a V5 (§7) são feitas e validadas primeiro
  no PC: camadas de validação do Vulkan, RenderDoc, e a DXVK 2.6.2 como oráculo na mesma
  máquina, no mesmo build, trocando só `RE5_GFX`. O Android entra em cada etapa depois que o
  PC passa, só com o que é dele (formatos, tiler, superfície).
- **Sem thread de comandos no começo.** O front-end grava direto no command buffer do
  quadro, na thread que chama. A `dxvk-cs` existe para tirar trabalho da thread do jogo; aqui
  o objetivo é que não haja trabalho a tirar. Uma thread de submissão (`vkQueueSubmit` e
  `vkQueuePresentKHR`) fica, porque o driver bloqueia nela.

### 4.4 O que um backend próprio pode ganhar na GPU (tiler)

O Mali é uma GPU por tiles: cada render pass carrega e grava a memória do tile, e banda é o
recurso escasso. A DXVK não sabe se a profundidade de um passe vai ser lida depois (o D3D9
permite), então grava (`storeOp = STORE`) sempre. Com o conhecimento do quadro do RE5
(quais RTs são lidos como textura, quais são descartados) um backend pode usar
`storeOp = DONT_CARE`/`NONE` e anexos transitórios. **Isso também cabe na opção C** (um patch
com a lista de superfícies do RE5), e só vale se a GPU for o gargalo (§3.3, primeira linha).

## 5. Partes técnicas de A

### 5.1 Shaders (SM2/SM3 -> SPIR-V)

- O RE5 é um jogo SM3 (`vs_3_0`/`ps_3_0`; a versão de cada shader criado entra no censo
  V1, que confirma se há SM2 ou *fixed function* misturados). **Reaproveitar o `dxso` da
  DXVK** (`src/dxso`, ~6,4 mil linhas, licença zlib) como biblioteca: ele já traduz o bytecode
  D3D9 para SPIR-V com o módulo `src/spirv`. Escrever um tradutor do zero não tem ganho.
- O `dxso` gera variações por constantes de especialização (tipo de sampler, `alphatest`,
  fog, projeção de textura). Num jogo fechado, o conjunto de variações é finito: gravar os
  hashes que aparecem e **pré-compilar no carregamento**, com cache em disco (o mesmo papel do
  `DXVK_STATE_CACHE_PATH`).
- Precisão: o Mali roda `mediump` bem mais rápido. `RelaxedPrecision` só nos pixel shaders e
  só se a comparação de quadro (§7) não mostrar diferença.
- *Fixed function*: medir antes se o RE5 desenha algo sem shader (UI, vídeo). Se sim, um
  par de shaders fixos cobre; não portar o gerador de FF inteiro da DXVK.

### 5.2 Estado e pipelines

- Estado D3D9 (`SetRenderState`, `SetSamplerState`, `SetTexture`, `SetStreamSource`,
  constantes) em blocos com bit de sujo. No `Draw*`: hash do estado de pipeline -> `VkPipeline`
  numa tabela; o resto vira estado dinâmico.
- O Mali-G68 informa Vulkan 1.3: `extendedDynamicState` (cull, depth, stencil, topologia) é
  do núcleo 1.3, o que corta a explosão de pipelines. `extendedDynamicState2/3` só se o
  `vulkaninfo` do aparelho mostrar.
- Constantes: os mais de 100 mil `Set*ShaderConstantF`/s do PC vão para um anel de uniform buffer
  por quadro com offset dinâmico, um upload por draw só da faixa suja.
- Descritores: um conjunto por draw de um pool por quadro (como a DXVK 1.9), ou *push
  descriptors* se o aparelho tiver `VK_KHR_push_descriptor`.

### 5.3 Recursos e texturas

- **VB/IB:** a ponte já mantém a sombra no guest e copia no `Unlock`. Com `D3DLOCK_DISCARD`
  e `NOOVERWRITE`, a cópia vai direto para um anel *host-visible* (sem a cópia intermediária
  da DXVK).
- **DXT:** BC1–BC3 quando `textureCompressionBC` existe (PC, Adreno com Turnip); ETC2/EAC
  quando houver o cache da A3; senão o `dxt_decode.h` de hoje. É a mesma escolha que a A3 fará
  na DXVK; o código de transcodificação é comum às duas.
- Render targets, `StretchRect`, `GetRenderTargetData`, consultas de oclusão: só os usos que
  o RE5 faz, medidos pelo censo de métodos (§7, etapa V1).

### 5.4 O que reaproveitar da DXVK

| Parte | Reaproveitar? |
|---|---|
| `src/dxso` + `src/spirv` (tradução de shaders) | **Sim**, vendorizado como está |
| Tabelas de formato D3D9 -> Vulkan (`d3d9_format.cpp`) | Sim, como referência/tabela |
| `src/dxvk` (backend genérico: barreiras, render passes, memória) | Não: é o que o backend próprio substitui. Ler o rastreamento de barreiras e o alocador como referência |
| WSI do Android (`android/dxvk/android_wsi.cpp`) | Sim: a superfície e a swapchain são as mesmas |
| State cache da DXVK | Não; o formato é dela. Um cache próprio por hash |

## 6. Critério para abrir o backend próprio

Abrir a opção A (etapas V2 em diante) se, na cena de gameplay do roteiro do §3.2 e **com a
A1 já feita** (senão o lock mascara tudo):

- `d3d9_ms` por quadro ≥ **20% do quadro** na thread do jogo que está no limite, **ou**
- `dxvk-cs` (ou `dxvk-submit`) ≥ 85% de um núcleo grande, com o fps limitado por ela.

E a GPU **não** pode estar no limite (§3.3): se estiver, o caminho é A3/A4 e a opção C.

O ganho esperado, para conferir depois: se o backend cortar metade do `d3d9_ms` (a DXVK já é
eficiente; metade é otimista para um front-end mais simples), o quadro cai `d3d9_ms / 2`. Com
20% do quadro em D3D9, isso dá ~11% de fps. Abaixo disso, o esforço do §8 não se paga.

## 7. Etapas incrementais (cada uma com a chave e um A/B)

| Etapa | O que | Pronto quando | Chave |
|---|---|---|---|
| **V0** | Medida (este commit): `d3d9_ms`, `present_ms`, CPU por thread no Android, `android_perf.py` | Tabela do §3.4 preenchida | `RE5_FPS_LOG=1` |
| V1 | Censo de uso da API: métodos, formatos, estados, flags de `Lock`, shaders únicos e variações por cena, gravados num arquivo | Lista fechada do que o RE5 usa | `RE5_D3D9_CENSUS=<arquivo>` |
| V2 | `re5vk` mínimo: instância, device, swapchain FIFO, `Clear` + `Present`; todo o resto aborta com o nome (D8) | Tela limpa na cor do `Clear`, sem crash, primeiro no PC (validação limpa), depois no Android | `RE5_GFX=re5vk` |
| V3 | Recursos: VB/IB em anel, texturas (BC/ETC2/RGBA), RTs | `test_d3d9` com o backend novo; vídeo de abertura (só textura) igual à DXVK | idem |
| V4 | Shaders pelo `dxso` + cache; pipelines | Menu principal: `snap_diff.py` contra a DXVK abaixo do limiar | idem |
| V5 | Gameplay: o resto dos métodos do censo V1 | Capítulo 1-1 inteiro; quadros 300/600/900 contra a DXVK | idem |
| V6 | Tiler: load/store, anexos transitórios, MSAA resolvido no tile | Banda menor no Mali (Arm Performance Studio) sem diferença de quadro | `RE5_GFX_TILER=1` |

O caminho DXVK continua o padrão até V5 passar no PC **e** no Android com fps maior ou igual.
Em cada etapa: `ctest` verde, e o A/B pelo `bench_re5.py --env RE5_GFX=...` (PC) e pelo
`android_perf.py` (celular).

## 8. Esforço e ganho

| Etapa | Esforço (uma pessoa, tempo dedicado) | Ganho |
|---|---|---|
| V0 | feito | a decisão |
| V1 | 2 a 3 dias | a lista do que portar; útil também para a A3 |
| V2 | 1 semana | nenhum (infraestrutura) |
| V3–V4 | 3 a 5 semanas | nenhum ainda: paridade visual |
| V5 | 3 a 6 semanas, mais a cauda de bugs visuais | até `d3d9_ms`/2 por quadro (§6) |
| V6 | 1 a 2 semanas | só se a GPU for o gargalo |

**PC:** o teto medido é o D3D9 nativo da AMD, +5 fps (8%). Um backend próprio, sem as
otimizações de 20 anos do driver, deve ficar abaixo disso. **Pelo desempenho, não vale no
PC**; o backend serve ao PC (§4.3) porque é lá que ele é desenvolvido e comparado, e porque
um código só para os dois hosts custa menos que dois. O ganho que justificaria o trabalho tem
que vir do Android.
**Android:** desconhecido até a V0 rodar no aparelho. O modelo do §6 diz o tamanho; o
critério diz se vale.

## 9. Próximo passo

Rodar o roteiro do §3.2 no Galaxy A54 e preencher o §3.4. Com o número em mãos, revisar a
ordem da matriz do plano mestre (§10): se o veredito for GPU, A3/A4; se for o lock, A1; se for
o `d3d9_ms` acima do critério do §6 depois da A1, começar a V1.
