# Plano — Varredura com o Ghidra: o que falta e o que está errado

> Auditoria completa do `re5-recompiled` com duas referências independentes: o
> **Ghidra** como segunda opinião sobre o binário e o **jogo original** como oráculo de
> comportamento. Motivo: o jogo chega aos vídeos, ao menu e ao benchmark, mas as
> barreiras atuais (escada que vira parede, personagem caindo em laço, tela piscando,
> controle do Xbox 360, paradas sem causa) não cedem a correções pontuais. Em vez de
> caçar um sintoma por vez, este plano mede tudo contra as duas referências e
> transforma cada divergência num item com dono.
>
> Etapas `V0`…`V8` (V de varredura), usadas em commits, no `STATUS.md` e nas conversas.
> Plano geral: [`PLANO_DO_PROJETO.md`](PLANO_DO_PROJETO.md). Estado:
> [`STATUS.md`](STATUS.md).

## 0. Regras desta varredura

- **O Ghidra é segunda opinião, não verdade.** Ele também erra (tabelas de salto,
  funções sem retorno, dados lidos como código). Divergência entre o Ghidra e o nosso
  pipeline é uma pergunta: é triada antes de virar semente, exclusão ou correção.
- **Números, não impressões.** Cada etapa termina com contagens em
  `docs/AUDITORIA_GHIDRA.md`; o `STATUS.md` só recebe "FEITO" com evidência.
- **Nada do jogo no git.** Projeto do Ghidra, exportações, despejos e traces ficam em
  `work/` (ignorado). Nos documentos entram endereços, nomes de classes e contagens,
  nunca pseudocódigo nem bytes do jogo.
- **Falhar alto continua valendo (D8).** A varredura serve para achar antes o que hoje
  só aparece como aborto no meio do jogo, não para trocar abortos por stubs.
- Script novo = teste novo em `tests/python/` com dados sintéticos (sem o Ghidra
  instalado, o teste do driver é pulado) e `ctest` verde.

## 1. Ponto de partida (medido em 2026-10-22)

| Item | Valor |
|---|---|
| Ghidra | 12.1.4 em `C:\Users\pedro\Desktop\ghidra_12.1.4_PUBLIC`, JDK 25 (Adoptium) já escolhido. FunctionID com `vsOlder_x86` (Visual Studio 1998–2010: cobre o MSVC 2005 do RE5, linker 8.00), analisadores de RTTI/EH da Microsoft, PDB, Sarif, PyGhidra e emulador de p-code presentes. O headless usa 2 GB de heap por padrão (`GHIDRA_HEADLESS_MAXMEM` muda) |
| Máquina | 12,9 GB de RAM, 30 GB livres no C:, 16 threads |
| Projeto do usuário | `testes com ghidra/re5.gpr`: vazio (nada importado) e **não ignorado pelo git** |
| Entrada do lifter | `bin_input/re5dx9.clean.exe` (SHA-256 `4534003f…`), entry `0x00E68A06`, seções `.text` `.rdata` `.data` `.data1` `.rsrc` `.idata` |
| Catálogo | 72.010 entradas: 56.996 `start` + 15.014 `alias` |
| Lift | 72.009 funções + 1 override (`HasCPUID`); **1 falha de lift**; **1.482 `RECOMP_TODO`** em 31 mnemônicos: `string` 267, `out` 255, `into` 239, `cli` 158, `aaa` 102, `arpl` 77, `in` 67, `aam` 34 … `fpu` 6, `pmulhuw` 3, `psubsb` 1, `x87` 1 |
| Etapas do plano geral nunca feitas | 1.3 (política de imports: `config/iat_overrides.json` vazio), 1.4 (RTTI e vtables: nenhuma semente), 1.5 (classificação CRT/Havok/zlib/jogo) |
| Flags do código gerado | `-O1 -fwrapv -fno-strict-aliasing`; falta `-ffp-contract=off` (inócuo no x86-64 de hoje, errado no ARM64) |

