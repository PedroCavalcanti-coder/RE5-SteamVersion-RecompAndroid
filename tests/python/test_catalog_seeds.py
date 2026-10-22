"""Etapa 2.6: `run_lift.py catalog --add-seeds` (extend_catalog e as sementes).

Imagem sintética: uma função cortada num início falso (o caso do 0x00EB49D6 do
RE5) e um corpo semeado que salta para um endereço sem entrada.
"""
import os
import sys
import tempfile
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))

import run_lift  # noqa: E402  (põe tools/pcrecomp/tools/disasm no path)
from disasm32 import Disassembler  # noqa: E402

BASE = 0x400000


class _Sec:
    def __init__(self, name, va, raw_off, raw_size, is_code):
        self.name, self.virtual_address = name, va
        self.raw_offset, self.raw_size = raw_off, raw_size
        self.virtual_size, self.is_code = raw_size, is_code


def image():
    text = bytearray(b"\x90" * 0x100)
    text[0x00:0x04] = b"\x55\x8b\xec\x90"        # A: push ebp; mov ebp, esp; nop
    text[0x04:0x06] = b"\x74\x1a"                # 0x401004: je 0x401020
    text[0x06:0x07] = b"\xc3"                    #           ret
    text[0x10:0x11] = b"\xc3"                    # B
    text[0x20:0x21] = b"\xc3"                    # alvo do je, sem entrada no catálogo
    return Disassembler(b"\x00" * 0x400 + bytes(text), BASE, [_Sec(".text", 0x1000, 0x400, 0x100, True)])


class ExtendCatalog(unittest.TestCase):
    def test_seed_becomes_alias_and_closes_its_targets(self):
        funcs = [
            {"address": BASE + 0x1000, "end": BASE + 0x1004, "size": 4, "entry_kind": "start", "is_thunk": False},
            {"address": BASE + 0x1010, "end": BASE + 0x1011, "size": 1, "entry_kind": "start", "is_thunk": False},
        ]
        added = run_lift.extend_catalog(image(), {}, funcs, {BASE + 0x1004}, BASE + 0x1000, BASE + 0x1100)
        self.assertEqual(added, [BASE + 0x1004, BASE + 0x1020])
        by = {f["address"]: f for f in funcs}
        self.assertEqual(by[BASE + 0x1004]["entry_kind"], "alias")
        # O fim da descida inclui o bloco do alvo do je (0x401020); o corte na
        # próxima entrada `start` (0x401010) é o que vale, como no catálogo.
        self.assertEqual(by[BASE + 0x1004]["size"], 0x0C)
        self.assertEqual(by[BASE + 0x1020]["size"], 1)
        self.assertEqual(by[BASE + 0x1000]["size"], 4)          # o que existia não muda
        self.assertEqual([f["address"] for f in funcs], sorted(by))

    def test_known_and_out_of_range_seeds_are_ignored(self):
        funcs = [{"address": BASE + 0x1010, "end": BASE + 0x1011, "size": 1, "entry_kind": "start",
                  "is_thunk": False}]
        added = run_lift.extend_catalog(image(), {}, funcs, {BASE + 0x1010, 0x12345678}, BASE + 0x1000, BASE + 0x1100)
        self.assertEqual(added, [])
        self.assertEqual(len(funcs), 1)

    def test_seed_files(self):
        with tempfile.TemporaryDirectory() as d:
            txt, js = os.path.join(d, "s.txt"), os.path.join(d, "s.json")
            with open(txt, "w", encoding="utf-8") as f:
                f.write("0x00EB49D6\n# comentário\n0x00EE2350  visto 2x\n0x00EE2350\n")
            with open(js, "w", encoding="utf-8") as f:
                f.write('[4198400, "0x401010", {"address": "0x401020"}]')
            self.assertEqual(run_lift.read_seed_files(f"{txt},{js}"),
                             {0x00EB49D6, 0x00EE2350, 0x401000, 0x401010, 0x401020})


if __name__ == "__main__":
    unittest.main()
