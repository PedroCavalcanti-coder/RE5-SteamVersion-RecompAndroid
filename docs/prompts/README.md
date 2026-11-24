# Prompts de trabalho (23/11/2026)

Cada arquivo é um prompt para colar numa sessão nova de IA aberta na raiz do repositório. Todos partem da base descrita em
`00_BASE_LEAN_2026-11-23.md` (build "lean": NDK r27, DXVK 1.9.2b, modo só o jogo; 35 de pico no A54).

| Arquivo | Tarefa |
|---|---|
| `01_otimizacao_profunda_cpu.md` | reduzir CPU por quadro (memmove, esperas, Java, código gerado, carregamento) |
| `02_vulkan11_nativo_novo.md` | backend Vulkan 1.1 novo e limpo, para passar a DXVK |
| `03_toque_nativo_no_jogo.md` | toque reconhecido e desenhado dentro do jogo, sem camada Java nem controle emulado |
| `04_mods_ptbr_e_ia_sheva.md` | tradução PT-BR e um mod grande com IA da Sheva melhorada |

Antes de começar qualquer um: commitar e marcar o estado atual do working tree (`git tag base-lean-2026-11-23`).
