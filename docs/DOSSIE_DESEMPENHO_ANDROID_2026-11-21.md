# Dossiê: desempenho do RE5 recompilado no Android — só o Dimensity 7030 roda "normal"

Documento para outra IA analisar. Reúne **tudo** o que foi medido e tentado para o problema de
desempenho no Android, de 30/10 a 21/11/2026: o que está no `docs/STATUS.md` (inclusive as rodadas
feitas por outras IAs, marcadas lá como "outra IA" ou "revisão externa"), os relatórios em `docs/`,
e a sessão de 20–21/11. Onde algo é hipótese, está dito. Os trechos de código são do working tree
atual (nada de 20–21/11 foi commitado).

---

## 1. O projeto em uma página

- **O que é:** recompilação estática do `re5dx9.exe` (Resident Evil 5, Steam, x86-32, Windows) para
  C, rodando em Linux x64, Windows x64 e Android ARM64. O lifter (`tools/pcrecomp`) traduz ~72 mil
  funções x86 para C; o runtime (`runtime/`) emula o Windows por HLE (kernel32, user32, d3d9, dsound,
  xinput…) e a CPU x86 por um `CPU` em C (`cpu.h`, gerado por `scripts/gen_cpu_header.py`).
- **Gráficos:** o jogo chama Direct3D 9. A ponte (`runtime/src/win32/d3d9.c`) repassa para um D3D9
  do host:
  - **DXVK** (D3D9 → Vulkan): no Android, a **DXVK native 1.9.2b** com o porte do NFSU2
    (`libdxvk_d3d9_192b.so`; padrão no Android desde 20/11). Há também uma **2.6.2 portada**
    (`android/dxvk_262`, `libdxvk_d3d9.so`), ainda com texto quebrado e tela preta no Mali.
  - **Backend Vulkan próprio** (`runtime/src/gfx/vk/`, "re5vk", selecionado por `RE5_GFX=vkmali` ou
    `vkadreno`): device D3D9 escrito do zero, com tradutor SM3 → SPIR-V próprio (`vk_dxso.c`).
- **Threads:** cada thread do guest é uma thread do host (D5). Há um "lock do guest": modo `global`
  (só uma thread roda código do guest por vez) ou `hle` (código do guest em paralelo, lock só no HLE;
  é o padrão do app, `parallel_mode`).
- **Threads do jogo que importam** (nomes como aparecem nos perfis):
  - `re5-main`: thread principal (é ela que chama `IDirect3DDevice9::Present`);
  - `g00457C60`: thread de render do jogo (a que faz as chamadas D3D9 de desenho);
  - `g004278D0` (×7 ou 8): workers do job system da MT Framework;
  - `g00449730` (×3): carregamento/streaming de recursos;
  - `g006FAEE0`: wrapper de thread da MT Framework (a do som pesa);
  - na DXVK: `dxvk-cs`, `dxvk-submit`, `dxvk-queue`; no driver Mali: `mali-cmar-backend`,
    `mali-compiler`, `mali-event-hand`.

## 2. O problema

No PC, com DXVK ou com o backend próprio, em qualquer CPU, o jogo roda normal (90+ fps no
benchmark). No Android:

- **Só o Dimensity 7030** (Motorola Edge 40 Neo) roda "normal": CPU do processo ~40% (soma dos
  núcleos), GPU ~99%, ~35 fps em jogo.
- **Todos os outros testados** (Galaxy A54 / Exynos 1380 / Mali-G68; outro celular; Snapdragon 8 Gen 4
  / Adreno; Z Flip5 / Snapdragon 8 Gen 2 / Adreno 740) ficam com **CPU ~15% e GPU baixa ao mesmo
  tempo**, com fps baixo (A54: 11–16 fps em jogo). Isso acontece **com a DXVK e com o backend
  próprio**.
- **Histórico dito pelo usuário:** o problema existe desde o começo do porte para Android. Na
  primeira versão (código do PC ainda mal otimizado) a CPU ficava em ~50% e o jogo em ~5 fps (era
  limitado pela CPU). Conforme o código ficou rápido, apareceu o limite atual.
- **Teste do usuário em 20/11 (A54):** tudo no mínimo e resolução 640×360 → só +3 fps (11 → 14);
  GPU caiu de ~80% (clock baixo) para ~40%; CPU de ~15% para ~17%.
- **Teste do usuário em 20/11 (A54):** `RE5_PERF_HINT=0` (sem nenhuma afinidade nem ADPF) → mesmo
  fps, CPU caiu para ~11%.

## 3. Aparelhos

