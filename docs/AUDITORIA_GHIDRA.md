# Auditoria com o Ghidra — re5-recompiled

> Registro quantitativo e evidências da varredura comparativa do `re5-recompiled`
> contra o Ghidra e o jogo original.
> Plano completo: [`PLANO_VARREDURA_GHIDRA.md`](PLANO_VARREDURA_GHIDRA.md).

---

## 1. Ponto de Partida (2026-10-22)

| Métrica / Parâmetro | Valor |
|---|---|
| Ghidra | 12.1.4 (`C:\Users\pedro\Desktop\ghidra_12.1.4_PUBLIC`) |
| JDK | OpenJDK 25.0.4.1 (Adoptium Hotspot 64-bit) |
| Heap Headless (`GHIDRA_HEADLESS_MAXMEM`) | 6G |
| Binário de entrada | `bin_input/re5dx9.clean.exe` (16.257.024 bytes) |
| SHA-256 da entrada | `4534003fe7949750c8c2bffd337f9d735eb9d27498a66e77e77cf1a87dfa4036` |
| Build correspondente | `steam-2023-02-13-oep-dump-reconstructed` (liftable: true) |
| Catálogo atual | 72.010 entradas (56.996 `start` + 15.014 `alias`) |
| Funções compiladas no lift | 72.009 funções + 1 override (`HasCPUID`) |
| Falhas de lift | 1 |
| Ocorrências de `RECOMP_TODO` | 1.482 em 31 mnemônicos |

---

## 2. V0 — Preparação e Ambiente

- [x] **`.gitignore` atualizado**: adicionadas regras para `testes com ghidra/`, `*.gpr` e `*.rep/`.
- [x] **Configuração reproduzível**: [`ConfigureAnalysis.java`](../scripts/ghidra/ConfigureAnalysis.java) implementado.
  - *Ativados*: RTTI e Exception Handling da Microsoft, Function ID, Decompiler Switch Analysis, Non-Returning Functions (Discovered/Known), Demangler Microsoft, Apply Data Archives, Windows x86 TEB Analyzer.
  - *Desativados (1ª passada)*: Aggressive Instruction Finder e Decompiler Parameter ID.
- [x] **Estatísticas de análise**: [`GhidraStats.java`](../scripts/ghidra/GhidraStats.java) implementado com contagem de funções (totais, nomeadas, `FUN_`, thunks, externas), classes RTTI e matches do Function ID.
- [x] **Driver de automação**: [`ghidra_sweep.py`](../scripts/ghidra_sweep.py) implementado, com validação de hash, heap 6G, importação única e `-process` incremental.
- [x] **Diagnóstico**: [`doctor.py`](../scripts/doctor.py) atualizado para detectar Ghidra e JDK.
- [x] **Testes automatizados**: [`test_ghidra_sweep.py`](../tests/python/test_ghidra_sweep.py) integrado à suíte de testes (`ctest` 100% aprovado).
- [x] **Projeto salvo**: `work/ghidra/re5.gpr` (concluído em 508 s / ~8,5 min).
  - **Funções totais encontradas**: 26.705
  - **Funções nomeadas**: 460
  - **Funções não nomeadas (`FUN_`)**: 25.859
  - **Funções thunk**: 117
  - **Funções externas**: 0
  - **Classes C++ identificadas (RTTI)**: 433 (Havok, MT Framework, ATL, STL, etc.)

---

## 3. V1 — Exportações do Ghidra

Status: **FEITO** (todos os 12 arquivos JSON gerados em `work/ghidra/export/`, mais `work/names.json`).