**O original roda o mesmo código.** O `re5dx9.exe` da pasta do jogo não é nenhum dos
três arquivos de `config/known_builds.json` (SHA-256 `30dcf9a9…`), mas o que ele põe na
memória é o código do `clean.exe`. Despejos do jogo rodando
(`bin_input/game/re5_dump_01.bin` e `_03.bin`, base `0x00400000`) comparados seção a
seção:

| Seção | Bytes diferentes | Leitura |
|---|---:|---|
| `.text` | **0** | código idêntico |
| `.rdata` | 1.049, só em `0x00F4A000..0x00F4A47B` | a IAT: endereços reais × thunks |
| `.data` | ~324 mil em ~18 mil faixas | estado do jogo em execução (esperado) |
| `.data1`, `.rsrc` | 0 | idênticas |
| `.idata` (`0x0137F000`) | toda | no original é a seção do protetor; no `clean.exe`, a tabela de imports reconstruída |

Então toda diferença de comportamento entre o original e o `re5.exe` vem do lift, do
compilador, do runtime/HLE ou do ambiente, nunca de código diferente. A última linha
vira um item de auditoria: nenhuma instrução do `.text` pode depender da faixa
`0x0137F000..0x01CFF000` (V1, `outside.json`).

## 2. O jogo original como oráculo

- **Como abre:** pela pasta `bin_input/game`, com o `Launch.exe`, que inicia o jogo sem
  abrir a Steam. O `re5dx9.exe` dessa pasta aberto direto abre a Steam: não usar. A
  preparação dessa pasta é do usuário e fica fora do repositório.
- **Quem roda:** o usuário. A IA não executa os executáveis da pasta do jogo (são de
  terceiros); ela prepara o `re5_logger.ini` de cada roteiro, diz o que capturar e
  analisa a saída.
- **O que já existe:** o `re5_logger` (proxy de `dinput8.dll`, ferramenta local, fora do
  git) grava as APIs por categoria com quem chamou
  (`[CALLER: 0x… (re5dx9.exe+0x…)]`) e o total lido de cada arquivo no `CloseHandle`;
  F11 grava um despejo da imagem com o mapa de memória.
- **Diferenças de ambiente que contam:** o original vê o CPUID real do Ryzen 7 5700U
  (SSE3 até AVX2), 16 processadores lógicos, a versão do Windows que o
  `GetVersionExA` lhe dá e a configuração salva na pasta Documentos de verdade. O
  `re5.exe` responde Core 2 com SSE2 e sem SSE3, 8 núcleos, Windows 7 SP1 e o perfil
  `C:\Users\Player` do VFS. Se o jogo escolhe caminhos de código por isso, "o original
  passa na escada" compara caminhos diferentes. A V4.4 mede e oferece paridade.

## 3. Etapas

### V0 — Preparação (IA)

- `.gitignore`: `testes com ghidra/`, `*.gpr`, `*.rep/`. Um projeto do Ghidra contém o
  binário inteiro.
- Projeto oficial em `work/ghidra/re5.gpr`. O usuário pode abri-lo na interface quando
  nenhuma análise headless estiver rodando (o projeto tem trava).
- `scripts/ghidra_sweep.py`: acha o Ghidra (`RE5_GHIDRA_DIR`, senão procura), confere o
  SHA-256 da entrada contra `config/known_builds.json`, roda o `analyzeHeadless` com
  `GHIDRA_HEADLESS_MAXMEM=6G`, `-scriptPath scripts/ghidra;tools/pcrecomp/tools/ghidra`,
  a pré-configuração e as exportações. Importa só na primeira vez; depois `-process`
  reaproveita a análise salva.
- `scripts/ghidra/ConfigureAnalysis.java` fixa as opções, para a análise ser
  reproduzível. Ligados: RTTI e EH da Microsoft, FunctionID, tabelas de salto do
  decompilador, funções sem retorno, demangler, tipos do Windows. Desligados na 1ª
  passada: Aggressive Instruction Finder e Decompiler Parameter ID (lentos e cheios de
  falsos positivos).
