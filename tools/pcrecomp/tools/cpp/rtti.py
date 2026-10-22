#!/usr/bin/env python3
"""Recover C++ classes, vtables and virtual methods from a PE's MSVC RTTI.

A binary compiled as C++ with RTTI left enabled carries, in the shipping
executable, a complete description of its own class graph. Where it is present
this is the richest symbol source there is, because unlike a name-only string
table it points at *code*:

  * every polymorphic class name, as a decorated `.?AV...@@` TypeDescriptor
  * every vtable -- MSVC stores the CompleteObjectLocator at `vtable[-1]`, so a
    word pointing at a COL is immediately followed by the method array
  * every virtual method address, by walking that array
  * the inheritance chain, from the ClassHierarchyDescriptor

**The method addresses are proof of function entry points**, which is why this
is worth running *before* disassembly rather than after. A method that is only
ever called virtually is named by no CALL anywhere in the image and has no
guaranteed prologue, so neither the branch scan nor the prologue scan finds it.
Feed `--seeds` to `disasm32.py --seed-functions`.

Plenty of binaries are C, or C++ with RTTI disabled, and yield nothing. That is
a normal result, not a failure: callers get empty dicts and should carry on.

    python tools/cpp/rtti.py game.exe -o rtti.json --seeds config/seeds.json
    python tools/disasm/disasm32.py game.exe --seed-functions config/seeds.json

Structures (32-bit MSVC; all fields are plain VAs, with none of the
image-relative indirection x64 RTTI uses -- which is what makes this port from
the Xbox original almost free, since the original Xbox is 32-bit MSVC too):

    TypeDescriptor            { void *vfptr; void *spare; char name[]; }
    CompleteObjectLocator     { u32 sig; u32 offset; u32 cdOffset;
                                TypeDescriptor *pTD; ClassHierarchyDescriptor *pCD; }
    ClassHierarchyDescriptor  { u32 sig; u32 attributes; u32 numBaseClasses;
                                BaseClassDescriptor **pBaseClassArray; }
    BaseClassDescriptor       { TypeDescriptor *pTD; u32 numContainedBases;
                                PMD where; u32 attributes; }

Ported from xboxrecomp's `tools/rtti/`. See docs/CONSOLIDATION.md.
"""
import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pe"))

TD_NAME = re.compile(rb"\.\?A[VU][A-Za-z0-9_@?$]{2,250}@@\x00")


def demangle(name):
    """'.?AVCBloodSplats@@' -> 'CBloodSplats'. Template arguments stay decorated."""
    if name.startswith((".?AV", ".?AU")) and name.endswith("@@"):
        name = name[4:-2]
    if name.startswith("?$"):
        name = name[2:]
    return name


class Image:
    """Raw-bounded view over a PE.

    Only file-backed bytes are addressable. A section's virtual_size runs past
    raw_size for BSS, and reading there would walk off the end of the buffer.
    """

    def __init__(self, path):
        from pe_analyze import analyze_pe          # noqa: E402
        info = analyze_pe(path)
        with open(path, "rb") as f:
            self.d = f.read()
        self.image_base = info.image_base
        # (va, raw_offset, raw_size, name, is_code)
        self.secs = [(info.image_base + s.virtual_address, s.raw_offset,
                      s.raw_size, s.name, s.is_code)
                     for s in info.sections]
        self.code = [(v, v + rs) for v, _, rs, _, is_code in self.secs if is_code]

    def raw(self, va):
        for v, r, rs, _, _ in self.secs:
            if v <= va < v + rs:
                return r + (va - v)
        return None

    def u32(self, va):
        r = self.raw(va)
        if r is None or r + 4 > len(self.d):
            return None
        return struct.unpack_from("<I", self.d, r)[0]

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)

    def follow_thunk(self, va):
        """Target of a bare `jmp rel32` at `va`, or None if it is not one.

        MSVC's incremental linker does not put function addresses in vtables.
        It puts the address of a five-byte `jmp` stub, and parks the whole stub
        table at the front of .text, so that relinking after an edit only has
        to repoint one jump instead of every reference.

        Nothing downstream wants the stub. A linker map lists the real
        function, so a stub address matches nothing in it; the disassembler
        already finds the stub trivially and gains nothing from being told; and
        a name applied to the stub names the stub.

        Measured on Trespasser, which is incrementally linked: 2,230 of 2,243
        vtable slots point at a stub. Taken raw, 13 of 2,243 recovered methods
        (0.6%) are addresses the linker map calls a function. Followed one hop,
        **2,243 of 2,243** are -- every single one, exactly on a function start.
        The difference between this being the richest symbol source in the
        binary and being noise is five bytes of indirection.
        """
        b = self.raw(va)
        if b is None or b + 5 > len(self.d) or self.d[b] != 0xE9:
            return None
        target = (va + 5 + struct.unpack_from("<i", self.d, b + 1)[0]) & 0xFFFFFFFF
        return target if self.is_code(target) else None


