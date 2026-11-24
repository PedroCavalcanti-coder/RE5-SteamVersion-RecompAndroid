#!/usr/bin/env python3
"""Roda o benchmark variável no celular (adb) sozinho e grava um trace do Perfetto no meio dele.

Sem root: grava o re5_env.txt da pasta do jogo com a entrada roteirizada do bench_re5.py
(RE5_INPUT_SYNC na música do título), abre o GameActivity direto pelo adb, espera a última
tecla do roteiro (o benchmark começa ali), aguarda --delay s e grava --secs s de trace com
escalonamento (sched_switch/waking, estados das threads), clocks de CPU e da GPU. O trace vai
para work/android/<label>.pftrace; scripts/android_trace_report.py o resume.

    python scripts/android_trace.py --label a54_dxvk [--env RE5_GFX=re5vk ...] [--delay 40] [--secs 20]

O re5_env.txt anterior é guardado e restaurado no fim. O renderizador e as outras opções do
launcher continuam valendo (vêm das preferências do app); --env acrescenta variáveis.
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PKG = "com.re5.recompiled"
GAME_DIR = f"/sdcard/Android/data/{PKG}/files/game"
SCRIPT = "300:Return,+500:Down,+250:Down,+250:Return,+300:Return"   # o mesmo do bench_re5.py

TRACE_CFG = """
buffers {{ size_kb: 131072 fill_policy: RING_BUFFER }}
buffers {{ size_kb: 4096 fill_policy: RING_BUFFER }}
data_sources {{
  config {{
    name: "linux.ftrace"
    target_buffer: 0
    ftrace_config {{
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      ftrace_events: "sched/sched_wakeup_new"
      ftrace_events: "sched/sched_process_exit"
      ftrace_events: "power/cpu_frequency"
      ftrace_events: "power/cpu_idle"
      ftrace_events: "power/gpu_frequency"
      ftrace_events: "power/suspend_resume"
      compact_sched {{ enabled: true }}
      symbolize_ksyms: false
    }}
  }}
}}
data_sources {{
  config {{
    name: "linux.process_stats"
    target_buffer: 1
    process_stats_config {{ scan_all_processes_on_start: true }}
  }}
}}
data_sources {{ config {{ name: "linux.sys_stats" target_buffer: 1
  sys_stats_config {{ cpufreq_period_ms: 100 }} }} }}
