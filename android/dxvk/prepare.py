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

# Nome das threads (dxvk-cs, dxvk-submit, dxvk-queue...): na 1.9.2b native o setThreadName do
# Linux é vazio, e as threads herdam o nome da thread do jogo que criou o device. O RE5_FPS_LOG
# e o scripts/android_perf.py separam a CPU da DXVK da do jogo por esse nome.
env_linux = args.output / "src/util/platform/util_env_linux.cpp"
text = env_linux.read_text()
empty = "  void setThreadName(const std::string& name) {\n  }"
if empty not in text:
    raise SystemExit(f"prepare: setThreadName vazio não encontrado em {env_linux}")
env_linux.write_text(text.replace("#include <unistd.h>", "#include <pthread.h>\n#include <unistd.h>").replace(
    empty, "  void setThreadName(const std::string& name) {\n"
           "    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());\n  }"))

def patch(rel, old, new):
    path = args.output / rel
    text = path.read_text()
    if old not in text:
        raise SystemExit(f"prepare: trecho não encontrado em {rel}: {old[:60]!r}")
    path.write_text(text.replace(old, new, 1))

# AFBC no Mali (docs/PROJETO_VULKAN_NATIVO.md, otimização da 1.9.2b): a DXVK marca toda textura
# de formato com par sRGB como VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, e com isso o driver do Mali
# desliga a compressão de textura e de framebuffer (AFBC) em quase todas as imagens do jogo.
# Sem DXVK_RE5_SRGB=1, as imagens não são mutáveis e um pedido de vista sRGB sai linear.
patch("src/d3d9/d3d9_common_texture.cpp", '#include <algorithm>',
      '#include <algorithm>\n#include <cstdlib>\n\n'
      'namespace dxvk {\n'
      '  static bool re5SrgbViews() {\n'
      '    static int v = -1;\n'
      '    if (v < 0) { const char* e = std::getenv("DXVK_RE5_SRGB"); v = e && e[0] == \'1\'; }\n'
      '    return v != 0;\n'
      '  }\n'
      '}')
patch("src/d3d9/d3d9_common_texture.cpp",
      'bool isMutable     = m_mapping.FormatSrgb != VK_FORMAT_UNDEFINED;',
      'bool isMutable     = m_mapping.FormatSrgb != VK_FORMAT_UNDEFINED && re5SrgbViews();')
patch("src/d3d9/d3d9_common_texture.cpp",
      '          bool                   Srgb) {\n    DxvkImageViewCreateInfo viewInfo;',
      '          bool                   Srgb) {\n'
      '    if (Srgb && !re5SrgbViews()) {\n'
      '      static bool warned = false;\n'
      '      if (!warned) Logger::warn("re5: vista sRGB pedida; sai linear (DXVK_RE5_SRGB=1 liga, sem AFBC)");\n'
      '      warned = true;\n'
      '      Srgb = false;\n'
      '    }\n'
      '    DxvkImageViewCreateInfo viewInfo;')

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
