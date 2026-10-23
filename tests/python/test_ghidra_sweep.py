"""Testes do driver de varredura Ghidra (scripts/ghidra_sweep.py)."""
import os
import sys
import tempfile
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCRIPTS = os.path.join(REPO, "scripts")
sys.path.insert(0, SCRIPTS)

import ghidra_sweep  # noqa: E402
import re5common as rc  # noqa: E402


class TestGhidraSweep(unittest.TestCase):
    def test_build_headless_args_import(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_ghidra = os.path.join(tmpdir, "ghidra")
            os.makedirs(os.path.join(fake_ghidra, "support"), exist_ok=True)
            headless_name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
            headless_bin = os.path.join(fake_ghidra, "support", headless_name)
            with open(headless_bin, "w") as f:
                f.write("#!/bin/sh\n")

            proj_dir = os.path.join(tmpdir, "proj")
            os.makedirs(proj_dir, exist_ok=True)
            fake_input = os.path.join(tmpdir, "test.exe")
            with open(fake_input, "wb") as f:
                f.write(b"MZ\x00\x00")

            cmd = ghidra_sweep.build_headless_args(
                ghidra_dir=fake_ghidra,
                project_dir=proj_dir,
                project_name="test_proj",
                input_file=fake_input,
                pre_script="ConfigureAnalysis.java",
                post_scripts=["GhidraStats.java"],
            )

            self.assertEqual(cmd[0], headless_bin)
            self.assertEqual(cmd[1], proj_dir)
            self.assertEqual(cmd[2], "test_proj")
            self.assertIn("-import", cmd)
            self.assertIn(fake_input, cmd)
            self.assertIn("-preScript", cmd)
            self.assertIn("ConfigureAnalysis.java", cmd)
            self.assertIn("-postScript", cmd)
            self.assertIn("GhidraStats.java", cmd)

    def test_build_headless_args_process_existing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_ghidra = os.path.join(tmpdir, "ghidra")
            os.makedirs(os.path.join(fake_ghidra, "support"), exist_ok=True)
            headless_name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
            headless_bin = os.path.join(fake_ghidra, "support", headless_name)
            with open(headless_bin, "w") as f:
                f.write("#!/bin/sh\n")

            proj_dir = os.path.join(tmpdir, "proj")
            os.makedirs(proj_dir, exist_ok=True)
            gpr_file = os.path.join(proj_dir, "test_proj.gpr")
            with open(gpr_file, "w") as f:
                f.write("<project/>")

            fake_input = os.path.join(tmpdir, "test.exe")
            with open(fake_input, "wb") as f:
                f.write(b"MZ\x00\x00")

            cmd = ghidra_sweep.build_headless_args(
                ghidra_dir=fake_ghidra,
                project_dir=proj_dir,
                project_name="test_proj",
                input_file=fake_input,
                no_analysis=True,
                post_scripts=["GhidraStats.java"],
            )

            self.assertIn("-process", cmd)
            self.assertIn("test.exe", cmd)
            self.assertIn("-noanalysis", cmd)
            self.assertNotIn("-import", cmd)

    def test_validate_input_unknown_fails_without_force(self):
        with tempfile.NamedTemporaryFile(suffix=".exe", delete=False) as f:
            f.write(b"random_dummy_content")
            tmp_path = f.name
        try:
            with self.assertRaises(ValueError):
                ghidra_sweep.validate_input(tmp_path, force=False)
            path, sha, match = ghidra_sweep.validate_input(tmp_path, force=True)
            self.assertIsNone(match)
            self.assertTrue(len(sha) == 64)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)


if __name__ == "__main__":
    unittest.main()