| Aparelho | SoC | CPU | GPU / driver | Observação |
|---|---|---|---|---|
| Motorola Edge 40 Neo | Dimensity 7030 (MT6879) | 6× A55 2,0 GHz + 2× A78 2,5 GHz | Mali-G610 MC3 (Valhall, CSF) | **roda normal** |
| Galaxy A54 (SM-A546E) | Exynos 1380 | 4× A55 2,0 GHz + 4× A78 2,4 GHz | Mali-G68 (Valhall com Job Manager, `mali_kbase` JM), r38p1, Vulkan 1.3.219 | aparelho principal de teste |
| Z Flip5 | Snapdragon 8 Gen 2 | — | Adreno 740 | lento (dito pelo usuário) |
| ? | Snapdragon 8 Gen 4 | 8 núcleos Oryon (todos "grandes") | Adreno | lento (dito pelo usuário; sem coleta aqui) |
| "outro celular" | ? | ? | ? | lento (dito pelo usuário) |

A54, sysfs da GPU: governador `Interactive` (Samsung), clock 221–949 MHz; o app já está em modo de
jogo `performance` (`cmd game list-modes`). No 7030 os contadores de GPU do sysfs são negados sem
root.

## 4. Os fatos medidos mais importantes (leia isto primeiro)

### 4.1 O trabalho de CPU por quadro é igual nos dois aparelhos

Perfetto (20/11), thread principal `re5-main`:

| | Rodando | Pronta sem núcleo | Dormindo | fps | **CPU por quadro** |
|---|---|---|---|---|---|
| 7030 (DXVK 1.9.2b) | 66,8% | 15,9% | 17,3% | ~35 | **~19 ms** |
| A54 (backend próprio) | 22% | — | ~73% | 11,7 | **~19 ms** |

O código ARM gerado executa o quadro com o mesmo custo. No A54 a thread principal passa ~66 ms de
cada quadro **parada**.

### 4.2 Onde ela fica parada: dentro do `Present`

Estatística do runtime por função importada (A54, 20/11, backend próprio, `RE5_FPS_LOG=1`):

```
thunk d3d9 ms/s: IDirect3DDevice9::Present=459.74 IDirect3DDevice9::DrawIndexedPrimitive=66.50
                 IDirect3DDevice9::DrawPrimitive=26.63 IDirect3DVertexBuffer9::Unlock=17.20 ...
```

| Mesma DXVK 1.9.2b | Thread do jogo dentro do `Present` |
|---|---|
| A54 | **430–560 ms por segundo** |
| 7030 | **~5–20 ms por segundo** |

Patch de medida dentro da DXVK (11/11, `android/dxvk/re5_present.py`): no A54, o
**`vkQueuePresentKHR` leva o quadro inteiro (~51–75 ms por chamada; 965–990 ms por segundo)**, na
thread `dxvk-submit`; a principal espera o present anterior 540–640 ms/s; acquire e latência ~0. No
7030, a mesma linha (20/11):

```
re5 present (ms por s, chamadas por s): queue_present 15 (20) acquire_antecipado 2 (20)
  acquire_jogo 0 (0) queue_submit 23 (167) espera_present 0 (20) espera_latencia 0 (20)
```

No backend próprio do A54 (20/11): `queue_present_ms` ~390–440 por segundo,
`acquire_ms` ~1, `fence_ms` ~0–40, `em_voo_ms` ~1.

### 4.3 A GPU do A54 trabalha a ~1/3 do clock

- DXVK no A54: GPU ~72–75% ocupada a **221–246 MHz** (máx. 949; o teto visto foi 845 em 11/11).
- Backend próprio no A54 (20/11, depois de tirar a emulação de float): sysfs 64% a 303 MHz de média.
  Pelos timestamps do Vulkan, a GPU passa 878 ms de cada segundo **dentro** de render passes, 5 ms
  entre passes. O governador vê menos uso do que os timestamps e não sobe o clock.
- Em 11/11, com o backend próprio ainda pesado (emulação ligada), a GPU ficou 96–98% a 949 MHz, e o
  fps era parecido com o da DXVK em 221 MHz.

### 4.4 O driver Mali do A54 custa muito mais CPU

simpleperf (20/11), mesma DXVK 1.9.2b:

| | `libGLES_mali.so` | thread `mali-cmar-backend` |
|---|---|---|
| A54 (Valhall JM) | 16,6% das amostras | 13% (`__ioctl`, `read`, `write`, `__ppoll`, `cmarp_release_atom_id`) |
| 7030 (Valhall CSF) | 5,9% | — (`mali-event-hand` 3,4%) |

### 4.5 Comparação justa de fps (por draws por quadro)

| Draws por quadro | 7030, DXVK 1.9.2b (APK de 11/11) | A54, DXVK 1.9.2b | A54, backend próprio (Vulkan 1.1) |
|---|---|---|---|
| 400–800 | 36,2 | 16,1 | 14,2 |
| 800+ | 31,1 | 13,1 | 10,6 |

