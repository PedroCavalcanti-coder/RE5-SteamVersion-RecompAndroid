#!/usr/bin/env python3
"""
InstallShield 3 archive extractor (the `.Z` / `.LIB` / `NAME.1 NAME.2` era).

InstallShield 3 predates the .hdr/.cab pair `isextract.py` reads: each archive
is one file that starts with the signature 0x8C655D13, holds its files as
PKWARE DCL "implode" streams, and ends with a table of contents. Win16 titles
from 1994-96 ship this way (Dogz, Oddballz, the SETUP.EXE + *.Z discs).

The reason this exists instead of shelling out to i3comp: a disk-spanning set
(`ODDLINGZ.1`, `ODDLINGZ.2`) splits a file's compressed stream across two
volumes, and i3comp run on either half extracts nothing for that file -- on
Oddballz that file is ODDBALLZ.WAD, the game itself. i3comp also silently skips
files in subdirectories unless the directories already exist. Here a split
file is the tail of volume N (from its offset) followed by volume N+1 from the
end of its header, which is all the header's split fields describe.

Layout (little-endian), established against Dogz and Oddballz:

  header, 0xFF bytes:
    0x00 u32 signature    0x0C u16 file count   0x29 u32 TOC offset
    0x31 u16 directory count
  TOC: directories, then files
    dir:  u16 nfiles, u16 entry size, u16 name len, name
    file: +0 u8 last volume, +1 u16 dir index, +3 u32 size, +7 u32 packed size,
          +0x0B u32 offset (in the first volume), +0x0F u32 DOS date/time,
          +0x17 u16 entry size, +0x1A u8 split, +0x1C u8 first volume,
          +0x1D u8 name len, +0x1E name

Usage:
  python tools/assets/is3extract.py ARCHIVE [ARCHIVE...] -o out/ [--list]
  python tools/assets/is3extract.py --selftest

Spanned sets are found from any member: pass ODDLINGZ.1 and ODDLINGZ.2 is read
too.
"""
import argparse
import glob
import os
import re
import struct
import sys

SIGNATURE = 0x8C655D13
HEADER_SIZE = 0xFF


# --- PKWARE DCL explode -------------------------------------------------------
# A Python port of blast.c from zlib's contrib/blast, altered from the original
# (translated to Python, restructured). Its notice, which must stay with it:
#
#   Copyright (C) 2003, 2012, 2013 Mark Adler
#   version 1.3, 24 Aug 2013
#
#   This software is provided 'as-is', without any express or implied
#   warranty.  In no event will the author be held liable for any damages
#   arising from the use of this software.
#
#   Permission is granted to anyone to use this software for any purpose,
#   including commercial applications, and to alter it and redistribute it
#   freely, subject to the following restrictions:
#
#   1. The origin of this software must not be misrepresented; you must not
#      claim that you wrote the original software. If you use this software
#      in a product, an acknowledgment in the product documentation would be
#      appreciated but is not required.
#   2. Altered source versions must be plainly marked as such, and must not be
#      misrepresented as being the original software.
#   3. This notice may not be removed or altered from any source distribution.
#
#   Mark Adler    madler@alumni.caltech.edu
#
# (NOTICE, LICENSES/blast-zlib.txt)

def _huffman(rep):
    """Expand blast's run-length table (count<<4 | length) into canonical
    count-per-length and symbols-by-code order."""
    lengths = []
    for b in rep:
        lengths += [b & 15] * ((b >> 4) + 1)
    count = [0] * 14
    for n in lengths:
        count[n] += 1
    offs = [0, 0]
    for n in range(1, 13):
        offs.append(offs[n] + count[n])
    symbol = [0] * len(lengths)
    for s, n in enumerate(lengths):
        if n:
            symbol[offs[n]] = s
            offs[n] += 1
    return count, symbol


_LIT = _huffman(bytes([
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173]))
_LEN = _huffman(bytes([2, 35, 36, 53, 38, 23]))
_DIST = _huffman(bytes([2, 20, 53, 230, 247, 151, 248]))
_BASE = (3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264)
_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8)


def explode(src):
    """Decompress one PKWARE DCL stream. Raises ValueError on bad input."""
    # ponytail: bit-at-a-time Python, ~1-2 MB/s; a table decoder if a disc
    # ever makes this the slow step.
    pos = 0
    bitbuf = 0
    bitcnt = 0
    n = len(src)

    def bits(need):
        nonlocal pos, bitbuf, bitcnt
        while bitcnt < need:
            if pos >= n:
                raise ValueError('DCL stream truncated')
            bitbuf |= src[pos] << bitcnt
            pos += 1
            bitcnt += 8
        v = bitbuf & ((1 << need) - 1)
        bitbuf >>= need
        bitcnt -= need
        return v

    def decode(h):
        count, symbol = h
        code = first = index = 0
        for ln in range(1, 14):
            code |= bits(1) ^ 1          # DCL stores its codes inverted
            c = count[ln]
            if code < first + c:
                return symbol[index + code - first]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise ValueError('bad DCL code')

    lit = bits(8)
    dict_bits = bits(8)
    if lit > 1 or not 4 <= dict_bits <= 6:
        raise ValueError(f'not a DCL stream (lit={lit}, dict={dict_bits})')
    out = bytearray()
    while True:
        if bits(1):
            sym = decode(_LEN)
            length = _BASE[sym] + bits(_EXTRA[sym])
            if length == 519:
                return bytes(out)
            shift = 2 if length == 2 else dict_bits
            dist = (decode(_DIST) << shift) + bits(shift) + 1
            if dist > len(out):
                raise ValueError('DCL distance too far back')
            start = len(out) - dist
            for i in range(length):         # may overlap its own output
                out.append(out[start + i])
        else:
            out.append(decode(_LIT) if lit else bits(8))


