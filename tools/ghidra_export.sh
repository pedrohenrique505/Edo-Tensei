#!/usr/bin/env bash
# Importa e analisa o ELF no Ghidra headless (projeto separado do da GUI)
# e roda o ExportPS2Functions.java do PS2Recomp, gerando TOML + CSV.
#
# Uso: tools/ghidra_export.sh [caminho/do/SLES_556.05]
# Requer: Ghidra em /opt/ghidra, plugin ghidra-emotionengine-reloaded,
#         ExportPS2Functions.java em external/PS2Recomp/ps2xRecomp/tools/ghidra
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$ROOT/game/SLES_556.05}"
GHIDRA="${GHIDRA_HOME:-/opt/ghidra}"
EXPORT_SCRIPT_DIR="${EXPORT_SCRIPT_DIR:-$ROOT/external/PS2Recomp/ps2xRecomp/tools/ghidra}"
PROJ_DIR="$ROOT/ghidra/headless"
OUT_DIR="$ROOT/ghidra/export"
NAME="$(basename "$ELF")"

mkdir -p "$PROJ_DIR" "$OUT_DIR"

SCRIPT=(-scriptPath "$ROOT/tools/ghidra;$EXPORT_SCRIPT_DIR"
        -postScript CreateGapFunctions.java
        -postScript ExportPS2Functions.java
        "$OUT_DIR/$NAME.toml" "$OUT_DIR/$NAME.csv")

if [ -d "$PROJ_DIR/EdoTensei_headless.rep" ]; then
  "$GHIDRA/support/analyzeHeadless" "$PROJ_DIR" EdoTensei_headless \
    -process "$NAME" -noanalysis "${SCRIPT[@]}"
else
  "$GHIDRA/support/analyzeHeadless" "$PROJ_DIR" EdoTensei_headless \
    -import "$ELF" -processor "r5900:LE:32:default" "${SCRIPT[@]}"
fi

echo "Saída em: $OUT_DIR"
ls -la "$OUT_DIR"