- `scripts/doctor.py` passa a dizer se o Ghidra e o JDK existem (componente opcional).

**Pronto quando:** análise completa salva (estimativa 1–3 h; medir) e números do
`GhidraStats.java` registrados: funções, classes RTTI, funções reconhecidas pelo FID.

### V1 — Exportação do Ghidra (IA)

Scripts Java em `scripts/ghidra/` (como os do pcrecomp), saída JSON em
`work/ghidra/export/`, esquema descrito no cabeçalho de cada script:

| Arquivo | Conteúdo | Usado em |
|---|---|---|
| `funcs.json` | entrada, corpo (faixas), nome e origem (FID/RTTI/demangle/padrão), thunk, sem retorno, convenção, `ret N`, nº de chamadores | V2, nomes nos logs |
| `switches.json` | cada salto computado: local, tabela, alvos, nº de casos; chamadas computadas | V2 |
| `code_ptrs.json` | toda referência de dados para código (origem, alvo, seção, se é slot de vtable) | V2 |
| `rtti.json` | classes, vtables, métodos, bases; conferido contra `tools/pcrecomp/tools/cpp/rtti.py` | V2, V6.2 |
| `instr.json` | censo de formas de instrução (mnemônico + tipos e tamanhos dos operandos + prefixos) dentro de funções; x87/SSE/MMX por função | V3 |
| `imports.json` | por import: locais de chamada, inclusive com endereço tomado (`mov esi,[IAT]` … `call esi`), e a função de cada um | V4.1 |
| `outside.json` | constantes e referências fora das seções do `clean.exe`: faixa do protetor, `0x7FFE0000` (`KUSER_SHARED_DATA`), endereços baixos fixos | V4.5 |
| `fs_refs.json` | acessos `fs:[n]` por deslocamento | V4.5 |
| `text_writes.json` | escritas em `.text`/`.rdata` (código automodificável, constantes escritas) | V3 |
| `eh.json` | quadros SEH (`_SEH_prolog4`, `_except_handler4`), C++ EH (`__CxxFrameHandler3`, blocos try/catch), quem chama `_CxxThrowException` e `RaiseException` | V4.3 |
| `cpuid.json` | cada `cpuid`, a folha pedida e os bits testados depois | V4.4 |
| `strings.json` | strings com referências | nomes de subsistemas, caminhos, chaves de registro |

Sob demanda: `DecompAddrs.java` e `FindRefs.java` (pcrecomp) para funções específicas,
com saída só em `work/`.

**Pronto quando:** as exportações saem do projeto analisado em minutos, e
`work/names.json` (FID + RTTI + demangle, por `tools/pcrecomp/tools/pe/merge_names.py`)
alimenta o runtime: trilha de dispatch, abortos e o `RE5_PROFILE` mostram o nome da
função ao lado do VA.

### V2 — Catálogo: o que falta (IA). Cumpre a 1.4 e fecha a 2.6

`scripts/audit_catalog.py`, sobre o `tools/pcrecomp/tools/disasm/score_recovery.py`,
compara `work/catalog.json` com o Ghidra:

1. **Inícios:** funções do Ghidra fora do catálogo (cada uma é um aborto futuro no meio
   do jogo); entradas nossas fora de qualquer função do Ghidra (dado lido como código);
   funções divididas.
2. **Fins**, com o enchimento `0xCC`/`0x90` aparado: curtas (cauda não traduzida) e
   longas.
3. **Saltos computados:** tabelas que o Ghidra resolveu e o `disasm32` não, ou resolveu
   com menos alvos.
4. **Alvos indiretos:** todo método de vtable (RTTI e `vtable_scan.py`) e todo ponteiro de
   dados para código precisa estar no catálogo.
5. **Funções sem retorno:** onde um lado continua decodificando depois de uma chamada
   que não volta e o outro não.
