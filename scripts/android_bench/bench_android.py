#!/usr/bin/env python3
"""Benchmark roteirizado do RE5 no Android: abre do zero, roda o benchmark embutido e mede N s.

Uso: bench_android.py <pasta_saida> [segundos] [CHAVE=VALOR ...]
"""
import os
import re
import subprocess
import sys
import time

ADB = r"C:\Users\Dev_RV01\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "com.re5.recompiled"
GAME_DIR = f"/storage/emulated/0/Android/data/{PKG}/files/game"
SCRIPT = "300:Return,+500:Down,+250:Down,+250:Return,+300:Return"   # o mesmo do bench_re5.py
OUT = sys.argv[1]
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 120
EXTRA = sys.argv[3:]
os.makedirs(OUT, exist_ok=True)


def adb(*a, inp=None, timeout=120):
    return subprocess.run([ADB] + list(a), input=inp, capture_output=True, text=True, errors="replace", timeout=timeout)


env = ["RE5_FPS_LOG=1", "RE5_INPUT_SYNC=titlelogo", "RE5_INPUT_HOLD=20", f"RE5_INPUT_SCRIPT={SCRIPT}"] + EXTRA
adb("shell", f"am force-stop {PKG}")
time.sleep(2)
adb("shell", f"run-as {PKG} rm -f files/game_running files/game_launch_claimed")
adb("shell", f"cat > {GAME_DIR}/re5_env.txt", inp="\n".join(env) + "\n")
adb("logcat", "-c")
log = os.path.join(OUT, "logcat.txt")
lc = subprocess.Popen([ADB, "logcat", "-v", "threadtime"], stdout=open(log, "wb"), stderr=subprocess.DEVNULL)
adb("shell", f"am start -n {PKG}/.GameActivity --es GAME_DIR {GAME_DIR} --es LAUNCH_ID bench-{int(time.time() * 1000)}")
t0 = time.time()
started = None
while time.time() - t0 < 420:
    time.sleep(2)
    text = open(log, encoding="utf-8", errors="replace").read()
    m = re.search(r"RE5_INPUT_SCRIPT: (\d+) teclas", text)
    n = int(m.group(1)) if m else None
    seen = len(re.findall(r"RE5_INPUT_SCRIPT: Present", text))
    if "Fatal signal" in text or "[FATAL" in text:
        print("[!] o jogo caiu")
        break
    if n and seen >= n:
        started = time.time()
        break
if started:
    print(f"[*] roteiro terminou {started - t0:.0f} s depois de abrir; medindo {SECS:.0f} s")
    loop = (f"i=0; while [ $i -lt {int(SECS)} ]; do echo \"$(cat /sys/kernel/gpu/gpu_busy) $(cat /sys/kernel/gpu/gpu_clock) "
            "$(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)\"; sleep 1; i=$((i+1)); done")
    r = adb("shell", loop, timeout=SECS + 60)
    open(os.path.join(OUT, "clocks.txt"), "w").write(r.stdout)
    rows = [l.split() for l in r.stdout.splitlines() if len(l.split()) == 4]
    if rows:
        import statistics as st
        print(f"[*] GPU uso médio {st.mean(int(x[0].rstrip('%')) for x in rows):.0f}% clock médio "
              f"{st.mean(int(x[1]) for x in rows) / 1000:.0f} MHz | CPU grande {st.mean(int(x[2]) for x in rows) / 1000:.0f} MHz "
              f"pequena {st.mean(int(x[3]) for x in rows) / 1000:.0f} MHz")
else:
    print("[!] o roteiro não terminou")
adb("shell", f"am force-stop {PKG}")
time.sleep(1)
lc.terminate()
open(os.path.join(OUT, "inicio.txt"), "w").write(f"{(started or 0) - t0:.1f}\n")
print("[*] fim")