# --- archive ---

def volumes(path):
    """Every member of the set `path` belongs to, keyed by volume number.
    A single archive (NAME.Z, NAME.LIB) is volume 1 on its own."""
    m = re.match(r'(.*)\.(\d+)$', path)
    if not m:
        return {1: path}
    found = {}
    for p in glob.glob(glob.escape(m.group(1)) + '.*'):
        mm = re.match(r'.*\.(\d+)$', p)
        if mm:
            found[int(mm.group(1))] = p
    return found


def read_toc(data):
    if struct.unpack_from('<I', data, 0)[0] != SIGNATURE:
        raise ValueError('not an InstallShield 3 archive')
    nfiles = struct.unpack_from('<H', data, 0x0C)[0]
    toc = struct.unpack_from('<I', data, 0x29)[0]
    ndirs = struct.unpack_from('<H', data, 0x31)[0]
    dirs = []
    p = toc
    for _ in range(ndirs):
        count, size, nlen = struct.unpack_from('<HHH', data, p)
        dirs.append((data[p + 6:p + 6 + nlen].decode('latin-1'), count))
        p += size
    # The header's count covers the whole set; a volume's TOC lists only its
    # own files, and the directory counts say how many that is.
    files = []
    for _ in range(min(nfiles, sum(c for _, c in dirs))):
        last_vol, dir_idx, size, packed, offset, dostime = \
            struct.unpack_from('<BHIIII', data, p)
        esize = struct.unpack_from('<H', data, p + 0x17)[0]
        first_vol, nlen = data[p + 0x1C], data[p + 0x1D]
        name = data[p + 0x1E:p + 0x1E + nlen].decode('latin-1')
        path = '\\'.join(x for x in (dirs[dir_idx][0], name) if x)
        files.append(dict(path=path, size=size, packed=packed, offset=offset,
                          first=first_vol or 1, last=last_vol or 1,
                          dostime=dostime))
        p += esize
    return files


def packed_bytes(f, vols):
    """The compressed stream for `f`, joined across volumes if it spans."""
    need = f['packed']
    out = bytearray()
    for v in range(f['first'], f['last'] + 1):
        data = vols[v]
        start = f['offset'] if v == f['first'] else HEADER_SIZE
        end = struct.unpack_from('<I', data, 0x29)[0] if v < f['last'] else start + need - len(out)
        out += data[start:end]
    if len(out) != need:
        raise ValueError(f"{f['path']}: {len(out)} of {need} packed bytes")
    return bytes(out)


def main():
    if '--selftest' in sys.argv:
        return _selftest()
    ap = argparse.ArgumentParser(description='Extract InstallShield 3 archives.')
    ap.add_argument('archives', nargs='+')
    ap.add_argument('-o', '--output', default='extracted')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()

    seen = set()
    bad = 0
    for arc in args.archives:
        paths = volumes(arc)
        key = tuple(sorted(os.path.abspath(p) for p in paths.values()))
        if key in seen:
            continue
        seen.add(key)
        vols = {n: open(p, 'rb').read() for n, p in paths.items()}
        files = {}
        for data in vols.values():
            for f in read_toc(data):
                files[f['path']] = f
        for f in files.values():
            if args.list:
                print(f"{f['size']:10} {f['packed']:10}  vol {f['first']}-{f['last']}  {f['path']}")
                continue
            try:
                raw = explode(packed_bytes(f, vols))
                if len(raw) != f['size']:
                    raise ValueError(f"{len(raw)} bytes, expected {f['size']}")
            except (ValueError, KeyError) as e:
                print(f"FAIL {f['path']}: {e}", file=sys.stderr)
                bad += 1
                continue
            dst = os.path.join(args.output, *f['path'].split('\\'))
            os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
            with open(dst, 'wb') as fh:
                fh.write(raw)
            print(f"{f['size']:10}  {f['path']}")
    return 1 if bad else 0


def _selftest():
    # The example stream from blast.c's own documentation: "AIAIAIAIAIAIA".
    assert explode(bytes([0x00, 0x04, 0x82, 0x24, 0x25, 0x8f, 0x80, 0x7f])) == b'AIAIAIAIAIAIA'
    # A split file: its tail sits in volume 1 from its offset up to the TOC,
    # and the rest follows the next volume's header.
    v1 = bytearray(0x200)
    struct.pack_into('<I', v1, 0x29, 0x180)
    v1[0x150:0x180] = b'A' * 0x30
    v2 = bytearray(0x200)
    v2[HEADER_SIZE:HEADER_SIZE + 0x10] = b'B' * 0x10
    f = dict(path='X', packed=0x40, offset=0x150, first=1, last=2)
    assert packed_bytes(f, {1: bytes(v1), 2: bytes(v2)}) == b'A' * 0x30 + b'B' * 0x10
    print('is3extract selftest OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
