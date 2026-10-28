#!/usr/bin/env python3
"""Amostrador externo de CPU por thread (e GPU) de qualquer processo do Windows.

Serve para comparar o re5dx9.exe original com o nosso re5.exe pela mesma régua:
quantos núcleos cada um usa, quais threads estão saturadas e quais esperam, e se a
GPU está ocupada. Só lê contadores do sistema (GetThreadTimes, contadores de GPU);
não injeta nada no processo medido.

    python scripts/thread_cpu.py --name re5dx9.exe --secs 90      # o original
    python scripts/thread_cpu.py --name re5.exe --secs 90         # o recompilado

Espera até 5 minutos o processo aparecer (abra o jogo e entre na cena a medir), amostra a cada
--interval segundos por --secs segundos e imprime: núcleos usados em média, cada thread
(média e pico em % de um núcleo, módulo onde a thread nasceu, nome) e o uso da GPU.
"""
import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import io
import statistics
import subprocess
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")

TH32CS_SNAPTHREAD = 0x4
TH32CS_SNAPPROCESS = 0x2
THREAD_QUERY_INFORMATION = 0x40
THREAD_QUERY_LIMITED_INFORMATION = 0x800
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
INVALID_HANDLE = ctypes.c_void_p(-1).value


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_char * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetModuleHandleExA.argtypes = [wt.DWORD, ctypes.c_void_p, ctypes.POINTER(wt.HMODULE)]
k32.GetModuleFileNameA.argtypes = [wt.HMODULE, ctypes.c_char_p, wt.DWORD]
ntdll.NtQueryInformationThread.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.ULONG, ctypes.c_void_p]
ntdll.NtQueryInformationThread.restype = ctypes.c_long
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.GetThreadDescription.restype = ctypes.c_long


def ft(f):
    return ((f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(pe)
    pid = None
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.decode("latin-1").lower() == name.lower():
            pid = pe.th32ProcessID
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def start_module(h):
    """Módulo da função inicial da thread (diz se é do jogo, da DXVK ou do driver)."""
    start = ctypes.c_void_p()
    if ntdll.NtQueryInformationThread(h, 9, ctypes.byref(start), ctypes.sizeof(start), None) != 0 or not start.value:
        return "?"
    mod = wt.HMODULE()
    if not k32.GetModuleHandleExA(0x4 | 0x2, start.value, ctypes.byref(mod)):
        return "?"
    buf = ctypes.create_string_buffer(260)
    k32.GetModuleFileNameA(mod, buf, 260)
    return buf.value.decode("latin-1").rsplit("\\", 1)[-1]


def thread_name(h):
    try:
        p = ctypes.c_wchar_p()
        if k32.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
            return p.value
    except OSError:
        pass
    return ""


def gpu_sampler(pid, n, interval):
    """typeperf em segundo plano: soma do 'GPU Engine' do processo (3D e total)."""
    cmd = ["typeperf", f"\\GPU Engine(pid_{pid}*)\\Utilization Percentage", "-si", str(int(interval)), "-sc", str(n)]
    try:
        return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    except OSError:
        return None


def parse_gpu(text):
    rows = list(csv.reader(io.StringIO(text)))
    rows = [r for r in rows if len(r) > 1]
    if len(rows) < 2:
        return None
    head = rows[0]
    tot, d3 = [], []
    for r in rows[1:]:
        s_all = s_3d = 0.0
        for name, v in zip(head[1:], r[1:]):
            try:
                x = float(v)
            except ValueError:
                continue
            s_all += x
            if "engtype_3D" in name:
                s_3d += x
        tot.append(s_all)
        d3.append(s_3d)
    return tot, d3


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--name", default="re5.exe")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--secs", type=float, default=90)
    ap.add_argument("--interval", type=float, default=2)
    ap.add_argument("--wait", type=float, default=300, help="quanto esperar o processo aparecer")
    ap.add_argument("--no-gpu", action="store_true")
    args = ap.parse_args()

    pid = args.pid
    t0 = time.time()
    while not pid and time.time() - t0 < args.wait:
        pid = find_pid(args.name)
        if not pid:
            time.sleep(1)
    if not pid:
        sys.exit(f"[!] {args.name} não apareceu em {args.wait:.0f} s")
    print(f"[*] medindo {args.name} (pid {pid}) por {args.secs:.0f} s, amostra a cada {args.interval:g} s")
    n = int(args.secs / args.interval)
    gpu = None if args.no_gpu else gpu_sampler(pid, n, args.interval)

    handles, meta, last, util = {}, {}, {}, {}
    wall_prev = time.time()
    for _ in range(n):
        time.sleep(args.interval)
        now = time.time()
        dt = now - wall_prev
        wall_prev = now
        alive = set()
        for tid in threads_of(pid):
            alive.add(tid)
            h = handles.get(tid)
            if h is None:
                h = k32.OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                if not h:
                    continue
                handles[tid] = h
                meta[tid] = (start_module(h), thread_name(h))
            c, e, kn, us = (wt.FILETIME() for _ in range(4))
            if not k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kn), ctypes.byref(us)):
                continue
            cpu = ft(kn) + ft(us)
            if tid in last:
                util.setdefault(tid, []).append(100.0 * (cpu - last[tid]) / dt)
            last[tid] = cpu
        if not alive:
            print("[*] o processo terminou")
            break

    rows = []
    for tid, series in util.items():
        rows.append((statistics.mean(series), max(series), tid, series))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows) / 100.0
    print(f"\nNúcleos usados em média (soma das threads): {total:.2f}")
    by_mod = {}
    for avg, _, tid, _ in rows:
        by_mod[meta[tid][0]] = by_mod.get(meta[tid][0], 0.0) + avg / 100.0
    for mod, v in sorted(by_mod.items(), key=lambda x: -x[1]):
        if v >= 0.005:
            print(f"  módulo {mod:<28} {v:5.2f} núcleos")
    print(f"\n{'tid':>7} {'média %':>8} {'pico %':>7}  {'módulo da thread':<26} nome")
    for avg, peak, tid, _ in rows[:24]:
        if avg < 1.0:
            break
        print(f"{tid:>7} {avg:8.1f} {peak:7.1f}  {meta[tid][0]:<26} {meta[tid][1]}")
    small = sum(1 for r in rows if r[0] < 1.0)
    print(f"  (+ {small} thread(s) abaixo de 1% de um núcleo)")
    if gpu:
        try:
            out = gpu.communicate(timeout=30)[0]
            g = parse_gpu(out)
            if g and g[0]:
                print(f"\nGPU do processo (typeperf): total média {statistics.mean(g[0]):.1f}%, 3D média {statistics.mean(g[1]):.1f}%, "
                      f"pico 3D {max(g[1]):.1f}%")
            else:
                print("\nGPU: sem dados do typeperf para esse processo")
        except subprocess.TimeoutExpired:
            gpu.kill()
            print("\nGPU: typeperf não respondeu")


if __name__ == "__main__":
    main()
