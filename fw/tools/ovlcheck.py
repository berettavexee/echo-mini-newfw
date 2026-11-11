#!/usr/bin/env python3
"""The rules of the code overlays (src/ovl.h, ETUDE §137), checked on the linked ELF.

  ovlcheck.py build/fw.elf build/ovl.o

The ELF is linked with --emit-relocs: every reference the code and data make is a relocation
kept in it. The overlays are the sections .ovl_*; a region shared with data (the USB overlay over
the directory listing, .dirbuf) counts as another overlay of the same region.
  1. Resident code and data reach an overlay only from a gate: a function of ovl.c (its global
     functions, read from ovl.o). The overlays' region bounds (symbols __ovl_*) are not
     references into them.
  2. An overlay reaches nothing else that lives at its addresses: another overlay, or the data
     it replaces (.dirbuf for the USB overlay).
  3. No data holds an overlay's address: the vector table, the tables of constants (rule 1
     catches them: they are not in a gate).
  4. An overlay reaches no fixed address (a linker-defined symbol) inside its own code: a
     buffer there would overwrite it (0.103o1: msc.c's transfer buffer at 0x03078000, the USB
     overlay's address; the buffers' addresses are symbols of link.ld since, see shared.h).
Exit code 1 and the faults listed when one is broken.
"""
import struct
import sys

SHN_UNDEF, SHN_LORESERVE, SHN_ABS = 0, 0xff00, 0xfff1
STT_FUNC = 2


def load(path):
    e = open(path, "rb").read()
    if e[:4] != b"\x7fELF" or e[4] != 1 or e[5] != 1:
        sys.exit(f"{path}: not an ELF32 little-endian file")
    shoff, = struct.unpack_from("<I", e, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", e, 0x2e)
    raw = [struct.unpack_from("<10I", e, shoff + i * shentsize) for i in range(shnum)]
    strs = raw[shstrndx]
    secs = []
    for s in raw:
        name = e[strs[4] + s[0]:].split(b"\0")[0].decode()
        secs.append(dict(name=name, type=s[1], flags=s[2], addr=s[3], off=s[4], size=s[5],
                         link=s[6], info=s[7]))
    syms = []
    for s in secs:
        if s["type"] == 2:                      # SHT_SYMTAB
            strtab = secs[s["link"]]
            for i in range(s["size"] // 16):
                nm, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", e,
                                                                     s["off"] + 16 * i)
                name = e[strtab["off"] + nm:].split(b"\0")[0].decode()
                syms.append(dict(name=name, value=value, size=size, type=info & 15,
                                 bind=info >> 4, shndx=shndx))
    return e, secs, syms


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    e, secs, syms = load(sys.argv[1])
    _, osecs, osyms = load(sys.argv[2])
    gates = {s["name"] for s in osyms if s["type"] == STT_FUNC and s["bind"] == 1 and
             s["shndx"] != SHN_UNDEF}
    alloc = lambda i: bool(secs[i]["flags"] & 2)                  # noqa: E731
    ovl = {i for i, s in enumerate(secs) if s["name"].startswith(".ovl_")}
    if not ovl:
        sys.exit("ovlcheck: no overlay in the ELF")

    def span(i):
        return secs[i]["addr"], secs[i]["addr"] + secs[i]["size"]

    def overlaps(i, j):
        a, b = span(i), span(j)
        return i != j and a[0] < b[1] and b[0] < a[1]

    # the functions of .text, to tell which one a reference is made from
    text = next(i for i, s in enumerate(secs) if s["name"] == ".text")
    funcs = sorted((s["value"] & ~1, s["size"], s["name"]) for s in syms
                   if s["type"] == STT_FUNC and s["shndx"] == text)

    def func_at(a):
        lo, hi = 0, len(funcs)
        while lo < hi:
            mid = (lo + hi) // 2
            if funcs[mid][0] <= a:
                lo = mid + 1
            else:
                hi = mid
        f = funcs[lo - 1] if lo else None
        return f[2] if f and f[0] <= a < f[0] + max(f[1], 1) else None

    faults, n = [], 0
    for rs in secs:
        if rs["type"] != 9 or not alloc(rs["info"]):    # SHT_REL of a loaded section
            continue
        src = rs["info"]
        for k in range(rs["size"] // 8):
            where, info = struct.unpack_from("<II", e, rs["off"] + 8 * k)
            sym = syms[info >> 8] if info >> 8 else None
            if sym and sym["shndx"] == SHN_ABS and src in ovl and info & 0xff:
                n += 1
                a, b = span(src)
                if a <= sym["value"] < b and not sym["name"].startswith("__ovl_"):
                    faults.append(f"rule 4: {secs[src]['name']} at 0x{where:08x} reaches "
                                  f"{sym['name']} = 0x{sym['value']:08x}, inside its own code")
                continue
            if not sym or info & 0xff == 0 or not 0 < sym["shndx"] < SHN_LORESERVE:
                continue
            n += 1
            dst = sym["shndx"]
            name = sym["name"] or secs[dst]["name"]
            if sym["name"].startswith("__ovl_"):       # a region's bounds
                continue
            if src in ovl:
                if overlaps(src, dst):
                    faults.append(f"rule 2: {secs[src]['name']} at 0x{where:08x} reaches {name} "
                                  f"({secs[dst]['name']}, at the same addresses)")
            elif dst in ovl:
                f = func_at(where) if src == text else None
                if f not in gates:
                    faults.append(f"rule 1: {f or secs[src]['name']} at 0x{where:08x} reaches "
                                  f"{name} in {secs[dst]['name']} (not a gate of ovl.c)")
    for i in sorted(ovl):
        others = ", ".join(secs[j]["name"] for j in range(len(secs)) if alloc(j) and
                           overlaps(i, j))
        print(f"ovlcheck: {secs[i]['name']}: {secs[i]['size']} bytes at 0x{secs[i]['addr']:08x}, "
              f"over {others or 'nothing'}")
    print(f"ovlcheck: {n} references checked, gates: {', '.join(sorted(gates))}")
    for f in faults[:40]:
        print("FAIL " + f)
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main())
