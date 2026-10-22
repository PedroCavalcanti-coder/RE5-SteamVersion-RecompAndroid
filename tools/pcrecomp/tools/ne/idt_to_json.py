#!/usr/bin/env python3
"""idt_to_json.py - build work/win16_imports.json from IDA's shipped .idt files.

win16.py needs an ordinal -> API name map, or every Win16 import resolves to
`MODULE_OrdN` and every purge lookup misses. The usual way to get one is to run
IDA over the binary and export it. You don't have to: IDA ships the maps as
plain text inside `ids/win.zip` (`kernel.idt`, `user.idt`, `gdi.idt`, ...), so
the names can be lifted straight out of the install with no IDA run at all.

    python idt_to_json.py --ida "C:/Program Files/IDA Professional 9.1" \
                          -o work/win16_imports.json

Merges into an existing file rather than replacing it: entries already present
win, so a name a project corrected by hand is never clobbered by this.

Names only. This does NOT give you stack purges -- .idt records a name per
ordinal and almost never an argument size. Purges live in win16.py's PURGE
table, and the honest way to add one is to read it off a real call site.
"""

import argparse
import json
import os
import re
import sys
import zipfile

# "12 Name=SETWINDOWEXT" / "1 Name=FATALEXIT flags=1"
_ENTRY = re.compile(r'^\s*(\d+)\s+Name=([A-Za-z_@$?][\w@$?]*)')

# The 16-bit modules worth having by default. Others can be named explicitly.
DEFAULT_MODULES = [
    'kernel', 'user', 'gdi', 'keyboard', 'sound', 'shell', 'mmsystem',
    'commdlg', 'toolhelp', 'win87em', 'ddeml', 'lzexpand', 'ver', 'olecli',
    'olesvr', 'storage', 'compobj', 'ole2', 'ctl3d', 'ctl3dv2',
]


def parse_idt(text):
    """Return {ordinal_str: NAME} for one .idt file, skipping the ordinal-0
    module description line (that is metadata, not an export)."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(';'):
            continue
        m = _ENTRY.match(line)
        if not m:
            continue
        ordinal, name = m.group(1), m.group(2)
        if ordinal == '0':          # "0 Name=GDI.dll Pascal=0"
            continue
        out[ordinal] = name.upper()
    return out


def load_from_zip(zip_path, modules):
    """Pull the named modules out of ids/win.zip. Returns {MODULE: {ord: name}}."""
    found = {}
    with zipfile.ZipFile(zip_path) as z:
        by_stem = {}
        for info in z.namelist():
            if info.lower().endswith('.idt'):
                by_stem.setdefault(os.path.splitext(os.path.basename(info))[0].lower(), info)
        for mod in modules:
            member = by_stem.get(mod.lower())
            if member is None:
                continue
            entries = parse_idt(z.read(member).decode('latin-1'))
            if entries:
                found[mod.upper()] = entries
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ida', help='IDA install directory (uses its ids/win.zip)')
    ap.add_argument('--zip', help='path to an ids/win.zip directly')
    ap.add_argument('-o', '--output', default='work/win16_imports.json')
    ap.add_argument('-m', '--modules', nargs='*', default=DEFAULT_MODULES,
                    help='module names to extract (default: the common Win16 set)')
    args = ap.parse_args()

    zip_path = args.zip
    if not zip_path and args.ida:
        zip_path = os.path.join(args.ida, 'ids', 'win.zip')
    if not zip_path or not os.path.isfile(zip_path):
        ap.error('need --ida <install dir> or --zip <ids/win.zip>; '
                 f'no such file: {zip_path!r}')

    found = load_from_zip(zip_path, args.modules)
    if not found:
        print(f'No .idt modules matched in {zip_path}', file=sys.stderr)
        return 1

    # Merge, existing entries winning: a hand-corrected name outranks IDA's.
    merged, kept = {}, 0
    if os.path.isfile(args.output):
        with open(args.output) as f:
            for mod, ents in json.load(f).items():
                merged[mod.upper()] = dict(ents)
    for mod, ents in found.items():
        target = merged.setdefault(mod, {})
        for ordinal, name in ents.items():
            if ordinal in target:
                kept += 1
            else:
                target[ordinal] = name

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'w') as f:
        json.dump(merged, f, indent=1, sort_keys=True)

    total = sum(len(v) for v in merged.values())
    print(f'{args.output}: {len(merged)} modules, {total} ordinals '
          f'({kept} existing entries kept)')
    for mod in sorted(found):
        print(f'  {mod:<10s} +{len(found[mod])}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
