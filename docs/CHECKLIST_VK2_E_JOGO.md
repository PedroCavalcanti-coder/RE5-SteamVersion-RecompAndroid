# Checklist: backend vk2 (Vulkan 1.1 nativo) e otimizações do jogo

Atualizado em 24/11/2026. Marcado = feito. "Ativo" = ligado por padrão no APK atual (`work/android/app-vk2-3.apk`).
Números de fps são do Galaxy A54 (Mali-G68), 360p, `RE5_LEAN=1`, salvo indicação. Medição pelo usuário = pico/média lidos no jogo;
`bench_android.py` = média/mediana/p10 de 100 s pelo roteiro de teclas.

## 1. Feito

### Backend vk2

| Feito | Item | Ganho medido | Ativo? | Chave |
|---|---|---|---|---|
| [x] | Backend novo `RE5_GFX=vk2` (`runtime/src/gfx/vk2/`): contexto Vulkan 1.1 em C puro (sem VMA nem vk-bootstrap), módulos provados recuperados da tag | vk2-1: média 29,3 / mediana 22,9 / p10 15,7 (DXVK do `app-padrao`: 33,8 / 23,6 / 15,8) | Sim, quando escolhido (DXVK continua o padrão) | `RE5_GFX=vk2` ou Configurações > Renderizador |
| [x] | **Blit de apresentação em lote separado**: só o blit espera a imagem da swapchain; o quadro não espera mais o compositor | **Junto com o item abaixo: de 31 / 22,6 para 45,5 / 36** (pico/média, usuário). +47% pico, +59% média | Sim | `RE5VK2_SPLIT_PRESENT=0` desliga |
| [x] | **Passe sem dependência fragmento→vértice** (bolha de pipeline da Mali, guia da Arm) | Ver linha acima (as duas entraram juntas; o ganho de cada uma não foi separado) | Sim | `RE5VK2_RP_VS_DEP=1` volta a dependência |
| [x] | Anéis por envio de 320 MB para 96 MB (4 slots × (8 MB UBO + 16 MB staging)) | Memória: −224 MB visíveis pela CPU (pela conta; não medido no aparelho) | Sim | `RE5VK2_UBO_MB`, `RE5VK2_STAGE_MB` |
| [x] | Blocos de memória de GPU inteiramente livres voltam ao sistema | Não medido | Sim | — |
| [x] | **Pré-compilação de pipelines** (manifesto `vk2.shaders` + threads no carregamento) | PC: 131 pipelines aprendidos e recriados antes do jogo. A54: 128 prontos antes do jogo pedir. Fps/engasgo não medidos | Sim (segundo plano) | `RE5VK2_PRECOMPILE=1\|wait\|0`, `RE5VK2_PRECOMPILE_THREADS` |
| [x] | Pré-compilação com prioridade baixa e 2 threads (não disputar CPU com o carregamento) | **Não medido** (carregamento vk2-1: 31,6 s até a 1ª cena 3D; DXVK: 24,0 s) | Sim, no `app-vk2-3` | `RE5VK2_PRECOMPILE_THREADS` |
| [x] | Botão "Apagar caches de shaders e texturas" e escolha de renderizador/pré-compilação no app | — | Sim | `RE5VK2_CACHE_CLEAR=1` |
| [x] | Conferência da ligação VS→PS (avisa se o PS lê o que o VS não escreve) | PC: 0 avisos em 131 pipelines | Sim (só avisa) | — |
| [x] | Linha `vk2 por quadro` no log (passes, envios, draws/passe, pipelines, memória) e análise de passes juntáveis | — | Só com `RE5_FPS_LOG=1` | `RE5_FPS_LOG=1` |
| [x] | Buffers visíveis pela CPU em memória de **sistema** antes da `DEVICE_LOCAL` (PC com placa discreta: a janela BAR da VRAM deixava o `Unlock` de 5 MB lento) | **PC: `VertexBuffer9::Unlock` 869 → ~92 ms/s (9x), `IndexBuffer9::Unlock` 86 → 9 ms/s.** No celular (memória unificada) não muda nada | Sim | — |
| [x] | Envio a cada n passes (`RE5VK2_SUBMIT_EVERY`) | **Piorou**: mediana 16,4, p10 12,2 (n=8). A 1ª versão travou o driver | **Não** (desligado) | `RE5VK2_SUBMIT_EVERY=<n>` |

