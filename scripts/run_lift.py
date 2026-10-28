#!/usr/bin/env python3
"""Fase 2 — driver do lifting x86-32 -> C (pcrecomp disasm32 + lift32_cpu).

    run_lift.py catalog [--exe EXE] [--seeds a.json,b.txt] [--out work/catalog.json]
        Descoberta de funções por descida recursiva (disasm32) a partir do
        entry point, de todo alvo direto de call/jmp e das sementes extras
        (RTTI, vtables). Grava o catálogo: endereço, fim e tipo de cada função.

    run_lift.py catalog --add-seeds work/runtime_seeds.txt
        Etapa 2.6: acrescenta ao catálogo existente só os endereços que o runtime
        despachou e não achou (RE5_SEED_LOG), em segundos, sem a descoberta inteira.

    run_lift.py lift [--exe EXE] [--catalog work/catalog.json] [--out generated/src]
                     [--per-file N | --bytes-per-file 0x10000] [--prefix re5]
        Faz o lift de cada função do catálogo com lift32_cpu e grava:
          <prefix>_NNNN.c           as funções de uma janela de endereços (lift.bytes_per_file)
                                    ou N por arquivo; só arquivos que mudaram são regravados
          <prefix>_funcs.h          as declarações
          <prefix>_dispatch_table.c tabela VA -> função, ordenada (busca binária)
          lift_stats.json           contagens + censo de RECOMP_TODO por mnemônico

Os dois comandos recusam um executável que o scripts/recon.py não aprova
(SteamStub, Enigma ou outro empacotador ainda presente): fazer lift de código
criptografado gera milhões de linhas de lixo que compilam. --allow-protected
existe só para experimentos e nunca deve gerar o código que vai para o build.

Tudo que este script escreve é derivado do binário do jogo e fica fora do git.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys
import time

import re5common as rc

rc.add_pcrecomp_to_path("pe", "disasm", "lift")


def resolve_exe(args, cfg):
    exe = args.exe or rc.repo_path(cfg["input"]["clean"])
    if not os.path.exists(exe):
        sys.exit(f"[!] {exe} não existe. O lifter precisa do executável limpo "
                 "(docs/PLANO_DO_PROJETO.md, Fase 1, etapa 1.1).")
    if rc.is_lfs_pointer(exe):
        sys.exit(f"[!] {exe} é um ponteiro Git LFS: rode `git lfs pull`.")
    return exe


def gate(exe, allow_protected):
    import recon
    r = recon.analyze(exe, rc.load_known_builds())
    if r["liftable"]:
        return r
    msg = f"[!] {exe}: camadas de proteção presentes: {', '.join(r['layers'])}"
    if not allow_protected:
        sys.exit(msg + "\n    O lifter só aceita código limpo. Ver docs/PLANO_DO_PROJETO.md, etapa 1.1.")
    print(msg + " (--allow-protected: saída NÃO serve para o build)", file=sys.stderr)
    return r


def read_seed_files(spec):
    """Sementes de arquivos separados por vírgula: JSON (lista de VAs ou de
    {"address": VA}) ou texto com um VA por linha ('#' comenta)."""
    seeds = set()
    for path in filter(None, (spec or "").split(",")):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if path.endswith(".json"):
            for e in json.loads(text):
                a = e.get("address") if isinstance(e, dict) else e
                seeds.add(int(a, 0) if isinstance(a, str) else a)
        else:
            for line in text.splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    seeds.add(int(line.split()[0], 16))
    return seeds


def extend_catalog(dis, iat, functions, seeds, code_start, code_end, max_rounds=8):
    """Acrescenta ao catálogo as entradas que o runtime pediu e não achou
    (etapa 2.6), sem refazer a descoberta inteira.

    Cada semente vira uma entrada `alias`: um corpo decodificado a partir dela,
    cortado na próxima entrada `start`, que pode sobrepor a função que a
    contém (é o caso típico: a descoberta pôs um início falso no meio de uma
    instrução e cortou a função verdadeira ali). Alias não serve de limite de
    corte, então nenhuma função existente muda. Os desvios diretos do corpo
    novo que saem dele também precisam de entrada, e entram pelo mesmo caminho.

    `functions` é a lista do catálogo, editada no lugar. Devolve os VAs novos.
    """
    import bisect
    by_addr = {f["address"]: f for f in functions}
    starts = sorted(a for a, f in by_addr.items() if f.get("entry_kind") != "alias")
    added = []
    work = sorted(seeds)
    for _ in range(max_rounds):
        nxt = set()
        for va in work:
            if va in by_addr or not code_start <= va < code_end:
                continue
            fn = dis.disassemble_function(va, iat)
            if not fn or not fn.blocks:
                continue
            i = bisect.bisect_right(starts, va)
            end = min(fn.end, starts[i] if i < len(starts) else code_end)
            by_addr[va] = {"address": va, "end": end, "size": end - va, "entry_kind": "alias", "is_thunk": False}
            added.append(va)
            for blk in fn.blocks.values():
                for ins in blk.instructions:
                    if not (ins.is_jump or ins.is_call):
                        continue
                    t = ins.get_branch_target()
                    if t is not None and not va <= t < end and t not in by_addr:
                        nxt.add(t)
        if not nxt:
            break
        work = sorted(nxt)
    functions[:] = sorted(by_addr.values(), key=lambda f: f["address"])
    return added


def fix_label_starts(dis, iat, functions, code_end, max_rounds=8):
    """Inícios falsos que são rótulos da função anterior (disasm32.label_reentries):
    viram `alias` e a função anterior volta ao tamanho de verdade, cortada só na
    próxima entrada `start`. Caso que motivou: o laço principal de uma thread do
    RE5 (0x00447080) cortado em 0x0044744D; cada volta do laço aninhava uma
    chamada no host até estourar a pilha (2026-10-24).

    `functions` é a lista do catálogo, editada no lugar. Devolve [(função, rótulo)].
    """
    from disasm32 import label_reentries
    by_addr = {f["address"]: f for f in functions}
    fixed = []
    for _ in range(max_rounds):
        starts = sorted(a for a, f in by_addr.items() if f.get("entry_kind") != "alias")
        found = []
        for fa, xa in zip(starts, starts[1:]):
            if by_addr[fa]["end"] != xa:
                continue                 # não foi cortada nesta entrada
            ff = dis.disassemble_function(fa, iat)
            if not ff or not ff.blocks or ff.end <= xa:
                continue
            fx = dis.disassemble_function(xa, iat)
            if fx and fx.blocks and label_reentries(ff, fx):
                found.append((fa, xa, ff.end))
        if not found:
            break
        for fa, xa, natural in found:
            by_addr[xa]["entry_kind"] = "alias"
            fixed.append((fa, xa))
        starts = sorted(a for a, f in by_addr.items() if f.get("entry_kind") != "alias")
        import bisect
        for fa, xa, natural in found:
            i = bisect.bisect_right(starts, fa)
            end = min(natural, starts[i] if i < len(starts) else code_end)
            by_addr[fa]["end"], by_addr[fa]["size"] = end, end - fa
    return fixed


def cmd_catalog(args, cfg):
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler

    exe = resolve_exe(args, cfg)
    gate(exe, args.allow_protected)
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    out = args.out or rc.repo_path(cfg["lift"]["catalog"])
    if args.fix_labels:
        with open(out, encoding="utf-8") as f:
            doc = json.load(f)
        if doc.get("sha256") != rc.sha256_file(exe):
            sys.exit("[!] o catálogo existente é de outro executável: rode `run_lift.py catalog` completo.")
        with open(exe, "rb") as f:
            dis = Disassembler(f.read(), info.image_base, info.sections)
        fixed = fix_label_starts(dis, iat, doc["functions"], info.code_end)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        print(f"[*] {len(fixed)} inícios falsos viraram alias: "
              f"{', '.join(f'0x{x:08X} (em 0x{f:08X})' for f, x in fixed[:20])}{' ...' if len(fixed) > 20 else ''}")
        return
    if args.add_seeds:
        with open(out, encoding="utf-8") as f:
            doc = json.load(f)
        if doc.get("sha256") != rc.sha256_file(exe):
            sys.exit("[!] o catálogo existente é de outro executável: rode `run_lift.py catalog` completo.")
        with open(exe, "rb") as f:
            dis = Disassembler(f.read(), info.image_base, info.sections)
        added = extend_catalog(dis, iat, doc["functions"], read_seed_files(args.add_seeds),
                               info.code_start, info.code_end)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        print(f"[*] {len(added)} entradas novas: {', '.join(f'0x{a:08X}' for a in added[:20])}"
              f"{' ...' if len(added) > 20 else ''}")
        print(f"[*] escrito {out} ({len(doc['functions'])} funções)")
        return
    seeds = {info.image_base + info.entry_point_rva} | read_seed_files(args.seeds)
    data = open(exe, "rb").read()
    dis = Disassembler(data, info.image_base, info.sections)
    t = time.time()
    funcs = dis.find_functions(info.code_start, info.code_end, iat, seeds=seeds, release_operands=True)
    print(f"[*] {len(funcs)} funções em {time.time() - t:.0f}s")
    doc = {
        "exe": os.path.basename(exe),
        "sha256": rc.sha256_file(exe),
        "image_base": info.image_base,
        "code_start": info.code_start,
        "code_end": info.code_end,
        "entry": info.image_base + info.entry_point_rva,
        "functions": [
            {"address": f.address, "end": f.end, "size": f.size,
             "entry_kind": f.entry_kind, "is_thunk": f.is_thunk}
            for f in sorted(funcs.values(), key=lambda f: f.address)
        ],
    }
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    json.dump(doc, open(out, "w"))
    print(f"[*] escrito {out}")


# Definição de uma função gerada (o lift32_cpu escreve `void L_<VA>(CPU *c)`).
SIG_RE = re.compile(r"^void (L_[0-9A-F]{8})\(CPU \*c\)$", re.M)
LABEL_RE = re.compile(r"^(L_[0-9A-F]{8}):$", re.M)
GOTO_RE = re.compile(r"goto (L_[0-9A-F]{8});")


# Laço de espera pura: o corpo só lê a memória (rd*) e compara, e nenhum registrador que ele
# escreve entra no endereço de uma leitura (sem ponteiro que avança, como num strlen).
_IMPURE_RE = re.compile(r"\b(wr\w*|push32\w*|pop32\w*|RE5_CALL|RE5_TAILCALL|dispatch\w*|rep_\w+|RECOMP_\w+|re5_\w+"
                        r"|FS_WR\w*|fst|fpu\w*|fpush|fpop|op_\w+|bit_string_op)\(")
_ASSIGN_REG_RE = re.compile(r"\b(R_(?:eax|ecx|edx|ebx|esp|ebp|esi|edi))\s*(?:=(?!=)|[-+*/&|^]=|<<=|>>=|\+\+|--)")
_RD_RE = re.compile(r"\brd\w*\(([^;]*?)\)\s*[;)&|^=!<>,]")


def is_wait_loop(region):
    if _IMPURE_RE.search(region) or "c->" in region.replace("(c,", ""):
        return False
    if not re.search(r"\brd(8|16|32)\(", region):
        return False
    written = {m.group(1) for m in _ASSIGN_REG_RE.finditer(region)}
    for m in re.finditer(r"\brd(?:8|16|32|64)\(", region):
        depth, i = 1, m.end()
        while depth and i < len(region):
            depth += {"(": 1, ")": -1}.get(region[i], 0)
            i += 1
        if any(w in region[m.end():i] for w in written):
            return False
    return True


def loop_barriers(text, spin=False):
    """RE5_LOOP_BARRIER() logo depois de cada rótulo que é alvo de um goto para
    trás (cabeça de laço), função por função. Um laço do jogo que só lê uma flag
    na memória (espera ocupada por outra thread) não tem escrita nenhuma que o
    compilador precise respeitar: com o restrict ele içaria a leitura para fora
    do laço e giraria para sempre. A barreira obriga a reler a cada volta."""
    out = []
    for part in re.split(r"(?=^void L_[0-9A-F]{8}\()", text, flags=re.M):
        heads, seen, waits = set(), {}, set()
        for m in re.finditer(r"^(L_[0-9A-F]{8}):$|goto (L_[0-9A-F]{8});", part, re.M):
            if m.group(1):
                seen[m.group(1)] = m.end()
            elif m.group(2) in seen:
                heads.add(m.group(2))
                if spin and is_wait_loop(part[seen[m.group(2)]:m.start()]):
                    waits.add(m.group(2))
        if heads:
            part = LABEL_RE.sub(lambda m: m.group(0) + (
                (" RE5_SPIN_BARRIER();" if m.group(1) in waits else " RE5_LOOP_BARRIER();") if m.group(1) in heads else ""), part)
        out.append(part)
    return "".join(out)


# call/jmp com alvo constante, como o lift32_cpu o escreve.
DIRECT_RE = re.compile(r"dispatch\(c, 0x([0-9A-F]{8})u\)")
TAIL_RE = re.compile(r"dispatch_tail\(c, 0x([0-9A-F]{8})u\)")
TODO_RE = re.compile(r'RECOMP_TODO\(0x[0-9A-Fa-f]+, "([^" ]+)')


def trace_wrapper(va, body):
    """O corpo lifted vira `L_<VA>_body` (static) e `L_<VA>` o chama entre
    re5_trace_enter/exit. Os rótulos internos `L_<VA>:` não colidem com o nome da
    função (rótulos têm espaço de nomes próprio em C)."""
    name = f"L_{va:08X}"
    body = body.replace(f"void {name}(CPU *c)", f"static void {name}_body(CPU *c)", 1)
    return (f"void re5_trace_enter(CPU *c, uint32_t va);\nvoid re5_trace_exit(CPU *c, uint32_t va);\n"
            f"void {name}(CPU *c);\n{body}\n"
            f"void {name}(CPU *c) {{ re5_trace_enter(c, 0x{va:08X}u); {name}_body(c); "
            f"re5_trace_exit(c, 0x{va:08X}u); }}")


def write_if_changed(path, text):
    """Grava só se o conteúdo mudou: o build incremental recompila só as unidades
    que o lift alterou (as outras mantêm o mtime). Devolve True se gravou."""
    try:
        with open(path, encoding="utf-8", newline="\n") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


def read_va_list(path):
    """config/overrides.txt e afins: 'VA  motivo' por linha, '#' comenta."""
    out = {}
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                parts = line.split(None, 1)
                out[int(parts[0], 16)] = parts[1] if len(parts) > 1 else ""
    return out


def cmd_lift(args, cfg):
    import pefile
    import lift32_cpu

    exe = resolve_exe(args, cfg)
    gate(exe, args.allow_protected)
    cat = json.load(open(args.catalog or rc.repo_path(cfg["lift"]["catalog"])))
    if cat.get("sha256") and cat["sha256"] != rc.sha256_file(exe):
        sys.exit("[!] o catálogo foi gerado de outro executável: rode `run_lift.py catalog` de novo.")
    outdir = args.out or rc.repo_path(cfg["lift"]["out_dir"])
    per_file = args.per_file or cfg["lift"]["functions_per_file"]
    # Janela de endereços por arquivo: uma função a mais ou a menos no catálogo
    # só muda o arquivo da janela dela, e o build incremental recompila só ele.
    # Sem janela, a divisão é por contagem e qualquer mudança desloca todo o resto.
    window = args.bytes_per_file if args.bytes_per_file is not None else cfg["lift"].get("bytes_per_file", 0)
    code_start = cat.get("code_start", 0)
    prefix = args.prefix

    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    lift32_cpu.IMAGE_BASE = base      # o Lifter lê este global do módulo

    def read_va(va, n):
        return pe.get_data(va - base, n)

    lifter = lift32_cpu.Lifter(exe, pe.OPTIONAL_HEADER.SizeOfImage, read_va)
    lifter.flags_dead_ret_below = cfg["lift"].get("flags_dead_at_ret_below")
    lifter.cache_flags = bool(cfg["lift"].get("cache_flags", True))
    lifter.tail_calls = bool(cfg["lift"].get("tail_calls", False)) and not getattr(args, "no_tail_calls", False)
    lifter.elide_flag_stores = bool(cfg["lift"].get("elide_flag_stores", False))
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    lifter.reloc_vas = {(base + e.rva) & 0xFFFFFFFF
                        for br in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
                        for e in br.entries if e.type == 3}

    overrides = read_va_list(os.path.join(rc.CONFIG_DIR, "overrides.txt"))
    excluded = read_va_list(os.path.join(rc.CONFIG_DIR, "exclude.txt"))
    # Pontos de rastreio (config/tracepoints.txt): a função vira um invólucro que
    # avisa o runtime na entrada e na saída (RE5_TRACE liga a impressão).
    traced = read_va_list(os.path.join(rc.CONFIG_DIR, "tracepoints.txt"))

    os.makedirs(outdir, exist_ok=True)
    stale = {fn for fn in os.listdir(outdir) if fn.startswith(prefix + "_") and fn.endswith((".c", ".h"))}
    written = []

    def emit(name, text):
        stale.discard(name)
        if write_if_changed(os.path.join(outdir, name), text):
            written.append(name)

    stats = collections.Counter()
    todo = collections.Counter()
    failures = []
    lifted = []
    # re5_cov: um byte por endereço de código; a função gerada marca o seu na entrada
    # (runtime/src/core/trace.c, teclas F8/F9 com RE5_COV=1). Só grava quando o byte ainda é 0:
    # o store incondicional fazia as 8 threads do jogo disputarem as mesmas linhas de cache
    # a cada chamada de função (falso compartilhamento); a leitura deixa a linha compartilhada.
    header = ["/* GERADO por scripts/run_lift.py (pcrecomp lift32_cpu) - não edite. */",
              "#define RE5_CACHED_REGS 1"] + (
        [] if lifter.cache_flags else
        ["/* lift.cache_flags = false: cf..of ficam na CPU (A/B da fase 0.2) */"] +
        ["#define R_%s c->%s" % (f, f) for f in ("cf", "pf", "af", "zf", "sf", "of")]) + [
              '#include "cpu.h"',
              "extern unsigned char re5_cov[];",
              "#define RE5_COV(va) do { unsigned char *_cv = &re5_cov[(va) - 0x401000u]; "
              "if (__builtin_expect(!*_cv, 0)) *_cv = 1; } while (0)", ""]

    # Cache por arquivo: o C de uma função só depende dos bytes dela, do VA, do
    # tamanho e do lifter. Se nada disso mudou numa janela e o arquivo existe,
    # ela não é refeita - a volta da etapa 2.6 (uma semente nova) leva segundos.
    stats_path = os.path.join(outdir, "lift_stats.json")
    try:
        with open(stats_path, encoding="utf-8") as f:
            prev_files = json.load(f).get("file_cache", {})
    except (OSError, ValueError):
        prev_files = {}
    lift_dir = os.path.dirname(os.path.abspath(lift32_cpu.__file__))
    tool_hash = hashlib.sha256()
    for fn in sorted(os.listdir(lift_dir)) + [os.path.abspath(__file__)]:
        path = fn if os.path.isabs(fn) else os.path.join(lift_dir, fn)
        if path.endswith(".py"):
            with open(path, "rb") as f:
                tool_hash.update(f.read())
    tool_hash.update(rc.sha256_file(exe).encode())
    tool_hash.update(repr(cfg["lift"].get("flags_dead_at_ret_below")).encode())
    tool_hash.update(repr(lifter.cache_flags).encode())
    tool_hash.update(repr(lifter.tail_calls).encode())
    tool_hash.update(repr(lifter.elide_flag_stores).encode())
    file_cache = {}

    funcs = [fd for fd in cat["functions"] if fd["size"] > 0]
    # Chamada direta (RE5_CALL, cpu_epilogue.h) quando o alvo constante é uma
    # função gerada: sem a busca do dispatch a cada call. Um alvo que só entre
    # no catálogo depois fica no dispatch (correto, só mais lento) até a janela
    # do chamador ser refeita.
    direct = cfg["lift"].get("direct_calls", True)
    spin_pause = bool(cfg["lift"].get("spin_pause", False))
    known = {fd["address"] for fd in funcs}

    def direct_calls(text):
        used = set()

        def sub(m):
            va = int(m.group(1), 16)
            if va not in known:
                return m.group(0)
            used.add(va)
            return f"RE5_CALL(c, L_{va:08X})"

        def sub_tail(m):
            va = int(m.group(1), 16)
            if va not in known:
                return m.group(0)
            used.add(va)
            return f"RE5_TAILCALL(c, L_{va:08X})"
        return DIRECT_RE.sub(sub, TAIL_RE.sub(sub_tail, text)), used
    groups = collections.OrderedDict()
    for i, fd in enumerate(funcs):
        idx = (fd["address"] - code_start) // window if window else i // per_file
        groups.setdefault(idx, []).append(fd)

    t = time.time()
    done = reused = 0
    for idx, fds in groups.items():
        name = f"{prefix}_{idx:04d}.c"
        key = hashlib.sha256(tool_hash.digest() + repr((direct, spin_pause)).encode() + repr(
            [(fd["address"], fd["size"], fd["address"] in overrides, fd["address"] in excluded,
              fd["address"] in traced) for fd in fds]
        ).encode()).hexdigest()
        lifted.extend(fd["address"] for fd in fds)
        old = prev_files.get(name)
        if old and old.get("key") == key and os.path.exists(os.path.join(outdir, name)):
            stale.discard(name)
            file_cache[name] = old
            todo.update(old.get("todo", {}))
            stats.update(old.get("counts", {}))
            failures.extend(old.get("failures", []))
            reused += 1
            continue
        chunk = []
        ftodo, fcounts, ffail = collections.Counter(), collections.Counter(), []
        for fd in fds:
            va, size = fd["address"], fd["size"]
            if va in overrides:
                # Corpo escrito à mão no runtime (runtime/src/core/overrides.c).
                chunk.append(f"void re5_ov_{va:08X}(CPU *c);" + chr(10) + f"void L_{va:08X}(CPU *c) {{ re5_ov_{va:08X}(c); }}")
                fcounts["override"] += 1
            elif va in excluded:
                chunk.append(f'void L_{va:08X}(CPU *c) {{ (void)c; RECOMP_TODO(0x{va:08X}, "excluded"); }}')
                fcounts["excluded"] += 1
            else:
                try:
                    body = lifter.lift_function(read_va(va, size), va)
                except Exception as ex:  # um erro de lift não pode sumir em silêncio
                    ffail.append({"address": f"0x{va:08X}", "error": repr(ex)})
                    body = f'void L_{va:08X}(CPU *c) {{ (void)c; RECOMP_TODO(0x{va:08X}, "lift_failed"); }}'
                ftodo.update(TODO_RE.findall(body))
                # Cobertura: cada função gerada marca o próprio byte em re5_cov (trace.c).
                body = body.replace("\n{\n", f"\n{{\n    RE5_COV(0x{va:08X}u);\n", 1)
                if va in traced:
                    body = trace_wrapper(va, body)
                    fcounts["traced"] += 1
                chunk.append(body)
                fcounts["lifted"] += 1
            chunk.append("")
            done += 1
            if done % 5000 == 0:
                print(f"[*] {done} funções refeitas ({time.time() - t:.0f}s)")
        body = "\n".join(chunk) + "\n"
        # A CPU mora no heap do host, fora do espaço do guest: nenhum acesso à
        # memória do guest (ponteiros may_alias) a apelida. Sem o restrict, cada
        # escrita no guest obrigava o compilador a reler e regravar os
        # registradores do guest no struct (código ~6% maior, medido em re5_0166.c).
        body = loop_barriers(SIG_RE.sub(r"void \1(CPU *RE5_RESTRICT c)", body), spin_pause)
        decls = []
        if direct:
            body, used = direct_calls(body)
            decls = [f"void L_{va:08X}(CPU *c);" for va in sorted(used)] + [""]
        emit(name, "\n".join(header + decls) + body)
        file_cache[name] = {"key": key, "todo": dict(ftodo), "counts": dict(fcounts), "failures": ffail}
        todo.update(ftodo)
        stats.update(fcounts)
        failures.extend(ffail)
    n_files = len(groups)

    emit(f"{prefix}_funcs.h",
         "/* GERADO por scripts/run_lift.py - não edite. */\n#pragma once\n#include \"cpu.h\"\n" +
         "".join(f"void L_{va:08X}(CPU *c);\n" for va in lifted))
    emit(f"{prefix}_dispatch_table.c",
         "/* GERADO por scripts/run_lift.py - não edite. */\n"
         f'#include "{prefix}_funcs.h"\n\n'
         "const re5_func_entry re5_lifted_table[] = {\n" +
         "".join(f"    {{0x{va:08X}u, L_{va:08X}}},\n" for va in sorted(set(lifted))) +
         "};\nconst uint32_t re5_lifted_count = "
         "(uint32_t)(sizeof re5_lifted_table / sizeof re5_lifted_table[0]);\n")
    for name in sorted(stale):
        os.remove(os.path.join(outdir, name))

    report = {
        "exe": os.path.basename(exe),
        "functions": len(lifted),
        "files": n_files,
        "seconds": round(time.time() - t, 1),
        "counts": dict(stats),
        "lift_failures": failures,
        "recomp_todo_sites": sum(todo.values()),
        "recomp_todo_by_mnemonic": dict(todo.most_common()),
        "files_rewritten": written,
        "files_removed": sorted(stale),
        "file_cache": file_cache,
    }
    with open(stats_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"[*] {len(lifted)} funções em {n_files} arquivos ({reused} reaproveitados), {len(failures)} falhas de "
          f"lift, {report['recomp_todo_sites']} RECOMP_TODO -> {outdir}")
    print(f"[*] {len(written)} arquivos regravados, {len(stale)} removidos (o resto ficou intacto)")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("catalog", "lift"):
        p = sub.add_parser(name)
        p.add_argument("--exe", help="executável (padrão: input.clean do re5_config.toml)")
        p.add_argument("--out")
        p.add_argument("--allow-protected", action="store_true")
        if name == "catalog":
            p.add_argument("--seeds", help="sementes extras (JSON ou texto), separadas por vírgula")
            p.add_argument("--add-seeds", help="só acrescenta estas sementes ao catálogo existente "
                                               "(ex.: work/runtime_seeds.txt gravado pelo re5 com RE5_SEED_LOG)")
            p.add_argument("--fix-labels", action="store_true",
                           help="no catálogo existente, inícios falsos que desviam de volta para o meio "
                                "da função anterior viram alias (disasm32.label_reentries)")
        else:
            p.add_argument("--catalog")
            p.add_argument("--per-file", type=int)
            p.add_argument("--bytes-per-file", type=lambda v: int(v, 0),
                           help="janela de endereços por arquivo .c (0 = dividir por contagem)")
            p.add_argument("--prefix", default="re5")
            p.add_argument("--no-tail-calls", action="store_true",
                           help="força lift.tail_calls = false (A/B; use com --out para não sobrescrever)")
    args = ap.parse_args()
    cfg = rc.load_config()
    return {"catalog": cmd_catalog, "lift": cmd_lift}[args.cmd](args, cfg) or 0


if __name__ == "__main__":
    sys.exit(main())
