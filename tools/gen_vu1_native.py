#!/usr/bin/env python3
"""Gera executor nativo C++ (generated/vu1_recompiled.cpp) para microprogramas VU1 quentes.

Traduz snapshots de 16 KiB da VU1 Code RAM em funcoes C++ registradas via
ps2_vu1_register_native_executor(), preservando:
  - latencia de 7 ciclos do DIV/SQRT/RSQRT (mesmo sem WAITQ);
  - latencia de 4 ciclos das flags MAC para FMAND;
  - latencia de 29 ciclos do ESIN + WAITP/MFP;
  - shadow de registrador VF quando upper e lower acessam o mesmo VF no mesmo par;
  - delay de 1 ciclo de escrita em VI para branches inteiros (IBEQ/IBNE/IBLTZ/IBGEZ);
  - delay slot de branches e de E-bit;
  - XGKICK com parsing de GIFtag (PACKED/REGLIST/IMAGE) e envio via Path1.
"""
import os
import struct
import sys

def hash64(b: bytes) -> int:
    h = 14695981039346656037
    for i in range(0, len(b), 8):
        w = struct.unpack_from("<Q", b, i)[0]
        h = ((h ^ w) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def imm11(instr: int) -> int:
    v = instr & 0x7FF
    return v - 0x800 if (v & 0x400) else v

def dest_mask(instr: int) -> int:
    return (instr >> 21) & 0xF

def lanes_from_dest(dest: int):
    return [c for c in range(4) if dest & (8 >> c)]

def upper_write_reg(hi: int) -> int:
    op = hi & 0x3F
    if op == 0x3F and ((hi >> 6) & 0x1F) == 0x0B:
        return 0  # NOP
    if op <= 0x2F:
        return (hi >> 6) & 0x1F  # fd
    if op >= 0x3C:
        sp = (hi & 3) | ((hi >> 4) & 0x7C)
        if 0x10 <= sp <= 0x17 or sp == 0x1D:
            return (hi >> 16) & 0x1F  # ft
    return 0

def upper_read_lanes(hi: int):
    """Returns dict {reg: lane_mask} for VF reads of upper instruction."""
    op = hi & 0x3F
    if op == 0x3F and ((hi >> 6) & 0x1F) == 0x0B:
        return {}
    dest = dest_mask(hi)
    fs = (hi >> 11) & 0x1F
    ft = (hi >> 16) & 0x1F
    reads = {}
    def add(r, m):
        if r != 0 and m != 0:
            reads[r] = reads.get(r, 0) | m
    if op <= 0x2F:
        if op == 0x2E:  # OPMSUB
            add(fs, 0xE)
            add(ft, 0xE)
        else:
            add(fs, dest)
            if op <= 0x1B:
                add(ft, 8 >> (op & 3))
            elif op >= 0x28:
                add(ft, dest)
    elif op >= 0x3C:
        sp = (hi & 3) | ((hi >> 4) & 0x7C)
        if sp in (0x2F, 0x30):
            return {}
        if sp == 0x2E:  # OPMULA
            add(fs, 0xE)
            add(ft, 0xE)
        elif sp == 0x1F:  # CLIP
            add(fs, 0xE)
            add(ft, 0x1)
        else:
            add(fs, dest)
            if sp <= 0x0F or (0x18 <= sp <= 0x1B):
                add(ft, 8 >> (sp & 3))
            elif 0x28 <= sp <= 0x2D:
                add(ft, dest)
    return reads

def lower_vf_lanes(lo: int, i_bit: bool):
    """Returns (reads_dict, write_reg, write_mask) for VF accesses of lower instruction."""
    if i_bit or lo == 0x8000033C or lo == 0:
        return {}, 0, 0
    top = (lo >> 25) & 0x7F
    dest = dest_mask(lo)
    fs = (lo >> 11) & 0x1F
    ft = (lo >> 16) & 0x1F
    reads = {}
    def add(r, m):
        if r != 0 and m != 0:
            reads[r] = reads.get(r, 0) | m
    if top == 0x00:  # LQ
        return reads, ft, dest
    if top == 0x01:  # SQ
        add(fs, dest)
        return reads, 0, 0
    if top == 0x40:
        lop = lo & 0x3F
        if lop >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 in (0x30, 0x31):  # MOVE/MR32
                add(fs, 0xF if f2 == 0x31 else dest)
                return reads, ft, dest
            if f2 in (0x34, 0x36):  # LQI/LQD
                return reads, ft, dest
            if f2 in (0x35, 0x37):  # SQI/SQD
                add(fs, dest)
                return reads, 0, 0
            if f2 in (0x38, 0x3A):  # DIV/RSQRT
                add(fs, 8 >> ((lo >> 21) & 3))
                add(ft, 8 >> ((lo >> 23) & 3))
                return reads, 0, 0
            if f2 == 0x39:  # SQRT
                add(ft, 8 >> ((lo >> 23) & 3))
                return reads, 0, 0
            if f2 == 0x3C:  # MTIR
                add(fs, 8 >> ((lo >> 21) & 3))
                return reads, 0, 0
            if f2 in (0x3D, 0x64):  # MFIR/MFP
                return reads, ft, dest
            if 0x70 <= f2 <= 0x7A:  # EFU
                add(fs, 8 >> ((lo >> 21) & 3))
                return reads, 0, 0
    return reads, 0, 0

def lower_vi_write(lo: int, i_bit: bool) -> int:
    """Returns VI register written by instructions with delaysNextBranchRead=true (matching ps2_vu1_core.cpp)."""
    if i_bit or lo == 0x8000033C or lo == 0:
        return 0
    top = (lo >> 25) & 0x7F
    vis = (lo >> 11) & 0x1F
    vit = (lo >> 16) & 0x1F
    vid = (lo >> 6) & 0x1F
    if top in (0x08, 0x09):  # IADDIU, ISUBIU (NOT ILW, NOT FMAND!)
        return vit & 0xF
    if top == 0x40:
        lop = lo & 0x3F
        if lop in (0x30, 0x31, 0x34, 0x35):  # IADD, ISUB, IAND, IOR
            return vid & 0xF
        if lop == 0x32:  # IADDI
            return vit & 0xF
        if lop >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 in (0x34, 0x36):  # LQI, LQD
                return vis & 0xF
            if f2 in (0x35, 0x37):  # SQI, SQD
                return vit & 0xF
            if f2 == 0x3C:  # MTIR (NOT XTOP/XITOP!)
                return vit & 0xF
    return 0

def emit_upper(hi: int, lines: list, need_mac: bool) -> bool:
    """Emits C++ for upper instruction. Returns True if opcode supported."""
    op = hi & 0x3F
    if op == 0x3F and ((hi >> 6) & 0x1F) == 0x0B:
        return True  # NOP
    dest = dest_mask(hi)
    lanes = lanes_from_dest(dest)
    fs = (hi >> 11) & 0x1F
    ft = (hi >> 16) & 0x1F
    fd = (hi >> 6) & 0x1F

    def emit_fmac(target_expr: str, expr_fn, is_acc=False):
        if not lanes:
            return
        lines.append("    {")
        for c in lanes:
            lines.append(f"      float r{c} = {expr_fn(c)};")
        if need_mac:
            lines.append("      uint32_t mac = 0u;")
            for c in lanes:
                bit = 8 >> c
                lines.append(f"      r{c} = normMac(r{c}, {bit}u, mac);")
            lines.append("      pushMac(mac, cyc + 4u);")
        if is_acc:
            for c in lanes:
                lines.append(f"      acc[{c}] = r{c};")
        elif fd != 0:
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
        lines.append("    }")

    # Standard FMAC ops
    if 0x00 <= op <= 0x03:  # ADDbc
        bc = op & 3
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(vf[{ft}][{bc}])")
        return True
    if 0x04 <= op <= 0x07:  # SUBbc
        bc = op & 3
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(vf[{ft}][{bc}])")
        return True
    if 0x08 <= op <= 0x0B:  # MADDbc
        bc = op & 3
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{bc}])")
        return True
    if 0x0C <= op <= 0x0F:  # MSUBbc
        bc = op & 3
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{bc}])")
        return True
    if 0x10 <= op <= 0x13:  # MAXbc
        bc = op & 3
        if fd != 0 and lanes:
            lines.append("    {")
            lines.append(f"      float bcv = vf[{ft}][{bc}];")
            for c in lanes:
                lines.append(f"      float r{c} = vuMinMax(vf[{fs}][{c}], bcv, true);")
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
            lines.append("    }")
        return True
    if 0x14 <= op <= 0x17:  # MINIbc
        bc = op & 3
        if fd != 0 and lanes:
            lines.append("    {")
            lines.append(f"      float bcv = vf[{ft}][{bc}];")
            for c in lanes:
                lines.append(f"      float r{c} = vuMinMax(vf[{fs}][{c}], bcv, false);")
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
            lines.append("    }")
        return True
    if 0x18 <= op <= 0x1B:  # MULbc
        bc = op & 3
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{bc}])")
        return True
    if op == 0x1C:  # MULq
        lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(q)")
        return True
    if op == 0x1D:  # MAXi
        if fd != 0 and lanes:
            lines.append("    {")
            for c in lanes:
                lines.append(f"      float r{c} = vuMinMax(vf[{fs}][{c}], ireg, true);")
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
            lines.append("    }")
        return True
    if op == 0x1E:  # MULi
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(ireg)")
        return True
    if op == 0x1F:  # MINIi
        if fd != 0 and lanes:
            lines.append("    {")
            for c in lanes:
                lines.append(f"      float r{c} = vuMinMax(vf[{fs}][{c}], ireg, false);")
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
            lines.append("    }")
        return True
    if op == 0x20:  # ADDq
        lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(q)")
        return True
    if op == 0x21:  # MADDq
        lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(q)")
        return True
    if op == 0x22:  # ADDi
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(ireg)")
        return True
    if op == 0x23:  # MADDi
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(ireg)")
        return True
    if op == 0x24:  # SUBq
        lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(q)")
        return True
    if op == 0x25:  # MSUBq
        lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(q)")
        return True
    if op == 0x26:  # SUBi
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(ireg)")
        return True
    if op == 0x27:  # MSUBi
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(ireg)")
        return True
    if op == 0x28:  # ADD
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(vf[{ft}][{c}])")
        return True
    if op == 0x29:  # MADD
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{c}])")
        return True
    if op == 0x2A:  # MUL
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{c}])")
        return True
    if op == 0x2B:  # MAX
        if fd != 0 and lanes:
            lines.append("    {")
            for c in lanes:
                lines.append(f"      float r{c} = vuMinMax(vf[{fs}][{c}], vf[{ft}][{c}], true);")
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
            lines.append("    }")
        return True
    if op == 0x2C:  # SUB
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(vf[{ft}][{c}])")
        return True
    if op == 0x2D:  # MSUB
        emit_fmac(f"vf[{fd}]", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{c}])")
        return True
    if op == 0x2E:  # OPMSUB
        exprs = {
            0: f"normFast(acc[0]) - normFast(vf[{fs}][1]) * normFast(vf[{ft}][2])",
            1: f"normFast(acc[1]) - normFast(vf[{fs}][2]) * normFast(vf[{ft}][0])",
            2: f"normFast(acc[2]) - normFast(vf[{fs}][0]) * normFast(vf[{ft}][1])",
            3: "0.0f",
        }
        emit_fmac(f"vf[{fd}]", lambda c: exprs[c])
        return True
    if op == 0x2F:  # MINI
        if fd != 0 and lanes:
            lines.append("    {")
            for c in lanes:
                lines.append(f"      float r{c} = vuMinMax(vf[{fs}][{c}], vf[{ft}][{c}], false);")
            for c in lanes:
                lines.append(f"      vf[{fd}][{c}] = r{c};")
            lines.append("    }")
        return True

    if op >= 0x3C:
        sp = (hi & 3) | ((hi >> 4) & 0x7C)
        if 0x00 <= sp <= 0x03:  # ADDAbc
            bc = sp & 3
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(vf[{ft}][{bc}])", is_acc=True)
            return True
        if 0x04 <= sp <= 0x07:  # SUBAbc
            bc = sp & 3
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(vf[{ft}][{bc}])", is_acc=True)
            return True
        if 0x08 <= sp <= 0x0B:  # MADDAbc
            bc = sp & 3
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{bc}])", is_acc=True)
            return True
        if 0x0C <= sp <= 0x0F:  # MSUBAbc
            bc = sp & 3
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{bc}])", is_acc=True)
            return True
        if 0x10 <= sp <= 0x13:  # ITOF0/4/12/15
            scales = {0x10: "1.0f", 0x11: "(1.0f/16.0f)", 0x12: "(1.0f/4096.0f)", 0x13: "(1.0f/32768.0f)"}
            if ft != 0 and lanes:
                lines.append("    {")
                for c in lanes:
                    lines.append(f"      int32_t iv{c}; std::memcpy(&iv{c}, &vf[{fs}][{c}], 4);")
                for c in lanes:
                    lines.append(f"      vf[{ft}][{c}] = static_cast<float>(iv{c}) * {scales[sp]};")
                lines.append("    }")
            return True
        if 0x14 <= sp <= 0x17:  # FTOI0/4/12/15
            scales = {0x14: "1.0f", 0x15: "16.0f", 0x16: "4096.0f", 0x17: "32768.0f"}
            if ft != 0 and lanes:
                lines.append("    {")
                for c in lanes:
                    lines.append(f"      int32_t iv{c} = vuFloatToInt(normFast(vf[{fs}][{c}]), {scales[sp]});")
                for c in lanes:
                    lines.append(f"      std::memcpy(&vf[{ft}][{c}], &iv{c}, 4);")
                lines.append("    }")
            return True
        if 0x18 <= sp <= 0x1B:  # MULAbc
            bc = sp & 3
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{bc}])", is_acc=True)
            return True
        if sp == 0x1C:  # MULAq
            lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(q)", is_acc=True)
            return True
        if sp == 0x1D:  # ABS
            if ft != 0 and lanes:
                lines.append("    {")
                for c in lanes:
                    lines.append(f"      float r{c} = std::fabs(normFast(vf[{fs}][{c}]));")
                for c in lanes:
                    lines.append(f"      vf[{ft}][{c}] = r{c};")
                lines.append("    }")
            return True
        if sp == 0x1E:  # MULAi
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(ireg)", is_acc=True)
            return True
        if sp == 0x20:  # ADDAq
            lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(q)", is_acc=True)
            return True
        if sp == 0x21:  # MADDAq
            lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(q)", is_acc=True)
            return True
        if sp == 0x22:  # ADDAi
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(ireg)", is_acc=True)
            return True
        if sp == 0x23:  # MADDAi
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(ireg)", is_acc=True)
            return True
        if sp == 0x24:  # SUBAq
            lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(q)", is_acc=True)
            return True
        if sp == 0x25:  # MSUBAq
            lines.append("    if (qPending && cyc >= qReady) { q = qVal; qPending = false; }")
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(q)", is_acc=True)
            return True
        if sp == 0x26:  # SUBAi
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(ireg)", is_acc=True)
            return True
        if sp == 0x27:  # MSUBAi
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(ireg)", is_acc=True)
            return True
        if sp == 0x28:  # ADDA
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) + normFast(vf[{ft}][{c}])", is_acc=True)
            return True
        if sp == 0x29:  # MADDA
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) + normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{c}])", is_acc=True)
            return True
        if sp == 0x2A:  # MULA
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{c}])", is_acc=True)
            return True
        if sp == 0x2C:  # SUBA
            emit_fmac("acc", lambda c: f"normFast(vf[{fs}][{c}]) - normFast(vf[{ft}][{c}])", is_acc=True)
            return True
        if sp == 0x2D:  # MSUBA
            emit_fmac("acc", lambda c: f"normFast(acc[{c}]) - normFast(vf[{fs}][{c}]) * normFast(vf[{ft}][{c}])", is_acc=True)
            return True
        if sp == 0x2E:  # OPMULA
            exprs = {
                0: f"normFast(vf[{fs}][1]) * normFast(vf[{ft}][2])",
                1: f"normFast(vf[{fs}][2]) * normFast(vf[{ft}][0])",
                2: f"normFast(vf[{fs}][0]) * normFast(vf[{ft}][1])",
                3: "0.0f",
            }
            emit_fmac("acc", lambda c: exprs[c], is_acc=True)
            return True
        if sp in (0x2F, 0x30):  # NOP
            return True
    return False

