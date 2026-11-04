# Relatório para investigação externa — desempenho (03/11/2026)

Para quem vai investigar sem ter acompanhado a sessão. Leia junto com `CLAUDE.md`,
`docs/ARQUITETURA.md` (decisões D1–D9) e `docs/STATUS.md`. Tudo o que está marcado como
medido tem a fonte ao lado. O resto é hipótese e está dito assim.

## 1. Onde estamos

Máquina do usuário: Ryzen 7 5700U (Zen 2, 8 núcleos / 16 threads, APU de 15 W, Radeon Vega,
driver AMD com Vulkan 1.3.260). Windows 11, 1080p.

| Medida (usuário, 1080p) | Original (`re5dx9.exe`, D3D9 nativo) | Nosso, DXVK 2.6.2 | Nosso, re5vk |
|---|---|---|---|
| Benchmark **variável**, média | — | 89,4–90,1 | **93,0** |
| Benchmark variável, área 1 | **~170** | 104,7 | — |
| Benchmark **fixo**, média | — | 68,9–74 | **78** |
| Uso de CPU | referência | — | ~10% **menos** que o original |

O ponto central: na área 1 o original faz ~170 fps e nós ~105, **usando menos CPU**. O limite
não é falta de CPU; é um caminho crítico serial (seção 3).

Variável e fixo são benchmarks diferentes do jogo: os números não se comparam entre si. Entre
rodadas do mesmo benchmark a oscilação é de ~5 fps.

## 2. O que mudou nesta sessão (todos no `master`, com teste)

| Commit | Mudança | Evidência |
|---|---|---|
| `7d763e71` | Lifter: a regex de `_sync_kind` casava `RE5_WIDE(c->ebx, ...)` como chamada que vê a CPU; o `x87_cache` sincronizava `c->fpu_top` em volta de quase todo acesso à memória | `R_ftop = c->fpu_top;` 2.407.396 → 535.035 no código gerado; **50 → 53,7 fps** |
| `643ea76e` | `kernel32_sync.c`: cada espera tem variável de condição própria (antes um broadcast global acordava toda thread parada a cada `SetEvent`) | acordadas inúteis no teste 16.787 → 0; **53,7 → 55,9 fps** |
| `d99d7e4a` | Lock do HLE com caminho rápido (CAS) | espera do lock no perfil 3,3–4,8% → 0,8–1,5% |
| `7df5125c` + `3f4f8b47` | Cache no ponto de chamada dos calls indiretos (`lift.call_site_cache`) | **empate** (89,4 com, 90,1 sem): desligado, código fica |
| `71bf1573` | Backend Vulkan próprio no master (`RE5_GFX=re5vk`, padrão `dxvk`) | — |
| (no merge) | re5vk: imagens nascem zeradas (era a causa da cena 3D preta) | usuário conferiu a cena normal |
| (no merge) | re5vk: swapchain acompanha a janela | tela cheia e janela corretas |
| `b12daa5b` | Tabela de handles sem mutex na leitura; re5vk copia só as constantes que o shader lê | handles: empate (73 x 74, fixo) |
| `e89f972c` | `RE5_PROFILE` diz em qual handler sem lock cada thread fica parada; `perfil_vulkan.bat` | — |
| — | `spin_pause` (pausa de CPU nos laços de espera) | **empate** (89,2 x 90,1): desligado |

## 3. O que o perfil mostra (re5vk, benchmark variável)

Fontes: `work/perf/re5vk_main.log` e `work/perf/re5vk_render.log` (não estão no git; são
gerados por `perfil_vulkan.bat re5vk main|render`). O perfil amostra a thread a cada ~1 ms
e liga `RE5_FPS_LOG`, então os fps dessas rodadas (~72) ficam abaixo dos normais.

CPU por thread, medida pelo SO (`work/perf/vk_main.log`, 454 s):

| Thread | Papel | CPU |
|---|---|---|
| `re5-main` | principal do jogo | 75% de um núcleo |
| `g00457C60` | render (é a que chama o D3D9) | 58% |
| `g004278D0` ×7 | workers do job system | 24–36% cada |
| driver Vulkan (`amdvlk64`) | — | ~0 |

**Thread principal** — roda ~44% do tempo; nos outros ~56% está parada em:
`EnterCriticalSection` 45%, `SetEvent` 15% (medido antes dos handles sem mutex), cópia do
`IDirect3DVertexBuffer9::Unlock` 12%, `WaitForMultipleObjects` 11%, `LeaveCriticalSection` 6%,
`Present` 5%. Quando roda: 87–91% código gerado; funções mais amostradas `0x00F492A0` (5,5%),
`0x004AB9E0` (4,7%), `0x00587840` (3,5%).

**Thread de render** — roda ~46%; parada 56% em `WaitForSingleObject` (espera trabalho da
principal). Quando roda, **82% é handler D3D9** e só 7% código do jogo:
`DrawIndexedPrimitive` 34%, `SetStreamSource` 9%, `SetTexture` 8,5%, `SetVertexShaderConstantF`
5%, `SetPixelShaderConstantF` 5%, `DrawPrimitive` 4%, `SetIndices` 3%. ~1 milhão de chamadas
D3D9 por segundo, ~1.000 draws por quadro.

