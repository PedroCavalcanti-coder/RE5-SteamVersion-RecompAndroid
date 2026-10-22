#!/usr/bin/env python3
"""Teste diferencial de FUNÇÕES inteiras do RE5: Unicorn x C do lift32_cpu.

O difftest_cpu.py compara instrução por instrução. Aqui uma função do jogo
roda inteira, com a imagem do re5dx9.clean.exe mapeada (código e tabelas da
.rdata), a partir do mesmo argumento: no Unicorn e no C que o lift32_cpu
escreve para ela e para as funções que ela chama (fecho das chamadas diretas,
ligadas por uma tabela própria). Compara xmm0 e st(0) na volta, bit a bit.

Feito para as rotinas matemáticas do CRT (__libm_sse2_*: argumento e resultado
em xmm0) e as _CI* (argumento e resultado em st(0)), que o jogo chama milhares
de vezes - ângulo de rampa, rotação, distância.

    python scripts/difftest_func.py                      # as rotinas padrão
    python scripts/difftest_func.py 0x00E66572 0x00E6476B
"""
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

import pefile
from unicorn import UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, Uc, UcError
from unicorn import x86_const as X

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "pcrecomp", "tools", "lift"))
import lift32_cpu  # noqa: E402

STACK_LO, STACK_HI = 0x30000000, 0x30100000
SENTINEL = 0x30200000          # endereço de retorno: o Unicorn para aqui
FPU_CW = 0x027F
# Rotinas do CRT que o jogo chama (contagem de chamadas no binário):
DEFAULT = [0x00E6476B, 0x00E64923, 0x00E64BED, 0x00E66572, 0x00E66E1A, 0x00E679E6, 0x00E67EE5, 0x00E68331,
           0x00E65200, 0x00E68C70]


def inputs(rng):
    xs = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, 0.1, 1e-8, 1e-300, 3.141592653589793, 1.5707963267948966,
          100.0, -100.0, 1e10, float("inf"), -float("inf"), float("nan"), 0.9999999, -0.9999999]
    for _ in range(300):
        xs.append(rng.uniform(-1, 1))
    for _ in range(200):
        xs.append(rng.uniform(-10, 10))
    for _ in range(100):
        xs.append(rng.uniform(-1000, 1000))
    for _ in range(50):
        xs.append(math.ldexp(rng.uniform(0.5, 1), rng.randrange(-60, 60)) * rng.choice((1, -1)))
    return xs


