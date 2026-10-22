#!/usr/bin/env python3
"""Reconnaissance of SPEED2.EXE: imports, ISA usage, control-flow shapes.

Usage: python scripts/recon.py <SPEED2.analysis.exe> [functions.json] > out.json
Linear-sweep statistics are over .text as decoded from function starts when a
catalog is given (exact), otherwise over a plain linear sweep (approximate).
"""
import sys, json, collections, pefile, capstone
from capstone import x86 as X

exe = sys.argv[1]
cat = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else None
pe = pefile.PE(exe)
B = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
t0, t1 = B + text.VirtualAddress, B + text.VirtualAddress + text.Misc_VirtualSize

imports = collections.OrderedDict()
iat = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    dll = d.dll.decode()
    imports[dll] = []
    for i in d.imports:
        n = i.name.decode() if i.name else "#%d" % i.ordinal
        imports[dll].append(n); iat[i.address] = (dll, n)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
md.skipdata = True
mn = collections.Counter(); groups = collections.Counter(); prefixes = collections.Counter()
seg = collections.Counter(); calls = collections.Counter(); jmps = collections.Counter()
iatcalls = collections.Counter(); ninsn = 0; rare = collections.Counter()
FP = set(); MMX = set(); SSE = set(); SSE2 = set()
cls = collections.Counter()
covered = None
if cat:
    ranges = sorted((f["address"], f["end"]) for f in cat["functions"] if f["end"] > f["address"])
    merged = []
    for lo, hi in ranges:
        if merged and lo <= merged[-1][1]: merged[-1][1] = max(merged[-1][1], hi)
        else: merged.append([lo, hi])
else:
    merged = [[t0, t1]]
for lo, hi in merged:
    for ins in md.disasm(img[lo - B:hi - B], lo):
        if ins.mnemonic == ".byte": continue
        ninsn += 1
        m = ins.mnemonic
        mn[m] += 1
        gs = set(ins.groups)
        if X.X86_GRP_FPU in gs or m.startswith("f"): cls["x87"] += 1
        if X.X86_GRP_MMX in gs or any(o.type == X.X86_OP_REG and ins.reg_name(o.reg).startswith("mm") for o in ins.operands): cls["mmx"] += 1
        if X.X86_GRP_SSE1 in gs: cls["sse"] += 1
        if X.X86_GRP_SSE2 in gs: cls["sse2"] += 1
        if X.X86_GRP_3DNOW in gs: cls["3dnow"] += 1
        p = ins.prefix
        if p[0] == 0xF0: prefixes["lock"] += 1
        if p[0] == 0xF3: prefixes["rep/repe"] += 1
        if p[0] == 0xF2: prefixes["repne"] += 1
        if p[1] == 0x64: seg["fs"] += 1
        if p[1] == 0x65: seg["gs"] += 1
        if p[1] in (0x2E, 0x36, 0x3E, 0x26): seg["cs/ss/ds/es"] += 1
        if p[2] == 0x66: prefixes["opsize16"] += 1
        if m in ("cpuid", "rdtsc", "int", "int3", "into", "in", "out", "hlt", "sysenter", "ud2", "iretd", "lds", "les", "lfs", "lgs", "lss", "arpl", "bound", "aaa", "aas", "daa", "das", "aam", "aad", "xlatb", "enter", "cmpxchg8b", "fxsave", "fxrstor", "emms", "femms", "pushal", "popal", "std"):
            rare[m] += 1
        if m == "call":
            o = ins.operands[0]
            if o.type == X.X86_OP_IMM: calls["direct"] += 1
            elif o.type == X.X86_OP_REG: calls["reg"] += 1
            else:
                if o.mem.base == 0 and o.mem.index == 0 and o.mem.disp in iat:
                    calls["iat"] += 1; iatcalls["%s!%s" % iat[o.mem.disp]] += 1
                elif o.mem.base and o.mem.index == 0: calls["mem[reg+disp] (vtable/COM)"] += 1
                else: calls["mem other"] += 1
        if m == "jmp":
            o = ins.operands[0]
            if o.type == X.X86_OP_IMM: jmps["direct"] += 1
            elif o.type == X.X86_OP_REG: jmps["reg"] += 1
            else:
                if o.mem.index and o.mem.scale == 4: jmps["table [idx*4+disp]"] += 1
                elif o.mem.base == 0 and o.mem.disp in iat: jmps["iat thunk"] += 1
                else: jmps["mem other"] += 1
out = dict(text=[t0, t1], decoded_ranges=len(merged), instructions=ninsn,
           imports=imports, import_count=sum(len(v) for v in imports.values()),
           classes=dict(cls), prefixes=dict(prefixes), segments=dict(seg), rare=dict(rare),
           calls=dict(calls), jmps=dict(jmps), iat_call_sites=dict(iatcalls.most_common()),
           top_mnemonics=mn.most_common(80),
           fpu_mnemonics=sorted([(k, v) for k, v in mn.items() if k.startswith("f")], key=lambda x: -x[1]),
           simd_mnemonics=sorted([(k, v) for k, v in mn.items() if any(k.startswith(p) for p in ("p", "mov", "cvt", "add", "sub", "mul", "div", "sqrt", "rsqrt", "rcp", "min", "max", "and", "or", "xor", "shuf", "unpck", "comis", "ucomis", "cmp", "ldmxcsr", "stmxcsr", "emms", "prefetch", "movnt")) and k not in ("mov", "movzx", "movsx", "movsb", "movsd", "movsw", "add", "sub", "and", "or", "xor", "cmp", "push", "pop", "pushal", "popal", "pushfd", "popfd", "cmpsb", "cmpsd", "cmpsw", "cmpxchg", "mul", "div", "pause")], key=lambda x: -x[1]))
json.dump(out, sys.stdout, indent=1)
