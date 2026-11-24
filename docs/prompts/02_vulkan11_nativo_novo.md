# Prompt 2: Vulkan 1.1 nativo novo, limpo e otimizado

Cole o texto abaixo numa sessão nova de IA aberta na raiz do repositório. Base: `docs/prompts/00_BASE_LEAN_2026-11-23.md`.

---

Você vai criar um **backend Vulkan 1.1 nativo novo** para o RE5 recompilado, a partir de código limpo, e fazê-lo **passar a DXVK**
no Galaxy A54 (Mali-G68). Na base lean a DXVK 1.9.2b faz **35 de pico / 24,8 de média** e o backend atual (vkmali) **29 / 21,8**:
a meta é superar 35 / 24,8, no mesmo trecho, em 360p. Leia primeiro `docs/prompts/00_BASE_LEAN_2026-11-23.md`, depois `CLAUDE.md`,
`docs/STATUS.md`, `docs/ARQUITETURA.md` (D10), `docs/PROJETO_VULKAN_NATIVO.md`, `docs/PESQUISA_VULKAN_ANDROID.md` e
`docs/RELATORIO_GPU_CPU_ENGASGOS_2026-11-20.md`. Em português.

## Escopo

- Novo módulo ao lado do atual (`runtime/src/gfx/vk2/` ou nome que o código pedir), escolhido por `RE5_GFX=vkmali2`. O `re5vk`
  atual **fica intacto como fallback**; não o refatore no lugar. Mesmo contrato: um `IDirect3DDevice9` do host
  (`runtime/include/re5/d3d9_bridge.h`), mesma ponte `vkn` (`runtime/src/gfx/vkn/`) para instância/dispositivo.
- **Vulkan 1.1 puro**: sem extended dynamic state, sem robustness2/nullDescriptor, sem `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE`
  (quebrado no Mali r38; stride no pipeline), no máximo **4 conjuntos de descritores** (limite de Mali Valhall).
- **Reaproveite o que está provado**, não reescreva: o tradutor SM3→SPIR-V (`runtime/src/gfx/vk/vk_dxso.c`, com testes e
  `spirv-val`), a conversão DXT→ETC2 paralela (`vk_etc2_pool.c`, `re5_dxt_to_etc2.h`), o cache de texturas e de pipelines, a
  thread de present, a troca de janela ao voltar do segundo plano (`re5vk` `surface_follow`, portão em `d3d9_host.c`).
  Reescreva o **núcleo de desenho**: estado, passes, descritores, envio.

## O que a medição diz sobre onde o vkmali perde

- Em cena pesada o jogo faz **~85–100 passes de renderização por quadro** (a maioria com 1 draw: pós-processamento e cadeias de
  mip/bloom); no A54 cada passe pequeno custa ~0,45 ms **fixos** (latência de job do Mali JM, não trabalho): 878 ms/s dentro de
  passes contra 5 ms/s entre eles (medido em 720p, no relatório de 20/11; em 360p a conta precisa ser refeita).
  Resolução menor quase não ajudou (+3 fps) por isso. O que fecha passes hoje: `SetRenderTarget`, `SetDepthStencil`,
  `StretchRect`, `Present`.
- A DXVK faz o mesmo jogo com menos custo: descubra **como**. Rode a DXVK com `DXVK_HUD=devinfo,fps,frametimes,submissions,
  drawcalls,pipelines,gpuload` e conte passes, envios, barreiras e pipelines por quadro; leia `third_party/dxvk-native`
  (1.9.2b, `src/d3d9/`, `src/dxvk/dxvk_context.cpp`, `dxvk_cmdlist`) para ver como ela agrupa trabalho, decide `loadOp`/`storeOp`,
  emite barreiras e quando faz `vkQueueSubmit`.
- Perfil do driver: a thread `mali-cmar-backend` gasta 14,5% do total (quase tudo em `ioctl`): cada envio e cada passe custa.

## Princípios de desenho (com a evidência que temos)

