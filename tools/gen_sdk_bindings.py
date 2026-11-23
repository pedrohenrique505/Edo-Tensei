#!/usr/bin/env python3
"""Gera runtime/overrides/edo_sdk_bindings.inc a partir do TOML do ps2_analyzer.

Uso:
  external/PS2Recomp/out/build/ps2xAnalyzer/ps2_analyzer game/SLES_556.05 ghidra/analyzer/analyzer.toml
  python3 tools/gen_sdk_bindings.py [ghidra/analyzer/analyzer.toml]
"""
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "ghidra/analyzer/analyzer.toml"
OUT = ROOT / "runtime/overrides/edo_sdk_bindings.inc"

# Não ligar: substituídos por implementação nossa, ou só TODO no runtime.
EXCLUDE = {
    "write",
    "sceTtyWrite", "sceTtyRead", "sceTtyInit", "sceTtyHandler",
    "sceDeci2Open", "sceDeci2ReqSend", "sceDeci2Poll", "sceDeci2ExRecv", "sceDeci2ExSend",
}

stubs = tomllib.loads(SRC.read_text())["general"]["stubs"]
entries, skipped = [], []
for item in stubs:
    name, addr = item.split("@")
    (skipped if name in EXCLUDE else entries).append((int(addr, 16), name))
entries.sort()

lines = [
    "// GERADO por tools/gen_sdk_bindings.py a partir do ps2_analyzer. Não edite à mão.",
    f"// {len(entries)} bindings; excluídos: {', '.join(sorted(n for _, n in skipped))}",
]
lines += [f'{{0x{a:08X}u, "{n}"}},' for a, n in entries]
OUT.write_text("\n".join(lines) + "\n")
print(f"{OUT.relative_to(ROOT)}: {len(entries)} bindings ({len(skipped)} excluídos)")
