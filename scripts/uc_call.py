#!/usr/bin/env python3
"""Chama uma função do jogo no Unicorn (x86 original) sobre a memória de uma captura F11.

    python scripts/uc_call.py work/dump/capture_1 0x007BDDD0 --vec 527.87,-133.15,226.97 --args vec
    python scripts/uc_call.py work/dump/capture_1 0x007BDDD0 --vec x,y,z --vec x,y,z ...   # um teste por --vec

Responde "o que o x86 de verdade faria com este estado": a captura (`capture_N_antes`) é a memória
inteira do guest num ponto do jogo. Cada `--vec x,y,z` vira um float[4] numa página de rascunho e a
função é chamada com os argumentos de `--args` (cdecl: 'vec' = ponteiro do vetor, número = valor;
`--ecx` para thiscall). Mostra eax/al na volta e o vetor depois da chamada (a função pode
corrigi-lo, p.ex. levar a posição até o chão). Imports chamados no meio são emulados como no
snap_diff.py; um import sem emulação para com o nome.
"""
import argparse
import json
import os
import struct
import sys

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_MODE_32, Uc, UcError
from unicorn import x86_const as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hk_scan  # noqa: E402

PAGE = 0x1000
SENTINEL = 0x7FFE0000
SCRATCH = 0x7FFD0000
STACK_TOP = 0x7FFC0000
THUNK_BASE, THUNK_LIMIT = 0xFFFC0000, 0xFFFF0000


def load_thunks(path):
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8", errors="replace"):
            va, nbytes, name = line.rstrip("\n").split("\t", 2)
            out[int(va, 16)] = (int(nbytes), name)
    return out


