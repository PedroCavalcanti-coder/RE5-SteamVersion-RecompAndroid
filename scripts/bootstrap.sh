#!/usr/bin/env bash
# Prepara uma sessão na nuvem: dependências Python do pipeline e binários do jogo
# via Git LFS. Chamado pelo hook SessionStart (.claude/settings.json). Idempotente.
# Fora das sessões remotas não faz nada, para não mexer no Python da sua máquina.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
    exit 0
fi

cd "$(dirname "$0")/.."

if ! python3 -c "import capstone, pefile, unicorn" 2>/dev/null; then
    python3 -m pip install --quiet -r scripts/requirements.txt >/dev/null 2>&1 || \
        echo "bootstrap: falha ao instalar scripts/requirements.txt" >&2
fi

# Os arquivos do jogo não estão no git (D7): só avisa se faltarem.
[ -f bin_input/re5dx9.clean.exe ] || echo "bootstrap: bin_input/re5dx9.clean.exe ausente (arquivos do jogo não vêm do git)" >&2

if [ ! -f build/ffmpeg/libavcodec/libavcodec.a ]; then
    bash scripts/build_ffmpeg.sh >/dev/null 2>&1 || \
        echo "bootstrap: scripts/build_ffmpeg.sh falhou; rode à mão" >&2
fi
exit 0
