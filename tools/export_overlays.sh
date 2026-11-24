#!/usr/bin/env bash
# Converte overlays MWo3 (BTL.BIN, ADV.BIN, ETC.BIN) em ELFs sinteticos,
# analisa cada um no Ghidra headless (projeto proprio) e exporta TOML + CSV.
#
# Uso: tools/export_overlays.sh [BTL ADV ETC]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA="${GHIDRA_HOME:-/opt/ghidra}"
EXPORT_SCRIPT_DIR="${EXPORT_SCRIPT_DIR:-$ROOT/external/PS2Recomp/ps2xRecomp/tools/ghidra}"
PROJ_DIR="$ROOT/ghidra/headless_overlays"
OUT_DIR="$ROOT/ghidra/export/overlays"

mkdir -p "$PROJ_DIR" "$OUT_DIR"

OVERLAYS=("$@")
if [ ${#OVERLAYS[@]} -eq 0 ]; then
  OVERLAYS=(ETC ADV BTL)
fi

for OVL in "${OVERLAYS[@]}"; do
  UP="${OVL^^}"
  BIN="$ROOT/game/PRG/${UP}.BIN"
  ELF="$OUT_DIR/${UP}.elf"
  TOML="$OUT_DIR/${UP}.toml"
  CSV="$OUT_DIR/${UP}.csv"

  if [ ! -f "$BIN" ]; then
    echo "ERRO: overlay nao encontrado: $BIN" >&2
    exit 1
  fi

  python3 "$ROOT/tools/mwo3_to_elf.py" "$BIN" "$ELF"

  PROJ_NAME="EdoOverlay_${UP}"
  rm -rf "$PROJ_DIR/${PROJ_NAME}.gpr" "$PROJ_DIR/${PROJ_NAME}.rep"

  "$GHIDRA/support/analyzeHeadless" "$PROJ_DIR" "$PROJ_NAME" \
    -import "$ELF" -processor "r5900:LE:32:default" \
    -scriptPath "$ROOT/tools/ghidra;$EXPORT_SCRIPT_DIR" \
    -postScript SeedOverlayFunctions.java

  "$GHIDRA/support/analyzeHeadless" "$PROJ_DIR" "$PROJ_NAME" \
    -process "${UP}.elf" -noanalysis \
    -scriptPath "$ROOT/tools/ghidra;$EXPORT_SCRIPT_DIR" \
    -postScript CreateGapFunctions.java \
    -postScript ExportPS2Functions.java "$TOML" "$CSV"

  echo "[export_overlays] ${UP}: $(wc -l < "$CSV") linhas em $CSV"
done
