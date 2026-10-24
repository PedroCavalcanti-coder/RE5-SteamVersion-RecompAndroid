#!/usr/bin/env python3
"""
lift32_cpu.py - mechanical x86-32 -> C static recompiler, CPU-struct model.

Disassembles a function (capstone) and emits `void L_<addr>(CPU *c)` against the
runtime in runtime/recomp32_cpu/cpu.h, one C statement per x86 instruction.
Calls and returns are modelled on the emulated stack exactly like x86 (the
caller pushes a return slot, the callee's `ret` pops it), so stack layout and
argument access match the original byte for byte.

How this differs from lift32.py (the other x86-32 lifter here):

  lift32.py       global registers (g_eax), PUSH32/MEM32 macros, RECOMP_CALL
                  dispatch. Simple and fast to read; one implicit machine.
  lift32_cpu.py   explicit `CPU *c` passed down. Reentrant - several machine
                  states can be live at once, which is what a HYBRID build needs
                  when real library code calls back into lifted code while an
                  outer lifted call is still on the stack (runtime/hybrid).

Also here and not in lift32.py:

  * `.reloc`-driven relocation. Address immediates AND absolute displacements
    inside memory operands are wrapped in GVA() so the output is correct at any
    load base. `mov dl, [ecx + 0x56d902]` is a table lookup at an absolute
    address just as much as `mov eax, [0x58d428]` is - the presence of a base
    register says nothing about it, and the reloc table is the authority.
  * `fs:` segment access -> FS_RD*/FS_WR* macros (cpu.h), so SEH prologues work
    on MSVC (real segment) and on gcc/clang (a host-provided simulated TIB at
    c->fs_base) from the same lift.
  * x87 FPU as a register stack of doubles.

Usage:
  py -3.11 lift32_cpu.py <pe-file> <funcs.txt> <out.c> [0xADDR ...]

funcs.txt lines: "0xADDR  size  name" (IDA/Ghidra export), used for bounds.
With no addresses given, lifts every function in funcs.txt.
"""
import os, sys, re
from capstone import *
from capstone.x86 import *

IMAGE_BASE = 0x11000000   # overridden from the PE's ImageBase in main()

# ---- register field mapping ----
R32 = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"}
R16 = {"ax":"eax","cx":"ecx","dx":"edx","bx":"ebx","sp":"esp","bp":"ebp","si":"esi","di":"edi"}
R8L = {"al":"eax","cl":"ecx","dl":"edx","bl":"ebx"}
R8H = {"ah":"eax","ch":"ecx","dh":"edx","bh":"ebx"}
# Segment registers. A flat PE never touches these, but segmented 32-bit code
# does - IR32.DLL's decode core loads DS and ES constantly - and without them
# every function containing one fails to lift at all. See cpu.h: they are
# storage only, and do not take part in addressing.
SEG = {"cs","ds","es","fs","gs","ss"}

# String instructions, and the prefixes that may legitimately precede one. An
# F3/F2 prefix on anything else is not a REP - see translate().
REP_PREFIXES = frozenset(("rep", "repe", "repz", "repne", "repnz"))
STRING_OPS = frozenset((
    "movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
    "scasb", "scasw", "scasd", "lodsb", "lodsw", "lodsd",
    "cmpsb", "cmpsw", "cmpsd", "insb", "insw", "insd",
    "outsb", "outsw", "outsd",
))

# ---- SSE ----
# Scalar ops, by mnemonic, to the C operator they are. The packed forms of
# these are deliberately absent: they need per-lane code and are rare enough in
# real binaries that a TODO is better than a plausible-looking wrong lane.
SSE_ARITH = {"add": "+", "sub": "-", "mul": "*", "div": "/"}

# Bitwise ops on the whole 128 bits. andn is `~dst & src`, note the order.
SSE_BITWISE = {
    "andps": "&", "andpd": "&", "pand": "&",
    "orps":  "|", "orpd":  "|", "por":  "|",
    "xorps": "^", "xorpd": "^", "pxor": "^",
    "andnps": "andn", "andnpd": "andn", "pandn": "andn",
}

# CMPccSS/SD. These do not set flags - they write an all-ones or all-zeros mask
# into the destination lane, so the result can be ANDed as a branchless select.
# Every predicate is written so C's "any comparison with NaN is false" rule
# gives the unordered behaviour the hardware has.
SSE_CMP_PRED = {
    "eq":    "(_a == _b)",
    "lt":    "(_a < _b)",
    "le":    "(_a <= _b)",
    "unord": "(_a != _a || _b != _b)",
    "neq":   "!(_a == _b)",
    "nlt":   "!(_a < _b)",
    "nle":   "!(_a <= _b)",
    "ord":   "!(_a != _a || _b != _b)",
}
SSE_CMP_RE = re.compile(r"^cmp(%s)(ss|sd|ps|pd)$" % "|".join(SSE_CMP_PRED))