6. **Aliases** (`entry_kind: alias`): têm de cair dentro de uma função conhecida.

Triagem: com referência → semente (`run_lift.py catalog --add-seeds`); sem referência →
lista para olhar; dado → `config/exclude.txt` com o motivo.

**Pronto quando:** 0 funções do Ghidra com referência fora do catálogo (ou cada uma
justificada), 0 tabelas de salto com alvo faltando, 0 métodos de vtable fora, e o
benchmark e o capítulo 1-1 rodam sem nenhum "dispatch sem função gerada".

### V3 — Lift: o que está traduzido errado (IA). Cumpre a 1.5

- **V3.1 `RECOMP_TODO` e a falha de lift.** Cada um dos 1.482 sites classificado contra
  o Ghidra: dentro de função real (lacuna do lifter → implementar com teste) ou dado
  (→ `exclude.txt`). Suspeitos de serem código real: `string` (267), `fpu` (6), `x87`
  (1), `pmulhuw` (3), `psubsb` (1).
- **V3.2 Cobertura do teste diferencial.** Cruzar o `instr.json` com as 1.246
  codificações do `difftest_cpu.py`; toda forma que aparece em código real e não foi
  testada entra no difftest. Meta: 100% das formas do código real.
- **V3.3 Onde o modelo (D9) difere do hardware.** Contagem e subsistema de cada grupo:
  `fld`/`fstp` de 80 bits, `fbld`/`fbstp`, mudanças do campo de precisão do x87;
  `rcpss`/`rsqrtss`/`rcpps`/`rsqrtps` (o lifter dá o valor exato, o hardware dá uma
  aproximação de 12 bits que difere entre AMD e Intel, e o Unicorn também calcula o
  exato, então o difftest não vê); `pushfd`/`popfd` (bits ID e AC; hoje só o `HasCPUID`
  é tratado); `bsf`/`bsr` com fonte zero; flags indefinidas que alguém lê; float→int
  fora da faixa; NaN em `minss`/`maxss`/`comiss`; denormais. Uma decisão por grupo:
  manter, corrigir no lifter ou override.
- **V3.4 C emitido e compilador.** Revisar o que o `lift32_cpu` emite com semântica
  indefinida em C ou dependente do host (float→int fora da faixa, deslocamento ≥
  largura, comparação com NaN). Acrescentar `-ffp-contract=off` ao código gerado e ao
  runtime já: no ARM64 o compilador funde `a*b+c` em FMA e a física muda (o porte do
  NFSU2 já usa a flag).
- **V3.5 Diferencial por função em massa.** `difftest_func.py` (Unicorn × C gerado, com a
  imagem mapeada) em todas as funções folha do Ghidra com ponto flutuante; depois nas do
  subsistema de colisão (V6).

**Pronto quando:** 0 `RECOMP_TODO` em código real, 0 falhas de lift, 100% das formas de
instrução do código real com difftest e a tabela da V3.3 com uma decisão por linha.

### V4 — Runtime e HLE: o que responde diferente do Windows (IA). Cumpre a 1.3

- **V4.1 Matriz de imports.** Para cada um dos 270: locais de chamada, o que o jogo lê do
  retorno e dos parâmetros de saída (pelos locais de chamada decompilados) e o que o
  handler preenche. Vai para `config/iat_overrides.json` (política `hle`/`const`/`bridge`
  com motivo), mais a lista dos handlers incompletos (já conhecidos: fuso em UTC,
  `TlsFree`, proteção por região e não por página).
- **V4.2 Métodos COM e Steam usados de verdade.** Com os tipos dos headers (d3d9, dsound,
  dinput, wmsdk, Steamworks 1.30) aplicados aos ponteiros de interface, listar os
  deslocamentos de vtable chamados em cada interface e cruzar com o que a ponte
  implementa. "Chamado e sem handler" é um aborto marcado para o futuro (por exemplo
  `ISteamUserStats` no fim de um capítulo, `ISteamRemoteStorage` nos saves): entra antes
  de o usuário chegar lá.
