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
STUB, STUB_VAL = SENTINEL + 0x800, SENTINEL + 0xF00
STUB_RET = STUB + 6 + 5                     # depois do fld (6 bytes) e do call (5)
REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


class Case:
    def __init__(self, rng):
        self.regs = {r: rng.getrandbits(32) for r in REGS}
        self.regs["esp"] = STACK_HI - 0x400
        self.writes = []                     # (endereço, bytes)
        self.dumps = [(self.regs["esp"] - 0x140, 0x150)]   # pilha: pushes, locais e argumentos
        self.ignore = ()                     # registradores voláteis que o override não imita
        self.fld = None                      # double empilhado em st(0) antes da chamada (stub)

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


def case_memset(rng):
    """0x00E63D00: memset(dst, val, n) cdecl; o CRT suja ecx/edx (voláteis)."""
    k = Case(rng)
    dst = DATA + rng.randrange(0, 64)
    n = rng.choice((0, 1, 3, 4, 7, 255, 256, 257, rng.randrange(0, 4096), rng.randrange(0, 0x20000)))
    val = rng.choice((0, 0xFF, rng.getrandbits(32)))
    blob = os.urandom(0x21000)
    k.writes += [(DATA + i, blob[i:i + 0x4000]) for i in range(0, len(blob), 0x4000)]   # linha W < 64 KB
    for i, v in enumerate((SENTINEL, dst, val, n)):
        k.w32(k.regs["esp"] + 4 * i, v)
    k.dumps = [(DATA, 0x21000)]
    k.ignore = ("ecx", "edx")
    return k


def case_memmove(rng):
    """0x00E63EC0/0x00E69B90: memcpy/memmove(dst, src, n) com sobreposição nos dois sentidos."""
    k = Case(rng)
    n = rng.choice((0, 1, 2, 3, 4, 15, 16, 255, 256, 257, rng.randrange(0, 2048), rng.randrange(0, 0x8000)))
    src = DATA + 0x8100 + rng.randrange(0, 64)   # src - n - 8 e dst + n ficam dentro de DATA..DATA+0x21000
    dst = rng.choice((DATA + rng.randrange(0, 64), src + rng.randrange(-n - 8, n + 8), src))
    blob = os.urandom(0x21000)
    k.writes += [(DATA + i, blob[i:i + 0x4000]) for i in range(0, len(blob), 0x4000)]   # linha W < 64 KB
    for i, v in enumerate((SENTINEL, dst, src, n)):
        k.w32(k.regs["esp"] + 4 * i, v)
    k.dumps = [(DATA, 0x21000)]
    k.ignore = ("ecx", "edx")
    return k


def _ftol_value(rng):
    return rng.choice((0.0, -0.0, 0.5, -0.5, 0.999999, -0.999999, 1.5, -1.5, 2.5, -2.5,
                       2147483647.0, 2147483647.9, 2147483648.0, -2147483648.0, -2147483648.9, -2147483649.0,
                       1e10, -1e10, 9.2e18, -9.2e18, 1e19, -1e19, float("inf"), float("-inf"), float("nan"),
                       rng.uniform(-1e6, 1e6), rng.uniform(-3e9, 3e9), rng.uniform(-1e19, 1e19),
                       float(rng.randrange(-2**40, 2**40)) + rng.choice((0.25, 0.5, 0.75))))


def case_ftol_sse(rng):
    """0x00E63D80: _ftol2_sse, st(0) -> eax (cvttsd2si); com SSE2 o CRT não toca edx/ecx."""
    k = Case(rng)
    k.fld = _ftol_value(rng)
    k.dumps = []
    return k


def case_ftol(rng):
    """0x00E63DB6: _ftol2 x87, st(0) -> edx:eax truncado; ecx é lixo do CRT."""
    k = Case(rng)
    k.fld = _ftol_value(rng)
    k.dumps = []
    k.ignore = ("ecx",)
    return k