# Packed integer arithmetic on xmm. (C operator, lane suffix, lanes). These
# wrap, which is what C's unsigned arithmetic does anyway, so the operator is
# the whole translation.
SSE_PINT = {}
for _op, _c in (("padd", "+"), ("psub", "-")):
    for _w, _bits in (("b", 8), ("w", 16), ("d", 32), ("q", 64)):
        SSE_PINT[_op + _w] = (_c, _bits, 128 // _bits)

# The saturating forms of the same two. (signed?, lane bits, helper).
SSE_PSAT = {}
for _op, _c in (("padd", "+"), ("psub", "-")):
    for _sgn, _pre in (("s", "i"), ("us", "u")):
        for _w, _bits in (("b", 8), ("w", 16)):
            SSE_PSAT[_op + _sgn + _w] = (_c, _bits, "sse_sat_%s%d" % (_pre, _bits))

# Packed min/max: (lane field, lanes, comparison that keeps the destination).
SSE_PMINMAX = {
    "pmaxsw": ("i16", 8, ">"), "pminsw": ("i16", 8, "<"),
    "pmaxub": ("u8", 16, ">"), "pminub": ("u8", 16, "<"),
}

# Packed compares. These write a lane-wide mask, not flags. pcmpgt is signed;
# pcmpeq does not care either way.
SSE_PCMP = {}
for _op, _c, _sgn in (("pcmpeq", "==", "u"), ("pcmpgt", ">", "i")):
    for _w, _bits in (("b", 8), ("w", 16), ("d", 32)):
        SSE_PCMP[_op + _w] = (_c, _bits, _sgn)

# Pack: two lanes narrow into one, saturating. (source bits, helper, signed
# source?) -- the destination half comes from the destination, the other from
# the source, so both need reading before either is written.
SSE_PACK = {
    "packsswb": (16, "sse_sat_i8",  "i"),
    "packuswb": (16, "sse_sat_u8",  "i"),
    "packssdw": (32, "sse_sat_i16", "i"),
}

# Unpack: interleave the low (or high) half of each operand. (lane bits, high?)
SSE_PUNPCK = {}
for _half, _hi in (("l", False), ("h", True)):
    for _w, _bits in (("bw", 8), ("wd", 16), ("dq", 32), ("qdq", 64)):
        SSE_PUNPCK["punpck" + _half + _w] = (_bits, _hi)

# Moves of the whole register. Aligned and unaligned differ only in whether the
# hardware faults on a misaligned address; the movnt* forms differ only in a
# cache hint, which a host that is not managing the guest cache can ignore.
# They are all a 128-bit copy here.
SSE_MOV128 = frozenset({"movaps", "movups", "movapd", "movupd", "movdqa", "movdqu",
                        "movntps", "movntpd", "movntdq"})

# Mnemonics that are SSE only when an XMM register is involved. `movsd` is also
# "move string dword"; `movq`/`movd` and every `p*` op below are also MMX, where
# the operands are mm0-7 and 64 bits wide. Routing these by name alone
# mistranslates string loops and MMX code into nonsense that still compiles.
SSE_AMBIGUOUS = frozenset({"movsd", "movq", "movd", "pxor", "pand", "por", "pandn",
                           "pmullw", "pmaddwd", "pmovmskb",
                           "packsswb", "packuswb", "packssdw"})

# psll*/psrl*/psra*: (C operator, lane width in bits, arithmetic?). pslldq and
# psrldq are not here - they shift the register by bytes, not each lane by
# bits, and are handled on their own.
SSE_SHIFT = {
    "psllw": ("<<", 16, False), "pslld": ("<<", 32, False),
    "psllq": ("<<", 64, False),
    "psrlw": (">>", 16, False), "psrld": (">>", 32, False),
    "psrlq": (">>", 64, False),
    "psraw": (">>", 16, True),  "psrad": (">>", 32, True),
}

SSE_AMBIGUOUS = (SSE_AMBIGUOUS | frozenset(SSE_PINT) | frozenset(SSE_PSAT)
                 | frozenset(SSE_PCMP) | frozenset(SSE_PMINMAX)
                 | frozenset(SSE_SHIFT)
                 | frozenset(k for k in SSE_PUNPCK if k != "punpcklqdq"
                                                   and k != "punpckhqdq"))

SSE_MNEMONICS = (
    SSE_MOV128 | SSE_AMBIGUOUS | frozenset(SSE_BITWISE)
    | frozenset({"movss", "sqrtss", "sqrtsd", "sqrtps",
                 "rcpss", "rsqrtss", "rcpps", "rsqrtps",
                 "shufps", "unpcklps", "unpckhps",
                 "minps", "maxps", "minpd", "maxpd", "minss", "maxss", "minsd", "maxsd",
                 "ucomiss", "comiss", "ucomisd", "comisd",
                 "cvtsi2ss", "cvtsi2sd", "cvttss2si", "cvttsd2si",
                 "cvtss2si", "cvtsd2si", "cvtss2sd", "cvtsd2ss",
                 "cvtdq2ps", "cvtps2dq", "cvttps2dq",
                 "cvtps2pd", "cvtpd2ps",
                 "pextrw", "pinsrw",
                 "unpcklpd", "unpckhpd", "unpcklps", "unpckhps",
                 "sqrtps", "sqrtpd",
                 "minps", "maxps", "minpd", "maxpd",
                 "shufps", "shufpd", "pshufd",
                 "psllw", "pslld", "psllq", "pslldq",
                 "psrlw", "psrld", "psrlq", "psrldq",
                 "psraw", "psrad",
                 "pmullw", "pmaddwd", "pmovmskb", "movmskps", "movmskpd",
                 "pshuflw", "pshufhw", "cvtdq2pd",
                 "movlps", "movlpd", "movhps", "movhpd"})
    | frozenset(SSE_PINT) | frozenset(SSE_PSAT) | frozenset(SSE_PCMP)
    | frozenset(SSE_PACK) | frozenset(SSE_PUNPCK)
    | frozenset(o + w for o in SSE_ARITH for w in ("ps", "pd"))
    | frozenset(o + w for o in SSE_ARITH for w in ("ss", "sd"))
)

def _disasm(md, code, va):
    """md.disasm(), going on through the hint NOPs capstone rejects.

    `0F 18 /r` with a register operand is a reserved hint NOP (see
    disasm32.hint_nop); `0F 1F /0` has the same length and capstone takes it.
    """
    code, off = bytes(code), 0
    while off < len(code):
        for ins in md.disasm(code[off:], va + off):
            off += ins.size
            yield ins
        b = code[off:off + 3]
        if len(b) < 3 or b[0] != 0x0F or b[1] != 0x18 or b[2] < 0xC0:
            return
        code = code[:off] + bytes((0x0F, 0x1F, b[2] & 0xC7)) + code[off + 3:]


def reg_read(name):
    if name in R32: return f"c->{name}"
    if name in R16: return f"R16(c->{R16[name]})"
    if name in R8L: return f"R8L(c->{R8L[name]})"
    if name in R8H: return f"R8H(c->{R8H[name]})"
    if name in SEG: return f"c->{name}"
    raise NotImplementedError(f"reg_read {name}")

def reg_write(name, val):
    if name in R32: return f"c->{name} = (uint32_t)({val});"
    if name in R16: return f"SET16(c->{R16[name]}, {val});"
    if name in R8L: return f"SET8L(c->{R8L[name]}, {val});"
    if name in R8H: return f"SET8H(c->{R8H[name]}, {val});"
    if name in SEG: return f"c->{name} = (uint16_t)({val});"
    raise NotImplementedError(f"reg_write {name}")

def reg_size(name):
    if name in R32: return 4
    if name in R16 or name in SEG: return 2
    return 1

def _todo(va, what):
    """An instruction the lifter cannot express.

    A bare `abort()` in two million lines of generated C says nothing at all -
    and in a release build MSVC turns it into `__fastfail`, which no exception
    handler sees, so the process vanishes with 0xC0000409 and an empty log. It
    took a memory-mapped dispatch trail to find the last one.

    RECOMP_TODO carries the address and the mnemonic to whatever the runtime
    wants to do with them. cpu.h defines it as plain abort() unless the runtime
    says otherwise, so nothing that already includes cpu.h changes.
    """
    return 'RECOMP_TODO(0x%08X, "%s");' % (va, what.replace('"', "'").strip())


# ---- flag liveness -------------------------------------------------------------
#
# Every add/sub/cmp/test/shift computes six flags through a cpu.h helper, and in
# compiled code the next flag reader is almost always a jcc right after a cmp - the
# rest are overwritten unread. lift_function runs a backward liveness over the
# emitted C and hands each helper the mask of flags something can still read
# (flags_sub_m & co.). Everything is derived from the C text, not from tables of
# x86 semantics, so the analysis agrees with what the model actually does:
#   uses  = every `c->xf` that is not an assignment, plus the flags the cpu.h
#           helpers it calls read internally (parsed from cpu.h itself);
#   kills = only definite writes: an unconditional masked-family helper, a shift by
#           a nonzero constant, a plain `c->xf = ...`, or a call (the callee leaves
#           the flags it leaves, and its own `ret` keeps them all live).
# Leaving the function any other way (ret, jmp/jcc/fallthrough to another catalog
# entry, dispatch, abort) keeps all flags live: a catalog entry can be the middle of
# a real function that branches on flags set before the jump.
FLAG_BITS = {"cf": 0x001, "pf": 0x004, "af": 0x010, "zf": 0x040, "sf": 0x080, "of": 0x800}
ALL_FLAGS = 0x8D5
_FLAG_READ = re.compile(r"c->(cf|zf|sf|of|pf|af)\b(?!\s*=(?!=))")
_FLAG_WRITE = re.compile(r"(?:^|[;{]\s*)c->(cf|zf|sf|of|pf|af)\s*=(?!=)")
_GOTO = re.compile(r"goto L_([0-9A-F]{8});")
_CALLS = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# helper -> (flags it always writes, index of its count argument or None)
_MASKED = {
    "flags_add": (ALL_FLAGS, None), "flags_sub": (ALL_FLAGS, None),
    "flags_adc": (ALL_FLAGS, None), "flags_sbb": (ALL_FLAGS, None),
    "flags_logicz": (0x8C5, None),   # cf of sf zf pf (af is left alone)
    "op_shl": (0x0C5, 2), "op_shr": (0x0C5, 2), "op_sar": (0x0C5, 2),   # cf zf sf pf
}


def _bits(names):
    v = 0
    for n in names:
        v |= FLAG_BITS[n]
    return v


def _helper_reads():
    """Flags each cpu.h helper reads, directly or through the helpers it calls."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime", "recomp32_cpu", "cpu.h")
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return None
    bodies = {}
    for m in re.finditer(r"static inline [^;{(]*?\b(\w+)\s*\(([^)]*)\)\s*\{", text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        bodies[m.group(1)] = text[m.end():i]
    reads = {n: _bits(_FLAG_READ.findall(b)) for n, b in bodies.items()}
    for _ in range(8):                                   # transitive closure
        for n, b in bodies.items():
            for callee in _CALLS.findall(b):
                if callee in reads and callee != n:
                    reads[n] |= reads[callee]
    return reads


_HELPER_READS = _helper_reads()


def _uses(text):
    if _HELPER_READS is None:
        return ALL_FLAGS
    v = _bits(_FLAG_READ.findall(text))
    for name in _CALLS.findall(text):
        v |= _HELPER_READS.get(name, 0)
    return v


def _call_args(text, start):
    """Arguments of the call whose '(' is at text[start]; None if unbalanced."""
    depth, i, args, cur = 0, start, [], ""
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
            if depth > 1:
                cur += ch
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args, i
            cur += ch
        elif ch == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
        i += 1
    return None


def _kills(mnem, text):
    if mnem == "call":
        return ALL_FLAGS
    if any(k in text for k in ("while", "if (", "?", "for (")):
        return 0                       # conditional: may write nothing
    v = _bits(_FLAG_WRITE.findall(text))
    for m in re.finditer(r"\b(%s)\(" % "|".join(_MASKED), text):
        defs, cnt_i = _MASKED[m.group(1)]
        if cnt_i is not None:
            parsed = _call_args(text, m.end() - 1)
            if not parsed:
                continue
            arg = parsed[0][cnt_i] if len(parsed[0]) > cnt_i else ""
            try:
                if int(arg.rstrip("uU"), 0) & 31 == 0:
                    continue
            except ValueError:
                continue               # variable count: a zero count writes nothing
        v |= defs
    return v


def _apply_masks(text, mask):
    """Give every masked-family helper in `text` the flags that are live."""
    if mask == ALL_FLAGS:
        return text
    return re.sub(r"\b(%s)\(c, " % "|".join(_MASKED), lambda m: f"{m.group(1)}_m(c, 0x{mask:03X}u, ", text)


def flag_liveness(items, labels, dead_at_ret=False):
    """items: [(insn, [C lines])]. Returns the live-out flag mask of each item.

    dead_at_ret: the caller never reads flags this function leaves (true of compiled
    code; a hand-written CRT routine can return a result in ZF), so `ret` keeps none."""
    n = len(items)
    index = {ins.address: i for i, (ins, _) in enumerate(items)}
    succ, use, kill = [], [], []
    for i, (ins, lines) in enumerate(items):
        text = " ".join(lines)
        mnem = ins.mnemonic.split()[-1]
        s, leaves = [], False
        for t in _GOTO.findall(text):
            a = int(t, 16)
            if a in index:
                s.append(index[a])
            else:
                leaves = True
        if mnem in ("ret", "retn") and dead_at_ret and "dispatch" not in text and "RECOMP_TODO" not in text:
            pass                       # returns to a caller that rewrites flags before reading any
        elif mnem != "call" and ("dispatch" in text or "return" in text or "RECOMP_TODO" in text):
            leaves = True
        stops = mnem in ("jmp", "ret", "retn", "retf", "iret", "iretd", "hlt")
        if not stops:
            if i + 1 < n:
                s.append(i + 1)
            else:
                leaves = True
        succ.append((s, leaves))
        use.append(_uses(text))
        kill.append(_kills(mnem, text))
    live_in = [0] * n
    live_out = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            s, leaves = succ[i]
            out = ALL_FLAGS if leaves else 0
            for j in s:
                out |= live_in[j]
            inn = use[i] | (out & ~kill[i])
            if out != live_out[i] or inn != live_in[i]:
                live_out[i], live_in[i] = out, inn
                changed = True
    flag_liveness.succ = succ           # the CFG, for cache_registers
    return [live_out[i] | use[i] for i in range(n)]


# ---- registers in locals --------------------------------------------------------
#
# The GPRs and XMM registers live in the CPU struct, and GCC cannot prove that a
# store to guest memory (a pointer computed from an integer) does not write it: after
# every wr32 it reloaded esi/edi and every xmm from the struct, so a mulss became
# load/mulss/store. Inside a function they become locals (R_eax, R_x0...), loaded at
# entry. The struct is brought up to date only where something outside the function
# body can look at it - a call into the runtime or another lifted function, a
# return, a helper that takes the CPU and touches registers - and only for the
# registers written since the last sync (forward "dirty" dataflow over the same CFG
# as the flag liveness). After such a call every register is reloaded; the compiler
# drops the loads nobody uses.
CACHED = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi") + tuple("x%d" % i for i in range(8)) + ("cf", "pf", "af", "zf", "sf", "of")
_REG_REF = re.compile(r"\bc->(eax|ecx|edx|ebx|esp|ebp|esi|edi|cf|pf|af|zf|sf|of)\b")
_XMM_REF = re.compile(r"\bc->xmm\[([0-7])\]")
_LOCAL = re.compile(r"\bR_(eax|ecx|edx|ebx|esp|ebp|esi|edi|x[0-7]|cf|pf|af|zf|sf|of)\b")
_ASSIGN = re.compile(r"\s*(?:\.\w+\s*\[[^\]]*\]|\.\w+|\[[^\]]*\])*\s*(=(?!=)|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)")
# Calls that see the whole CPU: everything must be in the struct before, reloaded after.
_FULL_SYNC = ("dispatch", "dispatch_jmp", "re5_override", "recomp_cpuid", "recomp_rdtsc", "RECOMP_TODO",
              "re5_recomp_todo", "RE5_CALL")
# Helpers that read/write some registers through the CPU: (reads, writes).
_PARTIAL_SYNC = {"rep_stos": (("eax", "ecx", "edi"), ("ecx", "edi")),
                 "rep_movs": (("ecx", "esi", "edi"), ("ecx", "esi", "edi"))}
# Everything else that takes `c` is a cpu.h helper/macro that never touches a GPR or
# an XMM register through it (checked against cpu.h when it is parsed, see below).
_SAFE_MACROS = {"FS_RD8", "FS_RD16", "FS_RD32", "FS_WR8", "FS_WR16", "FS_WR32", "STR_STEP", "RE5_LOOP_BARRIER",
                "RE5_COV", "R8L", "R8H", "R16", "SET8L", "SET8H", "SET16",
                "flags_sub_m", "flags_add_m", "flags_logicz_m", "flags_adc_m", "flags_sbb_m",
                "flags_sub", "flags_add", "flags_logicz", "flags_adc", "flags_sbb",
                "flags_incs", "flags_decs", "op_shl_m", "op_shr_m", "op_sar_m",
                "op_shl", "op_shr", "op_sar", "sse_compare", "do_sahf", "fcompare_eflags"}


_FLAGS = ("cf", "pf", "af", "zf", "sf", "of")


def _cpu_h_touch_regs():
    """cpu.h helpers that take the CPU: (all names, those that read or write a GPR/XMM
    register through it, those that read or write a flag through it)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime", "recomp32_cpu", "cpu.h")
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return None, set(), set()
    names, touching, flagging = set(), set(), set()
    for m in re.finditer(r"static inline [^;{(]*?\b(\w+)\s*\(([^)]*)\)\s*\{", text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        names.add(m.group(1))
        if "CPU" in m.group(2) and re.search(r"c->(eax|ecx|edx|ebx|esp|ebp|esi|edi|xmm)\b", text[m.end():i]):
            touching.add(m.group(1))
        if "CPU" in m.group(2) and re.search(r"c->(cf|pf|af|zf|sf|of)\b", text[m.end():i]):
            flagging.add(m.group(1))
    return names, touching, flagging


# The flags live in locals too (R_cf...). The helpers that have a macro form over the
# locals are in _SAFE_MACROS; the rest (rol/ror/rcl/rcr, shld/shrd, bsf/bsr, bt on
# memory, pushfd/popfd) still go through c->cf..c->of, so the flags are stored before
# and reloaded after them. Without that, `bt [esp], eax; jae` in the CRT's strpbrk
# (0x00E7DA28) wrote c->cf and branched on a stale R_cf.
_CPU_H_NAMES, _CPU_H_TOUCH, _CPU_H_FLAGS = _cpu_h_touch_regs()


def _lname(r):
    return "R_" + r


def _store(r):
    return f"c->xmm[{r[1:]}] = R_{r};" if r.startswith("x") else f"c->{r} = R_{r};"


def _load(r):
    return f"R_{r} = c->xmm[{r[1:]}];" if r.startswith("x") else f"R_{r} = c->{r};"


def _rewrite(line):
    line = re.sub(r"\bpush32\(c, ", "push32_r(c, &R_esp, ", line)
    line = re.sub(r"\bpop32\(c\)", "pop32_r(c, &R_esp)", line)
    line = _REG_REF.sub(lambda m: "R_" + m.group(1), line)
    return _XMM_REF.sub(lambda m: "R_x" + m.group(1), line)


def _writes(text):
    out = set()
    for m in _LOCAL.finditer(text):
        r = m.group(1)
        before = text[:m.start()].rstrip()
        if before.endswith("&"):
            out.add(r)
            continue
        if re.search(r"\b(SET8L|SET8H|SET16)\(\s*$", before):
            out.add(r)
            continue
        if before.endswith("++") or before.endswith("--"):
            out.add(r)
            continue
        if _ASSIGN.match(text, m.end()):
            out.add(r)
    for m in re.finditer(r"\b(?:flags_\w+|op_\w+)_m\(\s*c\s*,\s*0x([0-9A-Fa-f]+)u?", text):
        mask = int(m.group(1), 16)
        if mask & 0x001: out.add("cf")
        if mask & 0x004: out.add("pf")
        if mask & 0x010: out.add("af")
        if mask & 0x040: out.add("zf")
        if mask & 0x080: out.add("sf")
        if mask & 0x800: out.add("of")
    if any(k in text for k in ("flags_add(c,", "flags_sub(c,", "flags_adc(c,", "flags_sbb(c,", "flags_incs(", "flags_decs(")):
        out.update(_FLAGS)
    if "flags_logicz(c," in text:
        out.update(("cf", "pf", "zf", "sf", "of"))
    if any(k in text for k in ("op_shl(c,", "op_shr(c,", "op_sar(c,")):
        out.update(("cf", "pf", "zf", "sf"))
    if "sse_compare(" in text or "fcompare_eflags(" in text:
        out.update(_FLAGS)
    if "do_sahf(" in text:
        out.update(("cf", "pf", "af", "zf", "sf"))
    return out


def _sync_kind(text):
    """'full', a _PARTIAL_SYNC name, 'flags' or None for an instruction's (rewritten) text."""
    kind = None
    if "RECOMP_TODO(" in text or "re5_recomp_todo(" in text:
        return "full"                  # aborts: the crash report reads the struct
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(\s*&?c\b", text):
        name = m.group(1)
        if name in _FULL_SYNC:
            return "full"
        if name in _PARTIAL_SYNC:
            kind = name
        elif name in _SAFE_MACROS or name.endswith("_r"):
            continue
        elif _CPU_H_NAMES is not None and name in _CPU_H_NAMES and name not in _CPU_H_TOUCH:
            if name in _CPU_H_FLAGS and kind is None:
                kind = "flags"
            continue
        else:
            return "full"              # unknown callee that sees the CPU: be safe
    return kind


def _insert_around(text, names, before, after):
    """Put `before` in front of each call statement to one of `names` and `after` behind it."""
    out, i = [], 0
    pat = re.compile(r"\b(%s)\(" % "|".join(map(re.escape, names)))
    while True:
        m = pat.search(text, i)
        if not m:
            out.append(text[i:])
            return "".join(out)
        parsed = _call_args(text, m.end() - 1)
        if not parsed:
            out.append(text[i:])
            return "".join(out)
        end = parsed[1] + 1
        semi = text.find(";", end)
        stmt_end = semi + 1 if semi >= 0 and text[end:semi].strip() == "" else end
        out.append(text[i:m.start()] + before + " " + text[m.start():stmt_end] + (" " + after if after else ""))
        i = stmt_end


def cache_registers(items, succ):
    """items: [(insn, [rewritable C lines])], succ from flag_liveness. Returns new items
    and the declaration line for the top of the function."""
    n = len(items)
    texts = [[_rewrite(l) for l in lines] for _, lines in items]
    joined = [" ".join(t) for t in texts]
    kinds = [_sync_kind(t) for t in joined]
    writes = [_writes(t) for t in joined]
    preds = [[] for _ in range(n)]
    for i, (s, _) in enumerate(succ):
        for j in s:
            preds[j].append(i)
    dirty_in = [set() for _ in range(n)]
    dirty_out = [None] * n
    changed = True
    while changed:
        changed = False
        for i in range(n):
            din = set()
            for p in preds[i]:
                if dirty_out[p] is not None:
                    din |= dirty_out[p]
            if i == 0:
                din = set(din)                         # entry: everything loaded, nothing dirty
            # Only an unconditional `call` leaves every register clean: a conditional
            # dispatch, or a jump table that gotos before dispatching, falls through or
            # branches with the registers as they were.
            clean = kinds[i] == "full" and items[i][0].mnemonic == "call" and not writes_after_call(joined[i])
            dout = set() if clean else din | writes[i]
            if din != dirty_in[i] or dout != dirty_out[i]:
                dirty_in[i], dirty_out[i] = din, dout
                changed = True
    new_items = []
    for i, (ins, _) in enumerate(items):
        d = dirty_in[i] | writes[i]
        stores = " ".join(_store(r) for r in CACHED if r in d)
        loads_all = " ".join(_load(r) for r in CACHED)
        lines = []
        for line in texts[i]:
            if kinds[i] == "full":
                line = _insert_around(line, _FULL_SYNC, stores, loads_all)
            elif kinds[i] in _PARTIAL_SYNC:
                rd, wr = _PARTIAL_SYNC[kinds[i]]
                st = " ".join(_store(r) for r in rd if r in d)
                ld = " ".join(_load(r) for r in wr)
                line = _insert_around(line, (kinds[i],), st, ld)
            elif kinds[i] == "flags" and _sync_kind(line) == "flags":
                # The helper sits inside an expression (R_eax = op_rol(c, ...)), so the
                # sync goes around the whole statement, not around the call.
                st = " ".join(_store(r) for r in _FLAGS if r in d)
                ld = " ".join(_load(r) for r in _FLAGS)
                line = (st + " " if st else "") + line + " " + ld
            if "return;" in line:
                line = line.replace("return;", "{ " + stores + " return; }" if stores else "return;")
            lines.append(line)
        new_items.append((ins, lines))
    decl = ("uint32_t " + ", ".join(f"R_{r} = c->{r}" for r in CACHED[:8]) + "; "
            "XMM " + ", ".join(f"R_{r} = c->xmm[{r[1:]}]" for r in CACHED[8:16]) + "; "
            "uint32_t " + ", ".join(f"R_{r} = c->{r}" for r in CACHED[16:]) + ";")
    return new_items, decl


def writes_after_call(text):
    """Does the instruction write a register after its full-sync call (so it ends dirty)?"""
    m = None
    for m in re.finditer(r"\b(%s)\(" % "|".join(_FULL_SYNC), text):
        pass
    if not m:
        return False
    parsed = _call_args(text, m.end() - 1)
    return bool(parsed and _writes(text[parsed[1]:]))


class Lifter:
    def __init__(self, dll_path, image_size, read_va=None):
        self.image_lo = IMAGE_BASE
        self.image_hi = IMAGE_BASE + image_size
        self.read_va = read_va           # (va, n) -> bytes
        self.func_start = 0
        self.func_end = 0
        self.jumptables = {}             # jmp ea -> [target VAs]
        self.reloc_vas = set()           # VAs of relocatable address immediates
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def resolve_jumptable(self, table_va):
        """Read consecutive dword targets from a jump table.

        The old rule - stop at the first entry outside the *current function* -
        silently truncated any table whose last arms were recovered as
        functions of their own, which on a stripped binary happens constantly:
        an arm that is also a branch target gets its own catalog entry, the
        clamp ends the containing function there, and every arm past it
        disappears.

        It cost a day in Mario Kart. The window procedure's table has ten arms;
        arm nine, WM_NCCREATE, lives past a neighbour that recovery had called
        a function. The lifter emitted nine `goto`s and an `abort()`, so no
        window could ever be created - CreateWindowExW returned NULL with
        ERROR_NOT_ENOUGH_MEMORY and nothing said why.

        The image's own range is the bound that a real table has anyway: the
        dword after the last arm is whatever follows it in `.rdata` - here the
        byte index table the same switch uses - and is not an address at all.
        """
        targets = []
        if not self.read_va:
            return targets
        for i in range(256):
            try:
                raw = self.read_va(table_va + 4 * i, 4)
            except Exception:
                break
            t = int.from_bytes(raw, "little")
            if self.in_image(t):
                targets.append(t)
            else:
                break
        return targets

    def in_image(self, a):
        return self.image_lo <= (a & 0xffffffff) < self.image_hi

    def disp_is_addr(self, insn, d):
        """Is a memory operand's displacement an absolute address (needs GVA) or
        a plain constant offset? `.reloc` marks exactly the former, and it marks
        them whether or not the operand also has a base register - `mov dl,
        [ecx + 0x56d902]` indexes a table at an absolute address just as much as
        `mov eax, [0x58d428]` does. Fall back to a range check only when we have
        no reloc table at all."""
        enc = getattr(insn, "encoding", None)
        do = getattr(enc, "disp_offset", 0) if enc else 0
        if self.reloc_vas and do:
            return ((insn.address + do) & 0xffffffff) in self.reloc_vas
        return self.in_image(d)

    # ---- operand rendering ----
    def areg(self, name):
        """A register used to form an address.

        `c->bp` does not exist: the CPU struct holds 32-bit registers and the
        16-bit ones are windows onto them. Emitting the raw name works for
        eax..edi and produces uncompilable C the moment an address-size prefix
        puts a 16-bit register in the operand, which segmented code does."""
        if name in R32: return "c->%s" % name
        if name in R16: return "R16(c->%s)" % R16[name]
        if name in R8L: return "R8L(c->%s)" % R8L[name]
        if name in R8H: return "R8H(c->%s)" % R8H[name]
        return "c->%s" % name

    def mem_addr(self, insn, op):
        """The address of a memory operand, for instructions that need it
        directly rather than through rd/wr - `lds`/`les` and friends.

        Flat builds want exactly addr_expr. A segmented one needs the segment
        base added, the same as every other access, and overriding rd/wr does
        not cover this because the far-pointer load computes its own address.
        Getting it wrong reads the pointer from a near-null address and yields
        a plausible-looking selector:offset pair made of whatever was there."""
        return self.addr_expr(insn, op)

    def addr_expr(self, insn, op):
        m = op.mem
        terms = []
        base = self.md.reg_name(m.base) if m.base else None
        index = self.md.reg_name(m.index) if m.index else None
        d = m.disp & 0xffffffff
        if base: terms.append(self.areg(base))
        if index: terms.append(f"{self.areg(index)}*{m.scale}")
        if d or not terms:
            terms.append(f"GVA(0x{d:08X})" if self.disp_is_addr(insn, d) else f"0x{d:08X}u")
        return "(" + " + ".join(terms) + ")"

    def seg_name(self, op):
        seg = getattr(op.mem, "segment", 0)
        return self.md.reg_name(seg) if seg else None

    def seg_off(self, insn, op):
        """raw segment offset (base+index+disp, NOT image-relative / no GVA)"""
        m = op.mem
        terms = []
        if m.base:  terms.append(self.areg(self.md.reg_name(m.base)))
        if m.index: terms.append("%s*%d" % (self.areg(self.md.reg_name(m.index)),
                                            m.scale))
        d = m.disp & 0xffffffff
        if d or not terms: terms.append(f"0x{d:08X}u")
        return "(" + " + ".join(terms) + ")"

    def _bad_size(self, insn, op, what):
        """A memory operand these cannot express. Raised rather than guessed at,
        and named rather than left as a bare KeyError: the size is the whole
        diagnosis. 6 is an m16:32 far pointer, 10 an x87 extended double - both
        real, both needing their own handling in translate() rather than a
        width-indexed accessor here."""
        return NotImplementedError(
            "%s: %d-byte memory operand at %#x (%s %s) - %s"
            % (what, op.size, insn.address, insn.mnemonic, insn.op_str,
               "far pointer (m16:32)" if op.size == 6 else
               "x87 extended (m80)" if op.size == 10 else "unsupported width"))

    def rd(self, insn, op):
        sz = op.size
        if self.seg_name(op) == "fs":   # TIB-relative (SEH chain etc.); FS_* picks real vs simulated TIB
            off = self.seg_off(insn, op)
            if sz not in (1, 2, 4): raise self._bad_size(insn, op, "rd fs:")
            return {1:f"FS_RD8(c, {off})",2:f"FS_RD16(c, {off})",4:f"FS_RD32(c, {off})"}[sz]
        a = self.addr_expr(insn, op)
        if sz not in (1, 2, 4): raise self._bad_size(insn, op, "rd")
        return {1:f"rd8({a})", 2:f"rd16({a})", 4:f"rd32({a})"}[sz]

    def wr(self, insn, op, val):
        sz = op.size
        if self.seg_name(op) == "fs":
            off = self.seg_off(insn, op)
            if sz not in (1, 2, 4): raise self._bad_size(insn, op, "wr fs:")
            return {1:f"FS_WR8(c, {off}, {val});",2:f"FS_WR16(c, {off}, {val});",
                    4:f"FS_WR32(c, {off}, {val});"}[sz]
        a = self.addr_expr(insn, op)
        if sz not in (1, 2, 4): raise self._bad_size(insn, op, "wr")
        return {1:f"wr8({a}, {val});", 2:f"wr16({a}, {val});", 4:f"wr32({a}, {val});"}[sz]

    def src(self, insn, op):
        """read value of an operand"""
        if op.type == X86_OP_REG: return reg_read(self.md.reg_name(op.reg))
        if op.type == X86_OP_IMM:
            sz = op.size
            mask = {1:0xFF,2:0xFFFF,4:0xFFFFFFFF}[sz]
            v = op.imm & mask
            # a 4-byte immediate sitting at a relocated VA is an absolute address
            # (e.g. `push offset table`); emit GVA so it tracks the load base.
            if sz == 4 and self.reloc_vas:
                enc = getattr(insn, "encoding", None)
                io = getattr(enc, "imm_offset", 0) if enc else 0
                if io and ((insn.address + io) & 0xffffffff) in self.reloc_vas:
                    return f"GVA(0x{v:08X})"
            return f"0x{v:X}u"
        if op.type == X86_OP_MEM: return self.rd(insn, op)
        raise NotImplementedError("src type")

    def dst_write(self, insn, op, val):
        if op.type == X86_OP_REG: return reg_write(self.md.reg_name(op.reg), val)
        if op.type == X86_OP_MEM: return self.wr(insn, op, val)
        raise NotImplementedError("dst type")

    def op_size(self, insn, op):
        return op.size

    # ---- per-instruction translation ----
    def translate(self, insn, labels):
        m = insn.mnemonic
        ops = insn.operands
        ea = insn.address
        nxt = insn.address + insn.size

        # capstone keeps the `lock` prefix in the mnemonic. The instruction
        # underneath is the one to translate; the atomicity is in the helper
        # the xadd/cmpxchg cases below reach for, and for everything else a
        # `lock` on a single store is what the hardware does anyway.
        if m.startswith("lock "):
            m = m[5:]

        def two(): return ops[0], ops[1]
        def sz0(): return ops[0].size

        # ---- bit test and friends ----
        if m in ("bt", "bts", "btr", "btc"):
            opnum = {"bt": 0, "bts": 1, "btr": 2, "btc": 3}[m]
            d, s2 = ops[0], ops[1]
            idx = self.src(insn, s2)
            if d.type == X86_OP_MEM:
                return ["bit_string_op(c, %s, (int32_t)(%s), %d);"
                        % (self.addr_expr(insn, d), idx, opnum)]
            return ["bit_reg_op(c, &c->%s, %s, %d);"
                    % (self.md.reg_name(d.reg), idx, opnum)]

        # ---- LOCK-prefixed read-modify-write ----
        # Capstone folds the prefix into the mnemonic. Only the forms that turn
        # up are here: these are what libstdc++ compiles a reference count to,
        # and a game with several threads sharing strings really does race on
        # them. Anything else keeps its TODO rather than quietly losing the
        # atomicity, which would fail as a use-after-free somewhere unrelated.
        if m.startswith("lock "):
            base = m[5:]
            d, s2 = (ops[0], ops[1]) if len(ops) > 1 else (ops[0], None)
            if base in ("add", "xadd") and d.type == X86_OP_MEM and s2 is not None:
                addr = self.addr_expr(insn, d)
                val = self.src(insn, s2)
                out = ["{ uint32_t _old = atomic_xadd32(%s, %s);" % (addr, val)]
                if base == "xadd":
                    # xadd hands the register the value memory held before.
                    out[0] += " " + reg_write(self.md.reg_name(s2.reg), "_old")
                out[0] += " flags_add(c, _old, %s, 4); }" % val
                return out
            return [f"RECOMP_TODO(0x{ea:08X}, \"{m} {insn.op_str}\");"]

        if m[0] == "f":
            return self.fpu(insn)
        if self.is_sse(insn):
            return self.sse(insn)

        # Prefetches are hints. They move no data and set no flags, so the
        # correct translation really is nothing at all - and leaving them as a
        # TODO aborts a game in the middle of code that was working.
        if m.startswith("prefetch"):
            return ["/* prefetch: hint only */"]

        # cmovcc. Same conditions as Jcc, so the table is already written.
        # Note x86 reads the source operand unconditionally - an unmapped
        # address faults whether the move happens or not - but a C `if` reading
        # a live address is the shape every caller actually relies on.
        if m.startswith("cmov"):
            cond = self._cond("j" + m[4:])
            if cond:
                d, sop = ops[0], ops[1]
                return [f"if ({cond}) {{ {self.dst_write(insn, d, self.src(insn, sop))} }}"]
        if m == "sahf":
            return ["do_sahf(c, R8H(c->eax));"]
        if m == "lahf":
            return ["SET8H(c->eax, (c->sf<<7)|(c->zf<<6)|(c->af<<4)|(c->pf<<2)|2|c->cf);"]

        # arithmetic / logic
        if m == "mov":
            d, s = two(); return [self.dst_write(insn, d, self.src(insn, s))]
        if m == "lea":
            d, s = two(); return [reg_write(self.md.reg_name(d.reg), self.addr_expr(insn, s))]
        if m in ("add","sub","and","or","xor","adc","sbb"):
            d, s = two(); sz = sz0(); a = self._read_dst(insn, d); b = self.src(insn, s)
            if m == "add": r = f"flags_add(c, {a}, {b}, {sz})"
            elif m == "sub": r = f"flags_sub(c, {a}, {b}, {sz})"
            elif m == "adc": r = f"flags_adc(c, {a}, {b}, {sz})"
            elif m == "sbb": r = f"flags_sbb(c, {a}, {b}, {sz})"
            elif m == "and": r = f"flags_logicz(c, {a} & {b}, {sz})"
            elif m == "or":  r = f"flags_logicz(c, {a} | {b}, {sz})"
            elif m == "xor": r = f"flags_logicz(c, {a} ^ {b}, {sz})"
            return [self.dst_write(insn, d, r)]
        if m == "cmp":
            d, s = two(); return [f"flags_sub(c, {self._read_dst(insn,d)}, {self.src(insn,s)}, {d.size});"]
        if m == "test":
            d, s = two(); return [f"flags_logicz(c, {self._read_dst(insn,d)} & {self.src(insn,s)}, {d.size});"]
        if m == "inc":
            d = ops[0]; return [self.dst_write(insn, d, f"flags_incs(c, {self._read_dst(insn,d)}, {d.size})")]
        if m == "dec":
            d = ops[0]; return [self.dst_write(insn, d, f"flags_decs(c, {self._read_dst(insn,d)}, {d.size})")]
        if m == "neg":
            d = ops[0]; return [self.dst_write(insn, d, f"flags_sub(c, 0, {self._read_dst(insn,d)}, {d.size})")]
        if m == "not":
            d = ops[0]; return [self.dst_write(insn, d, f"(~({self._read_dst(insn,d)}))")]
        if m in ("shl","sal","shr","sar"):
            d = ops[0]; cnt = self.src(insn, ops[1]) if len(ops) > 1 else "1"
            fn = {"shl":"op_shl","sal":"op_shl","shr":"op_shr","sar":"op_sar"}[m]
            return [self.dst_write(insn, d, f"{fn}(c, {self._read_dst(insn,d)}, {cnt}, {d.size})")]
        if m in ("rol", "ror"):
            d = ops[0]; cnt = self.src(insn, ops[1]) if len(ops) > 1 else "1"
            fn = "op_rol" if m == "rol" else "op_ror"
            return [self.dst_write(insn, d,
                                   f"{fn}(c, {self._read_dst(insn,d)}, {cnt}, {d.size})")]
        if m == "bswap":
            # Register only, and 32-bit only: the 16-bit encoding is undefined
            # on real parts, so anything else here is data read as code.
            d = ops[0]
            if d.size == 4:
                return [self.dst_write(insn, d,
                                       f"op_bswap32({self._read_dst(insn, d)})")]
        if m in ("bsf", "bsr"):
            d, src = ops[0], ops[1]
            fn = "op_bsf" if m == "bsf" else "op_bsr"
            return [self.dst_write(insn, d,
                                   f"{fn}(c, {self._read_dst(insn,d)}, "
                                   f"{self.src(insn,src)}, {d.size})")]
        if m in ("rcl", "rcr"):
            d = ops[0]; cnt = self.src(insn, ops[1]) if len(ops) > 1 else "1"
            fn = "op_rcl" if m == "rcl" else "op_rcr"
            return [self.dst_write(insn, d,
                                   f"{fn}(c, {self._read_dst(insn,d)}, {cnt}, {d.size})")]
        if m in ("shld", "shrd"):
            d, s2 = ops[0], ops[1]
            cnt = self.src(insn, ops[2]) if len(ops) > 2 else "R8L(c->ecx)"
            fn = "op_shld" if m == "shld" else "op_shrd"
            return [self.dst_write(insn, d,
                                   f"{fn}(c, {self._read_dst(insn,d)}, "
                                   f"{self.src(insn,s2)}, {cnt}, {d.size})")]
        if m in ("lds", "les", "lfs", "lgs", "lss"):
            # Load a far pointer: the offset into the register, the selector
            # into a segment register. A flat PE never does this; segmented
            # 32-bit code does it constantly. See the CPU struct: the selector
            # is stored and does not affect any later access, so this is right
            # only while every segment shares one address space.
            d, src = ops[0], ops[1]
            seg = m[1:]
            sz = d.size
            return ["{ uint32_t _a = %s; %s c->%s = rd16(_a + %d); }"
                    % (self.mem_addr(insn, src),
                       self.dst_write(insn, d, "rd%d(_a)" % (sz * 8)),
                       seg, sz)]
        if m == "retf":
            # The far frame belongs to whoever called across the segment
            # boundary, and in a lifted build that is the runtime, not us.
            n = (ops[0].imm if ops and ops[0].type == X86_OP_IMM else 0)
            return ["/* retf %d: far frame unwound by the caller */ return;" % n]
        if m == "movzx":
            d, s = two(); return [self.dst_write(insn, d, f"({self.src(insn,s)})")]
        if m == "movsx":
            d, s = two(); ssz = s.size
            cast = {1:"int8_t",2:"int16_t"}[ssz]
            raw = self._read_raw(insn, s)
            return [self.dst_write(insn, d, f"(uint32_t)(int32_t)({cast})({raw})")]
        if m == "cdq":
            return ["c->edx = (c->eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;"]
        if m == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)R16(c->eax);"]
        if m == "xchg":
            d, s = two()
            return [f"{{ uint32_t _t = {self._read_dst(insn,d)}; " +
                    self.dst_write(insn, d, self.src(insn, s)).rstrip(';') + "; " +
                    self.dst_write(insn, s, "_t").rstrip(';') + "; }"]

        # xadd / cmpxchg - the interlocked pair.
        #
        # A game's reference counts and queue indices are these, and a
        # recompiled one has as many threads as the original, so they go to
        # real atomics in cpu.h rather than a read-modify-write that is right
        # until it is not. Only the 32-bit memory form is written out, because
        # that is the only one a Win32 compiler emits for Interlocked*.
        if m in ("xadd", "cmpxchg") and sz0() == 4:
            d, s = two()
            if d.type == X86_OP_MEM:
                a = self.addr_expr(insn, d)
                if m == "xadd":
                    return [f"{{ uint32_t _v = {self.src(insn, s)};",
                            f"  uint32_t _old = atomic_xadd32({a}, _v);",
                            f"  {self.dst_write(insn, s, '_old')}",
                            "  flags_add(c, _old, _v, 4); }"]
                return [f"{{ uint32_t _old = atomic_cmpxchg32({a}, c->eax, {self.src(insn,s)});",
                        "  flags_sub(c, c->eax, _old, 4);",
                        "  if (_old != c->eax) c->eax = _old; }"]
        if m.startswith("set") and m not in ("setssbsy",):   # setcc r/m8
            cond = self._cond("j" + m[3:])
            if cond is not None:
                return [self.dst_write(insn, ops[0], f"(({cond}) ? 1 : 0)")]
        if m == "pushfd": return ["push32(c, eflags_pack(c));"]
        if m == "popfd":  return ["eflags_unpack(c, pop32(c));"]
        # 16-bit forms (66 9C / 66 9D) move two bytes, not four (difftest_cpu).
        if m == "pushf":  return ["c->esp -= 2; wr16(c->esp, (uint16_t)eflags_pack(c));"]
        if m == "popf":   return ["eflags_unpack(c, rd16(c->esp)); c->esp += 2;"]
        if m in ("cbw",):  return ["SET16(c->eax, (uint16_t)(int16_t)(int8_t)R8L(c->eax));"]
        if m in ("cwd",):  return ["SET16(c->edx, (R16(c->eax) & 0x8000u) ? 0xFFFFu : 0u);"]
        # AL = [EBX + AL]: the MSVC CRT turns the x87 class (fxam) into a
        # result with a 16-byte table this way.
        if m == "xlatb": return ["SET8L(c->eax, rd8((uint32_t)(c->ebx + R8L(c->eax))));"]
        if m in ("wait","fwait","nop","int3","fnop","hint_nop"): return [f"/* {m} */"]
        # Direction flag: real state (cpu.h CPU.df), stepped by STR_STEP in every
        # string op. MSVC's strrchr (std; repne scasb) and memmove's overlapping
        # path (std; rep movsd) depend on it.
        if m == "std": return ["c->df = 1;"]
        if m == "cld": return ["c->df = 0;"]
        # Carry flag by hand: hand-written asm and some compiler idioms
        # (`stc; sbb eax,eax`, carry-chained bignum loops) set it explicitly.
        if m == "stc": return ["c->cf = 1;"]
        if m == "clc": return ["c->cf = 0;"]
        if m == "cmc": return ["c->cf ^= 1u;"]
        if m == "pushal":
            # Order is EAX ECX EDX EBX, then ESP as it was BEFORE the first
            # push, then EBP ESI EDI.
            return ["{ uint32_t _sp = c->esp; push32(c, c->eax); push32(c, c->ecx); push32(c, c->edx);"
                    " push32(c, c->ebx); push32(c, _sp); push32(c, c->ebp); push32(c, c->esi);"
                    " push32(c, c->edi); }"]
        if m == "popal":
            # The saved ESP slot is skipped, not loaded.
            return ["c->edi = pop32(c); c->esi = pop32(c); c->ebp = pop32(c); c->esp += 4;"
                    " c->ebx = pop32(c); c->edx = pop32(c); c->ecx = pop32(c); c->eax = pop32(c);"]
        # cpuid and rdtsc describe the machine, so the runtime answers them:
        # it picks the CPU model the game sees (feature bits decide which code
        # paths run) and the time base. The runtime must define both.
        if m == "cpuid": return ["recomp_cpuid(c);"]
        if m == "rdtsc": return ["recomp_rdtsc(c);"]
        if m == "enter":
            n = ops[0].imm if ops else 0
            return [f"push32(c, c->ebp); c->ebp = c->esp; c->esp -= {n};"]
        if m in ("loop","loope","loopz","loopne","loopnz"):
            t = ops[0]
            extra = ""
            if m in ("loope","loopz"): extra = " && c->zf"
            elif m in ("loopne","loopnz"): extra = " && !c->zf"
            if t.type == X86_OP_IMM and t.imm in labels:
                return [f"c->ecx--; if (c->ecx{extra}) goto L_{t.imm:08X};"]
            return [f"c->ecx--; if (c->ecx{extra}) {{ dispatch(c, 0x{t.imm:08X}u); return; }}"]

        # multiply / divide (1-operand edx:eax forms + imul r,rm[,imm])
        if m == "mul":
            s = ops[0]; sz = s.size; v = self.src(insn, s)
            if sz == 4: return [f"{{ uint64_t _p=(uint64_t)c->eax*(uint32_t)({v}); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); c->cf=c->of=(c->edx!=0); }}"]
            if sz == 2: return [f"{{ uint32_t _p=(uint32_t)R16(c->eax)*(uint16_t)({v}); SET16(c->eax,_p); SET16(c->edx,_p>>16); c->cf=c->of=((_p>>16)!=0); }}"]
            return [f"{{ uint16_t _p=(uint16_t)R8L(c->eax)*(uint8_t)({v}); SET16(c->eax,_p); c->cf=c->of=((_p>>8)!=0); }}"]
        if m == "imul":
            if len(ops) == 1:
                s = ops[0]; sz = s.size; v = self.src(insn, s)
                if sz == 4: return [f"{{ int64_t _p=(int64_t)(int32_t)c->eax*(int32_t)({v}); c->eax=(uint32_t)_p; c->edx=(uint32_t)((uint64_t)_p>>32); c->cf=c->of=((int32_t)_p!=_p); }}"]
                if sz == 2: return [f"{{ int32_t _p=(int32_t)(int16_t)R16(c->eax)*(int16_t)({v}); SET16(c->eax,_p); SET16(c->edx,_p>>16); c->cf=c->of=((int16_t)_p!=_p); }}"]
                return [f"{{ int16_t _p=(int16_t)(int8_t)R8L(c->eax)*(int8_t)({v}); SET16(c->eax,_p); c->cf=c->of=((int8_t)_p!=_p); }}"]
            d = ops[0]; sz = d.size; cast = {1:"int8_t",2:"int16_t",4:"int32_t"}[sz]
            if len(ops) == 2: a = self._read_dst(insn, d); b = self.src(insn, ops[1])
            else:             a = self.src(insn, ops[1]); b = self.src(insn, ops[2])
            return [f"{{ int64_t _p=(int64_t)({cast})({a})*({cast})({b}); " +
                    self.dst_write(insn, d, "(uint32_t)_p").rstrip(';') + f"; c->cf=c->of=(({cast})_p!=_p); }}"]
        if m in ("div","idiv"):
            s = ops[0]; sz = s.size; v = self.src(insn, s); sg = (m == "idiv")
            if sz == 4:
                if sg: return [f"{{ int64_t _n=(int64_t)(((uint64_t)c->edx<<32)|c->eax); int32_t _d=(int32_t)({v}); c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%_d); }}"]
                return [f"{{ uint64_t _n=((uint64_t)c->edx<<32)|c->eax; uint32_t _d=(uint32_t)({v}); c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%_d); }}"]
            if sz == 2:
                if sg: return [f"{{ int32_t _n=(int32_t)(((uint32_t)R16(c->edx)<<16)|R16(c->eax)); int16_t _d=(int16_t)({v}); SET16(c->eax,(uint16_t)(int16_t)(_n/_d)); SET16(c->edx,(uint16_t)(int16_t)(_n%_d)); }}"]
                return [f"{{ uint32_t _n=((uint32_t)R16(c->edx)<<16)|R16(c->eax); uint16_t _d=(uint16_t)({v}); SET16(c->eax,_n/_d); SET16(c->edx,_n%_d); }}"]
            if sg: return [f"{{ int16_t _n=(int16_t)R16(c->eax); int8_t _d=(int8_t)({v}); SET8L(c->eax,(uint8_t)(int8_t)(_n/_d)); SET8H(c->eax,(uint8_t)(int8_t)(_n%_d)); }}"]
            return [f"{{ uint16_t _n=R16(c->eax); uint8_t _d=(uint8_t)({v}); SET8L(c->eax,_n/_d); SET8H(c->eax,_n%_d); }}"]

        # string ops (DF in c->df, stepped by STR_STEP; rep prefix loops on ECX)
        parts = m.split()
        # An F3 prefix is only a REP on a string instruction. On anything else
        # it is a hint, and the one that matters is `repz ret` (F3 C3) - AMD's
        # branch-prediction idiom for a plain `ret`, which MSVC emits at every
        # branch target that returns. Strip the prefix and let the real handler
        # for the base mnemonic have it. Without this the block below indexed
        # base[-1] and died on the 't' of "ret".
        if parts[0] in REP_PREFIXES and len(parts) > 1 and parts[1] not in STRING_OPS:
            m = parts[1]
            parts = [m]
        if parts[0] in REP_PREFIXES or parts[0] in STRING_OPS:
            rep = parts[0] if len(parts) > 1 else None
            base = parts[1] if rep else parts[0]
            esz = {"b":1,"w":2,"d":4}[base[-1]]
            wfn = {1:"wr8",2:"wr16",4:"wr32"}[esz]; rfn = {1:"rd8",2:"rd16",4:"rd32"}[esz]
            areg = {1:"R8L(c->eax)",2:"R16(c->eax)",4:"c->eax"}[esz]
            step = f"STR_STEP(c, {esz})"
            if base.startswith("stos"): body = f"{wfn}(c->edi, {areg}); c->edi += {step};"
            elif base.startswith("movs"): body = f"{wfn}(c->edi, {rfn}(c->esi)); c->esi += {step}; c->edi += {step};"
            elif base.startswith("lods"): body = f"{('SET8L(c->eax,'+rfn+'(c->esi))') if esz==1 else (('SET16(c->eax,'+rfn+'(c->esi))') if esz==2 else 'c->eax = '+rfn+'(c->esi)')}; c->esi += {step};"
            elif base.startswith("scas"):
                cmp = f"flags_sub(c, {areg}, {rfn}(c->edi), {esz}); c->edi += {step};"
                if rep in ("repe","repz"):  return [f"while (c->ecx) {{ c->ecx--; {cmp} if (!c->zf) break; }}"]
                if rep in ("repne","repnz"):return [f"while (c->ecx) {{ c->ecx--; {cmp} if (c->zf) break; }}"]
                return [cmp]
            elif base.startswith("cmps"):
                cmp = f"flags_sub(c, {rfn}(c->esi), {rfn}(c->edi), {esz}); c->esi += {step}; c->edi += {step};"
                if rep in ("repe","repz"):  return [f"while (c->ecx) {{ c->ecx--; {cmp} if (!c->zf) break; }}"]
                if rep in ("repne","repnz"):return [f"while (c->ecx) {{ c->ecx--; {cmp} if (c->zf) break; }}"]
                return [cmp]
            else:
                return [_todo(ea, f"string {m} {insn.op_str}")]
            # rep stos/movs: one bulk host operation (cpu.h rep_stos/rep_movs).
            # The element loop below reloads ECX/EDI/ESI from the CPU struct on
            # every element, because a guest store may alias it; RE5's CRT
            # memset/memcpy (rep stosd/movsd) were the hottest guest code.
            if rep and base[:4] in ("stos", "movs"):
                return [f"rep_{base[:4]}(c, {esz});"]
            if rep: return [f"while (c->ecx) {{ {body} c->ecx--; }}"]
            return [body]

        # stack
        if m == "push":
            return [f"push32(c, {self.src(insn, ops[0])});"]
        if m == "pop":
            d = ops[0]
            if d.type == X86_OP_REG: return [reg_write(self.md.reg_name(d.reg), "pop32(c)")]
            return [self.wr(insn, d, "pop32(c)")]

        # control flow
        if m in ("jmp", "call") and ops and ops[0].type == X86_OP_MEM \
                and ops[0].size == 6:
            # `jmp/call fword ptr [...]` - an m16:32 far pointer. The CPU model
            # is flat and has one code segment, so there is no correct
            # translation: loading CS is the whole point of the instruction.
            # An honest abort beats reading four of the six bytes and jumping
            # somewhere plausible. In a 32-bit PE this is almost always a
            # WOW64/segment transition in code the program never reaches, or a
            # data run the catalog mistook for a function.
            return [_todo(ea, f"far {m} m16:32")]
        if m == "jmp":
            t = ops[0]
            if t.type == X86_OP_IMM and t.imm in labels:
                return [f"goto L_{t.imm:08X};"]
            if ea in self.jumptables:                       # switch via jump table
                tgts = self.jumptables[ea]
                addr = self.addr_expr(insn, t)
                out = [f"{{ uint32_t _jt = rd32({addr});"]
                for tv in sorted(set(tgts)):
                    if tv in labels:
                        out.append(f"  if (_jt == GVA(0x{tv:08X})) goto L_{tv:08X};")
                    else:
                        # An arm outside this function - a neighbour recovery
                        # called a function of its own, or the same code lifted
                        # twice under two extents. Written as a literal
                        # dispatch rather than left to the computed one below,
                        # because a literal is something a build step can SEE:
                        # the driver closes every address the generated text
                        # dispatches to, and a target that only appears in a
                        # register is a runtime abort nobody predicted.
                        out.append(f"  if (_jt == GVA(0x{tv:08X}))"
                                   f" {{ dispatch(c, 0x{tv:08X}u); return; }}")
                # Anything else the table holds - an entry this walk did not
                # enumerate at all. Aborting here was a guess that the table
                # could not hold anything else, and it was wrong.
                out.append("  dispatch_jmp(c, _jt); return; }")
                return out
            if t.type == X86_OP_IMM:                        # tail call to another function
                return [f"dispatch(c, 0x{t.imm:08X}u); return;"]
            return [f"/* indirect jmp */ dispatch_jmp(c, {self._target(insn,t)}); return;"]
        if m == "call":
            t = ops[0]
            tgt = self._target(insn, t)
            if t.type == X86_OP_IMM and t.imm == nxt:
                # The position-independent "get PC" idiom: a call to the very
                # next instruction, whose only purpose is to push EIP so the
                #  that follows can read it and form a GOT pointer. There
                # is no callee. Dispatching would look up an address in the
                # middle of this same function and find nothing - so do what
                # the hardware does, push the address and fall through.
                #   call 0x8072d9d   <- next instruction
                #   pop  ebx         <- ebx = 0x8072d9d
                #   add  ebx, ...    <- ebx = _GLOBAL_OFFSET_TABLE_
                # Every PIC i386 binary opens with this, which is every .so and
                # every PIE executable.
                return [f"push32(c, 0x{nxt:08X}u); /* get-PC, no callee */"]
            if t.type == X86_OP_IMM:
                return [f"push32(c, 0x{nxt:08X}u); dispatch(c, {tgt});"]
            # Indirect: resolve the target BEFORE pushing the return address.
            # `call dword ptr [esp+0x18]` reads its target with the pre-push esp;
            # pushing first shifts every esp-relative operand by 4 and calls the
            # wrong slot (seen in the wild: read a zero, jumped to 0).
            return [f"{{ uint32_t _ct = {tgt};"
                    f" push32(c, 0x{nxt:08X}u); dispatch(c, _ct); }}"]
        if m == "ret":
            n = (ops[0].imm if ops and ops[0].type == X86_OP_IMM else 0)
            return [f"c->esp += {4 + n}; return;"]
        if m.startswith("j"):
            cond = self._cond(m)
            if cond is None: return [_todo(ea, m)]
            t = ops[0]
            if t.type == X86_OP_IMM and t.imm in labels:
                return [f"if ({cond}) goto L_{t.imm:08X};"]
            if t.type == X86_OP_IMM:        # conditional jump to another function (shared epilogue)
                return [f"if ({cond}) {{ dispatch(c, 0x{t.imm:08X}u); return; }}"]
            return [_todo(ea, f"jcc {m} {insn.op_str}")]
        if m in ("nop","hint_nop"): return ["/* nop */"]
        if m == "leave":
            return ["c->esp = c->ebp; c->ebp = pop32(c);"]

        # MXCSR, the SSE control word. Kept per CPU and read back as written;
        # FTZ/DAZ also reach the host thread (cpu_ldmxcsr), because RE5's sound
        # thread sets flush-to-zero before its filters and reverb run and the
        # lifted SSE arithmetic is the host's. Rounding bits and exception
        # masks are stored only.
        if m == "stmxcsr":
            return [self.wr(insn, ops[0], "c->mxcsr")]
        if m == "ldmxcsr":
            return [f"cpu_ldmxcsr(c, {self.rd(insn, ops[0])});"]

        return [_todo(ea, f"{m} {insn.op_str}")]

    # ---- x87 FPU ----
    def _st_idx(self, op):
        mm = re.match(r"st\((\d)\)", self.md.reg_name(op.reg))
        return int(mm.group(1)) if mm else 0

    def _fmem(self, insn, op, kind):
        a = self.addr_expr(insn, op); sz = op.size
        if kind == "f":
            if sz == 10: return f"rdf80({a})"      # x87 extended, explicit integer bit 
            return f"rdf32({a})" if sz == 4 else f"rdf64({a})"
        return {2: f"rdi16({a})", 4: f"rdi32({a})", 8: f"rdi64({a})"}[sz]

    def fpu(self, insn):
        m = insn.mnemonic; ops = insn.operands
        memop = ops[0] if ops and ops[0].type == X86_OP_MEM else None

        # An x87 register here is a `double`, and that is a deliberate model
        # choice: a real x87 register is wider than the values it holds, so
        # widening costs nothing for the loads and stores a compiler emits.
        # It does cost something for the ones it does not. `fld tbyte` reads a
        # genuine 80-bit extended double, `fbld`/`fbstp` read and write packed
        # BCD, and `fldenv`/`fnstenv`/`fsave`/`frstor` move the whole 28-byte
        # FPU environment. None of those fit, and reading eight of ten bytes as
        # a double produces a number that looks plausible and is not.
        # `fld tbyte` and `fstp tbyte` are now honoured - see rdf80/wrf80 - but
        # the rest of the wide operands still are not. fbld/fbstp read and write
        # packed BCD, which is ten bytes of something else entirely, and
        # fldenv/fnstenv/fsave/frstor move the whole FPU environment.
        if m in ("fbld", "fbstp"):
            return [_todo(insn.address, f"x87 packed BCD: {m} {insn.op_str}")]
        # The 32-bit protected-mode environment is seven dwords: control,
        # status (with TOP in bits 11-13), tag, then the last instruction and
        # operand pointers, which nothing reads back. The MSVC CRT uses it to
        # hand condition codes back through the status word (fnstenv; and/or
        # the C bits; fldenv), so those bits round-trip; the control word is
        # the model's fixed one and the tag word says "valid". fnsave/frstor
        # add the eight registers as 80-bit values in stack order, and fnsave
        # then reinitialises the FPU as finit would.
        # (Capstone gives fnsave/frstor a dword operand, so the mnemonic decides.)
        if m in ("fnstenv", "fldenv", "fnsave", "frstor", "fsave", "fstenv") and memop is not None:
            a = self.addr_expr(insn, memop)
            if m in ("fnstenv", "fstenv", "fnsave", "fsave"):
                out = [f"{{ uint32_t _a = {a}; wr32(_a, c->fpu_cw & 0xFFFFu);"
                       " wr32(_a + 4, (c->fpu_sw & 0x4700u) | ((uint32_t)(c->fpu_top & 7) << 11));"
                       " wr32(_a + 8, 0u); wr32(_a + 12, 0u); wr32(_a + 16, 0u); wr32(_a + 20, 0u);"
                       " wr32(_a + 24, 0u);"]
                if m in ("fnsave", "fsave"):
                    out.append(" for (int _i = 0; _i < 8; _i++) wrf80(_a + 28 + 10 * _i, *fst(c, _i));"
                               " c->fpu_top = 0; c->fpu_sw = 0; c->fpu_cw = 0x037Fu;")
                out.append(" }")
                return ["".join(out)]
            out = [f"{{ uint32_t _a = {a}; uint32_t _sw = rd32(_a + 4); c->fpu_sw = _sw & 0x4700u;"
                   " c->fpu_cw = rd32(_a) & 0xFFFFu;"]
            if m == "frstor":
                out.append(" c->fpu_top = (int)((_sw >> 11) & 7);"
                           " for (int _i = 0; _i < 8; _i++) *fst(c, _i) = rdf80(_a + 28 + 10 * _i);")
            out.append(" }")
            return ["".join(out)]
        if memop is not None and memop.size in (28, 94, 108):
            return [_todo(insn.address, f"x87 m{memop.size * 8}: {m} {insn.op_str}")]

        if m in ("fld",):
            if memop: return [f"fpush(c, {self._fmem(insn, memop, 'f')});"]
            return [f"fpush(c, *fst(c, {self._st_idx(ops[0])}));"]
        if m in ("fild",):
            return [f"fpush(c, {self._fmem(insn, memop, 'i')});"]
        if m in ("fldz",): return ["fpush(c, 0.0);"]
        if m in ("fld1",): return ["fpush(c, 1.0);"]
        # The other five constants the 8087 carries in microcode. A compiler
        # emits fldln2 and fldl2e for log() and exp() whenever it cannot call
        # the CRT helper, and fldpi turns up in any code that builds a rotation
        # - so "rare" they are not. Written to more digits than a double can
        # hold on purpose: the value that matters is the nearest double to the
        # real constant, not to a shortened decimal.
        if m in ("fldpi",):
            return ["fpush(c, 3.14159265358979323846264338327950288);"]
        if m in ("fldl2e",):
            return ["fpush(c, 1.44269504088896340735992468100189214);"]
        if m in ("fldl2t",):
            return ["fpush(c, 3.32192809488736234787031942948939018);"]
        if m in ("fldlg2",):
            return ["fpush(c, 0.301029995663981195213738894724493027);"]
        if m in ("fldln2",):
            return ["fpush(c, 0.693147180559945309417232121458176568);"]
        if m in ("fst", "fstp"):
            pop = "; fpop(c);" if m == "fstp" else ";"
            if memop:
                sz = memop.size; a = self.addr_expr(insn, memop)
                st = "wrf32" if sz == 4 else ("wrf80" if sz == 10 else "wrf64")
                return [f"{st}({a}, *fst(c, 0)){pop}"]
            return [f"*fst(c, {self._st_idx(ops[0])}) = *fst(c, 0){pop}"]
        # fist/fistp round by the control word's RC field. Most of RE5's are
        # MSVC's truncating idiom (`or eax, 0xC00; fldcw; fistp qword`), 1222
        # of them, plus inline floor (RC=01) and ceil (RC=10); rounding them all
        # to nearest put sound-engine positions and table indices one off.
        if m in ("fist", "fistp"):
            pop = "; fpop(c);" if m == "fistp" else ";"
            sz = memop.size; a = self.addr_expr(insn, memop)
            st = {2: "wri16", 4: "wri32", 8: "wri64"}[sz]
            return [f"{st}({a}, fpu_rint(c, *fst(c, 0))){pop}"]
        # fprem truncates the implied quotient, fprem1 rounds it to nearest; both
        # report its low three bits in C0/C3/C1 (the CRT's sin/cos use them to
        # pick the octant). Found by scripts/difftest_cpu.py: fprem1 was fprem.
        if m in ("fprem", "fprem1"):
            return [f"*fst(c, 0) = fprem_op(c, *fst(c, 0), *fst(c, 1), {1 if m == 'fprem1' else 0});"]
        if m.startswith("fcmov"):
            # Conditional move between x87 registers, on the integer flags the
            # preceding fucomi or test left behind. Same conditions as cmovcc,
            # spelled the FPU way: fcmovb/fcmove/fcmovbe/fcmovu and their
            # negations.
            tail = m[5:]
            cond = {"b": "c->cf", "e": "c->zf", "be": "(c->cf || c->zf)",
                    "u": "c->pf", "nb": "!c->cf", "ne": "!c->zf",
                    "nbe": "(!c->cf && !c->zf)", "nu": "!c->pf"}.get(tail)
            if cond:
                return [f"if ({cond}) *fst(c, 0) = *fst(c, {self._st_idx(ops[-1])});"]
        if m in ("fchs",): return ["*fst(c, 0) = -*fst(c, 0);"]
        if m in ("fabs",): return ["*fst(c, 0) = fabs(*fst(c, 0));"]
        if m in ("fsqrt",): return ["*fst(c, 0) = sqrt(*fst(c, 0));"]
        if m in ("fsin",):  return ["*fst(c, 0) = sin(*fst(c, 0));"]
        if m in ("fcos",):  return ["*fst(c, 0) = cos(*fst(c, 0));"]
        if m in ("fptan",): return ["*fst(c, 0) = tan(*fst(c, 0)); fpush(c, 1.0);"]
        if m in ("fpatan",):return ["*fst(c, 1) = atan2(*fst(c, 1), *fst(c, 0)); fpop(c);"]
        if m in ("frndint",): return ["*fst(c, 0) = fpu_rint(c, *fst(c, 0));"]
        if m in ("fscale",): return ["*fst(c, 0) = fpu_fscale(*fst(c, 0), *fst(c, 1));"]
        if m in ("fsincos",): return ["{ double _s=sin(*fst(c,0)), _c=cos(*fst(c,0)); *fst(c,0)=_s; fpush(c,_c); }"]
        # The rest of the 8087's transcendental set. These are not exotic: a
        # compiler builds log() out of `fldln2; fxch; fyl2x` and exp() out of
        # `fldl2e; fmul; ...; f2xm1; fscale`, so a game that takes a logarithm
        # anywhere reaches them. Each one consumes st(0) the way the manual
        # says, which is the only part that is easy to get wrong: fyl2x pops,
        # f2xm1 does not.
        if m in ("fyl2x",):        # st(1) = st(1) * log2(st(0)), pop
            return ["{ double _x = *fst(c, 0), _y = *fst(c, 1);"
                    " fpop(c); *fst(c, 0) = _y * log2(_x); }"]
        if m in ("fyl2xp1",):      # st(1) = st(1) * log2(st(0) + 1), pop
            return ["{ double _x = *fst(c, 0), _y = *fst(c, 1);"
                    " fpop(c); *fst(c, 0) = _y * log2(_x + 1.0); }"]
        if m in ("f2xm1",):        # st(0) = 2**st(0) - 1, no pop
            return ["*fst(c, 0) = pow(2.0, *fst(c, 0)) - 1.0;"]
        if m in ("fprem", "fprem1"):
            # Both leave the remainder in st(0) and do not pop. The difference
            # is the rounding of the implied quotient - truncating for fprem,
            # to-nearest for fprem1 - which is fmod and remainder exactly.
            f = "fmod" if m == "fprem" else "remainder"
            return [f"*fst(c, 0) = {f}(*fst(c, 0), *fst(c, 1));"]
        if m in ("fxtract",):      # st(0) -> exponent, then push the mantissa
            return ["{ int _e = 0; double _m = frexp(*fst(c, 0), &_e);"
                    " *fst(c, 0) = (double)(_e - 1); fpush(c, _m * 2.0); }"]
        if m in ("ftst",):         # compare st(0) with zero, flags only
            return ["fcompare(c, *fst(c, 0), 0.0);"]
        if m in ("fxam",):
            # Class of st(0) in C3 C2 C0 (NaN 001, normal 010, infinity 011,
            # zero 100), its sign in C1. The register stack is doubles (D9),
            # so two classes cannot happen: a double subnormal is a normal
            # number in the 80-bit format, and "empty" needs the tag word,
            # which this model does not keep.
            return ["{ double _v = *fst(c, 0); uint32_t _s = c->fpu_sw & ~0x4700u;"
                    " if (signbit(_v)) _s |= 0x0200u;"
                    " if (isnan(_v)) _s |= 0x0100u; else if (isinf(_v)) _s |= 0x0500u;"
                    " else if (_v == 0.0) _s |= 0x4000u; else _s |= 0x0400u;"
                    " c->fpu_sw = _s; }"]
        if m in ("fxch",):
            # ops[-1], not ops[0]. Capstone prints `fxch st(1)` but reports it
            # with BOTH registers, st(0) first, so ops[0] is always st(0) and
            # the swap this emitted was st(0) with st(0) - a no-op, for every
            # fxch in the program.
            #
            # It is the kind of wrong that does not look wrong: an fxch is
            # usually followed by an fstp, so the value that gets stored is
            # simply the other one, and the arithmetic downstream stays
            # plausible. In Mario Kart it turned a fixed-timestep accumulator
            # into an infinite loop - the game subtracted its step from the
            # wrong register, the step was negative, and the guest thread never
            # came out of the frame it was in.
            #
            # Found twice, independently, on two branches that reached the same
            # line of code. Which is the argument for one toolbox and not
            # three.
            i = self._st_idx(ops[-1]) if ops else 1
            return [f"{{ double _t = *fst(c, 0); *fst(c, 0) = *fst(c, {i}); *fst(c, {i}) = _t; }}"]
        if m in ("fadd","fsub","fsubr","fmul","fdiv","fdivr",
                 "faddp","fsubp","fsubrp","fmulp","fdivp","fdivrp"):
            pops = m.endswith("p"); base = m[:-1] if pops else m
            rev = base in ("fsubr","fdivr"); core = base[:-1] if rev else base
            opc = {"fadd":"+","fsub":"-","fmul":"*","fdiv":"/"}[core]
            if memop:                              # st0 OP= mem  (no pop for mem form)
                src = self._fmem(insn, memop, "f"); dst = "(*fst(c, 0))"
                expr = f"{src} {opc} {dst}" if rev else f"{dst} {opc} {src}"
                return [f"*fst(c, 0) = {expr};"]
            # register form. Which register is the DESTINATION depends on
            # whether this pops, and capstone prints one operand for both:
            #
            #   fadd  st(1)   (D8 C1)  st(0) = st(0) + st(1)   dst is st(0)
            #   faddp st(1)   (DE C1)  st(1) = st(1) + st(0)   dst is st(1)
            #
            # The popping forms name their destination, which is the opposite
            # of the non-popping ones, and treating them alike wrote the result
            # into the register the very next fpop threw away - so `faddp`
            # returned the operand it was supposed to have added to. Forty
            # thousand of them in one game.
            if len(ops) == 2:
                a = self._st_idx(ops[0]); b = self._st_idx(ops[1])
            elif pops:
                a = self._st_idx(ops[0]) if ops else 1; b = 0
            else:
                a = 0; b = self._st_idx(ops[0]) if ops else 1
            dst = f"(*fst(c, {a}))"; src = f"(*fst(c, {b}))"
            expr = f"{src} {opc} {dst}" if rev else f"{dst} {opc} {src}"
            line = f"*fst(c, {a}) = {expr};"
            return [line + (" fpop(c);" if pops else "")]
        if m in ("fiadd","fisub","fisubr","fimul","fidiv","fidivr"):
            src = self._fmem(insn, memop, "i")          # (double) of an integer mem operand
            core = "f" + m[2:]                          # fidiv -> fdiv, fiadd -> fadd, ...
            rev = core in ("fsubr","fdivr"); base = core[:-1] if rev else core
            opc = {"fadd":"+","fsub":"-","fmul":"*","fdiv":"/"}[base]
            dst = "(*fst(c, 0))"
            expr = f"{src} {opc} {dst}" if rev else f"{dst} {opc} {src}"
            return [f"*fst(c, 0) = {expr};"]
        if m in ("ficom","ficomp"):
            out = [f"fcompare(c, *fst(c, 0), {self._fmem(insn, memop, 'i')});"]
            if m == "ficomp": out.append("fpop(c);")
            return out
        # fcom and fucom are the same comparison. They differ only in which
        # NaNs raise an invalid-operation exception - a QNaN is quiet for
        # fucom and not for fcom - and this model does not raise FP exceptions
        # at all, so the translation is identical. Which matters a great deal
        # in practice: MSVC emits `fucompp; fnstsw ax; test ah` for an ordinary
        # float comparison, so the unordered forms are the common ones. In one
        # game (Mario Kart Arcade GP DX) `fucompp` alone was 40,796 of the
        # 50,555 instructions the lifter could not express - 81% of the entire
        # gap was this one mnemonic, missing because only `fcompp` was listed.
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            if memop: src = self._fmem(insn, memop, "f")
            elif ops: src = f"*fst(c, {self._st_idx(ops[0])})"
            else: src = "*fst(c, 1)"
            out = [f"fcompare(c, *fst(c, 0), {src});"]
            if m in ("fcomp", "fucomp"): out.append("fpop(c);")
            if m in ("fcompp", "fucompp"): out += ["fpop(c);", "fpop(c);"]
            return out
        # The P6 forms put the result in EFLAGS instead of the status word, so
        # the compiler can branch on it directly without `fnstsw`. Capstone
        # spells the popping ones both ways (`fcomip` and `fcompi`).
        if m in ("fcomi", "fucomi", "fcomip", "fucomip", "fcompi", "fucompi"):
            src = f"*fst(c, {self._st_idx(ops[-1])})" if ops else "*fst(c, 1)"
            out = [f"fcompare_eflags(c, *fst(c, 0), {src});"]
            if m != "fcomi" and m != "fucomi": out.append("fpop(c);")
            return out
        # The status word carries TOP in bits 11-13 (difftest_cpu.py: it was 0).
        if m == "fnstsw":
            sw = "(uint16_t)((c->fpu_sw & ~0x3800u) | ((uint32_t)(c->fpu_top & 7) << 11))"
            if ops and ops[0].type == X86_OP_REG:   # ax
                return [f"SET16(c->eax, {sw});"]
            return [self.wr(insn, ops[0], sw)]
        # The control word is real state now (c->fpu_cw): what the guest loads
        # is what fistp/frndint round by and what fnstcw reads back.
        if m == "fnstcw":
            return [self.wr(insn, ops[0], "(uint16_t)c->fpu_cw")]
        if m == "fldcw":
            return [f"c->fpu_cw = {self.rd(insn, ops[0])};"]
        if m == "fninit":
            return ["c->fpu_cw = 0x037Fu; c->fpu_sw = 0; c->fpu_top = 0;"]
        if m in ("fwait", "wait", "fnclex", "fclex"):
            return [f"/* {m} ignored */"]
        if m in ("fldenv", "fnstenv"):
            return [f"/* {m} ignored (no FP exceptions modelled) */"]
        return [_todo(insn.address, f"fpu {m} {insn.op_str}")]

    # ---- SSE ----
    def _is_xmm(self, op):
        return op.type == X86_OP_REG and self.md.reg_name(op.reg).startswith("xmm")

    def _xi(self, op):
        """xmm register index. 32-bit code has xmm0..xmm7 and no more."""
        name = self.md.reg_name(op.reg) if op.type == X86_OP_REG else "<mem>"
        if not name.startswith("xmm") or not name[3:].isdigit():
            raise NotImplementedError("not an xmm operand: %s" % name)
        return int(name[3:])

    def is_sse(self, insn):
        m = insn.mnemonic
        if m not in SSE_MNEMONICS and not SSE_CMP_RE.match(m):
            return False
        if m in SSE_AMBIGUOUS:
            return any(self._is_xmm(o) for o in insn.operands)
        return True

    def _ss(self, insn, op):
        """operand as a float (scalar single)"""
        if self._is_xmm(op): return f"c->xmm[{self._xi(op)}][0]"
        return f"rdss({self.addr_expr(insn, op)})"

    def _sd(self, insn, op):
        """operand as a double (scalar double)"""
        if self._is_xmm(op): return f"xmm_get_sd(c->xmm[{self._xi(op)}])"
        return f"rdsd({self.addr_expr(insn, op)})"

    def _xm(self, insn, op):
        """operand as a whole 128-bit register value"""
        if self._is_xmm(op): return f"c->xmm[{self._xi(op)}]"
        return f"rdxm({self.addr_expr(insn, op)})"

    def sse(self, insn):
        m = insn.mnemonic
        ops = insn.operands
        d = ops[0]
        s = ops[1] if len(ops) > 1 else None

        # ---- scalar moves ----
        if m in ("movss", "movsd"):
            wide = m == "movsd"
            if not wide:
                if self._is_xmm(d) and self._is_xmm(s):
                    return [f"c->xmm[{self._xi(d)}][0] = c->xmm[{self._xi(s)}][0];"]
                if self._is_xmm(d):
                    return [f"c->xmm[{self._xi(d)}] = sse_load_ss({self._ss(insn, s)});"]
                return [f"wrss({self.addr_expr(insn, d)}, c->xmm[{self._xi(s)}][0]);"]
            else:
                if self._is_xmm(d) and self._is_xmm(s):
                    return [f"c->xmm[{self._xi(d)}] = xmm_movsd_reg(c->xmm[{self._xi(d)}], c->xmm[{self._xi(s)}]);"]
                if self._is_xmm(d):
                    return [f"c->xmm[{self._xi(d)}] = sse_load_sd({self._sd(insn, s)});"]
                return [f"wrsd({self.addr_expr(insn, d)}, xmm_get_sd(c->xmm[{self._xi(s)}]));"]

        if m in ("pextrw", "pinsrw"):
            i = ops[2].imm & 7 if len(ops) > 2 else 0
            if m == "pextrw":
                return [self.dst_write(insn, d, f"((xv8w)c->xmm[{self._xi(s)}])[{i}]")]
            return [f"c->xmm[{self._xi(d)}] = xmm_pinsrw(c->xmm[{self._xi(d)}], {i}, (uint16_t)({self.src(insn, s)}));"]

        if m in ("unpcklpd", "unpckhpd", "unpcklps", "unpckhps"):
            src = self._xm(insn, s)
            xd = self._xi(d)
            lo = m.endswith("lpd") or m.endswith("lps")
            if m.endswith("pd"):
                fn = "xmm_unpcklpd" if lo else "xmm_unpckhpd"
                return [f"c->xmm[{xd}] = {fn}(c->xmm[{xd}], {src});"]
            i, j = (0, 1) if lo else (2, 3)
            return [f"{{ XMM _s = {src};"
                    f" c->xmm[{xd}] = XMM_SHUF4(c->xmm[{xd}], _s, {i}, {4 + i}, {j}, {4 + j}); }}"]

        if m == "movd":
            if self._is_xmm(d):
                return [f"c->xmm[{self._xi(d)}] = xmm_from_u32({self.src(insn, s)});"]
            return [self.dst_write(insn, d, f"xmm_to_u32(c->xmm[{self._xi(s)}])")]
        if m == "movq":
            if self._is_xmm(d) and self._is_xmm(s):
                return [f"c->xmm[{self._xi(d)}] = xmm_from_u64(xmm_to_u64(c->xmm[{self._xi(s)}]));"]
            if self._is_xmm(d):
                return [f"c->xmm[{self._xi(d)}] = xmm_from_u64(rd64({self.addr_expr(insn, s)}));"]
            return [f"wr64({self.addr_expr(insn, d)}, xmm_to_u64(c->xmm[{self._xi(s)}]));"]

        # ---- 128-bit moves ----
        if m in SSE_MOV128:
            if self._is_xmm(d):
                return [f"c->xmm[{self._xi(d)}] = {self._xm(insn, s)};"]
            return [f"wrxm({self.addr_expr(insn, d)}, c->xmm[{self._xi(s)}]);"]

        # ---- bitwise, on the whole register ----
        if m in SSE_BITWISE:
            op = SSE_BITWISE[m]
            n = self._xi(d)
            fn = {"&": "xmm_and", "|": "xmm_or", "^": "xmm_xor", "andn": "xmm_andn"}[op]
            return [f"c->xmm[{n}] = {fn}(c->xmm[{n}], {self._xm(insn, s)});"]

        # ---- packed integer & saturating ----
        if m in SSE_PMINMAX or m in SSE_PINT or m in SSE_PSAT or m in SSE_PCMP or m in ("pmullw", "pmaddwd") or m in SSE_PACK or m in SSE_PUNPCK:
            xd = self._xi(d)
            return [f"c->xmm[{xd}] = xmm_{m}(c->xmm[{xd}], {self._xm(insn, s)});"]

        if m in ("pmovmskb", "movmskps", "movmskpd"):
            xs = self._xm(insn, s)
            if m == "pmovmskb":
                return [self.dst_write(insn, d, f"(uint32_t)_mm_movemask_epi8((__m128i){xs})")]
            elif m == "movmskps":
                return [self.dst_write(insn, d, f"(uint32_t)_mm_movemask_ps((__m128){xs})")]
            else:
                return [self.dst_write(insn, d, f"(uint32_t)_mm_movemask_pd((__m128d){xs})")]

        if m in ("pshuflw", "pshufhw"):
            imm = ops[2].imm if len(ops) > 2 else 0
            xd = self._xi(d)
            fn = "_mm_shufflehi_epi16" if m == "pshufhw" else "_mm_shufflelo_epi16"
            return [f"c->xmm[{xd}] = (XMM){fn}((__m128i){self._xm(insn, s)}, {imm & 0xFF});"]

        if m == "cvtdq2pd":
            xd = self._xi(d)
            return [f"c->xmm[{xd}] = xmm_cvtdq2pd({self._xm(insn, s)});"]

        if m in ("movlps", "movlpd", "movhps", "movhpd"):
            lane = 1 if m.startswith("movh") else 0
            if self._is_xmm(d):
                return [f"c->xmm[{self._xi(d)}] = xmm_set_qword(c->xmm[{self._xi(d)}], {lane}, rd64({self.addr_expr(insn, s)}));"]
            return [f"wr64({self.addr_expr(insn, d)}, xmm_get_qword(c->xmm[{self._xi(s)}], {lane}));"]

        if m in SSE_SHIFT:
            xd = self._xi(d)
            if s.type == X86_OP_IMM:
                return [f"c->xmm[{xd}] = xmm_{m}_imm(c->xmm[{xd}], {s.imm & 0xFF}u);"]
            else:
                return [f"c->xmm[{xd}] = xmm_{m}(c->xmm[{xd}], {self._xm(insn, s)});"]

        if m in ("pslldq", "psrldq"):
            xd = self._xi(d)
            k = min(s.imm & 0xFF, 16)
            return [f"c->xmm[{xd}] = xmm_{m}(c->xmm[{xd}], {k}u);"]

        if m in ("shufps", "shufpd", "pshufd"):
            imm = ops[2].imm if len(ops) > 2 else 0
            xd = self._xi(d)
            if m == "shufpd":
                return [f"c->xmm[{xd}] = xmm_shufpd(c->xmm[{xd}], {self._xm(insn, s)}, {imm & 3});"]
            else:
                sel = [(imm >> (2 * i)) & 3 for i in range(4)]
                idx = ([sel[0], sel[1], 4 + sel[2], 4 + sel[3]] if m == "shufps"
                       else [4 + k for k in sel])
                return [f"{{ XMM _s = {self._xm(insn, s)};"
                        f" c->xmm[{xd}] = XMM_SHUF4(c->xmm[{xd}], _s, {idx[0]}, {idx[1]}, {idx[2]}, {idx[3]}); }}"]

        # ---- packed arithmetic ----
        if m[:-2] in SSE_ARITH and m[-2:] in ("ps", "pd"):
            xd = self._xi(d)
            return [f"c->xmm[{xd}] = xmm_{m}(c->xmm[{xd}], {self._xm(insn, s)});"]
        if m in ("sqrtps", "sqrtpd"):
            fn = "_mm_sqrt_pd" if m == "sqrtpd" else "_mm_sqrt_ps"
            ty = "(__m128d)" if m == "sqrtpd" else "(__m128)"
            return [f"c->xmm[{self._xi(d)}] = (XMM){fn}({ty}{self._xm(insn, s)});"]
        if m in ("minps", "maxps", "minpd", "maxpd"):
            return [f"c->xmm[{self._xi(d)}] = xmm_{m}(c->xmm[{self._xi(d)}], {self._xm(insn, s)});"]

        # ---- scalar arithmetic ----
        if m[:-2] in SSE_ARITH and m[-2:] in ("ss", "sd"):
            wide = m[-2:] == "sd"
            xd = self._xi(d)
            if not wide:
                op = SSE_ARITH[m[:-2]]
                return [f"c->xmm[{xd}][0] = c->xmm[{xd}][0] {op} {self._ss(insn, s)};"]
            else:
                fn = f"xmm_{m[:-2]}sd"
                return [f"c->xmm[{xd}] = {fn}(c->xmm[{xd}], {self._sd(insn, s)});"]
        if m in ("sqrtss", "sqrtsd"):
            xd = self._xi(d)
            if m == "sqrtss":
                return [f"c->xmm[{xd}][0] = sqrtf({self._ss(insn, s)});"]
            else:
                return [f"c->xmm[{xd}] = xmm_sqrtsd(c->xmm[{xd}], {self._sd(insn, s)});"]
        if m in ("minss", "maxss", "minsd", "maxsd"):
            xd = self._xi(d)
            if m.endswith("ss"):
                fn = "sse_minf" if m.startswith("min") else "sse_maxf"
                return [f"c->xmm[{xd}][0] = {fn}(c->xmm[{xd}][0], {self._ss(insn, s)});"]
            else:
                fn = "xmm_minsd" if m.startswith("min") else "xmm_maxsd"
                return [f"c->xmm[{xd}] = {fn}(c->xmm[{xd}], {self._sd(insn, s)});"]
        if m in ("rcpss", "rsqrtss"):
            fn = "1.0f / " if m == "rcpss" else "1.0f / sqrtf"
            return [f"c->xmm[{self._xi(d)}][0] = {fn}({self._ss(insn, s)});"]
        if m in ("rcpps", "rsqrtps"):
            fn = "_mm_rcp_ps" if m == "rcpps" else "_mm_rsqrt_ps"
            return [f"c->xmm[{self._xi(d)}] = (XMM){fn}((__m128){self._xm(insn, s)});"]

        # ---- compares that set flags ----
        if m in ("ucomiss", "comiss", "ucomisd", "comisd"):
            wide = m.endswith("sd")
            d_val = f"xmm_get_sd(c->xmm[{self._xi(d)}])" if wide else f"c->xmm[{self._xi(d)}][0]"
            s_val = self._sd(insn, s) if wide else self._ss(insn, s)
            return [f"sse_compare(c, {d_val}, {s_val});"]

        # ---- compares that write a mask ----
        mm = SSE_CMP_RE.match(m)
        if mm and mm.group(2) in ("ps", "pd"):
            pred_idx = list(SSE_CMP_PRED.keys()).index(mm.group(1))
            wide = mm.group(2) == "pd"
            fn = "xmm_cmp_pd" if wide else "xmm_cmp_ps"
            return [f"c->xmm[{self._xi(d)}] = {fn}(c->xmm[{self._xi(d)}], {self._xm(insn, s)}, {pred_idx});"]
        if mm:
            pred_idx = list(SSE_CMP_PRED.keys()).index(mm.group(1))
            wide = mm.group(2) == "sd"
            fn = "xmm_cmp_sd" if wide else "xmm_cmp_ss"
            d_val = f"xmm_get_sd(c->xmm[{self._xi(d)}])" if wide else f"c->xmm[{self._xi(d)}][0]"
            s_val = self._sd(insn, s) if wide else self._ss(insn, s)
            return [f"c->xmm[{self._xi(d)}] = {fn}(c->xmm[{self._xi(d)}], {d_val}, {s_val}, {pred_idx});"]

        # ---- conversions ----
        if m == "cvtsi2ss":
            return [f"c->xmm[{self._xi(d)}][0] = (float)(int32_t)({self.src(insn, s)});"]
        if m == "cvtsi2sd":
            return [f"c->xmm[{self._xi(d)}] = xmm_cvtsi2sd(c->xmm[{self._xi(d)}], (int32_t)({self.src(insn, s)}));"]
        if m in ("cvttss2si", "cvtss2si"):
            fn = "sse_cvtt_i32" if m.startswith("cvtt") else "sse_cvtt_i32(nearbyintf"
            tail = ")" if m.startswith("cvtt") else "))"
            return [self.dst_write(insn, d, f"{fn}({self._ss(insn, s)}{tail}")]
        if m in ("cvttsd2si", "cvtsd2si"):
            fn = "sse_cvtt_i32" if m.startswith("cvtt") else "sse_cvtt_i32(nearbyint"
            tail = ")" if m.startswith("cvtt") else "))"
            return [self.dst_write(insn, d, f"{fn}({self._sd(insn, s)}{tail}")]
        if m == "cvtss2sd":
            return [f"c->xmm[{self._xi(d)}] = xmm_cvtss2sd(c->xmm[{self._xi(d)}], {self._ss(insn, s)});"]
        if m == "cvtsd2ss":
            return [f"c->xmm[{self._xi(d)}][0] = (float)({self._sd(insn, s)});"]
        if m in ("cvtdq2ps", "cvtps2dq", "cvttps2dq"):
            return [f"c->xmm[{self._xi(d)}] = xmm_{m}({self._xm(insn, s)});"]
        if m == "cvtps2pd":
            n = self._xi(d)
            if self._is_xmm(s):
                j = self._xi(s)
                return [f"c->xmm[{n}] = xmm_cvtps2pd(c->xmm[{j}][0], c->xmm[{j}][1]);"]
            return [f"{{ uint32_t _p = {self.addr_expr(insn, s)};"
                    f" c->xmm[{n}] = xmm_cvtps2pd(rdss(_p), rdss(_p + 4u)); }}"]
        if m == "cvtpd2ps":
            return [f"c->xmm[{self._xi(d)}] = xmm_cvtpd2ps({self._xm(insn, s)});"]

        return [_todo(insn.address, f"sse {m} {insn.op_str}")]

    def _read_dst(self, insn, op):
        # read a dst operand (for read-modify-write)
        if op.type == X86_OP_REG: return reg_read(self.md.reg_name(op.reg))
        if op.type == X86_OP_MEM: return self.rd(insn, op)
        raise NotImplementedError

    def _read_raw(self, insn, op):
        return self.src(insn, op)

    def _target(self, insn, op):
        if op.type == X86_OP_IMM: return f"0x{op.imm:08X}u"
        if op.type == X86_OP_REG: return reg_read(self.md.reg_name(op.reg))
        if op.type == X86_OP_MEM: return self.rd(insn, op)
        raise NotImplementedError

    def _cond(self, m):
        return {
            "je":"c->zf","jz":"c->zf","jne":"!c->zf","jnz":"!c->zf",
            "jbe":"(c->cf || c->zf)","jna":"(c->cf || c->zf)",
            "ja":"(!c->cf && !c->zf)","jnbe":"(!c->cf && !c->zf)",
            "jb":"c->cf","jc":"c->cf","jnae":"c->cf",
            "jae":"!c->cf","jnb":"!c->cf","jnc":"!c->cf",
            "jl":"(c->sf != c->of)","jnge":"(c->sf != c->of)",
            "jge":"(c->sf == c->of)","jnl":"(c->sf == c->of)",
            "jle":"(c->zf || (c->sf != c->of))","jng":"(c->zf || (c->sf != c->of))",
            "jg":"(!c->zf && (c->sf == c->of))","jnle":"(!c->zf && (c->sf == c->of))",
            "js":"c->sf","jns":"!c->sf","jo":"c->of","jno":"!c->of",
            "jp":"c->pf","jpe":"c->pf","jnp":"!c->pf","jpo":"!c->pf",
            "jecxz":"(c->ecx == 0)","jcxz":"(R16(c->ecx) == 0)",
        }.get(m)

    def lift_function(self, code, start):
        # Per function, not per Lifter. Two lifted functions can cover the same
        # `jmp [table]` - carving by descent produces overlapping extents - and
        # a stale entry here makes the second one emit `goto` to labels that
        # only exist in the first, which the C compiler rejects outright.
        self.jumptables = {}
        insns = list(_disasm(self.md, code, start))
        # collect intra-function branch targets
        end = start + len(code)

        # An extent that ends in the middle of an instruction.
        #
        # Clamping a recovered function against the next catalog entry does
        # this whenever that entry is false, and on a stripped binary some
        # always are. Capstone then stops one instruction early, and the
        # function's fall-through address is *inside* a real instruction:
        # dispatching there decodes its tail as something else. `74 5B` - a
        # two-byte `je` - read from its second byte is `pop ebx`, and a guest
        # stack one slot out is the worst failure this project can produce.
        #
        # The bytes are in the image either way, so read the few more that
        # finish the instruction and keep it. The fall-through then lands where
        # the program's own code does, which is somewhere with a body.
        if insns and self.read_va:
            tail = insns[-1].address + insns[-1].size
            if tail < end:
                try:
                    extra = self.read_va(tail, (end - tail) + 15)
                except Exception:
                    extra = b""
                for ins in _disasm(self.md, extra, tail):
                    if ins.address >= end:
                        break
                    insns.append(ins)
                end = max(end, insns[-1].address + insns[-1].size)

        self.func_start, self.func_end = start, end
        insn_addrs = set(ins.address for ins in insns)   # only these can be goto labels
        labels = set()
        for ins in insns:
            if ins.mnemonic.startswith("j") or ins.mnemonic in ("loop","loopne","loope","loopz","loopnz","loope"):
                for op in ins.operands:
                    # only label real instruction boundaries; targets that land
                    # mid-instruction or outside the func are handled via dispatch
                    if op.type == X86_OP_IMM and op.imm in insn_addrs:
                        labels.add(op.imm)
                    elif op.type == X86_OP_MEM and ins.mnemonic == "jmp":
                        d = op.mem.disp & 0xffffffff      # jump table base
                        # A switch indexes: `jmp [reg*4 + table]`. Without the
                        # scaled index this is `jmp [__imp_memcpy]` - an IAT
                        # thunk, of which a PE has hundreds - and walking the
                        # import table as if it were a jump table invents
                        # thousands of branch targets out of hint RVAs.
                        if (op.mem.index and op.mem.scale == 4 and
                                self.in_image(d)):
                            tgts = self.resolve_jumptable(d)
                            if tgts:
                                self.jumptables[ins.address] = tgts
                                # Only the arms that are instruction boundaries
                                # in THIS function can be goto labels; the rest
                                # are dispatched, see the emitter.
                                labels.update(t for t in tgts if t in insn_addrs)
        out = []
        out.append(f"void L_{start:08X}(CPU *c)")
        out.append("{")
        items = [(ins, self.translate(ins, labels)) for ins in insns]
        below = getattr(self, "flags_dead_ret_below", None)
        masks = flag_liveness(items, labels, dead_at_ret=below is not None and start < below)
        items = [(ins, [_apply_masks(l, mask) for l in lines]) for (ins, lines), mask in zip(items, masks)]
        cache = getattr(self, "cache_registers", True) and bool(items)
        if cache:
            items, decl = cache_registers(items, flag_liveness.succ)
            out.append(f"    {decl}")
        for ins, lines in items:
            if ins.address in labels:
                out.append(f"L_{ins.address:08X}:")
            for line in lines:
                out.append(f"    {line:<60} /* {ins.address:08X}: {ins.mnemonic} {ins.op_str} */")

        # A body whose last instruction is not a transfer falls THROUGH into
        # whatever follows it, and on x86 that is a real control transfer - so
        # the C function must make it one rather than just return. Returning
        # instead skips the callee's `ret`, leaves esp four bytes low, and the
        # caller resumes reading its own frame one slot out. That is the worst
        # class of bug this project can produce: nothing faults, and every
        # value after it is off by one slot.
        #
        # It happens whenever the extent is shorter than the real function:
        # a bounds file that disagrees, a catalog clamped to the next function
        # start (disasm32.clamp_extents), or a decode that stopped early on
        # bytes capstone would not take.
        # `repz ret` is a plain ret with an F3 prefix, so compare the last word
        # of the mnemonic, not the whole thing.
        last = insns[-1].mnemonic.split()[-1] if insns else None
        if last not in ("ret", "retn", "retf", "jmp", "iret", "iretd", "hlt"):
            # After the last instruction decoded, NOT at the end of the extent.
            # They differ exactly when the extent cuts an instruction in half -
            # a clamp to a false function start does that - and then the extent
            # end is an address in the middle of a real instruction. Dispatching
            # there decodes the tail of it as something else: `74 5B`, a two-byte
            # `je`, becomes a one-byte `pop ebx` at the second byte, and the
            # guest stack is one slot out from then on with nothing to show for
            # it. Mario Kart lost a pointer to a memcpy that way.
            nxt = insns[-1].address + insns[-1].size if insns else end
            out.append(f"    /* extent ends mid-function: fall through */")
            if cache:
                out.append("    " + " ".join(_store(r) for r in CACHED))
            out.append(f"    dispatch(c, 0x{nxt:08X}u); return;")

        out.append("}")
        return "\n".join(out)


def load_bounds(path):
    bounds = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0].startswith("0x"):
                bounds[int(parts[0], 16)] = int(parts[1])
    return bounds


def main():
    dll, funcs_txt, out_c = sys.argv[1], sys.argv[2], sys.argv[3]
    # targets: explicit 0xADDR args, or "@file" of addrs, or (none) => ALL in funcs_txt
    if len(sys.argv) >= 5 and sys.argv[4].startswith("@"):
        targets = [int(x, 16) for x in open(sys.argv[4][1:]).read().split()]
    else:
        targets = [int(x, 16) for x in sys.argv[4:]]
    import pefile
    pe = pefile.PE(dll, fast_load=True)
    global IMAGE_BASE
    IMAGE_BASE = pe.OPTIONAL_HEADER.ImageBase
    image_size = pe.OPTIONAL_HEADER.SizeOfImage
    print(f"[*] image base {IMAGE_BASE:#x}, size {image_size:#x}", file=sys.stderr)
    # flat reader: VA -> bytes
    def read_va(va, n):
        rva = va - IMAGE_BASE
        return pe.get_data(rva, n)
    # Base relocations: the set of VAs holding a 32-bit absolute address that the
    # loader fixes up. An instruction immediate at such a VA is a relocatable
    # address (e.g. `push offset table`) and must be emitted as GVA(imm) so the
    # lifted code is correct at any load base.
    reloc_vas = set()
    try:
        import pefile as _pf
        pe.parse_data_directories(directories=[_pf.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        for br in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in br.entries:
                if e.type == 3:   # IMAGE_REL_BASED_HIGHLOW
                    reloc_vas.add((IMAGE_BASE + e.rva) & 0xffffffff)
    except Exception as ex:
        print(f"[!] reloc parse failed: {ex}", file=sys.stderr)
    print(f"[*] {len(reloc_vas)} base relocations (address immediates -> GVA)", file=sys.stderr)
    bounds = load_bounds(funcs_txt)
    if not targets:                       # no addrs given => lift every function
        targets = sorted(bounds)
        print(f"[*] no targets given; lifting ALL {len(targets)} functions", file=sys.stderr)
    lifter = Lifter(dll, image_size, read_va)
    lifter.reloc_vas = reloc_vas

    chunks = ["/* AUTO-GENERATED by lift32_cpu.py - do not edit */",
              '#include "cpu.h"', ""]
    done = []
    for t in targets:
        size = bounds.get(t)
        if not size:
            print(f"[!] no size for {t:#x}", file=sys.stderr); continue
        code = read_va(t, size)
        chunks.append(lifter.lift_function(code, t))
        chunks.append("")
        done.append(t)
        print(f"[+] lifted L_{t:08X} ({size} bytes)", file=sys.stderr)
    with open(out_c, "w") as f:
        f.write("\n".join(chunks))
    # companion X-macro listing every lifted function (auto-syncs the dispatch table)
    list_h = out_c.rsplit(".", 1)[0] + "_list.h"
    with open(list_h, "w") as f:
        f.write("/* AUTO-GENERATED by lift32_cpu.py */\n#define LIFTED_FUNCS(X) \\\n")
        f.write(" \\\n".join(f"    X({t:08X})" for t in done) + "\n")
    print(f"[*] wrote {out_c} and {list_h} ({len(done)} funcs)", file=sys.stderr)


def selftest():
    """The x87 forms where capstone's operand list is a trap.

    difftest.py runs lifted C against Unicorn, and it covers lift32.py - the
    global-register lifter - not this one. Both of the bugs below had already
    been found and fixed there and came back here, so this is the smallest
    check that says so: lift the bytes and read the C.

    Text assertions, because the thing being tested is a code generator and the
    text is its output. Run it directly:  python tools/lift/lift32_cpu.py --selftest
    """
    lf = Lifter(None, 0x100000)
    def emit(code):
        insn = next(lf.md.disasm(code, 0x401000))
        return " ".join(lf.fpu(insn))

    # fxch st(N) is reported as [st(0), st(N)]: the first operand is always
    # st(0), so an emitter reading ops[0] swaps st(0) with itself and every
    # fxch in the program does nothing.
    for code, want in ((b"\xd9\xc9", 1), (b"\xd9\xca", 2), (b"\xd9\xcd", 5)):
        out = emit(code)
        assert f"*fst(c, {want})" in out and "fst(c, 0) = *fst(c, 0)" not in out, \
            "fxch st(%d) lifted as a no-op: %s" % (want, out)

    # The popping arithmetic names its DESTINATION - `faddp st(1)` is
    # st(1) += st(0) - which is the opposite of `fadd st(1)`, st(0) += st(1).
    # Writing to st(0) puts the result in the slot fpop() then discards.
    assert emit(b"\xde\xc1").startswith("*fst(c, 1) ="), \
        "faddp st(1) writes the wrong register: " + emit(b"\xde\xc1")
    assert emit(b"\xde\xca").startswith("*fst(c, 2) ="), \
        "fmulp st(2) writes the wrong register: " + emit(b"\xde\xca")
    # ...while the non-popping form still lands in st(0).
    assert emit(b"\xd8\xc1").startswith("*fst(c, 0) ="), \
        "fadd st(1) writes the wrong register: " + emit(b"\xd8\xc1")
    # fsubrp st(1): st(1) = st(0) - st(1), reversed as well as redirected.
    out = emit(b"\xde\xe1")
    assert out.startswith("*fst(c, 1) =") and "(*fst(c, 0)) - (*fst(c, 1))" in out, \
        "fsubrp st(1) is wrong: " + out

    print("lift32_cpu x87 selftest: ok")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    else:
        main()
