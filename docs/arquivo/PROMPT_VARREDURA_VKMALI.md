# Prompt: varredura do backend Vulkan nativo (vkmali / vkadreno)

Cole o texto abaixo numa sessão nova de IA aberta na raiz do repositório.

---

Você vai trabalhar no backend Vulkan nativo do RE5 recompilado (device D3D9 próprio, sem DXVK),
nos modos `vkmali` e `vkadreno` (`RE5_GFX=vkmali|vkadreno`). Antes de tudo, leia `CLAUDE.md`,
`docs/STATUS.md`, `docs/ARQUITETURA.md` (decisão D10) e `docs/PROJETO_VULKAN_NATIVO.md`.
Escreva em português (código, comentários, mensagens).

## Regras desta tarefa (leia primeiro)

- **Você NÃO vai abrir o jogo**, nem no PC nem no celular. Nada de `re5.exe`, `bench_re5.py`,
  `adb shell am start`, roteiro de teclas ou capturas de tela.
- O que você pode rodar: compilar (PC e Android), `ctest`, os testes unitários, o tradutor de
  shaders sobre o despejo `work/shaders/` com o `spirv-val` do NDK, e análise de código.
- **No final**, gere o APK limpo e instale no celular (se ele estiver no adb), ou deixe o APK pronto
  e diga o caminho. O usuário testa à mão no celular e manda fotos e descrição.
- Junto com o APK, entregue um **plano de teste para o usuário**: o que olhar, e quais linhas pôr no
  `re5_env.txt` para ligar/desligar cada mudança se algo der errado (cada mudança entra com uma
  chave `RE5VK_*`). Antes de instalar, deixe o `re5_env.txt` do celular só com o necessário
  (`RE5_GFX=vkmali` e o que o plano pedir), sem roteiro de teclas.
- Depois de instalar, confira os caches do app (`adb shell run-as com.re5.recompiled ls files`):
  `texcache/` (texturas ETC2, versão no cabeçalho) e `re5vk.pcache` (pipelines).

## Aparelhos e fatos estabelecidos

- Celular de teste: Galaxy A54, Mali-G68, driver **r38p1**, Vulkan 1.3.219, tela 2340x1080.
  O usuário também tem Adreno (`vkadreno`): **os mesmos defeitos aparecem lá**. Então não é bug
  de um driver só: é o nosso backend dependendo de algo que GPU desktop tolera e GPU móvel
  (tiler) não.
- **A DXVK funciona neste celular** (imagem certa, FPS parecido, menos GPU). O código do jogo
  recompilado para ARM está certo; os defeitos são do backend nativo (`runtime/src/gfx/vk/`,
  `runtime/src/gfx/vkn/`). A build da DXVK do Android está quebrada no momento por mudanças locais.
- **No PC (Intel UHD / NVIDIA) o backend nativo renderiza certo** com o mesmo SPIR-V.
- **Referência de comportamento: o código da DXVK no repositório**: compilador de shaders em
  `third_party/dxvk-native/src/dxso/` e device em `third_party/dxvk-native/src/d3d9/`. Para cada
  suspeita, compare caso a caso com o nosso tradutor (`vk_dxso.c`) e o nosso device
  (`vk_draw.c`, `vk_res.c`, `re5vk.c`, `vkn/*.cpp`).
- Despejo dos shaders reais do jogo (do PC): `work/shaders/` (164 `vs_*.bin` / `ps_*.bin`).
  `RE5_SPIRV_VAL=<ndk>/shader-tools/windows-x86_64/spirv-val.exe RE5VK_NANGUARD=1
  build/host-tests/tests/test_vk_dxso.exe work/shaders` traduz todos e valida.

## Defeitos conhecidos, em ordem de prioridade

### 0. GPU em 100% na tela de carregamento, jogo trava e o celular REINICIA (urgente)
- Apareceu com a última build. Novidades dela (todas com chave para desligar):
  `RE5VK_KILL_DEPTH` (PS que descarta grava `FragDepth = FragCoord.z` com DepthUnchanged),
  `RE5VK_NAN_CULL` (posição com NaN vira NaN inteira), máscaras de stencil 0xFF no estado estático
  do pipeline (sem chave: `vk_draw.c`, `pipeline_create`), e a passada só de profundidade voltou
  a ser inteira por padrão (`RE5VK_ZPREPASS=1`).
