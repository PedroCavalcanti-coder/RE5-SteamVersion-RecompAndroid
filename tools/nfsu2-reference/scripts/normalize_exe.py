#!/usr/bin/env python3
"""Make an ANALYSIS copy of SPEED2.EXE with its base-relocation directory restored.

The local SPEED2.EXE carries its original .reloc data in an unnamed section at
RVA 0x4F5000 (blocks of IMAGE_BASE_RELOCATION, 91,105 HIGHLOW entries) but its
data directory [5] is zeroed. pcrecomp's tools read relocations through the data
directory, so this writes a copy where:

  * DataDirectory[BASERELOC] = (section VA, parsed length of the block chain)
  * that section is named ".reloc"

Nothing else changes; code/data bytes are identical. The original is never
written. Output defaults to work/SPEED2.analysis.exe (gitignored).
"""
import struct, sys, hashlib, pefile

def main(src, dst):
    pe = pefile.PE(src)
    sec = None
    for s in pe.sections:
        d = s.get_data()
        if len(d) >= 8:
            va, sz = struct.unpack_from("<II", d, 0)
            if va == 0x1000 and 8 < sz < 0x1000 and s.Characteristics & 0x02000000:
                sec = s
    if sec is None:
        sys.exit("no discardable section that starts with a reloc block")
    d = sec.get_data()
    off = 0
    while off + 8 <= len(d):
        va, sz = struct.unpack_from("<II", d, off)
        if sz < 8 or va == 0 or va >= pe.OPTIONAL_HEADER.SizeOfImage: break
        off += sz
    dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']]
    dd.VirtualAddress, dd.Size = sec.VirtualAddress, off
    sec.Name = b".reloc\0\0"
    pe.write(dst)
    print("reloc dir: VA %#x size %#x -> %s" % (sec.VirtualAddress, off, dst))
    print("sha256 src", hashlib.sha256(open(src, "rb").read()).hexdigest())
    print("sha256 dst", hashlib.sha256(open(dst, "rb").read()).hexdigest())

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "work/SPEED2.analysis.exe")
