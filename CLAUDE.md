# CLAUDE.md — guia para sessões de IA neste repositório

Recompilação estática do `re5dx9.exe` (Resident Evil 5, Steam, x86-32) para C,
rodando em Linux x64 e Android ARM64. Leia antes de qualquer trabalho:

1. `docs/STATUS.md` — o que está feito, com evidência, e o que bloqueia.
2. `docs/PLANO_DO_PROJETO.md` — fases, etapas (1.1, 2.3...) e quem faz o quê.
3. `docs/ARQUITETURA.md` — decisões D1–D9. Não contrarie uma decisão sem mudar o documento.

## Comandos

```bash
python3 scripts/doctor.py                      # sempre primeiro: o ambiente está pronto?
python3 scripts/recon.py --all                 # portão: o executável é liftable?
bash scripts/build_ffmpeg.sh                   # FFmpeg mínimo dos vídeos, antes do 1º cmake
cmake --preset linux-debug && cmake --build --preset linux-debug
ctest --preset linux-debug                     # tem que ficar verde antes de todo push
python3 scripts/run_lift.py catalog            # Fase 2 (recusa entrada protegida)
python3 scripts/run_lift.py lift
./build/linux-debug/re5 --census --missing --exe bin_input/re5dx9.clean.exe
```

Dependências Python: `pip install -r scripts/requirements.txt` (o `scripts/bootstrap.sh`
faz isso pelo hook de SessionStart). Arquivos do jogo: não estão no git; quem tem o jogo
os põe em `bin_input/` (`re5dx9.clean.exe` e a pasta `bin_input/game` com `nativePC_MT`).

## Regras

- **Nunca commitar** `generated/src/`, `work/`, `build/` (derivados do binário; ver D7).
  **Nunca commitar arquivos do jogo** (executáveis, despejos, dados, saves): `bin_input/`
  é ignorado pelo git, inclusive por LFS.
- **Falhar alto** (D8): nada de stub que "retorna 0" para fazer avançar. Import sem
  HLE aborta com o nome; instrução sem tradução aborta com o endereço.
- Mudou o runtime ou os scripts? Teste novo ou estendido em `tests/`, e `ctest` verde.
- `STATUS.md` só recebe "FEITO" com evidência (números, log, captura) da própria sessão.
- `tools/pcrecomp/` é vendorizado: mudanças no lifter vão com teste e com nota no
  `STATUS.md` (são o que mais facilmente quebra o código gerado inteiro).
- Documentação, comentários e mensagens ao usuário em **português**.

## Convenções de código

- C11; prefixo `re5_` em tudo que é público; headers em `runtime/include/re5/`.
- O código gerado e o runtime incluem `"cpu.h"` — o gerado em
  `build/*/gen_include/` por `scripts/gen_cpu_header.py` (não edite à mão).
- Handler de import: `static void k_<Nome>(CPU *c)`, argumentos com `re5_arg(c, i)`,
  resultado em `c->eax`; os bytes de argumento do stdcall ficam na tabela do módulo,
  nunca no handler. Handler que bloqueia usa `RE5_BLOCKING_BEGIN/END`.
- Novo módulo HLE (`runtime/src/win32/<dll>.c`): exporte `re5_mod_<dll>`, registre em
  `runtime/src/core/imports.c` e acrescente ao `runtime/CMakeLists.txt`.
- Memória do guest só por `rd*/wr*` (cpu.h) ou `re5_g2h()`; nunca guarde ponteiros do
  host em memória do guest (use `re5_handle_new`).

## Mapa

```
scripts/        pipeline (recon, run_lift, gen_cpu_header, doctor, bootstrap)
config/         re5_config.toml, known_builds.json, overrides/exclude/hooks, iat_overrides.json
runtime/        include/re5/*.h, src/core (memória, dispatch, thunks, imports, loader, threads, vfs),
                src/win32 (HLE), src/host/main.c (executável re5)
generated/      stub/ (tabela vazia antes da Fase 2); src/ é gerado e ignorado pelo git
tests/          unit/ (C), e2e/ (PE32 sintético pelo pipeline inteiro), python/
tools/          pcrecomp (lifter), nfsu2-reference (projeto irmão: Android/DXVK)
third_party/    dxvk-native, FAudio, SDL2, FFmpeg, imgui, sse2neon, libadrenotools
```
