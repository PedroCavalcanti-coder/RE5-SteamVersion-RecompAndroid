#!/usr/bin/env python3
"""Perfil por instrução do guest: de onde vem o tempo DENTRO do código gerado.

O RE5_PROFILE diz em que função gerada as threads estão; com RE5_PROFILE_RIPS=<arquivo> ele
também grava cada endereço do host amostrado no código gerado ("0xENDEREÇO contagem" por
linha, endereços como no `nm re5.exe`). Este script passa os endereços pelo addr2line (o
build RelWithDebInfo tem -g), acha a linha do C gerado e, pelo comentário que o lift32_cpu
põe em cada linha (`/* VA: mnemônico operandos */`), a instrução x86 de origem. Agrega por
mnemônico, por classe (memória, x87, SSE, chamada, flags...) e lista as linhas mais quentes.

    RE5_PROFILE=60 RE5_PROFILE_RIPS=work/prof_rips.txt <jogo ou bench_re5.py --env ...>
    python scripts/prof_lines.py work/prof_rips.txt [--exe build/windows-x64/re5.exe]
    python scripts/prof_lines.py work/prof_rips.txt --write-hot config/hot_functions.txt --hot 2000

O -O1 move código entre linhas, então a atribuição é aproximada por instrução, mas as
proporções por classe são confiáveis.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
COMMENT = re.compile(r"/\* ([0-9A-F]{8}): (\S+)\s*(.*?)\s*\*/")


def classify(mn, ops, code):
    if mn in ("call",) or "RE5_CALL" in code or "dispatch(" in code:
        return "call/dispatch"
    if mn.startswith("ret"):
        return "ret"
    if mn.startswith("f"):
        return "x87"
    if re.match(r"(v)?(mov|add|sub|mul|div|min|max|sqrt|rcp|rsqrt|cmp|and|or|xor|unpck|shuf|cvt|comi|ucomi|pand|por|pxor|padd|psub|pmul|psll|psrl|psra|pcmp|pack|punpck|movs|movap|movup|movl|movh|movd|movq)\w*(ss|sd|ps|pd)?$", mn) and ("xmm" in ops):
        return "SSE"
    if mn.startswith("j") or mn in ("loop",):
        return "desvio"
    if mn in ("push", "pop", "pushal", "popal", "pushfd", "popfd", "leave"):
        return "pilha (push/pop)"
    if "ptr [" in ops:
        if mn.startswith("lea"):
            return "lea"
        return "inteiro com memória"
    if mn in ("cmp", "test"):
        return "cmp/test"
    return "inteiro em registrador"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rips")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "windows-x64", "re5.exe"))
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--write-hot", help="grava as funções geradas mais amostradas (para o run_lift agrupar)")
    ap.add_argument("--hot", type=int, default=2000, help="quantas funções em --write-hot")
    args = ap.parse_args()

    hits = collections.Counter()
    for line in open(args.rips, encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) == 2 and p[0].startswith("0x"):
            hits[int(p[0], 16)] += int(p[1])
    if not hits:
        sys.exit("[!] arquivo sem amostras")
    addrs = sorted(hits)
    r = subprocess.run(["addr2line", "-e", args.exe], input="\n".join(f"0x{a:x}" for a in addrs),
                       capture_output=True, text=True)
    locs = r.stdout.splitlines()
    if len(locs) != len(addrs):
        sys.exit(f"[!] addr2line devolveu {len(locs)} linhas para {len(addrs)} endereços")

    files = {}

    def src_line(path, n):
        if path not in files:
            try:
                files[path] = open(path, encoding="utf-8", errors="replace").read().split("\n")
            except OSError:
                files[path] = []
        f = files[path]
        return f[n - 1] if 0 < n <= len(f) else ""

    by_mn, by_cls, by_line = collections.Counter(), collections.Counter(), collections.Counter()
    line_text = {}
    total = sum(hits.values())
    unmapped = 0
    for a, loc in zip(addrs, locs):
        n = hits[a]
        m = re.match(r"(.*):(\d+)", loc.split(" (")[0])
        if not m or m.group(1) == "??":
            unmapped += n
            continue
        code = src_line(m.group(1), int(m.group(2)))
        c = COMMENT.search(code)
        if not c:
            by_cls["(linha sem instrução: prólogo/declarações)"] += n
            continue
        va, mn, ops = c.group(1), c.group(2), c.group(3)
        by_mn[mn] += n
        by_cls[classify(mn, ops, code)] += n
        key = (va, mn, ops)
        by_line[key] += n
        line_text[key] = code.split("/*")[0].strip()

    if args.write_hot:
        write_hot(args, hits)
    print(f"{total} amostras no código gerado; {unmapped} sem linha ({100.0 * unmapped / total:.1f}%)\n")
    print("Por classe de instrução do guest:")
    for k, v in by_cls.most_common():
        print(f"  {100.0 * v / total:5.1f}%  {k}")
    print("\nPor mnemônico:")
    for k, v in by_mn.most_common(20):
        print(f"  {100.0 * v / total:5.1f}%  {k}")
    print(f"\n{args.top} instruções mais quentes:")
    for (va, mn, ops), v in by_line.most_common(args.top):
        print(f"  {100.0 * v / total:5.2f}%  {va}  {mn} {ops}")
        print(f"          {line_text[(va, mn, ops)][:150]}")


def write_hot(args, hits):
    """Função gerada de cada amostra pelo `nm` (símbolos L_<VA>), em ordem de amostras."""
    import bisect
    out = subprocess.run(["nm", args.exe], capture_output=True, text=True).stdout
    syms = sorted((int(a, 16), int(n[2:], 16)) for a, t, n in
                  (l.split() for l in out.splitlines() if len(l.split()) == 3)
                  if t in "tT" and re.fullmatch(r"L_[0-9A-F]{8}", n))
    starts = [s[0] for s in syms]
    per_fn = collections.Counter()
    for rip, n in hits.items():
        i = bisect.bisect_right(starts, rip) - 1
        if i >= 0:
            per_fn[syms[i][1]] += n
    total = sum(per_fn.values())
    with open(args.write_hot, "w", encoding="utf-8") as f:
        f.write("# Funções geradas mais quentes, da mais para a menos amostrada (scripts/prof_lines.py\n"
                "# --write-hot, do RE5_PROFILE_RIPS em jogo). O run_lift.py põe cada uma na seção\n"
                "# .text$hNNNNN para o código quente ficar contíguo. Refaça depois de mudar o catálogo.\n")
        acc = 0
        for va, n in per_fn.most_common(args.hot):
            acc += n
            f.write(f"0x{va:08X}  {100.0 * n / total:6.3f}%  acumulado {100.0 * acc / total:5.1f}%\n")
    print(f"[*] {min(args.hot, len(per_fn))} funções em {args.write_hot} ({100.0 * acc / total:.1f}% das amostras)\n")


if __name__ == "__main__":
    main()
