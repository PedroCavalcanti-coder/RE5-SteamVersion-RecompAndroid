# Relatório: varredura do vknativo (vkmali) e o defeito das cabeças — 18 e 19/11/2026

Para quem for inspecionar em outro lugar. Tudo abaixo foi medido ou lido no código; o que é hipótese
está marcado. Nada foi commitado. Leia antes: `CLAUDE.md`, `docs/PROMPT_VARREDURA_VKMALI.md` (o pedido
original), `docs/PLANO_TESTE_VKMALI_2026-11-18.md` (plano de teste das rodadas 1 e 2).

## 1. O defeito

Aparelho: Galaxy A54 (SM-A546E), Mali-G68, driver r38p1 (Vulkan 1.3.0), `RE5_GFX=vkmali`. O mesmo
defeito aparece no Adreno (vkadreno). A DXVK no mesmo celular renderiza certo; no PC o backend nativo
também (Intel/NVIDIA).

- Só aparece **quando a cabeça de um personagem está na tela**. Sheva em gameplay e todos os NPCs não são
  afetados; Chris (gameplay) e as cabeças de cutscene (inclusive Sheva) são.
- Sintoma histórico: triângulos gigantes saindo do corpo; depois uma linha atravessando a cabeça; hoje
  **faixas longas marrom-avermelhadas, com textura de cabelo, que se movem com o personagem**, e em
  momentos piores a tela inteira estilhaçada em triângulos (5 fps).
- Cabeça/cabelo/rosto com lixo ou pretos sem textura.
- Também: GPU em 99–100% e ~100 passes de render por quadro (≈1500/s a ~15 fps), FPS 8–15.

## 2. Fatos medidos no aparelho (logcat, `RE5_FPS_LOG=1`, `RE5VK_RS_LOG=1`)

1. **Declarações de vértice dos meshes com skinning** (`RS_LOG decl de skinning`):
   - corpo/NPC, 1 stream: `[s0+0 SHORT4N pos][s0+8 UBYTE4 BLENDINDICES0][s0+12 UBYTE4N BLENDWEIGHT0]
     [s0+16 UBYTE4N NORMAL][s0+20 UBYTE4N TANGENT][s0+24 FLOAT16_2 TEXCOORD0][s0+28 FLOAT16_2 TEXCOORD1]`;
   - 8 ossos: igual com `BLENDINDICES1` (s0+12) e `BLENDWEIGHT1`;
   - **cabeça, 2 streams**: o anterior (8 ossos) mais `[s1+0 FLOAT16_2 TEXCOORD1][s1+4 UBYTE4N TANGENT]`.
   Só as cabeças usam o stream 1. Todos os tipos (2,1,4,10,5,8,15 → VkFormat 106,103,44,92,39,37,83) são
   "suportados" pela GPU.
2. **Texturas lidas no vertex shader** (6 VS de skinning com `texldl`: vs_49b28f19, vs_548f7a0b,
   vs_a3262cc3, vs_e5b76be9, vs_f6850db3, vs_fe8cde5e): `A32B32G32R32F 4x4` (vkf 109) e `R32F 256x1`
   (vkf 100), 1 mip, slot 16, layout SHADER_READ_ONLY, nenhuma é a textura de reserva preta.
3. **`RE5VK_VTX_ZERO=1` (leitura no VS devolve zero) NÃO resolve** — confirmado pelo log da rodada
   (a linha `diagnosticos de cabeca: VTX_ZERO=1` aparece). A textura de vértice não é a causa.
4. **`RE5VK_POS_HUGE=1000` (descarta posição NaN/Inf/|c|>1000) diminui** os raios mas não os elimina:
   a explosão tem valores finitos abaixo de 1000 no espaço de clip.
5. Com o `STATIC_STRIDE` (item 4.3) a cabeça/cabelo do Chris passou a aparecer **com textura** (print de
   19/11 10:20), em vez de rosto preto; duas faixas persistem.
6. Com `RE5VK_REL_CLAMP=2` (índice relativo fora da faixa devolve zero) o usuário diz "parece a mesma coisa".
   (Ainda não conferi esse log por falta de tempo; o usuário avaliou visualmente.)
7. O jogo roda o benchmark embutido do RE5 ("Press to end the test"); 800x600 interno.

## 3. Alterações feitas (todas com chave `RE5VK_*`; `getenv` lido uma vez)

