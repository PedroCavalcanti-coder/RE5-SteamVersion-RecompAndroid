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


SIG = re.compile(r"^static inline [^\n(]*?\b(\w+) ?\(uint32_t a\b([^)\n]*)\)", re.M)
EXPECTED_WIDE = 33   # funções com endereço do guest no 1º parâmetro (as atômicas contam 2x: #if/#else)


def wide_accessors(text):
    """Endereço largo (lift.wide_addr, re5/cpu_prelude.h): cada função que recebe o endereço
    do guest em `uint32_t a` passa a receber `re5_gaddr a` (64 bits, somado à base sem
    truncar) e ganha, logo depois da definição, uma macro com o mesmo nome que converte o
    argumento por RE5_GA: um uint32_t continua truncado como antes, um re5_wide passa inteiro.
    A macro vem depois da definição para não reescrever a própria definição."""
    out, pos, n = [], 0, 0
    for m in SIG.finditer(text):
        if m.start() < pos:
            continue
        name, rest = m.group(1), m.group(2)
        line_end = text.index("\n", m.end())
        if text[m.start():line_end].rstrip().endswith("}"):
            end = line_end            # função de uma linha
        else:
            end = text.index("\n}", m.end()) + 2
        body = text[m.start():end]
        if "RE5_G2H(a)" not in body:
            continue
        nparams = rest.count(",")
        body = body.replace("(uint32_t a", "(re5_gaddr a", 1).replace("RE5_G2H(a)", "RE5_G2HW(a)")
        ps = "".join(f", _p{i}" for i in range(nparams))
        out.append(text[pos:m.start()])
        out.append(body + f"\n#define {name}(a{ps}) {name}(RE5_GA(a){ps})")
        pos = end
        n += 1
    out.append(text[pos:])
    if n != EXPECTED_WIDE:
        sys.exit(f"gen_cpu_header: {n} funções com endereço do guest, esperado {EXPECTED_WIDE}.")
    text = "".join(out)
    if "RE5_G2H(a)" in text:
        sys.exit("gen_cpu_header: sobrou RE5_G2H(a) fora de uma função reconhecida")
    return text


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
    out = wide_accessors(out)
    header = ("/* GERADO por scripts/gen_cpu_header.py a partir de tools/pcrecomp/"
              "runtime/recomp32_cpu/cpu.h - não edite. */\n"
              '#include "re5/cpu_prelude.h"\n')
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + out + '\n#include "re5/cpu_epilogue.h"\n')


if __name__ == "__main__":
    main()
