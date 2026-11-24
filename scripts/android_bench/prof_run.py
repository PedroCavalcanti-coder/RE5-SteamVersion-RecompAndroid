#!/usr/bin/env python3
"""Perfil de um renderizador no A54: abre o jogo com o roteiro do benchmark, e quando o roteiro acaba grava
simpleperf (--app) por N s + amostras de CPU/GPU, depois fecha. Uso: prof_run.py <pasta> <dxvk|vkmali> [N] [CHAVE=VALOR...]"""
import os
import re
import subprocess
import sys
import time

ADB = r"C:\Users\Dev_RV01\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "com.re5.recompiled"
GAME_DIR = f"/storage/emulated/0/Android/data/{PKG}/files/game"
SCRIPT = "300:Return,+500:Down,+250:Down,+250:Return,+300:Return"
OUT, GFX = sys.argv[1], sys.argv[2]
SECS = int(sys.argv[3]) if len(sys.argv) > 3 else 40
EXTRA = sys.argv[4:]
os.makedirs(OUT, exist_ok=True)
NL = chr(10)


def adb(*a, inp=None, timeout=180):
    return subprocess.run([ADB] + list(a), input=inp, capture_output=True, text=True, errors="replace", timeout=timeout)


def set_pref(gfx):
    """A preferência do app vence o RE5_GFX do re5_env.txt: dxvk = sem a chave."""
    body = "" if gfx == "dxvk" else '    <string name="gfx_renderer">' + gfx + "</string>" + NL
    xml = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>" + NL + "<map>" + NL + body + "</map>" + NL
    adb("shell", f"run-as {PKG} sh -c 'cat > shared_prefs/re5_settings.xml'", inp=xml)


old_env = adb("shell", f"cat {GAME_DIR}/re5_env.txt").stdout
env = ["RE5_FPS_LOG=1", f"RE5_GFX={GFX}", "RE5_INPUT_SYNC=titlelogo", "RE5_INPUT_HOLD=20",
       f"RE5_INPUT_SCRIPT={SCRIPT}", "RE5_LEAN=1"] + EXTRA
try:
    adb("shell", f"am force-stop {PKG}")
    time.sleep(2)
    set_pref(GFX)
    adb("shell", f"run-as {PKG} rm -f files/game_running files/game_launch_claimed")
    adb("shell", f"cat > {GAME_DIR}/re5_env.txt", inp=NL.join(env) + NL)
    adb("logcat", "-c")
    log = os.path.join(OUT, "logcat.txt")
    lc = subprocess.Popen([ADB, "logcat", "-v", "threadtime"], stdout=open(log, "wb"), stderr=subprocess.DEVNULL)
    adb("shell", "input keyevent KEYCODE_WAKEUP")
    t0 = time.time()
    adb("shell", f"am start -n {PKG}/.GameActivity --es GAME_DIR {GAME_DIR} --es LAUNCH_ID prof-{int(t0 * 1000)}")
    ok = False
    while time.time() - t0 < 420:
        time.sleep(2)
        text = open(log, encoding="utf-8", errors="replace").read()
        m = re.search(r"RE5_INPUT_SCRIPT: (\d+) teclas", text)
        n = int(m.group(1)) if m else None
        if n and len(re.findall(r"RE5_INPUT_SCRIPT: Present", text)) >= n:
            ok = True
            break
        if "Fatal signal" in text:
            break
    print("[*] roteiro terminou" if ok else "[!] roteiro não terminou", f"{time.time() - t0:.0f} s", flush=True)
    if ok:
        time.sleep(3)
        pid = adb("shell", f"pidof {PKG}:game").stdout.strip()
        perf = subprocess.Popen([ADB, "shell", f"simpleperf record --app {PKG} -e cpu-clock -f 1000 --call-graph fp "
                                 f"--duration {SECS} -o /data/local/tmp/prof.data"],
                                stdout=open(os.path.join(OUT, "simpleperf.txt"), "wb"), stderr=subprocess.STDOUT)
        loop = (f'i=0; while [ $i -lt {SECS} ]; do echo "gpu $(cat /sys/kernel/gpu/gpu_busy) $(cat /sys/kernel/gpu/gpu_clock) '
                'cpu4 $(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq)"; sleep 1; i=$((i+1)); done')
        clk = adb("shell", loop, timeout=SECS + 60).stdout
        open(os.path.join(OUT, "clocks.txt"), "w").write(clk)
        perf.wait(timeout=180)
        adb("pull", "/data/local/tmp/prof.data", os.path.join(OUT, "prof.data"), timeout=300)
        open(os.path.join(OUT, "pid.txt"), "w").write(pid + NL + adb("shell", f"pm path {PKG}").stdout)
    adb("shell", f"am force-stop {PKG}")
    time.sleep(1)
    lc.terminate()
finally:
    adb("shell", f"cat > {GAME_DIR}/re5_env.txt", inp=old_env)
    set_pref("vkmali")
print("[*] fim")
