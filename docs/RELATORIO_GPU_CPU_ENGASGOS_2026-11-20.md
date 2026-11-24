# Relatório: GPU alta, engasgos e CPU baixa no backend nativo (A54, 20/11/2026)

Varredura pedida pelo usuário depois que o modo Vulkan 1.1 (`RE5VK_CORE`) resolveu as faixas nas cabeças.
Três problemas, em ordem: (1) GPU muito mais ocupada que na DXVK, com menos fps; (2) engasgos frequentes e
carregamento mais lento que a DXVK; (3) CPU sempre baixa (< 20%) em quase todos os celulares; só o
Dimensity 7030 chega a ~40%, e nas telas de carregamento algumas threads batem 97%.

Tudo abaixo foi medido nesta sessão ou lido no código. O que é hipótese está marcado.

## 1. Medidas lado a lado (mesmo aparelho e dia)

Galaxy A54 (Mali-G68 r38, Exynos 1380: 4 núcleos A78 + 4 A55). Coletas de 3 min em
`work/android/a54_core_20261120/` (nativo, modo Vulkan 1.1) e `work/android/a54_dxvk192b_20261120/`
(DXVK 1.9.2b, interrompida em 72 s).

| | Nativo (Vulkan 1.1) | DXVK 1.9.2b |
|---|---|---|
| Gameplay (mais de 3.000 draws/s) | 12,5 fps (120 s) | 16,2 fps (46 s) |
| GPU ocupada / clock | ~80% a **350–650 MHz** | ~75% a **221 MHz** (o mínimo) |
| Memória de GPU (`GL mtrack`) | ~1,0 GB (ETC2) | ~2,1 GB (DXT descomprimido em RGBA8) |
| CPU do processo | 131% de um núcleo (16% dos 8) | 128% de um núcleo |
| Thread de render do jogo (`g00457C60`) | 25% | 10% (+ `dxvk-cs` 17%) |
| Workers do jogo (`g004278D0`, 7) | 3–5% cada | 3–5% cada |
| Núcleos usados | só os 4 grandes | só os 4 grandes |

A DXVK move o dobro de bytes de textura e mesmo assim gasta 2 a 3 vezes menos ciclos de GPU. O excesso do
nativo não é textura.

## 2. GPU alta no nativo

### 2.1 Emulação de float e guarda de NaN (causa principal, medida)

`vk_dxso.c`: `RE5VK_FLOATEMU` vem ligada em toda plataforma e `RE5VK_NANGUARD` no Android. Com ela, cada
`mul`/`mad` vira `FMul` + 2 comparações + `LogicalOr` + `Select` por componente (`fmul4`), e cada
`dp3`/`dp4` vira multiplicações emuladas + somas no lugar de um `OpDot`. Contagem nos 164 shaders reais
de `work/shaders` (`RE5_DXSO_STATS=1 test_vk_dxso work/shaders`, novo nesta sessão):

| Configuração | ALU total | `OpSelect` | comparações | VS da cabeça (`vs_7ee27055`) |
|---|---|---|---|---|
| FLOATEMU=1 NANGUARD=1 (padrão do Android) | 82.483 | 13.942 | 22.420 | 511 |
| FLOATEMU=1 NANGUARD=0 | 79.768 | 12.636 | 22.420 | 499 |
| FLOATEMU=0 NANGUARD=0 | **29.065** | 1.987 | 1.127 | **191** |

A DXVK 1.9.2b roda **sem** emulação de float: `FloatEmulationByDefault = false`
(`build/dxvk-android/src/src/d3d9/d3d9_config.h`), sem perfil de app para o RE5 e com o executável do
Android chamado `app_process64`. Mesmo assim desenha certo no A54. O `STATUS.md` de 06/11 diz "a DXVK, com
`d3d9.floatEmulation`, fica normal", mas a opção não está ligada nela. As manchas escuras que motivaram a
emulação em 06/11 devem ter tido outra causa (hipótese: o stride dinâmico, achado hoje, ou o NaN de
`rcp`/`rsq`, que a guarda de ±FLT_MAX resolve sozinha).

