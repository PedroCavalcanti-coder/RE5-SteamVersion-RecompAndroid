#!/usr/bin/env python3
"""
pdb_ref.py - a scoring reference from the vendor's own symbols.

Microsoft publishes a PDB for every Windows binary on its symbol server. The
procedure records in it are what the compiler emitted, so they are ground
truth rather than a second opinion like an IDA export: the reference
score_recovery is meant to have.

The PDB is fetched by the CodeView id in the image's debug directory, cached,
and read with llvm-pdbutil (LLVM, Apache-2.0, run as a tool). The PDB and the
reference it produces are derived from a proprietary binary: they stay in a
gitignored work directory and are never committed.

Usage:
  python pdb_ref.py <image> -o ref.json [--cache DIR] [--pdbutil PATH]
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.error
import urllib.request

SYMSRV = 'https://msdl.microsoft.com/download/symbols'
PROC_RE = re.compile(r'S_[GL]PROC32(?:_ID)? \[size = \d+\] `([^`]*)`\s*\n'
                     r'(?:.*\n)*?\s*addr = (\d+):(\d+), code size = (\d+)')
PUB_RE = re.compile(r'S_PUB32 \[size = \d+\] `([^`]*)`\s*\n\s*flags = ([^,]*), addr = (\d+):(\d+)')
SC_RE = re.compile(r'SC\[[^\]]*\]\s*\| mod = \d+, (\d+):(\d+), size = (\d+),[^\n]*\n\s*([^\n]*)')


def codeview_id(data):
    """(pdb name, 'GUIDAGE' symbol-server key) from the RSDS debug record."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    ddoff = opt + (112 if magic == 0x20b else 96)
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    sh = opt + struct.unpack_from('<H', data, pe + 20)[0]
    secs = [struct.unpack_from('<8sIIII', data, sh + 40 * i) for i in range(nsec)]

    def off(rva):
        for _, vs, va, rs, ra in secs:
            if va <= rva < va + max(vs, rs):
                return ra + rva - va
        return None

    rva, size = struct.unpack_from('<II', data, ddoff + 6 * 8)
    o = off(rva) if rva else None
    for i in range(size // 28 if o is not None else 0):
        typ, _sz, _rva, ptr = struct.unpack_from('<12xIIII', data, o + 28 * i)
        if typ == 2 and data[ptr:ptr + 4] == b'RSDS':
            g = data[ptr + 4:ptr + 20]
            age = struct.unpack_from('<I', data, ptr + 20)[0]
            name = data[ptr + 24:data.index(b'\0', ptr + 24)].decode('latin-1')
            d1, d2, d3 = struct.unpack_from('<IHH', g)
            key = '%08X%04X%04X%s%X' % (d1, d2, d3, g[8:].hex().upper(), age)
            return os.path.basename(name.replace('\\', '/')), key, secs
    raise SystemExit('no RSDS CodeView record: no public PDB to fetch')


def fetch(name, key, cache):
    path = os.path.join(cache, name, key, name)
    if os.path.exists(path):
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + '.part'
    # The CDN behind msdl stalls mid-file without closing the connection: a
    # read with no timeout hung kernelbase.pdb at 11 MB for ten minutes. Time
    # out, then resume from what arrived rather than starting over.
    for attempt in range(12):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        hdr = {'User-Agent': 'Microsoft-Symbol-Server/10.0'}
        if have:
            hdr['Range'] = 'bytes=%d-' % have
        req = urllib.request.Request('%s/%s/%s/%s' % (SYMSRV, name, key, name), headers=hdr)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                mode = 'ab' if have and r.status == 206 else 'wb'
                with open(part, mode) as f:
                    shutil.copyfileobj(r, f)
            os.replace(part, path)
            return path
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise SystemExit('%s %s is not on the symbol server' % (name, key))
            if e.code == 416 and have:          # nothing past what we hold: it is whole
                os.replace(part, path)
                return path
            print('[!] %s: %s, retrying (%d)' % (name, e, attempt + 1))
        except OSError as e:
            print('[!] %s: %s, retrying (%d)' % (name, e, attempt + 1))
    raise SystemExit('could not fetch %s from the symbol server' % name)


def _dump(pdbutil, what, pdb):
    return subprocess.run([pdbutil, 'dump', what, pdb], capture_output=True,
                          text=True, errors='replace', check=True).stdout


def procs(pdb, pdbutil):
    """[(section, offset, size, name)] for every procedure in every module.

    A public Microsoft PDB is stripped: no procedure records, so no sizes.
    What survives is the publics (a start and a name, but only for functions
    with external linkage) and the section contributions. Windows is built
    /Gy, so each function sits in its own COMDAT contribution and that
    contribution is the function's range; COMDATs with no public are the
    static functions the publics leave out. A non-COMDAT contribution (an
    assembler module, say) holds many functions and gives no ranges.
    Size 0 means the end is unknown.
    """
    full = [(int(s), int(o), int(n), nm)
            for nm, s, o, n in PROC_RE.findall(_dump(pdbutil, '--symbols', pdb))]
    if full:
        return full
    comdat = {(int(s), int(o)): int(n)
              for s, o, n, fl in SC_RE.findall(_dump(pdbutil, '--section-contribs', pdb))
              if 'CNT_CODE' in fl and 'LNK_COMDAT' in fl}
    out, named = [], set()
    for nm, fl, s, o in PUB_RE.findall(_dump(pdbutil, '--publics', pdb)):
        if 'function' in fl:
            k = (int(s), int(o))
            named.add(k)
            out.append((k[0], k[1], comdat.get(k, 0), nm))
    out += [(s, o, n, None) for (s, o), n in comdat.items() if (s, o) not in named]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('image')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--cache', default='symbols')
    ap.add_argument('--pdbutil', default=shutil.which('llvm-pdbutil') or
                    r'C:\Program Files\LLVM\bin\llvm-pdbutil.exe')
    a = ap.parse_args(argv)
    data = open(a.image, 'rb').read()
    name, key, secs = codeview_id(data)
    pdb = fetch(name, key, a.cache)
    base = struct.unpack_from('<Q', data, struct.unpack_from('<I', data, 0x3c)[0] + 48)[0]  # PE32+ only
    # A PGO build moves cold blocks into COMDATs of their own, with no public
    # and a chained .pdata entry: fragments of another function, not
    # functions. Taking them as statics put 414 false misses into
    # kernelbase's score.
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'disasm'))
    # ntoskrnl also puts string literals in code COMDATs: CNT_CODE, no
    # public, and not code (3,290 of them on 26100).
    from disasm64 import PE64, looks_like_string
    chunks = PE64(data).pdata()[1]
    funcs = {}
    for sec, off, size, nm in procs(pdb, a.pdbutil):
        if 1 <= sec <= len(secs):
            va = base + secs[sec - 1][2] + off
            ro = secs[sec - 1][4] + off
            if nm is None and (va in chunks or looks_like_string(data[ro:ro + 256])):
                continue
            # A COMDAT's size includes the int3/nop padding after the last
            # instruction; a disassembler's end does not (score_recovery).
            # Not 0x00, which is a real encoding.
            while size and data[ro + size - 1] in (0xCC, 0x90):
                size -= 1
            funcs.setdefault(va, {'address': va, 'end': va + size if size else None,
                                  'name': nm or 'static_%X' % va})
    fl = sorted(funcs.values(), key=lambda f: f['address'])
    with open(a.output, 'w') as f:
        json.dump({'pdb': name, 'id': key, 'functions': fl}, f, indent=0)
    print('[*] %s %s: %d functions (%d with ranges, %d static) -> %s'
          % (name, key, len(fl), sum(f['end'] is not None for f in fl),
             sum(f['name'].startswith('static_') for f in fl), a.output))


if __name__ == '__main__':
    main()
