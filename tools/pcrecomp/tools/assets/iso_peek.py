#!/usr/bin/env python3
"""
iso_peek.py -- read an ISO9660 directory without downloading the ISO.

A disc image is mostly data you do not care about. Deciding whether a 234 MB
image is worth fetching usually needs three things: does it hold a 32-bit
build, what is it called, and how big is it. All three live in the directory
tree, which sits in the first few megabytes, and HTTP range requests can read
just that.

    # what is on this disc?
    python tools/assets/iso_peek.py https://archive.org/download/x/DISC.ISO

    # only the executables
    python tools/assets/iso_peek.py DISC.ISO --find "*.EXE"

    # pull one file out, wherever it is on the disc
    python tools/assets/iso_peek.py https://.../DISC.ISO --extract TMTWIN32.EXE

Finding Treasure Mountain's 164 KB executable this way cost 3 MB instead of
234 MB, and proved the disc was worth fetching before fetching it.

Works the same on a local path, so it doubles as a quick "what is in here"
that does not unpack anything.
"""

import argparse
import fnmatch
import os
import struct
import sys
import urllib.request

SECTOR = 2048


class Reader:
    """Bytes at an offset, from a local file or over HTTP."""

    def __init__(self, where):
        self.where = where
        self.http = where.startswith(('http://', 'https://'))
        self.fetched = 0
        if not self.http:
            self.fh = open(where, 'rb')

    def read(self, off, size):
        self.fetched += size
        if not self.http:
            self.fh.seek(off)
            return self.fh.read(size)
        req = urllib.request.Request(
            self.where, headers={'Range': 'bytes=%d-%d' % (off, off + size - 1)})
        with urllib.request.urlopen(req, timeout=120) as r:
            # A server that ignores Range answers 200 with the whole file; read
            # only what was asked for rather than pulling the disc into memory.
            return r.read(size)


def _entries(block, base_off, joliet=False):
    """Walk one directory extent. Yields (name, offset, size, is_dir)."""
    p = 0
    while p < len(block):
        length = block[p]
        if length == 0:
            # Directory records never straddle a sector; a zero length means
            # the rest of this sector is padding.
            nxt = (p // SECTOR + 1) * SECTOR
            if nxt <= p:
                return
            p = nxt
            continue
        if p + 33 > len(block):
            return
        lba = struct.unpack_from('<I', block, p + 2)[0]
        size = struct.unpack_from('<I', block, p + 10)[0]
        flags = block[p + 25]
        nlen = block[p + 32]
        raw = block[p + 33:p + 33 + nlen]
        # '\x00' and '\x01' are "." and ".." (one byte in Joliet too); ";1" is
        # the ISO version suffix.
        name = raw.decode('utf-16-be', 'replace') if joliet and nlen > 1 else raw.decode('latin1', 'replace')
        if name not in ('\x00', '\x01'):
            yield name.split(';')[0], lba * SECTOR, size, bool(flags & 2)
        p += length


def walk(reader, verbose=False):
    """Every file on the disc, as (path, offset, size), directories first."""
    pvd = reader.read(16 * SECTOR, SECTOR)
    if pvd[1:6] != b'CD001':
        raise SystemExit("not an ISO9660 image (no CD001 at sector 16) -- if it "
                         "is a raw 2352-byte BIN, run tools/assets/bin2iso.js first")
    # A Joliet supplementary descriptor (type 2, UCS-2 escape %/@ %/C %/E)
    # carries the long, mixed-case names; the primary tree has 8.3 ones
    # (SELECT~1.BMP for selectCar.bmp). Prefer it when the disc has one.
    vd, joliet = pvd, False
    for n in range(17, 32):
        d = reader.read(n * SECTOR, SECTOR)
        if d[1:6] != b'CD001' or d[0] == 255:
            break
        if d[0] == 2 and d[88:91] in (b'%/@', b'%/C', b'%/E'):
            vd, joliet = d, True
            break
    root_off = struct.unpack_from('<I', vd, 156 + 2)[0] * SECTOR
    root_size = struct.unpack_from('<I', vd, 156 + 10)[0]

    out = []
    pending = [('', root_off, root_size)]
    while pending:
        prefix, off, size = pending.pop(0)
        block = reader.read(off, size)
        for name, eoff, esize, is_dir in _entries(block, off, joliet):
            path = prefix + '/' + name if prefix else name
            if is_dir:
                pending.append((path, eoff, esize))
            else:
                out.append((path, eoff, esize))
        if verbose:
            print('  ... %s (%d entries so far)' % (prefix or '/', len(out)),
                  file=sys.stderr)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='path or http(s) URL of an ISO9660 image')
    ap.add_argument('--find', metavar='GLOB', help='only names matching this glob')
    ap.add_argument('--extract', metavar='NAME',
                    help='fetch one file (matched by glob against the full path)')
    ap.add_argument('--out', help='where --extract writes (default: its own name)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    reader = Reader(args.image)
    files = walk(reader, args.verbose)

    if args.extract:
        want = args.extract.upper()
        # An exact path wins outright. Otherwise match a glob, or a bare
        # filename anywhere on the disc -- but only report that ambiguous if
        # the exact form did not already settle it. A demo directory carrying
        # a cut-down copy of the same name is the normal case, not a puzzle.
        exact = [f for f in files if f[0].upper() == want]
        hits = exact or [f for f in files
                         if fnmatch.fnmatch(f[0].upper(), want)
                         or f[0].upper().endswith('/' + want)]
        if not hits:
            raise SystemExit("no such file on the disc: %s" % args.extract)
        if len(hits) > 1:
            raise SystemExit("ambiguous -- name one of these exactly:\n  %s"
                             % '\n  '.join(h[0] for h in hits[:8]))
        path, off, size = hits[0]
        dest = args.out or os.path.basename(path)
        with open(dest, 'wb') as f:
            f.write(reader.read(off, size))
        print("%s -> %s (%d bytes, %.1f MB read)"
              % (path, dest, size, reader.fetched / 1048576.0))
        return

    shown = 0
    for path, off, size in sorted(files):
        if args.find and not fnmatch.fnmatch(path.upper(), args.find.upper()):
            continue
        print("  %12d  %s" % (size, path))
        shown += 1
    print("\n%d files%s, %.1f MB read to find out"
          % (shown, '' if not args.find else ' of %d' % len(files),
             reader.fetched / 1048576.0))


if __name__ == '__main__':
    main()
