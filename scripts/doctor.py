#!/usr/bin/env python3
"""Verifica se o ambiente consegue rodar o pipeline e o build.

    python3 scripts/doctor.py

É o primeiro comando de toda sessão de trabalho (humana ou da IA): diz o que
falta antes de alguém perder tempo com um erro de build obscuro. Itens
`[FALTA]` bloqueiam; `[aviso]` limitam o que dá para fazer nesta máquina.
Código de saída 0 quando nada bloqueante falta.
"""
import importlib
import os
import shutil
import subprocess
import sys

import re5common as rc

OK, WARN, MISSING = "[ ok  ]", "[aviso]", "[FALTA]"
problems = 0


def report(status, what, detail=""):
    global problems
    if status == MISSING:
        problems += 1
    print(f"{status} {what}" + (f" — {detail}" if detail else ""))


def tool_version(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        if out.returncode != 0:
            return None
        return (out.stdout or out.stderr).strip().splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError):
        return None


def main():
    print("== Python ==")
    v = sys.version_info
    report(OK if v >= (3, 11) else MISSING, f"Python {v.major}.{v.minor}", "precisa de 3.11+ (tomllib)")
    for mod in ("capstone", "pefile", "unicorn"):
        try:
            m = importlib.import_module(mod)
            report(OK, mod, getattr(m, "__version__", ""))
        except ImportError:
            report(MISSING, mod, "pip install -r scripts/requirements.txt")

    print("== Toolchain ==")
    cc_exe = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    cc_ver = tool_version([cc_exe, "--version"]) if cc_exe else None
    report(OK if cc_ver else MISSING, "cc (gcc/clang)", cc_ver or "não encontrado")
    for name, cmd, required in (("cmake", ["cmake", "--version"], True),
                                ("ninja", ["ninja", "--version"], False),
                                ("clang", ["clang", "--version"], False),
                                ("git-lfs", ["git", "lfs", "version"], True)):
        ver = tool_version(cmd) if shutil.which(cmd[0]) else None
        report(OK if ver else (MISSING if required else WARN), name, ver or "não encontrado")
    sdl = tool_version(["pkg-config", "--modversion", "sdl2"]) if shutil.which("pkg-config") else None
    report(OK if sdl else WARN, "SDL2 (dev)", sdl or "sem headers: o host roda só headless (Fase 4 precisa)")
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
    report(OK if ndk else WARN, "Android NDK", ndk or "ANDROID_NDK_HOME não definido (necessário só na Fase 5)")

    print("== Varredura (Ghidra / opcional) ==")
    ghidra = rc.find_ghidra()
    report(OK if ghidra else WARN, "Ghidra", ghidra or "não encontrado (necessário para varredura/auditoria)")
    java_cmd, jdk_ver = rc.find_jdk()
    report(OK if java_cmd else WARN, "JDK", f"{jdk_ver} ({java_cmd})" if java_cmd else "não encontrado (Ghidra precisa de JDK 21+)")

    print("== Repositório ==")
    report(OK if os.path.isdir(os.path.join(rc.PCRECOMP, "tools", "lift")) else MISSING,
           "tools/pcrecomp", rc.PCRECOMP)
    cfg = rc.load_config()
    for key in ("original", "steamless", "clean"):
        p = rc.repo_path(cfg["input"][key])
        if not os.path.exists(p):
            st = WARN
            detail = "ausente" + (" — necessário para a Fase 2 (ver PLANO, etapa 1.1)" if key == "clean" else "")
        elif rc.is_lfs_pointer(p):
            st, detail = MISSING, "ponteiro Git LFS: rode `git lfs pull --include='bin_input/*'`"
        else:
            st, detail = OK, f"{os.path.getsize(p):,} bytes"
        report(st, f"input.{key} ({cfg['input'][key]})", detail)

    print("== Máquina ==")
    report(OK, "CPUs", str(os.cpu_count()))
    free = shutil.disk_usage(rc.REPO).free / 2**30
    report(OK if free >= 20 else WARN, "disco livre", f"{free:.0f} GiB (o código gerado + build pedem ~15-20 GiB)")

    print()
    if problems:
        print(f"{problems} item(ns) bloqueante(s).")
        return 1
    print("Ambiente pronto.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