**Experimento sem recompilar:** `RE5VK_FLOATEMU=0` e `RE5VK_NANGUARD=0` no `re5_env.txt`. Esperado: GPU
bem mais baixa e mais fps; conferir se as manchas de 06/11 voltam.

### 2.2 Swapchain no tamanho da tela com blit (secundária)

Log: `swapchain 1080x2340, formato 37, 5 imagens, MAILBOX (apresentação: blit)`. O nativo cria a swapchain
na resolução da tela e faz um blit de 800×600 para 2340×1080 a cada quadro. A DXVK cria no tamanho do back
buffer e deixa o compositor do Android escalar, sem custo de GPU. Correção: swapchain no tamanho do back
buffer (`ANativeWindow_setBuffersGeometry`) e apresentar direto.

### 2.3 Já descartados nesta sessão

- Layouts: no A54 já são os ótimos (`layouts: ótimos por sub-recurso (AFBC/UBWC)`), com AFBC preservado.
- Número de passes: ~100 por quadro, mas são do próprio jogo (cadeias de mip e de bloom). A DXVK tem os
  mesmos.
- Textura: o nativo usa ETC2 (menos banda que o RGBA8 da DXVK).

## 3. Engasgos e carregamento lento

### 3.1 Upload de textura com o mutex do device

`vk_res.c`, `tex_unlock` → `upload`: tudo sob `re5vk_lock(d)`. Isso inclui o hash FNV-1a de 64 bits
byte a byte sobre a textura inteira (`re5_tex_calc_hash`), o `fopen`/`fread` do cache em disco
(`re5_tex_cache.h`: um arquivo por nível de mip, ~24 mil arquivos, ~740 MB) ou a recodificação DXT → ETC2
na CPU. Enquanto uma thread de carregamento (`g00449730`) faz isso, a thread de render espera o mesmo mutex
(`mutex_ms` chegou a 35 ms por segundo). A DXVK 1.9.2b só descomprime DXT, o que é muito mais barato.

Correção: hash e cache **fora** do lock (só a cópia para o staging e o comando precisam dele); hash mais
rápido (XXH3 ou FNV de 64 bits por palavra, não por byte); cache num arquivo único com índice, em vez de
24 mil arquivos.

### 3.2 Todo upload fecha o passe de render (bug conhecido)

`upload()`: `int sep = ... && !im->rt_views && ...`. Mas `rt_views` é alocado em toda imagem
(`re5vk_image_create`), então o caminho de command buffer separado nunca roda. Já anotado no relatório de
19/11; medido hoje: **12.772 uploads em 3 min, 0 pelo caminho separado**. Cada um fecha o passe da cena
(num tiler, grava e recarrega a tela inteira). Correção: testar o uso de alvo de render (flag da criação),
não `rt_views`.

### 3.3 Pipeline compilado na hora do draw

`prepare` → `pipeline_create` → `vkCreateGraphicsPipelines` na thread de render, no primeiro draw de cada
combinação. O modo Vulkan 1.1 pôs cull, profundidade, stencil e bias na chave, então há mais pipelines
(195 em 3 min nesta rodada, com o cache apagado). O `re5vk.pcache` evita a recompilação no driver na
próxima abertura, mas a chamada continua síncrona. A DXVK 1.9.2b tem um *state cache* que pré-compila os
pipelines conhecidos com 5 threads na abertura (`Using 5 compiler threads`).

Correção: gravar as chaves dos pipelines (com os hashes de VS/PS/declaração) num arquivo e pré-compilar em
threads auxiliares durante o carregamento seguinte.

## 4. CPU baixa

### 4.1 Um mutex global para toda a sincronização do jogo