| # | Mudança | Arquivo | Chave / padrão |
|---|---|---|---|
| 1 | `KILL_DEPTH` (FragDepth no PS com texkill) agora **desligado** (suspeito de GPU hang) | `vk_dxso.c` `kill_depth_fix` | `RE5VK_KILL_DEPTH=1` liga |
| 2 | Passada só de profundidade: modo 2 (pula só draws com texkill) | `vk_draw.c` (~l.2040) | `RE5VK_ZPREPASS=0/1/2`, padrão 2 |
| 3 | Uploads de textura nova em command buffer separado (não fecha o passe) | `re5vk.c` (`re5vk_upload_cmd`, `submit` com 2 cbs), `vk_res.c` `upload` | `RE5VK_UPLOAD_CB=0` desliga. **BUG MEU:** a condição usa `!im->rt_views`, mas `rt_views` é alocado em toda imagem → o caminho nunca roda (`uploads_cb_sep` sempre 0). Falta corrigir (usar a flag de uso RT) |
| 4 | Limite padrão de 60 FPS no Android | `GameActivity.java` | pref `gfx_fps_cap` ausente → 60 |
| 5 | Contadores no log por segundo: `passes`, `uploads_cb_sep`, `uploads_cb_main` | `re5vk.c` `timing_tick`, `vk_core.h` | com `RE5_FPS_LOG=1` |
| 6 | Textura de vértice sempre NEAREST | `vk_draw.c` | `RE5VK_VTX_NEAREST=0` desliga (padrão 1) |
| 7 | Barreira de saída de SHADER_READ_ONLY sempre inclui VS+FS; estágio VS registrado mesmo com o passe aberto | `vk_res.c` `layout_stage_src`, `vk_draw.c` (prepare) | `RE5VK_STAGE_NOTE=0` desliga só o registro |
| 8 | **`STATIC_STRIDE`**: o pipeline guarda também o stride real de cada stream (antes 0, com estado dinâmico) | `vk_draw.c` (`pipeline_create`) | `RE5VK_STATIC_STRIDE=0` desliga (padrão 1) |
| 9 | Posição do VS: NaN **ou Inf** vira NaN (triângulo descartado) | `vk_dxso.c` `finish_outputs` | `RE5VK_NAN_CULL=0` desliga; `RE5VK_POS_HUGE=<n>` também descarta |componente|>n |
| 10 | Índice relativo fora da faixa devolve zero (modo DXVK) | `vk_dxso.c` `read_const`, `rel_zero` | `RE5VK_REL_CLAMP=2` (padrão 1 = preso no limite) |
| 11 | Diagnósticos | `vk_draw.c`, `vk_dxso.c` | `RE5VK_VTX_ZERO=1`, `RE5VK_SKIP_VTXTEX=1`, log "diagnosticos de cabeca" e "textura de vertice slot" |
| 12 | Teste `test_vk_dxso.c` ajustado para KILL_DEPTH desligado por padrão | `tests/unit/test_vk_dxso.c` | — |

Estado dos testes: `ctest` 42/42 (o `difftest_cpu` é pulado); os 164 shaders de `work/shaders` traduzem e
validam no `spirv-val` do NDK com e sem as chaves novas.

Chaves de diagnóstico que já existiam: `RE5VK_RS_LOG=1`, `RE5VK_SKIP_NOCOLOR`, `RE5VK_SKIP_REL`,
`RE5VK_REL_CLAMP`, `RE5VK_NANGUARD`, `RE5VK_FLOATEMU`, `RE5VK_NAN_CULL`, `RE5VK_LATE_KILL`,
`RE5VK_POS_INVARIANT`, `RE5VK_TBDR`.

## 4. O que foi investigado e descartado (com o porquê)

1. Formatos de vértice: UBYTE4 → `R8G8B8A8_USCALED` (ou UINT + conversão no VS se a GPU não tiver);
   D3DCOLOR → `B8G8R8A8_UNORM`; todos suportados (log). Descartado.
2. `mova`: arredonda (`floor(x+0.5)`), preserva 4 componentes e máscara; equivalente ao `opRound` da DXVK.
   `texldl` no VS: lod explícito = `coord.w`; equivalente. Descartado.
3. `BaseVertexIndex`: `vkCmdDrawIndexed(..., start, base, ...)` repassa como `vertexOffset`. OK.
4. `decl_elem`: casa elemento por (Usage, UsageIndex), então BLENDINDICES1 não colide com 0. OK.
5. Anel de UBO de constantes por envio com offset dinâmico; espera o fence do slot. Corrida descartada.
6. Textura de vértice (item 2 da seção 2) e seus hazards: descartada pelo teste `VTX_ZERO=1`. As correções
   de barreira (item 7 da tabela) são corretas em si, mas **não eram a causa**.
7. Samplers de vértice: slot D3DVERTEXTEXTURESAMPLER0..3 (257..260) → 16..19, binding `VS_SAMP+n`,
   stageFlags VERTEX, estados padrão corretos.
8. `rel_clamp` vs DXVK: DXVK lê o UBO sem clamp e faz `Select(idx < floatCount, valor, 0)`. O backend
   nativo prende no limite (padrão). Modo 2 reproduz a DXVK; sem efeito visível, segundo o usuário.
9. Precisão: não há `RelaxedPrecision` no tradutor.

## 5. Hipóteses abertas (ordem sugerida de ataque)

A. **Stream 1 / vertex input na Mali r38** (a mais promissora). Efeito observado ao ligar `STATIC_STRIDE`
   (rosto/cabelo voltaram a ter textura) sugere que o driver trata mal o **stride dinâmico**
   (`VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` + `vkCmdBindVertexBuffers2`) para bindings > 0, ou o
   conjunto atributo/binding. As faixas restantes podem ser outro atributo do stream 0 lido com o stride
   errado (ex.: só os meshes de cabeça têm layout diferente). Testar: tirar o estado dinâmico de stride
   (stride só estático na chave do pipeline + `vkCmdBindVertexBuffers` sem stride) — é o caminho Vulkan 1.1
   do plano, fora do escopo pedido, mas serve de experimento. Outro teste: `RE5VK_STATIC_STRIDE=0` e
   comparar.
