#!/usr/bin/env python3
"""Decode the IDB (Rockchip ID block) of an ECHO MINI boot-area capture (RECHO BOOTDUMP.BIN).

  idb.py BOOTDUMP.BIN [OUTDIR]

The capture holds eMMC sectors 0..0x1fff; the IDB copies start at sector 64 + 1024 * i.
Sector 0 of an IDB is RC4-encrypted with Rockchip's well-known key (magic 0x0ff0aa55 in
clear, 0xfcdc8c3b encrypted); sector 1 is plain. The remaining sectors are dumped raw and
RC4-decoded per 512-byte sector, the usual Rockchip scheme for the loader code.
"""
import os
import struct
import sys

KEY = bytes([124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17])


def rc4(data, key=KEY):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xff
        s[i], s[j] = s[j], s[i]
    out, i, j = bytearray(), 0, 0
    for b in data:
        i = (i + 1) & 0xff
        j = (j + s[i]) & 0xff
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xff])
    return bytes(out)


def main():
    b = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else None
    base = 64 * 512
    sec = [b[base + 512 * k:base + 512 * (k + 1)] for k in range(1024 - 64)]
    used = [k for k, s in enumerate(sec) if s.strip(b"\0") and s.strip(b"\xff")]
    print(f"IDB copy 0: sectors {used[0]}..{used[-1]} of the block used ({len(used)})")
    s0 = rc4(sec[0])
    print("sec0 (decrypted) magic %08x" % struct.unpack_from("<I", s0)[0])
    print("sec0 words", " ".join("%08x" % w for w in struct.unpack_from("<32I", s0)))
    s1 = sec[1]
    print("sec1 words", " ".join("%08x" % w for w in struct.unpack_from("<32I", s1)))
    print("sec1 +0x02 sys MiB %u, +0x0a %r" % (struct.unpack_from("<H", s1, 2)[0], s1[0x0a:0x0e]))
    if out:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "idb_sec0.bin"), "wb").write(s0)
        open(os.path.join(out, "idb_sec1.bin"), "wb").write(s1)
        raw = b"".join(sec[2:used[-1] + 1])
        dec = b"".join(rc4(s) for s in sec[2:used[-1] + 1])
        open(os.path.join(out, "idb_rest_raw.bin"), "wb").write(raw)
        open(os.path.join(out, "idb_rest_rc4.bin"), "wb").write(dec)
        print("wrote", out)


if __name__ == "__main__":
    main()