O 7030 em 1280×720; o A54 em 800×600. Os dois processam ~11,5 mil draws/s com o filtro antigo por
draws/s (coincidência ou não).

### 4.6 Afinidade e núcleos (descarta "o 7030 usa os núcleos de desempenho e os outros não")

- A54: principal e render presas em 4–7 (A78); os 7 workers também rodaram 93–97% do tempo nos A78
  (antes de 20/11 por herança da máscara; depois de 20/11 com máscara 0–7, por decisão do
  escalonador). As outras 56 threads com `Cpus_allowed_list 0-7` (conferido em `/proc/*/status`).
- 7030: principal e render nos dois A78 (`0xc0`); workers ~90% nos A55.
- `RE5_PERF_HINT=0` no A54 (sem afinidade nem ADPF): mesmo fps, CPU 11%.

## 5. Cronologia completa do que foi tentado

Cada item: o que foi feito, onde, e o resultado medido. "Sem medida" quando não houve.

### Fase A0 e decisão de backend (30/10–31/10)
- Medida no A54 (DXVK): "o quadro não é limitado pela CPU nem pela resolução. Ele se perde no
  pipeline da DXVK: só 2 quadros em voo, o `Present` com 53% do quadro e a GPU a 25–40% do clock
  máximo" (`docs/PROJETO_VULKAN_NATIVO.md`). **Decisão do usuário: escrever o backend Vulkan
  próprio** para controlar quadros em voo, apresentação e sincronização.
- DXVK 2.6.2 no A54 avaliada (03/11): não roda sem porte grande (sem `VK_EXT_robustness2`,
  `nullDescriptor`, `multiDrawIndirect`, `textureCompressionBC`, sem GPL).

### Otimizações de CPU, sobretudo no PC (03–04/11)
- Cache de chamada indireta no ponto de chamada (`RE5_DISPATCH_IC`): empate (89,4 × 90,1), desligado.
- Tabela de handles com leitura sem mutex (`handle_table.c`): empate na DXVK.
- re5vk copia só as constantes que o shader lê: re5vk 78 fps × DXVK 68,9 (benchmark fixo 1080p, PC).
- Seções críticas no modo `hle` como as do Windows (CAS + spin até o SpinCount + acordar um):
  re5vk 89 → 95–102 (variável), 78 → 87,1 (fixo) no PC.
- `obj_of` sem mutex na ponte; binds de vertex/index buffer só quando mudam; pipeline só quando
  `pipe_gen` muda.
- `Unlock` sem cópia no re5vk (`RE5VK_ZEROCOPY`, memória do guest importada): empate, desligado
  (e o A54 não tem `VK_EXT_external_memory_host`).
- `spin_pause` em 47 laços de espera: empate, desligado.

### CPU e GPU ociosas no celular (05–07/11)
- **Bug do ADPF corrigido (06/11, `perf_hint.c`):** sem limite de FPS o alvo da sessão era 30 fps
  (o sistema baixava o clock). Agora 60 fps sem limite. **Não medido no aparelho.**
- **Lock global x paralelo:** no modo `global`, `lock_ms` 1600–2400 ms/s (o guest roda um núcleo
  por vez). No modo `hle` (paralelo), `lock_ms` ~50 e a thread que apresenta passa ~47% dentro do
  `Present` da DXVK. **O app passou a usar o modo paralelo por padrão** (`parallel_mode`).
- PC com `RE5_PROFILE` e `Sleep` por endereço: nenhuma espera errada; o limitador do jogo
  (`0x00426F80`, `Sleep(resto-1)` + QPC) quase não dorme; `Sleep(1)` a 500/s em threads de fundo
  (`0x0048C5D0`, `0x00447141`); workers ~80% em `WaitForSingleObject`; esperas do jogo `INFINITE`.
- **Thread de som (07/11):** `g006FAEE0` gastava 42–82% de um A78 no A54 (4,6% no PC): laço
  `call 0x0049D830; Sleep(1)`. Causa: o `nanosleep` do Android acorda em ~1,07 ms, o `Sleep(1)` do
  Windows em ~2 ms. **Correção: `Sleep(n)` = n ms + 1 tique fora do Windows** (`RE5_SLEEP_TICK_US`,
  padrão 1000). Não medido.
- **CRT sem SSE2 (07/11, achado):** o `_get_sse2_info` do CRT usava o teste `pushfd/popfd` que o
  lift não preservava, e o jogo inteiro rodava o CRT de uma CPU sem SSE2 (`_ftol2` x87, memset/memcpy
  em laços de byte). Overrides para SSE2 = 1, memset/memcpy/memmove do host, `_ftol2` e `_ftol2_sse`
  (`overrides.c`, 1500/1500 iguais no `difftest_override`).