- O PPSSPP registra "device lost" ao escrever `gl_FragDepth` com teste de profundidade NEVER: o
  `KILL_DEPTH` é o suspeito principal da trava. Restrinja-o (só onde há descarte e teste de
  profundidade ativo, nunca com ZFUNC NEVER/ALWAYS sem escrita) ou desligue por padrão até ser
  testado, e ponha no plano de teste a bissecção pelas chaves.
- **Medido pelo usuário**: na tela de carregamento o FPS oscila até ~22 com a GPU em 100%; no menu
  fica em ~80. Então o carregamento faz trabalho pesado de verdade na GPU (não é falta de limite de
  quadros). Suspeito principal: cada upload de textura (`vk_res.c`, `upload`) chama
  `re5vk_end_rendering` e faz transições de layout; no carregamento sobem milhares de texturas, e
  num tiler cada fechamento/reabertura do render pass grava e relê o tile da tela inteira
  (2340x1080). Agrupe os uploads: grave as cópias e transições num command buffer de upload
  separado (ou no começo do envio, antes de abrir o passe) e não feche o passe da cena por causa
  de upload. Conte por quadro: passes abertos, uploads, barreiras.
- **Sem limite de quadros** (secundário, mas vale): o swapchain prefere `MAILBOX`/`IMMEDIATE`
  (`IRenderer.cpp`), o jogo pede intervalo imediato, e o único limitador é `RE5_FPS_CAP` (env).
  O menu roda a ~80 fps sem necessidade, gastando bateria e calor. Ponha um limitador por padrão no
  Android (FIFO, ou ritmo no Present com `RE5_FPS_CAP` só para mudar o valor), sem quebrar o PC.
- Um reinício do aparelho também pode vir de GPU hang: procure leituras fora de limite (UBO
  relativo, vertex buffer além do tamanho, índices além dos vértices), laços sem fim no shader
  (`loop`/`rep` com contador vindo do UBO, `breakc`), e falta de barreira entre escrita e leitura
  de imagem/buffer.

### 1. Linha / listras atravessando a cabeça dos personagens
- Só com a cabeça de um personagem na tela; sem cabeça visível, não aparece. Antes eram triângulos
  gigantes saindo do corpo; hoje é uma linha atravessando a cabeça.
- Descartado: formatos de vértice (SHORT4N, UBYTE4 USCALED, UBYTE4N, FLOAT16_2: suportados e
  iguais aos do PC); constantes de PS (iguais no PC e no ARM); anel de UBO (espera o fence do slot,
  memória coerente 0x7); troca de VkBuffer no Lock DISCARD; cache de bind de vertex buffer.
- Pista principal: 6 VS de skinning leem textura no VS (`texldl`, provavelmente animação facial):
  `vs_49b28f19`, `vs_548f7a0b`, `vs_a3262cc3`, `vs_e5b76be9`, `vs_f6850db3`, `vs_fe8cde5e`.
  Compare com a DXVK: lod explícito, formato e filtro da textura de vértice, sampler dos slots
  D3DVERTEXTEXTURESAMPLER0..3 (257..260), barreira/layout quando a GPU escreveu essa textura antes,
  e o `ifc` desses shaders.
