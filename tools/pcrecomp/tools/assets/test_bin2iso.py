#!/usr/bin/env python3
"""
test_bin2iso.py - bin2iso.js against BIN files built here, so the check needs
no disc: a Mode 1 track and a Mode 2 Form 1 track, each with distinct data
per sector, must both come back as the same 2048-byte-per-sector ISO.

    python tools/assets/test_bin2iso.py

Needs node on PATH; prints SKIP and exits 0 without it.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def sector(mode, data):
    head = SYNC + b'\x00\x02\x00' + bytes([mode])
    if mode == 2:
        head += b'\x00\x00\x08\x00' * 2  # XA subheader, Form 1 data
    raw = head + data
    return raw + b'\xee' * (2352 - len(raw))  # EDC/ECC filler


def test():
    if not shutil.which('node'):
        print('bin2iso: SKIP (node not on PATH)')
        return
    data = [bytes([i]) * 2048 for i in range(1, 5)]
    with tempfile.TemporaryDirectory() as tmp:
        for mode in (1, 2):
            src = os.path.join(tmp, f'mode{mode}.bin')
            dst = os.path.join(tmp, f'mode{mode}.iso')
            with open(src, 'wb') as f:
                f.write(b''.join(sector(mode, d) for d in data))
            subprocess.run(['node', os.path.join(HERE, 'bin2iso.js'), src, dst],
                           check=True, capture_output=True)
            with open(dst, 'rb') as f:
                assert f.read() == b''.join(data), f'mode {mode} data mismatch'
    print('bin2iso: ok (Mode 1 and Mode 2 Form 1)')


if __name__ == '__main__':
    test()