def type_descriptors(img):
    """{typedescriptor_va: decorated_name}. The name field is 8 bytes in."""
    out = {}
    for v, r, rs, _, _ in img.secs:
        for m in TD_NAME.finditer(img.d[r:r + rs]):
            out[v + m.start() - 8] = m.group()[:-1].decode("latin1")
    return out


def locators(img, td):
    """{col_va: (decorated_name, subobject_offset, class_hierarchy_desc_va)}."""
    out = {}
    for v, r, rs, _, _ in img.secs:
        limit = min(rs, len(img.d) - r)
        for off in range(0, max(0, limit - 20), 4):
            sig, sub, _cd, ptd, pcd = struct.unpack_from("<5I", img.d, r + off)
            # sig is 0 for 32-bit MSVC RTTI; 1 means the image-relative x64
            # layout, which this does not parse.
            if sig != 0 or ptd not in td or img.raw(pcd) is None:
                continue
            out[v + off] = (td[ptd], sub, pcd)
    return out


def hierarchy(img, td, cols):
    """{decorated_name: [base names]}, in MSVC's depth-first preorder.

    The order is preorder, NOT most-derived-to-least: trailing entries are
    secondary inheritance branches. Do not read the last entry as the root.
    """
    out = {}
    for name, _sub, pcd in cols.values():
        if name in out:
            continue
        n, pba = img.u32(pcd + 8), img.u32(pcd + 12)
        if not n or not (0 < n < 200) or pba is None or img.raw(pba) is None:
            continue
        bases = []
        for i in range(n):
            b = img.u32(pba + i * 4)
            if b is None or img.raw(b) is None:
                break
            ptd = img.u32(b)
            if ptd not in td:
                break
            bases.append(td[ptd])
        else:
            out[name] = bases
    return out


def vtables(img, cols):
    """[(vtable_va, decorated_name, subobject_offset, [method VAs])].

    One entry per COL reference, so a class using multiple inheritance appears
    once per subobject.
    """
    out = []
    for v, r, rs, _, _is_code in img.secs:
        # Every section, including executable ones. The Xbox original looked
        # only in .rdata and .data, which is right for an XBE; on PE it is not.
        # A linker is free to merge read-only data into .text, and a modern
        # MSVC one routinely does -- charmap.exe keeps all four of its vtables
        # there, and restricting the scan to non-code sections found zero of
        # them while happily reporting the four class names.
        #
        # Scanning code costs nothing in false positives: the filter is a word
        # exactly equal to the address of a CompleteObjectLocator this image
        # actually contains, and the array after it must be code pointers.
        limit = min(rs, len(img.d) - r)
        for off in range(0, max(0, limit - 4), 4):
            w = struct.unpack_from("<I", img.d, r + off)[0]
            if w not in cols:
                continue
            start = v + off + 4
            methods = []
            while img.is_code(img.u32(start + len(methods) * 4) or 0):
                methods.append(img.u32(start + len(methods) * 4))
            if methods:
                name, sub, _ = cols[w]
                out.append((start, name, sub, methods))
    return out


def recover(path):
    """Everything, from a path. Empty results mean the binary has no RTTI."""
    img = Image(path)
    td = type_descriptors(img)
    cols = locators(img, td)
    vts = vtables(img, cols)

    # Vtable slots are kept exactly as the image holds them -- emulating a
    # virtual call needs the value that is really there. The stub-to-function
    # map rides alongside so seeds and names can use the real address.
    thunks = {}
    for _va, _n, _s, ms in vts:
        for m in ms:
            if m not in thunks:
                t = img.follow_thunk(m)
                if t is not None:
                    thunks[m] = t

    # The primary vtable is the subobject-offset-0 one; longest wins on ties.
    primary_len, primary_va = {}, {}
    for va, name, sub, methods in vts:
        if sub == 0 and len(methods) > primary_len.get(name, 0):
            primary_len[name], primary_va[name] = len(methods), va

    return {
        "image": img,
        "type_descriptors": td,
        "locators": cols,
        "hierarchy": hierarchy(img, td, cols),
        "vtables": vts,
        "primary_len": primary_len,
        "primary_va": primary_va,
        "thunks": thunks,
    }


