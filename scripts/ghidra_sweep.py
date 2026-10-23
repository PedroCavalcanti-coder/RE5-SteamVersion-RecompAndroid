#!/usr/bin/env python3
"""Driver para varredura e análise com Ghidra no re5-recompiled (Etapas V0..V1).

Acha a instalação do Ghidra e JDK, confere o SHA-256 do binário de entrada,
configura o heap para 6G, aplica as opções reproduzíveis via ConfigureAnalysis.java
e salva o projeto oficial em work/ghidra/re5.gpr.

Uso:
    python scripts/ghidra_sweep.py [--ghidra-dir CAMINHO] [--max-mem 6G] [--dry-run]
"""
import argparse
import os
import subprocess
import sys

import re5common as rc


def get_headless_cmd(ghidra_dir):
    """Retorna o caminho do executável analyzeHeadless."""
    if not ghidra_dir or not os.path.isdir(ghidra_dir):
        return None
    name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
    path = os.path.join(ghidra_dir, "support", name)
    return path if os.path.exists(path) else None


def validate_input(input_path, force=False):
    """Valida se o binário existe e bate com um build liftable de known_builds.json."""
    abs_path = rc.repo_path(input_path)
    if not os.path.exists(abs_path):
        raise FileNotFoundError(f"Binário de entrada não encontrado: {abs_path}")

    sha = rc.sha256_file(abs_path)
    builds = rc.load_known_builds()
    matched = None
    for b in builds:
        if b.get("sha256", "").lower() == sha.lower():
            matched = b
            break

    if not matched:
        msg = f"SHA-256 {sha} do arquivo {input_path} não está em config/known_builds.json"
        if not force:
            raise ValueError(msg)
        print(f"[aviso] {msg} (ignorado com --force-input)")
    elif not matched.get("liftable", False):
        msg = f"Build '{matched.get('name')}' marcado como não-liftable (protegido)"
        if not force:
            raise ValueError(msg)
        print(f"[aviso] {msg} (ignorado com --force-input)")

    return abs_path, sha, matched


def build_headless_args(ghidra_dir, project_dir, project_name, input_file,
                        pre_script="ConfigureAnalysis.java",
                        post_scripts=None,
                        no_analysis=False,
                        force_import=False,
                        script_paths=None):
    """Monta a lista de argumentos para o analyzeHeadless."""
    headless_bin = get_headless_cmd(ghidra_dir)
    if not headless_bin:
        raise FileNotFoundError(f"analyzeHeadless não encontrado em {ghidra_dir}")

    if post_scripts is None:
        post_scripts = ["GhidraStats.java"]

    if script_paths is None:
        script_paths = [
            rc.repo_path(os.path.join("scripts", "ghidra")),
            rc.repo_path(os.path.join("tools", "pcrecomp", "tools", "ghidra")),
        ]
    elif isinstance(script_paths, str):
        sep = ";" if ";" in script_paths else ":"
        script_paths = [p for p in script_paths.split(sep) if p]

    sep = ";" if os.name == "nt" else ":"
    sp_str = sep.join(script_paths)

    gpr_path = os.path.join(project_dir, f"{project_name}.gpr")
    is_existing = os.path.exists(gpr_path) and not force_import

    cmd = [headless_bin, project_dir, project_name]

    if is_existing:
        # Reaproveita o projeto já analisado
        target_name = os.path.basename(input_file)
        cmd.extend(["-process", target_name])
        if no_analysis:
            cmd.append("-noanalysis")
    else:
        # Importa e analisa
        cmd.extend(["-import", input_file])

    cmd.extend(["-scriptPath", f'"{sp_str}"'])

    if not is_existing and pre_script:
        cmd.extend(["-preScript", pre_script])

    for ps in post_scripts:
        cmd.extend(["-postScript", ps])

    return cmd


