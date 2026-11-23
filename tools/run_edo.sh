#!/usr/bin/env bash
# Executa o build nativo de Naruto Shippuden: Ultimate Ninja 5 (SLES_556.05).
# Por padrão ativa o backend Vulkan paraLLEl-GS (gpu-present) e aponta para a ISO em ~/Downloads.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_ISO="$HOME/Downloads/Naruto Shippuden - Ultimate Ninja 5 (Europe) (En,Fr,De,Es,It)/Naruto Shippuden - Ultimate Ninja 5 (Europe) (En,Fr,De,Es,It).iso"

export EDO_ISO="${EDO_ISO:-$DEFAULT_ISO}"
export EDO_GS_BACKEND="${EDO_GS_BACKEND:-gpu-present}"

cd "$ROOT/game"
exec "$ROOT/build/edo-tensei" "$ROOT/game/SLES_556.05" "$@"
