#!/usr/bin/env python3
"""Diferencial por instantâneo: reexecuta no Unicorn (x86 original) uma chamada capturada no re5.exe e
compara com o que o C gerado produziu.

    python scripts/snap_diff.py work/dump/capture_1

O runtime grava (F11 com RE5_CAPTURE=<VA de um tracepoint>, ver runtime/src/core/trace.c):
  capture_N_antes.{bin,map,json}   memória e registradores na entrada da função
  capture_N_depois.{bin,map,json}  idem na saída (depois do C gerado)
  capture_N_thunks.tsv             VA, bytes de argumento e nome de cada thunk do host (imports)

Aqui o Unicorn parte do "antes", roda o código x86 do clean.exe até a função voltar, e cada byte que
ele escreveu (fora da pilha descartável abaixo do esp de entrada) é comparado com o "depois". Bytes
diferentes = o C gerado calculou outra coisa (ou outra thread mexeu no mesmo lugar no meio da
chamada: confira o endereço). Imports chamados no meio são emulados aqui (TLS, seções críticas,
Interlocked*); um import sem emulação para a execução com o nome.
"""
import argparse
import json
import os
import struct
import sys

from unicorn import UC_ARCH_X86, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_32, Uc, UcError
from unicorn import x86_const as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hk_scan  # noqa: E402

PAGE = 0x1000
SENTINEL = 0x7FFE0000
THUNK_BASE, THUNK_LIMIT = 0xFFFC0000, 0xFFFF0000


