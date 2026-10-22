#!/usr/bin/env bash
# Configure (first time) and build with MSVC x86 + Ninja.
set -e
cd "$(dirname "$0")/.."
source scripts/vsenv.sh x86 >/dev/null
[ -f build/build.ninja ] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
cmake --build build -j "${JOBS:-16}"