- **Giro do lock do HLE** (`RE5_HLE_SPIN`, padrão 2000).
- Revertido de uma rodada de outra IA (07/11): EDS desligado quando a extensão não aparecia, Present e
  espera de fence fora do mutex, `storeOp DONT_CARE` na profundidade, barreiras sem bits de escrita,
  `maxLod = minLod`, clear por render pass, D32 forçado, `dmb ish` em toda cabeça de laço, relógio por
  `cntvct_el0` (estourava o `uint64` com ~15 min de aparelho ligado). Ficou: modo paralelo padrão e
  JobThread padrão 4.

### Rodada de 10/11 de outra IA (commits `62af2332`, `40ce4f5d`, `6cbd3c53`, `49d13bf0`)
- `62af2332`: "corrige afinidade SD 8 Gen 4, spinlock ARM64 e tuning genérico" (`yield; isb` no giro,
  `-mtune=generic`).
- `40ce4f5d`: layouts ótimos UBWC/AFBC, oráculo TBDR (`STORE_OP_DONT_CARE` previsto), cache dinâmico,
  `RE5_QUERY_LAG=2` padrão no Android.
- `6cbd3c53`: drivers Vulkan customizados por AdrenoTools (Turnip) e "Modo Turbo Adreno".
- `49d13bf0`: avaliação e seleção de janela/loader para `RE5_GFX=gl`.
- O `2737cb77` (antes) tinha desfeito sem aviso a rodada de 07/11 (overrides do CRT, spin do HLE,
  Sleep + tique); restaurado em 11/11.
- **Usuário: nenhuma diferença de desempenho**; no re5vk, lixo de textura e sombras quebradas só no
  ARM. Revisão de 11/11: `QUERY_LAG` voltou a 0 (depois voltou a 2/3, ver abaixo), oráculo TBDR
  desligado por padrão, correção dos layouts de anexos amostrados, UBO depois de flush.

### Medidas de 11/11 (A54 e 7030)
- HUD enganava: % de CPU é dividido por todos os núcleos; um núcleo cheio aparece como ~12%. HUD
  passou a mostrar a thread mais ocupada (`15% CPU · máx 98% re5-main`).
- Benchmark fixo 800×600 no A54: **re5vk** 29,5 fps médio (mediana 19,4), GPU no limite (28,8 ms de
  GPU por quadro, 98% do clock); **DXVK** 17,5 médio (mediana 16,1), GPU 72% a 233 MHz (25% do
  máximo), Present 29,8 ms de um quadro de 57 ms.
- Perfetto (DXVK, A54): núcleos grandes ~50%; a principal dorme 58%; as esperas longas (~38 ms, uma
  por quadro) terminam com a `dxvk-submit`, que dorme 95% esperando o driver do Mali.
- Patch de medida na DXVK (`android/dxvk/re5_present.py`, ver §6.6): `vkQueuePresentKHR` leva o
  quadro inteiro em todos os modos.
- **Tentativas na DXVK, todas sem ganho (12–16 fps no mesmo trecho):**
  - `DXVK_RE5_PRESENT_DEPTH=2` (quadros em voo; a principal passa a esperar a latência, 550–600 ms/s;
    GPU 58% a 226 MHz);
  - **`DXVK_RE5_PRESENT_QUEUE=1` (present na 2ª fila gráfica do Mali, numa thread `dxvk-present`:
    GPU 70%, clock médio 244 MHz, máx. 552)** — ou seja, **apresentação assíncrona já foi tentada**;
  - FIFO por `d3d9.presentInterval=1`: **não aplicado** (a DXVK não leu o `DXVK_CONFIG_FILE`;
    inconclusivo).
- `RE5_QUERY_LAG=2` voltou a ser padrão no Android (o jogo girava no `GetData`).

### 12/11
- **ADPF com duração de GPU** (`APerformanceHint_reportActualWorkDuration2`, a espera no Present como
  GPU): aceito pelo sistema, **não muda nada** (GPU 246 MHz com e sem, 12–16 fps).
- re5vk `RE5VK_SRGB=0` (imagens não mutáveis, AFBC): GPU 66% a 393 MHz contra 96% a 949 MHz, **fps
  não subiu** (~13 × ~16; o governador baixou o clock junto).
- Correções só do Android a partir de revisão externa: guarda de NaN (`RE5VK_NANGUARD`), render
  targets sem `MUTABLE_FORMAT`, `VkImageFormatListCreateInfo` em 1.1, latência ≤ `lag-1`, barreira de
  fim de passe com vertex, clears juntados. Não medido.

### 12–19/11 (backend nativo novo e defeito das cabeças)
- Backend reestruturado (12/11): `vkadreno`/`vkmali`, VulkanContext com vk-bootstrap + VMA,
  `VkRenderPass` em cache (`runtime/src/gfx/vkn/`).
