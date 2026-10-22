#!/usr/bin/env bash
# Run the recompiled game; logs go to logs/. Extra args are passed through
# (--game-root DIR, --trace-native, --trace-callbacks, pcrecomp trace options).
cd "$(dirname "$0")/.."
exec ./build/NFSU2-Recompiled.exe --log-dir logs "$@"
