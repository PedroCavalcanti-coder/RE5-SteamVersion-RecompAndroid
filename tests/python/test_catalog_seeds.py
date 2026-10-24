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


def loop_image():
    text = bytearray(b"\x90" * 0x100)
    # F: o laço do 0x00447080 do RE5 em miniatura, cortado na condição do laço.
    text[0x00:0x01] = b"\x55"                    # 0x401000 push ebp
    text[0x01:0x03] = b"\x31\xc0"                # 0x401001 xor eax, eax
    text[0x03:0x04] = b"\x40"                    # 0x401003 inc eax          <- cabeça do laço
    text[0x04:0x06] = b"\xeb\x02"                # 0x401004 jmp 0x401008
    text[0x08:0x0B] = b"\x83\xf8\x05"            # 0x401008 cmp eax, 5       <- início falso
    text[0x0B:0x0D] = b"\x75\xf6"                # 0x40100B jne 0x401003     (volta para o meio de F)
    text[0x0D:0x0F] = b"\x5d\xc3"                # 0x40100D pop ebp; ret
    text[0x10:0x11] = b"\xc3"                    # 0x401010 G
    text[0x20:0x25] = b"\xe9\x0b\x00\x00\x00"    # 0x401020 jmp 0x401030     (tail call de verdade)
    text[0x30:0x31] = b"\xc3"                    # 0x401030 H
    return Disassembler(b"\x00" * 0x400 + bytes(text), BASE, [_Sec(".text", 0x1000, 0x400, 0x100, True)])


class FixLabels(unittest.TestCase):
    def test_loop_tail_becomes_alias_and_tail_call_stays(self):
        def e(a, end):
            return {"address": BASE + a, "end": BASE + end, "size": end - a, "entry_kind": "start", "is_thunk": False}
        funcs = [e(0x1000, 0x1008), e(0x1008, 0x100F), e(0x1010, 0x1011), e(0x1020, 0x1025), e(0x1030, 0x1031)]
        fixed = run_lift.fix_label_starts(loop_image(), {}, funcs, BASE + 0x1100)
        self.assertEqual(fixed, [(BASE + 0x1000, BASE + 0x1008)])
        by = {f["address"]: f for f in funcs}
        self.assertEqual(by[BASE + 0x1008]["entry_kind"], "alias")
        self.assertEqual(by[BASE + 0x1000]["end"], BASE + 0x100F)   # o laço inteiro numa função só
        self.assertEqual(by[BASE + 0x1030]["entry_kind"], "start")  # alvo de jmp que não volta: função
