#!/usr/bin/env python3
"""Auditoria comparativa entre o catálogo do re5-recompiled e o Ghidra (Etapa V2).

Compara work/catalog.json com as exportações do Ghidra (work/ghidra/export/):
1. Inícios: correspondência exata, divisões (splits), inventadas (dados decodificados)
   e funções do Ghidra ausentes do catálogo (com e sem referências).
2. Fins: comparação de extensão com descarte de enchimento 0xCC/0x90.
3. Saltos computados: alvos de tabelas de salto ausentes do corpo da função e do catálogo.
4. Alvos indiretos: métodos de vtables (RTTI) e ponteiros de código em seções de dados.
5. Funções sem retorno: funções marcadas no Ghidra como sem retorno.
6. Aliases: verificação se recaem dentro de funções conhecidas.

Gera:
- work/seeds_ghidra.txt: sementes a serem adicionadas via `run_lift.py catalog --add-seeds`.
- work/audit/catalog_report.json: relatório estruturado completo.

Uso:
    python scripts/audit_catalog.py [--catalog CAMINHO] [--out-seeds CAMINHO] [--out-report CAMINHO]
"""
import argparse
import bisect
import json
import os
import sys

import re5common as rc


def load_json_safe(path):
    if not path or not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_excludes(path):
    excludes = set()
    if path and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    excludes.add(int(line.split()[0], 16))
    return excludes