- **V4.3 Exceções.** Quantos `throw`, `try/catch` e `__try/__except` existem, de quem
  (CRT, jogo, bibliotecas) e se algum está no fluxo normal. Define com números o escopo
  da etapa 3.6 (portar o `eh32` do pcrecomp).
- **V4.4 Paridade de ambiente com o oráculo.** Cada `cpuid` e o que o jogo faz com os
  bits (há caminhos SSE3/SSSE3/SSE4? o `instr.json` responde); `GetSystemInfo` (8 × 16);
  `GetVersionExA` (7 SP1 × o que o original vê); `QueryPerformanceFrequency` e `rdtsc`
  (3 GHz fixos) e onde o jogo converte ticks em segundos; fuso; idioma; e a configuração
  que o jogo lê com `GetPrivateProfileStringA` (a do original × a do perfil do VFS:
  modo de quadros, resolução, qualidade). Resultado: um perfil "igual ao original nesta
  máquina" no runtime, ligável por variável, com os valores gravados pelo logger (V5.2).
- **V4.5 Memória especial.** Campos do TEB/PEB que o jogo lê × os que o runtime
  preenche; `KUSER_SHARED_DATA`; qualquer referência à faixa do protetor.
- **V4.6 Threads e ordem de memória.** Pontos de entrada do `CreateThread` (com nome),
  esperas em laço sobre memória comum (`Sleep(0)`, flags), publicações sem
  `Interlocked`: os candidatos a quebrar no modo `hle`, em que o código do guest roda em
  paralelo e o compilador C pode reordenar escritas que o x86 (TSO) nunca reordena.

**Pronto quando:** matriz completa e versionada, lista de "chamado sem handler" vazia ou
com ordem de implementação, perfil de paridade disponível.

### V5 — Oráculo dinâmico: o original × o `re5.exe` (usuário + IA)

- **V5.1 Roteiros fixos:** S1 abrir até a tela de título sem tocar em nada; S2 menu →
  benchmark fixo; S3 benchmark variável; S4 capítulo 1-1 até a escada; S5 controle do
  Xbox 360 no menu e andando.
- **V5.2 Captura no original (usuário, pelo `Launch.exe`):** `re5_logger.ini` por roteiro
  preparado pela IA (arquivos, memória, threads, módulos, janela, DirectX, áudio; entrada
  só no S5) e despejo F11 nos pontos marcados. Extensões do logger (ferramenta local):
  despejo por gatilho (número de `Present` ou tempo) em vez de tecla, e uma categoria
  "sistema" com o que o jogo vê (versão, núcleos, `QueryPerformanceFrequency`, CPUID).
- **V5.3 Captura no `re5.exe` (IA):** trace no mesmo formato (chamador, argumentos,
  retorno) e despejo da memória do guest no mesmo formato do logger
  (`RE5_GUEST_DUMP=<Present N>`).
- **V5.4 Comparadores (`scripts/oracle_diff.py`):**
  - **arquivos:** cada arquivo aberto, se abriu e quantos bytes foram lidos. Um arquivo do
    cenário que não abre (caminho, caixa, VFS) explicaria uma colisão errada sozinho;
  - **sequência de APIs** por papel de thread (principal, arquivos, som, workers),
    alinhada pelo endereço de quem chamou (com o nome da V1): primeira divergência;
  - **`.data` em pontos equivalentes:** o que o jogo calcula na inicialização (tabelas,
    flags de CPU, constantes de tempo) tem de ser igual; o resto é classificado pela
    função que escreve (`FindRefs.java --writes`).

**Pronto quando:** S1–S3 comparados, cada divergência explicada ou corrigida; S4 e S5
capturados pelo usuário.

### V6 — Barreira principal: colisão e física (IA + usuário)

Sintomas (2026-10-22), todos de consulta ao mundo de colisão:

| # | Sintoma | Onde |
|---|---|---|
| S-a | a escada vira parede invisível (sobe um degrau e volta) | capítulo 1-1 e benchmark variável |
| S-b | o personagem afunda no chão e é jogado para fora, em ciclo; no benchmark não passa de um container | benchmark |
| S-c | a câmera perde a colisão em certos momentos e atravessa **toda** parede | jogo |
| S-d | os itens de barril e caixa quebrados não aparecem | jogo |

**Pista nova (usuário, 2026-10-22):** a colisão da câmera funciona em **portas** e falha em
**paredes**. No despejo do Havok, portas e caixas são corpos dinâmicos (`hkpBoxMotion`) e
paredes, chão e escadas são corpos fixos (`hkpFixedRigidMotion`: 294 de formas convexas e 8
malhas MOPP). Todos os sintomas (chão, escada, câmera, ponto de queda do item) envolvem corpos
**fixos**: a hipótese passa a ser consulta (raio/varredura) contra corpos fixos do Havok falhando
(broadphase da camada fixa, `hkpFixedRigidMotion`, filtro de colisão), e não a malha em si (o despejo
tem as malhas, com o chão em Y=0,0 e 0,5). Teste: raios do jogo contra um corpo fixo e um dinâmico
da mesma cena, comparando com a força bruta (`scripts/hk_export.py` já dá a verdade em Python).

Leitura: chão, degrau, câmera e o ponto de queda do item (um raio até o chão) usam o
mesmo mundo de colisão estático. Uma causa única, que faça as consultas (raio, varredura
de esfera/cápsula, contato) devolverem resultado errado ou nenhum, explica os quatro; por
isso o item S-c "toda parede, em alguns momentos" é o mais informativo: é intermitente.
Intermitente e global pesa para dado de colisão que não está (ainda) carregado ou
corrida entre threads, mais do que para uma instrução traduzida errada (que erraria
sempre no mesmo lugar). S-d também separa por arquivo: o item vem de um recurso
carregado de forma assíncrona na hora da quebra, então o `oracle_diff` de arquivos (V5.4)
no S4 mostra se algum arquivo deixa de ser aberto ou lido.

Já descartados com medida: tradução das instruções isoladas, a libm do CRT, fronteiras
do catálogo, precisão do x87, **passo de tempo/fps** (E1: o original a ~20 fps passa
normalmente na escada e no benchmark; e o `re5.exe` a ~47 fps, com a DXVK 2.6.2,
continua com os quatro defeitos).

- **V6.1 Bissecção por experimentos baratos**, antes de qualquer leitura de código; cada
  um corta uma classe inteira de causas:

  | # | Experimento | Quem | Se o defeito… | …a causa está em |
  |---|---|---|---|---|
  | E1 | original com o fps limitado a ~20 (RTSS), na escada e no benchmark variável | usuário | aparece | **FEITO: não apareceu; passo de tempo descartado** |
  | E2 | `re5.exe` com `RE5_GUEST_LOCK=global` | IA (benchmark) + usuário (escada) | some | threads/ordem de memória (V4.6) |
  | E3 | `re5.exe` com o perfil de paridade (V4.4) | IA + usuário | some | caminho de código ou configuração diferente |
  | E4 | janelas da física (V6.2) em `-O0` | IA | some | compilador / C indefinido (V3.4) |
  | E5 | `oracle_diff` de arquivos no S4 | IA | mostra arquivo faltando | VFS / HLE de arquivos |

  O benchmark variável roda sem jogador (`scripts/bench_re5.py`), então E2–E4 rodam nele
  pela IA, conferidos por capturas de quadro (`RE5_D3D9_SHOT`, a cada 400 `Present`,
  folha de contato conferida por olho); a escada do capítulo 1-1 precisa do usuário.
  Com o E1 resolvido, o E4 (`-O0`) pode rodar: o fps baixo não muda o defeito.
