#!/usr/bin/env python3
"""Teste diferencial dos overrides com memória (fase P5): Unicorn x C de overrides.c.

O difftest_func.py compara funções de número (xmm0/st(0) na volta). Os overrides novos são
folhas que leem e escrevem estruturas na memória do guest, então aqui cada caso monta uma
cena (registradores, bytes na área de dados e na pilha), roda a função ORIGINAL do
re5dx9.clean.exe no Unicorn e o corpo de runtime/src/core/overrides.c no host, e compara os
registradores de volta (eax..edi, esp) e as regiões de memória que a função pode tocar,
inclusive a pilha abaixo do esp. Flags não entram: o lift as trata como mortas no `ret`.

    python scripts/difftest_override.py              # todos os overrides com cena
    python scripts/difftest_override.py 0x006702B0 --cases 3000

Sai com 77 (pulado) sem o binário ou sem o unicorn, como o difftest_cpu.
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
EXE = os.path.join(ROOT, "bin_input", "re5dx9.clean.exe")
DATA, DATA_SIZE = 0x20000000, 0x00100000
STACK_LO, STACK_HI = 0x30000000, 0x30100000
SENTINEL = 0x30200000
REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


class Case:
    def __init__(self, rng):
        self.regs = {r: rng.getrandbits(32) for r in REGS}
        self.regs["esp"] = STACK_HI - 0x400
        self.writes = []                     # (endereço, bytes)
        self.dumps = [(self.regs["esp"] - 0x140, 0x150)]   # pilha: pushes, locais e argumentos

    def w32(self, a, v):
        self.writes.append((a, struct.pack("<I", v & 0xFFFFFFFF)))


def case_adpcm(rng):
    """0x006702B0: eax = amostras, ecx -> [destino], [esp+4] = entrada, [esp+8] = estado."""
    k = Case(rng)
    st, obj, src, dst = DATA, DATA + 0x100, DATA + 0x1000, DATA + 0x4000
    flag = rng.random() < 0.5
    n = rng.choice((0, 1, 2, 3, rng.randrange(1, 64), rng.randrange(1, 2000)))
    if flag and n == 0:
        n = 1                                # n = 0 com meio byte pendente: laço de 2^31 nos dois
    pred = rng.choice((rng.randrange(-32768, 32768), rng.getrandbits(32), 0, 0x7FFFFFFF))
    idx = rng.choice((rng.randrange(0, 89), 0, 88, rng.randrange(0, 89)))
    k.writes.append((st, os.urandom(0x40)))
    k.w32(st, pred)
    k.w32(st + 0x18, idx)
    k.w32(st + 0x30, rng.getrandbits(32) if rng.random() < 0.3 else rng.getrandbits(8))
    k.w32(st + 0x34, rng.choice((1, 2, 0x80000000)) if flag else 0)
    k.w32(obj, dst)
    k.writes.append((src, bytes(rng.getrandbits(8) for _ in range(n // 2 + 2))))
    k.regs.update(eax=n, ecx=obj)
    esp = k.regs["esp"]
    k.w32(esp, SENTINEL)
    k.w32(esp + 4, src)
    k.w32(esp + 8, st)
    k.dumps += [(st, 0x40), (obj, 4), (dst, 4 * n + 16)]
    return k


def case_chain(rng):
    """0x00CD5FB0: ecx = nó; a cadeia de pais segue +0x144 até 0."""
    k = Case(rng)
    m = rng.choice((1, 1, 2, 3, rng.randrange(1, 20), rng.randrange(1, 60)))
    order = list(range(m))
    rng.shuffle(order)                       # nós fora de ordem na memória
    addr = [DATA + 0x200 * o for o in order]
    for j, a in enumerate(addr):
        node = bytearray(os.urandom(0x200))
        struct.pack_into("<I", node, 0x144, addr[j + 1] if j + 1 < m else 0)
        if rng.random() < 0.3:
            node[0x19B] = rng.choice((0, 255, 128))
            node[0x37] = rng.choice((0, 255, 1))
        k.writes.append((a, bytes(node)))
    k.regs["ecx"] = addr[0]
    k.w32(k.regs["esp"], SENTINEL)
    k.dumps += [(DATA, 0x200 * m)]
    return k


SCENES = {0x006702B0: case_adpcm, 0x00CD5FB0: case_chain}

HARNESS = r"""#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "%(overrides)s"
uint32_t g_image_delta;
#if !RE5_GUEST_BASE_FIXED
uintptr_t re5_guest_base;
#endif
void re5_fatal(const char *ch, const char *fmt, ...) { (void)ch; printf("X %%s\n", fmt); exit(3); }