| Arquivo | Quantidade Extraída | Observações |
|---|---:|---|
| `funcs.json` | 26.436 | Entradas de função completas com corpo, calling convention, thunk, ranges e contagem de callers |
| `switches.json` | 2.540 | Saltos e chamadas computadas com alvos, tabela de dados e número de casos |
| `code_ptrs.json` | 185.332 | Ponteiros em seções de dados (`.rdata`, `.data`, `.data1`) apontando para código |
| `rtti.json` | 433 classes, 494 vtables | 1.681 métodos virtuais recuperados via MSVC RTTI (`rtti.py`) |
| `instr.json` | 245 mnemônicos únicos | Censo: 246.052 instruções x87, 1.028.811 instruções SSE |
| `imports.json` | 270 imports | Locais de chamada (call direto vs endereço tomado) mapeados |
| `outside.json` | 0 | **0 dependências** fora das seções do `clean.exe` (confirma independência do protetor) |
| `fs_refs.json` | 1 offset | Acessos via segmento TEB (`fs:`) |
| `text_writes.json` | 1 escrita | Escritas em memória `.text`/`.rdata` |
| `eh.json` | 8 throws, 4 raises | Locais de chamada de `_CxxThrowException` e `RaiseException` |
| `cpuid.json` | 4 ocorrências | `0x0042626b`, `0x0042643f`, `0x00e7d526`, `0x00e7d539` |
| `strings.json` | 23.305 | Strings definidas com contagens e referências cruzadas |

**Tabela de Nomes (`work/names.json`)**:
- 26.442 endereços nomeados (24.825 de funções do Ghidra + 1.617 de classes/métodos RTTI via `merge_names.py`).
- 42 nomes desambiguados por sufixo de endereço.

---

## 4. V2 — Auditoria do Catálogo contra o Ghidra

Status: **FEITO** (implementado em [`scripts/audit_catalog.py`](../scripts/audit_catalog.py), testado via [`tests/python/test_audit_catalog.py`](../tests/python/test_audit_catalog.py), relatório salvo em `work/audit/catalog_report.json`).

### 1. Inícios (Starts)
- **Correspondência exata (True Positives)**: 25.268 funções (95,6% de todas as 26.436 funções do Ghidra).
- **Divisões internas (Splits)**: 687 entradas no catálogo que correspondem a blocos internos ou loops de funções do Ghidra.
- **Entradas fora do Ghidra (Inventadas)**: 31.041 entradas (resultado do escaneamento heurístico de prólogos do `disasm32` em dados/padding não-código, com quase 16 mil funções <= 16 bytes).
- **Funções do Ghidra ausentes do catálogo inicial**:
  - *Com referências/chamadores reais (>0)*: **7 funções** (triadas e adicionadas via sementes):
    - 2 funções completas: `0x00ACFB40` e `0x00DB7C60`.
    - 5 pontos de entrada internos / alternativos: `0x0048D759`, `0x007707BD`, `0x009A2126`, `0x00DD4DD9`, `0x00E79517`.
  - *Sem referências (código morto/dados)*: 1.146 funções (apenas 1 nomeada: `__alloca_probe_8` em `0x00E6FCB6`).

### 2. Fins (Ends, com descarte de 0xCC/0x90)
- **Avaliadas em comum**: 25.268 funções.
- **Extensão idêntica**: 24.476 funções (**96,87%** de concordância exata byte a byte).
- **Fins curtos com continuação imediata (Splits)**: 438 funções (todas com entrada de continuação imediata no catálogo em `cand_end`).
- **Fins curtos com cauda de código genuinamente perdida**: **0**! (Nenhuma função teve código real truncado sem cobertura).
- **Fins longos / padding**: 310 funções por padding 0xCC/0x90 e 44 com código adjacente.

### 3. Saltos Computados (Switches)
- **Switches analisados**: 2.540 saltos computados (12.401 alvos de ramificação).
- **Alvos ausentes do catálogo/corpo da função no catálogo inicial**: **43 alvos** (em 5 funções, incluindo alvos de cauda como `0x007DAB05..0x007DAB58` e `0x00A23D2A..0x00A2A111`).
- **Ação**: Todos os 43 alvos foram exportados para sementes e incorporados ao catálogo.

### 4. Alvos Indiretos
- **Métodos virtuais de vtable (RTTI)**: 1.617 métodos verificados. Ausentes do catálogo: **0** (100% de cobertura).
- **Ponteiros de código em dados (`.rdata`, `.data`, `.data1`)**: Nenhum ponteiro para função conhecida do Ghidra ausente do catálogo.