- **Experimentos acrescentados pelos sintomas novos:**

  | # | Experimento | Quem | Lê-se |
  |---|---|---|---|
  | E6 | S-d: no S4 do original e do `re5.exe`, comparar arquivos abertos/lidos (`oracle_diff`) | usuário captura, IA compara | arquivo que o original lê e o `re5.exe` não: HLE de arquivos/VFS (`ReadFileEx` + APC, leitura parcial, caixa do caminho) |
  | E7 | S-c: no `re5.exe`, registrar leituras de arquivo (`RE5_LOG=trace`, categoria arquivos) até o primeiro momento sem colisão de câmera | IA + usuário | colisão que chega depois de a cena já estar jogável: carregamento assíncrono incompleto |
  | E8 | `RE5_GUEST_LOCK=global` com `GetSystemInfo` = 2 núcleos (como o D5 original) | IA + usuário | defeito some com menos paralelismo: corrida nos workers da MT Framework |
- **V6.2 Mapa do subsistema** pelas classes RTTI e strings da V1: colisão da MT
  Framework, Havok (o jogo traz `serial.hkx`), controlador do personagem (chão, degrau,
  queda) e os pontos de entrada chamados por quadro.
- **V6.3 Diferencial por instantâneo (ferramenta nova).** O runtime grava, na entrada de
  funções escolhidas por VA, a CPU do guest e a memória que a função toca, e de novo na
  saída; o `difftest_func.py` reexecuta o x86 original daquela chamada no Unicorn a
  partir do instantâneo e compara escritas e retorno. Rodado no laço de queda do
  benchmark, aponta a primeira função cujo resultado difere do x86, mesmo que não seja
  folha (o Unicorn executa as chamadas internas com a imagem mapeada).

**Pronto quando:** causa raiz com evidência, correção com teste, e o usuário passa pela
escada e pelo container.

### V7 — As outras barreiras (IA + usuário)

| Sintoma | O que a varredura faz |
|---|---|
| Tela pisca às vezes | V4.2 no D3D9: flags de `Lock` usadas (`DISCARD`/`NOOVERWRITE`) por tipo de buffer, a lacuna conhecida da sombra de vertex buffer devolvida por `GetStreamSource`, consultas (`IDirect3DQuery9`); capturas `RE5_D3D9_SHOT` em volta do piscar; sequência D3D9 do original (categoria DirectX do logger) × a do `re5.exe` |
| Controle do Xbox 360 | o que o jogo lê do `XINPUT_STATE` (botões, gatilhos, zona morta) e como pede vibração × o HLE; S5 com `LogInput=1` no original × trace do `re5.exe` |
| `ExitProcess(0)` sem causa (rodadas em `-O2`) | quem chama `ExitProcess`/`exit` (V1) e trilha com nomes na próxima ocorrência; V3.4 |
| Acesso inválido em `0x18060014` (modo `hle`, uma vez) | V4.6; repetir com o watchdog e a trilha com nomes |

### V8 — Consolidação (IA)

- `docs/AUDITORIA_GHIDRA.md` com os números de cada etapa (só contagens, endereços e
  nomes de classes), atualizado a cada rodada.
- `STATUS.md` com a evidência de cada etapa; no `PLANO_DO_PROJETO.md`, 1.3, 1.4 e 1.5
  passam a apontar para V4.1, V2 e V3.
- Testes: `tests/python/test_audit_catalog.py`, `test_oracle_diff.py` etc. com JSON
  sintético; `ctest` verde em todo push.
- Repetir é barato: as exportações ficam em cache; mudou o catálogo ou o lifter, só
  V2/V3 rodam de novo (minutos).

## 4. Ordem e paralelismo

```
V0 ──► V1 ──► V2 ──► V3 ──► V4 ──────────────► V8 (contínuo)
│      (análise do Ghidra: 1–3 h em segundo plano)
├──► E1 (usuário) e E2 (IA): começam já, sem esperar o Ghidra
└──► V5.2: capturas S1–S3 do original (usuário), assim que os .ini estiverem prontos
               V5.3/V5.4 depois da V1 (nomes) ──► V6.2/V6.3 ──► V7
```

