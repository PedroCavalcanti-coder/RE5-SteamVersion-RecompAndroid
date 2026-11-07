#!/usr/bin/env python3
"""Mede no celular (adb) onde vai o quadro: thread do jogo, DXVK, driver ou GPU (fase A0).

É o thread_cpu.py do Android. Sem root e sem injetar nada: lê /proc/<pid>/task/*/stat do
processo do jogo (com.re5.recompiled:game) pelo `adb shell`, a frequência dos núcleos, o uso
da GPU quando o kernel o expõe (Mali da Samsung, Adreno, MediaTek) e, do logcat, as linhas do
RE5_FPS_LOG (fps, ms dentro do Direct3D do host e do Present, espera no lock do guest).

    # no launcher, em Avançado: RE5_FPS_LOG=1   (vai para re5_env.txt)
    python scripts/android_perf.py --secs 60 --label base      # entre na cena e rode

Imprime, por grupo (jogo = threads do guest, dxvk = dxvk-*, driver = mali-*/kgsl/adreno,
outros), os núcleos usados e os ms de CPU por quadro; as threads mais ocupadas (e se estão
num núcleo grande ou pequeno); e um veredito: GPU no limite, thread do jogo no limite (e
quanto dela é Direct3D) ou serialização no lock do guest. Com --out grava o relatório.
"""
import argparse
import re
import statistics
import subprocess
import sys
import threading
import time

PKG_PROC = "com.re5.recompiled:game"

# Uso da GPU: o primeiro caminho legível vence. Formatos: "45 %" / "45" (porcentagem) ou
# "ocupado total" (kgsl gpubusy).
GPU_FILES = [
    "/sys/kernel/gpu/gpu_busy",                          # Samsung Exynos (Mali)
    "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",      # Adreno
    "/sys/class/kgsl/kgsl-3d0/gpubusy",                  # Adreno (ocupado total)
    "/sys/kernel/ged/hal/gpu_utilization",               # MediaTek ("ocupado ocioso bloqueado")
    "/sys/class/misc/mali0/device/utilization",          # alguns Mali
]
# Clock da GPU. Unidades variam (Exynos kHz, kgsl e devfreq Hz, GED da MediaTek "opp kHz"):
# read_clock_mhz normaliza. O glob do devfreq pega Mali com devfreq do kernel (MediaTek, Tensor).
GPU_CLOCK_FILES = ["/sys/kernel/gpu/gpu_clock", "/sys/class/kgsl/kgsl-3d0/gpuclk",
                   "/sys/kernel/ged/hal/current_freqency", "/sys/class/devfreq/*mali*/cur_freq",
                   "/sys/class/misc/mali0/device/cur_freq"]
GPU_MAXCLOCK_FILES = ["/sys/kernel/gpu/gpu_max_clock", "/sys/class/kgsl/kgsl-3d0/max_gpuclk",
                      "/sys/class/devfreq/*mali*/max_freq", "/sys/class/misc/mali0/device/max_freq"]

FPS_RE = re.compile(r"fps ([\d.]+) \(Present \d+\).*?lock_ms ([\d.]+)(?: d3d9_ms ([\d.]+) present_ms ([\d.]+))?")


def adb(args, serial, **kw):
    cmd = ["adb"] + (["-s", serial] if serial else []) + args
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def shell(script, serial):
    return adb(["shell", script], serial).stdout


def find_pid(serial, proc_name):
    out = shell(f"pidof {proc_name}", serial).split()
    return int(out[0]) if out else None


def group_of(name):
    if name == "re5-main" or re.fullmatch(r"g[0-9A-F]{8}", name):
        return "jogo"
    if name.startswith("dxvk-"):
        return "dxvk"
    if name.startswith("mali-") or name.startswith("kgsl") or "dreno" in name or name.startswith("GPU"):
        return "driver"
    return "outros"


def parse_stats(text):
    """Linhas de /proc/<pid>/task/*/stat -> {tid: (nome, ticks, núcleo)}."""
    rows = {}
    for line in text.splitlines():
        lp, rp = line.find("("), line.rfind(")")
        if lp < 0 or rp < lp:
            continue
        try:
            tid = int(line[:lp])
            f = line[rp + 2:].split()
            # f[0] = campo 3 (estado): utime = 14, stime = 15, processor = 39.
            rows[tid] = (line[lp + 1:rp], int(f[11]) + int(f[12]), int(f[36]) if len(f) > 36 else -1)
        except (ValueError, IndexError):
            continue
    return rows


