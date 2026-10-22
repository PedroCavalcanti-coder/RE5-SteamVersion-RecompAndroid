#!/usr/bin/env python3
"""Fase 1 — reconhecimento do re5dx9.exe e portão de entrada do lifter.

    python3 scripts/recon.py                 # escolhe a melhor entrada disponível
    python3 scripts/recon.py <exe>           # analisa um executável específico
    python3 scripts/recon.py --all           # analisa todas as entradas de bin_input/
    python3 scripts/recon.py --markdown docs/RECON_RE5.md

Responde, com números, às perguntas que decidem se o pipeline pode seguir:

  * qual build é este (SHA-256 contra config/known_builds.json);
  * layout PE: seções, entropia, entry point, relocações, imports;
  * quais camadas de proteção ainda estão presentes (SteamStub, Enigma
    Protector, empacotador genérico);
  * veredito: `liftable` só quando nenhuma camada sobra. O lifter
    (scripts/run_lift.py) recusa qualquer entrada que não passe aqui.

Escreve work/recon.json (derivado do binário: fica fora do git).
Código de saída: 0 = pronto para lifting, 2 = protegido/bloqueado, 1 = erro.
"""
import argparse
import datetime
import json
import math
import os
import sys

import re5common as rc

try:
    import pefile
except ImportError:  # pragma: no cover - doctor.py reports this first
    sys.exit("pefile ausente: rode `pip install -r scripts/requirements.txt`")

HIGH_ENTROPY = 7.5          # bits/byte; código x86 real fica em ~6.0-6.7
ENIGMA_MARKERS = (b"Enigma Protector", b"Enigma Protector CA")


def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def section_name(s):
    return s.Name.rstrip(b"\x00").decode("latin-1")


def display_path(path):
    full = os.path.abspath(path)
    return os.path.relpath(full, rc.REPO) if full.startswith(rc.REPO + os.sep) else full


