#!/usr/bin/env python3
"""String and literal cross-references in any RKnano image (stock or RECHO).

  xref.py FW.IMG str REGEX        strings matching REGEX and the code sites that load them
  xref.py FW.IMG lit VALUE        code sites loading VALUE (ldr pc-relative or adr)
  xref.py FW.IMG dis OVL START END   disassembly (exec addresses), literals resolved
"""
import re, struct, sys
import capstone
sys.path.insert(0, __file__.rsplit("/", 3)[0])
from rknanofw import overlays                                       # noqa: E402

d = open(sys.argv[1], "rb").read()
c, B, E = overlays(d)
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
md.skipdata = True


def blob(i):
    cl, ce, cs = E[i][:3]
    return c[cl - B:cl - B + cs], ce


def lit(x, code, ce):
    """Value loaded by an ldr-literal or adr instruction, else None."""
    try:
        if x.mnemonic.startswith("ldr") and "[pc, #" in x.op_str:
            a = ((x.address + 4) & ~3) + int(x.op_str.split("#")[-1].rstrip("]"), 0)
            return struct.unpack_from("<I", code, a - ce)[0] if ce <= a < ce + len(code) - 3 else None
        if x.mnemonic.startswith("adr"):
            return ((x.address + 4) & ~3) + int(x.op_str.split("#")[-1], 0)
    except (ValueError, struct.error):
        return None
    return None


def sites(values):
    out = []
    for i, e in enumerate(E):
        if not e[2] or e[1] < 0x03000000:
            continue
        code, ce = blob(i)
        fn = ce
        for x in md.disasm(code, ce):
            if x.mnemonic.startswith("push") and "lr" in x.op_str:
                fn = x.address
            v = lit(x, code, ce)
            if v in values:
                out.append((v, i, x.address, fn))
    return out


def strings_exec():
    """(exec address, text) of every printable string in code and data of every overlay."""
    res = []
    for i, (cl, ce, cs, dl, de, ds, *_ ) in enumerate(E):
        for ld, ex, ln in ((cl, ce, cs), (dl, de, ds)):
            if not ln or ex < 0x03000000:
                continue
            b = c[ld - B:ld - B + ln]
            for m in re.finditer(rb"[\x20-\x7e]{5,}", b):
                res.append((ex + m.start(), m.group().decode()))
    return res


mode = sys.argv[2]
if mode == "str":
    rx = re.compile(sys.argv[3])
    hits = {a: s for a, s in strings_exec() if rx.search(s)}
    refs = sites(set(hits))
    for a, s in sorted(hits.items()):
        where = [f"ovl{o}:{x:#x} (fn {f:#x})" for v, o, x, f in refs if v == a]
        print(f"{a:#010x} {s!r}\n      " + (", ".join(where) or "no direct reference"))
elif mode == "lit":
    v = int(sys.argv[3], 0)
    for _, o, x, f in sites({v, v | 1}):
        print(f"ovl{o}:{x:#x} (fn {f:#x})")
elif mode == "dis":
    o, s, t = int(sys.argv[3]), int(sys.argv[4], 16), int(sys.argv[5], 16)
    code, ce = blob(o)
    strs = dict(strings_exec())
    for x in md.disasm(code[s - ce:t - ce], s):
        v = lit(x, code, ce)
        extra = ""
        if v is not None:
            extra = f"  ; ={v:#010x}" + (f" {strs[v]!r}" if v in strs else "")
        print(f"{x.address:08x}  {x.mnemonic:8s} {x.op_str}{extra}")
