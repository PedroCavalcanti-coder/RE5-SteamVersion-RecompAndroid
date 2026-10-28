#!/usr/bin/env python3
"""Roda o benchmark variável do RE5 sozinho e mede o fps (para comparar builds).

Entrada roteirizada sincronizada na música do título (RE5_INPUT_SYNC), fps por
segundo do runtime (RE5_FPS_LOG) e a tela de resultados do jogo capturada no fim.

    python scripts/bench_re5.py [--label nome] [--secs 420] [--env CHAVE=VALOR ...]

Escreve work/bench/<nome>/run.log e imprime média, mediana e p10 do fps
durante o benchmark (do primeiro ao último segundo com fps > 5 depois da tecla
que o inicia).
"""
import argparse
import os
import re
import statistics
import subprocess
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SCRIPT = "300:Return,+500:Down,+250:Down,+250:Return,+300:Return"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--label", default="base")
    ap.add_argument("--secs", type=float, default=420)
    ap.add_argument("--env", nargs="*", default=[])
    ap.add_argument("--re5", default=os.path.join(ROOT, "build", "test-build", "re5.exe"), help="executável a medir")
    ap.add_argument("--game-dir", default=os.path.join(ROOT, "bin_input", "game"))
    args = ap.parse_args()
    out = os.path.join(ROOT, "work", "bench", args.label)
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "run.log")
    env = dict(os.environ, RE5_INPUT_SYNC="titlelogo", RE5_INPUT_HOLD="20", RE5_INPUT_SCRIPT=SCRIPT, RE5_FPS_LOG="1",
               RE5_LOG_FILE=log, DXVK_LOG_PATH=out)
    for k in ("RE5_PROFILE", "RE5_WATCHDOG", "RE5_DSOUND_DUMP", "RE5_D3D9_SHOT", "DXVK_HUD"):
        env.pop(k, None)
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v
    p = subprocess.Popen([args.re5, "--game-dir", args.game_dir, "--exe",
                          "bin_input/re5dx9.clean.exe"], cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < args.secs and p.poll() is None:
        time.sleep(1)
    crashed = p.poll() is not None
    subprocess.run(["taskkill", "/F", "/IM", "re5.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = open(log, encoding="utf-8", errors="replace").read()
    start = None
    for line in text.splitlines():
        if "RE5_INPUT_SCRIPT: Present" in line:
            start = int(re.search(r"Present (\d+)", line).group(1))      # a última tecla inicia o benchmark
    fps = []
    for m in re.finditer(r"fps ([\d.]+) \(Present (\d+)\)", text):
        if start is not None and int(m.group(2)) > start + 300:
            fps.append(float(m.group(1)))
    while fps and fps[-1] > 100:   # tela de resultados (sem limite) depois do fim
        fps.pop()
    errors = [l for l in text.splitlines() if "[ERROR]" in l][:3]
    if not fps:
        print(f"[{args.label}] sem medida (crash={crashed}) {errors}")
        return
    q = sorted(fps)
    print(f"[{args.label}] {len(fps)} s de benchmark: média {statistics.mean(fps):.1f} fps, mediana "
          f"{statistics.median(fps):.1f}, p10 {q[len(q) // 10]:.1f}, mín {q[0]:.1f}"
          + (f"  ERROS: {errors}" if errors else ""))


if __name__ == "__main__":
    main()