def run_sweep(args):
    # 1. Localiza Ghidra e JDK
    ghidra_dir = args.ghidra_dir or rc.find_ghidra()
    if not ghidra_dir:
        print("[ERRO] Instalação do Ghidra não encontrada. Defina RE5_GHIDRA_DIR ou passe --ghidra-dir.", file=sys.stderr)
        return 1

    java_cmd, jdk_ver = rc.find_jdk()
    if not java_cmd:
        print("[ERRO] JDK não encontrado no sistema. Configure JAVA_HOME ou instale o JDK 21+.", file=sys.stderr)
        return 1

    print(f"Ghidra: {ghidra_dir}")
    print(f"JDK:    {jdk_ver} ({java_cmd})")

    # 2. Valida binário
    input_path, sha, build_info = validate_input(args.input, force=args.force_input)
    build_name = build_info.get("name") if build_info else "desconhecido"
    print(f"Entrada: {input_path} (build: {build_name}, sha256: {sha[:16]}...)")

    # 3. Diretórios de trabalho
    proj_dir = rc.repo_path(args.project_dir)
    os.makedirs(proj_dir, exist_ok=True)
    gpr_path = os.path.join(proj_dir, f"{args.project_name}.gpr")

    if os.path.exists(gpr_path) and not args.force_import:
        print(f"Projeto existente detectado: {gpr_path} (usando -process)")
    else:
        print(f"Criando novo projeto: {gpr_path} (usando -import)")

    # 4. Constrói comando
    post_scripts = args.post_scripts or ["GhidraStats.java"]
    cmd = build_headless_args(
        ghidra_dir=ghidra_dir,
        project_dir=proj_dir,
        project_name=args.project_name,
        input_file=input_path,
        pre_script=args.pre_script,
        post_scripts=post_scripts,
        no_analysis=args.no_analysis,
        force_import=args.force_import,
    )

    env = dict(os.environ)
    env["GHIDRA_HEADLESS_MAXMEM"] = args.max_mem
    env["DOUBLE_CLICKED"] = "n"
    if "JAVA_HOME" not in env:
        # Se JAVA_HOME não estiver definido, infere a partir do java_cmd
        java_home_cand = os.path.abspath(os.path.join(os.path.dirname(java_cmd), ".."))
        env["JAVA_HOME"] = java_home_cand

    print(f"Heap max: {args.max_mem}")
    print(f"Comando:  {' '.join(cmd)}")

    if args.dry_run:
        print("[dry-run] Comando montado com sucesso.")
        return 0

    # 5. Executa
    print("\n=== Iniciando analyzeHeadless (pressione Ctrl+C para cancelar) ===")
    if os.name == "nt":
        cmd_str = " ".join(f'"{c}"' if (" " in c or ";" in c) and not (c.startswith('"') and c.endswith('"')) else c for c in cmd)
        res = subprocess.run(cmd_str, env=env, shell=True)
    else:
        res = subprocess.run(cmd, env=env)
    return res.returncode


def main():
    cfg = rc.load_config()
    default_input = cfg.get("input", {}).get("clean", "bin_input/re5dx9.clean.exe")

    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--ghidra-dir", default=None, help="Caminho raiz da instalação do Ghidra")
    parser.add_argument("--input", default=default_input, help="Binário PE para importar/processar")
    parser.add_argument("--project-dir", default=os.path.join("work", "ghidra"), help="Diretório do projeto Ghidra")
    parser.add_argument("--project-name", default="re5", help="Nome do projeto Ghidra (.gpr)")
    parser.add_argument("--max-mem", default="6G", help="Limite de memória heap para o headless (padrão: 6G)")
    parser.add_argument("--pre-script", default="ConfigureAnalysis.java", help="Script executado antes da análise")
    parser.add_argument("--post-scripts", nargs="*", default=None, help="Scripts pós-análise a executar")
    parser.add_argument("--no-analysis", action="store_true", help="Pular análise (--process com -noanalysis)")
    parser.add_argument("--force-import", action="store_true", help="Forçar nova importação mesmo se projeto existir")
    parser.add_argument("--force-input", action="store_true", help="Aceitar binário de entrada mesmo sem SHA conhecido")
    parser.add_argument("--dry-run", action="store_true", help="Apenas valida e imprime o comando sem executar")

    args = parser.parse_args()
    return run_sweep(args)


if __name__ == "__main__":
    sys.exit(main())
