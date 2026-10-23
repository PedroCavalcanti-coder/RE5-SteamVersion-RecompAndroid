#!/usr/bin/env python3
"""Acompanha a compilação do código gerado: quantos .c de generated/src já viraram .obj neste build.

O total vem do número de .c em generated/src (muda quando o catálogo muda: 181 janelas de 64 KiB +
a tabela de dispatch hoje), e só contam os .obj gravados depois que o monitor começou - os de builds
anteriores continuam na pasta e inflavam a conta (ex.: 182/178).
"""
import glob
import os
import time

SRC_DIR = os.path.join("generated", "src")
OBJ_DIR = os.path.join("build", "test-build", "generated", "CMakeFiles", "re5_generated.dir", "src")


def main():
    total = len(glob.glob(os.path.join(SRC_DIR, "*.c")))
    start = time.time()
    print("=" * 65)
    print("  re5-recompiled: Monitoramento de Compilacao em Tempo Real")
    print("=" * 65)
    while True:
        objs = [o for o in glob.glob(os.path.join(OBJ_DIR, "*.obj")) if os.path.getmtime(o) >= start]
        count = len(objs)
        pct = (count / total) * 100 if total else 0
        latest_file = latest_time = ""
        if objs:
            latest = max(objs, key=os.path.getmtime)
            latest_file = os.path.basename(latest)
            latest_time = time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(latest)))
        print(f"\r[*] Objetos compilados neste build: {count}/{total} ({pct:5.1f}%) | Último: {latest_file} "
              f"({latest_time})", end="", flush=True)
        if count >= total:
            print("\n\n[+] Todos os objetos compilados com sucesso!")
            break
        time.sleep(1.5)


if __name__ == "__main__":
    main()
