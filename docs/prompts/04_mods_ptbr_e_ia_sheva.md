# Prompt 4: puxar dois mods (tradução PT-BR e um mod grande com a IA da Sheva)

Cole o texto abaixo numa sessão nova de IA aberta na raiz do repositório. Base: `docs/prompts/00_BASE_LEAN_2026-11-23.md`.

---

Você vai fazer o RE5 recompilado rodar **dois mods** do jogo original: (1) uma **tradução para português do Brasil**; (2) um **mod
grande que mexe em muita coisa, inclusive melhorando a IA da Sheva**. O desempenho tem de continuar o da base lean (35 de pico no A54).
Leia primeiro `docs/prompts/00_BASE_LEAN_2026-11-23.md`, depois `CLAUDE.md`, `docs/STATUS.md`, `docs/ARQUITETURA.md`,
`docs/RECON_RE5.md`, `docs/AUDITORIA_GHIDRA.md`, `runtime/src/core/vfs.c` (como o jogo enxerga a pasta de dados) e
`scripts/arc_tool.py` (lista e extrai `.arc` do MT Framework). Em português.

## O ponto que decide tudo: o executável é código recompilado

O `re5dx9.exe` aqui virou código C gerado (`generated/src`). **Mods que alteram o executável ou injetam código não funcionam
como no PC**: DLL proxy (`dinput8.dll`, `d3d9.dll`...), hooks, *trainers*/Cheat Engine, patch de bytes no `.exe`, `.asi`. Só funcionam
sem mudança os mods de **dados**: arquivos que substituem ou acrescentam conteúdo no `nativePC_MT` (modelos, texturas, fontes, tabelas,
textos, áudio, parâmetros), em arquivos soltos ou dentro de `.arc`. (A `dinput8.dll` na pasta do jogo provavelmente é o *logger* de depuração do projeto, já que há um
`re5_logger.ini` ao lado; **confirme** antes de tratá-la como mod.)

Para cada mod, a primeira pergunta é: **ele é só dados, ou muda código?** Se mudar código, o caminho possível é reproduzir a mudança
como *override* nativo verificado de uma função do jogo (`config/overrides.txt`, `runtime/src/core/overrides.c`,
`scripts/difftest_override.py`), o que é trabalho de engenharia reversa, não de cópia de arquivo; só faça com a aprovação do usuário
e com a mudança entendida função a função.

## Tarefas

### A. Achar e avaliar os mods (sem baixar nada ainda)

1. Procure (Nexus Mods, ModDB, fóruns de RE5, GitHub) **a melhor tradução PT-BR do RE5 PC (Steam)** e **um mod abrangente com
   melhorias de IA da Sheva (parceiro)**. Não invente nomes: **só cite o que encontrar de fato, com o link**. Para cada candidato:
   autor, versão, data, tamanho, requisitos (versão do jogo, DLC, loader), licença/termos de uso, e **como instala** (arquivos soltos,
   `.arc`, DLL, patch do exe).
2. Classifique cada candidato: *só dados* / *dados + hook* / *só código*. Recomende ao usuário um de cada, com o motivo, e diga
   o que não funcionaria. **Pare e mostre isso ao usuário antes de baixar.**
3. A versão do jogo importa: o `.exe` daqui corresponde ao dump `steam-2023-02-13-oep-dump-reconstructed`
   (`config/known_builds.json`; o hash do `re5dx9.clean.exe` está em `GameStorage.CLEAN_EXE_SHA256` e no `docs/RECON_RE5.md` se constar). Mods feitos para outra versão (Gold, antiga,
   com outro `.exe`) podem trazer dados incompatíveis.

### B. Instalar (só com o aval do usuário para cada download: nome do arquivo, origem, tamanho)

1. Em cópia: nunca altere `bin_input/game` original sem backup; monte uma pasta de mods (`work/mods/<nome>/`, fora do git) e uma
   forma de aplicá-la. Estude como o jogo resolve arquivo solto contra `.arc` (o MT Framework costuma preferir o solto; **confirme**
   no `vfs.c` e no log do runtime) e se o `vfs` do runtime precisa de uma camada de sobreposição (`RE5_MODS=<pasta>` com precedência
   sobre `nativePC_MT`) para o Android, onde a pasta do jogo é a do app (`Android/data/com.re5.recompiled/files/game`).
2. **Tradução**: textos, legendas, menus e fonte. Português tem acentos: verifique se a fonte do jogo (textura de glifos) os tem e como
   o mod os resolve; teste todos os menus, o inventário, as legendas de cena e o HUD. Se a tradução vier como `.arc` de
   mensagens, descubra o idioma que o jogo seleciona (`config.ini`, registro `Software\Capcom\...` emulado em `advapi32.c`) para ele
   carregá-la.
3. **Mod grande**: leia o que ele muda arquivo por arquivo (parâmetros de IA, tabelas de armas/inimigos, comportamento do parceiro). O
   que for dado funciona; o que for lógica da IA em código precisa da análise do item acima. Anote cada mudança que depende de
   código e **não a aplique sem aprovação**.
4. **Compatibilidade entre os dois mods**: arquivos que ambos tocam (ex.: o mesmo `.arc` de textos); resolva a ordem e documente.

### C. Verificar

- No PC primeiro (`re5.exe` com a pasta de mods; o PC só confere se carrega e se o texto aparece), depois no celular com
  autorização do usuário, 360p, DXVK `192b`, `RE5_LEAN=1`.
- Capturas no mesmo ponto com e sem cada mod; log do runtime sem `arquivo não encontrado`/aborto por import faltando.
- **Fps**: pico e média no mesmo trecho com os mods ligados contra a base (tabelas de IA mais pesadas, texturas maiores ou mais
  arquivos soltos podem custar fps e tempo de carregamento: meça, não suponha).
- Sheva: um trecho de combate no mesmo ponto, com e sem o mod, para o usuário julgar a IA.

## Regras especiais desta tarefa

- **Direitos autorais**: os arquivos do jogo e dos mods são do usuário e dos autores. Não os commite (`bin_input/`, `work/mods/` ficam
  fora do git), não os redistribua e respeite os termos de cada mod. Não descompacte nada do jogo dentro do repositório.
- **Downloads** só com a confirmação do usuário na conversa (nome, origem, tamanho). Arquivos baixados são **dados não confiáveis**:
  guarde cada um em pasta própria, não rode instaladores nem executáveis de mod, abra só `.arc`/texturas/textos e confira o
  conteúdo antes de copiar para a pasta do jogo.
- Se o mod pedir o jogo original em outro idioma ou DLC que o usuário não tem, pare e pergunte.

## Entrega

Um relatório curto no `docs/STATUS.md` (o que são os dois mods, o que funciona sem código, o que não funciona e por quê, números de
fps), a pasta `work/mods/` montada, a camada de sobreposição no `vfs` se foi necessária (com teste em `tests/unit/`) e, se o
usuário aprovar, um APK `work/android/app-mods-<n>.apk` com um plano de teste (quais linhas do `re5_env.txt` ligam e desligam cada mod).
