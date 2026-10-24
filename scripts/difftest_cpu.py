#!/usr/bin/env python3
"""Teste diferencial do lift32_cpu (etapa 2.2): o C gerado faz o que a CPU faz?

O difftest.py do pcrecomp cobre o lift32 (registradores globais); o RE5 usa o
lift32_cpu, que não tinha nada parecido - e foi lá que moravam o fistp que
ignorava o fldcw (som chiado, itens) e outros. Aqui cada instrução roda duas
vezes a partir do mesmo estado: no Unicorn (a referência do que um x86 faz) e no
C que o lift32_cpu escreve, compilado e executado num processo x64 com a mesma
memória mapeada nos mesmos endereços (o cpu.h genérico do pcrecomp: endereço do
guest = endereço do host). Compara os 8 registradores, as flags que a
instrução define, a pilha x87 (TOP e valores), C0/C2/C3 nas comparações, os
XMM e cada byte de memória escrito.

As instruções vêm do próprio binário: todas as das funções do catálogo,
agrupadas pela forma (mnemônico + tipos e tamanhos dos operandos), algumas
codificações de cada grupo, cada uma com vários estados aleatórios.

    python scripts/difftest_cpu.py                 # amostra padrão
    python scripts/difftest_cpu.py -k fistp,mulss  # só mnemônicos que contêm isto
    python scripts/difftest_cpu.py --per-group 4 --seeds 5

Precisa de: unicorn, capstone, pefile (pip), gcc (MinGW no Windows) e o
work/catalog.json. Sai com 1 se alguma instrução divergir.
"""
import argparse
import collections
import json
import os
import random
import struct
import subprocess
import sys
import tempfile

try:
    import numpy as np
    import pefile
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_MEM
    from unicorn import UC_ARCH_X86, UC_MODE_32, Uc, UcError
    from unicorn import x86_const as X
except ImportError as _ex:   # ctest: sem as dependências, pula (77)
    print(f"[-] difftest_cpu: {_ex} (pip install unicorn capstone pefile numpy)")
    sys.exit(77)

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "pcrecomp", "tools", "lift"))
import lift32_cpu  # noqa: E402

LO, HI = 0x00400000, 0x01800000          # memória dos dois lados (a .data do RE5 cabe aqui)
CODE = 0x017F0000                        # código do Unicorn (fora do alcance dos operandos)
AUX = 0x017E0000                         # entradas/saídas da pilha x87 no Unicorn
SAFE_HI = 0x017D0000                     # operandos de memória têm de cair abaixo disto
PTR_LO, PTR_HI = 0x00500000, 0x005F0000  # onde os registradores-ponteiro apontam
ESP0 = 0x00680000
FPU_CW = 0x027F
NST = 4                                  # valores na pilha x87 antes da instrução

GPR = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UC_GPR = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_EBX,
          X.UC_X86_REG_ESP, X.UC_X86_REG_EBP, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI]
UC_XMM = [getattr(X, f"UC_X86_REG_XMM{i}") for i in range(8)]
FLAGS = {"cf": 0, "pf": 2, "af": 4, "zf": 6, "sf": 7, "df": 10, "of": 11}   # df: direção das operações de string