def seeds(result):
    """Sorted virtual-method addresses, for disasm32 --seed-functions.

    A vtable slot is proof of a function entry point, which the branch scan and
    the prologue scan both miss for a method only ever called virtually.

    Incremental-link stubs are followed to the function they jump to -- see
    Image.follow_thunk for why that is the difference between 0.6% and 100%
    agreement with a linker map.
    """
    t = result.get("thunks", {})
    return sorted({t.get(m, m)
                   for _, _, _, ms in result["vtables"] for m in ms})


def methods_by_class(result):
    """{method_va: {demangled class names whose vtable holds it}}.

    A method in exactly one set is uniquely attributable. Which *ancestor*
    declared a given slot is deliberately not inferred -- with multiple
    inheritance that needs MSVC layout modelling, and the preorder base array
    does not answer it.
    """
    t = result.get("thunks", {})
    out = {}
    for _va, name, _sub, ms in result["vtables"]:
        for m in ms:
            out.setdefault(t.get(m, m), set()).add(demangle(name))
    return out


def owning_class(result):
    """{method_va: owner class name} for methods whose owner is well defined.

    A method address appearing in several vtables is one implementation shared
    by those classes -- inherited, not duplicated. The class that declared it is
    then the one that is an ancestor of every other class in the set. Unlike
    "which slot did which ancestor declare", this needs only the hierarchy sets
    and is unambiguous.

    Methods with no common ancestor (multiple inheritance, or unrelated classes
    sharing a compiler-generated thunk) are omitted rather than guessed at.
    """
    ancestors = {demangle(k): {demangle(b) for b in v}
                 for k, v in result["hierarchy"].items()}
    out = {}
    for addr, classes in methods_by_class(result).items():
        if len(classes) == 1:
            out[addr] = next(iter(classes))
            continue
        owners = [c for c in classes
                  if all(c in ancestors.get(o, ()) for o in classes)]
        if len(owners) == 1:
            out[addr] = owners[0]
    return out


def names(result):
    """{"0xADDR": "Class__ADDR"} for whatever applies names downstream.

    The method's own name is not recoverable -- RTTI carries class names, not
    member names -- so the address is kept to stay unique and the class is
    prepended. "CBloodSplats__004A1C30" beats "sub_004A1C30" both for reading
    generated code and for reading a crash stack.
    """
    return {f"0x{addr:08X}": f"{owner}__{addr:08X}"
            for addr, owner in owning_class(result).items()}


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", nargs="?", help="the 32-bit PE to read RTTI from")
    ap.add_argument("-o", "--out", help="write the full recovery as JSON")
    ap.add_argument("--seeds", help="write virtual-method addresses for "
                                    "disasm32.py --seed-functions")
    ap.add_argument("--names", help="write {address: Class__ADDR} names")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="list every class with its vtable size")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        demo()
        return 0
    if not args.exe:
        ap.error("an executable is required (or --selftest)")

    r = recover(args.exe)
    classes = sorted({demangle(n) for _, n, _, _ in r["vtables"]})
    method_addrs = seeds(r)
    owned = owning_class(r)

    print(f"[*] {os.path.basename(args.exe)}")
    print(f"[*] type descriptors : {len(r['type_descriptors']):,}")
    print(f"[*] complete object locators : {len(r['locators']):,}")
    print(f"[*] vtables          : {len(r['vtables']):,}")
    print(f"[*] classes          : {len(classes):,}")
    print(f"[*] virtual methods  : {len(method_addrs):,}  "
          f"({len(owned):,} attributable to one class)")
    if not r["type_descriptors"]:
        print("[*] No RTTI. This binary is C, or was built with RTTI off. "
              "That is a normal result.")

    if args.verbose:
        for c in classes:
            n = r["primary_len"].get(f".?AV{c}@@", 0)
            bases = r["hierarchy"].get(f".?AV{c}@@", [])
            print(f"    {c:<48} {n:>4} slots"
                  + (f"  : {', '.join(demangle(b) for b in bases[1:4])}" if len(bases) > 1 else ""))

    if args.out:
        doc = {
            "binary": os.path.basename(args.exe),
            "classes": {
                demangle(n): {
                    "vtable": f"0x{r['primary_va'].get(n, 0):08X}",
                    "slots": r["primary_len"].get(n, 0),
                    "bases": [demangle(b) for b in r["hierarchy"].get(n, [])[1:]],
                } for n in {nm for _, nm, _, _ in r["vtables"]}
            },
            "methods": {f"0x{a:08X}": owner for a, owner in sorted(owned.items())},
        }
        with open(args.out, "w") as f:
            json.dump(doc, f, indent=1)
        print(f"[*] wrote {args.out}")

    if args.seeds:
        doc = [{"address": a, "address_hex": f"0x{a:08X}",
                "note": "virtual method from RTTI vtable"} for a in method_addrs]
        with open(args.seeds, "w") as f:
            json.dump(doc, f, indent=1)
        print(f"[*] wrote {args.seeds} ({len(doc):,} seeds)")

    if args.names:
        with open(args.names, "w") as f:
            json.dump(names(r), f, indent=1)
        print(f"[*] wrote {args.names} ({len(owned):,} names)")

    return 0


