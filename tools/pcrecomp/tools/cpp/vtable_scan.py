#!/usr/bin/env python3
"""Find C++ vtables in a PE without needing RTTI, and the methods in them.

`rtti.py` is better wherever it works: a CompleteObjectLocator is an exact
match, so it yields class names and an inheritance graph with no guessing. But
most binaries are built with RTTI off, and then the vtables are still there --
just anonymous. This finds them structurally, as **runs of consecutive code
pointers in a data section**.

A method reached only through a vtable slot is called by no CALL anywhere in
the image, so the branch scan never names it, and an optimised `__thiscall`
accessor has no `push ebp; mov ebp, esp` for the prologue scan to match. Those
methods are exactly what a recompile loses, and they surface at runtime as an
unresolved indirect call. Feed `--seeds` to `disasm32.py --seed-functions`.

    python tools/cpp/vtable_scan.py game.exe --seeds config/seeds.json

Why a run and not a pointer
---------------------------
`disasm32.find_data_code_pointers` already treats every isolated word that
looks like a code address as a function, which is why it invents so many. A
*run* of three or more is a far stronger signal: data rarely holds three
consecutive words that all land in executable memory, and a vtable always
does. This is the principled version of that scan, not a replacement for it --
the unaligned scan still catches packed pointer arrays that are not vtables.

Every candidate method is then put through `probes_as_function_body`, because
a run of code pointers can also be a jump table, an import thunk table, or a
relocation list, and seeding data as code splits real functions.

Ported from xboxrecomp's `tools/func_id/vtable_scanner.py`.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pe"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "disasm"))

# Three consecutive code pointers. Two is common in ordinary data; three is
# not, and a vtable with fewer than three slots is rare enough that chasing it
# costs more in false positives than it recovers. Measured on Trespasser,
# where RTTI gives the true answer: 90 of 449 real vtables have fewer than
# three slots and are structurally invisible to this. That is the price.
MIN_VTABLE_ENTRIES = 3

# Sections excluded by what they are FOR, not by whitelisting what they are
# not. Whitelisting ".rdata and .data" is how the RTTI port first missed every
# vtable in charmap.exe, which keeps them in .text -- so anything unrecognised
# is scanned, and only sections with a known non-vtable purpose are skipped.
#
# Measured on Trespasser, taking the fraction of each candidate's slots that
# are real linker-map function starts:
#
#   .rdata   85.3%  -- real vtables, including 216 that RTTI does not describe
#   .data     0.0%  -- 12 candidates, 36 slots, none of them functions
#   .idata    0.0%  -- 51 candidates: the import thunk table, by construction
#   .rsrc     1.2%  -- 466 candidates: resource blobs that happen to look like
#                      addresses, and by volume the single largest source of
#                      noise in the whole scan
#
# .data stays in despite scoring 0% here: a non-const vtable is legal and lands
# there, and one binary is not enough to rule it out. The excluded four are
# excluded because their contents are defined to be something else.
SKIP_SECTIONS = {".rsrc", ".idata", ".edata", ".reloc", ".tls", ".00cfg",
                 ".pdata", ".didat", ".debug"}


def find_vtables(dis, sections, image_base, data, include_code=False):
    """[{address, entries, section}] for every run of >= 3 code pointers.

    Any address in an executable section counts as an entry, not just a known
    function start -- that is the point, since the entries this finds are the
    ones nothing else knows about.
    """
    out = []
    for s in sections:
        if s.is_code and not include_code:
            continue
        if s.name.lower() in SKIP_SECTIONS:
            continue
        raw, size = s.raw_offset, min(s.raw_size, len(data) - s.raw_offset)
        if size <= 4:
            continue
        blob = data[raw:raw + size]
        va = image_base + s.virtual_address
        i = 0
        while i < len(blob) - 4:
            if not dis.is_code_address(struct.unpack_from("<I", blob, i)[0]):
                i += 4
                continue
            entries, j = [], i
            while j < len(blob) - 4:
                w = struct.unpack_from("<I", blob, j)[0]
                if not dis.is_code_address(w):
                    break
                entries.append(w)
                j += 4
            if len(entries) >= MIN_VTABLE_ENTRIES:
                out.append({"address": va + i, "entries": entries,
                            "section": s.name})
                i = j
            else:
                i += 4
    return out


def filter_vtables(vtables):
    """Drop runs that are obviously a data table rather than a vtable.

    All three tests are about *regularity*. A vtable's slots point at whatever
    the compiler laid out and have no pattern; a table of offsets, a jump
    table, or an array of adjacent stubs does.
    """
    out = []
    for vt in vtables:
        e = vt["entries"]
        # Every slot the same address: a filler array, not a class.
        if len(set(e)) == 1:
            continue
        # A perfect arithmetic progression with a small step: an array of
        # equally-sized stubs, e.g. an import or a dispatch thunk table.
        if len(e) >= 4:
            diffs = [e[k + 1] - e[k] for k in range(len(e) - 1)]
            if len(set(diffs)) == 1 and diffs[0] <= 16:
                continue
        # Mostly-adjacent addresses: the same thing, less regularly emitted.
        if len(e) >= 6:
            near = sum(1 for k in range(len(e) - 1) if abs(e[k + 1] - e[k]) <= 8)
            if near > len(e) * 0.8:
                continue
        out.append(vt)
    return out


def scan(exe_path, include_code=False, probe=True):
    """(vtables, methods). `methods` is sorted and probe-gated."""
    from pe_analyze import analyze_pe          # noqa: E402
    from disasm32 import Disassembler          # noqa: E402

    info = analyze_pe(exe_path)
    with open(exe_path, "rb") as f:
        data = f.read()
    dis = Disassembler(data, info.image_base, info.sections)

    vts = filter_vtables(find_vtables(dis, info.sections, info.image_base,
                                      data, include_code))
    seen = {m for vt in vts for m in vt["entries"]}
    if probe:
        # A run of code pointers can still be a jump table or a relocation
        # list. Decoding each candidate is the same corroboration the
        # data-pointer scan gets, and for the same reason.
        seen = {m for m in seen if dis.probes_as_function_body(m)}
    return vts, sorted(seen), dis


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", nargs="?", help="the 32-bit PE to scan")
    ap.add_argument("--seeds", help="write method addresses for "
                                    "disasm32.py --seed-functions")
    ap.add_argument("-o", "--out", help="write the vtables as JSON")
    ap.add_argument("--include-code", action="store_true",
                    help="also scan executable sections. Needed when the "
                         "linker merged read-only data into .text, which "
                         "modern MSVC does; noisier, so it is not the default.")
    ap.add_argument("--no-probe", action="store_true",
                    help="do not require each method to decode as a function "
                         "body (reports what the raw heuristic found)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        demo()
        return 0
    if not args.exe:
        ap.error("an executable is required (or --selftest)")

    vts, methods, _ = scan(args.exe, args.include_code, probe=not args.no_probe)
    by_sec = {}
    for vt in vts:
        by_sec[vt["section"]] = by_sec.get(vt["section"], 0) + 1

    print(f"[*] {os.path.basename(args.exe)}")
    print(f"[*] vtable candidates : {len(vts):,}  " +
          ("(" + ", ".join(f"{n}: {c:,}" for n, c in sorted(by_sec.items())) + ")"
           if by_sec else ""))
    print(f"[*] distinct methods  : {len(methods):,}")
    if vts:
        sizes = sorted(len(v["entries"]) for v in vts)
        print(f"[*] slots per vtable  : min {sizes[0]}, "
              f"median {sizes[len(sizes) // 2]}, max {sizes[-1]}")
    if not vts:
        print("[*] No vtable-shaped runs found. If this is a C++ binary, try "
              "--include-code: the linker may have merged .rdata into .text.")

    if args.out:
        with open(args.out, "w") as f:
            json.dump([{"address": f"0x{v['address']:08X}",
                        "section": v["section"],
                        "slots": len(v["entries"]),
                        "entries": [f"0x{e:08X}" for e in v["entries"]]}
                       for v in vts], f, indent=1)
        print(f"[*] wrote {args.out}")

    if args.seeds:
        with open(args.seeds, "w") as f:
            json.dump([{"address": a, "address_hex": f"0x{a:08X}",
                        "note": "method from a vtable-shaped run of code pointers"}
                       for a in methods], f, indent=1)
        print(f"[*] wrote {args.seeds} ({len(methods):,} seeds)")
    return 0


def demo():
    """One runnable check: a real run is kept, each filtered shape is dropped."""
    class _Sec:
        def __init__(self, name, va, off, size, chars):
            self.name, self.virtual_address = name, va
            self.raw_offset, self.raw_size = off, size
            self.virtual_size, self.characteristics = size, chars

        @property
        def is_code(self):
            return bool(self.characteristics & 0x20000020)

    from disasm32 import Disassembler

    BASE = 0x400000
    TEXT, RDATA = 0x1000, 0x2000
    data = bytearray(0x1600)
    rd = bytearray(0x200)

    def put(off, *words):
        struct.pack_into("<%dI" % len(words), rd, off, *words)

    # A real vtable: four unrelated code addresses, then a zero to end it.
    put(0x00, BASE + TEXT + 0x10, BASE + TEXT + 0x140,
        BASE + TEXT + 0x80, BASE + TEXT + 0x300, 0)
    # All identical.
    put(0x20, *([BASE + TEXT + 0x10] * 4), 0)
    # Arithmetic progression, step 16: a stub table.
    put(0x40, *[BASE + TEXT + 0x10 + 16 * k for k in range(5)], 0)
    # Mostly adjacent, step 4: also a table.
    put(0x60, *[BASE + TEXT + 0x200 + 4 * k for k in range(7)], 0)
    # Only two entries: below the threshold, deliberately.
    put(0x90, BASE + TEXT + 0x20, BASE + TEXT + 0x60, 0)

    data[0x1400:0x1400 + len(rd)] = rd
    secs = [_Sec(".text", TEXT, 0x400, 0x1000, 0x20000020),
            _Sec(".rdata", RDATA, 0x1400, 0x200, 0x40000040)]
    dis = Disassembler(bytes(data), BASE, secs)

    raw = find_vtables(dis, secs, BASE, bytes(data))
    found = {v["address"]: len(v["entries"]) for v in raw}
    assert BASE + RDATA + 0x00 in found, "a run of four code pointers is a vtable"
    assert found[BASE + RDATA + 0x00] == 4, found
    assert BASE + RDATA + 0x90 not in found, \
        "two entries is below MIN_VTABLE_ENTRIES and must not be reported"

    kept = {v["address"] for v in filter_vtables(raw)}
    assert BASE + RDATA + 0x00 in kept, "the real one survives filtering"
    assert BASE + RDATA + 0x20 not in kept, "all-identical entries are filler"
    assert BASE + RDATA + 0x40 not in kept, "an even stride is a stub table"
    assert BASE + RDATA + 0x60 not in kept, "adjacent addresses are a data table"

    # The scan must not walk into the code section by default: .text here is
    # full of zeros, but a real one is full of bytes that look like anything.
    assert all(v["section"] == ".rdata" for v in raw), \
        "executable sections are skipped unless asked for"

    # A section whose purpose is not holding vtables is skipped whatever is in
    # it. .rsrc was 466 of 1,104 candidates on Trespasser and 1.2% real.
    rsrc = _Sec(".rsrc", 0x3000, 0x1400, 0x200, 0x40000040)
    assert find_vtables(dis, [rsrc], BASE, bytes(data)) == [], \
        "a known non-vtable section is skipped even when its bytes look right"

    print("vtable_scan.py self-test OK")


if __name__ == "__main__":
    sys.exit(main())
