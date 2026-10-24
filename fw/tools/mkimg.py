#!/usr/bin/env python3
"""Build an RKnanoFW image (HIFIECnn.IMG) from the firmware ELF.

  mkimg.py build/fw.elf OUT.IMG [--date YYYY-MM-DD] [--pack codecs.bin]

Layout (format in re/REPORT.md section 2, loader in re/newfw/ETUDE.md section 2.2):
  0x000      header (512 bytes): BCD date, "RKnanoFW" at 0x1f8, fw_end at 0x1f4, code part at 0x6c
  0x200      code part: overlay table {base, n, n x 8 words}, then at +0x200 the code
  fw_end     copy of the header (the loader compares it with the header)
  size       Rockchip CRC32 of everything before, size = (fw_end + 0x104000) rounded down to
             1 MiB, as the stock and RECHO images (RECHO's installer checks it).
Overlay entry 0: code at 0x03050000 (text, then the load copy of .data), data at 0x03004000
(same bytes, starting with the vector table), BSS. Entries 1 and 2, also loaded by the IDB
loader, are empty. The codec pack (tools/mkpack.py) follows the code part, 512-aligned, before
fw_end; header +0x148 = {1, offset, length} (the stock image-pack slot). The font blob
(tools/mkfusion.py) follows it, 512-aligned; header +0x80 = {offset, length} (the stock 16 px
font slot, count at +0x74 = 2 as in the stock header, +0x78 = the empty 12 px slot).
"""
import datetime
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rkcrc import rkcrc                                            # noqa: E402

BASE = 0x03050000
CODE_IN_PART = 0x200


def elf(path):
    e = open(path, "rb").read()
    if e[:4] != b"\x7fELF" or e[4] != 1 or e[5] != 1:
        sys.exit("not an ELF32 little-endian file")
    phoff, shoff = struct.unpack_from("<II", e, 0x1c)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", e, 0x2a)
    segs = [struct.unpack_from("<8I", e, phoff + i * phentsize) for i in range(phnum)]
    sh = [struct.unpack_from("<10I", e, shoff + i * shentsize) for i in range(shnum)]
    names = sh[shstrndx]
    secs = {}
    for s in sh:
        nm = e[names[4] + s[0]:].split(b"\0")[0].decode()
        secs[nm] = s
    return e, segs, secs


def main():
    src, dst = sys.argv[1], sys.argv[2]
    a = sys.argv
    date = datetime.date.fromisoformat(a[a.index("--date") + 1]) if "--date" in a else datetime.date.today()
    e, segs, secs = elf(src)

    # code region image: every PT_LOAD placed at its load (physical) address inside CODE
    loads = [s for s in segs if s[0] == 1 and s[4]]
    lo = min(s[3] for s in loads)
    hi = max(s[3] + s[4] for s in loads)
    if lo != BASE or hi > 0x03080000:
        sys.exit(f"load addresses 0x{lo:08x}-0x{hi:08x} outside 0x03050000-0x03080000")
    code = bytearray(hi - lo)
    for _, off, vaddr, paddr, filesz, *_ in loads:
        code[paddr - lo:paddr - lo + filesz] = e[off:off + filesz]
    code += bytes(-len(code) % 4)

    data, bss = secs[".data"], secs[".bss"]
    d_vma, d_size = data[3], data[5]
    d_lma = next(s[3] for s in loads if s[2] == d_vma)
    b_vma, b_size = bss[3], bss[5]
    if d_vma != 0x03004000:
        sys.exit(f".data at 0x{d_vma:08x}: the vector table must be at 0x03004000")
    if b_vma + b_size > 0x0304a000:
        sys.exit(f"data and BSS end at 0x{b_vma + b_size:08x}: the IDB loader runs from "
                 "0x0304a000 while it clears our BSS (ETUDE §23.2)")
    vec = struct.unpack_from("<2I", code, d_lma - lo)
    print(f"  vectors: SP 0x{vec[0]:08x} reset 0x{vec[1]:08x}")
    if not BASE <= vec[1] & ~1 < hi or not vec[1] & 1:
        sys.exit("reset vector is not a Thumb address in the code")

    code_load = BASE + CODE_IN_PART
    ent0 = (code_load, BASE, len(code), code_load + d_lma - lo, d_vma, d_size, b_vma, b_size)
    table = struct.pack("<2I", BASE, 3) + struct.pack("<8I", *ent0) + bytes(64)
    part = table + bytes(CODE_IN_PART - len(table)) + code

    pack = open(a[a.index("--pack") + 1], "rb").read() if "--pack" in a else b""
    pack_off = (0x200 + len(part) + 511) & ~511
    font = open(a[a.index("--font") + 1], "rb").read() if "--font" in a else b""
    font_off = (pack_off + len(pack) + 511) & ~511
    fw_end = (font_off + len(font) + 0x3fff) & ~0x3fff
    size = (fw_end + 0x104000) & ~0xfffff
    bcd = lambda v: int(str(v), 16)
    hdr = bytearray(0x200)
    struct.pack_into("<HBB", hdr, 0, bcd(date.year), bcd(date.day), bcd(date.month))
    struct.pack_into("<I", hdr, 0x0c, 0x76543210)
    hdr[0x10:0x10 + 15] = b"ECHO MINI newfw"
    hdr[0x30:0x30 + 3] = b"0.1"
    struct.pack_into("<I", hdr, 0x50, 4)
    for i in range(4):
        struct.pack_into("<2I", hdr, 0x54 + 8 * i, 0x200, len(part) if i == 3 else 0)
    if pack:                                    # our codec pack, in the image-pack slot
        struct.pack_into("<3I", hdr, 0x148, 1, pack_off, len(pack))
    if font:                                    # our font blob, in the 16 px font slot
        struct.pack_into("<I2I2I", hdr, 0x74, 2, 0, 0, font_off, len(font))
    struct.pack_into("<I", hdr, 0x1f4, fw_end)
    hdr[0x1f8:0x200] = b"RKnanoFW"

    img = bytearray(size)
    img[0:0x200] = hdr
    img[0x200:0x200 + len(part)] = part
    img[pack_off:pack_off + len(pack)] = pack
    img[font_off:font_off + len(font)] = font
    img[fw_end:fw_end + 0x200] = hdr
    img += struct.pack("<I", rkcrc(bytes(img)))
    open(dst, "wb").write(img)
    print(f"  entry 0: code 0x{BASE:08x}+0x{len(code):x}, data 0x{d_vma:08x}+0x{d_size:x} "
          f"(load copy at 0x{d_lma:08x}), bss 0x{b_vma:08x}+0x{b_size:x}")
    print(f"wrote {dst}: {len(img)} bytes, fw_end 0x{fw_end:x}, date {date}, "
          f"CRC 0x{rkcrc(bytes(img[:-4])):08x}")


if __name__ == "__main__":
    main()
