#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
export NFS_RUNTIME_D3D9_LIBRARY="$HOME/.cache/nfsu2-dxvk-query/libdxvk_android.so"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export DXVK_LOG_LEVEL=warn
export DXVK_LOG_PATH=none
if [[ ! -f "$NFS_RUNTIME_D3D9_LIBRARY" ]];then
  bash "$test_dir/../../ports/dxvk_android/query_check.sh"
fi
bash "$test_dir/run.sh" "$@"
