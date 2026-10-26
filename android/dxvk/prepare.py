"""Prepara a DXVK native 1.9.2b de third_party/dxvk-native para o Android ARM64.

Os patches (intrínsecos x86 -> ARM64, recursos opcionais da Mali/Adreno, log no
logcat, cache de pipeline em disco) são os do porte do NFSU2,
tools/nfsu2-reference/ports/dxvk_android/prepare.py, aplicados sem mudança: lá a
fonte vem de um clone git na revisão fixa; aqui, da cópia vendorizada, que tem o
mesmo conteúdo (todos os patches batem) mas não é um repositório git.

Depois gera os shaders embutidos (SPIR-V em cabeçalhos C) com o glslang.

Uso: python android/dxvk/prepare.py <saída> [--glslang <glslang ou glslangValidator>]
"""
import argparse
import pathlib
import re
import runpy
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "third_party/dxvk-native"
NFSU2 = ROOT / "tools/nfsu2-reference/ports/dxvk_android/prepare.py"

parser = argparse.ArgumentParser()
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--glslang", default=shutil.which("glslang") or shutil.which("glslangValidator"))
args = parser.parse_args()

release = (SOURCE / "RELEASE").read_text().strip()
if release != "1.9.2":
    raise SystemExit(f"prepare: {SOURCE} é a DXVK {release}, o porte é da 1.9.2b")
if not args.glslang:
    raise SystemExit("prepare: glslang/glslangValidator não está no PATH (use --glslang)")

# A receita do NFSU2 confere a revisão com "git rev-parse HEAD" na fonte; a
# cópia vendorizada não tem git, então a resposta é a própria revisão fixada.
pin = re.search(r'^PIN = "([0-9a-f]{40})"', NFSU2.read_text(), re.M).group(1)
real_check_output = subprocess.check_output
subprocess.check_output = lambda cmd, *a, **k: pin if cmd[:2] == ["git", "-C"] else real_check_output(cmd, *a, **k)
sys.argv = [str(NFSU2), str(SOURCE), str(args.output)]
try:
    runpy.run_path(str(NFSU2), run_name="__main__")
finally:
    subprocess.check_output = real_check_output

# Shaders embutidos: os .h que a DXVK inclui (gerados pelo meson no build original).
generated = args.output / "generated"
generated.mkdir(exist_ok=True)
count = 0
for directory in ("src/dxvk/shaders", "src/dxvk/hud/shaders", "src/d3d9/shaders"):
    for shader in sorted((args.output / directory).iterdir()):
        if shader.suffix not in (".comp", ".frag", ".vert", ".geom"):
            continue
        subprocess.run([args.glslang, "--quiet", "--target-env", "vulkan1.1", "--vn", shader.stem,
                        str(shader), "-o", str(generated / (shader.stem + ".h"))], check=True)
        count += 1
print(f"{count} shaders embutidos gerados (Vulkan 1.1) em {generated}")
