#!/usr/bin/env python3
"""Converte um overlay Metrowerks MWo3 (BTL.BIN, ADV.BIN, ETC.BIN) em um ELF
MIPS32 LE sintetico compativel com Ghidra (r5900:LE:32:default) e ps2_recomp.

Layout do cabecalho MWo3 (0x40 bytes):
  +0x00  magic       "MWo3" (0x336F574D)
  +0x04  overlay_id  1=BTL, 2=ADV, 3=ETC
  +0x08  base_addr   0x006C6D00
  +0x0C  text_size   tamanho de .text (comeca em base_addr + 0x40)
  +0x10  data_size   tamanho de .data/.rodata/.text.ctors/.ctors
  +0x14  bss_size    tamanho de .bss
  +0x18  ctors_start inicio da tabela de ponteiros de construtores estaticos
  +0x1C  ctors_end   fim da tabela de ponteiros de construtores estaticos
  +0x20  name        string ASCII (ex.: "BTL_product.bin")
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

MWO3_MAGIC = 0x336F574D
HDR_SIZE = 0x40
PAGE_OFFSET = 0x1000

# Constantes ELF32
ET_EXEC = 2
EM_MIPS = 8
PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4
SHT_NULL = 0
SHT_PROGBITS = 1
SHT_STRTAB = 3
SHT_NOBITS = 8
SHF_WRITE = 1
SHF_ALLOC = 2
SHF_EXECINSTR = 4


def parse_mwo3(raw: bytes) -> dict:
    if len(raw) < HDR_SIZE:
        raise ValueError(f"Arquivo muito pequeno ({len(raw)} bytes)")
    magic, ovl_id, base, text_sz, data_sz, bss_sz, ctors_s, ctors_e = struct.unpack_from(
        "<8I", raw, 0
    )
    if magic != MWO3_MAGIC:
        raise ValueError(f"Magic invalido: 0x{magic:08X} (esperado 0x{MWO3_MAGIC:08X})")
    if HDR_SIZE + text_sz + data_sz != len(raw):
        raise ValueError(
            f"Tamanho inconsistente: 0x40 + 0x{text_sz:X} + 0x{data_sz:X} != 0x{len(raw):X}"
        )
    name = raw[0x20:0x40].split(b"\x00", 1)[0].decode("ascii", errors="replace")

    ctors = []
    if ctors_e > ctors_s:
        off_s = ctors_s - base
        off_e = ctors_e - base
        count = (off_e - off_s) // 4
        ctors = list(struct.unpack_from(f"<{count}I", raw, off_s))

    return {
        "id": ovl_id,
        "name": name,
        "base": base,
        "text_start": base + HDR_SIZE,
        "text_size": text_sz,
        "data_start": base + HDR_SIZE + text_sz,
        "data_size": data_sz,
        "bss_start": base + len(raw),
        "bss_size": bss_sz,
        "ctors_start": ctors_s,
        "ctors_end": ctors_e,
        "ctors": ctors,
        "file_size": len(raw),
    }


def build_elf(raw: bytes, info: dict) -> bytes:
    base = info["base"]
    text_start = info["text_start"]
    text_sz = info["text_size"]
    data_start = info["data_start"]
    data_end = base + info["file_size"]
    bss_start = info["bss_start"]
    bss_sz = info["bss_size"]
    ctors = info["ctors"]
    ctors_s = info["ctors_start"]

    # Em BTL.BIN e ADV.BIN, o codigo dos construtores estaticos fica contiguo
    # no final da regiao .data, de min(ctors) ate ctors_start.
    # Marcamos essa regiao como .text.ctors (SHF_EXECINSTR) para que o Ghidra,
    # o CreateGapFunctions e o ps2_recomp analisem/recompilem tambem os ctors.
    sections: list[tuple[str, int, int, int, int, int, int]] = []
    # (name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_addralign)
    sections.append((".header", SHT_PROGBITS, SHF_ALLOC, base, PAGE_OFFSET, HDR_SIZE, 16))
    sections.append(
        (
            ".text",
            SHT_PROGBITS,
            SHF_ALLOC | SHF_EXECINSTR,
            text_start,
            PAGE_OFFSET + HDR_SIZE,
            text_sz,
            16,
        )
    )

    if ctors:
        ctor_code_start = min(ctors)
        if not (data_start <= ctor_code_start < ctors_s <= data_end):
            raise ValueError(
                f"Layout inesperado de ctors: data_start=0x{data_start:X} "
                f"ctor_code_start=0x{ctor_code_start:X} ctors_s=0x{ctors_s:X} data_end=0x{data_end:X}"
            )
        plain_data_sz = ctor_code_start - data_start
        ctor_code_sz = ctors_s - ctor_code_start
        ctors_tail_sz = data_end - ctors_s
        if plain_data_sz > 0:
            sections.append(
                (
                    ".data",
                    SHT_PROGBITS,
                    SHF_ALLOC | SHF_WRITE,
                    data_start,
                    PAGE_OFFSET + (data_start - base),
                    plain_data_sz,
                    16,
                )
            )
        sections.append(
            (
                ".text.ctors",
                SHT_PROGBITS,
                SHF_ALLOC | SHF_EXECINSTR,
                ctor_code_start,
                PAGE_OFFSET + (ctor_code_start - base),
                ctor_code_sz,
                16,
            )
        )
        if ctors_tail_sz > 0:
            sections.append(
                (
                    ".ctors",
                    SHT_PROGBITS,
                    SHF_ALLOC | SHF_WRITE,
                    ctors_s,
                    PAGE_OFFSET + (ctors_s - base),
                    ctors_tail_sz,
                    4,
                )
            )
    else:
        sections.append(
            (
                ".data",
                SHT_PROGBITS,
                SHF_ALLOC | SHF_WRITE,
                data_start,
                PAGE_OFFSET + (data_start - base),
                info["data_size"],
                16,
            )
        )

    if bss_sz > 0:
        sections.append(
            (
                ".bss",
                SHT_NOBITS,
                SHF_ALLOC | SHF_WRITE,
                bss_start,
                PAGE_OFFSET + info["file_size"],
                bss_sz,
                16,
            )
        )

    # Constroi .shstrtab
    shstrtab = bytearray(b"\x00")
    name_offsets: list[int] = [0]
    for s in sections:
        name_offsets.append(len(shstrtab))
        shstrtab.extend(s[0].encode("ascii") + b"\x00")
    shstrtab_name_off = len(shstrtab)
    shstrtab.extend(b".shstrtab\x00")

    payload_end = PAGE_OFFSET + len(raw)
    shstrtab_off = (payload_end + 15) & ~15
    shoff = (shstrtab_off + len(shstrtab) + 15) & ~15

    # Entry point: primeiro ctor ou inicio de .text
    entry = ctors[0] if ctors else text_start

    # ELF32 header (52 bytes)
    e_ident = (
        b"\x7fELF"
        + bytes([1, 1, 1, 0, 0])  # ELFCLASS32, ELFDATA2LSB, EV_CURRENT, SYSV
        + b"\x00" * 7
    )
    shnum = 1 + len(sections) + 1  # NULL + sections + .shstrtab
    shstrndx = shnum - 1
    e_flags = 0x20924001  # flags tipicas de EE/R5900 (MIPS III + EABI32 + R5900)
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        e_ident,
        ET_EXEC,
        EM_MIPS,
        1,  # e_version
        entry,
        52,  # e_phoff
        shoff,
        e_flags,
        52,  # e_ehsize
        32,  # e_phentsize
        1,  # e_phnum
        40,  # e_shentsize
        shnum,
        shstrndx,
    )

    # Program header unico PT_LOAD (RWX) cobrindo o overlay inteiro + bss
    phdr = struct.pack(
        "<IIIIIIII",
        PT_LOAD,
        PAGE_OFFSET,
        base,
        base,
        len(raw),
        len(raw) + bss_sz,
        PF_R | PF_W | PF_X,
        0x1000,
    )

    out = bytearray()
    out.extend(ehdr)
    out.extend(phdr)
    out.extend(b"\x00" * (PAGE_OFFSET - len(out)))
    out.extend(raw)
    out.extend(b"\x00" * (shstrtab_off - len(out)))
    out.extend(shstrtab)
    out.extend(b"\x00" * (shoff - len(out)))

    # SHT_NULL
    out.extend(b"\x00" * 40)
    # Secoes
    for idx, (sname, stype, sflags, saddr, soff, ssz, salign) in enumerate(sections, start=1):
        out.extend(
            struct.pack(
                "<IIIIIIIIII",
                name_offsets[idx],
                stype,
                sflags,
                saddr,
                soff,
                ssz,
                0,
                0,
                salign,
                0,
            )
        )
    # .shstrtab
    out.extend(
        struct.pack(
            "<IIIIIIIIII",
            shstrtab_name_off,
            SHT_STRTAB,
            0,
            0,
            shstrtab_off,
            len(shstrtab),
            0,
            0,
            1,
            0,
        )
    )
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Converte overlay MWo3 (.BIN) em ELF32 MIPS R5900.")
    ap.add_argument("input", help="Caminho para BTL.BIN / ADV.BIN / ETC.BIN")
    ap.add_argument("output", help="Caminho do ELF de saida")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        raw = f.read()

    info = parse_mwo3(raw)
    elf = build_elf(raw, info)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(elf)

    print(
        f"[mwo3_to_elf] {os.path.basename(args.input)} -> {args.output} "
        f"(id={info['id']} name={info['name']} base=0x{info['base']:08X} "
        f"text=0x{info['text_size']:X} data=0x{info['data_size']:X} "
        f"bss=0x{info['bss_size']:X} ctors={len(info['ctors'])})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
