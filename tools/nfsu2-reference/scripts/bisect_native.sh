#!/usr/bin/env bash
# Hybrid bisection: is the symptom (crash) present when guest code in
# [lo, hi) runs as the original machine code? Runs in the background (no focus).
#   scripts/bisect_native.sh LO HI [seconds]   -> prints GOOD (no crash) or BAD
cd "$(dirname "$0")/.."
lo=$1; hi=$2; secs=${3:-65}
log=logs/bisect_${lo}_${hi}.txt
NFSU2_NATIVE_RANGE="${lo}-${hi}" NFSU2_BACKGROUND=1 NFSU2_TAP="20@25000,20@32000,20@40000" \
  timeout $secs ./build/NFSU2-Recompiled.exe > $log 2>&1
if grep -q "CRASH\] reason" $log; then
  echo "BAD  $lo-$hi  $(grep -m1 'guest func' $log | sed 's/.*: //')"
else
  echo "GOOD $lo-$hi  frames=$(grep -o 'frames [0-9]*' $log | tail -1)"
fi
