#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
task_build="$HOME/.cache/nfsu2-graphics-memory"
cmake -S "$test_dir" -B "$task_build" -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build "$task_build" -j2
"$task_build/graphics_memory_check"