def audit_catalog(catalog_data, funcs_data, switches_data=None, code_ptrs_data=None,
                  rtti_data=None, pe_bytes=None, image_base=0x00400000, excludes=None):
    """Executa a auditoria completa e retorna um dicionário estruturado com as métricas."""
    excludes = excludes or set()

    # 1. Catálogo candidato
    cat_funcs = catalog_data.get("functions", []) if isinstance(catalog_data, dict) else catalog_data
    cand_starts = sorted({f["address"] for f in cat_funcs if f.get("entry_kind") != "alias"})
    cand_aliases = sorted({f["address"] for f in cat_funcs if f.get("entry_kind") == "alias"})
    cand_all = set(cand_starts) | set(cand_aliases)

    cand_ends = {f["address"]: f["end"] for f in cat_funcs if f.get("entry_kind") != "alias"}
    cand_ranges = sorted([(f["address"], f["end"]) for f in cat_funcs if f.get("entry_kind") != "alias"])
    cand_starts_list = [s for s, e in cand_ranges]

    def inside_cand_func(addr):
        idx = bisect.bisect_right(cand_starts_list, addr) - 1
        if idx >= 0 and cand_ranges[idx][0] <= addr < cand_ranges[idx][1]:
            return cand_ranges[idx]
        return None

    # 2. Referência do Ghidra (funcs.json)
    g_funcs = funcs_data or {}
    ref_starts = set()
    ref_ranges = []
    g_text_ends = {}

    for k, v in g_funcs.items():
        addr = int(k, 16)
        ref_starts.add(addr)
        for r in v.get("ranges", []):
            rs = int(r[0], 16)
            re_ex = int(r[1], 16) + 1  # Ghidra range max é inclusivo
            if 0x00401000 <= rs < 0x00F4A000:
                ref_ranges.append((rs, min(re_ex, 0x00F4A000)))
        text_ranges = [r for r in v.get("ranges", []) if 0x00401000 <= int(r[0], 16) < 0x00F4A000]
        if text_ranges:
            g_text_ends[addr] = max(int(r[1], 16) + 1 for r in text_ranges)

    # Mescla faixas de funções do Ghidra para buscas de pertencimento
    ref_ranges.sort()
    merged_ref_ranges = []
    for s, e in ref_ranges:
        if merged_ref_ranges and s <= merged_ref_ranges[-1][1]:
            merged_ref_ranges[-1] = (merged_ref_ranges[-1][0], max(merged_ref_ranges[-1][1], e))
        else:
            merged_ref_ranges.append((s, e))

    merged_ref_starts = [s for s, e in merged_ref_ranges]

    def inside_ref_ranges(addr):
        idx = bisect.bisect_right(merged_ref_starts, addr) - 1
        return idx >= 0 and merged_ref_ranges[idx][0] <= addr < merged_ref_ranges[idx][1]

    # --- Seção 1: Inícios (Starts) ---
    tp = [a for a in cand_starts if a in ref_starts]
    fp = [a for a in cand_starts if a not in ref_starts]
    fp_split = [a for a in fp if inside_ref_ranges(a)]
    fp_invented = [a for a in fp if not inside_ref_ranges(a)]

    # Funções do Ghidra fora do catálogo (não estão nem em cand_starts nem em aliases)
    missing_ghidra_all = [a for a in sorted(ref_starts) if a not in cand_all and a not in excludes]
    missing_with_callers = [a for a in missing_ghidra_all if g_funcs.get(f"0x{a:08x}", {}).get("callers", 0) > 0]
    missing_unreferenced = [a for a in missing_ghidra_all if g_funcs.get(f"0x{a:08x}", {}).get("callers", 0) == 0]

    # --- Seção 2: Fins (Ends) com 0xCC/0x90 aparado ---
    common_starts = set(cand_ends) & set(g_text_ends)
    end_exact = 0
    end_short_padded = 0
    end_short_real = 0
    end_short_split = 0
    end_long_padded = 0
    end_long_real = 0
    short_details = []

    for a in sorted(common_starts):
        c_end = cand_ends[a]
        g_end = g_text_ends[a]
        if c_end == g_end:
            end_exact += 1
        elif c_end < g_end:
            # curto
            is_padded = False
            if pe_bytes:
                fo_start = c_end - image_base
                fo_end = g_end - image_base
                if 0 <= fo_start < len(pe_bytes) and 0 <= fo_end <= len(pe_bytes):
                    gap = pe_bytes[fo_start:fo_end]
                    is_padded = all(b in (0xCC, 0x90) for b in gap)
            if is_padded:
                end_short_padded += 1
            else:
                end_short_real += 1
                if c_end in cand_all:
                    end_short_split += 1
                else:
                    short_details.append({
                        "address": f"0x{a:08X}",
                        "cand_end": f"0x{c_end:08X}",
                        "ghidra_end": f"0x{g_end:08X}",
                        "diff": g_end - c_end
                    })
        else:
            # longo
            is_padded = False
            if pe_bytes:
                fo_start = g_end - image_base
                fo_end = c_end - image_base
                if 0 <= fo_start < len(pe_bytes) and 0 <= fo_end <= len(pe_bytes):
                    gap = pe_bytes[fo_start:fo_end]
                    is_padded = all(b in (0xCC, 0x90) for b in gap)
            if is_padded:
                end_long_padded += 1
            else:
                end_long_real += 1

    # --- Seção 3: Saltos Computados (Switches) ---
    switches = switches_data or {}
    total_switches = len(switches)
    total_switch_targets = 0
    missing_switch_arms = []

    for sw_addr_str, sw in switches.items():
        sw_addr = int(sw_addr_str, 16)
        parent = inside_cand_func(sw_addr)
        for t_str in sw.get("targets", []):
            total_switch_targets += 1
            t_addr = int(t_str, 16)
            if not (0x00401000 <= t_addr < 0x00F4A000):
                continue
            # Coberto internamente pela função que contém o switch?
            if parent and parent[0] <= t_addr < parent[1]:
                continue
            # Coberto como entrada despachável (start ou alias) no catálogo?
            if t_addr in cand_all:
                continue
            missing_switch_arms.append({
                "switch_addr": f"0x{sw_addr:08X}",
                "target_addr": f"0x{t_addr:08X}",
                "func": sw.get("func")
            })

    # --- Seção 4: Alvos Indiretos (RTTI e Ponteiros de Dados) ---
    rtti = rtti_data or {}
    rtti_methods = rtti.get("methods", {})
    rtti_total = len(rtti_methods)
    rtti_missing = []
    for m_str, owner in rtti_methods.items():
        m_addr = int(m_str, 16)
        if m_addr not in cand_all and m_addr not in excludes:
            rtti_missing.append({"address": f"0x{m_addr:08X}", "owner": owner})

    code_ptrs = code_ptrs_data or []
    data_code_ptrs_in_code = set()
    for p in code_ptrs:
        if p.get("section") in (".rdata", ".data", ".data1"):
            t = int(p["target"], 16)
            if 0x00401000 <= t < 0x00F4A000:
                data_code_ptrs_in_code.add(t)

    # Ponteiros de dados que apontam para inícios de funções conhecidas do Ghidra mas faltam no catálogo
    data_ptrs_missing_funcs = sorted([a for a in (data_code_ptrs_in_code & ref_starts) if a not in cand_all])

    # --- Seção 5: Funções sem retorno ---
    no_return_funcs = [int(k, 16) for k, v in g_funcs.items() if v.get("no_return")]

    # --- Seção 6: Aliases ---
    aliases_total = len(cand_aliases)
    aliases_inside = sum(1 for a in cand_aliases if inside_ref_ranges(a))
    aliases_outside = aliases_total - aliases_inside

    # --- Compilação de sementes a gerar ---
    # Sementes imediatas: funções do Ghidra com chamadores + alvos de switch não despacháveis
    seeds_to_add = set(missing_with_callers)
    for arm in missing_switch_arms:
        t_addr = int(arm["target_addr"], 16)
        seeds_to_add.add(t_addr)
    for m in rtti_missing:
        seeds_to_add.add(int(m["address"], 16))
    for dp in data_ptrs_missing_funcs:
        seeds_to_add.add(dp)

    seeds_to_add -= excludes

    return {
        "catalog": {
            "total_entries": len(cat_funcs),
            "starts": len(cand_starts),
            "aliases": len(cand_aliases),
        },
        "starts": {
            "ghidra_total": len(ref_starts),
            "true_positives": len(tp),
            "false_positives": len(fp),
            "fp_splits": len(fp_split),
            "fp_invented": len(fp_invented),
            "missing_from_catalog": len(missing_ghidra_all),
            "missing_with_callers": len(missing_with_callers),
            "missing_unreferenced": len(missing_unreferenced),
            "missing_with_callers_list": [f"0x{a:08X}" for a in missing_with_callers],
        },
        "ends": {
            "common_scored": len(common_starts),
            "exact": end_exact,
            "short_padded": end_short_padded,
            "short_real": end_short_real,
            "short_split_continuation": end_short_split,
            "short_uncovered_gap": len(short_details),
            "long_padded": end_long_padded,
            "long_real": end_long_real,
            "short_details": short_details,
        },
        "switches": {
            "total_switches": total_switches,
            "total_targets": total_switch_targets,
            "missing_arms_count": len(missing_switch_arms),
            "missing_arms": missing_switch_arms,
        },
        "indirect": {
            "rtti_virtual_methods": rtti_total,
            "rtti_missing": len(rtti_missing),
            "rtti_missing_list": rtti_missing,
            "data_code_pointers": len(data_code_ptrs_in_code),
            "data_code_pointers_missing_funcs": len(data_ptrs_missing_funcs),
            "data_code_pointers_missing_funcs_list": [f"0x{a:08X}" for a in data_ptrs_missing_funcs],
        },
        "no_return": {
            "count": len(no_return_funcs),
            "sample": [f"0x{a:08X}" for a in no_return_funcs[:10]],
        },
        "aliases": {
            "total": aliases_total,
            "inside_known_funcs": aliases_inside,
            "outside_known_funcs": aliases_outside,
        },
        "seeds": {
            "count": len(seeds_to_add),
            "list": sorted([f"0x{a:08X}" for a in seeds_to_add]),
        },
    }


