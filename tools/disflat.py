#!/usr/bin/env python3
"""Disassemble a flat Thumb blob with pc-relative literals and strings resolved.

  disflat.py BLOB BASE [START END]     (hex addresses)
"""
import re, struct, sys
import capstone

b = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16)
s = int(sys.argv[3], 16) if len(sys.argv) > 3 else base
t = int(sys.argv[4], 16) if len(sys.argv) > 4 else base + len(b)
strs = {base + m.start(): m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", b)}
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
md.skipdata = True
for x in md.disasm(b[s - base:t - base], s):
    extra = ""
    try:
        if x.mnemonic.startswith("ldr") and "[pc, #" in x.op_str:
            a = ((x.address + 4) & ~3) + int(x.op_str.split("#")[-1].rstrip("]"), 0)
            v = struct.unpack_from("<I", b, a - base)[0]
            extra = f"  ; ={v:#010x}" + (f" {strs[v]!r}" if v in strs else "")
        elif x.mnemonic.startswith("adr"):
            v = ((x.address + 4) & ~3) + int(x.op_str.split("#")[-1], 0)
            extra = f"  ; ={v:#010x}" + (f" {strs[v]!r}" if v in strs else "")
    except (ValueError, struct.error):
        pass
    print(f"{x.address:08x}  {x.mnemonic:8s} {x.op_str}{extra}")
