#!/usr/bin/env python3
"""Gera o cpu.h usado pelo runtime e pelo código gerado, a partir do pcrecomp.

    python3 scripts/gen_cpu_header.py <pcrecomp cpu.h> <saída cpu.h>

O runtime recomp32_cpu do pcrecomp supõe um host de 32 bits: um registrador
do guest É um ponteiro do host (`*(uint32_t *)(uintptr_t)a`). Isso só vale no
Windows x86. No Linux x64 e no Android ARM64 o espaço de 4 GiB do guest é uma
reserva que começa em `re5_guest_base`, e todo acesso precisa somar essa base.

Em vez de manter um fork à mão do cpu.h (e perder as correções do upstream),
este script faz três reescritas mecânicas e verificadas:

  1. cada `(uintptr_t)a` vira `(uintptr_t)RE5_G2H(a)` (soma a base do guest);
  2. as leituras/escritas escalares `*(uintN_t *)` passam a usar tipos com
     alinhamento 1 (RE5_UA_*): x86 aceita acesso desalinhado, o C não, e um
     compilador ARM64 pode gerar instruções que exigem alinhamento;
  3. prefixa runtime/include/re5/cpu_prelude.h e anexa cpu_epilogue.h
     (ABI de dispatch que o código gerado chama).

Ele FALHA se os padrões mudarem no upstream — melhor um erro de build do que
um acesso de memória sem a base. O CMake roda este script automaticamente.
"""
import re
import sys

ADDR = re.compile(r"\(uintptr_t\)a\b")
SCALAR = re.compile(r"\*\(uint(8|16|32)_t\s*\*\)\(uintptr_t\)RE5_G2H\(a\)")
# Quantidades medidas no cpu.h vendorizado. Se o upstream mudar, os números
# mudam e alguém precisa revisar se os novos acessos seguem o padrão.
EXPECTED_ADDR = 33   # +1: rep_ptr (rep stos/movs em bloco)
EXPECTED_SCALAR = 6


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="utf-8").read()
    n = len(ADDR.findall(text))
    if n != EXPECTED_ADDR:
        sys.exit(f"gen_cpu_header: {n} acessos '(uintptr_t)a' em {src}, esperado {EXPECTED_ADDR}. "
                 "O cpu.h do pcrecomp mudou: revise os novos acessos e atualize EXPECTED_ADDR.")
    out = ADDR.sub("(uintptr_t)RE5_G2H(a)", text)
    n = len(SCALAR.findall(out))
    if n != EXPECTED_SCALAR:
        sys.exit(f"gen_cpu_header: {n} acessos escalares, esperado {EXPECTED_SCALAR}.")
    out = SCALAR.sub(lambda m: f"*(RE5_UA_U{m.group(1)} *)(uintptr_t)RE5_G2H(a)", out)
    # Qualquer outro cast de endereço do guest para ponteiro escaparia da base.
    leftovers = [l for l in out.splitlines() if "(uintptr_t)" in l and "RE5_G2H" not in l]
    if leftovers:
        sys.exit("gen_cpu_header: casts para ponteiro sem RE5_G2H:\n  " + "\n  ".join(leftovers))
    header = ("/* GERADO por scripts/gen_cpu_header.py a partir de tools/pcrecomp/"
              "runtime/recomp32_cpu/cpu.h - não edite. */\n"
              '#include "re5/cpu_prelude.h"\n')
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + out + '\n#include "re5/cpu_epilogue.h"\n')


if __name__ == "__main__":
    main()
