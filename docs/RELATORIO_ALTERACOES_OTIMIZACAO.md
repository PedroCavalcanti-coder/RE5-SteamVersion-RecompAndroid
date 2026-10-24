# Relatório de Alterações de Otimização e Diagnóstico

**Data:** 24/10/2026  
**Contexto:** Otimizações de desempenho na recompilação do Resident Evil 5 (`re5-recompiled`) baseadas no plano revisado após a linha de base no commit `c875e6ce`.

---

## 1. Todas as Alterações Realizadas

### A. Item 1: XMM como Vetor Nativo (MANTIDO)
* **Objetivo:** Eliminar o derramamento massivo de pilha (`(%rsp)`) causado pela antiga `union XMM` (que continha arrays escalares `f32[4]`, `u32[4]`, etc.), a qual forçava o GCC a dividir registradores de 128 bits em inteiros de 64 bits e usar manipulação de bits (`movd`, `andq`, `orq`) na memória da pilha para cada escrita escalar.
* **Arquivos alterados:**
  - `tools/pcrecomp/runtime/recomp32_cpu/cpu.h`:
    - Definido `typedef xv4f XMM;` onde `xv4f` é `float __attribute__((vector_size(16)))`.
    - Implementados helpers estáticos inline com intrínsecos SSE (`_mm_load_ss`, `_mm_add_ps`, `_mm_mul_ps`, etc.).
    - Preservada a verificação `EXPECTED_ADDR = 33` em `rdxm`/`wrxm`.
  - `tools/pcrecomp/tools/lift/lift32_cpu.py`:
    - `_ss(insn, op)` passou a acessar `c->xmm[i][0]` diretamente.
    - `_sd(insn, op)` passou a usar `xmm_get_sd(c->xmm[i])`.
    - `_xm(insn, op)` passou a retornar `c->xmm[i]`.
    - Instruções escalares (`movss`, `movsd`) geram escritas diretas na lane 0 (`c->xmm[d][0] = ...`) ou carregamentos por valor (`sse_load_ss`), **sem tomar o endereço `&R_x`**.
    - Instruções vetoriais empacotadas (`mulps`, `addps`, `shufps`, etc.) emitem operações de vetor nativo.
    - Adicionado `SSE_SHIFT` ao conjunto `SSE_AMBIGUOUS` para evitar que instruções MMX de 64 bits (como `psllq mm1`) fossem tratadas erroneamente como SSE de 128 bits e quebrassem o lifter.
  - `scripts/difftest_cpu.py` & `scripts/difftest_func.py`:
    - Adaptado o harness de teste diferencial para acessar `((const uint32_t *)&c->xmm[r])[k]` em vez do campo removido `.u32`.
  - `tests/python/test_lifter_gaps.py`:
    - Atualizadas as asserções de sintaxe de C esperadas pelo teste para a nova sintaxe vetorial.

---

### B. Alterações Além do Item 1 (REVERTIDAS)

#### 1. Remoção do salvamento/recarga de XMM nas chamadas (`cache_registers` em `lift32_cpu.py`)
* **O que foi feito:** No gerenciador de registradores locais do lifter, assumiu-se a regra padrão da ABI x86-32 padrão da Microsoft de que registradores XMM são voláteis (*caller-saved scratch*). Com isso, removeu-se o salvamento (`c->xmm[...] = R_x...`) e recarga dos 8 vetores XMM antes e depois de chamadas `_FULL_SYNC` (`RE5_CALL`, `dispatch`).
* **Motivo da falha:** O RE5 foi compilado pela Capcom no MSVC 2008 com otimizações em nível de programa inteiro (LTCG / `/GL`). Nessas compilações, o compilador gera convenções de chamada internas proprietárias que fogem da ABI padrão: funções passam vetores e parâmetros geométricos diretamente em `xmm0..xmm7`, ou esperam que certos registradores XMM permaneçam intactos através de funções internas da MT Framework. Ao não sincronizar `c->xmm`, os registradores foram corrompidos entre chamadas internas do jogo.

#### 2. Inlining direto de funções folha em tempo de lift (`lift32_cpu.py`)
* **O que foi feito:** Inlining manual das chamadas diretas para:
  - `0x00428D60` (`sqrtf`) -> emitindo `fpush(c, sqrtf(rdf32(c->esp)));`
  - `0x00428D70` -> `c->eax = c->ecx;`
  - `0x00428D90` -> `c->eax = 0x01249EC0u;`
  - `0x00428DA0` -> `c->eax = (uint32_t)rd8(c->ecx + 0x0Cu);`
  - `0x00428DB0` -> `c->eax = (uint32_t)rd8(c->ecx + 0x0Du);`
