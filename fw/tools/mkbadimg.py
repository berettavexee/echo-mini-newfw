#!/usr/bin/env python3
"""Variants of a firmware image for the installer tests of the emulator (update.c's rules).

  mkbadimg.py IN.IMG OUTDIR

Writes into OUTDIR:
  NEWDATE.IMG   valid, the same code with its FW_DATE string changed (another image that the
                same ELF still describes: the boot log tells which copy started)
  BADCRC.IMG    one byte of the code changed, CRC left as it was     -> CRC mismatch
  BADCOPY.IMG   the header copy at fw_end changed, CRC recomputed   -> header copy mismatch
  BADSIZE.IMG   fw_end not a multiple of 512, CRC recomputed        -> size / layout not accepted
  SHORT.IMG     cut before its CRC word                             -> size / layout not accepted
  NOTFW.IMG     no RKnanoFW signature                               -> not an RKnanoFW image
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from rkcrc import rkcrc  # noqa: E402


def layout(d):
    fw_end = struct.unpack_from("<I", d, 0x1f4)[0]
    return fw_end, (fw_end + 0x104000) & ~0xfffff


def with_crc(d):
    _, size = layout(d)
    return d[:size] + struct.pack("<I", rkcrc(d[:size]))


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, "rb").read())
    fw_end, size = layout(d)
    assert d[0x1f8:0x200] == b"RKnanoFW" and len(d) >= size + 4, "not one of our images"
    assert struct.unpack_from("<I", d, size)[0] == rkcrc(bytes(d[:size])), "CRC of the input"
    os.makedirs(out, exist_ok=True)
    made = {}

    v = bytearray(d)                            # FW_DATE "YYYY-MM-DD": the day turned over
    n = 0
    i = v.find(b"20", 0x200)
    while 0 <= i < fw_end:
        s = bytes(v[i:i + 10])
        if len(s) == 10 and s[4:5] == b"-" and s[7:8] == b"-" and s[:4].isdigit() and \
                s[5:7].isdigit() and s[8:10].isdigit():
            day = (int(s[8:10]) % 28) + 1
            v[i + 8:i + 10] = b"%02d" % day
            n += 1
        i = v.find(b"20", i + 1)
    assert n, "no FW_DATE string found"
    made["NEWDATE.IMG"] = with_crc(bytes(v))

    v = bytearray(d)
    v[0x1000] ^= 0xff
    made["BADCRC.IMG"] = bytes(v)

    v = bytearray(d)
    v[fw_end + 0x10] ^= 0xff
    made["BADCOPY.IMG"] = with_crc(bytes(v))

    v = bytearray(d)
    struct.pack_into("<I", v, 0x1f4, fw_end + 4)
    v[fw_end + 4:fw_end + 4 + 0x200] = v[:0x200]
    made["BADSIZE.IMG"] = with_crc(bytes(v))

    made["SHORT.IMG"] = bytes(d[:size])

    v = bytearray(d)
    v[0x1f8:0x200] = b"XXnanoFW"
    v[fw_end + 0x1f8:fw_end + 0x200] = b"XXnanoFW"
    made["NOTFW.IMG"] = with_crc(bytes(v))

    for name, data in made.items():
        open(os.path.join(out, name), "wb").write(data)
    print(f"{out}: {', '.join(made)} (from {os.path.basename(src)}, {n} date strings changed)")


if __name__ == "__main__":
    main()
