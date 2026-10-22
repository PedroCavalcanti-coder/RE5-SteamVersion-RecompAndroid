#!/usr/bin/env bash
# Compila o FFmpeg mínimo dos vídeos do RE5 (etapa 4.5): demuxer asf e os
# decodificadores vc1/wmv3 (vídeo) e wmapro/wmav2 (áudio). Bibliotecas estáticas
# em build/ffmpeg (ou no diretório do 1º argumento), que o runtime/CMakeLists.txt
# procura em RE5_FFMPEG_DIR. Idempotente: não refaz o que já existe.
#
#   scripts/build_ffmpeg.sh [saida]        # Linux ou MSYS2/Git Bash com MinGW
#
# No Windows precisa de gcc (MinGW-w64), mingw32-make (ou make) e nasm no PATH.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/ffmpeg}"
src="$root/third_party/FFmpeg"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

if [ -f "$out/libavcodec/libavcodec.a" ] && [ -f "$out/libavformat/libavformat.a" ] && [ -f "$out/libavutil/libavutil.a" ]; then
    echo "build_ffmpeg: já compilado em $out"
    exit 0
fi
[ -x "$src/configure" ] || { echo "build_ffmpeg: $src/configure não existe (submódulo do FFmpeg?)" >&2; exit 1; }

opts=(--disable-everything --enable-decoder=vc1,wmv3,wmapro,wmav2 --enable-demuxer=asf --enable-parser=vc1
      --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale
      --disable-swresample --disable-network --disable-autodetect --enable-static --disable-shared
      --disable-debug)
make_cmd=make
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
    opts+=(--target-os=mingw32 --arch=x86_64 --cc=gcc)
    command -v mingw32-make >/dev/null && make_cmd=mingw32-make
    # O configure cria temporários em $TMPDIR: tem que ser um caminho POSIX.
    export TMPDIR="$out/tmp"
    mkdir -p "$TMPDIR"
    ;;
esac
command -v nasm >/dev/null && opts+=(--x86asmexe=nasm) || opts+=(--disable-x86asm)

cd "$out"
"$src/configure" --prefix="$out/install" "${opts[@]}"
if [ "$make_cmd" = mingw32-make ]; then
    # O mingw32-make não entende /c/...: os caminhos do configure viram C:/...
    posix="$(cygpath -u "$root")"
    win="$(cygpath -m "$root")"
    sed -i "s#$posix#$win#g" [Mm]akefile ffbuild/config.mak config.h
fi
"$make_cmd" -j"$(nproc 2>/dev/null || echo 8)"
echo "build_ffmpeg: pronto em $out"
