#!/usr/bin/env bash
# Compila a libdxvk_d3d9.so do Android ARM64: DXVK native 1.9.2b
# (third_party/dxvk-native) com os patches do porte do NFSU2 e o WSI de
# ANativeWindow do re5 (android/dxvk). Copia para o jniLibs do app, de onde o
# runtime a abre com dlopen (runtime/src/gfx/d3d9_host.c). Idempotente.
#
#   scripts/build_dxvk_android.sh [saida]      (padrão: build/dxvk-android)
#
# Precisa de: python, cmake, ninja, glslang (ou glslangValidator) no PATH ou em
# GLSLANG_DIR/bin, e o NDK em ANDROID_NDK_HOME (ou o ndk.dir do
# android/local.properties).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/dxvk-android}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

[ -n "${GLSLANG_DIR:-}" ] && export PATH="$GLSLANG_DIR/bin:$PATH"
glslang="$(command -v glslang || command -v glslangValidator || true)"
[ -n "$glslang" ] || { echo "build_dxvk_android: glslang/glslangValidator não está no PATH (ou GLSLANG_DIR)" >&2; exit 1; }

ndk="${ANDROID_NDK_HOME:-}"
if [ -z "$ndk" ] && [ -f "$root/android/local.properties" ]; then
    ndk="$(sed -n 's/^ndk\.dir=//p' "$root/android/local.properties" | sed 's/\\:/:/g; s/\\\\/\//g')"
fi
[ -f "$ndk/build/cmake/android.toolchain.cmake" ] ||
    { echo "build_dxvk_android: NDK não encontrado (ANDROID_NDK_HOME ou ndk.dir em android/local.properties)" >&2; exit 1; }

py="${PYTHON:-$(command -v python || command -v python3)}"   # no Windows, python3 pode ser o atalho da loja
"$py" "$root/android/dxvk/prepare.py" "$out/src" --glslang "$glslang"

cmake -S "$root/android/dxvk" -B "$out/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DDXVK_SOURCE="$out/src"
cmake --build "$out/build"

jni="$root/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$jni"
strip="$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -1)"
"$strip" --strip-unneeded -o "$jni/libdxvk_d3d9.so" "$out/build/libdxvk_d3d9.so"
echo "build_dxvk_android: $jni/libdxvk_d3d9.so"
