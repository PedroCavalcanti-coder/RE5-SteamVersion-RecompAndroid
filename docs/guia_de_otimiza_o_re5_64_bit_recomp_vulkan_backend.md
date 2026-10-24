# Guia de Engenharia de Performance: RE5 Recomp 64-bit & Vulkan Backend

Este documento compila a especificação da Skill/Custom Instruction para otimização em C e x86-64 ASM, bem como as estratégias arquiteturais aplicadas à recompilação de *Resident Evil 5* (MT Framework) em 64 bits com backend Vulkan, com foco especial na eliminação de gargalos de CPU.

---

## 1. Claude System Prompt / MCP Skill: Low-Level Architect

Adicione esta instrução nas configurações de projeto (Claude Project / Custom Instructions) para que o assistente analise rotinas com foco em microarquitetura moderna e compiladores:

```markdown
# Role: Low-Level Performance Engineer & x86-64 Systems Architect

## Core Objective
Analyze, profile, and optimize C and x86-64 assembly routines for maximum throughput, low instruction latency, and cache efficiency without sacrificing correctness.

## Optimization Methodology & Analysis Checklist
When analyzing any C or assembly snippet, systematically evaluate:

1. Microarchitecture & Pipelines:
   - Instruction latency vs. reciprocal throughput (Agner Fog / uops.info standards).
   - Port pressure and execution bottlenecks (avoid ALU port saturation).
   - Loop unrolling trade-offs vs. instruction cache (I-Cache) pressure.
   - Elimination of partial register stalls, false dependencies, and store-to-load forwarding penalties.

2. Branching & Control Flow:
   - Eliminate branch mispredictions in critical paths.
   - Prefer branchless patterns: bit-twiddling, conditional moves (`cmovcc`), and mask arithmetic.

3. Memory & Cache Hierarchy:
   - Cache-line alignment (64-byte alignment, avoiding cache splits).
   - Sequential memory access patterns for hardware prefetchers.
   - Structure of Arrays (SoA) vs. Array of Structures (AoS).
   - Aliasing and compiler optimization inhibitors: evaluate `restrict`, memory barriers, and volatile misuse.

4. SIMD & Vectorization:
   - Leverage SSE4.2, AVX2, and AVX-512 vector extensions where applicable.
   - Alignment requirements for aligned loads/stores (`vmovaps`/`vmovdqa`) vs unaligned (`vmovups`/`vmovdqu`).
   - Vector intrinsics recommendations for hot loops.

5. Calling Conventions & Registers:
   - System V AMD64 vs Microsoft x64: correct utilization of caller-saved vs callee-saved registers.
   - Red Zone exploitation (System V: 128 bytes below RSP) for leaf functions.
   - Clean register allocation avoiding unnecessary spills to the stack.

## Response Format
For every optimization request:
1. Bottleneck Diagnosis: Pinpoint the exact instruction or memory hazard causing overhead.
2. Optimized Routine: Provide optimized C code or explicit x86-64 assembly (Intel syntax default).
3. Mechanical Sympathy Breakdown: Explain why the change runs faster at hardware level (cycles, branches, vectorization).
4. Compiler Verification Tip: Compiler flags (GCC/Clang/MSVC) and Godbolt validation steps.
```

---

## 2. Modelo de Memória e Recompilação (32-bit para 64-bit)

Na transição de um binário desenhado originalmente para 32 bits (onde ponteiros ocupam 4 bytes) para x86-64, a simples expansão ingênua de ponteiros dobra o tráfego do barramento de memória e quebra alinhamentos de cache.

### 2.1 Arena Virtual Fixa (Zero-Cost Addressing)
Evite calcular conversões de ponteiros em runtime ou realizar checagens dinâmicas de bounds por acesso de memória. Mapeie uma arena contígua de 4 GB fixa no início do processo:

```c
// Windows:
void* game_base = VirtualAlloc((void*)0x10000000, 0x100000000ULL, 
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

// Linux / POSIX:
void* game_base = mmap((void*)0x10000000, 0x100000000ULL, 
                       PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
```
* **Vantagem em Assembly:** Instruções x86-64 podem usar endereçamento base+índice direto com deslocamento de 32 bits sem custo computacional extra:
  ```nasm
  mov eax, [r14 + rbx*4 + 0x10] ; r14 aponta para game_base fixo
  ```

