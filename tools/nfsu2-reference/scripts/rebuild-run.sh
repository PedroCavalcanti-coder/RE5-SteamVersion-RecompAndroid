#!/usr/bin/env bash
# Regenerate the C (when catalog/config/pcrecomp changed), build, run.
set -e
cd "$(dirname "$0")/.."
W=../work
if [ "${RELIFT:-0}" = 1 ]; then
  python scripts/run_lift.py lift $W/SPEED2.analysis.exe $W/catalog.json src/recomp/gen --split 400
fi
scripts/build.sh
scripts/run.sh "$@"