def write_seeds(seeds_list, out_path):
    """Escreve as sementes em formato texto para o run_lift.py --add-seeds."""
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# Sementes geradas pela auditoria Ghidra (Etapa V2)\n")
        f.write(f"# Total: {len(seeds_list)} endereços\n#\n")
        for va in seeds_list:
            f.write(f"{va}\n")


def write_report(report, out_path):
    """Salva o relatório estruturado em JSON."""
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)


def format_summary(rep):
    """Formata um resumo legível para o terminal."""
    st = rep["starts"]
    ed = rep["ends"]
    sw = rep["switches"]
    ind = rep["indirect"]
    al = rep["aliases"]
    sd = rep["seeds"]

    lines = [
        "=" * 68,
        "   AUDITORIA DO CATÁLOGO vs GHIDRA — RESULTADOS (Etapa V2)",
        "=" * 68,
        f"Catálogo candidato: {rep['catalog']['total_entries']:,} entradas "
        f"({rep['catalog']['starts']:,} starts, {rep['catalog']['aliases']:,} aliases)",
        f"Referência Ghidra : {st['ghidra_total']:,} funções",
        "-" * 68,
        "1. INÍCIOS (STARTS):",
        f"   - Correspondências exatas (True Positives) : {st['true_positives']:,} "
        f"({st['true_positives']*100/st['ghidra_total']:.1f}% do Ghidra)",
        f"   - Divisões internas (Splits)                : {st['fp_splits']:,}",
        f"   - Entradas fora do Ghidra (Inventadas)     : {st['fp_invented']:,} (prologue scan)",
        f"   - Funções Ghidra ausentes do catálogo      : {st['missing_from_catalog']:,}",
        f"       * com referências/chamadores (>0)      : {st['missing_with_callers']} (ALERTA)",
        f"       * sem referências (código morto/dados) : {st['missing_unreferenced']:,}",
        "-" * 68,
        "2. FINS (ENDS, aparando 0xCC/0x90):",
        f"   - Avaliadas em comum                        : {ed['common_scored']:,}",
        f"   - Extensão idêntica                         : {ed['exact']:,} ({ed['exact']*100/ed['common_scored']:.1f}%)",
        f"   - Curto por padding 0xCC/0x90               : {ed['short_padded']:,}",
        f"   - Curto com continuação imediata (Split)    : {ed['short_split_continuation']:,}",
        f"   - Curto com cauda genuinamente perdida      : {ed['short_uncovered_gap']} (Excelente: 0)",
        f"   - Longo por padding 0xCC/0x90               : {ed['long_padded']:,}",
        f"   - Longo com código excedente                : {ed['long_real']:,}",
        "-" * 68,
        "3. SALTOS COMPUTADOS (SWITCHES):",
        f"   - Saltos analisados                         : {sw['total_switches']:,} ({sw['total_targets']:,} alvos)",
        f"   - Alvos ausentes do catálogo/corpo da func  : {sw['missing_arms_count']} (ALERTA)",
        "-" * 68,
        "4. ALVOS INDIRETOS:",
        f"   - Métodos virtuais de vtable (RTTI)         : {ind['rtti_virtual_methods']:,} (Ausentes: {ind['rtti_missing']})",
        f"   - Ponteiros de dados para funções ausentes  : {ind['data_code_pointers_missing_funcs']}",
        "-" * 68,
        "5. ALIASES:",
        f"   - Dentro de funções do Ghidra               : {al['inside_known_funcs']:,}",
        f"   - Fora de funções do Ghidra                 : {al['outside_known_funcs']:,}",
        "-" * 68,
        f"SEMENTES RECOMENDADAS PARA O CATÁLOGO: {sd['count']}",
    ]
    if sd["list"]:
        lines.append("   Endereços:")
        for s in sd["list"][:25]:
            lines.append(f"     {s}")
        if len(sd["list"]) > 25:
            lines.append(f"     ... e mais {len(sd['list']) - 25} endereços.")
    lines.append("=" * 68)
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--catalog", default=rc.repo_path("work/catalog.json"),
                    help="caminho do catálogo do projeto (padrão: work/catalog.json)")
    ap.add_argument("--ghidra-funcs", default=rc.repo_path("work/ghidra/export/funcs.json"),
                    help="exportação de funções do Ghidra")
    ap.add_argument("--ghidra-switches", default=rc.repo_path("work/ghidra/export/switches.json"),
                    help="exportação de switches do Ghidra")
    ap.add_argument("--ghidra-code-ptrs", default=rc.repo_path("work/ghidra/export/code_ptrs.json"),
                    help="exportação de ponteiros de código do Ghidra")
    ap.add_argument("--ghidra-rtti", default=rc.repo_path("work/ghidra/export/rtti.json"),
                    help="exportação de RTTI do Ghidra")
    ap.add_argument("--exe", default=rc.repo_path("bin_input/re5dx9.clean.exe"),
                    help="executável limpo para checagem de bytes de enchimento")
    ap.add_argument("--exclude", default=rc.repo_path("config/exclude.txt"),
                    help="arquivo de exclusões conhecidas")
    ap.add_argument("--out-seeds", default=rc.repo_path("work/seeds_ghidra.txt"),
                    help="arquivo de sementes a gerar")
    ap.add_argument("--out-report", default=rc.repo_path("work/audit/catalog_report.json"),
                    help="relatório estruturado em JSON")
    ap.add_argument("--quiet", action="store_true", help="não imprime o resumo no terminal")

    args = ap.parse_args()

    cat = load_json_safe(args.catalog)
    if not cat:
        sys.exit(f"[!] Catálogo não encontrado em {args.catalog}")

    g_funcs = load_json_safe(args.ghidra_funcs)
    if not g_funcs:
        sys.exit(f"[!] Exportação funcs.json não encontrada em {args.ghidra_funcs}")

    g_switches = load_json_safe(args.ghidra_switches)
    g_code_ptrs = load_json_safe(args.ghidra_code_ptrs)
    g_rtti = load_json_safe(args.ghidra_rtti)

    pe_bytes = None
    if args.exe and os.path.exists(args.exe):
        with open(args.exe, "rb") as f:
            pe_bytes = f.read()

    excludes = load_excludes(args.exclude)

    rep = audit_catalog(
        catalog_data=cat,
        funcs_data=g_funcs,
        switches_data=g_switches,
        code_ptrs_data=g_code_ptrs,
        rtti_data=g_rtti,
        pe_bytes=pe_bytes,
        excludes=excludes,
    )

    if args.out_seeds:
        write_seeds(rep["seeds"]["list"], args.out_seeds)
        if not args.quiet:
            print(f"[*] Escritas {rep['seeds']['count']} sementes em {args.out_seeds}")

    if args.out_report:
        write_report(rep, args.out_report)
        if not args.quiet:
            print(f"[*] Relatório salvo em {args.out_report}")

    if not args.quiet:
        print(format_summary(rep))

    return 0


if __name__ == "__main__":
    sys.exit(main())