class Machine:
    def __init__(self, capture):
        self.dump = hk_scan.Dump(capture + "_antes")
        self.r0 = json.load(open(capture + "_antes.json"))
        self.thunks = load_thunks(capture + "_thunks.tsv")
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.mapped = set()
        spans = []
        for rva, size, _ in self.dump.runs:
            lo, hi = rva & ~(PAGE - 1), (rva + size + PAGE - 1) & ~(PAGE - 1)
            if spans and lo - spans[-1][1] < (1 << 20):
                spans[-1][1] = max(spans[-1][1], hi)
            else:
                spans.append([lo, hi])
        for lo, hi in spans:
            uc.mem_map(lo, hi - lo)
            self.mapped.update(range(lo, hi, PAGE))
        self.pristine = []
        for rva, size, off in self.dump.runs:
            uc.mem_write(rva, self.dump.mm[off:off + size])
        for base, n in ((THUNK_BASE, THUNK_LIMIT - THUNK_BASE), (SENTINEL, PAGE), (SCRATCH, PAGE),
                        (STACK_TOP - 0x100000, 0x100000)):
            if base not in self.mapped:
                uc.mem_map(base, n)
                self.mapped.update(range(base, base + n, PAGE))
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_CODE, self._thunk, begin=THUNK_BASE, end=THUNK_LIMIT - 1)
        self.stop = None

    def _unmapped(self, uc, access, address, size, value, user):
        if address < 0x10000:
            return False
        for a in {address & ~(PAGE - 1), (address + size - 1) & ~(PAGE - 1)}:
            if a not in self.mapped:
                uc.mem_map(a, PAGE)
                self.mapped.add(a)
        return True

    def rd32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def wr32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def _thunk(self, uc, address, size, user):
        nb, name = self.thunks.get(address, (0, f"?{address:08X}"))
        esp = uc.reg_read(X.UC_X86_REG_ESP)
        ret = self.rd32(esp)
        argv = [self.rd32(esp + 4 + 4 * i) for i in range(nb // 4)]
        short = name.split("!")[-1]
        eax = 0
        if short == "TlsGetValue":
            eax = self.rd32(self.r0["fs_base"] + 0xE10 + 4 * argv[0]) if argv[0] < 64 else 0
        elif short == "GetCurrentThreadId":
            eax = self.r0["tid"]
        elif short == "TryEnterCriticalSection":
            eax = 1
        elif short in ("EnterCriticalSection", "LeaveCriticalSection", "SetLastError", "GetLastError"):
            eax = 0
        elif short in ("InterlockedIncrement", "InterlockedDecrement"):
            eax = (self.rd32(argv[0]) + (1 if short.endswith("Increment") else -1)) & 0xFFFFFFFF
            self.wr32(argv[0], eax)
        else:
            self.stop = f"import sem emulação: {name} (de {ret:08X})"
            uc.emu_stop()
            return
        uc.reg_write(X.UC_X86_REG_EAX, eax)
        uc.reg_write(X.UC_X86_REG_ESP, esp + 4 + nb)
        uc.reg_write(X.UC_X86_REG_EIP, ret)

    def call(self, va, args, ecx=0, max_insns=50_000_000):
        uc = self.uc
        esp = STACK_TOP - 0x1000
        for a in reversed(args):
            esp -= 4
            self.wr32(esp, a)
        esp -= 4
        self.wr32(esp, SENTINEL)
        for rid in (X.UC_X86_REG_EAX, X.UC_X86_REG_EBX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI):
            uc.reg_write(rid, 0)
        uc.reg_write(X.UC_X86_REG_ECX, ecx)
        uc.reg_write(X.UC_X86_REG_ESP, esp)
        uc.reg_write(X.UC_X86_REG_EBP, esp + 0x100)
        uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(X.UC_X86_REG_MXCSR, self.r0.get("mxcsr") or 0x1F80)
        uc.reg_write(X.UC_X86_REG_FPCW, self.r0.get("fpu_cw") or 0x027F)
        try:
            uc.reg_write(X.UC_X86_REG_FS_BASE, self.r0["fs_base"])
        except Exception:  # noqa: BLE001 - versões do Unicorn sem FS_BASE gravável
            pass
        self.stop = None
        try:
            uc.emu_start(va, SENTINEL, count=max_insns)
        except UcError as ex:
            self.stop = self.stop or f"Unicorn parou: {ex} em eip={uc.reg_read(X.UC_X86_REG_EIP):08X}"
        if uc.reg_read(X.UC_X86_REG_EIP) != SENTINEL and not self.stop:
            self.stop = f"não voltou (eip={uc.reg_read(X.UC_X86_REG_EIP):08X}; limite de instruções?)"
        return uc.reg_read(X.UC_X86_REG_EAX)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture", help="prefixo da captura, ex.: work/dump/capture_1")
    ap.add_argument("va", type=lambda v: int(v, 16))
    ap.add_argument("--vec", action="append", default=[], help="x,y,z (um teste por --vec)")
    ap.add_argument("--args", default="vec", help="argumentos cdecl, vírgulas: 'vec' ou número")
    ap.add_argument("--ecx", type=lambda v: int(v, 16), default=0)
    args = ap.parse_args()

    m = Machine(args.capture)
    for v in args.vec or [None]:
        argv = []
        if v is not None:
            xyz = [float(t) for t in v.split(",")]
            m.uc.mem_write(SCRATCH, struct.pack("<4f", *(xyz + [0.0] * (4 - len(xyz)))))
        for a in args.args.split(","):
            a = a.strip()
            if a == "vec":
                argv.append(SCRATCH)
            elif a:
                argv.append(int(a, 0) & 0xFFFFFFFF)
        eax = m.call(args.va, argv, args.ecx)
        after = struct.unpack("<3f", m.uc.mem_read(SCRATCH, 12))
        line = f"{v or '-':>28}  eax={eax:08X} al={eax & 0xFF}  vetor depois=({after[0]:.2f}, {after[1]:.2f}, {after[2]:.2f})"
        print(line + (f"  [{m.stop}]" if m.stop else ""))


if __name__ == "__main__":
    main()