* **Motivo da falha:** Em particular para `0x00428D60`, a rotina original do jogo manipulava alinhamento de pilha `and esp, -8` e interagia com a pilha do x87 de formas dependentes de ponteiros de quadro específicos. Além disso, eliminar a chamada alterou o comportamento de chamadas de convenção estendida da engine.

#### 3. Supressão de `RE5_LOOP_BARRIER()` em laços com chamadas (`run_lift.py`)
* **O que foi feito:** O lifter passou a verificar se o corpo de um laço continha `dispatch(` ou `RE5_CALL(` e, nesses casos, omitia o `RE5_LOOP_BARRIER()`.
* **Motivo da reversão:** Manter estritamente o comportamento original do commit `c875e6ce`, onde o `RE5_LOOP_BARRIER()` é gerado em todas as cabeças de laço para trás para garantir que nenhuma thread de worker da MT Framework entre em loop infinito de leitura de memória com ponteiros `restrict`.

#### 4. Macro `RE5_COV` condicional (`run_lift.py`)
* **O que foi feito:** Alterado o cabeçalho dos arquivos gerados para `#define RE5_COV(va) ((void)0)` caso `-DRE5_ENABLE_COV` não estivesse presente.
* **Motivo da reversão:** Revertido para restaurar a integridade exata do lifter de `c875e6ce`.

---

## 2. Diagnóstico: Por que "o jogo ficou sem nada"?

Quando o jogo roda com as matrizes de mundo, projeção ou iluminação corrompidas:
1. As chamadas internas da MT Framework responsáveis por carregar transformações de câmera, nós de cena e matrizes esqueléticas passam valores em `xmm0`..`xmm7`.
2. Como a sincronização de `c->xmm` havia sido desativada nas chamadas, esses valores se perderam entre as chamadas de funções.
3. Com matrizes contendo zeros ou NaNs, todas as geometrias e modelos 3D sofreram *clipping* imediato (foram descartados pela pipeline de renderização) ou foram desenhados fora do frustum da câmera, resultando na tela sem elementos de jogo ("o jogo ficou sem nada").

---

## 3. Estado Atual do Item 1 (Vetor Nativo SSE)

* **Todas as alterações além do Item 1 foram desfeitas e o Item 1 foi commitado (`030b33b1`).**
* **O Item 1 (vetor nativo SSE) permanece intacto e 100% ativo**, fornecendo a eliminação das operações escalares da `union XMM`, sem alterar a sincronização de chamadas nem as convenções internas da engine.
* **Resultado medido pelo usuário no benchmark:**
  * Ganho de **+3 FPS** de média (de ~60 para ~63.3 FPS a 800x600).
  * Queda significativa no uso de **RAM** e **VRAM**.
* **Auditoria de cobertura:** Varredura em todas as 72.121 funções (1.348.489 instruções SSE) confirmou 100% de cobertura (0 `RECOMP_TODO`).

---

## 4. Investigação Gráfica (DirectX 9 vs Vulkan) e Teste B (`dxvk.conf`)

### A. Diagnóstico da Pilha Gráfica
* O Resident Evil 5 foi desenvolvido na MT Framework em **DirectX 9.0c**.
* A ponte do recompilador (`runtime/src/gfx/d3d9_host.c`) carrega a DLL nativa x64 do **DXVK 2.6.2**, que traduz as chamadas D3D9 diretamente para comandos **Vulkan** enviados ao driver AMD (`vulkan-1.dll`).
* É por isso que o menu do jogo reporta "DirectX 9.0c" enquanto o RivaTuner (RTSS) detecta e exibe "VULKAN".
* **Gargalo identificado:** Na captura do benchmark, a GPU Vega da APU Ryzen operava a apenas **200 MHz** (clock de repouso/idle) com 34% de uso, indicando que o gargalo principal está na CPU (single-core) executando o código de jogo traduzido.

