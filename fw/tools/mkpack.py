#!/usr/bin/env python3
"""Codec pack for our firmware image: core B codec images (ELF) -> pack.bin.

  mkpack.py OUT.bin ID:codec.elf [ID:codec.elf ...]

Pack (src/hifi_proto.h): u32 "NNCP", u32 count, count x {codec, offset, code_size, data_addr,
data_size, bss_end}; each blob 512-aligned from the pack start: code (run at 0 in core B, loaded
at HIGHRAM0), then data (loaded at data_addr in HIGHRAM1).
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mkimg import elf                                              # noqa: E402

MAGIC = 0x50434e4e


def blob(path):
    e, segs, secs = elf(path)
    loads = [s for s in segs if s[0] == 1 and s[4]]
    code = bytearray()
    for _, off, vaddr, paddr, filesz, *_ in loads:
        if vaddr < 0x20000:
            end = vaddr + filesz
            if len(code) < end:
                code += bytes(end - len(code))
            code[vaddr:end] = e[off:off + filesz]
    data = secs.get(".data")
    d_addr, d_size = (data[3], data[5]) if data else (0x01023000, 0)
    d = e[data[4]:data[4] + d_size] if d_size else b""
    bss = secs[".bss"]
    bss_end = bss[3] + bss[5]
    if len(code) > 0x20000 or bss_end > 0x01050000:
        sys.exit(f"{path}: code {len(code)} bytes or BSS end 0x{bss_end:08x} too large")
    return bytes(code), d_addr, bytes(d), bss_end


def main():
    out, items = sys.argv[1], sys.argv[2:]
    ents, blobs = [], b""
    head = 8 + 24 * len(items)
    off = (head + 511) & ~511
    for it in items:
        cid, path = it.split(":", 1)
        code, d_addr, d, bss_end = blob(path)
        body = code + bytes(-len(code) % 512) + d
        body += bytes(-len(body) % 512)
        ents.append((int(cid), off + len(blobs), len(code), d_addr, len(d), bss_end))
        blobs += body
        print(f"  codec {cid}: code {len(code)} bytes, data {len(d)} bytes at 0x{d_addr:08x}, "
              f"BSS end 0x{bss_end:08x}")
    hdr = struct.pack("<2I", MAGIC, len(ents)) + b"".join(struct.pack("<6I", *x) for x in ents)
    pack = hdr + bytes(off - len(hdr)) + blobs
    open(out, "wb").write(pack)
    print(f"wrote {out}: {len(pack)} bytes")


if __name__ == "__main__":
    main()