- Também: endereçamento relativo `c[a0.x + n]` (`read_const`, `rel_clamp`; hoje o índice é preso
  em [0, n-1]); a DXVK (issue #3861) lembra que o Vulkan não checa limites de UBO e o D3D define o
  comportamento: veja o que a DXVK devolve fora da faixa. E o arredondamento do `mova`.
- Já existe (não comprovado como causa): fallback NEAREST para formato sem filtro linear
  (`format_linear` em vk_draw.c) e `RE5VK_NAN_CULL`.

### 2. Folhas, cerca de arame e cabelo com retângulo preto
- Causa localizada: a passada só de profundidade (draws sem escrita de cor, ZWRITE 1, ZFUNC
  LESSEQUAL). O PS dela, `ps_5a40b59f`: `texld r0, v0, s0; add r0, r0.w, -c0.x; texkill r0`, com
  c0.x = 0.999 (igual no PC e no ARM). No celular o descarte não vale nessa passada e a
  profundidade do quad inteiro fica gravada; na passada de cor o recorte funciona e sobra o fundo
  preto. Anexo de profundidade D24S8 (formato Vulkan 129); alvo de cor B8G8R8A8 (44).
- Sem efeito no celular: texkill adiado (`RE5VK_LATE_KILL`, resolveu o cabelo da Sheva na passada
  de cor), escrita de cor forçada com blend ZERO/ONE (`RE5VK_MASKED_KILL=1`), posição Invariant
  (`RE5VK_POS_INVARIANT`).
- Pular a passada inteira (`RE5VK_ZPREPASS=0`) deixa as folhas certas, mas personagens aparecem
  através das paredes. `RE5VK_ZPREPASS=2` pula só os draws dela cujo PS descarta (contorno).
- Pesquisa: PPSSPP (lista de bugs de driver Vulkan) documenta `discard` ignorado no Mali < r40 e no
  Adreno 500 em combinações de profundidade/stencil, com contorno `FragDepth = FragCoord.z`
  (é o `KILL_DEPTH`, ver item 0); ANGLE documenta bug da ARM (corrigido no r43) com máscara de
  escrita de stencil 0 no estado estático + `discard` (já contornado com 0xFF); PPSSPP também
  documenta que a máscara de cor 0 não é respeitada no Adreno 5xx (contorno: blend ZERO/ONE).
  A DXVK usa `OpDemoteToHelperInvocation` quando disponível.
- Confira também o render pass dessa passada (`IRenderer.cpp`: `renderPassFor`, `MaliRenderer`,
  anexos `VK_ATTACHMENT_UNUSED`, máscara de escrita 0) e load/store da profundidade entre a passada
  de profundidade e a de cor (política TBDR: DONT_CARE/NONE).

### 3. Luz e sombra às vezes erradas
- Não investigado. Comece por: formatos dos mapas de sombra e texturas float amostradas (filtro
  linear, `compare`, `INTZ`), o NANGUARD (só Android: `vk_dxso.c`, `nan_guard`), o FLOATEMU, e
  viewport/scissor.

### 4. Desempenho: GPU muito mais alta que a DXVK para o mesmo FPS
Meça pelo código antes de mudar (conte operações por quadro com contadores no log, que o usuário
roda com `RE5_FPS_LOG=1`; o log já imprime fps, draws e `gpu_ms`). Itens gerais de Vulkan, não só
de Mali:
- **Ritmo de quadros**: limitador por padrão (item 0); present mode e número de imagens do
  swapchain (hoje 5); não deixar a CPU encher 8 envios à frente da GPU (`VK_SLOTS 8`).
- **Render passes**: quantas vezes por quadro o passe abre e fecha (cada quebra num tiler grava e
  relê o tile inteiro); juntar draws no mesmo passe; CLEAR/DONT_CARE em vez de LOAD/STORE sempre
  que o conteúdo anterior/posterior não é usado; clear dentro do passe por loadOp em vez de
  `vkCmdClearAttachments`; evitar passe só de clear (bug conhecido do Mali); resolve e cópias.
- **Barreiras e layouts**: barreiras com estágios e acessos exatos (sem ALL_COMMANDS), agrupadas;
  nada de `GENERAL` sem necessidade; transição de layout só quando muda.
- **Pipelines**: cache em disco (`re5vk.pcache`, já gravando); evitar variantes desnecessárias
  (cada estado fixo na chave gera pipeline novo); criar pipelines fora do quadro (aquecimento na
  carga); medir quantos pipelines novos por quadro.
- **Descritores e constantes**: hoje cada draw que muda constante sobe o bloco inteiro (até 256
  vec4 do VS + 224 do PS) para o anel de UBO (8 MB por envio); subir só a faixa usada/alterada,
  usar o `const_f` real, push constants para dados pequenos (referência do alpha test); cache de
  descriptor sets (já existe; medir acertos); offsets dinâmicos.
- **Envios**: um `vkQueueSubmit` por envio; quantos por quadro; esperas em fence (o log tem
  `lock_ms`, `present_ms`); evitar flush no meio do quadro (anel cheio, pool de descritores cheio).
- **Uploads**: anel de staging, uploads agrupados por envio, transcodificação DXT→ETC2 (cara na
  primeira carga; cache em disco), mipmaps.
- **Memória**: VMA; anexos transitórios em memória LAZY (tipo 0x11 existe no Mali); nada de
  host-visible para textura grande quando o device-local puro existe.
- **Shaders**: rodar `spirv-opt -O` no SPIR-V gerado (ou equivalente) se disponível no NDK;
  remover código morto; o NANGUARD e o FLOATEMU custam instruções em todo `mul`/`dp`/`rsq` —
  meça e restrinja ao necessário; `RelaxedPrecision` só onde for seguro (nunca em posição,
  coordenada de textura ou profundidade); poucos temporários; constantes `def` como constantes do
  SPIR-V.
- **Vértices/índices**: formatos nativos, sem conversão no shader quando a GPU suporta o formato.
- **CPU do device**: custo por draw (hash da chave de pipeline, varredura de samplers, cópias de
  estado); o jogo faz ~2 mil draws por quadro (~63 mil por segundo a 30 fps, `draws` no log do PC).

## Alvo: Vulkan 1.1

O backend deve funcionar com **Vulkan 1.1** para maior compatibilidade (aparelhos antigos, drivers
sem atualização). Hoje ele exige mais que isso:
- `extendedDynamicState` é obrigatório (`VulkanContext.cpp`, `requireExtendedDynamicState`):
  cull, front face, topologia, teste/escrita/comparação de profundidade, stencil e stride de
  vértice dinâmicos, com `vkCmdBindVertexBuffers2`. Isso é extensão (núcleo só no 1.3).
- Há caminhos com `vkCmdBeginRendering` (dynamic rendering, 1.3) e uso de `eds2`.
Plano: detectar cada recurso e, sem ele, pôr o estado na chave do pipeline (estado estático) e
usar `vkCmdBindVertexBuffers` com stride no pipeline, e VkRenderPass (já é o caminho do vkmali).
`instanceApiVersion` 1.1 como mínimo; extensões só opcionais. Teste unitário para a escolha de
caminho (`tests/unit/test_vkn_policy.cpp`). A DXVK legada (1.10.x) e o PPSSPP rodam em Vulkan
1.0/1.1: use-os como referência de como viver sem dynamic state.

## Já feito (não desfaça sem motivo)

- Conversor DXT→ETC2/EAC corrigido (sinal dos modificadores; EAC exato em 0/255), teste contra
  decodificador de referência (`tests/unit/test_dxt_to_etc2.c`); cache de texturas v4.
- `re5_env.txt` aceita chaves `RE5VK_*` (antes eram ignoradas sem aviso).
- Cache de pipelines: o contador `npipes` não incrementava e o cache nunca era gravado; corrigido.
- texkill adiado; posição Invariant; fallback NEAREST; KILL_DEPTH; NAN_CULL; máscaras de stencil
  0xFF; modos de `RE5VK_ZPREPASS`.
- Diagnósticos: `RE5VK_RS_LOG=1` (estados, constantes, draws sem cor, formatos de vértice,
  declarações de skinning, consultas FOURCC), `RE5VK_SKIP_NOCOLOR`, `RE5VK_SKIP_REL`,
  `RE5VK_REL_CLAMP`.

## Build

- Android: NDK 30 em `build/android-arm64-ndk30` (`cmake --build`), `llvm-strip
  --strip-unneeded` para `android/app/src/main/jniLibs/arm64-v8a/libre5.so`, depois
  `android/gradlew.bat assembleDebug` com `JAVA_HOME="C:/Program Files/Android/Android Studio/jbr"`.
  Saída: `android/app/build/outputs/apk/debug/app-debug.apk`.
- PC (testes): `cmake --build build/host-tests` e `ctest` em `build/host-tests` (o teste
  `kernel32` falha de vez em quando, sem relação).

## Regras do projeto

- `CLAUDE.md` vale: falhar alto (D8), nada de stub silencioso; teste novo ou estendido em `tests/`
  para cada mudança; `ctest` verde; nunca commitar `generated/src`, `work/`, `build/` nem arquivos
  do jogo; não commitar sem o usuário pedir; `STATUS.md` só recebe "FEITO" com evidência do
  celular (vinda do usuário).
- Toda mudança de comportamento entra com uma chave `RE5VK_*` para desligar e comparar.
- Não chame de corrigido o que não foi visto no celular. Contorno é contorno: diga que é.
- Registre o que foi descartado e por quê.
