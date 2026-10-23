#!/usr/bin/env python3
"""Lê um despejo da memória do guest (RE5_DUMP=1; F10 ou RE5_DUMP_AT) e acha os objetos do Havok.

    python scripts/hk_scan.py work/dump/guest_1 [--rtti work/ghidra/export/rtti.json] [--classes]

O Havok 5.5 do RE5 traz RTTI (work/ghidra/export/rtti.json): o 1º dword de cada objeto
polimórfico é o endereço da vtable da classe. Varrer a memória atrás desses endereços acha
todos os corpos rígidos (hkpRigidBody), formas (hkpBoxShape, hkpMoppBvTreeShape, malhas...) e
o hkpCharacterProxy dos personagens.

A classe `Dump` também serve de módulo: Dump(base).u32(va), .f32(va), .read(va, n).
"""
import argparse
import collections
import json
import mmap
import os
import struct
import sys

HK_CLASSES = [
    "hkpRigidBody", "hkpWorld", "hkpSimulationIsland", "hkpCharacterProxy", "hkpShapePhantom",
    "hkpBoxShape", "hkpCapsuleShape", "hkpSphereShape", "hkpCylinderShape", "hkpTriangleShape",
    "hkpConvexVerticesShape", "hkpMultiSphereShape", "hkpTransformShape", "hkpConvexTransformShape",
    "hkpConvexTranslateShape", "hkpListShape", "hkpMoppBvTreeShape", "hkpBvShape",
    "hkpExtendedMeshShape", "hkpStorageExtendedMeshShape", "hkpMeshShape", "hkpStorageMeshShape",
    "hkpSimpleMeshShape", "hkpMoppCode",
]


class Dump:
    """O .bin + .map de um despejo, como memória do guest (só as páginas que não eram zero)."""

    def __init__(self, base):
        self.runs = []  # (va, size, offset)
        for line in open(base + ".map"):
            va, size, off = (int(x, 16) for x in line.split())
            self.runs.append((va, size, off))
        self.runs.sort()
        self._starts = [r[0] for r in self.runs]
        with open(base + ".bin", "rb") as f:
            self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

    def _run(self, va):
        import bisect
        i = bisect.bisect_right(self._starts, va) - 1
        if i >= 0:
            rva, size, off = self.runs[i]
            if va < rva + size:
                return rva, size, off
        return None

    def read(self, va, n):
        """n bytes a partir de va; páginas ausentes (zero no despejo) voltam como zeros."""
        out = bytearray()
        while n > 0:
            r = self._run(va)
            if r is None:
                import bisect
                i = bisect.bisect_right(self._starts, va)
                nxt = self._starts[i] if i < len(self._starts) else va + n
                k = min(n, nxt - va)
                out += bytes(k)
            else:
                rva, size, off = r
                k = min(n, rva + size - va)
                out += self.mm[off + (va - rva):off + (va - rva) + k]
            va += k
            n -= k
        return bytes(out)

    def mapped(self, va):
        return self._run(va) is not None

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def f32(self, va):
        return struct.unpack("<f", self.read(va, 4))[0]

    def find_u32(self, value):
        """Endereços (alinhados em 4) onde aparece o dword `value`."""
        pat = struct.pack("<I", value)
        for rva, size, off in self.runs:
            pos = self.mm.find(pat, off, off + size)
            while pos != -1:
                if (pos - off) % 4 == 0:
                    yield rva + (pos - off)
                pos = self.mm.find(pat, pos + 1, off + size)


def load_vtables(path):
    d = json.load(open(path))
    cl = d["classes"]
    items = cl.items() if isinstance(cl, dict) else [(c["name"], c) for c in cl]
    return {name: int(c["vtable"], 16) for name, c in items if c.get("vtable")}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump")
    ap.add_argument("--rtti", default=os.path.join(os.path.dirname(__file__), "..", "work", "ghidra", "export",
                                                   "rtti.json"))
    ap.add_argument("--classes", action="store_true", help="conta todas as classes do RTTI, não só as de colisão")
    args = ap.parse_args()
    vt = load_vtables(args.rtti)
    d = Dump(args.dump)
    print(f"{len(d.runs)} trechos, {sum(r[1] for r in d.runs) >> 20} MiB")
    names = list(vt) if args.classes else [n for n in HK_CLASSES if n in vt]
    counts = collections.OrderedDict()
    for n in names:
        hits = [va for va in d.find_u32(vt[n])]
        counts[n] = hits
    for n, hits in counts.items():
        if hits:
            print(f"{n:34s} {len(hits):6d}   ex.: {', '.join(f'{h:08X}' for h in hits[:4])}")


if __name__ == "__main__":
    sys.exit(main())