def demo():
    """One runnable check: build a tiny image with real RTTI records in it."""
    class _Sec:
        def __init__(self, name, va, off, size, chars):
            self.name, self.virtual_address = name, va
            self.raw_offset, self.raw_size = off, size
            self.virtual_size, self.characteristics = size, chars

        @property
        def is_code(self):
            return bool(self.characteristics & 0x20000020)

    BASE = 0x400000
    # Layout: .text at RVA 0x1000 (file 0x400), .rdata at RVA 0x2000 (file 0x1400).
    TEXT_VA, RDATA_VA = BASE + 0x1000, BASE + 0x2000
    rdata = bytearray(0x200)

    def put(off, *words):
        struct.pack_into("<%dI" % len(words), rdata, off, *words)

    # TypeDescriptor at rdata+0x00: vfptr, spare, then the name at +8.
    name = b".?AVCWidget@@\x00"
    put(0x00, 0, 0)
    rdata[0x08:0x08 + len(name)] = name
    td_va = RDATA_VA + 0x00

    # ClassHierarchyDescriptor at +0x40: sig, attrs, numBases, pBaseArray
    chd_va = RDATA_VA + 0x40
    bca_va = RDATA_VA + 0x60
    put(0x40, 0, 0, 1, bca_va)
    # BaseClassDescriptor array: one entry, pointing at a BCD at +0x70
    bcd_va = RDATA_VA + 0x70
    put(0x60, bcd_va)
    put(0x70, td_va, 0, 0, 0)

    # CompleteObjectLocator at +0x80: sig=0, offset=0, cdOffset=0, pTD, pCHD
    col_va = RDATA_VA + 0x80
    put(0x80, 0, 0, 0, td_va, chd_va)

    # The vtable: the COL word, then three method pointers into .text.
    m = [TEXT_VA + 0x10, TEXT_VA + 0x40, TEXT_VA + 0x80]
    put(0xA0, col_va, m[0], m[1], m[2], 0)

    data = bytearray(0x1600)
    data[0x1400:0x1400 + len(rdata)] = rdata

    img = Image.__new__(Image)
    img.d = bytes(data)
    img.image_base = BASE
    img.secs = [(TEXT_VA, 0x400, 0x1000, ".text", True),
                (RDATA_VA, 0x1400, 0x200, ".rdata", False)]
    img.code = [(TEXT_VA, TEXT_VA + 0x1000)]

    td = type_descriptors(img)
    assert td == {td_va: ".?AVCWidget@@"}, td

    cols = locators(img, td)
    assert col_va in cols, f"the COL should be found: {[hex(k) for k in cols]}"
    assert cols[col_va][0] == ".?AVCWidget@@" and cols[col_va][1] == 0, cols[col_va]

    h = hierarchy(img, td, cols)
    assert h == {".?AVCWidget@@": [".?AVCWidget@@"]}, h

    vts = vtables(img, cols)
    assert len(vts) == 1, vts
    va, nm, sub, ms = vts[0]
    assert va == RDATA_VA + 0xA4, hex(va)
    assert ms == m, [hex(x) for x in ms]
    assert sub == 0

    r = {"image": img, "type_descriptors": td, "locators": cols,
         "hierarchy": h, "vtables": vts,
         "primary_len": {".?AVCWidget@@": 3},
         "primary_va": {".?AVCWidget@@": va}}
    assert seeds(r) == sorted(m), "every vtable slot is a function entry point"
    assert owning_class(r) == {a: "CWidget" for a in m}
    assert names(r)[f"0x{m[0]:08X}"] == f"CWidget__{m[0]:08X}"

    # The method array stops at the first non-code word: the trailing 0 above
    # must not be swallowed, or every vtable runs into whatever follows it.
    assert len(ms) == 3, "the array ends where the code pointers end"

    assert demangle(".?AUPlainStruct@@") == "PlainStruct"
    assert demangle(".?AV?$vector@H@std@@").startswith("vector@H@std"), \
        "template arguments stay decorated rather than being half-parsed"

    print("rtti.py self-test OK")


if __name__ == "__main__":
    sys.exit(main())
