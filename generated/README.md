# `generated/` — código C gerado a partir do `re5dx9.exe`

- `src/` — saída do `scripts/run_lift.py lift` (ignorada pelo git: derivada do binário
  da Capcom e com dezenas de milhões de linhas; ver decisão D7). Regenere com:

  ```bash
  python3 scripts/run_lift.py catalog
  python3 scripts/run_lift.py lift
  cmake --build --preset linux-debug     # o CMake detecta src/re5_dispatch_table.c
  ```

- `stub/` — tabela de funções vazia. Enquanto `src/` não existir, o executável `re5`
  linka com ela: carrega a imagem, imprime o censo e avisa que falta a Fase 2.