SKIP = {  # controle de fluxo, sistema, segmentos e o que o modelo de propósito não segue (D9)
    "call", "ret", "retf", "jmp", "ljmp", "lcall", "loop", "loope", "loopne", "jecxz", "jcxz", "int", "int3",
    "into", "iretd", "hlt", "cpuid", "rdtsc", "in", "out", "insb", "insd", "outsb", "outsd", "sysenter",
    "les", "lds", "lss", "lfs", "lgs", "fnstenv", "fldenv", "fnsave", "frstor", "fsave", "fstenv", "fwait",
    "wait", "fbld", "fbstp", "fldcw", "fnstcw", "fninit", "fnclex", "ldmxcsr", "stmxcsr", "nop", "pause",
    "bound", "arpl", "aaa", "aas", "daa", "das", "aam", "aad", "enter",
    "fxsave", "fxrstor",
    # popfd/popf tiram da pilha um EFLAGS sorteado: com TF/AC ligados o Unicorn passa a
    # tratar armadilhas e a referência sai errada (até no x87). lahf/sahf/pushfd ficam.
    "popfd", "popf",
    # A referência erra aqui: o QEMU calcula fprem/fprem1 em double aproximado
    # (261,4498 rem1 -250,1177 deu -238,79; o exato é 11,3321, o que o modelo dá
    # com remainder) e não desempilha no fyl2x de argumento negativo.
    "fprem", "fprem1", "fyl2x", "fyl2xp1", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "emms", "fisttp",
}
# Flags que a instrução deixa indefinidas (Intel SDM): não comparadas. AF nunca é
# comparada (o modelo não a segue fora dos casos que o RE5 usa: nenhum DAA/AAA).
UNDEF = collections.defaultdict(set)
for m in ("mul", "imul"):
    UNDEF[m] |= {"sf", "zf", "pf"}
for m in ("div", "idiv"):
    UNDEF[m] |= {"cf", "of", "sf", "zf", "pf"}
for m in ("and", "or", "xor", "test"):
    pass
for m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
    UNDEF[m] |= {"of"}          # definida só com contagem 1; tratada abaixo
for m in ("bsf", "bsr"):
    UNDEF[m] |= {"cf", "of", "sf", "pf"}
for m in ("bt", "bts", "btr", "btc"):
    UNDEF[m] |= {"of", "sf", "pf", "zf"}
# fcomi e família: o Intel SDM zera OF/SF/AF (o modelo faz isso); o Unicorn
# (QEMU) as preserva. Divergência da referência, não do modelo.
for m in ("fcomi", "fcomip", "fcompi", "fucomi", "fucomip", "fucompi"):
    UNDEF[m] |= {"of", "sf"}
TRANSCENDENTAL = {"fsin", "fcos", "fsincos", "fptan", "fpatan", "f2xm1", "fyl2x", "fyl2xp1"}
APPROX = {"rcpss", "rsqrtss", "rcpps", "rsqrtps"}
FPU_CMP = ("fcom", "fucom", "ftst", "fxam", "fprem", "ficom")


def collect(exe, catalog, only, per_group, rng):
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    groups = collections.defaultdict(list)
    funcs = json.load(open(catalog))["functions"]
    for f in funcs:
        a, e = f["address"], f["end"]
        if f["size"] <= 0:
            continue
        for i in md.disasm(img[a - base:e - base], a):
            m = i.mnemonic
            head = m.split()[-1]
            if head in SKIP or m.split()[0] in SKIP or head.startswith("j") or i.prefix[0] == 0xF0:
                continue
            if any(op.type == X86_OP_MEM and op.mem.segment != 0 for op in i.operands):
                continue      # fs:/gs:/es: explícito
            if m == "fld" and any(op.type == X86_OP_MEM and op.size == 10 for op in i.operands):
                continue      # 80 bits de memória aleatória: codificações inválidas (pseudo-NaN)
            if only and not any(k in m for k in only):
                continue
            sig = (m, tuple((op.type, op.size) for op in i.operands))
            g = groups[sig]
            if len(g) < 64:
                g.append(bytes(i.bytes))
    cases = []
    for sig, encs in sorted(groups.items()):
        uniq = list(dict.fromkeys(encs))
        rng.shuffle(uniq)
        for b in uniq[:per_group]:
            cases.append(b)
    return cases


SPECIAL_D = [float("nan"), float("inf"), -float("inf"), -0.0, 5e-324, 2.2250738585072014e-308, 1e300, -1e300,
             2147483648.0, -2147483649.0, 0.5, -0.5, 1.5, 2.5]
SPECIAL_F = [0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000, 0x80000000, 0x00000001, 0x00800000, 0x7F7FFFFF,
             0x4F000000, 0xCF000001, 0x3F000000, 0xBF000000, 0x3FC00000, 0x40200000]