### Diagnósticos que já deram resposta

- [x] A GPU não esperava a CPU: o envio único esperava a imagem da swapchain (corrigido acima). `gpu_ms` ≈ 1000 ms/s com 57% no sysfs era a espera contada no timestamp.
- [x] Custo de GPU com personagens: cena 640x360 com até 25 mil draws/s (450–580 ms/s) e sombra 1024x1024 com 1.300–4.300 draws/s (85–130 ms/s).
- [x] A base de 35 fps limpa (`b98db313`) + vk2 deu **menos** fps que a árvore de trabalho: as edições de CPU sem commit de outras sessões ajudam.
- [x] Rampa de gama: o jogo manda uma não identidade, mas **a DXVK também não aplica** em janela (`d3d9_swapchain.cpp`: só com `!Windowed`). Não explica a iluminação estourada no vk2.

## 2. A testar agora (APK `app-vk2-3.apk`)

- [ ] Fps sem log (pico/média) contra os 45,5 / 36 do `app-vk2-2`.
- [ ] Tempo de carregamento do "Iniciar" até a 1ª cena (2ª partida), contra 31,6 s.
- [ ] Com `RE5_FPS_LOG=1`: os números "passes que reabrem alvo" e "juntáveis sem leitura cruzada" por quadro (decidem o item 3.1).
- [ ] Separar o ganho de `RE5VK2_SPLIT_PRESENT` e `RE5VK2_RP_VS_DEP` (uma rodada com cada um desligado).
- [ ] Capturas no mesmo ponto com `RE5_GFX=dxvk` e `vk2` nos trechos onde a luz estoura.

## 3. Próximas otimizações do vk2 (por retorno esperado)

Perfil por método do D3D9 no PC (`RE5_API_PROF=1`, build Debug: valores absolutos só valem para comparar métodos): o `Unlock` de buffers de
vértice é de longe o maior (cópia da sombra do guest para o buffer do host). Resta ~92 ms/s no PC e ~38 ms/s no A54 (49 por s, ~5 MB cada).
Para zerar: o host dar memória do próprio guest (`VK_EXT_external_memory_host`, o antigo `RE5VK_ZEROCOPY` do vkmali, que não veio para o vk2);
**Conferido no A54 (`adb shell cmd gpu vkjson`, 24/11/2026): a Mali-G68 NÃO expõe `VK_EXT_external_memory_host`.** Só `VK_KHR_external_memory`,
`_fd`, `VK_EXT_external_memory_dma_buf` e `VK_ANDROID_external_memory_android_hardware_buffer`. O zero-copy por ponteiro do host não é possível nesse aparelho
(o que explica o `RE5VK_ZEROCOPY` do vkmali nunca ter virado padrão). Alternativa mais arriscada: exportar a memória como fd (dma-buf) e fazer `mmap(MAP_FIXED)`
dentro do espaço de endereços do guest; exige reservar a faixa no gerenciador de memória do guest e cuidar da coerência de cache (`DMA_BUF_IOCTL_SYNC`). Não feito.