1. **Menos passes, mais draws por passe.** Junte passes consecutivos no mesmo alvo; adie o fechamento (`RE5VK_LAZY_RT` já existe,
   ganho pequeno: estude por quê); **suba uploads e transições de layout para antes do passe** (o `pgr4-android` derrubou de
   ~1.131 quebras de passe por quadro para 74–83 só fazendo isso). Trate `StretchRect` sem fechar passe quando possível (cópia/blit
   no mesmo comando fora do passe, ou resolver por amostragem).
2. **Um `vkQueueSubmit` por quadro** (hoje há envios extras para uploads e `GetRenderTargetData`); sem espera de fence no meio.
3. **Pipelines fora do caminho do jogo**: `VkPipelineCache` persistente (já existe), pré-aquecimento numa thread a partir do cache
   de estados (`pgr4`: zero criações na thread principal em 485 pipelines). Cuidado: no Simpsons a compilação assíncrona causava
   pausas e foi desligada; ofereça a chave e meça engasgos.
4. **Constantes e vértices**: memória visível pela CPU e coerente (já é assim), UBO com offsets dinâmicos num anel por quadro,
   sem atualizar descritores por draw; cache de conjuntos de descritores entre quadros.
5. **Sem sincronização GPU→CPU**: `GetData` de consultas dado como pronto por atraso (`RE5_QUERY_LAG`, só seguro com
   `Lock(NOOVERWRITE)` respeitado), nada de `vkWaitForFences` no meio do quadro.
6. **TBDR**: `loadOp = DONT_CARE/CLEAR` sempre que o conteúdo anterior não é lido, `storeOp = DONT_CARE` para profundidade/
   stencil não lidos depois (o oráculo está em `vk_tbdr.c`), layouts amigáveis a AFBC (`re5vk_layout_*`).
7. **Medição barata**: contadores por quadro, nada de `clock_gettime` por chamada fora de `RE5VK_TIMING=1`.
8. **Sem alocação no caminho quente** e sem mutex global por chamada do D3D9: o jogo chama a API de duas threads, mas a gravação
   de comandos pode ser de uma só se a outra enfileirar.

## Referências externas lidas (23/11, resumo; os relatórios vieram de leitores web, confira antes de depender)

- `Meller2/pgr4-android` (MIT): hoist de uploads (1.131→~80 quebras de passe), pré-carga de pipelines, cache de descritores.
- `victorgbd/NFSMW-Recompiled-Mobile` (GPL-3): Mali Valhall só expõe 4 conjuntos de descritores (`vkCreatePipelineLayout`
  falha com 5); sincronização que o Adreno perdoava e o Mali-G57 não; consultas de oclusão do sol evitadas no Mali.
- `SANDRO00O/GeneralsZH-Android-Port` (GPL-3): DXVK 2.6 com patches `dxvk-vulkan11-adaptive.patch` e `dxvk-mali-g76-*.patch` — **o
  paralelo mais próximo do nosso caso e não foi lido**; leia para **ideias**.
- GPL-3: **não copie código**; só ideias e medições.

## Verificação

- Visual: cabeças sem faixas, texto, sombras, tela de título e cenas de corte, comparando com a DXVK (`RE5_GFX=dxvk`) por captura
  no mesmo ponto. Shaders: `spirv-val` no despejo `work/shaders/`.
- `ctest` verde; novos testes para o que for lógica pura (agrupamento de passes, anel de UBO, cache).
- Desempenho no A54, 360p, `RE5_LEAN=1`: pico e média, 3 rodadas, contra DXVK e contra o vkmali. Perfil de CPU
  (`prof_run.py`, sem `FPS_LOG`) e contagem de passes/envios por quadro **antes e depois de cada princípio**.

## Entrega

APK `work/android/app-vk2-<n>.apk` (vkmali2 selecionável nas configurações, DXVK continua o padrão até a meta ser batida), plano
de teste curto com chaves `RE5VK2_*` para ligar/desligar cada princípio, e um relatório no `docs/STATUS.md` com a tabela
"mudança → passes/quadro, envios/quadro, fps pico/média".