- 18–19/11: varredura do `vkmali` e do defeito das cabeças (faixas): descartados textura de vértice,
  `mova`, `texldl`, formatos, barreiras, UBO; achado parcial: stride estático no pipeline
  (`docs/RELATORIO_VKNATIVO_CABECA_2026-11-19.md`). FPS 8–15 no A54, ~100 passes/quadro.

### Sessão de 20–21/11 (esta)
Detalhes em `docs/RELATORIO_GPU_CPU_ENGASGOS_2026-11-20.md` e no fim do `STATUS.md`.
- **Faixas nas cabeças: causa achada.** O driver r38 do Mali erra o stride dinâmico
  (`VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE`). Prova: DXVK 2.6.2 desenhava linhas até no 2D e
  parou com o stride no pipeline; o backend próprio em **modo Vulkan 1.1** (`RE5VK_CORE=1`, sem
  extended dynamic state) ficou sem faixas. Não afetou o fps.
- `robustBufferAccess2` (o Mali r38 não tem `VK_EXT_robustness2`; inócuo lá).
- Coletas de 3 min com simpleperf (`--app`), Perfetto, sysfs e `dumpsys meminfo` no A54 e no 7030
  (§4).
- Contadores de passes: ~100 passes/quadro no A54, quase todos cadeias de mip e de bloom (legítimos);
  "fechamento adiado" do passe (`RE5VK_LAZY_RT`): ganho pequeno.
- **Emulação de float do D3D9 desligada por padrão** (`RE5VK_FLOATEMU`, `RE5VK_NANGUARD`): ALU dos 164
  shaders 82,5 mil → 29 mil instruções. No A54: GPU ~1/3 a menos de clock, **+1 a 2 fps**.
- **Upload de textura** fora do lock do device e caminho de command buffer separado consertado (0 →
  12.236 uploads por ele).
- **Giro antes de dormir no `WaitFor*`** (`RE5_WAIT_SPIN`, 4000 no Android). Sem medida isolada.
- **Afinidade:** threads criadas pelo runtime voltam a 0–7; render só presa com ≥3 núcleos grandes.
  Confirmado no `/proc`; sem ganho medido.
- Cache de pipelines gravado numa thread; swapchain do Android no tamanho do back buffer
  (`ANativeWindow_setBuffersGeometry`); DXVK padrão do Android = 1.9.2b.
- **Medição de GPU por passe** (`RE5VK_PASS_TIMING=1`): no A54, 878 ms/s dentro dos passes, 5 ms entre
  passes; cena 1280×720 = 49 ms/quadro; passes minúsculos ~0,4–0,5 ms cada.
- **Revisão da camada x86 → ARM** (§6.4): acessos ao guest sem barreira, `LOCK` → `__sync_*`, x87 em
  software, `ldmxcsr` raro, `Sleep`/QPC corretos. Nada explica a diferença entre aparelhos.
- DXVK 2.6.2 portada: deixou de cair (buffers nulos com dummy) e de desenhar linhas (stride fixo); tem
  texto quebrado, tela preta e uma queda no `Present` com 2,5 GB.
- Comparação APK antigo (11/11, no 7030) × atual no 7030 com DXVK: em 400–800 draws/quadro, 36,2 ×
  35,5; em 800+, 31,1 × 27,1 (amostras curtas, cenas talvez diferentes). **Não concluído:** o
  benchmark roteirizado para A/B (§8) não chegou a rodar.

## 6. Trechos de código relevantes

### 6.1 `Present` da ponte D3D9 (`runtime/src/win32/d3d9.c`)

O tempo dentro do `Present` do host é medido sempre e vai ao ADPF como "GPU". O limitador de FPS
roda depois.

```c
    {
        RE5_BLOCKING_BEGIN();
        uint64_t t0 = re5_stat_now_ns();   /* sempre: o ADPF recebe a espera no Present como GPU */
        hr = dev->lpVtbl->Present(dev, src, dst, over, dirty);
        present_ns = re5_stat_now_ns() - t0;
        ...
        cap_idle = frame_cap();
        RE5_BLOCKING_END();
    }
    re5_perf_note_present(cap_idle, present_ns);   /* Android: núcleos grandes + ADPF (fase A2) */
```

### 6.2 Atalho das consultas de evento (`d3d9.c`, `RE5_QUERY_LAG`, padrão 3 no Android)

O jogo espera a GPU por `D3DQUERYTYPE_EVENT`. O atalho responde "pronto" n `Present` depois do
`Issue`, sem perguntar ao host.

