# Plano de teste curto do backend vk2 (Vulkan 1.1 nativo), A54

Backend novo, do zero no contexto e no envio de memória, com os módulos provados recuperados da tag
`base-lean-2026-11-23` (ver `docs/STATUS.md`, entrada de 24/11/2026). **A DXVK continua o padrão.** O vk2 só liga com
`RE5_GFX=vk2` no `re5_env.txt`.

## Como ligar (celular: só com a sua autorização)

1. Instale `work/android/app-vk2-1.apk` (um APK por vez, `adb install` com caminho do Windows).
2. No `re5_env.txt` do app ponha:
   ```
   RE5_LEAN=1
   RE5_GFX=vk2
   RE5_FPS_LOG=1
   ```
   `RE5_FPS_LOG=1` custa CPU (relógio por chamada). Para o número de fps de verdade, **rode sem ele** e leia o pico/média do
   jogo; use o log só para contar passes e envios.
3. Resolução 360p (`Resolution=640x360`), mesma parte do benchmark, 1 rodada de aquecimento, 3 rodadas medidas.
4. Ao fim, devolva o `re5_env.txt` ao que estava.

## Pré-compilar shaders e apagar o cache

- Configurações > Gráficos: **Renderizador** = vk2; **Compilar shaders** = "Antes de entrar no jogo" (o 1º quadro espera acabar; até 180 s).
  A primeira partida só aprende os shaders; da segunda em diante o log mostra `vk2 pré-compilação: N pipelines (...)` e
  `N pipelines prontos antes do jogo pedir`.
- **Apagar caches de shaders e texturas** (mesma tela) limpa `cache/vk2`, `dxvk` e `mesa`. Sem o botão: `RE5VK2_CACHE_CLEAR=1` no `re5_env.txt`.
- Para ver se funcionou: `RE5_FPS_LOG=1`, linha `vk2 por quadro`: "pipelines criados" tem que parar de subir durante o jogo na 2ª partida.

## O que olhar

- Cabeças sem faixas, texto, sombras, tela de título e cenas de corte (compare com `RE5_GFX=dxvk` no mesmo ponto).
- Log (`RE5_FPS_LOG=1`): linha `vk2 por quadro (...)`: passes/quadro, envios/quadro, draws/passe, pipelines criados, memória de GPU.
- `adb logcat | grep vk2`: GPU escolhida, swapchain, `texturas DXT: ETC2` (o Mali não tem BC).

## Chaves `RE5VK2_*` (cada uma liga/desliga um item; padrão entre parênteses)

| Chave | O que faz |
|---|---|
| `RE5VK2_LAZY_RT` (1) | troca de alvo adiada até o próximo draw; só fecha o passe se o conjunto mudou de fato |
| `RE5VK2_PRESENT_THREAD` (1 no Android) | `vkQueuePresentKHR` numa thread própria |
| `RE5VK2_LATENCY` (2 no Android) | quadros em voo antes do Present esperar a GPU |
| `RE5VK2_PRESENT` (fifo no Android) | `fifo`, `mailbox`, `immediate` |
| `RE5VK2_TBDR` (0) | oráculo de `STORE_OP_DONT_CARE` (já deu lixo antes; só para experimento) |
| `RE5VK2_ETC2_THREADS` (núcleos − 2, até 6) | recodificação DXT→ETC2 em paralelo; 0 = uma thread |
| `RE5VK2_FLOATEMU` / `RE5VK2_NANGUARD` | emulação de float e guarda de NaN do tradutor de shaders (custam ALU; ver relatório de 20/11) |
| `RE5VK2_PCACHE` (`vk2.pcache`) | `VkPipelineCache` em disco; `0` desliga |
| `RE5VK2_GPU` | índice da GPU (PC com mais de uma) |
| `RE5VK2_PASS_TIMING` (0) | timestamp por passe (com `RE5_FPS_LOG`) |
| `RE5VK2_PASS_TRACE=<n>` | loga a sequência de passes de um quadro a cada n |

O que **ainda não** existe como chave porque ainda não foi implementado: pré-aquecimento de pipelines por thread
(princípio 3), conjunto de descritores persistente entre quadros e UBO único por quadro (princípio 4), `StretchRect`
sem fechar o passe (princípio 1). Ver o STATUS.