### 5. Resolução e Fechamento de V2
- **Sementes geradas**: 50 sementes críticas (`work/seeds_ghidra.txt`: 7 funções com callers + 43 alvos de switch).
- **Catálogo estendido**: `python scripts/run_lift.py catalog --add-seeds work/seeds_ghidra.txt` adicionou 111 novas entradas despacháveis (fechamento transitivo), elevando o catálogo para **72.121 funções**.
- **Resultado pós-semeadura no audit**:
  - Funções do Ghidra com referências ausentes: **0**.
  - Alvos de switch ausentes do catálogo: **0**.
  - Métodos de vtable ausentes: **0**.
  - Sementes pendentes: **0**.

---

## 5. Experimentos Iniciais

### E1 — Jogo original com fps limitado a ~20 (RTSS)
- **Hipótese**: Passo de tempo / integração física instável em baixo framerate no motor original.
- **Responsável**: Usuário.
- **Resultado medido (2026-10-22)**: O jogo original funciona **normalmente** a ~20 fps (escada e benchmark variável).
- **Conclusão**: **DESCARTADA** a hipótese de sensibilidade a framerate/passo de tempo do jogo original. O defeito é exclusivo do `re5-recompiled` (pipeline de lift, threads/HLE, ambiente ou runtime).

### E9 — Só 2 de 8 núcleos trabalhando (2026-10-22) — **causa achada: `config.ini`**
- **Medido** (`RE5_PROFILE=10`, benchmark, `work/bench/prof_cores`): em jogo, 1,5-2,0 threads
  rodando por tick; no watchdog, dos 7 workers do MT Framework (tids `0x1024..0x103C`) só o
  primeiro tinha dispatches, os outros 6 parados em `WaitForSingleObject`.
- **Ghidra** (`FUN_00425ee0` construtor do sistema de jobs, `DAT_012340a4` = `0x18040010` no
  guest): cria sempre 7 workers (`FUN_004278f0`); `+0x94` = "JobThread" (`FUN_00426660` limita a
  1..8 e ao nº de processadores) e todo quadro `FUN_00426e60` copia `+0x94` para `+0x8c`, que o
  despachante `FUN_00427450` usa como nº de threads da lista de jobs. O valor vem do
  `config.ini` (`[CPU] JobThread=`) pelo carregador de propriedades.
- **Tracepoints** (`config/tracepoints.txt`, `RE5_TRACE=1`, `RE5_TRACE_PEEK`): `0x00426660`
  chamado uma vez com valor 2, `[+0x8c]=2`, processadores = 8.
- **Causa:** o `config.ini` do perfil do runtime
  (`bin_input/game/user/drive_c/Users/Player/Documents/CAPCOM/RESIDENT EVIL 5/`) tinha
  `JobThread=2` (o do jogo original do usuário tem 8). O jogo não recalcula o valor depois de
  gravado; sem o arquivo ele gera `JobThread=8` (testado). Provavelmente herdado de uma rodada
  antiga em que o `GetSystemInfo` do runtime dizia 2 núcleos.
- **Correção:** `JobThread=8` (cópia do arquivo anterior em `config.ini.bak_20261022`).
  Depois: 2,15-2,27 threads por tick (antes 1,4-1,6) no mesmo trecho e ~50-54 Present/s
  (antes 46-53): o ganho em fps é pequeno, porque o caminho crítico é a thread principal
  (código gerado em 80% das amostras, perfil plano) e não a contagem de workers.

### E10 — Diferencial por instantâneo do movimento (2026-10-22)
- **Ferramenta:** `RE5_CAPTURE=<VAs de tracepoints>` + F11 grava memória e registradores na entrada e
  na saída da próxima chamada; `scripts/snap_diff.py` reexecuta o x86 original no Unicorn a partir do
  "antes" e compara cada byte escrito com o "depois" do C gerado.
- **Resultado no benchmark:** `hkpCharacterProxy::integrate` (`0x00DD9F20`, 2 chamadas: 1.030 e 750 bytes
  escritos), o movimento de personagem do jogo `0x00B72FA0` (3.311 bytes) e `0x009DF110` (390 bytes):
  **0 bytes diferentes** e `eax`/`esp` iguais. A tradução do Havok e do movimento está correta nessas
  chamadas; falta a captura no ponto travado da escada.
