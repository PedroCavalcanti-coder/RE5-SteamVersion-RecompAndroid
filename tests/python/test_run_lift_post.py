"""Transformações que o run_lift.py aplica ao C do lift32_cpu (desempenho, Fase 4):
restrict na CPU, barreira nas cabeças de laço e chamadas diretas."""
import os
import sys
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))

import run_lift  # noqa: E402

FN = """void L_00401000(CPU *c)
{
L_00401000:
    c->eax = rd32(0x00500000u);
    flags_logicz(c, c->eax & c->eax, 4);
    if (c->zf) goto L_00401000;
L_0040100A:
    if (c->cf) goto L_00401010;
    push32(c, 0x00401010u); dispatch(c, 0x00402000u);
L_00401010:
    c->esp += 4; return;
}
void L_00402000(CPU *c)
{
    dispatch(c, 0x00403000u); return;
}
"""


class RunLiftPost(unittest.TestCase):
    def test_restrict_only_on_definitions(self):
        out = run_lift.SIG_RE.sub(r"void \1(CPU *RE5_RESTRICT c)", FN + "void L_00409000(CPU *c);\n")
        self.assertEqual(out.count("(CPU *RE5_RESTRICT c)"), 2)
        self.assertIn("void L_00409000(CPU *c);", out)      # declaração fica igual

    def test_loop_barrier_on_backward_targets_only(self):
        out = run_lift.loop_barriers(FN)
        # espera ocupada: L_00401000 é alvo de um goto para trás
        self.assertIn("L_00401000: RE5_LOOP_BARRIER();", out)
        # L_00401010 só recebe goto para frente: sem barreira
        self.assertIn("L_00401010:\n", out)
        self.assertEqual(out.count("RE5_LOOP_BARRIER"), 1)

    def test_loop_barrier_is_per_function(self):
        # um rótulo com o mesmo nome em outra função não conta como "já visto"
        two = ("void L_00401000(CPU *c)\n{\n    goto L_00401008;\nL_00401008:\n    return;\n}\n"
               "void L_00401008(CPU *c)\n{\nL_00401008:\n    return;\n}\n")
        self.assertNotIn("RE5_LOOP_BARRIER", run_lift.loop_barriers(two))

    def test_direct_call_regex(self):
        m = run_lift.DIRECT_RE.findall(FN)
        self.assertEqual(m, ["00402000", "00403000"])
        self.assertIsNone(run_lift.DIRECT_RE.search("dispatch(c, _ct);"))


if __name__ == "__main__":
    unittest.main()