def read_clock_mhz(text):
    """Clock da GPU em MHz, de Hz, kHz ou MHz; no GED da MediaTek ("opp kHz") o último número."""
    nums = [int(x) for x in re.findall(r"\d+", text)]
    if not nums:
        return None
    v = nums[-1] if len(nums) >= 2 else nums[0]
    if v >= 10_000_000:      # Hz
        return v / 1e6
    if v >= 10_000:          # kHz
        return v / 1e3
    return float(v)


def read_gpu(text, path=None):
    if path and "/ged/" in path:   # MediaTek: "ocupado ocioso bloqueado", em %
        m = re.search(r"(\d+)", text)
        return float(m.group(1)) if m else None
    m = re.search(r"(\d+)\s+(\d+)", text)
    if m and int(m.group(2)) > 0 and "%" not in text:   # kgsl gpubusy: ocupado total
        return 100.0 * int(m.group(1)) / int(m.group(2))
    m = re.search(r"(\d+(?:\.\d+)?)", text)
    return float(m.group(1)) if m else None


def core_classes(serial):
    """Núcleos agrupados pela frequência máxima: o grupo mais alto é o dos grandes."""
    out = shell("for c in /sys/devices/system/cpu/cpu[0-9]*; do echo ${c##*cpu} $(cat $c/cpufreq/cpuinfo_max_freq); done", serial)
    mx = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and p[1].isdigit():
            mx[int(p[0])] = int(p[1])
    if not mx:
        return {}, {}
    levels = sorted(set(mx.values()))
    names = {lv: ("pequeno" if i == 0 and len(levels) > 1 else "grande" if i == len(levels) - 1 else "médio")
             for i, lv in enumerate(levels)}
    return {c: names[f] for c, f in mx.items()}, mx


