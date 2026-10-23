"""Testes do script de auditoria do catálogo contra o Ghidra (scripts/audit_catalog.py)."""
import json
import os
import sys
import tempfile
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCRIPTS = os.path.join(REPO, "scripts")
sys.path.insert(0, SCRIPTS)

import audit_catalog  # noqa: E402


class TestAuditCatalog(unittest.TestCase):
    def test_audit_catalog_synthetic(self):
        # 1. Catálogo sintético: 3 funções (2 starts, 1 alias)
        cat_data = {
            "functions": [
                {"address": 0x00401000, "end": 0x00401050, "entry_kind": "start"},
                {"address": 0x00401050, "end": 0x00401080, "entry_kind": "alias"},
                {"address": 0x00402000, "end": 0x00402030, "entry_kind": "start"},
            ]
        }

        # 2. Ghidra funcs sintético:
        # - 0x00401000: bate com o start do catálogo, mas vai até 0x00401080 (0x00401050 é um split no catálogo)
        # - 0x00403000: ausente do catálogo com chamadores (deve virar semente)
        # - 0x00404000: ausente do catálogo sem chamadores
        g_funcs = {
            "0x00401000": {
                "entry": "0x00401000",
                "callers": 2,
                "ranges": [["0x401000", "0x40107f"]],  # inclusive -> exclusive é 0x401080
            },
            "0x00403000": {
                "entry": "0x00403000",
                "callers": 1,
                "ranges": [["0x403000", "0x403020"]],
            },
            "0x00404000": {
                "entry": "0x00404000",
                "callers": 0,
                "ranges": [["0x404000", "0x404020"]],
            },
        }

        # 3. Switches sintéticos:
        # - 0x00401020: dentro de 0x00401000..0x00401050, com alvos:
        #     * 0x00401030 (coberto pelo próprio corpo)
        #     * 0x00401060 (não coberto pelo corpo, mas coberto pelo alias 0x00401050)
        #     * 0x00405000 (fora de tudo -> deve virar semente)
        g_switches = {
            "0x00401020": {
                "addr": "0x00401020",
                "func": "0x00401000",
                "targets": ["0x00401030", "0x00401050", "0x00405000"],
            }
        }

        # 4. RTTI sintético:
        g_rtti = {
            "methods": {
                "0x00402000": "MyClass",  # presente no catálogo
                "0x00406000": "MissingClass",  # ausente do catálogo -> vira semente
            }
        }

        # 5. PE bytes sintéticos para padding trimming
        # 0x00402000: catálogo vai até 0x00402030. Se Ghidra fosse até 0x00402034 com 0xCC:
        pe_dummy = bytearray(0x10000)
        # Offset 0x00402030 - 0x00400000 = 0x2030
        pe_dummy[0x2030:0x2034] = b"\xCC\xCC\xCC\xCC"

        res = audit_catalog.audit_catalog(
            catalog_data=cat_data,
            funcs_data=g_funcs,
            switches_data=g_switches,
            code_ptrs_data=[],
            rtti_data=g_rtti,
            pe_bytes=pe_dummy,
            image_base=0x00400000,
        )

        # Validações dos inícios
        self.assertEqual(res["catalog"]["starts"], 2)
        self.assertEqual(res["catalog"]["aliases"], 1)
        self.assertEqual(res["starts"]["true_positives"], 1)  # 0x00401000
        self.assertEqual(res["starts"]["missing_with_callers"], 1)  # 0x00403000
        self.assertIn("0x00403000", res["starts"]["missing_with_callers_list"])
        self.assertEqual(res["starts"]["missing_unreferenced"], 1)  # 0x00404000

        # Validações de fins
        self.assertEqual(res["ends"]["short_real"], 1)  # 0x00401000 vai até 0x401050 mas ghidra vai até 0x401080
        self.assertEqual(res["ends"]["short_split_continuation"], 1)  # 0x00401050 está no catálogo

        # Validações de switches
        self.assertEqual(res["switches"]["total_switches"], 1)
        self.assertEqual(res["switches"]["missing_arms_count"], 1)
        self.assertEqual(res["switches"]["missing_arms"][0]["target_addr"], "0x00405000")

        # Validações de RTTI
        self.assertEqual(res["indirect"]["rtti_missing"], 1)
        self.assertEqual(res["indirect"]["rtti_missing_list"][0]["address"], "0x00406000")

        # Validações de sementes recomendadas:
        # Devem incluir: 0x00403000 (func com caller), 0x00405000 (switch arm), 0x00406000 (rtti method)
        seeds = res["seeds"]["list"]
        self.assertIn("0x00403000", seeds)
        self.assertIn("0x00405000", seeds)
        self.assertIn("0x00406000", seeds)

    def test_write_seeds_and_report(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            seeds_path = os.path.join(tmpdir, "seeds.txt")
            report_path = os.path.join(tmpdir, "report.json")

            sample_seeds = ["0x00401000", "0x00402000"]
            sample_report = {"test": 123, "seeds": sample_seeds}

            audit_catalog.write_seeds(sample_seeds, seeds_path)
            audit_catalog.write_report(sample_report, report_path)

            self.assertTrue(os.path.exists(seeds_path))
            self.assertTrue(os.path.exists(report_path))

            with open(seeds_path, "r", encoding="utf-8") as f:
                content = f.read()
                self.assertIn("0x00401000", content)
                self.assertIn("0x00402000", content)

            with open(report_path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
                self.assertEqual(loaded["test"], 123)


if __name__ == "__main__":
    unittest.main()