`kernel32_sync.c`: eventos, mutexes e semáforos passam todos pelo mesmo `g_mu`. `SetEvent`, `ResetEvent`
e `WaitFor*` das 10 ou mais threads disputam esse lock. Medido (simpleperf, 20/11): os 7 workers passam
**42%** das amostras em `syscall`, vindas de `k_SetEvent`, `pthread_mutex_unlock` (acordar quem está na
fila do `g_mu`), `wait_objects` e `k_ResetEvent`. O `re5-main` passa 28% em `syscall` pelo mesmo caminho.
No PC, os workers dormem e acordam ~8 mil vezes por segundo, com média de 0,9 ms.

### 4.2 Espera sem giro

`wait_objects` vai direto para o condvar; não há giro curto antes de dormir (as seções críticas têm,
`CS_SPIN_DEFAULT`). No celular, cada acordar custa a latência de tirar o núcleo do estado ocioso e de o
escalonador colocar a thread. Com milhares de entregas por segundo entre a thread principal e os workers,
a latência soma e a CPU fica ociosa esperando.

### 4.3 Todos os threads presos nos 4 núcleos grandes

`perf_hint.c`, `setup`: prende a thread principal e a de render na máscara dos núcleos grandes
(`pin_big`). Os workers são criados depois pela thread principal e **herdam** a afinidade. Medido: os 7
workers, a principal, a de render e as do driver do Mali rodam 100% nos núcleos grandes, e os 4 pequenos
ficam ociosos. Perfetto (20/11): os workers ficam 5 a 7% do tempo prontos sem núcleo.

### 4.4 Por que CPU e GPU ficam ambas baixas (hipótese)

Na DXVK, CPU e GPU baixas com fps baixo indica um quadro limitado por latência, não por trabalho. A CPU
monta o quadro e espera, a GPU executa e espera, e cada lado fica ocioso o bastante para o governador
manter o clock baixo (a GPU da DXVK ficou em 221 MHz, o mínimo). Os itens 4.1 a 4.3 alongam a parte da
CPU. Explicaria o Dimensity 7030 (governador e escalonador da MediaTek mais agressivos, ou topologia
2+6). **Não medido** num Dimensity.

## 4b. Comparação com o Dimensity 7030 (mesmo dia, mesma DXVK 1.9.2b)

Motorola Edge 40 Neo (MT6879: 6 núcleos A55 a 2,0 GHz + 2 A78 a 2,5 GHz, GPU Mali-G610 MC3 Valhall),
app de 11/11 com a DXVK 1.9.2b, 1280×720. Coleta em `work/android/d7030_dxvk192b_20261120/`. Os contadores
de GPU do sysfs são negados sem root nesse aparelho, e o trace não tem eventos de clock da GPU.

Fps por faixa de draws por quadro (o filtro por draws/s misturava menus; usuário: ~35 fps em jogo no 7030):

| Draws por quadro | 7030, DXVK 1.9.2b | A54, DXVK 1.9.2b | A54, nativo (Vulkan 1.1) |
|---|---|---|---|
| 400–800 | 36,2 | 16,1 | 14,2 |
| 800+ | 31,1 | 13,1 | 10,6 |

O 7030 processa cerca de 2,3 vezes mais draws por segundo, com mais pixels (1280×720 contra 800×600).

- **7030: limitado pela thread principal.** Perfetto: `re5-main` roda 66,8% e fica 15,9% pronta sem núcleo
  (só dois A78, divididos com a thread de render, as duas presas lá). Workers ~90% nos núcleos pequenos,
  acordados pela principal 21.840 vezes em 20 s. A CPU "normal" (~40%) é isso: o jogo está limitado pela CPU.
- **A54: a CPU espera a GPU.** Na DXVK a principal fica 37% do tempo bloqueada no `Present` (`present_ms`
  367/s contra 93/s no 7030). simpleperf: o driver do Mali custa 16,6% das amostras no A54
  (`libGLES_mali`), mais a thread `mali-cmar-backend` (13%: `ioctl`, `read`, `write`, `ppoll`). No 7030,
  5,9%. Hipótese coerente com o hardware: a G68 (Bifrost, job manager) monta e envia cada job pela CPU; a
  G610 (Valhall, CSF) entrega a fila de comandos ao firmware da GPU.
