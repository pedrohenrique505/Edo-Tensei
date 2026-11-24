#!/usr/bin/env python3
"""Recompila overlays MWo3 (BTL, ADV, ETC) com o ps2_recomp usando os ELFs e
CSVs exportados por tools/export_overlays.sh, gera diretorios generated_ovl_<nome>/
com simbolos prefixados (sem colisao entre overlays) e cria register_ovl_<nome>.cpp
para troca dinamica na g_sparseFunctionTable.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS2_RECOMP = os.path.join(ROOT, "external", "PS2Recomp", "out", "build", "ps2xRecomp", "ps2_recomp")
OVL_EXPORT_DIR = os.path.join(ROOT, "ghidra", "export", "overlays")

ENTRY_RE = re.compile(
    r"g_ps2RecompiledFunctionTable\[\d+\]\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;\s*//\s*(0x[0-9a-fA-F]+)"
)


def prefix_csv(src_csv: str, dst_csv: str, prefix: str) -> int:
    count = 0
    with open(src_csv, "r", newline="") as fin, open(dst_csv, "w", newline="") as fout:
        reader = csv.reader(fin)
        writer = csv.writer(fout)
        header = next(reader)
        writer.writerow(header)
        for row in reader:
            if not row:
                continue
            name = row[0]
            if not name.startswith(prefix):
                row[0] = prefix + name
            writer.writerow(row)
            count += 1
    return count


def write_recomp_toml(toml_path: str, elf_path: str, out_dir: str, csv_path: str) -> None:
    content = (
        "[general]\n"
        f'input = "{elf_path}"\n'
        f'output = "{out_dir}"\n'
        f'ghidra_output = "{csv_path}"\n'
        "single_file_output = false\n"
        "patch_syscalls = false\n"
        "patch_cop0 = true\n"
        "patch_cache = true\n"
        "stubs = []\n"
        "skip = []\n"
    )
    with open(toml_path, "w") as f:
        f.write(content)


def convert_register_functions(out_dir: str, lower: str) -> int:
    reg_src = os.path.join(out_dir, "register_functions.cpp")
    reg_dst = os.path.join(out_dir, f"register_ovl_{lower}.cpp")

    entries: list[tuple[int, str]] = []
    with open(reg_src, "r") as f:
        for line in f:
            m = ENTRY_RE.search(line)
            if m:
                sym = m.group(1)
                addr = int(m.group(2), 16)
                entries.append((addr, sym))

    with open(reg_dst, "w") as f:
        f.write('#include "ps2_runtime.h"\n')
        f.write('#include "ps2_recompiled_functions.h"\n')
        f.write("#include <cstddef>\n")
        f.write("#include <cstdint>\n\n")
        f.write("struct EdoOverlayEntry {\n")
        f.write("    uint32_t address;\n")
        f.write("    PS2Runtime::RecompiledFunction fn;\n")
        f.write("};\n\n")
        f.write("namespace {\n")
        f.write("const EdoOverlayEntry kEntries[] = {\n")
        for addr, sym in entries:
            f.write(f"    {{ 0x{addr:08x}u, &{sym} }},\n")
        f.write("};\n")
        f.write("} // namespace\n\n")
        f.write(f"const EdoOverlayEntry *edoOverlayEntries_{lower}(size_t *outCount) {{\n")
        f.write("    if (outCount) {\n")
        f.write(f"        *outCount = {len(entries)}u;\n")
        f.write("    }\n")
        f.write("    return kEntries;\n")
        f.write("}\n")

    os.remove(reg_src)
    vu1 = os.path.join(out_dir, "vu1_recompiled.cpp")
    if os.path.exists(vu1):
        os.remove(vu1)
    return len(entries)


def recompile_overlay(ovl: str) -> None:
    up = ovl.upper()
    lower = ovl.lower()
    prefix = f"ovl_{lower}_"

    elf_path = os.path.join(OVL_EXPORT_DIR, f"{up}.elf")
    raw_csv = os.path.join(OVL_EXPORT_DIR, f"{up}.csv")
    prefixed_csv = os.path.join(OVL_EXPORT_DIR, f"{up}.prefixed.csv")
    toml_path = os.path.join(OVL_EXPORT_DIR, f"recomp.{lower}.toml")
    out_dir = os.path.join(ROOT, f"generated_ovl_{lower}")

    if not os.path.exists(elf_path) or not os.path.exists(raw_csv):
        raise FileNotFoundError(
            f"Faltam artefatos de {up} em {OVL_EXPORT_DIR}. Rode tools/export_overlays.sh {up} primeiro."
        )

    csv_rows = prefix_csv(raw_csv, prefixed_csv, prefix)
    write_recomp_toml(toml_path, elf_path, out_dir, prefixed_csv)

    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    print(f"[recomp_overlays] {up}: {csv_rows} linhas CSV -> rodando ps2_recomp...")
    subprocess.run([PS2_RECOMP, toml_path], check=True)

    # Verifica se algum simbolo em ps2_recompiled_functions.h ficou sem prefixo
    hdr_path = os.path.join(out_dir, "ps2_recompiled_functions.h")
    unprefixed = []
    with open(hdr_path, "r") as f:
        for line in f:
            m = re.match(r"void\s+([A-Za-z_][A-Za-z0-9_]*)\(", line)
            if m and not m.group(1).startswith(prefix):
                unprefixed.append(m.group(1))
    if unprefixed:
        print(
            f"[recomp_overlays] AVISO {up}: {len(unprefixed)} funcoes sem prefixo (ex.: {unprefixed[:5]}), renomeando..."
        )
        # Se o ps2_recomp gerou funcoes extras sem prefixo (ex.: entry_0x...), prefixamos em todos os arquivos do diretorio
        pattern = re.compile(r"\b(" + "|".join(re.escape(s) for s in unprefixed) + r")\b")
        for fname in os.listdir(out_dir):
            if not fname.endswith((".cpp", ".h")):
                continue
            fpath = os.path.join(out_dir, fname)
            with open(fpath, "r") as f:
                text = f.read()
            new_text = pattern.sub(lambda m: prefix + m.group(1), text)
            if new_text != text:
                with open(fpath, "w") as f:
                    f.write(new_text)

    entry_count = convert_register_functions(out_dir, lower)
    print(f"[recomp_overlays] {up}: OK ({entry_count} entrypoints em register_ovl_{lower}.cpp)")


def main() -> int:
    ap = argparse.ArgumentParser(description="Recompila overlays MWo3 com ps2_recomp.")
    ap.add_argument("overlays", nargs="*", default=["ETC", "ADV", "BTL"])
    args = ap.parse_args()

    if not os.path.exists(PS2_RECOMP):
        print(f"ERRO: ps2_recomp nao encontrado em {PS2_RECOMP}", file=sys.stderr)
        return 1

    for ovl in args.overlays:
        recompile_overlay(ovl)
    return 0


if __name__ == "__main__":
    sys.exit(main())
