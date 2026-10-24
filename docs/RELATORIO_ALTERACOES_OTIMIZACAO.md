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

## 3. Estado Atual Após a Restauração

* **Todas as alterações além do Item 1 foram desfeitas.**
* **O Item 1 (vetor nativo SSE) permanece intacto e 100% ativo**, fornecendo a eliminação das operações escalares da `union XMM`, sem alterar a sincronização de chamadas nem as convenções internas da engine.
* Todas as 72.121 funções foram regeradas preservando a compatibilidade total de chamadas e estado da CPU.