duration_ms: {ms}
"""


def adb(serial, *args, inp=None, check=False, timeout=120):
    """adb com prazo: um comando preso no aparelho (simpleperf esperando um app que caiu) não
    pode segurar o script para sempre."""
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    try:
        return subprocess.run(cmd, input=inp, capture_output=True, text=inp is None or isinstance(inp, str),
                              errors="replace" if inp is None or isinstance(inp, str) else None, check=check,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        sys.exit(f"[!] adb {' '.join(args)[:80]} passou de {timeout:.0f} s")


def game_pid(serial):
    out = adb(serial, "shell", f"pidof {PKG}:game").stdout.split()
    return int(out[0]) if out else None


def sh(serial, script):
    return adb(serial, "shell", script).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--serial")
    ap.add_argument("--label", required=True)
    ap.add_argument("--env", nargs="*", default=[], help="CHAVE=VALOR a mais no re5_env.txt")
    ap.add_argument("--delay", type=float, default=40, help="s depois da última tecla do roteiro")
    ap.add_argument("--secs", type=float, default=20, help="duração do trace")
    ap.add_argument("--timeout", type=float, default=420, help="desiste se o roteiro não terminar")
    ap.add_argument("--simpleperf", metavar="THREADS",
                    help="em vez do Perfetto: simpleperf com tempo parado e pilhas destas threads (ex.: re5-main,dxvk-submit)")
    args = ap.parse_args()
    s = args.serial
    out_dir = os.path.join(ROOT, "work", "android")
    os.makedirs(out_dir, exist_ok=True)

    old_env = adb(s, "shell", f"cat {GAME_DIR}/re5_env.txt 2>/dev/null").stdout
    env = ["RE5_FPS_LOG=1", "RE5_INPUT_SYNC=titlelogo", "RE5_INPUT_HOLD=20", f"RE5_INPUT_SCRIPT={SCRIPT}"] + args.env
    adb(s, "shell", f"cat > {GAME_DIR}/re5_env.txt", inp="\n".join(env) + "\n")
    print("[*] re5_env.txt:", ", ".join(env[3:]) or "(só o roteiro)")

    logpath = os.path.join(out_dir, f"{args.label}_logcat.txt")
    try:
        adb(s, "shell", f"am force-stop {PKG}")
        adb(s, "logcat", "-c")
        lc = subprocess.Popen(["adb"] + (["-s", s] if s else []) + ["logcat", "-v", "time", "RE5Native:V", "DEBUG:V",
                                                                    "libc:V", "AndroidRuntime:E", "*:S"],
                              stdout=open(logpath, "w", encoding="utf-8", errors="replace"), stderr=subprocess.DEVNULL)
        launch = f"bench-{int(time.time() * 1000)}"
        r = adb(s, "shell", f"am start -n {PKG}/.GameActivity --es LAUNCH_ID {launch} --es GAME_DIR {GAME_DIR}")
        print("[*] jogo aberto:", (r.stdout.strip().splitlines() or ["?"])[-1])

        # espera a última tecla do roteiro no logcat (o benchmark começa ali)
        t0 = time.time()
        keys_seen, n_keys, started = 0, None, None
        while time.time() - t0 < args.timeout:
            time.sleep(2)
            text = open(logpath, encoding="utf-8", errors="replace").read()
            m = re.search(r"RE5_INPUT_SCRIPT: (\d+) teclas", text)
            if m:
                n_keys = int(m.group(1))
            keys_seen = len(re.findall(r"RE5_INPUT_SCRIPT: Present", text))
            if "Fatal signal" in text or "re5_fatal" in text or "FATAL" in text:
                sys.exit("[!] o jogo caiu; veja " + logpath)
            if n_keys and keys_seen >= n_keys:
                started = time.time()
                break
        if not started:
            sys.exit(f"[!] o roteiro não terminou em {args.timeout:.0f} s ({keys_seen}/{n_keys} teclas); veja {logpath}")
        print(f"[*] roteiro terminou ({time.time() - t0:.0f} s depois de abrir); gravo em {args.delay:.0f} s")
        time.sleep(args.delay)
        # O roteiro conta quadros e pode errar o menu (já caiu em "sair"): confere que o jogo está
        # vivo e num trecho de jogo (o menu faz ~500 draws/s; o benchmark, milhares).
        pid = game_pid(s)
        text = open(logpath, encoding="utf-8", errors="replace").read()
        draws = [int(x) for x in re.findall(r"fps [\d.]+ \(Present \d+\).*? draws (\d+)", text)[-5:]]
        if not pid:
            sys.exit(f"[!] o jogo fechou antes da gravação (roteiro errou o menu?); veja {logpath}")
        if not draws or max(draws) < 3000:
            sys.exit(f"[!] o jogo não está no benchmark (draws/s recentes: {draws}); roteiro errou o menu? {logpath}")
        print(f"[*] jogo vivo (pid {pid}), draws/s recentes {draws}")

        if args.simpleperf:
            # Pilhas de chamada com o tempo parado (--trace-offcpu): em que função cada thread
            # dorme. O app de debug é "debuggable", então o simpleperf entra nele com --app.
            dev = "/data/local/tmp/re5.perf.data"
            r = adb(s, "shell", f"simpleperf record -p {pid} -e cpu-clock -f 500 --trace-offcpu --call-graph dwarf "
                                f"--duration {int(args.secs)} -o {dev}", timeout=args.secs + 90)
            if r.returncode:
                sys.exit("[!] simpleperf record falhou:\n" + r.stdout + r.stderr)
            rep = adb(s, "shell", f"simpleperf report -i {dev} --comms {args.simpleperf} --sort comm,symbol "
                                  f"-g caller --percent-limit 1.5 --children", timeout=600)
            local = os.path.join(out_dir, f"{args.label}_simpleperf.txt")
            with open(local, "w", encoding="utf-8") as f:
                f.write(rep.stdout + rep.stderr)
            print(f"[*] simpleperf: {local}")
        else:
            cfg = TRACE_CFG.format(ms=int(args.secs * 1000))
            dev_trace = f"/data/misc/perfetto-traces/{args.label}.pftrace"
            # Ocupação e clock da GPU no aparelho durante o trace (Exynos: /sys/kernel/gpu; Adreno: kgsl).
            gpu_samples = []

            def sample_gpu():
                end = time.time() + args.secs
                while time.time() < end:
                    out = adb(s, "shell", "cat /sys/kernel/gpu/gpu_busy /sys/kernel/gpu/gpu_clock 2>/dev/null || "
                                          "cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage "
                                          "/sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null", timeout=10).stdout.split()
                    if len(out) >= 2:
                        gpu_samples.append((out[0].rstrip("%"), out[-1]))
                    time.sleep(0.5)

            gt = threading.Thread(target=sample_gpu, daemon=True)
            gt.start()
            r = adb(s, "shell", f"perfetto --txt -c - -o {dev_trace}", inp=cfg, timeout=args.secs + 60)
            gt.join(timeout=15)
            if gpu_samples:
                try:
                    busy = [float(b) for b, _ in gpu_samples]
                    clk = [float(c) for _, c in gpu_samples]
                    clk = [c / 1000 if c > 10000 else c for c in clk]   # kHz no Exynos
                    clk = [c / 1e3 if c > 1e5 else c for c in clk]      # Hz no kgsl
                    print(f"[*] GPU no trace: ocupada média {sum(busy) / len(busy):.0f}% (máx {max(busy):.0f}%), "
                          f"clock médio {sum(clk) / len(clk):.0f} MHz (máx {max(clk):.0f}); {len(busy)} amostras")
                except ValueError:
                    print("[*] GPU no trace: amostras ilegíveis", gpu_samples[:3])
            if r.returncode:
                sys.exit("[!] perfetto falhou:\n" + r.stdout + r.stderr)
            local = os.path.join(out_dir, f"{args.label}.pftrace")
            r = adb(s, "pull", dev_trace, local)
            if r.returncode:
                sys.exit("[!] adb pull falhou:\n" + r.stdout + r.stderr)
            print(f"[*] trace: {local} ({os.path.getsize(local) // 1024} KiB)")
        shot = os.path.join(out_dir, f"{args.label}_shot.png")
        with open(shot, "wb") as f:
            f.write(subprocess.run(["adb"] + (["-s", s] if s else []) + ["exec-out", "screencap", "-p"],
                                   capture_output=True).stdout)
        print(f"[*] captura: {shot}")
        time.sleep(3)
    finally:
        adb(s, "shell", f"am force-stop {PKG}")
        try:
            lc.terminate()
        except Exception:
            pass
        adb(s, "shell", f"cat > {GAME_DIR}/re5_env.txt", inp=old_env)
        print("[*] jogo fechado; re5_env.txt anterior restaurado")


if __name__ == "__main__":
    main()
