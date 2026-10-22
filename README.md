# Resident Evil 5: Gold Edition — Static Recompilation (`re5-recompiled`)

[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20Android%20ARM64-blue.svg)](#arquitetura)
[![Graphics](https://img.shields.io/badge/Graphics-Direct3D%209%20%E2%86%92%20Vulkan%20(DXVK%20Native)-orange.svg)](#camada-gr%C3%A1fica)
[![Engine](https://img.shields.io/badge/Engine-Capcom%20MT%20Framework%201.4-red.svg)](#motor-mt-framework)
[![Audio](https://img.shields.io/badge/Audio-FAudio%20%7C%20XAudio2%20%7C%20SDL2-green.svg)](#subsistemas-multim%C3%ADdia)
[![Status](https://img.shields.io/badge/Status-Fase%200%20conclu%C3%ADda%20-yellow.svg)](docs/STATUS.md)
[![License](https://img.shields.io/badge/License-MIT%20(Tooling%20%26%20Runtime)-lightgrey.svg)](#aviso-legal)

Projeto de **recompilação estática de binário x86 (PE32)** de *Resident Evil 5: Gold Edition* (`re5dx9.exe`) para código C portável e moderno, viabilizando a execução nativa com aceleração gráfica **Vulkan** em desktop (Windows/Linux) e dispositivos móveis **Android (ARM64-v8a)** sem emulação dinâmica ou Wine/Box64.

Baseado na metodologia e ferramentas do ecossistema [pcrecomp](https://github.com/sp00nznet/pcrecomp) e na arquitetura de referência do [nfsu2-recompiled](https://github.com/codepdbh/nfsu2-recompiled).

---

> **Documentação do projeto:** [Plano por fase (o que a IA e você fazem em cada etapa)](docs/PLANO_DO_PROJETO.md) ·
> [Status com evidências](docs/STATUS.md) · [Arquitetura e decisões](docs/ARQUITETURA.md) ·
> [Reconhecimento do binário](docs/RECON_RE5.md)

## Estrutura do Repositório

```text
re5-recompiled/
├── CMakeLists.txt      # Build raiz (CMake + Ninja); presets em CMakePresets.json
├── CLAUDE.md           # Guia para as sessões de IA: comandos, regras, convenções
├── android/            # Projeto Android (Fase 5): estrutura planejada em android/README.md
├── bin_input/          # Executável e arquivos originais do jogo (Git LFS; o repositório é privado)
├── config/             # re5_config.toml, builds conhecidos, overrides/exclude/hooks, política de imports
├── docs/               # Plano, status, arquitetura e relatório de reconhecimento
├── generated/          # Código C gerado pelo lifter (src/ é regenerado, não versionado; stub/ antes da Fase 2)
├── runtime/            # Runtime: memória do guest, dispatch, thunks, loader PE, threads, VFS, HLE Win32
├── scripts/            # Pipeline: doctor, recon (portão do lifter), run_lift, gen_cpu_header, bootstrap
├── tests/              # Unitários em C, ponta a ponta com PE32 sintético e testes Python
├── third_party/        # Dependências e bibliotecas nativas de suporte
│   ├── dxvk-native/    # Implementação Direct3D 9 nativa compilada diretamente sobre Vulkan
│   ├── FAudio/         # Reimplementação de XAudio2, X3DAudio e XAPO sobre SDL2
│   ├── FFmpeg/         # Decodificação de streams de vídeo WMV (Windows Media Video 9 / VC-1)
│   ├── imgui/          # Dear ImGui para overlay de controles touch customizáveis em Android
│   ├── libadrenotools/ # Suporte a injeção de drivers Vulkan customizados (Turnip/Mesa) no Android
│   ├── SDL2/           # Gerenciamento de janelas, superfícies Vulkan, eventos e áudio
│   ├── sse2neon/       # Tradução header-only de intrínsecos x86 SSE/SSE2 para ARM NEON
│   └── dxvk/           # Referência da árvore upstream do DXVK
└── tools/              # Ferramentas de lifting e análise
    ├── pcrecomp/       # Ferramentas de desmonte x86, análise CFG e lifter para C
    ├── nfsu2-reference/# Repositório de referência para integração de runtime e pipeline Android
```

---

## Dependências e Módulos Importados

Todos os submódulos e repositórios necessários foram integrados e verificados:

| Dependência | Versão / Commit | Finalidade Técnica |
|---|---|---|
| **dxvk-native** | `native-1.9.2b` (`c8dc91fa`) | Camada Direct3D 9 → Vulkan nativa sem dependência de DLLs do Windows |
| **libadrenotools** | `master` (`8fae8ce`) | Carregamento dinâmico de drivers Turnip em SoCs Qualcomm Snapdragon |
| **FAudio** | `master` (`6839b88`) | HLE para chamadas COM `XAudio2Create` e posicionamento 3D `X3DAudio` |
| **SDL2** | `SDL2` (`8acf7cfc0`) | Janela cross-platform, eventos de input, `SDL_WINDOW_VULKAN` e backend de som |
| **sse2neon** | `master` (`2272251`) | Tradução de operações vetoriais de 128 bits (__m128) para ARM64 NEON |
| **imgui** | `master` (`4be0b47ec`) | Renderização do HUD touch, botões analógicos e editor de layout mobile |
| **FFmpeg** | `master` (`bbd37149e4`)| Decodificação de cutscenes `.wmv` da MT Framework |
| **pcrecomp** | `main` (`dd01905`) | Pipeline de desmontagem, catálogo de imports e lifter x86 → C |
| **nfsu2-reference** | `main` (`f70a0e3`) | Referência para thunks COM D3D9 de 32 bits e WSI Android |

---

## Arquitetura e Particularidades da MT Framework 1.4

Diferente de executáveis single-thread dos anos 2000, o *Resident Evil 5* opera sobre o motor **MT Framework 1.4 da Capcom**, introduzindo desafios específicos de baixo nível:

```
                      ┌────────────────────────┐
                      │    re5dx9.exe (x86)    │
                      └───────────┬────────────┘
                                  │
                 [ pcrecomp disasm32 ]
                                  │
                    [ lift32_cpu (Reentrante) ]
                                  │
                      ┌───────────▼────────────┐
                      │    ~100.000 Funções C  │
                      │   (~25M-35M linhas)    │
                      └─────┬────────────┬─────┘
                            │            │
       ┌────────────────────┘            └────────────────────┐
       ▼                                                      ▼
 ┌──────────────────────┐                               ┌──────────────────────┐
 │   Host Desktop PC    │                               │    Android ARM64     │
 │ (MSVC x86 / Clang64) │                               │ (NDK Clang + NEON)   │
 ├──────────────────────┤                               ├──────────────────────┤
 │ • Win32 HLE nativo   │                               │ • Memory Mapping 32b │
 │ • DXVK Native Vulkan │                               │ • sse2neon / DMB     │
 │ • FAudio + SDL2      │                               │ • DXVK + Turnip WSI  │
 └──────────────────────┘                               └──────────────────────┘
```

1. **Paralelismo Agressivo & Memory Ordering**:
   * A MT Framework utiliza um sistema multithreaded de tarefas (*Worker Threads*) com *spinlocks* manuais e filas *lock-free*.
   * **x86 TSO vs ARM64 Weak Ordering**: x86 tem consistência de memória forte por hardware e ARM64, ordenação fraca. Primeiro, um **lock global do guest** serializa a execução do código do jogo (correção antes de velocidade); relaxar esse lock com uma estratégia de TSO é uma etapa explícita de otimização (decisão D5 em [ARQUITETURA](docs/ARQUITETURA.md)).
2. **Ponteiros de 32 bits em Hosts de 64 bits**:
   * O código recompilado espera endereços de 32 bits a partir de `0x00400000` (o executável não tem relocações). O runtime reserva 4 GiB contíguos no host e todo acesso do guest soma essa base; objetos de 64 bits do host (COM da DXVK Native, arquivos, threads) chegam ao guest por uma *Handle Table* de 32 bits e vtables de thunks.
3. **Ponto Flutuante e SIMD**:
   * Física Havok 5.5 e transformações de matrizes utilizam SSE/SSE2, que o lifter traduz para C portável (lanes em union, sem intrínsecos); `sse2neon.h` fica para código escrito à mão no runtime.
   * Se o jogo ligar flush-to-zero/denormals-are-zero, o modo é espelhado no registrador `FPCR` do ARM64.

---

## Cronograma de Engenharia

Detalhamento de cada etapa, com o que a IA faz e o que depende de você: [docs/PLANO_DO_PROJETO.md](docs/PLANO_DO_PROJETO.md).

| Fase | Foco Técnico Principal | Meta de Conclusão |
|:---:|---|---|
| **Fase 0** ✅ | **Fundação**<br>Build CMake, runtime base (memória do guest, dispatch, thunks, loader PE, threads, VFS), pipeline recon → catálogo → lift. | Pipeline testado de ponta a ponta com um PE32 sintético (`ctest` 6/6). |
| **Fase 1** ⛔ | **Binário limpo e Análise Estática PE32**<br>Obter código sem SteamStub **e sem Enigma Protector**; seções, IAT real, v-tables e RTTI da MT Framework. | `scripts/recon.py` aprova a entrada; mapeamento em `docs/RECON_RE5.md` e `config/iat_overrides.json`. |
| **Fase 2** | **Lifting x86 → C e Resolução de CFG**<br>Tradução de x86/SSE para código C reentrante (`CpuContext* ctx`) via `pcrecomp`. | Compilação limpa dos arquivos gerados em `generated/src/`. |
| **Fase 3** | **Runtime HLE Win32 e Memória**<br>Mapeamento de espaço virtual de 32 bits, pool de threads POSIX e sistema de arquivos `.arc`. | Binário inicializa o `WinMain` e carrega configurações básicas. |
| **Fase 4** | **Subsistemas Multimídia (PC Target)**<br>Thunks D3D9 para DXVK Native, áudio FAudio/XAudio2 e janela Vulkan via SDL2. | Jogo renderiza menus e entra em gameplay no PC nativo. |
| **Fase 5** | **Cross-Compilation Android ARM64**<br>Compilação via NDK Clang, tradução SIMD via `sse2neon`, alinhamento de memória e WSI Android. | Biblioteca `libre5_recomp.so` gerada e executando em dispositivo Android. |
| **Fase 6** | **Polimento Mobile e Otimização**<br>Overlay touch ImGui, transcodificação/decodificação de cutscenes WMV e tuning Vulkan (Turnip). | 60 FPS estáveis com controles na tela e suporte a gamepad. |

---

## Como Começar

### Pré-requisitos
* **Compilador Desktop**: Visual Studio 2022 (MSVC x86/x64) ou Clang/GCC moderno no Linux.
* **Build System**: CMake 3.22+ e Ninja.
* **Android**: Android Studio, NDK r25+ e CMake.
* **Python**: Python 3.10+ (para os scripts do `pcrecomp` e geradores de ponte COM).
* **Binário Original**: Cópia legítima do jogo *Resident Evil 5: Gold Edition* (Steam).

### Passo 0: Ambiente, build e testes
```bash
pip install -r scripts/requirements.txt
git lfs pull --include='bin_input/*'
python3 scripts/doctor.py
cmake --preset linux-debug && cmake --build --preset linux-debug
ctest --preset linux-debug
```

### Passo 1: Binário de entrada

O binário de entrada (`bin_input/re5dx9.clean.exe`, um executável x86 sem proteção) é fornecido por quem tem o jogo e nunca vai para o git; este repositório não inclui nem documenta como remover proteções de cópia.
Verifique com `python3 scripts/recon.py --all`.

---

## Aviso Legal

Este projeto **não distribui** código binário, assets, texturas, modelos, áudio ou marcas registradas de propriedade da **Capcom Co., Ltd.**

Todos os arquivos originais do jogo devem ser fornecidos pelo usuário a partir de uma cópia legalmente adquirida de *Resident Evil 5*. O código deste repositório consiste estritamente em ferramentas de código aberto e runtimes de compatibilidade de engenharia limpa sob licença MIT.