```c
static int query_lag(void)
{
    static int lag = -1;
    if (lag < 0) {
        const char *e = getenv("RE5_QUERY_LAG");
#if defined(__ANDROID__)
        lag = e ? atoi(e) : 3;
#else
        lag = e ? atoi(e) : 0;
#endif
        ...
static void q_GetData(CPU *c)
{
    ...
    int lag = query_lag();
    if (lag && o->q_issued && g_presents + 1u - o->q_issued >= (uint32_t)lag) {
        ...
        if (o->q_event_cached > 0) {
            if (data && size >= sizeof(BOOL)) *(BOOL *)data = TRUE;
            c->eax = (uint32_t)D3D_OK;
            return;
        }
    }
    HRESULT hr = q->lpVtbl->GetData(q, data, size, flags);
```

### 6.3 Quadros em voo no backend próprio (`runtime/src/gfx/vk/re5vk.c`)

```c
/* Quadros em voo antes de o Present esperar a GPU (RE5VK_LATENCY, padrão 2 no Android, 3 no PC). */
static int latency(void)
{
    ...
#if defined(__ANDROID__)
        v = e && atoi(e) > 0 ? atoi(e) : 2;
    ...
        /* RE5_QUERY_LAG=n ... a latência tem que ser <= n-1 ... */
        int lag = lg ? atoi(lg) : 3;
        if (lag > 0 && !(un && *un == '1') && v > lag - 1) { ... v = cap; }
#endif
```

O `Present` do backend próprio faz `vkQueueSubmit` + blit/desenho do back buffer +
`vkQueuePresentKHR` na própria thread que chamou o `Present` (a `re5-main`), e depois espera o slot
de `this_serial - latency()`.

### 6.4 A camada x86 → ARM (`build/*/gen_include/cpu.h`, gerado)

```c
static inline uint32_t rd32(re5_gaddr a) { return *(RE5_UA_U32 *)(uintptr_t)RE5_G2HW(a); }
static inline void wr32(re5_gaddr a, uint32_t v) { *(RE5_UA_U32 *)(uintptr_t)RE5_G2HW(a) = v; }

/* `lock`-prefixed read-modify-write. */
static inline uint32_t atomic_xadd32(re5_gaddr a, uint32_t v) {
    return __sync_fetch_and_add((volatile uint32_t *)(uintptr_t)RE5_G2HW(a), v);
}
static inline uint32_t atomic_cmpxchg32(re5_gaddr a, uint32_t cmp, uint32_t nv) {
    return __sync_val_compare_and_swap((volatile uint32_t *)(uintptr_t)RE5_G2HW(a), cmp, nv);
}
static inline uint32_t atomic_xchg32(re5_gaddr a, uint32_t v) {
    return __sync_lock_test_and_set((volatile uint32_t *)(uintptr_t)RE5_G2HW(a), v);
}

/* x87: o controle de arredondamento é estado em software (c->fpu_cw), sem fesetround */
static inline double fpu_rint(const CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3u) { ... default: return nearbyint(v); }
}

/* ldmxcsr: só o FZ vai ao FPCR (o jogo chama raramente) */
#elif defined(__aarch64__)
static inline void cpu_ldmxcsr(CPU *c, uint32_t v) {
    uint64_t fpcr;
    c->mxcsr = v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (v & 0x8040u) ? (fpcr | (1ull << 24)) : (fpcr & ~(1ull << 24));   /* FZ */
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
}
```

Observação de correção (não de desempenho): acessos normais sem barreira assumem que o jogo
sincroniza por `LOCK`/Interlocked*; x86 é TSO e ARM não. A fase A1 do plano trata disso. O
`__sync_lock_test_and_set` é só *acquire* (o `xchg` do x86 é barreira completa).

### 6.5 Sono, espera e lock do guest (`runtime/src/win32/kernel32*.c`, `runtime/src/core/thread.c`)

```c
/* Sleep(n>0) dura n ms + um tique fora do Windows (RE5_SLEEP_TICK_US, padrão 1000) */
uint64_t k32_guest_sleep_us(uint32_t ms)
{
    ...
    return ms ? (uint64_t)ms * 1000u + (uint64_t)tick : 0;
}
static void k_Sleep(CPU *c)
{
    uint32_t ms = re5_arg(c, 0);
    RE5_BLOCKING_BEGIN();
    if (ms) host_sleep_us(k32_guest_sleep_us(ms));
    else host_sleep_ms(0);
    RE5_BLOCKING_END();
}
```

Eventos, mutexes e semáforos passam por **um mutex global** (`g_mu`); `wait_objects` dorme num
condvar por waiter. Desde 20/11 há um giro antes de dormir:

```c
        if (!spun && ms != 0 && wait_spin()) {   /* giro curto sem g_mu antes de dormir (só uma vez) */
            spun = 1;
            re5_mutex_unlock(&g_mu);
            for (uint32_t k = 0, lim = wait_spin(); k < lim; k++) {
                ... if (all ? all_ok : any) break;
                spin_pause();   /* "yield" no ARM */
            }
            re5_mutex_lock(&g_mu);
            continue;
        }
```

