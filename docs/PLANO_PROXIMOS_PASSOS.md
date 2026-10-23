# Próximos passos — colisão, escada e itens (revisão de 2026-10-22, noite)

> Escrito depois de um dia de varredura (ver [`PLANO_VARREDURA_GHIDRA.md`](PLANO_VARREDURA_GHIDRA.md) e
> [`AUDITORIA_GHIDRA.md`](AUDITORIA_GHIDRA.md)). Objetivo: parar de tentar às cegas e decidir com
> poucos testes onde o defeito está, antes de escrever qualquer código.

## 1. O que já está provado (com medida)

| Suspeito | Resultado | Como foi medido |
|---|---|---|
| Tradução das instruções | correta | difftest: 7.174/7.174 casos iguais ao hardware |
| Tradução do movimento e do Havok **no quadro travado** | correta | captura F11 + replay no Unicorn: 0 bytes diferentes em 6 chamadas (`0x00B72FA0`, `0x009DF110`, `0x00CFC5E0`, `hkpCharacterProxy::integrate`) |
| Dados de colisão | carregados e intactos | SBC1 do s115 inteiro na memória; Havok do estágio presente |
| Tempo do quadro (`dt`) | correto | `dt` passado ao movimento = tempo real |
| Catálogo de funções | completo | auditoria V2: 0 funções, switches ou vtables faltando |
| Núcleos | corrigido | `config.ini` tinha `JobThread=2` |

E do jogo original (usuário): **abaixo de ~5 fps o Chris também levita** no original. Portanto o
comportamento a fps muito baixo é do próprio jogo e não serve de pista. A diferença real é só esta:
**a 20 fps e acima, o original sobe aquela escada e o recompilado não.**

## 2. Conclusão sobre "refazer renderização, CPU e física"

Não recomendo. Motivos:

- A física é o **Havok 5.5 compilado dentro do executável** e o movimento é código da Capcom: não
  existe fonte para "refazer"; reescrever seria reimplementar Havok + MT Framework às cegas, com
  muito mais chance de criar defeitos do que de corrigir. E as capturas mostram que o código
  traduzido faz exatamente o que o x86 faz no quadro travado.
- A renderização funciona (o jogo desenha certo); não tem relação com colisão.
- O que falta não é código novo nem modelo maior: é **uma medida que separe as hipóteses que
  sobraram**. O modo de raciocínio do modelo não muda isso.

O que um quadro calculado certo pode ainda produzir errado ao longo do tempo:

| Hipótese | Por quê seria | Como separar |
|---|---|---|
| H1 corrida entre threads | jobs de física e o personagem rodam em paralelo nos workers; a ordem pode diferir do original | `JobThread=1` e `RE5_GUEST_LOCK=global` |
| H2 entrada do jogador diferente | teclado/controle chegam ao jogo com outro valor (andar x correr, intensidade do analógico); RE5 pode só subir degrau correndo/com certa velocidade | testar a escada correndo e andando, teclado e controle; registrar o que o jogo lê |
| H3 estado de quadros anteriores | algo calculado antes (animação, flag de "em escada") chega diferente | replay de um quadro inteiro (seção 3, passo P3) |
| H4 resposta do HLE diferente | um import devolve algo diferente do Windows (tempo, entrada, arquivos) e muda o caminho | comparar a sequência de imports com o logger do original |

## 3. Plano (nesta ordem; cada passo tem critério de parada)

**P1 — Três testes rápidos, sem código (usuário, ~30 min).** Na escada do s115:
1. Recompilado com `JobThread=1` no `config.ini` (e depois de volta a 8).
2. Recompilado com `RE5_GUEST_LOCK=global` (linha do `.bat`).
3. Recompilado e original, cada um **andando e correndo**, com **teclado e com controle**.
4. Original a ~45 fps (o fps normal do recompilado) para confirmar que ele sobe.

Se 1 ou 2 resolver → é H1: o trabalho passa a ser a sincronização (P4a). Se só falhar andando, ou
só com um dispositivo → é H2 (P4b). Se nada mudar → P2/P3.

> **Resultado do P1 (usuário, 2026-10-22):** o recompilado não sobe a escada em nenhum caso
> (`JobThread=1`, lock global, andando, correndo, teclado, controle). H1 (corrida entre threads) e H2
> (entrada) ficam improváveis; o próximo passo é o P3.

**P2 — Entrada (IA, se P1 apontar H2 ou nada).** Registrar por quadro o que o jogo lê do teclado/
controle (`XInputGetState`, `GetDeviceState`) no recompilado e no original (`re5_logger` com
`LogInput=1`, mesmo trajeto). Diferença de valor/intensidade → corrigir o HLE de entrada.

**P3 — Replay de um quadro inteiro (IA).** Com `JobThread=1` e lock global (tudo numa thread), capturar
a função de atualização do quadro (o laço principal do MT Framework) no ponto travado e reexecutar
no Unicorn. Igual → o defeito não está no código do quadro (volta a H2/H4). Diferente → bissecção
com os tracepoints até a função culpada, e a correção vai no lifter ou no runtime, com teste.

**P4 — Correção dirigida.**
- a) Corrida: achar a sincronização que o original garante e o runtime não (eventos, seções críticas,
  ordem de despertar dos workers) e corrigir no `kernel32_sync`; medir no benchmark.
- b) Entrada: corrigir o HLE de teclado/controle.
- c) Só se tudo acima falhar: override nativo pontual da lógica de degrau/chão (uma função, com
  teste), nunca reescrita de subsistema.

**Itens sorteados (paralelo, mesma ferramenta).** Com F8/F9 (cobertura) quebrar uma caixa com item
fixo e uma com item sorteado; a diferença dá as funções do sorteio (`sItem::ITEM_DROP_*`,
`rLotSelector`). Capturar essas funções com F11 e reexecutar no Unicorn; conferir o gerador
aleatório e se o `rLotSelector` do estágio (XFS) foi lido.

## 4. O que não fazer

- Não reescrever física, renderização ou "CPU" (o lifter): todas as medidas dizem que estão certos
  nos pontos testados.
- Não testar abaixo de ~10 fps: o original também quebra ali.
- Não mudar mais de uma variável por teste.
