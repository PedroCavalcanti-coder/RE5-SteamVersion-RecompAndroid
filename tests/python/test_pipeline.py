"""Testes dos scripts do pipeline (rodam no CTest como `python_pipeline`).

Os que dependem do binário do jogo são pulados quando ele não está presente
(checkout sem `git lfs pull`), para o build funcionar em qualquer máquina.
"""
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCRIPTS = os.path.join(REPO, "scripts")
sys.path.insert(0, SCRIPTS)

import re5common as rc  # noqa: E402
import recon  # noqa: E402


def game_file(key):
    p = rc.repo_path(rc.load_config()["input"][key])
    return p if os.path.exists(p) and not rc.is_lfs_pointer(p) else None


class GenCpuHeader(unittest.TestCase):
    def test_every_guest_access_goes_through_the_base(self):
        src = os.path.join(rc.PCRECOMP, "runtime", "recomp32_cpu", "cpu.h")
        with tempfile.TemporaryDirectory() as d:
            dst = os.path.join(d, "cpu.h")
            subprocess.run([sys.executable, os.path.join(SCRIPTS, "gen_cpu_header.py"), src, dst], check=True)
            text = open(dst, encoding="utf-8").read()
        self.assertIn('#include "re5/cpu_prelude.h"', text)
        self.assertIn('#include "re5/cpu_epilogue.h"', text)
        self.assertEqual(text.count("RE5_G2H(a)"), 33)   # +1: rep_ptr (rep stos/movs em bloco)
        self.assertNotIn("(uintptr_t)a)", text)
        self.assertEqual(text.count("RE5_UA_U32 *"), 2)   # rd32 e wr32


class ReconGate(unittest.TestCase):
    def setUp(self):
        self.known = rc.load_known_builds()

    @unittest.skipUnless(game_file("original"), "re5dx9.exe ausente")
    def test_original_has_steamstub_and_enigma(self):
        r = recon.analyze(game_file("original"), self.known)
        self.assertEqual(r["known_build"], "steam-2023-02-13-original")
        self.assertEqual(r["layers"], ["steamstub", "enigma"])
        self.assertFalse(r["liftable"])
        self.assertFalse(r["relocations"])
        self.assertEqual(r["image_base"], 0x00400000)

    @unittest.skipUnless(game_file("steamless"), "re5dx9.exe.unpacked.exe ausente")
    def test_steamless_output_still_has_enigma(self):
        r = recon.analyze(game_file("steamless"), self.known)
        self.assertEqual(r["known_build"], "steam-2023-02-13-steamless")
        self.assertEqual(r["layers"], ["enigma"])
        self.assertFalse(r["liftable"])

    @unittest.skipUnless(game_file("steamless"), "re5dx9.exe.unpacked.exe ausente")
    def test_lifter_refuses_protected_input(self):
        with tempfile.TemporaryDirectory() as d:
            p = subprocess.run([sys.executable, os.path.join(SCRIPTS, "run_lift.py"), "catalog",
                                "--exe", game_file("steamless"), "--out", os.path.join(d, "c.json")],
                               capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("enigma", p.stderr + p.stdout)

    @unittest.skipUnless(os.environ.get("RE5_TEST_BUILD_DIR") and
                         os.path.exists(os.path.join(os.environ.get("RE5_TEST_BUILD_DIR", ""), "mini_pe", "fx.exe")),
                         "PE sintético não foi construído")
    def test_synthetic_pe_is_liftable(self):
        exe = os.path.join(os.environ["RE5_TEST_BUILD_DIR"], "mini_pe", "fx.exe")
        r = recon.analyze(exe, self.known)
        self.assertTrue(r["liftable"], r["evidence"])


if __name__ == "__main__":
    unittest.main()