class Logcat(threading.Thread):
    """Linhas do RE5_FPS_LOG que chegam durante a amostragem."""

    def __init__(self, serial):
        super().__init__(daemon=True)
        cmd = ["adb"] + (["-s", serial] if serial else []) + ["logcat", "-T", "1", "-v", "brief", "RE5Native:I", "*:S"]
        self.p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
        self.samples = []
        self.cpu_lines = []
        self.on = False

    def run(self):
        for line in self.p.stdout:
            if not self.on:
                continue
            m = FPS_RE.search(line)
            if m:
                self.samples.append(tuple(float(x) if x is not None else None for x in m.groups()))
            elif "] cpu " in line:   # "[INFO ] cpu       ..." (re5_log_process_cpu)
                self.cpu_lines.append(line.rstrip())

    def stop(self):
        self.p.terminate()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--serial", help="aparelho (adb -s)")
    ap.add_argument("--process", default=PKG_PROC)
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--interval", type=float, default=2)
    ap.add_argument("--wait", type=float, default=300, help="quanto esperar o processo aparecer")
    ap.add_argument("--label", default="", help="nome da medida no relatório")
    ap.add_argument("--out", help="grava o relatório neste arquivo também")
    args = ap.parse_args()
    s = args.serial

    if adb(["get-state"], s).stdout.strip() != "device":
        sys.exit("android_perf: nenhum aparelho no adb (adb devices)")
    model = shell("getprop ro.product.model", s).strip()
    soc = shell("getprop ro.soc.model", s).strip() or shell("getprop ro.board.platform", s).strip()
    vk = shell("getprop ro.hardware.vulkan", s).strip()
    t_end = time.time() + args.wait
    pid = None
    while not pid and time.time() < t_end:
        pid = find_pid(s, args.process)
        if not pid:
            time.sleep(2)
    if not pid:
        sys.exit(f"android_perf: {args.process} não apareceu em {args.wait:.0f} s")
    klass, maxf = core_classes(s)
    gpu_file = next((f for f in GPU_FILES if shell(f"cat {f} 2>/dev/null", s).strip()), None)
    clk_file = next((f for f in GPU_CLOCK_FILES if shell(f"cat {f} 2>/dev/null", s).strip()), None)
    gmax = next((read_clock_mhz(t) for t in (shell(f"cat {f} 2>/dev/null", s) for f in GPU_MAXCLOCK_FILES)
                 if t.strip()), None)
    print(f"android_perf: {model} ({soc}, vulkan {vk or '?'}), pid {pid}; GPU: {gpu_file or 'sem contador legível'}",
          file=sys.stderr)

    log = Logcat(s)
    log.start()
    time.sleep(0.5)
    log.on = True
    def probe_for(pid):
        return (f"cat /proc/{pid}/task/*/stat 2>/dev/null; echo @@; "
                f"cat /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq 2>/dev/null; echo @@; "
                f"{'cat ' + gpu_file if gpu_file else 'echo'} 2>/dev/null; echo @@; "
                f"{'cat ' + clk_file if clk_file else 'echo'} 2>/dev/null; echo @@; "
                f"grep -E 'VmRSS' /proc/{pid}/status 2>/dev/null")
    probe = probe_for(pid)
    hz = 100
    prev, prev_t = None, None
    util = {}    # tid -> [% de um núcleo por intervalo]
    names, cores = {}, {}
    freqs, gpu, gclk, rss = [], [], [], []
    t_stop = time.time() + args.secs
    while time.time() < t_stop:
        t = time.time()
        parts = shell(probe, s).split("@@")
        if len(parts) < 5:
            if not find_pid(s, args.process):
                print("android_perf: o processo saiu", file=sys.stderr)
                break
            continue
        rows = parse_stats(parts[0])
        if not rows:
            # O jogo fechou ou reabriu (o launcher abre um processo novo): segue o novo, do zero.
            new = find_pid(s, args.process)
            if new and new != pid:
                print(f"android_perf: o processo {pid} saiu; seguindo o novo, {new}", file=sys.stderr)
                pid, probe, prev = new, probe_for(new), None
                util, names, cores = {}, {}, {}
                freqs, gpu, gclk, rss = [], [], [], []
                log.samples.clear()
                log.cpu_lines.clear()
            elif not new:
                print("android_perf: o processo saiu", file=sys.stderr)
                break
            continue
        if prev is not None:
            dt = t - prev_t
            for tid, (name, ticks, cpu) in rows.items():
                if tid in prev:
                    util.setdefault(tid, []).append(100.0 * (ticks - prev[tid][1]) / hz / dt)
                names[tid], cores[tid] = name, cpu
            fr = [int(x) for x in parts[1].split() if x.isdigit()]
            if fr:
                freqs.append(fr)
            g = read_gpu(parts[2], gpu_file) if parts[2].strip() else None
            if g is not None:
                gpu.append(g)
            c = read_clock_mhz(parts[3])
            if c:
                gclk.append(c)
            m = re.search(r"VmRSS:\s+(\d+)", parts[4])
            if m:
                rss.append(int(m.group(1)) / 1024)
        prev, prev_t = rows, t
        time.sleep(max(0.0, args.interval - (time.time() - t)))
    log.on = False
    log.stop()

    out = []
    p = out.append
    p(f"# android_perf {args.label}".rstrip())
    p(f"aparelho: {model} ({soc}), vulkan {vk or '?'}; {len(next(iter(util.values()), []))} intervalos de "
      f"{args.interval:.0f} s")
    fps_s = [x[0] for x in log.samples]
    fps = statistics.mean(fps_s) if fps_s else None
    if fps:
        q = sorted(fps_s)
        p(f"fps (RE5_FPS_LOG): média {fps:.1f}, mediana {statistics.median(fps_s):.1f}, p10 {q[len(q) // 10]:.1f} "
          f"em {len(fps_s)} s")
    else:
        p("fps: nenhuma linha do RE5_FPS_LOG no logcat (ligue RE5_FPS_LOG=1 no launcher, em Avançado)")
    frame_ms = 1000.0 / fps if fps else None

    avg = {tid: statistics.mean(v) for tid, v in util.items() if v}
    total = sum(avg.values())
    p(f"processo: {total / 100:.2f} núcleos em média" + (f", {total / 100 * frame_ms:.1f} ms de CPU por quadro"
                                                          if frame_ms else ""))
    groups = {}
    for tid, u in avg.items():
        groups[group_of(names[tid])] = groups.get(group_of(names[tid]), 0) + u
    for g in ("jogo", "dxvk", "driver", "outros"):
        u = groups.get(g, 0)
        p(f"  {g:7s} {u:6.1f}% de um núcleo ({100 * u / total if total else 0:4.1f}% do processo)" +
          (f", {u / 100 * frame_ms:5.1f} ms/quadro" if frame_ms else ""))
    p("threads mais ocupadas (% de um núcleo: média / pico; núcleo da última amostra):")
    for tid in sorted(avg, key=avg.get, reverse=True)[:14]:
        if avg[tid] < 1:
            break
        cls = klass.get(cores.get(tid, -1), "?")
        p(f"  {tid:6d} {names[tid]:16s} {group_of(names[tid]):7s} {avg[tid]:5.1f} / {max(util[tid]):5.1f}   "
          f"cpu{cores.get(tid, -1)} ({cls})")
    if freqs:
        ncore = min(len(f) for f in freqs)
        mean_f = [statistics.mean(f[i] for f in freqs) / 1000 for i in range(ncore)]
        p("frequência média dos núcleos (MHz, máx entre parênteses): " +
          " ".join(f"cpu{i}={mean_f[i]:.0f}({maxf.get(i, 0) // 1000})" for i in range(ncore)))
    if gpu:
        p(f"GPU ocupada: média {statistics.mean(gpu):.0f}%, pico {max(gpu):.0f}% ({gpu_file})" +
          (f"; clock médio {statistics.mean(gclk):.0f} MHz" if gclk else "") +
          (f" (máx {gmax:.0f} MHz, {100 * statistics.mean(gclk) / gmax:.0f}% do máximo)" if gclk and gmax else ""))
    else:
        p("GPU ocupada: sem contador legível sem root neste aparelho (use a escala de resolução da A0 para separar)")
    if rss:
        p(f"RAM (VmRSS): média {statistics.mean(rss):.0f} MiB, pico {max(rss):.0f} MiB")

    d3d = [x for x in log.samples if x[2] is not None]
    if d3d and fps:
        lock_f = statistics.mean(x[1] for x in d3d) / fps
        d3d9_f = statistics.mean(x[2] for x in d3d) / fps
        pres_f = statistics.mean(x[3] for x in d3d) / fps
        p(f"por quadro ({frame_ms:.1f} ms): Direct3D do host (ponte + front-end da DXVK) {d3d9_f:.2f} ms "
          f"({100 * d3d9_f / frame_ms:.0f}%), Present {pres_f:.2f} ms ({100 * pres_f / frame_ms:.0f}%), "
          f"espera no lock do guest {lock_f:.1f} ms (somada das threads)")

        # Veredito: a ordem importa (GPU primeiro: se ela não dá conta, nada na CPU ajuda).
        main_tid = max((t for t in avg if group_of(names[t]) == "jogo"), key=avg.get, default=None)
        main_u = avg.get(main_tid, 0)
        dx = groups.get("dxvk", 0) + groups.get("driver", 0)
        p("veredito:")
        if (gpu and statistics.mean(gpu) >= 90) or pres_f >= 0.25 * frame_ms:
            p("  GPU/apresentação no limite: o Present espera a GPU. Um backend próprio só ajuda se gastar menos "
              "GPU (A3 ETC2, A4 escala/FIFO) - o custo de CPU da DXVK não é o gargalo.")
        elif main_u >= 85:
            p(f"  thread do jogo no limite ({names[main_tid]}, {main_u:.0f}% de um núcleo). Dela, o Direct3D do host "
              f"é {100 * d3d9_f / frame_ms:.0f}% do quadro: é o teto do que um backend próprio tira da thread "
              f"crítica (o resto é código do jogo: A1/A5/P5).")
        elif lock_f >= 0.5 * frame_ms:
            p("  serialização no lock do guest (modo global): as threads do jogo esperam umas às outras. "
              "O prêmio é a A1 (paralelismo), não o backend gráfico.")
        else:
            p("  nenhum recurso no limite claro: medir de novo com a resolução da A0 (720p x 540p) e Frame Rate 30.")
        p(f"  DXVK + driver fora da thread do jogo: {dx:.0f}% de um núcleo "
          f"({dx / 100 * frame_ms:.1f} ms/quadro em paralelo; só pesa se houver núcleo grande livre de menos).")
    elif fps:
        p("sem d3d9_ms/present_ms no log: o libre5.so é anterior a essa medida (recompile o runtime)")
    if log.cpu_lines:
        p("relatório do runtime (RE5_FPS_LOG, a cada 30 s), último:")
        for line in log.cpu_lines[-20:]:
            p("  " + line)

    text = "\n".join(out)
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")


if __name__ == "__main__":
    main()