def emit_lower(pc: int, lo: int, lines: list) -> bool:
    if lo == 0x8000033C or lo == 0:
        return True
    top = (lo >> 25) & 0x7F
    dest = dest_mask(lo)
    lanes = lanes_from_dest(dest)
    fs = (lo >> 11) & 0x1F
    ft = (lo >> 16) & 0x1F
    vis = fs & 0xF
    vit = ft & 0xF
    vid = ((lo >> 6) & 0x1F) & 0xF
    im11 = imm11(lo)

    if top == 0x00:  # LQ
        if ft != 0 and lanes:
            lines.append(f"    {{ uint32_t a = ((uint32_t)(int32_t)(vi[{vis}] + ({im11})) * 16u) & dataMask;")
            if dest == 0xF:
                lines.append(f"      std::memcpy(vf[{ft}], vuData + a, 16u); }}")
            else:
                lines.append(f"      float t[4]; std::memcpy(t, vuData + a, 16u);")
                for c in lanes:
                    lines.append(f"      vf[{ft}][{c}] = t[{c}];")
                lines.append("    }")
        return True
    if top == 0x01:  # SQ
        if lanes:
            lines.append(f"    {{ uint32_t a = ((uint32_t)(int32_t)(vi[{vit}] + ({im11})) * 16u) & dataMask;")
            if dest == 0xF:
                lines.append(f"      std::memcpy(vuData + a, vf[{fs}], 16u); }}")
            else:
                for c in lanes:
                    lines.append(f"      std::memcpy(vuData + a + {c*4}u, &vf[{fs}][{c}], 4u);")
                lines.append("    }")
        return True
    if top == 0x04:  # ILW
        comp = 0 if (dest & 8) else (1 if (dest & 4) else (2 if (dest & 2) else 3))
        if vit != 0:
            lines.append(f"    {{ uint32_t a = ((uint32_t)(int32_t)(vi[{vis}] + ({im11})) * 16u) & dataMask;")
            lines.append(f"      uint32_t v; std::memcpy(&v, vuData + a + {comp*4}u, 4u); vi[{vit}] = (int32_t)(int16_t)(v & 0xFFFFu); }}")
        return True
    if top == 0x05:  # ISW
        lines.append(f"    {{ uint32_t a = ((uint32_t)(int32_t)(vi[{vis}] + ({im11})) * 16u) & dataMask;")
        lines.append(f"      uint32_t v = (uint32_t)(uint16_t)(vi[{vit}] & 0xFFFF);")
        for c in lanes:
            lines.append(f"      std::memcpy(vuData + a + {c*4}u, &v, 4u);")
        lines.append("    }")
        return True
    if top == 0x08:  # IADDIU
        imm15 = ((lo >> 10) & 0x7800) | (lo & 0x7FF)
        if vit != 0:
            lines.append(f"    vi[{vit}] = (int32_t)(int16_t)((vi[{vis}] + {imm15}) & 0xFFFF);")
        return True
    if top == 0x09:  # ISUBIU
        imm15 = ((lo >> 10) & 0x7800) | (lo & 0x7FF)
        if vit != 0:
            lines.append(f"    vi[{vit}] = (int32_t)(int16_t)((vi[{vis}] - {imm15}) & 0xFFFF);")
        return True
    if top == 0x15:  # FSSET
        imm12 = (((lo >> 21) & 1) << 11) | (lo & 0x7FF)
        lines.append(f"    pushFsset(0x{imm12 & 0xFC0:03X}u, cyc + 4u);")
        return True
    if top == 0x16:  # FSAND
        imm12 = (((lo >> 21) & 1) << 11) | (lo & 0x7FF)
        lines.append("    commitMac(cyc);")
        if vit != 0:
            lines.append(f"    vi[{vit}] = (int32_t)((statusReg & 0xFFFu) & 0x{imm12:03X}u);")
        return True
    if top == 0x1A:  # FMAND
        lines.append("    commitMac(cyc);")
        if vit != 0:
            lines.append(f"    vi[{vit}] = (int32_t)(int16_t)(macReg & (uint32_t)(uint16_t)vi[{vis}]);")
        return True
    if top == 0x20:  # B
        tgt = (pc + 8 + im11 * 8) & 0x3FF8
        lines.append(f"    branchPending = true; branchDelay = 1; branchTarget = 0x{tgt:04X}u;")
        return True
    if top == 0x21:  # BAL
        tgt = (pc + 8 + im11 * 8) & 0x3FF8
        ret_addr = ((pc + 16) // 8) & 0xFFFF
        if vit != 0:
            lines.append(f"    vi[{vit}] = {ret_addr};")
        lines.append(f"    branchPending = true; branchDelay = 1; branchTarget = 0x{tgt:04X}u;")
        return True
    if top == 0x24:  # JR
        lines.append(f"    branchPending = true; branchDelay = 1; branchTarget = ((uint32_t)(uint16_t)readBranchVi({vis}) * 8u) & 0x3FF8u;")
        return True
    if top == 0x28:  # IBEQ
        tgt = (pc + 8 + im11 * 8) & 0x3FF8
        lines.append(f"    if ((int16_t)readBranchVi({vis}) == (int16_t)readBranchVi({vit})) {{ branchPending = true; branchDelay = 1; branchTarget = 0x{tgt:04X}u; }}")
        return True
    if top == 0x29:  # IBNE
        tgt = (pc + 8 + im11 * 8) & 0x3FF8
        lines.append(f"    if ((int16_t)readBranchVi({vis}) != (int16_t)readBranchVi({vit})) {{ branchPending = true; branchDelay = 1; branchTarget = 0x{tgt:04X}u; }}")
        return True
    if top == 0x2C:  # IBLTZ
        tgt = (pc + 8 + im11 * 8) & 0x3FF8
        lines.append(f"    if ((int16_t)readBranchVi({vis}) < 0) {{ branchPending = true; branchDelay = 1; branchTarget = 0x{tgt:04X}u; }}")
        return True
    if top == 0x2F:  # IBGEZ
        tgt = (pc + 8 + im11 * 8) & 0x3FF8
        lines.append(f"    if ((int16_t)readBranchVi({vis}) >= 0) {{ branchPending = true; branchDelay = 1; branchTarget = 0x{tgt:04X}u; }}")
        return True
    if top == 0x40:
        lop = lo & 0x3F
        if lop == 0x30:  # IADD
            if vid != 0:
                lines.append(f"    vi[{vid}] = (int32_t)(int16_t)((vi[{vis}] + vi[{vit}]) & 0xFFFF);")
            return True
        if lop == 0x31:  # ISUB
            if vid != 0:
                lines.append(f"    vi[{vid}] = (int32_t)(int16_t)((vi[{vis}] - vi[{vit}]) & 0xFFFF);")
            return True
        if lop == 0x34:  # IAND
            if vid != 0:
                lines.append(f"    vi[{vid}] = vi[{vis}] & vi[{vit}];")
            return True
        if lop == 0x35:  # IOR
            if vid != 0:
                lines.append(f"    vi[{vid}] = vi[{vis}] | vi[{vit}];")
            return True
        if lop >= 0x3C:
            f2 = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2 == 0x30:  # MOVE
                if ft != 0 and lanes:
                    lines.append("    {")
                    for c in lanes:
                        lines.append(f"      float t{c} = vf[{fs}][{c}];")
                    for c in lanes:
                        lines.append(f"      vf[{ft}][{c}] = t{c};")
                    lines.append("    }")
                return True
            if f2 == 0x31:  # MR32
                if ft != 0 and lanes:
                    lines.append(f"    {{ float t[4] = {{vf[{fs}][1], vf[{fs}][2], vf[{fs}][3], vf[{fs}][0]}};\n")
                    for c in lanes:
                        lines.append(f"      vf[{ft}][{c}] = t[{c}];")
                    lines.append("    }")
                return True
            if f2 == 0x34:  # LQI
                lines.append(f"    {{ uint32_t a = ((uint32_t)(uint16_t)vi[{vis}] * 16u) & dataMask;")
                if ft != 0 and lanes:
                    if dest == 0xF:
                        lines.append(f"      std::memcpy(vf[{ft}], vuData + a, 16u);")
                    else:
                        lines.append(f"      float t[4]; std::memcpy(t, vuData + a, 16u);")
                        for c in lanes:
                            lines.append(f"      vf[{ft}][{c}] = t[{c}];")
                if vis != 0:
                    lines.append(f"      vi[{vis}] = (int32_t)(int16_t)(vi[{vis}] + 1);")
                lines.append("    }")
                return True
            if f2 == 0x35:  # SQI
                lines.append(f"    {{ uint32_t a = ((uint32_t)(uint16_t)vi[{vit}] * 16u) & dataMask;")
                if lanes:
                    if dest == 0xF:
                        lines.append(f"      std::memcpy(vuData + a, vf[{fs}], 16u);")
                    else:
                        for c in lanes:
                            lines.append(f"      std::memcpy(vuData + a + {c*4}u, &vf[{fs}][{c}], 4u);")
                if vit != 0:
                    lines.append(f"      vi[{vit}] = (int32_t)(int16_t)(vi[{vit}] + 1);")
                lines.append("    }")
                return True
            if f2 == 0x38:  # DIV
                fsf = (lo >> 21) & 3
                ftf = (lo >> 23) & 3
                lines.append("    if (qPending) { if (cyc < qReady) cyc = qReady; q = qVal; qPending = false; }")
                lines.append(f"    {{ float num = normFast(vf[{fs}][{fsf}]), den = normFast(vf[{ft}][{ftf}]);")
                lines.append("      qVal = (den == 0.0f) ? ((std::signbit(num) != std::signbit(den)) ? -3.402823466e+38f : 3.402823466e+38f) : normFast(num / den);")
                lines.append("      qReady = cyc + 7u; qPending = true; }")
                return True
            if f2 == 0x3B:  # WAITQ
                lines.append("    if (qPending) { if (cyc < qReady) cyc = qReady; q = qVal; qPending = false; }")
                return True
            if f2 == 0x3D:  # MFIR
                if ft != 0 and lanes:
                    lines.append(f"    {{ int32_t v = (int32_t)(int16_t)(vi[{vis}] & 0xFFFF); float f; std::memcpy(&f, &v, 4);")
                    for c in lanes:
                        lines.append(f"      vf[{ft}][{c}] = f;")
                    lines.append("    }")
                return True
            if f2 == 0x64:  # MFP
                lines.append("    if (pPending && cyc >= pReady) { p = pVal; pPending = false; }")
                if ft != 0 and lanes:
                    for c in lanes:
                        lines.append(f"    vf[{ft}][{c}] = p;")
                return True
            if f2 == 0x68:  # XTOP
                if vit != 0:
                    lines.append(f"    vi[{vit}] = (int32_t)(st.top & 0x3FFu);")
                return True
            if f2 == 0x69:  # XITOP
                if vit != 0:
                    lines.append(f"    vi[{vit}] = (int32_t)(st.itop & 0x3FFu);")
                return True
            if f2 == 0x6C:  # XGKICK
                lines.append(f"    vu1Xgkick((uint32_t)(uint16_t)vi[{vis}], vuData, dataSize, gs, memory, outGifHash, outGifBytes, replaySink);")
                return True
            if f2 == 0x79:  # ESIN
                comp = (lo >> 21) & 3
                lines.append("    if (pPending) { if (cyc < pReady) cyc = pReady; p = pVal; pPending = false; }")
                lines.append(f"    pVal = vuEsin(normFast(vf[{fs}][{comp}])); pReady = cyc + 29u; pPending = true;")
                return True
            if f2 == 0x7B:  # WAITP
                lines.append("    if (pPending) { if (cyc < pReady) cyc = pReady; p = pVal; pPending = false; }")
                return True
    return False

def gen_program(name: str, h64: int, data: bytes) -> str:
    pairs = []
    for i in range(0, len(data), 8):
        lo, hi = struct.unpack_from("<II", data, i)
        pairs.append((lo, hi))

    # Pre-check which pairs have non-zero instructions
    valid_pcs = [i * 8 for i, (lo, hi) in enumerate(pairs) if (lo != 0 or hi != 0)]
    valid_pc_set = set(valid_pcs)
    branch_targets = set()
    entry_labels = {0}
    has_branch = [False] * len(pairs)
    has_ebit = [False] * len(pairs)
    has_jr = False
    jr_extra_targets = set()
    for i, (lo, hi) in enumerate(pairs):
        if lo == 0 and hi == 0:
            if i + 1 < len(pairs) and (pairs[i + 1][0] or pairs[i + 1][1]):
                entry_labels.add((i + 1) * 8)
            continue
        if hi & 0x40000000:
            has_ebit[i] = True
            if i + 2 < len(pairs) and (pairs[i + 2][0] or pairs[i + 2][1]):
                entry_labels.add((i + 2) * 8)
        if (hi & 0x80000000) == 0:
            top = (lo >> 25) & 0x7F
            if top in (0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
                has_branch[i] = True
                tgt = (i * 8 + 8 + imm11(lo) * 8) & 0x3FF8
                if tgt in valid_pc_set:
                    branch_targets.add(tgt)
                if i + 2 < len(pairs) and (pairs[i + 2][0] or pairs[i + 2][1]):
                    jr_extra_targets.add((i + 2) * 8)
            elif top in (0x24, 0x25):
                has_branch[i] = True
                has_jr = True
                if i + 2 < len(pairs) and (pairs[i + 2][0] or pairs[i + 2][1]):
                    jr_extra_targets.add((i + 2) * 8)
            elif top == 0x08:  # IADDIU viT, vi0, imm15 -> potential jump target address
                vis = (lo >> 11) & 0xF
                if vis == 0:
                    imm15 = ((lo >> 10) & 0x7800) | (lo & 0x7FF)
                    tgt = (imm15 * 8) & 0x3FF8
                    if tgt in valid_pc_set:
                        jr_extra_targets.add(tgt)
    if has_jr:
        branch_targets |= (entry_labels | jr_extra_targets) & valid_pc_set
    all_labels = (entry_labels | branch_targets) & valid_pc_set
    all_labels_sorted = sorted(all_labels)
    branch_targets_sorted = sorted(branch_targets)

    # Compute per-pair need_mac window (only emit normMac/pushMac within 10 pairs of a flag reader,
    # or across branches targeting a flag-reading window) so hot vertex loops stay branchless & fast.
    pair_needs_mac = [False] * len(pairs)
    flag_readers = []
    for i, (lo, hi) in enumerate(pairs):
        if (hi & 0x80000000) == 0 and ((lo >> 25) & 0x7F) in (0x14, 0x15, 0x16, 0x17, 0x18, 0x1A, 0x1B):
            flag_readers.append(i)
            for k in range(max(0, i - 10), i + 1):
                pair_needs_mac[k] = True
    if flag_readers:
        for i, (lo, hi) in enumerate(pairs):
            if has_branch[i]:
                top = (lo >> 25) & 0x7F
                if top in (0x24, 0x25):
                    for k in range(max(0, i - 6), min(len(pairs), i + 2)):
                        pair_needs_mac[k] = True
                else:
                    tgt_idx = ((i * 8 + 8 + imm11(lo) * 8) & 0x3FF8) // 8
                    if tgt_idx < len(pairs) and any(pair_needs_mac[t] for t in range(tgt_idx, min(len(pairs), tgt_idx + 6))):
                        for k in range(max(0, i - 6), min(len(pairs), i + 2)):
                            pair_needs_mac[k] = True
    has_fmand = bool(flag_readers)

    out = []
    out.append(f"__attribute__((optimize(\"O1\"))) static bool run_{name}(VU1State &st, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink) {{")
    out.append("  uint32_t pc = st.pc & 0x3FF8u;")
    out.append("  switch (pc) {")
    for pc in all_labels_sorted:
        out.append(f"  case 0x{pc:04X}u: break;")
    out.append("  default: return false;")
    out.append("  }")
    out.append("  const uint32_t dataMask = dataSize - 1u;")
    out.append("  float vf[32][4];")
    out.append("  std::memcpy(vf, st.vf, sizeof(vf));")
    out.append("  float acc[4] = {st.acc[0], st.acc[1], st.acc[2], st.acc[3]};")
    out.append("  int32_t vi[16];")
    out.append("  std::memcpy(vi, st.vi, sizeof(vi));")
    out.append("  float q = st.q, p = st.p, ireg = st.i;")
    out.append("  uint32_t macReg = st.mac;")
    out.append("  uint32_t statusReg = st.status;")
    out.append("  uint32_t cyc = 0u;")
    out.append("  uint32_t vfReady[32][4]{};")
    out.append("  bool qPending = false; float qVal = 0.0f; uint32_t qReady = 0u;")
    out.append("  bool pPending = false; float pVal = 0.0f; uint32_t pReady = 0u;")
    out.append("  struct MacSlot { uint32_t mac; uint32_t status; uint32_t ready; uint32_t issue; bool valid; bool isFsset; bool writesStatus; };")
    out.append("  MacSlot macPipe[8]{}; uint32_t macHead = 0u;")
    out.append("  auto commitMac = [&](uint32_t c) noexcept {")
    out.append("    for (uint32_t k = 0; k < 8u; ++k) {")
    out.append("      uint32_t idx = (macHead + k) & 7u;")
    out.append("      if (macPipe[idx].valid && macPipe[idx].ready <= c) {")
    out.append("        if (macPipe[idx].isFsset) { statusReg = (statusReg & 0x3Fu) | macPipe[idx].status; }")
    out.append("        else { macReg = macPipe[idx].mac; if (macPipe[idx].writesStatus) { uint32_t s = ((macReg & 0xFu) ? 1u : 0u) | ((macReg & 0xF0u) ? 2u : 0u) | ((macReg & 0xF00u) ? 4u : 0u) | ((macReg & 0xF000u) ? 8u : 0u); statusReg = (statusReg & 0xFC0u) | s | (s << 6); } }")
    out.append("        macPipe[idx].valid = false;")
    out.append("      }")
    out.append("    }")
    out.append("  };")
    out.append("  auto pushMac = [&](uint32_t m, uint32_t r) noexcept {")
    out.append("    macPipe[macHead & 7u] = {m, 0u, r, cyc, true, false, true}; ++macHead;")
    out.append("  };")
    out.append("  auto pushFsset = [&](uint32_t s, uint32_t r) noexcept {")
    out.append("    for (uint32_t k = 0; k < 8u; ++k) { if (macPipe[k].valid && macPipe[k].issue == cyc) macPipe[k].writesStatus = false; }")
    out.append("    macPipe[macHead & 7u] = {0u, s, r, cyc, true, true, false}; ++macHead;")
    out.append("  };")
    out.append("  bool branchPending = false; int branchDelay = 0; uint32_t branchTarget = 0u;")
    out.append("  bool ebit = false;")
    out.append("  int viBakReg = 0; int32_t viBakVal = 0;")
    out.append("  auto readBranchVi = [&](int r) noexcept -> int32_t { return (r != 0 && r == viBakReg) ? viBakVal : vi[r]; };")
    out.append("  switch (pc) {")
    for pc in all_labels_sorted:
        out.append(f"  case 0x{pc:04X}u: goto L_{pc:04X};")
    out.append("  default: goto done;")
    out.append("  }")
    out.append("branch_dispatch:")
    out.append("  switch (pc) {")
    for pc in branch_targets_sorted:
        out.append(f"  case 0x{pc:04X}u: goto L_{pc:04X};")
    out.append("  default: goto done;")
    out.append("  }")

    for idx in range(len(pairs)):
        pc = idx * 8
        lo, hi = pairs[idx]
        if lo == 0 and hi == 0:
            continue
        i_bit = (hi & 0x80000000) != 0
        e_bit = (hi & 0x40000000) != 0
        if pc in all_labels:
            out.append(f"L_{pc:04X}:")
        out.append("  {")

        # Per-lane stall tracking for accurate 7-cycle DIV and 4-cycle FMAND
        r_lanes = upper_read_lanes(hi)
        l_lanes, l_write, l_wmask = lower_vf_lanes(lo, i_bit)
        all_reads = dict(r_lanes)
        for r, m in l_lanes.items():
            all_reads[r] = all_reads.get(r, 0) | m
        if all_reads:
            checks = []
            for r in sorted(all_reads):
                for c in lanes_from_dest(all_reads[r]):
                    checks.append(f"if (vfReady[{r}][{c}] > cyc) {{ cyc = vfReady[{r}][{c}]; viBakReg = 0; }}")
            if checks:
                out.append("    " + " ".join(checks))

        # Pipeline stalls for lower FDIV (WAITQ/DIV/SQRT/RSQRT) and EFU (WAITP/E*) happen BEFORE upper executes!
        if not i_bit and ((lo >> 25) & 0x7F) == 0x40 and (lo & 0x3F) >= 0x3C:
            f2_pre = (lo & 3) | ((lo >> 4) & 0x7C)
            if f2_pre in (0x38, 0x39, 0x3A, 0x3B):
                out.append("    if (qPending) { if (cyc < qReady) { cyc = qReady; viBakReg = 0; } q = qVal; qPending = false; }")
            elif f2_pre == 0x7B or (0x70 <= f2_pre <= 0x7D):
                out.append("    if (pPending) { if (cyc < pReady) { cyc = pReady; viBakReg = 0; } p = pVal; pPending = false; }")

        u_write = upper_write_reg(hi)
        u_wmask = dest_mask(hi) if u_write != 0 else 0
        shadow_reg = u_write if (u_write != 0 and (u_write in l_lanes or u_write == l_write)) else 0
        vi_w = lower_vi_write(lo, i_bit)

        if vi_w != 0:
            out.append(f"    int32_t oldVi = vi[{vi_w}];")

        need_mac_here = pair_needs_mac[idx]
        if i_bit:
            if not emit_upper(hi, out, need_mac_here):
                raise RuntimeError(f"Unsupported upper op 0x{hi:08X} at pc=0x{pc:04X}")
            fval = struct.unpack("<f", struct.pack("<I", lo))[0]
            out.append(f"    {{ uint32_t ib = 0x{lo:08X}u; std::memcpy(&ireg, &ib, 4); }}")
        elif shadow_reg != 0:
            out.append(f"    float oldVf[4], upVf[4]; std::memcpy(oldVf, vf[{shadow_reg}], 16);")
            if not emit_upper(hi, out, need_mac_here):
                raise RuntimeError(f"Unsupported upper op 0x{hi:08X} at pc=0x{pc:04X}")
            out.append(f"    std::memcpy(upVf, vf[{shadow_reg}], 16); std::memcpy(vf[{shadow_reg}], oldVf, 16);")
            if not emit_lower(pc, lo, out):
                raise RuntimeError(f"Unsupported lower op 0x{lo:08X} at pc=0x{pc:04X}")
            for c in lanes_from_dest(u_wmask):
                out.append(f"    vf[{shadow_reg}][{c}] = upVf[{c}];")
        else:
            if not emit_upper(hi, out, need_mac_here):
                raise RuntimeError(f"Unsupported upper op 0x{hi:08X} at pc=0x{pc:04X}")
            if not emit_lower(pc, lo, out):
                raise RuntimeError(f"Unsupported lower op 0x{lo:08X} at pc=0x{pc:04X}")

        if vi_w != 0:
            out.append(f"    viBakReg = {vi_w}; viBakVal = oldVi;")
        else:
            out.append("    viBakReg = 0;")

        # Update per-lane vfReady for FMAC write (latency 4) and lower load write (latency 1)
        if u_write != 0 and u_wmask != 0:
            ups = " ".join(f"vfReady[{u_write}][{c}] = cyc + 4u;" for c in lanes_from_dest(u_wmask))
            out.append(f"    {ups}")
        if l_write != 0 and l_wmask != 0 and l_write != u_write:
            lps = " ".join(f"vfReady[{l_write}][{c}] = cyc + 1u;" for c in lanes_from_dest(l_wmask))
            out.append(f"    {lps}")

        out.append("    ++cyc;")
        next_pc = (pc + 8) & 0x3FF8
        may_be_ebit_delay = (idx > 0 and has_ebit[idx - 1])
        may_be_branch_delay = (idx > 0 and has_branch[idx - 1]) or has_branch[idx]
        if may_be_ebit_delay:
            out.append("    bool wasEbit = ebit;")
            if e_bit:
                out.append("    ebit = true;")
            out.append(f"    if (wasEbit) {{ pc = branchPending && branchDelay == 0 ? branchTarget : 0x{next_pc:04X}u; goto done; }}")
        elif e_bit:
            out.append("    ebit = true;")
        if may_be_branch_delay:
            out.append("    if (branchPending) { if (branchDelay > 0) { --branchDelay; } else { pc = branchTarget; branchPending = false; goto branch_dispatch; } }")
        out.append("  }")

    out.append("done:")
    out.append("  if (qPending) { q = qVal; }")
    out.append("  if (pPending) { p = pVal; }")
    out.append("  commitMac(0xFFFFFFFFu);")
    out.append("  vf[0][0] = 0.0f; vf[0][1] = 0.0f; vf[0][2] = 0.0f; vf[0][3] = 1.0f;")
    out.append("  vi[0] = 0;")
    out.append("  std::memcpy(st.vf, vf, sizeof(vf));")
    out.append("  std::memcpy(st.acc, acc, sizeof(acc));")
    out.append("  std::memcpy(st.vi, vi, sizeof(vi));")
    out.append("  st.q = q; st.p = p; st.i = ireg; st.mac = macReg; st.status = statusReg;")
    out.append("  st.pc = pc; st.ebit = false; st.branchPending = false;")
    out.append("  st.cycles += cyc;")
    out.append("  return true;")
    out.append("}\n")
    return "\n".join(out)

def main():
    inputs = sys.argv[1:-1]
    out_path = sys.argv[-1]
    progs = []
    seen_prefix = set()
    for i, path in enumerate(inputs):
        b = open(path, "rb").read()
        h = hash64(b)
        ph = hash64(b[:min(len(b), 0x200)])
        if ph in seen_prefix:
            continue
        seen_prefix.add(ph)
        progs.append((f"prog_{len(progs)}_{h:016x}", h, ph, b))

    header = """// Gerado por tools/gen_vu1_native.py - Executor nativo VU1 para microprogramas quentes.
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using Ps2Vu1NativeFn = bool (*)(uint64_t codeHash, VU1State &st, uint8_t *vuData, uint32_t dataSize,
                                GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink);
extern void ps2_vu1_register_native_executor(Ps2Vu1NativeFn fn);

namespace {

inline float normFast(float value) noexcept {
    // MXCSR FTZ+DAZ (0x8040) is enabled by VU1Interpreter::run() on x86-64.
    return value;
}

inline float normMac(float value, uint32_t laneBit, uint32_t &mac) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    const uint32_t exp = (bits >> 23) & 0xFFu;
    if (bits & 0x80000000u) mac |= (laneBit << 4);
    if (exp == 0u) {
        if (bits & 0x007FFFFFu) mac |= (laneBit << 8);
        mac |= laneBit;
        bits &= 0x80000000u;
        std::memcpy(&value, &bits, 4);
    } else if (exp == 0xFFu) {
        mac |= (laneBit << 12);
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, 4);
    }
    return value;
}

inline float vuMinMax(float a, float b, bool maximum) noexcept {
    int32_t left, right;
    std::memcpy(&left, &a, 4);
    std::memcpy(&right, &b, 4);
    const bool reverse = left < 0 && right < 0;
    const bool chooseLeft = (maximum != reverse) ? left > right : left < right;
    const int32_t bits = chooseLeft ? left : right;
    float res;
    std::memcpy(&res, &bits, 4);
    return res;
}

inline int32_t vuFloatToInt(float value, float scale) noexcept {
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(scaled);
}

inline float vuEsin(float value) noexcept {
    constexpr float c[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
    const float sq = value * value;
    float poly = c[4];
    for (int i = 3; i >= 0; --i) poly = c[i] + sq * poly;
    return value * poly;
}

static void vu1Xgkick(uint32_t qwordAddr, uint8_t *vuData, uint32_t dataSize,
                      GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink) {
    if (!vuData || dataSize == 0u) return;
    static thread_local uint8_t pkt[65536];
    const uint32_t mask = dataSize - 1u;
    uint32_t src = (qwordAddr * 16u) & mask;
    uint32_t copied = 0u;
    while (copied + 16u <= sizeof(pkt)) {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, vuData + src, 8u);
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const bool eop = ((tagLo >> 15) & 1u) != 0u;
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u) nreg = 16u;
        uint64_t tagBytes = 16u;
        if (flg == 0u) tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
        else if (flg == 1u) tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1ull) & ~1ull) * 8u;
        else tagBytes += static_cast<uint64_t>(nloop) * 16u;
        if (copied + tagBytes > sizeof(pkt)) break;
        uint32_t rem = static_cast<uint32_t>(tagBytes);
        while (rem > 0u) {
            const uint32_t chunk = std::min<uint32_t>(rem, dataSize - src);
            std::memcpy(pkt + copied, vuData + src, chunk);
            copied += chunk;
            src = (src + chunk) & mask;
            rem -= chunk;
        }
        if (eop) break;
    }
    if (copied == 0u) return;
    if (outGifHash && outGifBytes) {
        uint64_t h = *outGifHash;
        for (uint32_t i = 0; i < copied; ++i) h = (h ^ pkt[i]) * 1099511628211ull;
        *outGifHash = h;
        *outGifBytes += copied;
    }
    if (replaySink) return;
    if (memory) memory->submitGifPacket(GifPathId::Path1, pkt, copied);
    else gs.processGIFPacket(pkt, copied);
}

"""
    bodies = [gen_program(name, h, b) for name, h, ph, b in progs]
    dispatch = ["static bool edoVu1NativeExecute(uint64_t codeHash, VU1State &st, uint8_t *vuData, uint32_t dataSize,",
                "                                GS &gs, PS2Memory *memory, uint64_t *outGifHash, uint64_t *outGifBytes, bool replaySink) {",
                "  switch (codeHash) {"]
    for name, h, ph, _ in progs:
        dispatch.append(f"  case 0x{h:016X}ull:")
        if ph != h:
            dispatch.append(f"  case 0x{ph:016X}ull:")
        dispatch.append(f"    return run_{name}(st, vuData, dataSize, gs, memory, outGifHash, outGifBytes, replaySink);")
    dispatch.append("  default: return false;")
    dispatch.append("  }")
    dispatch.append("}")
    dispatch.append("")
    dispatch.append("struct EdoVu1NativeAutoRegister {")
    dispatch.append("  EdoVu1NativeAutoRegister() {")
    dispatch.append("    const char *dis = std::getenv(\"EDO_DISABLE_VU1_NATIVE\");")
    dispatch.append("    if (!dis || !*dis || *dis == '0') ps2_vu1_register_native_executor(&edoVu1NativeExecute);")
    dispatch.append("  }")
    dispatch.append("} g_edoVu1NativeAutoRegister;")
    dispatch.append("} // namespace")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write(header)
        for body in bodies:
            f.write(body)
        f.write("\n".join(dispatch) + "\n")
    print(f"Generated {out_path} with {len(progs)} native VU1 microprograms.")

if __name__ == "__main__":
    main()
