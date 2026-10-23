"""Lacunas do lift32_cpu fechadas na etapa 2.1 (tools/pcrecomp, vendorizado).

Cada instrução é traduzida isoladamente e o C gerado é conferido: nenhuma pode
virar RECOMP_TODO, e a semântica que importa (ordem do pushad, ESP pulado no
popad, quem responde cpuid/rdtsc) está no texto.
"""
import os
import sys
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "pcrecomp", "tools", "lift"))

import capstone  # noqa: E402
from lift32_cpu import Lifter  # noqa: E402


def lift(encoding):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    insn = next(iter(md.disasm(encoding, 0x1000)))
    lifter = Lifter.__new__(Lifter)
    lifter.md = md
    lifter.reloc_vas = set()                                  # deslocamentos: constantes, não endereços
    lifter.image_lo, lifter.image_hi = 0x00400000, 0x01400000
    return " ".join(lifter.translate(insn, set()))


class LifterGaps(unittest.TestCase):
    def test_carry_flag(self):
        self.assertEqual(lift(b"\xf9"), "c->cf = 1;")       # stc
        self.assertEqual(lift(b"\xf8"), "c->cf = 0;")       # clc
        self.assertEqual(lift(b"\xf5"), "c->cf ^= 1u;")     # cmc

    def test_machine_queries_go_to_the_runtime(self):
        self.assertEqual(lift(b"\x0f\xa2"), "recomp_cpuid(c);")
        self.assertEqual(lift(b"\x0f\x31"), "recomp_rdtsc(c);")

    def test_pushad_order_and_original_esp(self):
        c = lift(b"\x60")
        self.assertNotIn("RECOMP_TODO", c)
        order = [c.index(f"push32(c, {r})") for r in
                 ("c->eax", "c->ecx", "c->edx", "c->ebx", "_sp", "c->ebp", "c->esi", "c->edi")]
        self.assertEqual(order, sorted(order))
        self.assertLess(c.index("_sp = c->esp"), c.index("push32"))

    def test_popad_skips_esp(self):
        c = lift(b"\x61")
        self.assertNotIn("RECOMP_TODO", c)
        self.assertNotIn("c->esp = pop32", c)
        order = [c.index(f"{r} = pop32") for r in
                 ("c->edi", "c->esi", "c->ebp", "c->ebx", "c->edx", "c->ecx", "c->eax")]
        self.assertEqual(order, sorted(order))
        self.assertLess(c.index("c->ebp = pop32"), c.index("c->esp += 4"))
        self.assertLess(c.index("c->esp += 4"), c.index("c->ebx = pop32"))

    def test_reciprocals(self):
        # rcpss xmm3, xmm5 (0x00E64C4F, a parada depois do M7): só a pista 0 muda.
        self.assertEqual(lift(b"\xf3\x0f\x53\xdd"), "c->xmm[3].f32[0] = 1.0f / (c->xmm[5].f32[0]);")
        self.assertEqual(lift(b"\xf3\x0f\x52\xc1"), "c->xmm[0].f32[0] = 1.0f / sqrtf(c->xmm[1].f32[0]);")
        self.assertIn("rdss((c->eax))", lift(b"\xf3\x0f\x53\x00"))           # rcpss xmm0, [eax]
        # Empacotadas: a fonte é copiada antes (rcpps xmm1, xmm1 não pode ler pista já escrita).
        c = lift(b"\x0f\x53\xc9")
        self.assertIn("XMM _s = c->xmm[1];", c)
        self.assertIn("c->xmm[1].f32[_i] = 1.0f / (_s.f32[_i]);", c)
        self.assertIn("1.0f / sqrtf(_s.f32[_i])", lift(b"\x0f\x52\xca"))   # rsqrtps

    def test_fxam(self):
        # 0x00E71F24 (CRT): classe de st(0) em C3 C2 C0 e sinal em C1, sem pop.
        c = lift(b"\xd9\xe5")
        self.assertNotIn("RECOMP_TODO", c)
        self.assertNotIn("fpop", c)
        for frag in ("~0x4700u", "signbit(_v)) _s |= 0x0200u", "isnan(_v)) _s |= 0x0100u",
                     "isinf(_v)) _s |= 0x0500u", "_v == 0.0) _s |= 0x4000u", "else _s |= 0x0400u"):
            self.assertIn(frag, c)

    def test_fpu_environment(self):
        # CRT 0x00E75AA3: fnstenv [esp]; and/or nos bits C do status; fldenv [esp].
        c = lift(b"\xd9\x34\x24")                       # fnstenv [esp]
        self.assertIn("wr32(_a, c->fpu_cw & 0xFFFFu);", c)   # a palavra de controle atual
        self.assertIn("wr32(_a + 4, (c->fpu_sw & 0x4700u) | ((uint32_t)(c->fpu_top & 7) << 11));", c)
        c = lift(b"\xd9\x24\x24")                       # fldenv [esp]
        self.assertIn("c->fpu_sw = _sw & 0x4700u;", c)
        self.assertIn("c->fpu_cw = rd32(_a) & 0xFFFFu;", c)
        self.assertNotIn("fpu_top =", c)                # o TOP do ambiente não mexe na pilha
        # CRT 0x00E66D3D: fnsave [ecx+8] / frstor [ecx+8] em volta de uma chamada.
        c = lift(b"\xdd\x71\x08")
        self.assertIn("wrf80(_a + 28 + 10 * _i, *fst(c, _i));", c)
        self.assertIn("c->fpu_top = 0; c->fpu_sw = 0; c->fpu_cw = 0x037Fu;", c)  # fnsave reinicia como finit
        c = lift(b"\xdd\x61\x08")
        self.assertIn("c->fpu_top = (int)((_sw >> 11) & 7);", c)
        self.assertIn("*fst(c, _i) = rdf80(_a + 28 + 10 * _i);", c)
        self.assertNotIn("RECOMP_TODO", c)

    def test_rounding_control(self):
        # Motor de som, 0x00496FE9: fnstcw; or eax, 0xC00; fldcw; fistp qword; fldcw.
        # A palavra de controle é estado de verdade e o fistp arredonda pelo RC.
        self.assertEqual(lift(b"\xd9\x7c\x24\x14"),            # fnstcw word ptr [esp+0x14]
                         "wr16((c->esp + 0x00000014u), (uint16_t)c->fpu_cw);")
        self.assertEqual(lift(b"\xd9\x2c\x24"), "c->fpu_cw = rd16((c->esp));")   # fldcw word ptr [esp]
        self.assertEqual(lift(b"\xdf\x3c\x24"),                 # fistp qword ptr [esp]
                         "wri64((c->esp), fpu_rint(c, *fst(c, 0))); fpop(c);")
        self.assertEqual(lift(b"\xdb\x14\x24"),                 # fist dword ptr [esp]: sem pop
                         "wri32((c->esp), fpu_rint(c, *fst(c, 0)));")
        self.assertEqual(lift(b"\xdf\x1c\x24"),                 # fistp word ptr [esp]
                         "wri16((c->esp), fpu_rint(c, *fst(c, 0))); fpop(c);")
        self.assertEqual(lift(b"\xd9\xfc"), "*fst(c, 0) = fpu_rint(c, *fst(c, 0));")   # frndint
        self.assertEqual(lift(b"\xdb\xe3"), "c->fpu_cw = 0x037Fu; c->fpu_sw = 0; c->fpu_top = 0;")  # fninit

    def test_rep_string_bulk(self):
        # CRT do RE5: memset 0x00E63D5F (rep stosd) e memcpy (rep movsd) viram uma operação só.
        self.assertEqual(lift(b"\xf3\xab"), "rep_stos(c, 4);")      # rep stosd
        self.assertEqual(lift(b"\xf3\xaa"), "rep_stos(c, 1);")      # rep stosb
        self.assertEqual(lift(b"\xf3\xa5"), "rep_movs(c, 4);")      # rep movsd
        self.assertEqual(lift(b"\xf3\xa4"), "rep_movs(c, 1);")      # rep movsb
        self.assertIn("while (c->ecx)", lift(b"\xf3\xa6"))         # repe cmpsb continua laço
        self.assertNotIn("rep_", lift(b"\xab"))                    # stosd sem rep: um elemento

    def test_packed_sse_is_one_vector_op(self):
        # Desempenho (2026-10-23): mulps pista a pista virava 4 mulss no host.
        self.assertEqual(lift(b"\x0f\x59\xc3"), "c->xmm[0] = xmm_mulps(c->xmm[0], c->xmm[3]);")
        self.assertEqual(lift(b"\x66\x0f\xfe\xc3"), "c->xmm[0] = xmm_paddd(c->xmm[0], c->xmm[3]);")
        self.assertEqual(lift(b"\x0f\x55\xc3"), "c->xmm[0] = xmm_andn(c->xmm[0], c->xmm[3]);")
        self.assertIn("XMM_SHUF4(c->xmm[0], _s, 0, 3, 5, 6)", lift(b"\x0f\xc6\xc1\x9c"))   # shufps 0x9c
        self.assertIn("XMM_SHUF4(c->xmm[0], _s, 0, 4, 1, 5)", lift(b"\x0f\x14\xc1"))       # unpcklps

    def test_flag_liveness_and_register_cache(self):
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        lifter = Lifter.__new__(Lifter)
        lifter.md = md
        lifter.reloc_vas = set()
        lifter.image_lo, lifter.image_hi = 0x00400000, 0x01400000
        lifter.read_va = None
        lifter.flags_dead_ret_below = 0x00E63000
        # add eax, ebx ; sub ecx, 1 ; cmp eax, ecx ; je +1 ; push esi ; call 0x1fff ; ret
        code = bytes.fromhex("01d8" "83e901" "39c8" "7401" "56" "e8f00f0000" "c3")
        c = lifter.lift_function(code, 0x1000)
        lines = {l.split("/* ")[1][:8]: l for l in c.splitlines() if "/* 0000" in l}
        self.assertIn("flags_add_m(c, 0x000u", lines["00001000"])       # add: tudo sobrescrito pelo cmp
        self.assertIn("flags_sub_m(c, 0x000u", lines["00001002"])
        self.assertIn("flags_sub_m(c, 0x040u", lines["00001005"])       # cmp: só ZF (je)
        self.assertIn("uint32_t R_eax = c->eax", c)                       # registradores em locais
        self.assertIn("push32_r(c, &R_esp, R_esi)", lines["00001009"])
        call = lines["0000100A"]
        self.assertLess(call.index("c->esp = R_esp;"), call.index("dispatch(c, 0x00001FFFu)"))  # sincroniza antes
        self.assertIn("c->eax = R_eax;", call)                            # add escreveu eax
        self.assertNotIn("c->edx = R_edx;", call)                         # edx não mudou: não grava
        self.assertGreater(call.index("R_eax = c->eax;"), call.index("dispatch("))              # recarrega depois
        self.assertIn("return; }", lines["0000100F"])
        # No CRT o ret mantém as flags vivas (0x00E72105 devolve resultado em ZF).
        lifter.flags_dead_ret_below = 0x00000800
        c2 = lifter.lift_function(bytes.fromhex("39c8" "c3"), 0x1000)
        self.assertIn("flags_sub(c, ", c2)

    def test_direction_flag(self):
        # std/cld eram ignorados ("string ops assume forward"): o strrchr do CRT
        # (0x00E64680, std; repne scasb) devolvia NULL e o 0x0072BC80 registrava toda
        # malha SBC como colisão de personagem (drops sem chão); o memmove para cima
        # (std; rep movsd) replicava em vez de copiar.
        self.assertEqual(lift(b"\xfd"), "c->df = 1;")              # std
        self.assertEqual(lift(b"\xfc"), "c->df = 0;")              # cld
        for enc in (b"\xae", b"\xac", b"\xa4", b"\xaa", b"\xa6"):  # scasb lodsb movsb stosb cmpsb
            c = lift(enc)
            self.assertIn("STR_STEP(c, 1)", c)
            self.assertNotIn("+= 1;", c)
        self.assertIn("STR_STEP(c, 1)", lift(b"\xf2\xae"))        # repne scasb (o strrchr)

    def test_difftest_findings(self):
        # Achados do scripts/difftest_cpu.py (lift32_cpu contra o Unicorn).
        self.assertIn(".u64[1]", lift(b"\x0f\x17\x17"))        # movhps [edi], xmm2: a metade ALTA
        self.assertIn(".u64[1]", lift(b"\x0f\x16\x07"))        # movhps xmm0, [edi]
        self.assertIn(".u64[0]", lift(b"\x0f\x13\x07"))        # movlps [edi], xmm0: a baixa
        self.assertIn("c->fpu_top & 7) << 11", lift(b"\xdf\xe0"))   # fnstsw ax com TOP
        self.assertIn(", 1);", lift(b"\xd9\xf5"))              # fprem1: quociente arredondado
        self.assertIn(", 0);", lift(b"\xd9\xf8"))              # fprem: truncado

    def test_mxcsr(self):
        # Thread de som, 0x0048C591: stmxcsr; or eax, 0x8000 (FTZ); ldmxcsr.
        self.assertEqual(lift(b"\x0f\xae\x1c\x24"), "wr32((c->esp), c->mxcsr);")        # stmxcsr [esp]
        self.assertEqual(lift(b"\x0f\xae\x54\x24\x04"),                                 # ldmxcsr [esp+4]
                         "cpu_ldmxcsr(c, rd32((c->esp + 0x00000004u)));")

    def test_packed_min_max(self):
        # CRT 0x00E671BE: pmaxsw xmm1, xmm3 (com sinal, 8 palavras); a forma MMX segue TODO.
        c = lift(b"\x66\x0f\xee\xcb")
        self.assertIn("XMM _s = c->xmm[3]; XMM _d = c->xmm[1];", c)
        self.assertIn("c->xmm[1].i16[7] = _d.i16[7] > _s.i16[7] ? _d.i16[7] : _s.i16[7];", c)
        self.assertIn("c->xmm[1].u8[15] = _d.u8[15] < _s.u8[15]", lift(b"\x66\x0f\xda\xcb"))   # pminub
        self.assertIn("RECOMP_TODO", lift(b"\x0f\xee\xcb"))                                   # pmaxsw mm1, mm3

    def test_xlatb(self):
        # 0x00E71F5D (CRT, logo depois do fxam): AL = [EBX + AL], só o byte baixo muda.
        self.assertEqual(lift(b"\xd7"), "SET8L(c->eax, rd8((uint32_t)(c->ebx + R8L(c->eax))));")

    def test_hint_nop_with_register_operand(self):
        # 0F 18 C9 (0x00456094, laço de render do RE5): NOP de dica que o capstone
        # recusa. Descoberta e lift têm de passar por ele, sem cortar a função.
        import lift32_cpu
        sys.path.insert(0, os.path.join(REPO, "tools", "pcrecomp", "tools", "disasm"))
        import disasm32
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        code = b"\x0f\x18\xc9\x0f\x87\x10\x00\x00\x00\xc3"   # hint nop; ja +0x10; ret
        self.assertEqual(list(md.disasm(code, 0x1000)), [])   # o capstone para no primeiro byte
        for got in (list(lift32_cpu._disasm(md, code, 0x1000)), list(disasm32.decode(md, code, 0x1000))):
            self.assertEqual([(i.address, i.size, i.mnemonic) for i in got],
                             [(0x1000, 3, "nop"), (0x1003, 6, "ja"), (0x1009, 1, "ret")])
        self.assertIsNone(disasm32.hint_nop(b"\x0f\x18\x08", 0))      # prefetcht0 [eax]: o capstone já lê
        self.assertEqual(disasm32.hint_nop(b"\x0f\x18\xc9", 0), b"\x0f\x1f\xc1")


if __name__ == "__main__":
    unittest.main()