- **Despejos do usuário na escada (F10 repetido):** o jogador para em pontos fixos e só avança a cada
  travada do F10 (quadro longo). Perto dele só há a malha do cenário (degraus reais: espelho ~0,25 m,
  piso ~0,45 m; cápsula de raio 0,4); nenhuma caixa extra do Havok. O `hkpCharacterProxy` do jogador
  tem `maxSlopeCosine` = cos(90°): a inclinação não é limitada pelo Havok, o jogo decide a velocidade
  (`0x00CFC5E0` escala a velocidade do jogo para o Havok e chama o integrate).
- **Itens:** itens fixos aparecem; só os sorteados (caixas e inimigos) não aparecem.
- **Captura no ponto travado da escada (usuário, estágio s115):** `0x00B72FA0` (3.746 e 3.254 bytes),
  `0x009DF110` (340 e 300) e `0x00CFC5E0` (1.336 e 552): **0 bytes diferentes** do x86. O quadro travado
  é calculado exatamente como o x86 calcularia a partir do mesmo estado.
- **Dados:** a colisão própria do jogo (`rCollision`, SBC1 `stage\s115\scr\scr00\s115h_scr`, 103.400 B) está
  na memória inteira e intacta (399/399 janelas do arquivo achadas, em `0x23AAAD40`+); o Havok do estágio
  também. O `dt` que o jogo passa ao movimento é o tempo real do quadro (0,1 s a 10 fps).
- **Sensibilidade ao fps (usuário):** a 1 fps sobe a escada, a 5 fps sobe devagar, a 20 fps não sobe; a
  5 fps, descendo, anda no ar e desce devagar. Falta o mesmo teste no jogo original (o E1 só mediu 20 fps).

### Ferramentas novas (2026-10-22)
- **Tracepoints:** `config/tracepoints.txt` + `RE5_TRACE=1` (`RE5_TRACE_ARGS`, `RE5_TRACE_MAX`,
  `RE5_TRACE_SAMPLE`, `RE5_TRACE_PEEK=va,va`): entrada/saída de funções escolhidas, com thread,
  nome, `ecx`, endereço de retorno, argumentos e `eax`.
- **Cobertura de funções:** todo função gerada marca `re5_cov[va-0x401000]`; com `RE5_COV=1`,
  F8 zera e F9 grava `work/cov/cov_<n>.txt` ("VA nome"). Compara-se "jogo parado" com "quebrando
  a caixa": a diferença é o código da ação (itens que não caem, escada, câmera).

### E2 — `re5.exe` com `RE5_GUEST_LOCK=global` no Benchmark Variável
- **Hipótese**: Ordem de memória / race conditions entre threads do guest no modo `hle`.
- **Status**: em andamento (2026-10-22). `scripts/bench_re5.py` com `RE5_D3D9_SHOT` a cada
  400 `Present` (4800..23600), folha de contato por `PIL` (`work/bench/<rótulo>/sheet.png`).
- **Linha de base `hle`** (`work/bench/e2_hle`): 251 s, mediana 39,4 fps, p10 30,3. **O defeito
  aparece no benchmark**: nos quadros 8400-8800, 9600-9800 e 10800 a câmera está abaixo do
  chão (metade de baixo preta, telhados no alto; o chão não é desenhado visto por baixo),
  com o personagem normal em 8000, 10000 e de 11000 em diante; é o afundar e ser jogado para
  fora em ciclo relatado pelo usuário. Serve de detector sem jogador: fração de quadros
  com a metade inferior preta.
- **`ExitProcess(0)` sem causa**: as duas rodadas terminaram nele (a `hle` no `Present` 14237,
  antes dos 21325 da rodada `base`; a `global` no `Present` 33). O runtime agora registra,
  em `ExitProcess`, o endereço do chamador (com o nome), a trilha de dispatch, e em
  `PostQuitMessage`/`SDL_WINDOWEVENT_CLOSE` a origem do pedido.
