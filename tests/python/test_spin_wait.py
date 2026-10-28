"""Fase P7: laço de espera pura ganha RE5_SPIN_BARRIER; laço que avança ponteiro não."""
import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import run_lift  # noqa: E402


def fn(body):
    return "void L_00001000(CPU *c)\n{\n" + body + "}\n"


class SpinWait(unittest.TestCase):
    def test_wait_on_flag(self):
        t = fn("L_00001005:\n    R_eax = rd32(0x00500000u);\n    flags_logicz(c, R_eax, R_eax, 4);\n"
               "    if (R_zf) goto L_00001005;\n    return;\n")
        out = run_lift.loop_barriers(t, True)
        self.assertIn("L_00001005: RE5_SPIN_BARRIER();", out)

    def test_wait_on_flag_via_register_pointer(self):
        t = fn("L_00001005:\n    R_eax = rd32((R_esi + 8u));\n    flags_logicz(c, R_eax, R_eax, 4);\n"
               "    if (R_zf) goto L_00001005;\n    return;\n")
        self.assertIn("RE5_SPIN_BARRIER", run_lift.loop_barriers(t, True))

    def test_strlen_style_loop_is_not_a_wait(self):
        t = fn("L_00001005:\n    R_eax = rd8(R_esi);\n    R_esi = R_esi + 1u;\n"
               "    flags_logicz(c, R_eax, R_eax, 1);\n    if (!R_zf) goto L_00001005;\n    return;\n")
        out = run_lift.loop_barriers(t, True)
        self.assertIn("L_00001005: RE5_LOOP_BARRIER();", out)
        self.assertNotIn("SPIN", out)

    def test_loop_with_store_or_call_is_not_a_wait(self):
        t = fn("L_00001005:\n    R_eax = rd32(0x00500000u);\n    wr32(0x00500004u, R_eax);\n"
               "    if (R_zf) goto L_00001005;\n    return;\n")
        self.assertNotIn("SPIN", run_lift.loop_barriers(t, True))
        t = fn("L_00001005:\n    R_eax = rd32(0x00500000u);\n    RE5_CALL(c, L_00002000);\n"
               "    if (R_zf) goto L_00001005;\n    return;\n")
        self.assertNotIn("SPIN", run_lift.loop_barriers(t, True))

    def test_disabled(self):
        t = fn("L_00001005:\n    R_eax = rd32(0x00500000u);\n    if (R_zf) goto L_00001005;\n    return;\n")
        self.assertNotIn("SPIN", run_lift.loop_barriers(t, False))


if __name__ == "__main__":
    unittest.main()