1. **Já:** V0 e a análise do Ghidra em segundo plano; E1 (usuário) e E2 (IA), os dois
   experimentos mais baratos e que mais cortam hipóteses.
2. V1 → V2 → V3.1: fecham o catálogo e os `RECOMP_TODO` reais (menos abortos já).
3. V4 (estático) e V5 (dinâmico) em paralelo.
4. V6 com tudo o que veio antes; V7; V8 o tempo todo.

## 5. Barreiras × hipóteses × onde se testa

| Barreira | Hipóteses vivas | Etapa |
|---|---|---|
| Escada vira parede, afunda no chão em ciclo, container, câmera atravessa paredes, itens de barril/caixa não aparecem | (passo de tempo: descartado pelo E1) ordem de memória no modo `hle`; caminho de código por CPUID/núcleos ou configuração diferente; arquivo/colisão do cenário não lido ou carregado tarde; função traduzida errado em contexto real; C indefinido ou otimização | E2 + E8 + V4.6; E3 + V4.4; E5/E6/E7 + V5.4; V6.3; E4 + V3.4 |
| Tela pisca | ponte D3D9 (sombra de buffer, flags de `Lock`, consultas) | V7, V4.2 |
| Controle do Xbox 360 | leitura do `XINPUT_STATE`, zona morta, vibração | V7, V5 (S5) |
| Abortos ao avançar no jogo | funções fora do catálogo; métodos COM/Steam sem handler; exceções | V2, V4.2, V4.3 |
| Paradas sem causa | C indefinido, threads | V3.4, V4.6, V7 |

## 6. Saídas e onde ficam

| O quê | Onde | No git |
|---|---|---|
| Projeto do Ghidra, exportações, auditorias, traces, despejos | `work/ghidra/`, `work/audit/`, `work/oracle/` | não |
| Scripts do Ghidra (Java) e o driver | `scripts/ghidra/`, `scripts/ghidra_sweep.py` | sim |
| Comparadores | `scripts/audit_catalog.py`, `scripts/oracle_diff.py` | sim |
| Despejo e instantâneo do guest | `runtime/` (`RE5_GUEST_DUMP`, instantâneo por VA) | sim, com teste |
| Sementes e exclusões | `work/seeds_ghidra.txt` → catálogo; `config/exclude.txt` | só as exclusões |
| Política de imports | `config/iat_overrides.json` | sim |
| Números | `docs/AUDITORIA_GHIDRA.md`, `docs/STATUS.md` | sim |
| Mudanças no `re5_logger` | `tools/re5_logger/` (local) | não |

## 7. Riscos e limites

- O Ghidra erra: divergência é pergunta, não resposta.
- RAM: 12,9 GB não comportam o Ghidra (6 GB) e o build `-j16` ao mesmo tempo.
- Trava do projeto: interface aberta bloqueia o headless, e vice-versa.
- Nem o original nem o `re5.exe` são determinísticos (tempo, threads, IA dos inimigos):
  a comparação é por eventos e por pontos de sincronização, não quadro a quadro. Um
  modo de tempo fixo dos dois lados (relógio virtual no runtime; no logger, os relógios
  do Windows) fica como recurso se a V5 precisar.
- Remoção de proteção está fora deste plano e do repositório: a varredura parte do
  `clean.exe` que já existe e do original como o usuário já o roda.

## 8. O que preciso de você

1. **E1:** rodar o original pelo `Launch.exe` com o RTSS limitando a ~20 fps, na escada do
   capítulo 1-1 e no benchmark variável. O defeito aparece?
2. Quando a IA entregar o `re5_logger.ini` de cada roteiro: capturar S1–S3 (depois S4 e
   S5) e deixar a saída em `bin_input/game`.
3. Opcional: explorar `work/ghidra/re5.gpr` na interface do Ghidra; nomes e comentários
   que você puser lá entram nos logs pela exportação de nomes (V1).
