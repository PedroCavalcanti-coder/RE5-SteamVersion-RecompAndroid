#!/usr/bin/env python3
"""Resume o fps do benchmark (linhas do RE5_FPS_LOG depois da última tecla do roteiro)."""
import re
import statistics as st
import sys

for path in sys.argv[1:]:
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    i0 = max((i for i, l in enumerate(lines) if "RE5_INPUT_SCRIPT: Present" in l), default=0)
    rows = []
    for l in lines[i0:]:
        m = re.search(r"fps ([\d.]+) \(Present \d+\).*?draws (\d+).*?present_ms ([\d.]+)", l)
        if m:
            rows.append((float(m.group(1)), int(m.group(2)), float(m.group(3))))
    rows = rows[5:]   # descarta a transição para o benchmark
    if not rows:
        print(path, "sem dados")
        continue
    f = [r[0] for r in rows]
    print(f"{path}: {len(rows)} s | fps média {st.mean(f):.1f} mediana {st.median(f):.1f} p10 {sorted(f)[len(f) // 10]:.1f} "
          f"| draws/quadro {sum(r[1] for r in rows) / max(sum(f), 1):.0f} | present_ms/s {st.mean(r[2] for r in rows):.0f}")