# O _ftol2_sse só faz cvttsd2si se o CRT já marcou o SSE2 (0x012B11AC): o Unicorn roda com a flag em 1.
SCENES = {0x006702B0: case_adpcm, 0x00CD5FB0: case_chain, 0x00E63D00: case_memset, 0x00E63EC0: case_memmove,
          0x00E69B90: case_memmove, 0x00E63DB6: case_ftol, 0x00E63D80: case_ftol_sse}
FLAG_SSE2 = 0x012B11AC

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
        } else if (line[0] == 'F') {
            unsigned long long b = strtoull(line + 2, NULL, 16);
            double v;
            memcpy(&v, &b, 8);
            fpush(c, v);
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
                cregs = dict(k.regs)
                writes = list(k.writes)
                if k.fld is not None:            # o stub faz call: o C vê o retorno já na pilha
                    cregs["esp"] -= 4
                    writes.append((cregs["esp"], struct.pack("<I", STUB_RET)))
                f.write("C " + " ".join(f"{cregs[n]:08X}" for n in REGS) + "\n")
                for a, b in writes:
                    f.write(f"W {a:08X} {b.hex()}\n")
                if k.fld is not None:
                    f.write(f"F {struct.unpack('<Q', struct.pack('<d', k.fld))[0]:016X}\n")
                f.write("G\n")
                for a, n in k.dumps:
                    f.write(f"D {a:08X} {n:X}\n")
        p = subprocess.run([hexe, imgf, f"{va:08X}", script], capture_output=True, text=True)
        out = p.stdout.splitlines()
        per = 1 + len(cases[0].dumps)
        assert all(len(k.dumps) == len(cases[0].dumps) for k in cases)
        bad = []
        for i, k in enumerate(cases):
            rows = out[i * per:(i + 1) * per] if len(k.dumps) + 1 == per else None
            uc.mem_write(DATA, zero_data)
            uc.mem_write(STACK_LO, zero_stack)
            for n in REGS:
                uc.reg_write(ureg[n], k.regs[n])
            for a, b in k.writes:
                uc.mem_write(a, b)
            uc.mem_write(FLAG_SSE2, struct.pack("<I", 1))
            uc.reg_write(X.UC_X86_REG_FPCW, 0x027F)
            uc.reg_write(X.UC_X86_REG_FPSW, 0)
            uc.reg_write(X.UC_X86_REG_FPTAG, 0xFFFF)
            start = va
            if k.fld is not None:
                # stub: fld qword [STUB_VAL]; call va; jmp SENTINEL
                uc.mem_write(STUB_VAL, struct.pack("<d", k.fld))
                code = b"\xDD\x05" + struct.pack("<I", STUB_VAL)
                code += b"\xE8" + struct.pack("<i", va - (STUB + len(code) + 5))
                assert STUB + len(code) == STUB_RET
                code += b"\xE9" + struct.pack("<i", SENTINEL - (STUB_RET + 5))
                if uc.mem_read(STUB, len(code)) != code:
                    uc.mem_write(STUB, code)
                    # O Unicorn guarda a tradução do stub: sem isto, a cena seguinte chamava o alvo antigo.
                    if hasattr(uc, "ctl_flush_tb"):
                        uc.ctl_flush_tb()
                    else:
                        uc.ctl_remove_cache(STUB, STUB + len(code))
                start = STUB
            try:
                uc.emu_start(start, SENTINEL, count=50_000_000)
            except UcError as ex:
                bad.append((i, f"Unicorn: {ex}"))
                continue
            want = ["R " + " ".join(f"{uc.reg_read(ureg[n]):08X}" for n in REGS)]
            if k.ignore and rows:                # voláteis: compara os outros
                rr, ww = rows[0].split(), want[0].split()
                for n in k.ignore:
                    j = 1 + REGS.index(n)
                    if j < len(rr):
                        rr[j] = ww[j] = "-"
                rows = [" ".join(rr)] + rows[1:]
                want = [" ".join(ww)] + want[1:]
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
