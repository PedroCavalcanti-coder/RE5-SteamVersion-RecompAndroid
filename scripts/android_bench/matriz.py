#!/usr/bin/env python3
"""Matriz de builds no A54: para cada variante instala o APK, roda o benchmark (aquecimento + 2 medidas)
e imprime fps média/mediana/p10 e GPU. Uso: matriz.py <pasta_saida>"""
import os
import re
import statistics as st
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ADB = r"C:\Users\Dev_RV01\AppData\Local\Android\Sdk\platform-tools\adb.exe"
WORK = r"C:\Users\Dev_RV01\Documents\Github\re5-recompiled\work\android"
OUT = sys.argv[1]
ONLY = sys.argv[2].split(",") if len(sys.argv) > 2 else None
V = [
    ("A_antigo_clang18", "app-ref49-ndk27.apk", []),
    ("C_atual_clang18", "app-atual-ndk27.apk", []),
    ("C2_atual_clang18_dxvk_antiga", "app-C2.apk", ["RE5_D3D9_LIB=libdxvk_d3d9_ref.so"]),
    ("D_javaantigo_nativoatual_clang18", "app-D.apk", []),
    ("B_atual_clang21", "app-B.apk", []),
]
COMMON = ["RE5_FPS_LOG=1", "RE5_GFX=dxvk"]


def fps_of(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    i0 = max((i for i, l in enumerate(lines) if "RE5_INPUT_SCRIPT: Present" in l), default=0)
    rows = []
    for l in lines[i0:]:
        m = re.search(r"fps ([\d.]+) \(Present \d+\)", l)
        if m:
            rows.append(float(m.group(1)))
    rows = rows[5:]
    return rows


def clocks(path):
    try:
        r = [l.split() for l in open(path).read().splitlines() if len(l.split()) == 4]
        return (st.mean(int(x[0].rstrip("%")) for x in r), st.mean(int(x[1]) for x in r) / 1000)
    except Exception:
        return (-1, -1)


res = []
for name, apk, extra in V:
    if ONLY and name.split("_")[0] not in ONLY:
        continue
    subprocess.run([ADB, "shell", f"am force-stop com.re5.recompiled"])
    r = subprocess.run([ADB, "install", "-r", os.path.join(WORK, apk)], capture_output=True, text=True)
    print(name, "instalado:", r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-80:], flush=True)
    for k, tag in enumerate(["m1"]):
        d = os.path.join(OUT, f"{name}_{tag}")
        subprocess.run([sys.executable, os.path.join(HERE, "bench_android.py"), d, "90"] + COMMON + extra,
                       capture_output=True, text=True)
        rows = fps_of(os.path.join(d, "logcat.txt")) if os.path.exists(os.path.join(d, "logcat.txt")) else []
        gu, gm = clocks(os.path.join(d, "clocks.txt"))
        if rows:
            line = (f"{name:36s} {tag:6s} n={len(rows):3d} média {st.mean(rows):5.1f} mediana {st.median(rows):5.1f} "
                    f"p10 {sorted(rows)[len(rows) // 10]:5.1f} | GPU {gu:3.0f}% {gm:4.0f} MHz")
        else:
            line = f"{name:36s} {tag:6s} sem dados"
        print(line, flush=True)
        if tag != "aquece":
            res.append((name, st.mean(rows) if rows else 0))
print("== resumo (média das 2 medidas)")
for name, _, _ in V:
    v = [x for n, x in res if n == name]
    if v:
        print(f"{name:36s} {st.mean(v):5.1f} fps")
subprocess.run([ADB, "shell", "cat > /storage/emulated/0/Android/data/com.re5.recompiled/files/game/re5_env.txt"],
               input="", text=True)
