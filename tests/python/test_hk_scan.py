"""scripts/hk_scan.py: leitura do despejo da memória do guest (trechos esparsos) e busca de vtables."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
import hk_scan  # noqa: E402


class DumpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = os.path.join(self.tmp.name, "guest_1")
        a = bytearray(8192)
        struct.pack_into("<I", a, 16, 0x0105443C)       # vtable "rígida" em 0x10000010
        struct.pack_into("<f", a, 4096 + 8, 1.5)
        b = bytearray(4096)
        struct.pack_into("<I", b, 0, 0x0105443C)        # outra, em 0x20000000
        with open(base + ".bin", "wb") as f:
            f.write(bytes(a) + bytes(b))
        with open(base + ".map", "w") as f:
            f.write("10000000 2000 0\n20000000 1000 2000\n")
        self.d = hk_scan.Dump(base)

    def tearDown(self):
        self.d.mm.close()
        self.tmp.cleanup()

    def test_read_across_runs_and_holes(self):
        self.assertEqual(self.d.u32(0x10000010), 0x0105443C)
        self.assertEqual(self.d.f32(0x10001008), 1.5)
        # de 0x10001FF0 (dentro do 1º trecho) até 0x20000000 há um buraco de zeros
        blob = self.d.read(0x10001FF0, 0x10000000 + 0x10)
        self.assertEqual(len(blob), 0x10000010)
        self.assertEqual(blob[:16], bytes(16))
        self.assertEqual(struct.unpack_from("<I", blob, 0x0FFFE010)[0], 0x0105443C)
        self.assertFalse(self.d.mapped(0x18000000))
        self.assertEqual(self.d.u32(0x18000000), 0)

    def test_find_u32_aligned(self):
        self.assertEqual(sorted(self.d.find_u32(0x0105443C)), [0x10000010, 0x20000000])


if __name__ == "__main__":
    unittest.main()
