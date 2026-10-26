#!/usr/bin/env bash
# Compila o Direct3D 9 da DXVK para o host: o Vulkan por trás da ponte D3D9
# (etapa 4.2, decisão D4). Só o d3d9 (sem dxgi/d3d8/d3d10/d3d11). Saída em
# build/dxvk/<versão> (ou no 2º argumento), que o CMakeLists.txt raiz copia
# para dxvk/<versão>/ ao lado do re5.exe. Idempotente.
#
#   scripts/build_dxvk.sh [versão] [saida]
#
#   2.6.2 (padrão, third_party/dxvk-2.6.2): Vulkan 1.3, VK_KHR_maintenance5
#         opcional - a mais nova que roda em drivers sem ele (AMD Vega/GCN no
#         Windows: o ramo de manutenção da AMD não ganha extensões novas).
#   3.1.1 (third_party/dxvk): Vulkan 1.4 + maintenance5/6 e load_store_op_none.
#   No Android (Mali do Exynos 1380 e outros Vulkan 1.1) a versão é a DXVK
#   native 1.9.2b (third_party/dxvk-native) com o porte do NFSU2
#   (tools/nfsu2-reference/ports/dxvk_android): scripts/build_dxvk_android.sh.
#
#   Windows (Git Bash/MSYS2 com MinGW-w64): d3d9.dll x64, ligada estática ao
#       libgcc/libstdc++ (só depende do vulkan-1.dll do driver).
#   Linux: libdxvk_d3d9.so com o WSI do SDL2 (a "HWND" é o SDL_Window*).
#
# Precisa de: meson (no PATH, ou MESON="py -3.11 -m mesonbuild.mesonmain"),
# ninja e glslang (ou glslangValidator) no PATH, ou GLSLANG_DIR com bin/glslang.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
ver="${1:-2.6.2}"
case "$ver" in
2.6.2) src="$root/third_party/dxvk-2.6.2" ;;
3.1.1) src="$root/third_party/dxvk" ;;
*) echo "build_dxvk: versão $ver desconhecida (2.6.2 ou 3.1.1)" >&2; exit 1 ;;
esac
out="${2:-$root/build/dxvk/$ver}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

[ -f "$src/meson.build" ] || { echo "build_dxvk: $src/meson.build não existe" >&2; exit 1; }
[ -n "${GLSLANG_DIR:-}" ] && export PATH="$GLSLANG_DIR/bin:$PATH"
command -v glslang >/dev/null || command -v glslangValidator >/dev/null ||
    { echo "build_dxvk: glslang/glslangValidator não está no PATH (ou GLSLANG_DIR)" >&2; exit 1; }
if [ -n "${MESON:-}" ]; then
    read -ra meson <<< "$MESON"          # ex.: MESON="py -3.11 -m mesonbuild.mesonmain"
elif command -v meson >/dev/null; then
    meson=(meson)
else
    py="$(command -v python3 || command -v py)"
    [ "$(basename "$py")" = py ] && py=("$py" -3) || py=("$py")
    "${py[@]}" -m mesonbuild.mesonmain --version >/dev/null 2>&1 ||
        { echo "build_dxvk: meson não encontrado (pip install meson)" >&2; exit 1; }
    meson=("${py[@]}" -m mesonbuild.mesonmain)
fi

opts=(--buildtype release -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false)
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
    lib="$out/d3d9.dll"
    built="$out/build/src/d3d9/d3d9.dll"
    # O build-win64.txt da DXVK pede binutils com prefixo (x86_64-w64-mingw32-ar...),
    # que o MinGW nativo do Windows não tem: o cross-file usa os nomes que existem.
    cross="$out/mingw-win64.txt"
    pick() { command -v "x86_64-w64-mingw32-$1" >/dev/null && echo "x86_64-w64-mingw32-$1" || echo "$1"; }
    cat > "$cross" <<EOF
[binaries]
c = '$(pick gcc)'
cpp = '$(pick g++)'
ar = '$(pick ar)'
strip = '$(pick strip)'
windres = '$(pick windres)'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    opts+=(--cross-file "$cross")
    ;;
*)
    lib="$out/libdxvk_d3d9.so"
    built="$out/build/src/d3d9/libdxvk_d3d9.so"
    opts+=(-Dnative_sdl2=enabled -Dnative_glfw=disabled -Dnative_sdl3=disabled)
    ;;
esac

if [ -f "$lib" ] && [ "$lib" -nt "$src/RELEASE" ]; then
    echo "build_dxvk: já compilado em $lib"
    exit 0
fi
[ -f "$out/build/build.ninja" ] || "${meson[@]}" setup "${opts[@]}" "$out/build" "$src"
ninja -C "$out/build" -j"$(nproc 2>/dev/null || echo 8)"
cp "$built" "$lib"
echo "build_dxvk: DXVK $(cat "$src/RELEASE") pronta em $lib"