### 2.2 Auditoria de `#pragma pack` e Alinhamento
* Estruturas com `pack(1)` ou `pack(4)` geram acessos não alinhados de 64 bits (`mov rax, [rbx+1]`), cruzando limites de linhas de cache (cache-line splits) e custando até 15–20 ciclos por acesso.
* Force alinhamento de 16 ou 32 bytes em tipos matemáticos:
  ```c
  struct alignas(16) Vec4 {
      float x, y, z, w;
  };
  ```

---

## 3. Vetorização SIMD (Colisão SBC1 e Traversal de Terreno)

O formato de colisão estática de cenários da Capcom (`.sbc`) processa centenas de caixas AABB e nós de octree/BVH.

### 3.1 Teste 4-Way / 8-Way de AABB (Structure of Arrays)
Transforme arrays de estruturas (`AABB { Vec3 min, max; }`) em estruturas de arrays para alimentar registradores SIMD (`__m128` ou `__m256`):

```c
#include <immintrin.h>

// Testa 4 AABBs em paralelo contra 1 raio
inline int RayIntersect4AABB(__m128 ray_orig_x, __m128 inv_dir_x,
                             __m128 min_x, __m128 max_x,
                             __m128 tmin, __m128 tmax) {
    __m128 t1 = _mm_mul_ps(_mm_sub_ps(min_x, ray_orig_x), inv_dir_x);
    __m128 t2 = _mm_mul_ps(_mm_sub_ps(max_x, ray_orig_x), inv_dir_x);

    __m128 near_x = _mm_min_ps(t1, t2);
    __m128 far_x  = _mm_max_ps(t1, t2);

    tmin = _mm_max_ps(tmin, near_x);
    tmax = _mm_min_ps(tmax, far_x);

    __m128 mask = _mm_cmple_ps(tmin, tmax);
    return _mm_movemask_ps(mask); // Retorna máscara de bits com os hits
}
```

### 3.2 Matemática Sem Desvios (Branchless)
* Substitua `if (dot > 0.0f)` em checagens de normais e inclinações de escada/chão por seleções vetoriais:
  - `_mm_blendv_ps` (SSE4.1) / `vblendvps` (AVX).
  - `cmovcc` em x86-64 puro para evitar desvios imprevisíveis na branch prediction unit (BPU).

---

## 4. Backend Vulkan: Arquitetura de Baixo Overhead

Gargalos onde a CPU satura e a GPU opera em baixa utilização decorrem geralmente da camada de submissão e vinculação de recursos.

### 4.1 Descriptor Indexing (Bindless Textures)
* **Problema:** Ficar trocando `VkDescriptorSet` para cada draw call via `vkCmdBindDescriptorSets`.
* **Solução:** Ative `VK_EXT_descriptor_indexing`. Crie um único array contendo todas as texturas da fase e envie apenas um índice de 32 bits via **Push Constants**:
  ```glsl
  #version 460
  #extension GL_EXT_nonuniform_qualifier : enable
  layout(set = 0, binding = 0) uniform sampler2D uTextures[];
  layout(push_constant) uniform PushConsts { uint textureID; };
  ```

### 4.2 Dynamic Uniform Offsets para Matrizes de Ossos
* Em vez de alocar dezenas de pequenos uniform buffers:
  1. Reserve um grande `VkBuffer` contíguo em memória visível pelo host (`HOST_VISIBLE | HOST_COHERENT`).
  2. Guarde todas as matrizes do frame em offsets alinhados (ex.: 256 bytes).
  3. Vincule o descritor apenas **uma vez** e forneça o offset dinâmico na chamada:
     ```c
     uint32_t offset = current_entity_bone_offset;
     vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, 
                             pipelineLayout, 0, 1, &matrixSet, 1, &offset);
     ```

### 4.3 Vulkan 1.3 Dynamic Rendering
* Elimine `VkRenderPass` e `VkFramebuffer` estáticos substituindo por `vkCmdBeginRendering` (`VK_KHR_dynamic_rendering`). Reduz código boilerplate e minimiza overhead de estado na thread de render.