int main(int argc, char **argv)
{
    size_t size = %(size)d;
#if RE5_GUEST_BASE_FIXED
    uint8_t *mem = VirtualAlloc((void *)re5_guest_base, 0x31000000u, MEM_RESERVE, PAGE_NOACCESS);
    if (mem != (uint8_t *)re5_guest_base) return 2;
#else
    uint8_t *mem = VirtualAlloc(NULL, 0x31000000u, MEM_RESERVE, PAGE_NOACCESS);
    if (!mem) return 2;
    re5_guest_base = (uintptr_t)mem;
#endif
    if (!VirtualAlloc(mem + 0x00400000u, size, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(mem + %(data)du, %(data_size)du, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(mem + %(stack_lo)du, %(stack_size)du, MEM_COMMIT, PAGE_READWRITE)) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (fread(mem + 0x00400000u, 1, size, f) != size) return 2;
    fclose(f);
    re5_lifted_fn fn = re5_override_get((uint32_t)strtoul(argv[2], NULL, 16));
    if (!fn) return 5;
    FILE *in = fopen(argv[3], "r");
    char line[1 << 16];
    CPU cpu, *c = &cpu;
    static unsigned char hex[256];
    for (int i = 0; i < 10; i++) hex['0' + i] = (unsigned char)i;
    for (int i = 0; i < 6; i++) hex['A' + i] = hex['a' + i] = (unsigned char)(10 + i);
    while (fgets(line, sizeof line, in)) {
        char *p = line;
        if (line[0] == 'C') {
            memset(mem + %(data)du, 0, %(data_size)du);
            memset(mem + %(stack_lo)du, 0, %(stack_size)du);
            memset(&cpu, 0, sizeof cpu);
            c->fpu_cw = 0x027F; c->mxcsr = 0x1F80;
            uint32_t r[8];
            sscanf(line + 2, "%%x %%x %%x %%x %%x %%x %%x %%x", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7]);
            c->eax = r[0]; c->ecx = r[1]; c->edx = r[2]; c->ebx = r[3];
            c->esp = r[4]; c->ebp = r[5]; c->esi = r[6]; c->edi = r[7];
        } else if (line[0] == 'W') {
            uint32_t a = (uint32_t)strtoul(line + 2, &p, 16);
            for (p++; p[0] && p[1] && p[0] != '\n'; p += 2) mem[a++] = (uint8_t)(hex[(unsigned char)p[0]] << 4 | hex[(unsigned char)p[1]]);
        } else if (line[0] == 'G') {
            fn(c);
            printf("R %%08X %%08X %%08X %%08X %%08X %%08X %%08X %%08X\n", c->eax, c->ecx, c->edx, c->ebx,
                   c->esp, c->ebp, c->esi, c->edi);
        } else if (line[0] == 'D') {
            uint32_t a = (uint32_t)strtoul(line + 2, &p, 16), n = (uint32_t)strtoul(p, NULL, 16);
            printf("M ");
            for (uint32_t i = 0; i < n; i++) printf("%%02X", mem[a + i]);
            printf("\n");
        }
    }
    return 0;
}
"""


def main():
    try:
        import pefile
        from unicorn import UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, Uc, UcError
        from unicorn import x86_const as X
    except ImportError as ex:
        print(f"[-] pulado: {ex}")
        sys.exit(77)
    if not os.path.exists(EXE):
        print(f"[-] pulado: falta {EXE}")
        sys.exit(77)
    args = sys.argv[1:]
    ncases = 1500
    if "--cases" in args:
        i = args.index("--cases")
        ncases = int(args[i + 1])
        del args[i:i + 2]
    vas = [int(a, 16) for a in args] or list(SCENES)

    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    work = tempfile.mkdtemp(prefix="difftest_override_")
    imgf = os.path.join(work, "img.bin")
    open(imgf, "wb").write(bytes(img))
    cfile = os.path.join(work, "h.c")
    open(cfile, "w").write(HARNESS % dict(
        overrides=os.path.join(ROOT, "runtime", "src", "core", "overrides.c").replace("\\", "/"),
        size=len(img), data=DATA, data_size=DATA_SIZE, stack_lo=STACK_LO, stack_size=STACK_HI - STACK_LO))
    gen = os.path.join(ROOT, "build", os.environ.get("RE5_BUILD", "windows-x64"), "gen_include")
    hexe = os.path.join(work, "h.exe")
    r = subprocess.run([os.environ.get("RECOMP_CC", "gcc"), "-O2", "-w", "-fno-strict-aliasing", "-fwrapv",
                        "-I", gen, "-I", os.path.join(ROOT, "runtime", "include"), cfile, "-o", hexe],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:3000])
        sys.exit("[!] não compilou")

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, (len(img) + 0xFFF) & ~0xFFF, UC_PROT_ALL)
    uc.mem_write(base, bytes(img))
    uc.mem_map(DATA, DATA_SIZE)
    uc.mem_map(STACK_LO, STACK_HI - STACK_LO)
    uc.mem_map(SENTINEL, 0x1000)
    ureg = {n: getattr(X, f"UC_X86_REG_{n.upper()}") for n in REGS}
    zero_data, zero_stack = b"\0" * DATA_SIZE, b"\0" * (STACK_HI - STACK_LO)

    bad_total = 0
    for va in vas:
        rng = random.Random(va)
        cases = [SCENES[va](rng) for _ in range(ncases)]
        script = os.path.join(work, f"{va:08X}.txt")
        with open(script, "w") as f:
            for k in cases:
                f.write("C " + " ".join(f"{k.regs[n]:08X}" for n in REGS) + "\n")
                for a, b in k.writes:
                    f.write(f"W {a:08X} {b.hex()}\n")
                f.write("G\n")
                for a, n in k.dumps:
                    f.write(f"D {a:08X} {n:X}\n")
        p = subprocess.run([hexe, imgf, f"{va:08X}", script], capture_output=True, text=True)
        out = p.stdout.splitlines()
        per = 1 + len(cases[0].dumps)
        bad = []
        for i, k in enumerate(cases):
            rows = out[i * per:(i + 1) * per] if len(k.dumps) + 1 == per else None
            uc.mem_write(DATA, zero_data)
            uc.mem_write(STACK_LO, zero_stack)
            for n in REGS:
                uc.reg_write(ureg[n], k.regs[n])
            for a, b in k.writes:
                uc.mem_write(a, b)
            try:
                uc.emu_start(va, SENTINEL, count=50_000_000)
            except UcError as ex:
                bad.append((i, f"Unicorn: {ex}"))
                continue
            want = ["R " + " ".join(f"{uc.reg_read(ureg[n]):08X}" for n in REGS)]
            want += ["M " + bytes(uc.mem_read(a, n)).hex().upper() for a, n in k.dumps]
            if rows is None or len(rows) < per:
                bad.append((i, f"o C parou (saída {p.returncode}: {out[-1:] if out else ''})"))
                break
            if rows != want:
                diff = next(j for j in range(per) if rows[j] != want[j])
                what = "registradores" if diff == 0 else f"memória 0x{k.dumps[diff - 1][0]:08X}"
                bad.append((i, f"{what}\n        Unicorn {want[diff][:160]}\n        C       {rows[diff][:160]}"))
        print(f"  0x{va:08X}: {len(cases) - len(bad)}/{len(cases)} iguais")
        for i, why in bad[:3]:
            print(f"      caso {i}: {why}")
        bad_total += bool(bad)
    sys.exit(1 if bad_total else 0)


if __name__ == "__main__":
    main()
