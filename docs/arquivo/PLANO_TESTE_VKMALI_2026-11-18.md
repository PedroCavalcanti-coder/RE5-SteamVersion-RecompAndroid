# Plano de teste: varredura vkmali (18/11/2026)

APK: `android/app/build/outputs/apk/debug/app-debug.apk` (instalado no A54). Nada abaixo foi visto no
celular: tudo é **hipótese/contorno** até você testar. Chaves vão em `re5_env.txt` (uma por linha).
Base: `RE5_GFX=vkmali`. Rode com `RE5_FPS_LOG=1` para ver os contadores.

## Mudanças (todas com chave)

| Mudança | Padrão | Para desligar / mudar |
|---|---|---|
| `KILL_DEPTH` (FragDepth no PS com texkill) | **desligado** (suspeito do hang) | `RE5VK_KILL_DEPTH=1` liga |
| Passada só de profundidade: pula só os draws com texkill | **modo 2** | `RE5VK_ZPREPASS=1` inteira, `=0` nenhuma |
| Upload de textura nova em command buffer separado (não fecha o passe) | ligado | `RE5VK_UPLOAD_CB=0` |
| Textura de vértice (skinning/face) sempre NEAREST (contorno) | ligado | `RE5VK_VTX_NEAREST=0` |
| Limite de 60 FPS no Android se nunca escolhido | 60 | Configurações > Limite de FPS, ou `RE5_FPS_CAP=` |

## O que olhar
1. **Tela de carregamento**: GPU ainda a 100%? Trava/reinício? No log: `passes` por segundo caiu?
   `uploads_cb_sep` alto e `uploads_cb_main` baixo?
2. **Menu**: FPS ~60 (antes ~80).
3. **Folhas, cerca de arame, cabelo**: retângulo preto voltou? (modo 2 da passada de profundidade.)
   Se personagens aparecerem através de paredes: `RE5VK_ZPREPASS=1`.
4. **Cabeça dos personagens**: a linha/listras continua? Compare com `RE5VK_VTX_NEAREST=0`.
5. Bissecção se o hang voltar: `RE5VK_UPLOAD_CB=0`, depois `RE5VK_NAN_CULL=0`, depois `RE5VK_NANGUARD=0`.

## Não feito / sem causa achada
- Item 1 (linha na cabeça): `mova` e `texldl` conferidos contra a DXVK (equivalentes); `c[a0+n]` fora da
  faixa é preso em [0,255] (DXVK devolve zero). Sem causa comprovada; o NEAREST é palpite.
- Item 3 (luz/sombra): não investigado a fundo.
- Item 4: contadores de passes/uploads adicionados; UBO já copia só `const_f` floats; latência já é 2 no
  Android. `spirv-opt` existe só como ferramenta offline (sem biblioteca no runtime).
- Vulkan 1.1: fora do escopo desta rodada.

## Rodada 2: lixo no cabelo/rosto (so com a cabeca na tela)

Hipotese nova (nao vista no celular): corrida de leitura-escrita na textura de vertice da animacao
facial. Os 6 VS de skinning leem a textura no VS (`texldl`); o jogo reescreve essa textura quando a
cabeca aparece. Dentro de um passe ja aberto, o estagio VS que le a textura nao era registrado, entao a
barreira do upload seguinte esperava so o fragment shader e o VS lia a textura enquanto ela era
reescrita (num tiler o vertice roda adiantado). Correcoes:
- barreira de saida de SHADER_READ_ONLY agora sempre espera VS+FS (`vk_res.c`, `layout_stage_src`);
- o estagio do VS passa a ser registrado mesmo com o passe aberto (`RE5VK_STAGE_NOTE=0` desliga o registro).
Se o lixo persistir, ligue `RE5_FPS_LOG=1` + `RE5VK_RS_LOG=1` e mande o log com a cabeca na tela, e teste
`RE5VK_VTX_NEAREST=0` / `RE5VK_UPLOAD_CB=0` para isolar.
