#!/usr/bin/env python3
"""Constrói o PE32 sintético e roda o pipeline nele (usado pelo CTest).

    build_fixture.py --out DIR [--clang C] [--lld-link L] [--dlltool D]

Saídas em DIR: fx.exe, catalog.json e gen/ (re5_0000.c, re5_funcs.h,
re5_dispatch_table.c, lift_stats.json). Passa pelos mesmos scripts que o jogo
real: recon.py (o portão), run_lift.py catalog e run_lift.py lift.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
SCRIPTS = os.path.join(REPO, "scripts")


def find(cands, given):
    if given:
        return given
    for c in cands:
        p = shutil.which(c)
        if p:
            return p
    sys.exit(f"build_fixture: nenhum de {cands} encontrado")


def run(cmd, **kw):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--clang")
    ap.add_argument("--lld-link")
    ap.add_argument("--dlltool")
    a = ap.parse_args()
    clang = find(["clang", "clang-18", "clang-17", "clang-19"], a.clang)
    lld = find(["lld-link", "lld-link-18", "lld-link-17", "lld-link-19"], a.lld_link)
    dlltool = find(["llvm-dlltool", "llvm-dlltool-18", "llvm-dlltool-17", "llvm-dlltool-19"], a.dlltool)

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    obj, exe = os.path.join(out, "fx.obj"), os.path.join(out, "fx.exe")
    deffile, lib = os.path.join(out, "kernel32.def"), os.path.join(out, "kernel32.lib")
    with open(deffile, "w") as f:
        f.write("LIBRARY kernel32.dll\nEXPORTS\nGetTickCount@0\nSleep@4\nSetLastError@4\nGetLastError@0\n")
    run([clang, "--target=i686-pc-windows-msvc", "-O1", "-fno-inline", "-c",
         os.path.join(HERE, "mini_pe", "fx.c"), "-o", obj])
    run([dlltool, "-m", "i386", "-d", deffile, "-l", lib, "-k"])
    run([lld, "/nologo", "/entry:entry", "/subsystem:console", "/base:0x400000", "/fixed",
         "/nodefaultlib", f"/out:{exe}", obj, lib])

    py = [sys.executable]
    run(py + [os.path.join(SCRIPTS, "recon.py"), exe, "--json", os.path.join(out, "recon.json")])
    cat = os.path.join(out, "catalog.json")
    run(py + [os.path.join(SCRIPTS, "run_lift.py"), "catalog", "--exe", exe, "--out", cat])
    run(py + [os.path.join(SCRIPTS, "run_lift.py"), "lift", "--exe", exe, "--catalog", cat,
              "--out", os.path.join(out, "gen")])


if __name__ == "__main__":
    main()