Lock do guest no modo `hle` (`thread.c`): giro de `RE5_HLE_SPIN` (2000) com `yield` antes de dormir.

### 6.6 Patch de medida e de apresentação na DXVK 1.9.2b (`android/dxvk/re5_present.py`)

```
1) Medida (com RE5_FPS_LOG=1): ms por segundo em vkQueuePresentKHR, no acquire antecipado da
   dxvk-submit, no acquire da thread do jogo, no vkQueueSubmit e nas duas esperas da thread do
   jogo no Present (present anterior; latência de quadros). Uma linha "re5 present:" a cada 5 s.
2) DXVK_RE5_PRESENT_DEPTH=<n> (1 a 3; padrão 1 = o código original): a thread do jogo só espera
   o present de n quadros atrás. Cada quadro leva o próprio semáforo e o índice da imagem até o
   vkQueuePresentKHR ... e o acquire passa a ser feito só na thread do jogo.
3) DXVK_RE5_PRESENT_QUEUE=1: present numa 2ª fila gráfica, numa thread própria (dxvk-present).
```

### 6.7 ADPF e afinidade (`runtime/src/core/perf_hint.c`)

```c
/* Com as duas threads conhecidas: afinidade e sessão ADPF. */
static void setup(void)
{
    uint64_t big = re5_perf_big_mask("/sys/devices/system/cpu");
    if (big) {
        pin_big(g_present_tid, big, "principal");
        if (g_render_tid && g_render_tid != g_present_tid) {
            if (re5_perf_pin_render(big, getenv("RE5_PIN_RENDER"))) pin_big(g_render_tid, big, "de render");
            ...
        }
    }
    /* sessão ADPF com as duas threads; alvo pelo RE5_FPS_CAP (sem limite: 60 fps) */
```

A cada `Present`: `reportActualWorkDuration2` com total, CPU (total − espera no Present) e GPU (a
espera no Present). Medido em 12/11: aceito, sem efeito.

## 7. O que já foi descartado (com a prova)

| Hipótese | Por que caiu |
|---|---|
| Código ARM gerado lento | CPU por quadro igual no A54 e no 7030 (~19 ms) |
| Compilador (GCC × Clang) | NDK só tem Clang; a CPU não é o limite no A54 |
| Núcleos de desempenho não usados | A54 usa os A78 (até mais que o 7030); `RE5_PERF_HINT=0` não mudou o fps |
| GPU no limite | 360p: GPU 40% e +3 fps; DXVK: GPU 72% a 221 MHz |
| Bolhas entre render passes | 5 ms/s entre passes contra 878 ms/s dentro |
| Barreiras com vertex shader | transições de amostragem já usam só o estágio que lê |
| Lock global do guest | modo paralelo (`hle`) é o padrão; `lock_ms` ~20–60 ms/s |
| Present assíncrono na DXVK | `DXVK_RE5_PRESENT_QUEUE=1` e `PRESENT_DEPTH=2` sem ganho (11/11) |
| ADPF / dica de GPU | aceito, clock da GPU igual (12/11) |
| Modo de jogo do Android | já em `performance` |
| Emulação de float | desligada em 20/11: só +1–2 fps |
| Resolução e qualidade | 360p e tudo no mínimo: +3 fps |

## 8. Hipóteses abertas e o que não foi feito

1. **O `vkQueuePresentKHR` segurar o quadro inteiro no A54 (e talvez no Adreno), e não no 7030.**
   Ninguém explicou por quê: espera da fence de renderização no driver (sincronização implícita no
   `queueBuffer`), `BufferQueue`/SurfaceFlinger segurando buffers (modo MAILBOX com 5–6 imagens),
   ou ritmo do compositor. A tentativa com 2ª fila e thread própria não ganhou fps, o que sugere que
   a espera não é só "a thread certa": algo serializa os quadros (o quadro N+1 não começa na GPU
   antes de o N ser apresentado?). Não testado: FIFO de verdade (o `DXVK_CONFIG_FILE` não foi lido),
   `VK_PRESENT_MODE_FIFO_RELAXED`, menos imagens na swapchain, `VK_KHR_present_wait`,
   `ANativeWindow_setFrameRate`, Swappy (Android Frame Pacing), e `adb shell dumpsys SurfaceFlinger
   --latency` para ver o lado do compositor.
2. **Governador da GPU do A54 não sobe o clock** com uso 60–75%. Sem root não há como fixar.
   Explicaria parte, mas não o 360p (GPU a 40% e fps igual).
3. **Por que a principal espera mesmo com a GPU folgada (360p).** Quadro ~70–80 ms, CPU da principal
   ~19 ms, GPU ~33 ms: se fossem sobrepostos, daria ~30 ms. Os números sugerem quadros totalmente
   serializados, CPU → GPU → apresentação, sem sobreposição. O que serializa no 7030 não serializa.
