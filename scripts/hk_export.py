#!/usr/bin/env python3
"""Extrai a colisão do Havok de um despejo da memória do guest (RE5_DUMP=1) para triângulos no
mundo, e consulta a altura do chão sob os personagens.

    python scripts/hk_export.py work/dump/guest_2 [--obj saida.obj] [--json saida.json]

Layouts do Havok 5.5 do RE5 (x86, medidos no despejo, ver docs/AUDITORIA_GHIDRA.md):

  hkpWorldObject  +0x08 mundo, +0x10 forma (hkpCollidable.m_shape)
  hkpRigidBody    transformação em +0xE0: 3 colunas de rotação (16 B cada) e a translação em +0x110
  hkpSimpleShapePhantom (personagens)   transformação em +0xA0: 3 colunas e translação em +0xD0
  hkpMoppBvTreeShape  filho em +0x34 (hkpSingleShapeContainer)
  hkpSimpleMeshShape  vértices: ptr +0x18, n +0x1C (vec4); triângulos: ptr +0x24, n +0x28
                      (16 B: a, b, c, soldagem)
  hkpConvexVerticesShape  raio +0x10, meia-extensão AABB +0x20, centro AABB +0x30
  hkpCapsuleShape raio +0x10, vértice A +0x20 (xyz, w = raio), vértice B +0x30
  Eixos: Y para cima, metros.
"""
import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hk_scan  # noqa: E402

HEAP = 0x10000000  # objetos do heap (as vtables de modelo no .data/.text também casam com o valor)


def mat_apply(rot, t, p):
    """rot = 3 colunas (cada uma uma tupla xyz); p local -> mundo."""
    x, y, z = p
    return tuple(rot[0][i] * x + rot[1][i] * y + rot[2][i] * z + t[i] for i in range(3))


class Hk:
    def __init__(self, base, rtti):
        self.d = hk_scan.Dump(base)
        self.vt = hk_scan.load_vtables(rtti)
        self.by_vt = {v: k for k, v in self.vt.items()}

    def cls(self, va):
        if va < 0x10000 or not self.d.mapped(va):
            return None
        return self.by_vt.get(self.d.u32(va))

    def objects(self, name):
        return sorted(v for v in self.d.find_u32(self.vt[name]) if v >= HEAP)

    def vec(self, va, n=3):
        return struct.unpack("<%df" % n, self.d.read(va, 4 * n))

    def transform(self, va, rot_off, t_off):
        rot = [self.vec(va + rot_off + 16 * i) for i in range(3)]
        return rot, self.vec(va + t_off)

    def mesh(self, shape):
        """[(a, b, c)] de triângulos em coordenadas locais, ou None se a forma não é uma malha."""
        c = self.cls(shape)
        if c == "hkpMoppBvTreeShape":
            return self.mesh(self.d.u32(shape + 0x34))
        if c == "hkpSimpleMeshShape":
            vp, vn = self.d.u32(shape + 0x18), self.d.u32(shape + 0x1C)
            tp, tn = self.d.u32(shape + 0x24), self.d.u32(shape + 0x28)
            if not (0 < vn < 2_000_000 and 0 < tn < 4_000_000):
                return None
            vraw = self.d.read(vp, 16 * vn)
            verts = [struct.unpack_from("<3f", vraw, 16 * i) for i in range(vn)]
            traw = self.d.read(tp, 16 * tn)
            tris = []
            for i in range(tn):
                a, b, c2 = struct.unpack_from("<3I", traw, 16 * i)
                if a < vn and b < vn and c2 < vn:
                    tris.append((verts[a], verts[b], verts[c2]))
            return tris
        return None

    def world_triangles(self):
        """(corpo, tipo de movimento, triângulos no mundo) de cada corpo rígido com malha."""
        out = []
        for rb in self.objects("hkpRigidBody"):
            shape = self.d.u32(rb + 0x10)
            tris = self.mesh(shape)
            if not tris:
                continue
            rot, t = self.transform(rb, 0xE0, 0x110)
            motion = self.cls(self.d.u32(rb + 0xD0) and rb + 0xD0)
            out.append((rb, motion, [tuple(mat_apply(rot, t, p) for p in tri) for tri in tris]))
        return out

    def phantoms(self):
        res = []
        for ph in self.objects("hkpSimpleShapePhantom"):
            shape = self.d.u32(ph + 0x10)
            rot, t = self.transform(ph, 0xA0, 0xD0)
            sc = self.cls(shape)
            info = None
            if sc == "hkpCapsuleShape":
                info = (self.vec(shape + 0x20, 4), self.vec(shape + 0x30, 4))
            res.append((ph, sc, rot, t, info))
        return res


def ray_down(tris, x, z):
    """Alturas (Y) dos triângulos cortados pela vertical em (x, z), em ordem."""
    hits = []
    for (a, b, c) in tris:
        d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if abs(d) < 1e-12:
            continue
        l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
        l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
        l3 = 1 - l1 - l2
        if l1 >= -1e-6 and l2 >= -1e-6 and l3 >= -1e-6:
            hits.append(l1 * a[1] + l2 * b[1] + l3 * c[1])
    return sorted(hits)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump")
    ap.add_argument("--rtti", default=os.path.join(os.path.dirname(__file__), "..", "work", "ghidra", "export",
                                                   "rtti.json"))
    ap.add_argument("--obj")
    args = ap.parse_args()
    hk = Hk(args.dump, args.rtti)
    bodies = hk.world_triangles()
    ntri = sum(len(t) for _, _, t in bodies)
    print(f"{len(bodies)} corpos rígidos com malha, {ntri} triângulos no mundo")
    motions = {}
    for _, m, t in bodies:
        motions[m] = motions.get(m, 0) + len(t)
    print("por tipo de movimento:", motions)
    allt = [t for _, _, ts in bodies for t in ts]
    if allt:
        xs = [p[0] for t in allt for p in t]; ys = [p[1] for t in allt for p in t]; zs = [p[2] for t in allt for p in t]
        print(f"caixa do mundo: x {min(xs):.1f}..{max(xs):.1f}  y {min(ys):.1f}..{max(ys):.1f}  z {min(zs):.1f}..{max(zs):.1f}")
    print("\npersonagens (hkpSimpleShapePhantom):")
    for ph, sc, rot, t, info in hk.phantoms():
        hits = ray_down(allt, t[0], t[2])
        print(f"  {ph:08X} {str(sc):16s} pos ({t[0]:8.2f},{t[1]:7.2f},{t[2]:8.2f})  chão sob ele (Y): "
              f"{', '.join(f'{h:.2f}' for h in hits[:6]) or 'nenhum'}")
    if args.obj:
        with open(args.obj, "w") as f:
            n = 1
            for rb, m, tris in bodies:
                f.write(f"o body_{rb:08X}\n")
                for tri in tris:
                    for p in tri:
                        f.write("v %.4f %.4f %.4f\n" % p)
                    f.write(f"f {n} {n + 1} {n + 2}\n")
                    n += 3
        print("OBJ:", args.obj)


if __name__ == "__main__":
    sys.exit(main())
