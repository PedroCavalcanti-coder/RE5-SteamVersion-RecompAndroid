#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
export NFS_D3D9_TEST_DISPLAY=1
bash "$test_dir/../../ports/dxvk_android/query_check.sh"
xvfb-run -a -s '-screen 0 1280x720x24' bash "$test_dir/run_with_d3d9.sh" "$@"