def rand_state(rng, insn, special=0.0):
    regs = []
    for r in range(8):
        if r == 4:
            regs.append(ESP0 - 4 * rng.randrange(0, 64))
        elif rng.random() < 0.5:
            regs.append((PTR_LO + rng.randrange(0, (PTR_HI - PTR_LO) // 4) * 4) & 0xFFFFFFFF)
        else:
            choice = rng.random()
            if choice < 0.3:
                regs.append(rng.randrange(0, 64))
            elif choice < 0.4:
                regs.append(0xFFFFFFFF - rng.randrange(0, 16))
            else:
                regs.append(rng.getrandbits(32))
    m = insn.mnemonic
    if m.startswith(("rep", "movs", "stos", "lods", "scas", "cmps")):
        regs[1] = rng.randrange(0, 9)                                    # ecx pequeno
        regs[6] = PTR_LO + rng.randrange(0, 0x1000) * 4                  # esi
        regs[7] = PTR_LO + 0x40000 + rng.randrange(0, 0x1000) * 4        # edi
    flags = 0x202 | sum((rng.getrandbits(1) << b) for b in FLAGS.values())
    st = []
    for _ in range(NST):
        k = rng.random()
        if rng.random() < special:
            st.append(rng.choice(SPECIAL_D))
        elif k < 0.1:
            st.append(0.0)
        elif k < 0.3:
            st.append(float(rng.randrange(-100000, 100000)))
        elif k < 0.4:
            st.append(rng.uniform(-1, 1) * 1e-3)
        else:
            st.append(rng.uniform(-1000, 1000))
    xmm = []
    for _ in range(8):
        if rng.random() < special:
            lanes = [rng.choice(SPECIAL_F) if rng.random() < 0.5 else
                     struct.unpack("<I", struct.pack("<f", rng.uniform(-500, 500)))[0] for _ in range(4)]
        elif rng.random() < 0.8:
            lanes = [struct.unpack("<I", struct.pack("<f", rng.uniform(-500, 500)))[0] for _ in range(4)]
        else:
            lanes = [rng.getrandbits(32) for _ in range(4)]
        xmm.append(lanes)
    return regs, flags, st, xmm


def ea_ok(insn, regs):
    """Os operandos de memória caem na janela segura, para os dois lados."""
    names = dict(zip(GPR, regs))
    for op in insn.operands:
        if op.type != X86_OP_MEM:
            continue
        mem = op.mem
        a = mem.disp
        if mem.base:
            a += names.get(insn.reg_name(mem.base), None) if insn.reg_name(mem.base) in names else 0
            if insn.reg_name(mem.base) not in names:
                return False
        if mem.index:
            n = insn.reg_name(mem.index)
            if n not in names:
                return False
            a += names[n] * mem.scale
        a &= 0xFFFFFFFF
        if not (LO <= a and a + max(op.size, 16) <= SAFE_HI):
            return False
    if insn.mnemonic.startswith(("push", "pop")) or "pushal" in insn.mnemonic or "popal" in insn.mnemonic:
        pass
    return True


def run_unicorn(uc, code, regs, flags, st, xmm):
    pro = b"".join(b"\xdd\x05" + struct.pack("<I", AUX + 8 * i) for i in reversed(range(NST)))   # fld qword [AUX+8i]
    for i, v in enumerate(st):
        uc.mem_write(AUX + 8 * i, struct.pack("<d", v))
    uc.mem_write(AUX + 0x100, struct.pack("<H", FPU_CW))
    pro = b"\xd9\x2d" + struct.pack("<I", AUX + 0x100) + b"\xdb\xe3"[:0] + pro         # fldcw [cw]
    prog = pro + code
    uc.mem_write(CODE, prog)
    uc.reg_write(X.UC_X86_REG_FPSW, 0)
    uc.reg_write(X.UC_X86_REG_FPTAG, 0xFFFF)
    uc.reg_write(X.UC_X86_REG_MXCSR, 0x1F80)
    for r, v in zip(UC_GPR, regs):
        uc.reg_write(r, v)
    uc.reg_write(X.UC_X86_REG_EFLAGS, flags)
    for r, lanes in zip(UC_XMM, xmm):
        uc.reg_write(r, lanes[0] | lanes[1] << 32 | lanes[2] << 64 | lanes[3] << 96)
    uc.emu_start(CODE, CODE + len(prog))
    if uc.reg_read(X.UC_X86_REG_EIP) != CODE + len(prog):
        raise UcError(0)    # a CPU de referência parou numa exceção (#DE, #GP): o caso não vale
    out = {"regs": [uc.reg_read(r) & 0xFFFFFFFF for r in UC_GPR], "eflags": uc.reg_read(X.UC_X86_REG_EFLAGS)}
    sw = uc.reg_read(X.UC_X86_REG_FPSW)
    top = (sw >> 11) & 7
    out["sw"], out["top"] = sw, top
    xs = []
    for r in UC_XMM:
        v = uc.reg_read(r)
        xs.append([(v >> (32 * k)) & 0xFFFFFFFF for k in range(4)])
    out["xmm"] = xs
    n = (8 - top) & 7
    epi = b"".join(b"\xdd\x1d" + struct.pack("<I", AUX + 0x200 + 8 * i) for i in range(n))   # fstp qword
    if epi:
        uc.mem_write(CODE, epi)
        uc.emu_start(CODE, CODE + len(epi))
    out["st"] = [struct.unpack("<d", uc.mem_read(AUX + 0x200 + 8 * i, 8))[0] for i in range(n)]
    return out


HARNESS_HEAD = r"""
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "cpu.h"
uint32_t g_image_delta;
#if !RE5_GUEST_BASE_FIXED
uintptr_t re5_guest_base;
#endif
const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;
void re5_recomp_todo(uint32_t va, const char *what) { printf("X todo 0x%08X %s\n", va, what); fflush(stdout); exit(3); }
void re5_override(CPU *c, uint32_t va) { (void)c; re5_recomp_todo(va, "override"); }
void re5_guest_preempt(void) {}
void dispatch(CPU *c, uint32_t va) { (void)c; printf("X dispatch 0x%08X\n", va); fflush(stdout); exit(3); }
void dispatch_jmp(CPU *c, uint32_t va) { dispatch(c, va); }
void recomp_cpuid(CPU *c) { (void)c; }
void recomp_rdtsc(CPU *c) { (void)c; }
typedef struct { uint32_t regs[8], flags; double st[4]; uint32_t xmm[8][4]; } init_t;
"""

HARNESS_MAIN = r"""
static uint8_t *g_pat;
int main(int argc, char **argv)
{
    /* O cpu.h gerado do RE5: endereço do guest = re5_guest_base + VA (como no jogo). */
#if RE5_GUEST_BASE_FIXED
    /* base constante (guest_base.h): a janela do teste fica no mesmo lugar que no jogo */
    uint8_t *mem = VirtualAlloc((void *)(re5_guest_base + %(lo)du), %(size)du, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
    if (mem != (uint8_t *)(re5_guest_base + %(lo)du)) { printf("X VirtualAlloc\n"); return 2; }
#else
    uint8_t *mem = VirtualAlloc(NULL, %(size)du, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { printf("X VirtualAlloc\n"); return 2; }
    re5_guest_base = (uintptr_t)mem - %(lo)du;
#endif
    g_pat = malloc(%(size)du);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(g_pat, 1, %(size)du, f) != %(size)du) { printf("X padrão\n"); return 2; }
    fclose(f);
    memcpy(mem, g_pat, %(size)du);
    int start = argc > 2 ? atoi(argv[2]) : 0;
    for (int i = start; i < NCASES; i++) {
        printf("C %%d\n", i); fflush(stdout);
        CPU cpu; memset(&cpu, 0, sizeof cpu); CPU *c = &cpu;
        const init_t *in = &k_init[i];
        c->eax = in->regs[0]; c->ecx = in->regs[1]; c->edx = in->regs[2]; c->ebx = in->regs[3];
        c->esp = in->regs[4]; c->ebp = in->regs[5]; c->esi = in->regs[6]; c->edi = in->regs[7];
        eflags_unpack(c, in->flags);
        c->fpu_cw = 0x027F; c->mxcsr = 0x1F80;
        for (int k = %(nst)d - 1; k >= 0; k--) fpush(c, in->st[k]);
        memcpy(c->xmm, in->xmm, sizeof in->xmm);
        k_case[i](c);
        printf("R %%08X %%08X %%08X %%08X %%08X %%08X %%08X %%08X F %%08X T %%d S %%08X\n", c->eax, c->ecx, c->edx, c->ebx,
               c->esp, c->ebp, c->esi, c->edi, eflags_pack(c), c->fpu_top, c->fpu_sw);
        int n = (8 - c->fpu_top) & 7;
        printf("ST");
        for (int k = 0; k < n; k++) { double v = *fst(c, k); uint64_t b; memcpy(&b, &v, 8); printf(" %%016llX", (unsigned long long)b); }
        printf("\nXM");
        for (int r = 0; r < 8; r++) for (int k = 0; k < 4; k++) printf(" %%08X", ((const uint32_t *)&c->xmm[r])[k]);
        printf("\nM");
        int nd = 0;
        for (uint32_t a = 0; a < %(size)du; a += 8) {
            if (*(uint64_t *)(mem + a) == *(uint64_t *)(g_pat + a)) continue;
            for (uint32_t b = a; b < a + 8; b++)
                if (mem[b] != g_pat[b]) { if (nd++ < 64) printf(" %%08X:%%02X", %(lo)du + b, mem[b]); mem[b] = g_pat[b]; }
        }
        printf("\n"); fflush(stdout);
    }
    printf("FIM\n");
    return 0;
}
"""


def c_double(v):
    """Literal C exato (hexadecimal), com NaN/inf pelas macros do math.h."""
    v = float(v)
    if v != v:
        return "NAN"
    if v in (float("inf"), -float("inf")):
        return "INFINITY" if v > 0 else "-INFINITY"
    return v.hex()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-k", default="", help="só mnemônicos que contêm um destes (vírgulas)")
    ap.add_argument("--per-group", type=int, default=2)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--exe", default=os.path.join(ROOT, "bin_input", "re5dx9.clean.exe"))
    ap.add_argument("--catalog", default=os.path.join(ROOT, "work", "catalog.json"))
    ap.add_argument("--cc", default=os.environ.get("RECOMP_CC", "gcc"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--special", type=float, default=0.25, help="chance de valor especial (NaN, inf, -0...)")
    args = ap.parse_args()
    rng = random.Random(1234)
    if not os.path.exists(args.catalog) or not os.path.exists(args.exe):
        print(f"[-] difftest_cpu: sem {args.catalog} ou {args.exe} (Fase 2)")
        sys.exit(77)

    encs = collect(args.exe, args.catalog, [k for k in args.k.split(",") if k], args.per_group, rng)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = lift32_cpu.Lifter.__new__(lift32_cpu.Lifter)
    lifter.md = md
    lifter.reloc_vas = set()
    lifter.image_lo, lifter.image_hi = 0x00400000, 0x01400000

    cases = []   # (bytes, insn text, state, C lines)
    skipped = collections.Counter()
    for b in encs:
        insn = next(md.disasm(b, CODE + 6 + 6 * NST))
        try:
            lines = lifter.translate(insn, set())
        except Exception as ex:   # o lifter recusa: conta, não testa
            skipped["lift: " + type(ex).__name__] += 1
            continue
        text = " ".join(lines)
        if "RECOMP_TODO" in text or "goto" in text or "dispatch" in text or "c->eip" in text:
            skipped["sem tradução/fluxo"] += 1
            continue
        for _ in range(args.seeds):
            for _try in range(20):
                st = rand_state(rng, insn, args.special)
                if ea_ok(insn, st[0]):
                    cases.append((b, f"{insn.mnemonic} {insn.op_str}", st, lines))
                    break
            else:
                skipped["operando fora da janela"] += 1
    print(f"[*] {len(encs)} codificações, {len(cases)} casos; ignorados: {dict(skipped)}")

    pat = np.random.default_rng(99).integers(0, 256, HI - LO, dtype=np.uint8)
    work = tempfile.mkdtemp(prefix="difftest_cpu_")
    patf = os.path.join(work, "pat.bin")
    pat.tofile(patf)

    src = [HARNESS_HEAD]
    for i, (_, txt, _, lines) in enumerate(cases):
        src.append(f"static void case_{i}(CPU *c) {{ /* {txt} */ " + " ".join(lines) + " }")
    src.append("static void (*const k_case[])(CPU *) = {" + ", ".join(f"case_{i}" for i in range(len(cases))) + "};")
    src.append("static const init_t k_init[] = {")
    for _, _, (regs, flags, st, xmm), _ in cases:
        src.append("{{" + ",".join(f"0x{r:08X}u" for r in regs) + f"}}, 0x{flags:08X}u, {{" +
                   ",".join(c_double(v) for v in st) + "}, {" +
                   ",".join("{" + ",".join(f"0x{l:08X}u" for l in lanes) + "}" for lanes in xmm) + "}},")
    src.append("};")
    src.append(f"#define NCASES {len(cases)}")
    src.append(HARNESS_MAIN % {"lo": LO, "size": HI - LO, "nst": NST})
    cfile = os.path.join(work, "harness.c")
    open(cfile, "w").write("\n".join(src))
    exe = os.path.join(work, "harness.exe")
    # O cpu.h que o jogo usa (scripts/gen_cpu_header.py), do build.
    gen = os.path.join(ROOT, "build", os.environ.get("RE5_BUILD", "test-build"), "gen_include")
    r = subprocess.run([args.cc, "-O1", "-w", "-fno-strict-aliasing", "-fwrapv", "-I", gen, "-I",
                        os.path.join(ROOT, "runtime", "include"), cfile, "-o", exe], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000])
        sys.exit("[!] o harness não compilou")

    # Lado C, com recomeço depois de um caso que derruba o processo.
    cres, start = {}, 0
    while start < len(cases):
        p = subprocess.run([exe, patf, str(start)], capture_output=True, text=True)
        cur = None
        for line in p.stdout.splitlines():
            if line.startswith("C "):
                cur = int(line[2:])
                cres[cur] = {}
            elif line.startswith("R ") and cur is not None:
                t = line.split()
                cres[cur]["regs"] = [int(x, 16) for x in t[1:9]]
                cres[cur]["eflags"], cres[cur]["top"], cres[cur]["sw"] = int(t[10], 16), int(t[12]), int(t[14], 16)
            elif line.startswith("ST") and cur is not None:
                cres[cur]["st"] = [struct.unpack("<d", struct.pack("<Q", int(x, 16)))[0] for x in line.split()[1:]]
            elif line.startswith("XM") and cur is not None:
                v = [int(x, 16) for x in line.split()[1:]]
                cres[cur]["xmm"] = [v[4 * k:4 * k + 4] for k in range(8)]
            elif line.startswith("M") and cur is not None:
                cres[cur]["mem"] = {int(a, 16): int(b, 16) for a, b in (x.split(":") for x in line.split()[1:])}
            elif line.startswith("X "):
                print("[!] harness:", line)
        if cur is None:
            sys.exit(f"[!] harness não rodou: {p.stdout[:500]} {p.stderr[:500]}")
        if "FIM" in p.stdout:
            break
        cres[cur]["crash"] = True
        start = cur + 1

    # Lado Unicorn e comparação.
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(LO, HI - LO)
    uc.mem_write(LO, pat.tobytes())
    bad = collections.defaultdict(list)
    for i, (b, txt, (regs, flags, st, xmm), _) in enumerate(cases):
        m = txt.split()[0] if not txt.startswith("rep") else txt.split()[1]
        try:
            u = run_unicorn(uc, b, regs, flags, st, xmm)
        except UcError as ex:
            uc.mem_write(LO, pat.tobytes())
            continue
        um = np.frombuffer(bytes(uc.mem_read(LO, SAFE_HI - LO)), dtype=np.uint8)
        diff = np.nonzero(um != pat[:SAFE_HI - LO])[0]
        umem = {LO + int(d): int(um[d]) for d in diff[:64]}
        for d in diff:
            uc.mem_write(LO + int(d), bytes([int(pat[d])]))
        c = cres.get(i, {})
        why = []
        if c.get("crash") or "regs" not in c:
            why.append("o C derrubou o processo")
        else:
            for k, (a1, a2) in enumerate(zip(u["regs"], c["regs"])):
                if a1 != a2:
                    why.append(f"{GPR[k]} hw={a1:08X} C={a2:08X}")
            undef = set(UNDEF.get(m, ())) | {"af"}
            if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
                undef |= {"of"}
            for name, bit in FLAGS.items():
                if name in undef:
                    continue
                if (u["eflags"] >> bit & 1) != (c["eflags"] >> bit & 1):
                    why.append(f"{name.upper()} hw={u['eflags'] >> bit & 1} C={c['eflags'] >> bit & 1}")
            if u["top"] != c["top"]:
                why.append(f"TOP hw={u['top']} C={c['top']}")
            else:
                for k, (v1, v2) in enumerate(zip(u["st"], c["st"])):
                    same = v1 == v2 or (v1 != v1 and v2 != v2)
                    if not same and m in TRANSCENDENTAL:
                        same = abs(v1 - v2) <= 1e-12 * max(1.0, abs(v1))
                    if not same:
                        why.append(f"st({k}) hw={v1!r} C={v2!r}")
            if m.startswith(FPU_CMP) and (u["sw"] & 0x4500) != (c["sw"] & 0x4500):
                why.append(f"C3C2C0 hw={u['sw'] & 0x4500:04X} C={c['sw'] & 0x4500:04X}")
            for r in range(8):
                for k in range(4):
                    a1, a2 = u["xmm"][r][k], c["xmm"][r][k]
                    if a1 == a2:
                        continue
                    if m in APPROX:
                        f1 = struct.unpack("<f", struct.pack("<I", a1))[0]
                        f2 = struct.unpack("<f", struct.pack("<I", a2))[0]
                        if abs(f1 - f2) <= 2 ** -11 * max(abs(f1), 1e-30):
                            continue
                    why.append(f"xmm{r}[{k}] hw={a1:08X} C={a2:08X}")
            cm = c.get("mem", {})
            for a in sorted(set(umem) | set(cm)):
                if a >= SAFE_HI:
                    continue
                if umem.get(a) != cm.get(a):
                    why.append(f"mem {a:08X} hw={umem.get(a)} C={cm.get(a)}")
                    break
        if why:
            bad[m].append((txt + (f" st={st[:2]}" if m.startswith("f") else ""), b.hex(), why[:4]))

    total = len(cases)
    nbad = sum(len(v) for v in bad.values())
    print(f"[*] {total - nbad}/{total} casos iguais ao hardware (Unicorn); {len(bad)} mnemônicos com divergência")
    for m, lst in sorted(bad.items(), key=lambda kv: -len(kv[1])):
        print(f"  {m}: {len(lst)} caso(s)")
        for txt, hx, why in lst[:3]:
            print(f"      {txt:40s} [{hx}]  " + "; ".join(why))
    if not args.keep:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("[*] arquivos em", work)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
