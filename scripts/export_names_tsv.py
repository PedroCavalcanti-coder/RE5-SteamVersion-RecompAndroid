#!/usr/bin/env python3
"""Gera work/names.tsv a partir de work/names.json para consumo do runtime em C."""
import json
import os
import sys

import re5common as rc


def main():
    src = rc.repo_path("work/names.json")
    dst = rc.repo_path("work/names.tsv")
    if not os.path.exists(src):
        sys.exit(f"[!] {src} não encontrado.")

    with open(src, "r", encoding="utf-8") as f:
        names = json.load(f)

    entries = []
    for k, v in names.items():
        va = int(k, 16)
        name = v.get("name") if isinstance(v, dict) else v
        entries.append((va, name))

    entries.sort(key=lambda x: x[0])
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        for va, name in entries:
            f.write(f"{va:08X}\t{name}\n")

    print(f"[*] Exportados {len(entries)} nomes para {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