- **Conclusão:** a CPU baixa do A54 é consequência do limite do lado da GPU (GPU, driver e o clock que não
  sobe), não um defeito de CPU próprio. No A54 a prioridade é cortar trabalho de GPU e de driver por draw;
  no 7030, aliviar a thread principal (não dividir os dois A78 entre a principal e a de render).

## 5. Ordem de ataque sugerida

1. **`RE5VK_FLOATEMU=0 RE5VK_NANGUARD=0`** no A54 (sem recompilar): medir GPU e fps e conferir manchas.
   Se o visual ficar certo, trocar o padrão.
2. **Upload fora do lock + caminho separado consertado** (3.1 e 3.2): engasgos e carregamento.
3. **Sincronização** (4.1 e 4.2): locks por objeto (ou eventos com estado atômico e `futex` direto) e giro
   curto antes de dormir; medir com `RE5_FPS_LOG` e simpleperf.
4. **Afinidade** (4.3): prender só a principal e a de render; A/B rápido com `RE5_PERF_HINT=0`.
5. **Pré-compilação de pipelines** (3.3) e **swapchain no tamanho do back buffer** (2.2).

### Feito no mesmo dia (não testado em celular; detalhes no `STATUS.md`)

Itens 1, 2, 4 e 5 (swapchain); do item 3, o giro antes de dormir (os locks por objeto ficaram para depois);
cache de pipelines gravado numa thread; DXVK padrão do Android voltou à 1.9.2b. Ficou de fora a
pré-compilação de pipelines (3.3).

## 5b. Medição por passe no A54 (depois das otimizações, mesmo dia)

`RE5VK_PASS_TIMING=1` (novo: timestamp antes de cada `vkCmdBeginRenderPass` e depois de cada
`vkCmdEndRenderPass`). Coleta em `work/android/a54_passtiming_20261120/`, 137 s de gameplay a 11,7 fps,
jogo em **1280×720** (as medidas antigas do A54 eram 800×600).

- GPU por segundo: **878 ms dentro dos passes**, 5 ms entre passes, 10 ms nas bordas do envio. Não há
  bolhas entre passes; a suspeita das barreiras também caiu na leitura do código (as transições para amostrar
  já usam só o estágio que lê).
- Por quadro (~85 ms): cena 1280×720 **49,2 ms** (19,5 passes, 1.152 draws); sombras 1024×1024 3,5 ms;
  passes pequenos (cadeias de mip e de bloom, 1×1 a 640×360) ~12 ms. Custo fixo de ~0,4–0,5 ms por passe,
  mesmo de 1×1: cada passe da cadeia depende do anterior.
- **GPU no sysfs: 64% de uso a 303 MHz de média (máx. 949)**, com o app em modo de jogo `performance` e o
  governador `Interactive` da Samsung. O medidor do governador vê menos uso que os timestamps, e o clock não
  sobe. Sem root não há como fixá-lo.
- Afinidade nova confirmada: só a principal e a de render presas em 4–7; as outras 56 threads com 0–7.

Alavancas que restam, por tamanho do ganho esperado: resolução interna do jogo (1280×720 → 960×540 corta
~44% dos pixels da cena); o caminho de apresentação assíncrona (a thread fica 458 ms/s bloqueada no
`vkQueuePresentKHR`; ganho limitado, a GPU já está 89% coberta); e `DONT_CARE` nos passes que sobrescrevem
o alvo inteiro (o oráculo TBDR, desligado por glitches em 11/11).

## 6. Arquivos de evidência

- `work/android/a54_core_20261120/` e `a54_dxvk192b_20261120/`: `logcat.txt`, `amostras.txt`,
  `meminfo_*.txt`, perfis do simpleperf e (no primeiro) o trace do Perfetto.
- Contagem de ALU: `RE5_DXSO_STATS=1 RE5VK_FLOATEMU=<0|1> RE5VK_NANGUARD=<0|1>
  build/host-tests/tests/test_vk_dxso.exe work/shaders`.
