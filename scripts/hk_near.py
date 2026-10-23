#!/usr/bin/env python3
"""Lista os corpos de colisão do Havok perto de um ponto (ou de um personagem) num despejo da memória.

    python scripts/hk_near.py work/dump/guest_3                      # só lista os personagens
    python scripts/hk_near.py work/dump/guest_3 --phantom 33752A80 -r 3
    python scripts/hk_near.py work/dump/guest_3 --at 7.9,0.5,6.8 -r 3

Para cada corpo: endereço, movimento (fixo/dinâmico), forma, collisionFilterInfo (+0x28: camada = bits 0-4,
grupo do sistema = bits 5-15, subsistema = 16-20, "não colide com" = 21-25), centro e meia-extensão no mundo,
e se parece uma caixa (8 vértices, 6 planos). É o jeito de achar a "parede invisível": caixas fixas onde
não há geometria visível, ou com filtro diferente das vizinhas.
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hk_export  # noqa: E402


def decode_filter(f):
    return f"0x{f:08X} (camada {f & 0x1F}, grupo 0x{(f >> 5) & 0x7FF:03X}, sub {(f >> 16) & 0x1F}, " \
           f"não-colide {(f >> 21) & 0x1F})"


def local_aabb(hk, shape):
    """(centro, meia-extensão, nº de vértices, nº de planos, tipo) da forma, em coordenadas locais."""
    c = hk.cls(shape)
    if c == "hkpMoppBvTreeShape":
        return local_aabb(hk, hk.d.u32(shape + 0x34))
    if c == "hkpConvexVerticesShape":
        half = hk.vec(shape + 0x20)
        ctr = hk.vec(shape + 0x30)
        return ctr, half, hk.d.u32(shape + 0x4C), hk.d.u32(shape + 0x54) & 0x3FFFFFFF, c
    if c == "hkpSimpleMeshShape":
        tris = hk.mesh(shape)
        if not tris:
            return None
        xs = [p[i] for t in tris for p in t for i in (0,)]
        pts = [p for t in tris for p in t]
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        return [(a + b) / 2 for a, b in zip(lo, hi)], [(b - a) / 2 for a, b in zip(lo, hi)], len(pts), 0, c
    return None


def world_box(rot, t, ctr, half):
    wc = hk_export.mat_apply(rot, t, ctr)
    # AABB do mundo de uma caixa orientada: soma dos valores absolutos das colunas
    wh = tuple(sum(abs(rot[k][i]) * half[k] for k in range(3)) for i in range(3))
    return wc, wh


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump")
    ap.add_argument("--rtti", default=os.path.join(os.path.dirname(__file__), "..", "work", "ghidra", "export",
                                                   "rtti.json"))
    ap.add_argument("--phantom", help="endereço de um fantasma (personagem) como centro")
    ap.add_argument("--at", help="x,y,z do centro")
    ap.add_argument("-r", type=float, default=3.0, help="raio em metros")
    args = ap.parse_args()
    hk = hk_export.Hk(args.dump, args.rtti)
    phs = hk.phantoms()
    print("personagens:")
    for ph, sc, rot, t, info in phs:
        if sc == "hkpCapsuleShape":
            print(f"  {ph:08X} pos ({t[0]:7.2f},{t[1]:6.2f},{t[2]:7.2f}) filtro {decode_filter(hk.d.u32(ph + 0x28))}")
    center = None
    if args.phantom:
        pv = int(args.phantom, 16)
        center = next(t for ph, _, _, t, _ in phs if ph == pv)
    elif args.at:
        center = tuple(float(x) for x in args.at.split(","))
    if center is None:
        return 0
    print(f"\ncorpos a até {args.r} m de ({center[0]:.2f},{center[1]:.2f},{center[2]:.2f}):")
    rows = []
    for rb in hk.objects("hkpRigidBody"):
        shape = hk.d.u32(rb + 0x10)
        la = local_aabb(hk, shape)
        if not la:
            continue
        rot, t = hk.transform(rb, 0xE0, 0x110)
        wc, wh = world_box(rot, t, la[0], la[1])
        # distância do ponto à caixa do mundo
        dist = math.sqrt(sum(max(0.0, abs(center[i] - wc[i]) - wh[i]) ** 2 for i in range(3)))
        if dist > args.r:
            continue
        fixed = hk.d.u32(rb + 0xD0) == hk.vt["hkpFixedRigidMotion"]
        rows.append((dist, rb, fixed, la[4], la[2], la[3], wc, wh, hk.d.u32(rb + 0x28)))
    for dist, rb, fixed, kind, nv, npl, wc, wh, filt in sorted(rows):
        box = "CAIXA" if (nv == 8 and npl == 6) else ""
        print(f"  {rb:08X} {'fixo    ' if fixed else 'dinâmico'} {kind[3:]:22s} v={nv:<5d} pl={npl:<3d} {box:5s} "
              f"centro ({wc[0]:7.2f},{wc[1]:6.2f},{wc[2]:7.2f}) meia ({wh[0]:5.2f},{wh[1]:5.2f},{wh[2]:5.2f}) "
              f"d={dist:5.2f}  {decode_filter(filt)}")
    print(f"{len(rows)} corpos")
    return 0


if __name__ == "__main__":
    sys.exit(main())
