# Prompt 1: otimização profunda do que achamos e da CPU

Cole o texto abaixo numa sessão nova de IA aberta na raiz do repositório. Base: `docs/prompts/00_BASE_LEAN_2026-11-23.md`.

---

Você vai reduzir o tempo de CPU por quadro do RE5 recompilado no Android (Galaxy A54), partindo da **base lean** (build de 35 fps
de pico). Leia primeiro `docs/prompts/00_BASE_LEAN_2026-11-23.md` (estado, compilador, números, regras, o que já NÃO deu fps),
depois `CLAUDE.md`, `docs/STATUS.md` (final), `docs/PLANO_MESTRE_OTIMIZACAO.md`, `docs/DOSSIE_DESEMPENHO_ANDROID_2026-11-21.md`
(seção 11) e `docs/AUDITORIA_GHIDRA.md`. Em português.

## Por que a CPU

No A54 a GPU não é o limite: o governador a mantém em ~250 MHz com 60–75% de uso, e baixar a resolução para 360p quase não
muda o fps. O tempo de quadro é decidido pela **thread principal (`re5-main`) e pela thread de render (`g00457C60`)**, que
trabalham em sequência (~19 ms de CPU por quadro na principal, medido no A54 e no 7030). Qualquer ms tirado dali vira fps.

## Alvos, em ordem (todos com evidência no perfil; confirme cada um antes de mexer)

1. **`memmove` da thread principal (16% do tempo dela) — chamador desconhecido.** Descubra quem chama: perfil com
   `--call-graph dwarf` ou build com `-fno-omit-frame-pointer`. Candidatos: cópia de buffer no `Unlock` do D3D9, sombras de
   textura, `memcpy`/`memmove` do CRT do jogo (override `re5_ov_00E63EC0`/`00E69B90` para o `memmove` do host), cópias do
   HLE. Se for evitável (cópia redundante, tamanho exagerado, não alinhada), corte.
2. **Esperas e travas (`syscall` 18% + `wait_objects` 7% na principal).** O jogo vive em `WaitForSingleObject` /
   `CRITICAL_SECTION` / eventos. Meça quantos futex por quadro, quem acorda quem, e se o giro antes de dormir
   (`RE5_WAIT_SPIN`, `RE5_HLE_SPIN`) ajuda ou piora; veja `lock_ms` do `RE5_FPS_LOG`. Considere substituir esperas por
   variáveis de condição com menos trocas de contexto, e afinidade das threads (`RE5_PIN_RENDER=1`, `RE5_WORKER_BIG`): os
   núcleos grandes rodam a 1,6–1,9 GHz de um máximo de 2,4.
3. **Custo do Java do app (4–6 fps).** O app completo custa 29→35 de pico contra o modo lean. A solução de fundo é o
   prompt 3 (toque nativo, sem camada Java); aqui só encontre **qual peça** custa mais, com `RE5_NO_TOUCH`, `RE5_NO_HUD`,
   `RE5_NO_OVERLAY`, `RE5_NO_MISC` (um por vez, 1,5 min cada), e deixe essa peça barata (HUD de 500 ms varre
   `/proc/self/task/*/stat` de ~50 threads na thread de UI; a camada de controles redesenha a cada toque).
4. **Código gerado do jogo (≈80% das amostras de CPU em jogo).** O perfil é plano, mas há pontos: `0x00587840`
   (2535 bytes, 8,6% no PC), `re5_ov_00CD5FB0`, e o limitador de quadro. Use `scripts/prof_lines.py` e os `difftest_*` para
   criar *overrides* nativos verificados (P5 do plano mestre) das folhas mais quentes, em NEON quando fizer sentido.
5. **Compilação guiada por perfil.** O PGO por instrumentação deu empate no PC (<2%) e `-mtune`/`-march` não deram nada no
   A54. Se for tentar, use perfil por **amostragem** (simpleperf + `llvm-profgen`, AutoFDO) porque o binário instrumentado de
   ~60 MB de código roda lento demais para treinar; ordenação de funções (`--symbol-ordering-file` do lld) para juntar as
   quentes, e `-O2` só nas unidades quentes do código gerado. Cada uma só entra com ganho medido.
6. **Carregamento** (inflate da zlib 1.2.1 do jogo em `0x006CC2C0..0x006CF940`, quente ao carregar): troque pelo `inflate`
   nativo com `difftest_override`. Meça com cache de texturas limpo.
7. **Custos de medição**: `RE5_FPS_LOG` ligava `clock_gettime` por chamada no re5vk (23% da thread de render; o re5vk saiu da árvore, o custo de medição continua valendo para qualquer log novo). Faça o log
   leve (contadores por quadro) para que medir não distorça; o perfil de CPU sempre sem ele.

## Como medir

- Perfil: `scripts/android_bench/prof_run.py` (benchmark + `simpleperf --app`), **num trecho pesado** (o primeiro perfil caiu num
  trecho leve a 60 fps e não serviu). Compare `re5-main` e `g00457C60` antes e depois, por símbolo.
- Fps: pico e média no mesmo trecho, 3 execuções. DXVK `192b`, 360p, `RE5_LEAN=1`.
- Cada mudança atrás de uma chave `RE5_*`; `ctest` e `scripts/difftest_override.py`/`difftest_cpu.py` verdes.

## Entrega

APK `work/android/app-cpu-<n>.apk` com todas as mudanças ligadas, um plano de teste curto para o usuário (linhas do
`re5_env.txt` para desligar cada uma) e uma entrada no `docs/STATUS.md` com os números **antes/depois por mudança**. Se uma
mudança não render, registre como "sem ganho" e não a deixe ligada.
