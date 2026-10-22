"""
patch_borland_eh.py - Guard Borland's cleanup-chain walker in a lifted tree.

Borland C++ for Windows links a walker for the exception-cleanup chain into
every module that uses C++ exceptions. The chain lives on the task's stack, and
a setjmp / catch buffer on it is marked with the cookie 0xFACE. In a static
recompile a window procedure runs re-entrantly on the same guest stack, so a
node can be left pointing at a discarded frame or at another module's catch
buffer; following it (or a null link) turns the walk into a runaway loop and
the game hangs in its first exception. The guard stops the walk at a null link,
the cookie, or a node whose first word is the cookie, by jumping to the
walker's own exit.

The routine is found by its bytes, not by address: it is the same RTL code in
Catz (CATZDLL seg 49 and CATZ.WAD seg 5, both at 0x141A), Dogz (DOGZDLL seg 22,
DOGZ.WAD seg 5) and Oddballz (ODDBALLZ.WAD seg 59, at 0x147A).

    python patch_borland_eh.py --work work MODULE...     (same order as ne_link.py)

Run after ne_link.py. Idempotent. Upstreamed from catz/tools/patch_lifted.py,
which patched the two Catz addresses by hand.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ne_parse import parse_ne
import gen_image

# `cmp word [bp-2], -1 / je +8 / mov ax,[bp-2] / cmp ax,[bp+0Ch] / jne ...`:
# the loop head of the walker. The function starts here; its exit is +0x0E.
SIGNATURE = bytes.fromhex('837efeff74088b46fe3b460c75a1837e')
EXIT = 0x0E

GUARD = '''    /* Borland EH cleanup-chain guard (tools/ne/patch_borland_eh.py): stop
     * the walk at a null link or a 0xFACE setjmp/catch cookie instead of
     * following it into a discarded frame. */
    {{ uint16_t _nd = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2));
      if (_nd == 0x0000 || _nd == 0xFACE ||
          mem_read16(cpu, cpu->ss, _nd) == 0xFACE) {{ {exit}(cpu); return; }} }}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('modules', nargs='+')
    ap.add_argument('--work', default='work')
    args = ap.parse_args()

    found = 0
    for ne in gen_image.renumber_all([parse_ne(p) for p in args.modules]):
        for s in ne.segments:
            if not s.is_code or not s.data:
                continue
            off = bytes(s.data).find(SIGNATURE)
            if off < 0:
                continue
            found += 1
            fn = f'seg{s.index:03d}_{off:04X}'
            path = os.path.join(args.work, 'src', f'seg{s.index:03d}.c')
            text = open(path, encoding='utf-8').read()
            anchor = f'    TRACE_FN("{fn}");\n'
            if anchor not in text:
                sys.exit(f'{fn}: not a lifted function start -- did the decoder split it differently?')
            if '0xFACE' in text.split(anchor, 1)[1][:800]:
                print(f'  {fn}: already guarded')
                continue
            guard = GUARD.format(exit=f'seg{s.index:03d}_{off + EXIT:04X}')
            text = text.replace(anchor, anchor + guard, 1)
            open(path, 'w', encoding='utf-8', newline='\n').write(text)
            print(f'  {fn}: EH cleanup-chain guard applied ({os.path.basename(ne.filename)})')
    if not found:
        print('  no Borland EH walker in these modules')


if __name__ == '__main__':
    main()