---

## 5. Estratégias para Sanar o Gargalo "CPU Alta / GPU Baixa"

Quando o perfil do jogo exibe 100% de uso em um ou mais núcleos de CPU e GPU abaixo de 40%, adote as seguintes correções sistemáticas:

```
[ Diagnóstico: CPU Alta / GPU Baixa ]
                  │
  ┌───────────────┼───────────────┐
  ▼               ▼               ▼
[ 1. API Overhead ] [ 2. CPU Loops/Waits ] [ 3. Heavy Work ]
  - Gravação paralela  - Spinlocks ocultos   - Vertex skinning
  - 1x vkQueueSubmit   - Frame limiter busy     na GPU
  - Bindless / Push    - Wait sem _mm_pause  - Frustum culling
    Constants            ou futex               via SIMD
```

### 5.1 Gravação Paralela de Command Buffers
* Aloque uma thread dedicada para lógica do jogo e $N$ threads de trabalho para despachar renderização.
* Cada worker grava em um **Secondary Command Buffer**:
  ```c
  VkCommandBufferInheritanceInfo inheritInfo = { ... };
  VkCommandBufferBeginInfo beginInfo = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
      .pInheritanceInfo = &inheritInfo
  };
  vkBeginCommandBuffer(workerCmd[i], &beginInfo);
  // Grava draws do chunk i
  vkEndCommandBuffer(workerCmd[i]);
  ```
* A thread principal apenas executa os secundários em lote:
  ```c
  vkCmdExecuteCommands(mainCmd, numWorkers, workerCmd);
  ```

### 5.2 Eliminação de Submissões Excessivas
* Nunca chame `vkQueueSubmit` por entidade ou por passe isolado. Agrupe todos os comandos do frame e faça **uma única submissão gráfica por frame** (ou duas se compute assíncrono estiver ativo).

### 5.3 Caça a Spinlocks e Busy-Waiting
* Motores da era de 2009 frequentemente usavam loops vazios para sincronizar áudio, física ou jobs de render:
  ```c
  // PÉSSIMO: queima 100% da CPU em espera ociosa
  while (!job->is_done) {}
  ```
* **Mitigação Imediata:**
  Adicione pausa no pipeline x86 para liberar recursos de execução do núcleo:
  ```c
  while (!job->is_done) {
      _mm_pause(); // Reduz consumo de energia e evita pipeline stalls
  }
  ```
* **Solução Definitiva:** Substitua por semáforos leves do SO (`futex` no Linux, `WaitOnAddress` no Windows, ou `std::atomic::wait`).

### 5.4 Offloading do Skinning Esquelético para a GPU
* Se o código recompilado estiver multiplicando vértices por matrizes de ossos na CPU e copiando tudo via `memcpy` todo frame:
  - Mova a interpolação linear dos vértices (`BlendWeights`) para o **Vertex Shader** ou para uma passagem de **Compute Shader** com *Storage Buffers* (SSBO).
  - A CPU deve apenas escrever as matrizes de transformação dos ossos uma vez por frame.

### 5.5 Frustum Culling Antecipado
* Implemente um descarte rápido por esfera delimitadora (Bounding Sphere) via SIMD na CPU antes de montar a draw call:
  - Se o objeto não cruza os 6 planos da câmera, a CPU não toca nos descritores nem nas chamadas `vkCmdDrawIndexed`.

---

## 6. Flags de Compilação Recomendadas (GCC / Clang / MSVC)

Configure o compilador da recompilação para extrair o máximo do conjunto de instruções moderno da máquina alvo:

### Clang / GCC:
```bash
-O3 \
-march=x86-64-v3 \       # Garante AVX2, FMA, BMI2 (padrão de PCs modernos)
-flto \                  # Link-Time Optimization entre unidades de compilação
-fno-semantic-interposition \
-fno-trapping-math \
-fstrict-aliasing
```

### MSVC:
```bat
/O2 /arch:AVX2 /GL /LTCG /fp:fast
```

Use sempre a palavra-chave `__restrict` em buffers de entrada/saída de vértices, matrizes e nós de colisão para habilitar vetorização automática pelo compilador.