def analyze(path, known):
    with open(path, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data)
    opt, fh = pe.OPTIONAL_HEADER, pe.FILE_HEADER
    base, ep = opt.ImageBase, opt.AddressOfEntryPoint

    sections = []
    entry_section = None
    for s in pe.sections:
        raw = s.get_data()
        sec = {
            "name": section_name(s),
            "va": base + s.VirtualAddress,
            "vsize": s.Misc_VirtualSize,
            "raw_size": s.SizeOfRawData,
            "entropy": round(entropy(raw), 4),
            "exec": bool(s.Characteristics & 0x20000000),
            "write": bool(s.Characteristics & 0x80000000),
        }
        if s.VirtualAddress <= ep < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            entry_section = len(sections)
        sections.append(sec)

    imports = {}
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        names = [(i.name.decode() if i.name else f"#{i.ordinal}") for i in d.imports]
        imports[d.dll.decode()] = names
    n_imports = sum(len(v) for v in imports.values())

    relocs = opt.DATA_DIRECTORY[5]
    sha = rc.sha256_file(path)
    match = next((b for b in known if b["sha256"] == sha), None)

    # ---- camadas de proteção ----
    layers, evidence = [], []
    names = [s["name"] for s in sections]
    if ".bind" in names:
        layers.append("steamstub")
        evidence.append("seção .bind presente (SteamStub)")
    enigma_hits = [m.decode() for m in ENIGMA_MARKERS if m in data]
    exec_secs = [s for s in sections if s["exec"]]
    packed_exec = [s for s in exec_secs if s["entropy"] >= HIGH_ENTROPY]
    blank_names = sum(1 for s in sections if not s["name"])
    if enigma_hits:
        layers.append("enigma")
        evidence.append("assinatura do Enigma Protector no binário: " + ", ".join(sorted(set(enigma_hits))))
    if exec_secs and len(packed_exec) >= max(1, len(exec_secs) // 2):
        evidence.append(f"{len(packed_exec)}/{len(exec_secs)} seções executáveis com entropia >= {HIGH_ENTROPY} "
                        "(código comprimido/criptografado; código x86 real fica em ~6.0-6.7)")
        if "enigma" not in layers and "steamstub" not in layers:
            layers.append("packer")
    if entry_section is not None and entry_section != 0 and sections[entry_section]["exec"]:
        evidence.append(f"entry point na seção #{entry_section} '{sections[entry_section]['name']}', "
                        "não na primeira seção de código (stub de loader)")
    if blank_names >= 2:
        evidence.append(f"{blank_names} seções sem nome (típico de protetor)")
    if len(data) > (1 << 20) and imports and n_imports <= 2 * len(imports):
        evidence.append(f"tabela de imports mínima: {n_imports} funções em {len(imports)} DLLs "
                        "(o loader resolve a IAT real em tempo de execução)")

    return {
        "path": display_path(path),
        "sha256": sha,
        "size": len(data),
        "known_build": match["name"] if match else None,
        "machine": hex(fh.Machine),
        "pe_timestamp": datetime.datetime.fromtimestamp(fh.TimeDateStamp, datetime.UTC)
                        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "linker": f"{opt.MajorLinkerVersion}.{opt.MinorLinkerVersion:02d}",
        "image_base": base,
        "size_of_image": opt.SizeOfImage,
        "entry_point": base + ep,
        "entry_section": entry_section,
        "relocations": bool(relocs.VirtualAddress and relocs.Size),
        "relocs_stripped": bool(fh.Characteristics & 0x0001),
        "stack_reserve": opt.SizeOfStackReserve,
        "sections": sections,
        "imports": imports,
        "import_count": n_imports,
        "overlay_size": len(data) - pe.get_overlay_data_start_offset() if pe.get_overlay_data_start_offset() else 0,
        "layers": layers,
        "evidence": evidence,
        "liftable": not layers,
    }


def pick_default_input(cfg):
    for key in ("clean", "steamless", "original"):
        p = rc.repo_path(cfg["input"][key])
        if os.path.exists(p) and not rc.is_lfs_pointer(p):
            return p
    return None


def hx(v):
    return f"0x{v:08X}"


def to_markdown(reports):
    out = ["# Reconhecimento do binário (gerado por `scripts/recon.py`)", "",
           f"Gerado em {datetime.datetime.now(datetime.UTC).strftime('%Y-%m-%d %H:%M UTC')}. "
           "Regenere com `python3 scripts/recon.py --all --markdown docs/RECON_RE5.md`.", ""]
    for r in reports:
        verdict = "**PRONTO PARA LIFTING**" if r["liftable"] else \
            "**BLOQUEADO** — camadas presentes: " + ", ".join(f"`{l}`" for l in r["layers"])
        out += [f"## `{r['path']}`", "",
                "| Campo | Valor |", "|---|---|",
                f"| SHA-256 | `{r['sha256']}` |",
                f"| Tamanho | {r['size']:,} bytes |",
                f"| Build conhecido | {r['known_build'] or '—'} |",
                f"| Timestamp PE | {r['pe_timestamp']} |",
                f"| Linker | {r['linker']} (MSVC 2005) |" if r["linker"].startswith("8.") else f"| Linker | {r['linker']} |",
                f"| Image base / SizeOfImage | {hx(r['image_base'])} / {hx(r['size_of_image'])} |",
                f"| Entry point | {hx(r['entry_point'])} (seção #{r['entry_section']}) |",
                f"| Relocações | {'sim' if r['relocations'] else 'não'} (RELOCS_STRIPPED={r['relocs_stripped']}) |",
                f"| Imports declarados | {r['import_count']} funções em {len(r['imports'])} DLLs |",
                f"| Veredito | {verdict} |", "",
                "| # | Seção | VA | VSize | Raw | Entropia | Flags |", "|---|---|---|---|---|---|---|"]
        for i, s in enumerate(r["sections"]):
            flags = ("X" if s["exec"] else "-") + ("W" if s["write"] else "-")
            mark = " ← EP" if i == r["entry_section"] else ""
            out.append(f"| {i} | `{s['name'] or '(sem nome)'}` | {hx(s['va'])} | {hx(s['vsize'])} | "
                       f"{hx(s['raw_size'])} | {s['entropy']:.4f} | {flags}{mark} |")
        if r["evidence"]:
            out += ["", "Evidências:", ""] + [f"- {e}" for e in r["evidence"]]
        out += ["", "<details><summary>Imports declarados</summary>", ""]
        for dll, names in r["imports"].items():
            out.append(f"- `{dll}`: {', '.join(names)}")
        out += ["", "</details>", ""]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("exe", nargs="?", help="executável a analisar")
    ap.add_argument("--all", action="store_true", help="analisa original, steamless e clean (os que existirem)")
    ap.add_argument("--markdown", help="também escreve um relatório Markdown neste caminho")
    ap.add_argument("--json", default=os.path.join(rc.WORK_DIR, "recon.json"))
    args = ap.parse_args()

    cfg = rc.load_config()
    known = rc.load_known_builds()
    if args.all:
        paths = [rc.repo_path(cfg["input"][k]) for k in ("original", "steamless", "clean")]
        paths = [p for p in paths if os.path.exists(p)]
    elif args.exe:
        paths = [args.exe]
    else:
        p = pick_default_input(cfg)
        paths = [p] if p else []
    if not paths:
        print("[!] nenhum executável em bin_input/. Rode `git lfs pull` ou copie o re5dx9.exe.", file=sys.stderr)
        return 1

    reports = []
    for p in paths:
        if rc.is_lfs_pointer(p):
            print(f"[!] {p} é um ponteiro Git LFS: rode `git lfs pull --include=bin_input/*`", file=sys.stderr)
            return 1
        r = analyze(p, known)
        reports.append(r)
        state = "liftable" if r["liftable"] else "BLOQUEADO (" + ", ".join(r["layers"]) + ")"
        print(f"[*] {r['path']}: {r['known_build'] or 'build desconhecido'} -> {state}")
        for e in r["evidence"]:
            print(f"      - {e}")

    rc.ensure_work_dir()
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(reports if len(reports) > 1 else reports[0], f, indent=2)
    print(f"[*] escrito {os.path.relpath(args.json, rc.REPO)}")
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(to_markdown(reports))
        print(f"[*] escrito {args.markdown}")
    return 0 if any(r["liftable"] for r in reports) else 2


if __name__ == "__main__":
    sys.exit(main())