### B. Teste B (`dxvk.conf`)
* Foi testada uma configuração de baixa latência via `dxvk.conf` contendo `d3d9.maxFrameLatency = 1` e `dxvk.numCompilerThreads = 8`.
* **Resultado:** O FPS caiu.
* **Causa técnica:**
  1. Forçar a latência para 1 quadro eliminou o enfileiramento padrão (buffer de 3 quadros do DXVK), forçando a CPU já sobrecarregada a esperar a GPU apresentar antes de computar o quadro seguinte.
  2. As 8 threads de compilação concorreram com os núcleos do Ryzen 5700U usados pelas threads de simulação e áudio do jogo.
* O arquivo `dxvk.conf` foi **removido** para restaurar o comportamento padrão de maior vazão.

---

## 5. Item 2: Flags em Variáveis Locais (IMPLEMENTADO)

### A. Motivação e Diagnóstico
Anteriormente, todas as operações aritméticas (`cmp`, `test`, `add`, `sub`, etc.) e desvios condicionais (`je`, `jne`, `jbe`, `jl`, etc.) liam e escreviam as flags diretamente na memória da estrutura `CPU *c` (`c->zf`, `c->cf`, `c->sf`, etc.).
No código x86-64 final gerado pelo GCC:
* Um simples `test eax, eax; je target` gerava:
  - Escrita do resultado em memória: `movl %eax, 36(%rbx)`
  - Releitura da memória para o salto: `cmpl $0, 36(%rbx); je ...`
* Leituras e escritas na memória do guest (`wr32`, `wrss`, etc.) forçavam o compilador a reler as flags da memória por não poder garantir a ausência de *aliasing*.

### B. Solução Implementada
1. **Inclusão das flags no cache de registradores (`CACHED` em `lift32_cpu.py`):**
   * As flags `cf`, `pf`, `af`, `zf`, `sf`, `of` foram adicionadas à tupla `CACHED` junto aos registradores GPR e XMM.
   * No topo de toda função gerada, são declaradas variáveis locais:
     `uint32_t R_cf = c->cf, R_pf = c->pf, R_af = c->af, R_zf = c->zf, R_sf = c->sf, R_of = c->of;`
   * As flags são sincronizadas de volta para `c->...` apenas em chamadas externas (`RE5_CALL`, `dispatch`) e no retorno da função caso tenham sido modificadas (*dirty tracking*).
2. **Macros de Alta Performance em `cpu.h` (`#if defined(RE5_CACHED_REGS)`):**
   * Criadas versões em macro com *statement expressions* GNU C (`({ ... })`) para os helpers mascarados:
     - `flags_sub_m`, `flags_add_m`, `flags_logicz_m`, `flags_adc_m`, `flags_sbb_m`
     - `flags_incs`, `flags_decs`
     - `op_shl_m`, `op_shr_m`, `op_sar_m`
     - `sse_compare`, `do_sahf`, `fcompare_eflags`
   * Essas macros operam diretamente nas variáveis locais `R_cf`, `R_zf`, `R_sf`, etc., permitindo que o GCC as aloque em registradores reais da CPU host.
   * `flags_incs` e `flags_decs` reutilizam `flags_add_m` e `flags_sub_m` com máscara `(ALL_FLAGS & ~RF_CF)`, preservando o Carry Flag nativamente sem código duplicado.
3. **Fusão de Instruções pelo Compilador:**
   * Uma comparação `flags_sub_m(c, 0x040u, R_eax, 0, 4)` seguida de `if (R_zf) goto ...` agora é compilada pelo GCC diretamente em uma única instrução nativa do host:
     ```assembly
     testl %eax, %eax
     je .L_target
     ```
   * Zero acessos à pilha ou à memória da estrutura `c` dentro dos laços.

### C. Validação e Resultados
* **Re-lifting completo:** Todas as 72.121 funções foram regeneradas em 181 arquivos com **0 erros de lift**.
* **Redução no tamanho do binário:** O `re5.exe` diminuiu de ~113.9 MB para **~112.5 MB** (-1.34 MB de código de máquina redundante eliminado).
* **Testes unitários e diferenciais:**
  - `python -m unittest discover -s tests/python`: **55/55 testes passaram**.
  - `ctest --test-dir build/test-build`: **100% dos testes passaram** (20/20, incluindo `difftest_cpu` diferencial de 128s contra o emulador Unicorn).

### D. Travamento depois do Item 2: flags desatualizadas em volta de helpers do cpu.h