def main():
    entries = [int(a, 16) for a in sys.argv[1:]] or DEFAULT
    exe = os.path.join(ROOT, "bin_input", "re5dx9.clean.exe")
    cat = {f["address"]: f for f in json.load(open(os.path.join(ROOT, "work", "catalog.json")))["functions"]
           if f["size"] > 0}
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    lift32_cpu.IMAGE_BASE = base
    lifter = lift32_cpu.Lifter(exe, pe.OPTIONAL_HEADER.SizeOfImage, lambda va, n: pe.get_data(va - base, n))
    lifter.reloc_vas = set()

    # Fecho das chamadas diretas a partir das entradas.
    bodies, todo = {}, list(entries)
    while todo:
        va = todo.pop()
        if va in bodies or va not in cat or len(bodies) > 200:
            continue
        f = cat[va]
        body = lifter.lift_function(img[va - base:f["end"] - base], va)
        bodies[va] = body
        for m in __import__("re").finditer(r"dispatch(?:_jmp)?\(c, 0x([0-9A-F]{8})u\)", body):
            todo.append(int(m.group(1), 16))
    print(f"[*] {len(entries)} entradas, {len(bodies)} funções no fecho")

    rng = random.Random(7)
    xs = inputs(rng)
    ys = [rng.choice((rng.uniform(-10, 10), rng.uniform(-1, 1), 2.0, 0.5, -1.0, 0.0)) for _ in xs]   # 2º argumento (xmm1)
    work = tempfile.mkdtemp(prefix="difftest_func_")
    imgf = os.path.join(work, "img.bin")
    open(imgf, "wb").write(bytes(img))
    src = [r"""#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "cpu.h"
uint32_t g_image_delta;
uintptr_t re5_guest_base;
const re5_func_entry re5_lifted_table[1];
const uint32_t re5_lifted_count = 0;
void re5_recomp_todo(uint32_t va, const char *what) { printf("X todo 0x%08X %s\n", va, what); fflush(stdout); exit(3); }
void re5_override(CPU *c, uint32_t va) { (void)c; re5_recomp_todo(va, "override"); }
void re5_guest_preempt(void) {}
void recomp_cpuid(CPU *c) { (void)c; }
void recomp_rdtsc(CPU *c) { (void)c; }
"""]
    for va in bodies:
        src.append(f"void L_{va:08X}(CPU *c);")
    src.append("static const struct { uint32_t va; void (*fn)(CPU *); } k_fn[] = {" +
               ", ".join(f"{{0x{va:08X}u, L_{va:08X}}}" for va in bodies) + "};")
    src.append(r"""
static int g_out;
void dispatch(CPU *c, uint32_t va)
{
    for (size_t i = 0; i < sizeof k_fn / sizeof k_fn[0]; i++)
        if (k_fn[i].va == va) { k_fn[i].fn(c); return; }
    g_out = 1; printf("O 0x%08X\n", va); fflush(stdout); exit(4);
}
void dispatch_jmp(CPU *c, uint32_t va) { dispatch(c, va); }
""")
    for body in bodies.values():
        src.append(body)
    src.append(r"""
int main(int argc, char **argv)
{
    size_t size = %d;
    /* Guest em re5_guest_base + VA: imagem em 0x00400000, pilha em 0x30000000. */
    uint8_t *mem = VirtualAlloc(NULL, 0x31000000u, MEM_RESERVE, PAGE_NOACCESS);
    if (!mem) return 2;
    if (!VirtualAlloc(mem + 0x00400000u, size, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(mem + 0x30000000u, 0x00100000u, MEM_COMMIT, PAGE_READWRITE)) return 2;
    re5_guest_base = (uintptr_t)mem;
    FILE *f = fopen(argv[1], "rb");
    if (fread(mem + 0x00400000u, 1, size, f) != size) return 2;
    fclose(f);
    uint32_t entry = (uint32_t)strtoul(argv[2], NULL, 16);
    for (int i = 3; i + 1 < argc; i += 2) {
        uint64_t bits = strtoull(argv[i], NULL, 16), bits1 = strtoull(argv[i + 1], NULL, 16);
        double x; memcpy(&x, &bits, 8);
        CPU cpu; memset(&cpu, 0, sizeof cpu); CPU *c = &cpu;
        c->fpu_cw = 0x027F; c->mxcsr = 0x1F80;
        c->esp = %du - 0x100u;
        c->xmm[0].u64[0] = bits;
        c->xmm[1].u64[0] = bits1;
        fpush(c, x);
        push32(c, %du);
        dispatch(c, entry);
        double st0 = *fst(c, 0); uint64_t sb; memcpy(&sb, &st0, 8);
        printf("R %%016llX %%016llX %%d\n", (unsigned long long)c->xmm[0].u64[0], (unsigned long long)sb, c->fpu_top);
        fflush(stdout);
    }
    return 0;
}
""" % (len(img), STACK_HI, SENTINEL))
    cfile = os.path.join(work, "h.c")
    open(cfile, "w").write("\n".join(src))
    gen = os.path.join(ROOT, "build", os.environ.get("RE5_BUILD", "test-build"), "gen_include")
    hexe = os.path.join(work, "h.exe")
    r = subprocess.run([os.environ.get("RECOMP_CC", "gcc"), "-O1", "-w", "-fno-strict-aliasing", "-fwrapv", "-I", gen,
                        "-I", os.path.join(ROOT, "runtime", "include"), cfile, "-o", hexe],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:3000])
        sys.exit("[!] não compilou")

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, (len(img) + 0xFFF) & ~0xFFF, UC_PROT_ALL)
    uc.mem_write(base, bytes(img))
    uc.mem_map(STACK_LO, STACK_HI - STACK_LO)
    uc.mem_map(SENTINEL, 0x1000)
    uc.mem_map(0x30400000, 0x1000)
    uc.mem_write(0x30400000, struct.pack("<H", FPU_CW))
    total_bad = 0
    for entry in entries:
        if entry not in bodies:
            print(f"  0x{entry:08X}: fora do catálogo")
            continue
        bits = [struct.unpack("<Q", struct.pack("<d", x))[0] for x in xs]
        bits1 = [struct.unpack("<Q", struct.pack("<d", y))[0] for y in ys]
        args = [a for pair in zip(bits, bits1) for a in (f"{pair[0]:016X}", f"{pair[1]:016X}")]
        p = subprocess.run([hexe, imgf, f"{entry:08X}"] + args, capture_output=True, text=True)
        crow = [l for l in p.stdout.splitlines() if l.startswith("R ")]
        if len(crow) != len(bits):
            print(f"  0x{entry:08X}: o C parou no caso {len(crow)}: {p.stdout.splitlines()[-1:] if p.stdout else p.returncode}")
            total_bad += 1
            continue
        bad = []
        for x, b, b1, row in zip(xs, bits, bits1, crow):
            # Unicorn: fld qword [arg]; e a função
            uc.mem_write(0x30400010, struct.pack("<Q", b))
            pro = b"\xd9\x2d" + struct.pack("<I", 0x30400000) + b"\xdd\x05" + struct.pack("<I", 0x30400010)
            uc.mem_write(SENTINEL + 0x100, pro)
            uc.reg_write(X.UC_X86_REG_FPSW, 0)
            uc.reg_write(X.UC_X86_REG_FPTAG, 0xFFFF)
            uc.reg_write(X.UC_X86_REG_MXCSR, 0x1F80)
            uc.emu_start(SENTINEL + 0x100, SENTINEL + 0x100 + len(pro))
            uc.reg_write(X.UC_X86_REG_XMM0, b)
            uc.reg_write(X.UC_X86_REG_XMM1, b1)
            esp = STACK_HI - 0x100 - 4
            uc.mem_write(esp, struct.pack("<I", SENTINEL))
            uc.reg_write(X.UC_X86_REG_ESP, esp)
            try:
                uc.emu_start(entry, SENTINEL, count=2000000)
            except UcError as ex:
                bad.append((x, f"Unicorn: {ex}"))
                continue
            if uc.reg_read(X.UC_X86_REG_EIP) != SENTINEL:
                bad.append((x, "Unicorn não voltou"))
                continue
            ux = uc.reg_read(X.UC_X86_REG_XMM0) & 0xFFFFFFFFFFFFFFFF
            uc.mem_write(0x30400020, b"\0" * 8)
            uc.mem_write(SENTINEL + 0x200, b"\xdd\x1d" + struct.pack("<I", 0x30400020))   # fstp qword
            top = (uc.reg_read(X.UC_X86_REG_FPSW) >> 11) & 7
            uc.emu_start(SENTINEL + 0x200, SENTINEL + 0x206)
            ust = struct.unpack("<Q", uc.mem_read(0x30400020, 8))[0]
            t = row.split()
            cx, cst, ctop = int(t[1], 16), int(t[2], 16), int(t[3])
            fx = lambda v: struct.unpack("<d", struct.pack("<Q", v))[0]
            same_x = ux == cx or (fx(ux) != fx(ux) and fx(cx) != fx(cx))
            same_s = ust == cst or (fx(ust) != fx(ust) and fx(cst) != fx(cst))
            if not same_x or not same_s or top != ctop:
                bad.append((x, f"xmm0 hw={fx(ux)!r} C={fx(cx)!r}; st0 hw={fx(ust)!r} C={fx(cst)!r}; TOP {top}/{ctop}"))
        guess = ""
        print(f"  0x{entry:08X}: {len(xs) - len(bad)}/{len(xs)} iguais{guess}")
        for x, why in bad[:4]:
            print(f"      x={x!r}: {why}")
        total_bad += bool(bad)
    sys.exit(1 if total_bad else 0)


if __name__ == "__main__":
    main()
