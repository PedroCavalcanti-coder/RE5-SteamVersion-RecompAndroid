#!/usr/bin/env python3
import glob
import os
import sys
import time

OBJ_DIR = os.path.join("build", "test-build", "generated", "CMakeFiles", "re5_generated.dir", "src")
TOTAL = 178

def main():
    print("=" * 65)
    print("  re5-recompiled: Monitoramento de Compilacao em Tempo Real")
    print("=" * 65)
    while True:
        objs = glob.glob(os.path.join(OBJ_DIR, "*.obj"))
        count = len(objs)
        pct = (count / TOTAL) * 100 if TOTAL else 0
        latest_file = ""
        latest_time = ""
        if objs:
            latest = max(objs, key=os.path.getmtime)
            latest_file = os.path.basename(latest)
            latest_time = time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(latest)))

        print(f"\r[*] Objetos compilados: {count}/{TOTAL} ({pct:5.1f}%) | Último: {latest_file} ({latest_time})", end="", flush=True)

        if count >= TOTAL:
            print("\n\n[+] Todos os objetos compilados com sucesso!")
            break
        time.sleep(1.5)

if __name__ == "__main__":
    main()
