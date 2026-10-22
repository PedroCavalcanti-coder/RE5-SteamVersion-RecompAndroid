#!/usr/bin/env python3
"""Make an ANALYSIS copy of a game DLL relocated to its fixed guest base, plus seeds.

  prepare_module.py <game.dll> <base> <analysis.dll> <seeds.json>

Lifted code bakes absolute addresses, so a DLL is lifted as if loaded at the
base the runtime maps it at (server.dll: 0x00A00000, between SPEED2.EXE's image
and the first heap arena on both ABIs). The copy has every base relocation
applied for that base; the runtime applies the same relocations to the
original file at load time. Seeds: the entry point, every export and every
relocated pointer that lands in code (vtables, callbacks, thread starts and
jump-table targets). Both outputs are derived from the game binary: write
them under ../work (never committed). The original file is never written.
"""
import json, sys, pefile


def main(src, base, dst, seeds_path):
    pe = pefile.PE(src)
    old = pe.OPTIONAL_HEADER.ImageBase
    pe.relocate_image(base)
    pe.OPTIONAL_HEADER.ImageBase = base
    pe.write(dst)

    pe = pefile.PE(dst)
    code = [s for s in pe.sections if s.Characteristics & 0x20000000]
    def in_code(va):
        return any(base + s.VirtualAddress <= va < base + s.VirtualAddress + s.Misc_VirtualSize for s in code)
    seeds = [{"address": base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, "note": "entry"}]
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        seeds.append({"address": base + e.address, "note": "export " + (e.name or b"#%d" % e.ordinal).decode()})
    image = pe.get_memory_mapped_image()
    pointers = set()
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for entry in block.entries:
            if entry.type != 3:  # IMAGE_REL_BASED_HIGHLOW
                continue
            value = int.from_bytes(image[entry.rva:entry.rva + 4], "little")
            if in_code(value):
                pointers.add(value)
    seeds += [{"address": va, "note": "relocated code pointer"} for va in sorted(pointers)]
    json.dump(seeds, open(seeds_path, "w"), indent=1)
    print(f"[*] {src}: base {old:#x} -> {base:#x}, {len(seeds)} seeds ({len(pointers)} code pointers) -> {dst}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2], 0), sys.argv[3], sys.argv[4])
