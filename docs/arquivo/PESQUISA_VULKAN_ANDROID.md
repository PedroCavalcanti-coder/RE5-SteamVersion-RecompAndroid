# Vulkan no Android: o que outros projetos fazem e o que aplicar aqui (2026-11-07)

Pesquisa pedida pelo usuário: ports e projetos que rodam Direct3D/jogos de PC em Vulkan no
celular (Winlator/Vortek, forks da DXVK, porte Android do Generals Zero Hour, PPSSPP, Dolphin, PanVK)
e as recomendações da Arm para Mali. Cada item diz o estado atual no nosso código e o que fazer.
Nada aqui foi medido no aparelho ainda.

## O que dizem os outros projetos

| Projeto | O que fazem | Fonte |
|---|---|---|
| Winlator + Vortek | Driver Vulkan próprio por cima do driver do sistema (cliente/servidor por anel de memória ashmem). Contorna o que falta no Mali: sem `ClipDistance` no shader, sem BCn (descompacta na CPU), remendo de SPIR-V. | [Vortek, parte 1](https://dev.to/possiblyquestionable/vortek-internals-part-1-command-buffers-3n7h), [parte 2](https://dev.to/possiblyquestionable/vortek-internals-part-2-driver-specific-workarounds-2d8l), [repo](https://github.com/brunodev85/vortek) |
| winlator_mali | Ajustes de DXVK para Mali-G52+; alguns drivers precisam de uma extensão desmarcada para não perder texturas. | [ar37-rs/winlator_mali](https://github.com/ar37-rs/winlator_mali) |
| DXVK-Sarek | DXVK 1.10.x com recursos opcionais para GPUs abaixo do Vulkan 1.3; roda em Mali; a 1.12 corrige tela preta no Mali ("unbound texture optimization"). | [v1.10.7](https://github.com/pythonlover02/dxvk-sarek/releases/tag/v1.10.7), [GamingOnLinux](https://gamingonlinux.com/2026/04/gaming-on-linux-with-an-older-gpu-levels-up-with-dxvk-sarek-v1-12-bringing-major-new-features) |
| Porte Android do Generals ZH | DXVK nativo no Android com o mínimo baixado de Vulkan 1.3 para 1.1 por caminho de recursos adaptativo; roda em Mali-G76/G57. É o exemplo mais próximo do nosso caso (DXVK nativa, sem Wine). | [releases v1.1.3](https://github.com/MYSOREZ/GeneralsZH-Android-Port/releases/tag/v1.1.3), [v1.1.4](https://github.com/MYSOREZ/GeneralsZH-Android-Port/releases/tag/v1.1.4) |
| DXVK-GPLALL | DXVK 2.6 + GPL assíncrono, com build "native"; exige Vulkan 1.3. | [wiki](https://github.com/Digger1955/dxvk-gplall/wiki/Detailed-Changelog) |
| PanVK (Mesa) | Driver aberto para Mali; relatos de DXVK 2.x/3.x de D3D9 funcionando no G57. Precisa de kernel/ROM, não é algo que o app instala. | [panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android/wiki) |
| PPSSPP | Funde passes de render consecutivos no mesmo alvo e reescreve load/store de cada passe pelo uso real; depende de anotar certo `CLEAR`/`DONT_CARE`. | [PR #12462](https://github.com/hrydgard/ppsspp/pull/12462), [issue #12242](https://github.com/hrydgard/ppsspp/issues/12242), [commit 3967d07](https://github.com/hrydgard/ppsspp/commit/3967d074bfdb5ffb19088d17233ce3d7c332aad7) |
| Dolphin / Arm | Primitive restart quebrado no Mali com índice de 16 bits (Dolphin desliga); `LOAD_OP_LOAD` desaconselhado. | [tópico de contornos da Arm](https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/9344/vulkan-driver-issues---workaround-collection-thread) |
| Arm (Mali) | Começar o passe com `CLEAR`/`DONT_CARE`; não gravar o que nenhum passe seguinte lê (a profundidade quase sempre); não quebrar passes à toa; ganho relatado de até ~50% nas escritas externas. | [load/store da Arm](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/picking-the-most-efficient-load-store-operations), [Vulkan Guide: TBR](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html) |
| Google / MediaTek | ADPF + MAGT: sessão de dicas sozinha cortou ~10% do tempo da thread de render; `getThermalHeadroom()` para baixar carga antes do *throttling*. | [história do ADPF com a MediaTek](https://developer.android.com/stories/games/mediatek-adpf?hl=zh-cn) |

## O que aplicar, em ordem

1. **re5vk como caminho principal no celular, com as regras de GPU por tiles.** A DXVK 1.9.2b é
   antiga e decide os passes sozinha; no re5vk somos nós. Estado atual (`vk_draw.c`):
   - Clear no começo do passe já vira `loadOp CLEAR` (`pending_clear`). Bom.
   - Fora isso todo anexo é `LOAD` + `STORE`, e todo fim de passe tem uma barreira
     `ALL_COMMANDS -> ALL_COMMANDS` (`re5vk_barrier_all`). No Mali a barreira total impede o
     vértice do passe seguinte de andar junto com o fragmento do anterior (bolha no pipeline).
   - **Fazer:** (a) barreira só com os estágios reais (saída de cor/teste de profundidade ->
     fragmento/cópia) e só quando o alvo vai ser lido; (b) `STORE_OP_DONT_CARE` na profundidade
     quando o próximo uso dela é um Clear ou nenhum (decidido no fim do quadro, com o histórico do
     quadro anterior, como o PPSSPP faz; não às cegas: uma rodada de outra IA fez isso sem regra e
     foi revertida); (c) `LOAD_OP_DONT_CARE` quando o passe cobre o alvo inteiro e o jogo não lê o
     conteúdo antigo; (d) fundir passes consecutivos no mesmo alvo quando nada amostra o alvo no meio.
   - Medir com `android_perf.py` antes e depois, e com o contador de leitura/escrita externa do
     Mali se o usuário puder rodar o Arm Performance Studio.
2. **DXVK como reserva: trocar a 1.9.2b por uma base mais nova que aceite Vulkan 1.1.** O porte do
   Generals ZH faz exatamente isso (DXVK nativa, mínimo 1.1 com caminho de recursos); a DXVK-Sarek é
   a outra opção (1.10.x, Mali). Ganho esperado: o front-end de D3D9 da DXVK melhorou muito depois
   da 1.9, e a 1.10 já tem ajustes para tiler. Custo: refazer os patches do porte do NFSU2
   (`android/dxvk/prepare.py`).
3. **Texturas (fase A3).** Todos os projetos acima descompactam BCn na CPU no Mali; nós também.
   O passo além é guardar em disco já recompactado (ETC2/ASTC), como sugere o gist de cache de ASTC
   do autor do estudo do Vortek ([gist](https://gist.github.com/leegao)). Só vale se a medida mostrar
   banda de textura: no A54 o fps não mudava com a resolução.
4. **Contornos conhecidos do Mali, conferidos no nosso código:** primitive restart desligado
   (`vkCmdSetPrimitiveRestartEnable(VK_FALSE)`), planos de recorte não aplicados (aviso no log),
   floatEmulation ligada. Nada a fazer agora.
5. **ADPF/térmica (fase A2):** a sessão já existe (`perf_hint.c`). Falta o `getThermalHeadroom()`
   baixando a resolução interna antes do *throttling*, que é o que a MediaTek recomenda.
6. **Fora do nosso alcance:** PanVK (driver aberto do Mali) e governador de GPU da MediaTek
   ([projeto da comunidade](https://seyud.github.io/Mediatek_Mali_GPU_Governor/en/guide/introduction.html))
   precisam de root ou ROM. Não entram no app.

## Próximo passo

Medir o aparelho MediaTek (`scripts/android_perf.py`, agora com clock da GPU em MHz e % do máximo)
com DXVK e com re5vk, na mesma cena. Se a GPU estiver no limite com clock alto, o item 1 é o
caminho; se estiver ociosa ou com clock baixo, o problema é alimentar a GPU (CPU, sincronização),
e o item 1 ajuda menos.