* **Sintoma relatado:** com o Item 2, o jogo trava depois de um tempo; o fps fica igual ou menor.
* **Causa encontrada (2026-10-24):** só os helpers de flags que ganharam versão em macro
  (`flags_*`, `op_shl/shr/sar`, `sse_compare`, `fcompare_eflags`, `do_sahf`) passaram a
  escrever em `R_cf..R_of`. Os outros helpers do `cpu.h` continuaram lendo e escrevendo
  `c->cf..c->of`: `op_rol/ror`, `op_rcl/rcr`, `op_shld/shrd`, `op_bsf/bsr`,
  `bit_string_op` (`bt/bts/btr/btc` na memória), `eflags_pack/unpack` (`pushfd/popfd`).
  O lifter só sincronizava em volta de helpers que mexem em GPR/XMM
  (`_cpu_h_touch_regs`), então o desvio seguinte lia uma flag local velha. Caso real no CRT,
  `strpbrk`/`strcspn` em `0x00E7DA28`:
  ```c
  bit_string_op(c, (R_esp), (int32_t)(R_eax), 0);   /* bt dword ptr [esp], eax -> c->cf */
  if (!R_cf) goto L_00E7DA1C;                       /* jae: lia o CF do test anterior */
  ```
  A busca de caractere do CRT respondia errado: a análise de texto do jogo fica corrompida
  e pode entrar em laço. Problema secundário: `op_shl/op_shr/op_sar` sem máscara (flags
  vivas no `ret` do CRT) não marcavam as flags como sujas, então o `ret` não as gravava.
* **Correção (`lift32_cpu.py`):** `_cpu_h_touch_regs` também lista os helpers que tocam
  `c->cf..c->of`; nas instruções que os chamam, o lifter grava as flags sujas antes do
  comando e recarrega as seis depois (novo tipo de sincronização `"flags"`). `_writes`
  reconhece `op_shl/op_shr/op_sar(c, ...)`. Teste novo em
  `tests/python/test_lifter_gaps.py` (bt na memória e shl no ret do CRT).
* **Verificado:** relift (72.121 funções, 0 falhas de lift, 47 arquivos regravados), build,
  `python -m unittest` 55/55, `ctest` 20/20. C gerado em `0x00E7DA28`:
  `c->cf = R_cf; c->zf = R_zf; bit_string_op(...); R_cf = c->cf; ...`.
  **Testado no jogo:** o travamento continuou (ver E).
* **Por que o fps não subiu:** o store de flag em `c` não ficava no caminho crítico, e a
  releitura antes do `jcc` é escondida pela predição de desvio. O Item 2 tira pouco
  trabalho e acrescenta 6 variáveis locais disputando registradores em `-O1`. A medição de
  onde está o tempo (perfil amostrado) continua pendente.

### E. Item 2 REVERTIDO; causa do fechamento (2026-10-24)

* **Medido pelo usuário** com o Item 2 + correção D + diagnóstico de pilha: ~10 fps a menos
  de média que com o Item 1 sozinho, e o jogo fecha depois de um tempo, não num ponto fixo.
* **Decisão:** voltar o código ao commit do Item 1 (`030b33b1`). O Item 2, a correção D e
  o diagnóstico ficaram em `work/patches/item2_flags_e_diag_crash.patch` (+ `test_crash.c`).
* **O fechamento** (com o diagnóstico novo do `crash.c`): estouro da pilha do host num ciclo
  `L_004470A0 -> L_0044711C -> L_0044744D -> L_004470A0`, com a trilha
  EnterCriticalSection/Sleep/LeaveCriticalSection repetida. `0x0044711C` e `0x0044744D` são
  pedaços da mesma função real que o catálogo tem como funções separadas; cada `jmp` entre
  eles vira chamada C aninhada, e o laço de espera cresce a pilha a cada volta. Ver
  `docs/STATUS.md`. Pode também acontecer com o Item 1 sozinho se o laço de espera durar o
  bastante; precisa de correção no catálogo ou no `jmp` entre funções.

### F. Item 1 também desfeito no código (2026-10-24)

* A pedido do usuário, o código voltou ao estado de `c875e6ce` (antes do Item 1):
  `lift32_cpu.py`, `cpu.h`, `difftest_cpu.py`, `difftest_func.py`, `test_lifter_gaps.py`.
  O commit `030b33b1` continua no histórico (nada reescrito); este relatório e o
  `STATUS.md` ficam com os registros.