1. [x] **Fila de passes com reordenação (estilo PPSSPP): implementada e DESCARTADA pelos números.** Código pronto e desligado
   (`RE5VK2_PASS_QUEUE=1`: passes em command buffers secundários, juntando passes ao mesmo alvo quando independentes). Medição no PC
   (benchmark, `RE5_FPS_LOG=1`): **15,7 passes/quadro reabrem um alvo já usado, e 0,0/quadro seriam juntáveis sem leitura cruzada** (o passe
   entre as duas visitas sempre lê ou escreve o alvo). Sem passe para juntar, a fila não reduz nada. Não vale testar no A54.
   Texto original da ideia: **Fila de passes com reordenação (estilo PPSSPP).** ~65% dos fechamentos de passe são troca de alvo, e 16% reabrem um alvo já usado
   (A, B, A). Juntar as visitas economiza ~0,45 ms fixos por passe e o load/store do alvo na Mali. Exige refazer o controle de layouts
   (as barreiras hoje são decididas na gravação, na ordem original). **Só vale se a análise do item 2 mostrar muitos juntáveis.**
2. [ ] **Load/store do passe por conhecimento do futuro**: com a fila, trocar LOAD por CLEAR/DONT_CARE e STORE por DONT_CARE quando o conteúdo
   não é lido depois (o oráculo TBDR existe, mas sem a fila ele errava).
3. [ ] **Skinning mais barato** (o fps cai com muitos personagens): copiar para o UBO só o trecho de constantes que o shader de ossos lê,
   em vez dos 4 KB do banco a cada draw; medir o custo por draw com `RE5VK2_PASS_TIMING`.
4. [ ] **Sombra só de profundidade**: nos passes de sombra (1024x1024), variante do pipeline sem pixel shader quando ele não descarta
   pixel; opção de resolução da sombra menor.
5. [ ] **CPU da camada vk2**: `d3d9_ms` 304 contra 140 da DXVK e `lock_ms` 138 contra 35. Conjuntos de descritores persistentes entre
   quadros, UBO único por quadro, menos trabalho no `prepare` por draw.
6. [ ] **Carregamento**: os 900 ms de `lock_ms` no 1º segundo (criação de recursos sob o mutex do device); cache de texturas ETC2 num
   arquivo só, em vez de um por nível de mip; recodificação fora do mutex.
7. [ ] **Present**: cópia em vez de blit quando o back buffer e a swapchain têm o mesmo tamanho.
8. [ ] **Pipeline que falta no manifesto**: hoje compila na thread do jogo (engasgo na 1ª vez). Avaliar compilação em segundo plano com
   o draw esperando só esse pipeline (no Simpsons a versão que pulava o draw causou pausas).
9. [ ] Quadros em voo: testar `RE5VK2_LATENCY=3` com `RE5_QUERY_LAG=4` agora que a GPU não espera o compositor.

## 4. Jogo

### 4.1 Multiplayer LAN (não funciona e pode custar fps)

- [x] Lido no código: `steam_lan_init` sobe **sempre** na inicialização da Steam (`steam_api.c`), mesmo no single player. A thread
  `lan_worker` acorda **a cada 5 ms** (200 vezes por segundo), pega o mutex da LAN, envia beacon a cada 1 s e relê as interfaces de
  rede a cada 5 s. No A54 os 4 núcleos grandes estão ocupados; 200 acordadas/s não são de graça.
- [ ] Medir: fps com a LAN desligada (precisa de uma chave nova, `RE5_LAN=0`, que não sobe a thread).
- [ ] Corrigir: subir a LAN só quando o jogo abrir o menu de rede; trocar o laço de 5 ms por espera em `select()` com prazo.
- [ ] Investigar por que não conecta: log de `steam_lan` nos dois aparelhos (porta de descoberta, broadcast bloqueado no Wi-Fi, firewall do
  Android). Não diagnosticado nesta sessão.

### 4.2 Carregamento de shader

- [x] Manifesto + pré-compilação (item 1).
- [ ] Gravar o SPIR-V traduzido junto do manifesto (pula a tradução DXSO na 2ª vez).
- [ ] A 1ª execução continua só aprendendo; não há como pré-compilar shaders que o jogo ainda não pediu sem distribuir o bytecode do
  jogo (não pode ir no APK).