4. **Custo de driver por draw no Valhall com Job Manager** (`mali-cmar-backend` 13% + `libGLES_mali` 16,6% no A54).
5. **Comparação APK antigo × atual** no mesmo trecho (benchmark roteirizado: `RE5_INPUT_SYNC=titlelogo`,
   `RE5_INPUT_HOLD=20`, `RE5_INPUT_SCRIPT=300:Return,+500:Down,+250:Down,+250:Return,+300:Return`):
   não rodou. Mudanças desde o último commit que alteram o ritmo das threads no Android, todas com
   chave: `RE5_SLEEP_TICK_US=0`, `RE5_HLE_SPIN=0`, `RE5_WAIT_SPIN=0`, `RE5_WORKER_BIG=1 RE5_PIN_RENDER=1`,
   `RE5_QUERY_LAG=2`, e o limite de 60 fps (`gfx_fps_cap`).
6. Não coletado: Snapdragon 8 Gen 4 e o "outro celular" (sem dados aqui). Seria útil saber se o
   `vkQueuePresentKHR` também segura o quadro neles.

## 9. Como medir (ferramentas existentes)

- `RE5_FPS_LOG=1` no `re5_env.txt` (`/sdcard/Android/data/com.re5.recompiled/files/game/`): por
  segundo, fps, draws, `lock_ms`, `d3d9_ms`, `present_ms`, consultas; no backend próprio, `gpu_ms`,
  `acquire_ms`, `fence_ms`, `em_voo_ms`, `submit_ms`, `queue_present_ms`, `mutex_ms`, passes; esperas
  do jogo por endereço (`espera ...`); tempo por função importada (`thunk d3d9 ms/s`).
- `RE5VK_PASS_TIMING=1`: tempo de GPU por passe e por tamanho de alvo.
- Variáveis do launcher têm prioridade sobre o `re5_env.txt` (o renderizador salvo vence `RE5_GFX`).
- simpleperf no aparelho sem root: `simpleperf record --app com.re5.recompiled -e cpu-clock -f 1000
  --call-graph fp` (com `-p` dá "Can't record kernel samples").
- Perfetto: `scripts/android_trace.py` (roteiro do benchmark + trace) e `scripts/android_trace_report.py`.
- `scripts/android_perf.py`: CPU por thread, clocks, uso da GPU e veredito.

## 10. Arquivos de evidência

- 20–21/11: `work/android/a54_core_20261120/`, `a54_dxvk192b_20261120/`, `a54_passtiming_20261120/`,
  `a54_360p_usuario_20261120/`, `a54_otim_usuario_20261120/`, `d7030_dxvk192b_20261120/`,
  `d7030_novo_dxvk_b_20261120/`, `d7030_novo_vkmali_20261120/` (logcat, amostras, meminfo,
  simpleperf, Perfetto).
- 11–12/11: `work/android/a54_1111_*`, `a54_dxvk_*`, `a54_dxvk_gpuhint*`, `a54_re5vk_srgb*`.
- Relatórios: `docs/RELATORIO_GPU_CPU_ENGASGOS_2026-11-20.md`,
  `docs/RELATORIO_VKNATIVO_CABECA_2026-11-19.md`, `docs/PROJETO_VULKAN_NATIVO.md`,
  `docs/PESQUISA_VULKAN_ANDROID.md`, `docs/PLANO_MESTRE_OTIMIZACAO.md`, `docs/STATUS.md`.

## 11. Correções e achados depois da primeira versão (21/11)

- **Arquitetura:** o Mali-G68 do A54 é **Valhall com Job Manager** (`mali_kbase` JM, thread
  `mali-cmar-backend`), não Bifrost como estava acima; o Mali-G610 do 7030 é **Valhall CSF**.
- **Mutex do re5vk no `Present`:** confirmado no código: `v_Present` pega `re5vk_lock(d)` na entrada e
  só solta depois do `vkQueuePresentKHR` (comentário: a fila exige acesso exclusivo). Efeito medido
  pequeno: `mutex_ms` ~8–10 ms/s no A54.
- **`RE5_QUERY_LAG`:** `q_issued = g_presents + 1` no `Issue`; o atalho só vale `lag` Present depois.
  Chamadas ao `GetData` do host por segundo: 7030 com lag 2 (APK de 11/11) = 0; A54 com lag 3 =
  10–20; 7030 com lag 3 (build de 20/11) = 19–20, rodando bem. Não separa os aparelhos.
- **Hipótese nova a checar primeiro:** os aparelhos lentos conhecidos são Samsung (A54, Z Flip5;
  possivelmente o do 8 Gen 4); o 7030 é Motorola. O GOS da Samsung limita jogos, e o app se declara
  jogo (`android.game_mode_config` no manifesto). Falta saber a marca do "outro celular".
