#!/usr/bin/env bash
set -euo pipefail

export PATH="/c/mingw64/bin:$PATH"

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/ffmpeg-android}"
src="$root/third_party/FFmpeg"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

if [ -f "$out/libavcodec/libavcodec.a" ] && [ -f "$out/libavformat/libavformat.a" ] && [ -f "$out/libavutil/libavutil.a" ]; then
    echo "build_ffmpeg_android: já compilado em $out"
    exit 0
fi

ndk="${ANDROID_NDK_HOME:-}"
if [ -z "$ndk" ] && [ -f "$root/android/local.properties" ]; then
    ndk="$(sed -n 's/^ndk\.dir=//p' "$root/android/local.properties" | sed 's/\\:/:/g; s/\\\\/\//g')"
fi
if [ ! -d "$ndk" ]; then
    echo "build_ffmpeg_android: NDK não encontrado" >&2
    exit 1
fi

toolchain="$(ls -d "$ndk"/toolchains/llvm/prebuilt/* 2>/dev/null | head -1)"
target="aarch64-linux-android29"

# Wrapper scripts for CC, AR, RANLIB, STRIP to work cleanly with FFmpeg configure
mkdir -p "$out/tools"
cat << EOF > "$out/tools/cc"
#!/bin/sh
exec "$toolchain/bin/clang" --target=$target "\$@"
EOF
chmod +x "$out/tools/cc"

cat << EOF > "$out/tools/ar"
#!/bin/sh
exec "$toolchain/bin/llvm-ar" "\$@"
EOF
chmod +x "$out/tools/ar"

cat << EOF > "$out/tools/ranlib"
#!/bin/sh
exec "$toolchain/bin/llvm-ranlib" "\$@"
EOF
chmod +x "$out/tools/ranlib"

cat << EOF > "$out/tools/strip"
#!/bin/sh
exec "$toolchain/bin/llvm-strip" "\$@"
EOF
chmod +x "$out/tools/strip"

opts=(
    --enable-cross-compile
    --target-os=android
    --arch=aarch64
    --cpu=armv8-a
    --cc="$out/tools/cc"
    --ar="$out/tools/ar"
    --ranlib="$out/tools/ranlib"
    --strip="$out/tools/strip"
    --extra-cflags="-O3 -fPIC"
    --disable-everything
    --enable-decoder=vc1,wmv3,wmapro,wmav2
    --enable-demuxer=asf
    --enable-parser=vc1
    --disable-programs
    --disable-doc
    --disable-avdevice
    --disable-avfilter
    --disable-swscale
    --disable-swresample
    --disable-network
    --disable-autodetect
    --enable-static
    --disable-shared
    --disable-debug
    --disable-asm
)

export TMPDIR="$out/tmp"
mkdir -p "$TMPDIR"

cd "$out"
"$src/configure" --prefix="$out/install" "${opts[@]}"

make_cmd=make
command -v mingw32-make >/dev/null && make_cmd=mingw32-make

if [ "$make_cmd" = mingw32-make ]; then
    posix="$(cygpath -u "$root")"
    win="$(cygpath -m "$root")"
    sed -i "s#$posix#$win#g" [Mm]akefile ffbuild/config.mak config.h
fi

"$make_cmd" -j"$(nproc 2>/dev/null || echo 8)"
echo "build_ffmpeg_android: pronto em $out"