B. **Outros estados dinâmicos de `VK_EXT_extended_dynamic_state`** na Mali r38 (cull, front face, topologia,
   depth/stencil op): o pipeline usa quase todos dinâmicos (`vk_draw.c` ~l.1330). Um deles pode estar sendo
   ignorado para esses draws. Experimento: estático na chave para os que variam nas cabeças.
C. **Vértice com atributo fora do stride/limite**: `vertexAttributeAccessBeyondStride`/alinhamento de
   offset+stride a 4 bytes (FLOAT16_2 em s0+24/28 e s1; stream 1 tem stride 8). Verificar `maxVertexInputBindingStride`
   e se o stride real do stream 1 é múltiplo do tamanho do componente.
D. **Constantes de osso**: confirmar com captura (RenderDoc/AGI) o VS In vs VS Out do draw que explode. Se
   VS In já está explodido → problema de vertex input (A/B/C); se só o VS Out → matrizes `c23+...`.
   O `RE5VK_RS_LOG=1` já loga estado, constantes de PS e decls; vale estender para logar as constantes
   `c23..c100` e os 4 primeiros vértices/índices dos draws com `vsi->uses_rel`.
E. **Valores de índice de osso**: `UBYTE4` vira `USCALED` → float 0..255; com `a0 = round(idx*3)`, índices
   válidos chegam a `c23 + 3*idx`. Um vértice de peso ≠ 0 e índice acima de ~77 sai do UBO (256 vec4).
   O modo `REL_CLAMP=2` zera nesse caso; se a faixa some com zero e não com clamp, os dados de vértice do
   stream estão lidos errados (volta a A/C).
F. Rostos pretos sem textura (rodada com POS_HUGE sem STATIC_STRIDE): ver 5 da seção 2; possivelmente
   tangente/UV2 do stream 1 lidos com stride 0 (consistente com A).

## 6. Pistas e armadilhas operacionais (aprendidas à custa de tempo)

- **O app lê `re5_env.txt` de `/sdcard/Android/data/com.re5.recompiled/files/game/`** (não de `files/`).
  Gravei na pasta errada por várias rodadas; o arquivo certo tinha `RE5VK_VTX_ZERO=1`.
  Variável já no ambiente tem prioridade sobre o arquivo (`runtime/src/host/main.c`, `load_env_file`).
  Reiniciar o app (`am force-stop com.re5.recompiled`) depois de mudar o arquivo.
- adb: `C:\Users\Dev_RV01\AppData\Local\Android\Sdk\platform-tools\adb.exe`; com Git Bash use
  `export MSYS_NO_PATHCONV=1`. O servidor adb às vezes cai (`adb kill-server && adb start-server`).
- O log útil: `adb logcat -d -s RE5Native | grep re5vk`. `stderr` do host não aparece no logcat.
- Build Android: NDK 30 em `build/android-arm64-ndk30` → `llvm-strip --strip-unneeded` →
  `android/app/src/main/jniLibs/arm64-v8a/libre5.so` → `android/gradlew.bat assembleDebug` com
  `JAVA_HOME="C:/Program Files/Android/Android Studio/jbr"`. Testes de host: `cmake --build build/host-tests`
  e `ctest` nessa pasta. Shaders reais: `work/shaders` (164 `vs_*.bin`/`ps_*.bin`); o `/tmp/dis.py` usado
  para desmontar não está no repo (disassembler de SM3 de ~60 linhas; pode ser refeito).
- Disco: o PC ficou com ~9 GB livres; maiores pastas regeneráveis: `build/windows-x64` (11 GB),
  `build/host-tests` (5,7 GB), `build/android-arm64*` (~7,5 GB).

## 7. Pendências fora do defeito (do plano original)

- Item 3 (luz/sombra) não foi investigado a fundo.
- Item 4 (desempenho): ~100 passes/quadro e GPU em 100%; contadores novos medem; o caminho de upload em cb
  separado precisa do conserto do item 3 da tabela. `spirv-opt` só existe como ferramenta offline.
- Vulkan 1.1 (sem `extendedDynamicState`/`dynamic rendering`): **não feito**, a pedido do usuário.
  Observação: a hipótese A/B acima é justamente um motivo extra para fazê-lo.
- `STATUS.md` não foi atualizado: só aceita "FEITO" com evidência do celular, e o defeito não está resolvido.

## 8. Resumo em uma frase

A textura de vértice, o skinning (`mova`, `texldl`, formatos), as barreiras e o UBO foram descartados ou
corrigidos sem curar o defeito; o único achado que mudou o resultado visível (rosto/cabelo com textura) foi
dar stride estático ao pipeline, o que aponta para **vertex input / estados dinâmicos do
VK_EXT_extended_dynamic_state na Mali r38 nos meshes de cabeça (2 streams)**; as faixas restantes ainda
não têm causa comprovada.