def load_thunks(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        va, nbytes, name = line.rstrip("\n").split("\t", 2)
        out[int(va, 16)] = (int(nbytes), name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture", help="prefixo, ex.: work/dump/capture_1")
    ap.add_argument("--max-insns", type=int, default=200_000_000)
    ap.add_argument("--show", type=int, default=60, help="faixas diferentes a mostrar")
    ap.add_argument("--funcs", metavar="CATALOGO", help="grava <captura>_funcs.tsv: funções do catálogo "
                    "executadas no Unicorn e quantas vezes (ex.: work/catalog.json)")
    ap.add_argument("--watch", default="", help="VAs (hex, vírgulas): a cada entrada, mostra ecx, os 6 "
                    "argumentos da pilha e 0x40 bytes (floats) para onde ecx e cada argumento apontam")
    args = ap.parse_args()

    before = hk_scan.Dump(args.capture + "_antes")
    after = hk_scan.Dump(args.capture + "_depois")
    r0 = json.load(open(args.capture + "_antes.json"))
    r1 = json.load(open(args.capture + "_depois.json"))
    thunks = load_thunks(args.capture + "_thunks.tsv")
    va = r0["va"]

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    mapped = set()

    def map_page(p):
        if p not in mapped:
            uc.mem_map(p, PAGE)
            mapped.add(p)

    # Uma região do Unicorn por trecho contíguo (dezenas de milhares de páginas soltas deixam o
    # Unicorn lentíssimo): junta os trechos do despejo separados por menos de 1 MiB.
    spans = []
    for rva, size, off in before.runs:
        lo, hi = rva & ~(PAGE - 1), (rva + size + PAGE - 1) & ~(PAGE - 1)
        if spans and lo - spans[-1][1] < (1 << 20):
            spans[-1][1] = max(spans[-1][1], hi)
        else:
            spans.append([lo, hi])
    for lo, hi in spans:
        uc.mem_map(lo, hi - lo)
        mapped.update(range(lo, hi, PAGE))
    for rva, size, off in before.runs:
        uc.mem_write(rva, before.mm[off:off + size])
    print(f"memória: {len(before.runs)} trechos em {len(spans)} regiões", flush=True)
    if THUNK_BASE not in mapped:
        uc.mem_map(THUNK_BASE, THUNK_LIMIT - THUNK_BASE)
        mapped.update(range(THUNK_BASE, THUNK_LIMIT, PAGE))
    map_page(SENTINEL)

    def on_unmapped(uc_, access, address, size, value, user):
        # Páginas zeradas não entram no despejo: aparecem aqui sob demanda.
        if address < 0x10000:
            return False
        map_page(address & ~(PAGE - 1))
        if (address + size - 1) & ~(PAGE - 1) != address & ~(PAGE - 1):
            map_page((address + size - 1) & ~(PAGE - 1))
        return True

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    esp0 = r0["esp"]
    written = set()

    def on_write(uc_, access, address, size, value, user):
        if not (esp0 - 0x200000 <= address < esp0):      # abaixo do esp de entrada: rascunho da própria chamada
            for i in range(size):
                written.add(address + i)

    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

    calls = {}
    stop = {"why": None}

    def rd32(a):
        return struct.unpack("<I", uc.mem_read(a, 4))[0]

    def wr32(a, v):
        uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def on_thunk(uc_, address, size, user):
        nb, name = thunks.get(address, (0, f"?{address:08X}"))
        esp = uc.reg_read(X.UC_X86_REG_ESP)
        ret = rd32(esp)
        argv = [rd32(esp + 4 + 4 * i) for i in range(max(nb // 4, 0))]
        short = name.split("!")[-1]
        calls[name] = calls.get(name, 0) + 1
        eax = 0
        if short == "TlsGetValue":
            eax = rd32(r0["fs_base"] + 0xE10 + 4 * argv[0]) if argv[0] < 64 else 0
        elif short == "GetCurrentThreadId":
            eax = r0["tid"]
        elif short in ("EnterCriticalSection", "LeaveCriticalSection", "InitializeCriticalSection",
                       "DeleteCriticalSection", "SetLastError", "Sleep"):
            eax = 0
        elif short == "TryEnterCriticalSection":
            eax = 1
        elif short == "InterlockedIncrement":
            v = (rd32(argv[0]) + 1) & 0xFFFFFFFF
            wr32(argv[0], v)
            eax = v
        elif short == "InterlockedDecrement":
            v = (rd32(argv[0]) - 1) & 0xFFFFFFFF
            wr32(argv[0], v)
            eax = v
        elif short == "InterlockedExchange":
            eax = rd32(argv[0])
            wr32(argv[0], argv[1])
        elif short == "InterlockedExchangeAdd":
            eax = rd32(argv[0])
            wr32(argv[0], eax + argv[1])
        elif short == "InterlockedCompareExchange":
            eax = rd32(argv[0])
            if eax == argv[2]:
                wr32(argv[0], argv[1])
        elif short == "QueryPerformanceCounter":
            uc.mem_write(argv[0], bytes(8))
            eax = 1
        elif short == "GetLastError":
            eax = 0
        else:
            stop["why"] = f"import sem emulação no Unicorn: {name} (de {ret:08X})"
            uc.emu_stop()
            return
        uc.reg_write(X.UC_X86_REG_EAX, eax)
        uc.reg_write(X.UC_X86_REG_ESP, esp + 4 + nb)
        uc.reg_write(X.UC_X86_REG_EIP, ret)

    uc.hook_add(UC_HOOK_CODE, on_thunk, begin=THUNK_BASE, end=THUNK_LIMIT - 1)

    regs = {"eax": X.UC_X86_REG_EAX, "ecx": X.UC_X86_REG_ECX, "edx": X.UC_X86_REG_EDX, "ebx": X.UC_X86_REG_EBX,
            "esp": X.UC_X86_REG_ESP, "ebp": X.UC_X86_REG_EBP, "esi": X.UC_X86_REG_ESI, "edi": X.UC_X86_REG_EDI}
    for k, rid in regs.items():
        uc.reg_write(rid, r0[k])
    fl = (r0["cf"] << 0) | (1 << 1) | (r0["pf"] << 2) | (r0["af"] << 4) | (r0["zf"] << 6) | (r0["sf"] << 7) | \
         (1 << 9) | (r0["of"] << 11)
    uc.reg_write(X.UC_X86_REG_EFLAGS, fl)
    for i, h in enumerate(r0["xmm"]):
        uc.reg_write(getattr(X, f"UC_X86_REG_XMM{i}"), int.from_bytes(bytes.fromhex(h), "little"))
    uc.reg_write(X.UC_X86_REG_MXCSR, r0["mxcsr"] or 0x1F80)
    uc.reg_write(X.UC_X86_REG_FPCW, r0["fpu_cw"] or 0x027F)
    try:
        uc.reg_write(X.UC_X86_REG_FS_BASE, r0["fs_base"])
    except Exception as ex:  # noqa: BLE001
        print("[aviso] FS_BASE não aceito pelo Unicorn:", ex)

    # --funcs: quais funções do catálogo a chamada executou (p.ex. se passou pelo detector de colisão)
    ran = {}
    if args.funcs:
        cat = json.load(open(args.funcs, encoding="utf-8"))
        starts = {f["address"] for f in cat["functions"]}

        def on_block(uc_, addr, size, _ud):
            if addr in starts:
                ran[addr] = ran.get(addr, 0) + 1

        uc.hook_add(UC_HOOK_BLOCK, on_block)

    watch = {int(x, 16) for x in args.watch.split(",") if x.strip()}

    def show_ptr(label, p):
        try:
            raw = bytes(uc.mem_read(p, 0x40))
        except UcError:
            return
        fl = struct.unpack("<16f", raw)
        print(f"      {label}={p:08X}: " + " ".join(f"{x:.4g}" if 1e-5 < abs(x) < 1e7 or x == 0 else
                                                  f"#{struct.unpack_from('<I', raw, 4 * i)[0]:08X}"
                                                  for i, x in enumerate(fl)))

    def on_watch(uc_, addr, size, _ud):
        if addr not in watch:
            return
        sp = uc.reg_read(X.UC_X86_REG_ESP)
        ecx = uc.reg_read(X.UC_X86_REG_ECX)
        argv = [rd32(sp + 4 * (i + 1)) for i in range(6)]
        print(f"  > {addr:08X} ret={rd32(sp):08X} ecx={ecx:08X} args=" + " ".join(f"{a:08X}" for a in argv))
        show_ptr("ecx", ecx)
        for i, a in enumerate(argv):
            if a >= 0x10000:
                show_ptr(f"arg{i}", a)

    if watch:
        uc.hook_add(UC_HOOK_CODE, on_watch, begin=min(watch), end=max(watch))

    ret_real = rd32(esp0)
    wr32(esp0, SENTINEL)
    print(f"função {va:08X}, retorno real {ret_real:08X}, esp {esp0:08X}, ecx {r0['ecx']:08X}")
    try:
        uc.emu_start(va, SENTINEL, count=args.max_insns)
    except UcError as ex:
        stop["why"] = stop["why"] or f"Unicorn parou: {ex} em eip={uc.reg_read(X.UC_X86_REG_EIP):08X}"
    eip = uc.reg_read(X.UC_X86_REG_EIP)
    if stop["why"]:
        print("[!]", stop["why"])
    elif eip != SENTINEL:
        print(f"[!] não voltou (eip={eip:08X}): limite de instruções?")
    print("imports chamados:", calls)

    esp_end = uc.reg_read(X.UC_X86_REG_ESP)
    eax_u = uc.reg_read(X.UC_X86_REG_EAX)
    print(f"eax: unicorn {eax_u:08X}  C {r1['eax']:08X}  {'IGUAL' if eax_u == r1['eax'] else 'DIFERENTE'}")
    print(f"esp: unicorn {esp_end:08X}  C {r1['esp']:08X}  {'IGUAL' if esp_end == r1['esp'] else 'DIFERENTE'}")

    # Compara cada byte escrito pelo Unicorn com o "depois" do C.
    addrs = sorted(a for a in written if a != esp0 and not (esp0 <= a < esp0 + 4))
    diffs = []
    for a in addrs:
        u = bytes(uc.mem_read(a, 1))[0]
        c = after.read(a, 1)[0]
        if u != c:
            diffs.append(a)
    print(f"{len(addrs)} bytes escritos pelo Unicorn; {len(diffs)} diferentes do C")
    if args.funcs:
        with open(args.capture + "_funcs.tsv", "w", encoding="utf-8") as f:
            for a in sorted(ran):
                f.write(f"{a:08X}\t{ran[a]}\n")
        print(f"{len(ran)} funções executadas -> {args.capture}_funcs.tsv")
    # agrupa em faixas
    ranges = []
    for a in diffs:
        if ranges and a - ranges[-1][1] <= 4:
            ranges[-1][1] = a
        else:
            ranges.append([a, a])
    for lo, hi in ranges[:args.show]:
        lo4, hi4 = lo & ~3, (hi | 3) + 1
        u = bytes(uc.mem_read(lo4, hi4 - lo4))
        c = after.read(lo4, hi4 - lo4)
        b = before.read(lo4, hi4 - lo4)
        fu = " ".join(f"{x:.6g}" for x in struct.unpack(f"<{len(u) // 4}f", u))
        fc = " ".join(f"{x:.6g}" for x in struct.unpack(f"<{len(c) // 4}f", c))
        print(f"  {lo4:08X}..{hi4 - 1:08X}  antes {b.hex()}\n      unicorn {u.hex()}  ({fu})\n      C       {c.hex()}  ({fc})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