### 4.3 Iluminação estourada no vk2 e luz/sombra melhores

- [x] Descartado: a rampa de gama (a DXVK também não aplica em janela).
- [ ] Achar a causa: capturas `RE5_D3D9_SHOT` no mesmo quadro com `dxvk` e `vk2` no PC e no A54, e comparar o histograma de brilho por
  passe (`RE5VK2_PASS_TRACE`). Suspeitos: formato dos alvos de HDR (R16F × UNORM), saturação no blend, `sRGB`, NaN/Inf na cadeia de
  exposição (a emulação de float está desligada, como na DXVK 1.9.2b), filtro linear em formato que a Mali não filtra.
- [ ] Melhorar a luz (depois de corrigir): troca de shader por hash (uma pasta de shaders substitutos que o vk2 carrega no lugar do
  original). Com isso: sombra com filtro PCF (amostrador de comparação), mapa de sombra de resolução maior, ajuste do bloom e da exposição.
  **Luz e sombra dinâmica nova** (fontes de luz que o jogo não tem) exige passes extras e mexer no render do jogo: grande, e custa fps
  no A54. Proposta: primeiro a correção, depois sombra PCF com chave.

### 4.4 Opções do launcher dentro do jogo

- O que já existe no menu do próprio jogo (PC): resolução, qualidade e opções de vídeo (`config.ini`). Essas podem sair do launcher.
- O que não é do jogo (renderizador, limite de fps, pré-compilação, controles na tela, LAN, idioma do app) precisa de um menu próprio
  desenhado pelo runtime dentro do jogo. A outra sessão está fazendo o toque nativo com desenho no `Present` (`touch_ui_draw.c`): é a
  base para um menu de configurações dentro do jogo, aberto por um botão ou gesto.
- [ ] Lista final do que fica no launcher (só o que precisa valer antes de o jogo abrir: renderizador, driver Vulkan, extração).

### 4.5 Mods direto no port (com créditos)

Regra do port: só mods **de dados** (arquivos no `nativePC_MT`, `.arc`, texturas, textos) funcionam como estão. Mods que injetam código
(DLL, `.asi`, patch do `.exe`) teriam de ser refeitos como override nativo da função do jogo (ver `docs/prompts/04_mods_ptbr_e_ia_sheva.md`).

| Mod | O que achei (não verificado arquivo por arquivo) | Situação |
|---|---|---|
| Tradução PT-BR (texto e menus) | Não achei tradução de texto/menus do RE5 PC. Achei uma **dublagem PT-BR feita com IA**, distribuída no Nexus Mods, que segundo o guia substitui arquivos da pasta do jogo ([guia Critical Hits](https://criticalhits.com.br/dicas/resident-evil-5-como-instalar-dublagem-brasileira-pt-br/)) | [ ] Achar autor, link do Nexus e termos; conferir se é só dados |
| IA da Sheva / do parceiro | Não achei mod só de IA da Sheva. Achei mods de correção de personagens e de troca do parceiro no Nexus do RE5 Gold ([lista do Nexus](https://www.nexusmods.com/residentevil5goldedition/mods/76), [outro](https://nexusmods.com/residentevil5goldedition/mods/438)) | [ ] Ler cada página: autor, o que muda, se é só dados |
| Movimentação (andar atirando, 360°) | Uma notícia descreve um mod de movimento livre em 360° para o RE5 PC, sem nome nem link ([GameGPU](https://en.gamegpu.com/news/igry/novyj-mod-dlya-resident-evil-5-dobavlyaet-svobodnoe-peremeshchenie-v-360)). Quase certamente muda código | [ ] Achar a origem; avaliar como override nativo |

Créditos: cada mod incluído terá autor, link e licença no `docs/` e na tela "Sobre" do app, e só entra com o aval do autor quando os termos
pedirem. Os arquivos de mod não vão para o git nem para o APK sem permissão.
