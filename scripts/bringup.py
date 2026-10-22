#!/usr/bin/env python3
"""Etapa 2.6 em laço: roda o re5 até ele parar num endereço sem função gerada,
semeia o catálogo com ele, refaz o lift (só as janelas afetadas) e o build, e
roda de novo. Para no primeiro erro que não é de catálogo (import sem HLE,
instrução sem tradução, crash) e mostra o fim do log.

    bringup.py [--build-dir build/test-build] [--game-dir DIR] [--max-rounds 50]

Cada volta deixa o log em work/bringup/run_NNN.log. As sementes acumulam em
work/runtime_seeds.txt (o mesmo arquivo serve para um `catalog --seeds` completo
mais tarde, para a descoberta incorporar essas entradas como funções de verdade).
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re5common as rc  # noqa: E402

DEFAULT_GAME = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "bin_input", "game")


def run(cmd, **kw):
    print("[$] " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, **kw)


def seed_count(path):
    """Sementes distintas. O runtime registra a mesma de novo a cada execução
    que para nela; uma repetida não é progresso (o catálogo já a recusou)."""
    if not os.path.exists(path):
        return 0
    with open(path, encoding="utf-8") as f:
        return len({line.split()[0].lower() for line in f if line.strip()})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default=rc.repo_path("build/test-build"))
    ap.add_argument("--game-dir", default=DEFAULT_GAME)
    ap.add_argument("--exe", default=rc.repo_path(rc.load_config()["input"]["clean"]))
    ap.add_argument("--max-rounds", type=int, default=50)
    ap.add_argument("--timeout", type=int, default=600, help="segundos por execução do re5")
    args = ap.parse_args()

    seeds = rc.repo_path("work/runtime_seeds.txt")
    logdir = rc.repo_path("work/bringup")
    os.makedirs(logdir, exist_ok=True)
    re5 = os.path.join(args.build_dir, "re5.exe" if os.name == "nt" else "re5")
    scripts = os.path.dirname(os.path.abspath(__file__))

    for rnd in range(1, args.max_rounds + 1):
        log = os.path.join(logdir, f"run_{rnd:03d}.log")
        env = dict(os.environ, RE5_SEED_LOG=seeds, RE5_LOG_FILE=log)
        before = seed_count(seeds)
        t = time.time()
        try:
            p = run([re5, "--game-dir", args.game_dir, "--exe", args.exe], env=env,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=args.timeout)
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        print(f"[*] volta {rnd}: re5 saiu com {code} em {time.time() - t:.0f}s; log em {log}")
        if seed_count(seeds) == before:
            print("[*] parou por outro motivo que não catálogo (ou numa semente que o catálogo já recusou). "
                  "Fim do log:")
            with open(log, encoding="utf-8", errors="replace") as f:
                tail = f.read().splitlines()[-40:]
            print("\n".join("    " + line for line in tail))
            return 0
        with open(seeds, encoding="utf-8") as f:
            print(f"[*] semente nova: {f.read().split()[-1]}")
        for cmd in ([sys.executable, os.path.join(scripts, "run_lift.py"), "catalog", "--add-seeds", seeds],
                    [sys.executable, os.path.join(scripts, "run_lift.py"), "lift"],
                    ["cmake", "--build", args.build_dir, "-j", str(os.cpu_count() or 4)]):
            if run(cmd).returncode != 0:
                print("[!] falhou; parando")
                return 1
    print(f"[!] {args.max_rounds} voltas sem chegar a um erro que não seja de catálogo")
    return 2


if __name__ == "__main__":
    sys.exit(main())
