#!/usr/bin/env python3
"""Lista e extrai arquivos de um .arc do MT Framework (RE5: versão 7, entradas zlib).

    python scripts/arc_tool.py lista bin_input/game/nativePC_MT/Image/Archive/s100.arc [filtro]
    python scripts/arc_tool.py extrai s100.arc "nome da entrada" saida.bin

Formato: "ARC\\0", u16 versão, u16 nº de arquivos; depois nº x 0x50 bytes:
nome[64], u32 tipo (hash), u32 tamanho comprimido, u32 tamanho (29 bits) | flags (3 bits), u32 deslocamento.
Os dados de cada entrada são um fluxo zlib. Só para análise local dos dados do próprio usuário.
"""
import struct
import sys
import zlib


def entries(path):
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"ARC\0":
            raise SystemExit(f"{path}: não é um ARC ({head[:4]!r})")
        version, count = struct.unpack("<HH", head[4:])
        raw = f.read(count * 0x50)
    out = []
    for i in range(count):
        name, typ, csize, size, off = struct.unpack_from("<64sIIII", raw, i * 0x50)
        out.append((name.split(b"\0")[0].decode("latin1"), typ, csize, size & 0x1FFFFFFF, size >> 29, off))
    return version, out


def read_entry(path, e):
    name, typ, csize, size, flags, off = e
    with open(path, "rb") as f:
        f.seek(off)
        blob = f.read(csize)
    try:
        return zlib.decompress(blob)
    except zlib.error:
        return blob


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, path = sys.argv[1], sys.argv[2]
    version, es = entries(path)
    if cmd == "lista":
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        print(f"versão {version}, {len(es)} entradas")
        for name, typ, csize, size, flags, off in es:
            if flt in name.lower():
                print(f"{name:60s} tipo {typ:08X} {csize:9d} -> {size:9d} fl {flags} @{off:X}")
    elif cmd == "extrai":
        want = sys.argv[3].lower()
        for e in es:
            if e[0].lower() == want:
                open(sys.argv[4], "wb").write(read_entry(path, e))
                print("ok", len(read_entry(path, e)), "bytes")
                return 0
        print("entrada não encontrada")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