**Leitura:** as duas threads se revezam, cada uma esperando a outra metade do tempo. Encurtar o
trecho de qualquer uma sobe o fps. No trecho da render, quase tudo é o nosso front-end D3D9.

## 4. Perguntas para investigar

1. **Custo por draw no re5vk** (`runtime/src/gfx/vk/vk_draw.c`, `prepare()` e
   `v_DrawIndexedPrimitive`). O que ainda é feito a cada draw:
   - 16 vertex buffers religados (`vkCmdBindVertexBuffers2`) mesmo sem mudança;
   - descritor novo alocado e `vkUpdateDescriptorSets` a cada troca de textura ou shader;
   - `pipe_key` montada com `memset`, hash e busca a cada draw;
   - `vkCmdBindIndexBuffer` a cada draw indexado;
   - `re5vk_lock`/`unlock` (mutex do device) em cada método.

   Quanto disso é custo real? Qual a ordem de ataque? Vale um caminho de descritores push
   (`VK_KHR_push_descriptor`) ou templates? O driver aceita (Vulkan 1.3.260, AMD)?
2. **Caminho do thunk** (`runtime/src/core/thunks.c`, `re5_thunk_invoke`) e da ponte COM
   (`runtime/src/win32/d3d9.c`: `obj_of`, `self_obj`, `d9_in`) em cada uma das ~1 milhão de
   chamadas por segundo: quanto custa por chamada, e o que dá para cortar sem quebrar as regras
   da seção 5?
3. **Qual seção crítica a thread principal disputa** (45% das esperas dela) e com quem. Hoje o
   perfil não diz o endereço. Sugestão: medir no `EnterCriticalSection` (`kernel32_sync.c`)
   endereço e dono por amostra, ou contar contenção por endereço.
4. **Cópia do `Unlock` de vertex buffer** (12% das esperas da principal): a ponte copia o
   buffer-sombra do guest para o host no `Unlock`. Dá para copiar só o intervalo travado, ou
   usar `D3DLOCK_DISCARD`/`NOOVERWRITE` para evitar a cópia?
5. **Por que o original faz ~170 fps** com mais CPU que nós. O D3D9 nativo da AMD custa menos
   por chamada que o nosso front-end, ou o original paraleliza melhor entre principal e render?
   Uma medida no original (`scripts/thread_cpu.py --name re5dx9.exe`) responde.

## 5. Regras do projeto que uma proposta tem que respeitar

- **Falhar alto (D8):** nada de stub que devolve 0 para seguir em frente.
- **Uma mudança por vez, atrás de chave quando mudar comportamento, com teste em `tests/` e
  `ctest` verde, e medida A/B pelo usuário no jogo.** Empate = desligado (foi assim com
  `spin_pause` e `call_site_cache`).
- Memória do guest só por `rd*/wr*`/`re5_g2h()`; nunca guardar ponteiro do host na memória do
  guest (use `re5_handle_new`).
- O Android usa o lock **global** (`RE5_GUEST_LOCK=global`): ele reveza as threads contando
  dispatches. Qualquer atalho que pule `dispatch`/`go()` precisa manter essa contagem.
- **Não proponha (já avaliado e errado):**
  - `(re5_thread *)c` para achar a thread pela CPU: há CPUs avulsas fora de `re5_thread` (nos
    testes, por exemplo), e a escrita corromperia memória;
  - cache de chamada com dois `static` separados (`va` e `fn`): outra thread lê o `va` novo
    com o `fn` velho e chama a função errada (o código atual guarda um ponteiro para o slot);
  - tirar o lock do HLE de toda a ponte D3D9: a ponte tem estado global (hash de objetos,
    travas, sombras) e o perfil mostrou que quase não há disputa, só custo de aquisição.

## 6. Mapa rápido dos arquivos

| Área | Arquivo |
|---|---|
| Lifter (x86 → C) | `tools/pcrecomp/tools/lift/lift32_cpu.py`; pós-processo em `scripts/run_lift.py` |
| Dispatch e trilha | `runtime/src/core/dispatch.c` |
| Thunks / lock do HLE / perfil | `runtime/src/core/thunks.c`, `runtime/src/core/thread.c` |
| Sincronização Win32 | `runtime/src/win32/kernel32_sync.c` |
| Ponte COM do D3D9 | `runtime/src/win32/d3d9.c`, `runtime/src/gfx/d3d9_host.c` |
| Backend re5vk | `runtime/src/gfx/vk/` (`re5vk.c` device/present, `vk_draw.c` estado/draw, `vk_res.c` recursos, `vk_dxso.c` tradutor SM3 → SPIR-V) |
| Configuração do lift | `config/re5_config.toml` |
| Como rodar o perfil | `perfil_vulkan.bat [re5vk|dxvk] [main|render|all]` |
