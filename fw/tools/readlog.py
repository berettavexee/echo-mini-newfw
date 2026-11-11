#!/usr/bin/env python3
"""Prints NEWFWLOG.TXT (the circular log of the newfw firmware) in chronological order.

  readlog.py [--elf FW.ELF] [NEWFWLOG.TXT]   (default: the file at the root of the player)

Sector 0 is the header "NEWFWLOG 1 next=XXXXXXXX fill=XXX wraps=XXXXXXXX ..." (hex): the next
line goes into sector 'next' after 'fill' bytes. When the log has wrapped, the oldest text starts
in the sector after 'next'. Unwritten space is filled with spaces and is dropped here.
--elf: the lines the firmware wrote raw ("?log 01a4 5 "abc"": a token record whose formats were
no longer in the image, src/log.h) made text with the formats of that ELF (its section "logfmt").
"""
import re
import struct
import sys

DEFAULT = "/run/media/beretta/ECHO MINI/NEWFWLOG.TXT"


def elf_formats(path):
    """the section "logfmt" of an ELF32 (the log's formats, by offset)"""
    e = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", e, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", e, 0x2e)
    sh = [struct.unpack_from("<10I", e, shoff + i * shentsize) for i in range(shnum)]
    for s in sh:
        if e[sh[shstrndx][4] + s[0]:].split(b"\0")[0] == b"logfmt":
            return e[s[4]:s[4] + s[5]]
    sys.exit(f"{path}: no section logfmt")


RAW = re.compile(rb'\?log ([0-9a-f]{4})((?: (?:"[^"]*"|[0-9a-f]+))*)$')
CONV = re.compile(rb"%(0?)(\d*)l*([sducxX%])")


def untoken(line, fmts):
    """a raw line made text, as log.c's vformat() would have"""
    m = RAW.search(line)
    if not m:
        return line
    tok = int(m.group(1), 16)
    fmt = fmts[tok:fmts.index(b"\0", tok)]
    args = re.findall(rb'"([^"]*)"|([0-9a-f]+)', m.group(2))
    k = 0

    def conv(c):
        nonlocal k
        pad, width, t = c.group(1), int(c.group(2) or 0), c.group(3)
        if t == b"%":
            return b"%"
        s, v = args[k] if k < len(args) else (b"", b"0")
        k += 1
        if t == b"s":
            return s.rjust(width)
        n = int(v or b"0", 16)
        if t == b"c":
            return bytes([n & 0xff])
        if t == b"d" and n >= 1 << 31:
            r = b"-" + str((1 << 32) - n).encode()
        else:
            r = format(n, {b"d": "d", b"u": "d", b"x": "x", b"X": "X"}[t]).encode()
        return r.rjust(width, b"0" if pad else b" ")
    return line[:m.start()] + CONV.sub(conv, fmt)


def main():
    a = sys.argv[1:]
    fmts = None
    if "--elf" in a:
        i = a.index("--elf")
        fmts = elf_formats(a[i + 1])
        del a[i:i + 2]
    path = a[0] if a else DEFAULT
    d = open(path, "rb").read()
    h = d[:512]
    if not h.startswith(b"NEWFWLOG 1 next="):
        sys.exit(f"{path}: no newfw log header (the firmware has not written it yet)")
    nxt, fill, wraps = int(h[16:24], 16), int(h[30:33], 16), int(h[40:48], 16)
    n = len(d) // 512
    sec = [d[512 * i:512 * (i + 1)] for i in range(n)]
    order = (list(range(nxt + 1, n)) if wraps else []) + list(range(1, nxt + 1))
    crash = [s for s in sec if s.startswith(b"NEWFWCRASH")]          # the fault record (0.99r32)
    order = [i for i in order if not sec[i].startswith((b"NEWFWSTATE", b"NEWFWCRASH"))]
    text = b"".join(sec[i][:fill] if i == nxt else sec[i] for i in order)
    lines = [ln.rstrip(b" ") for ln in text.split(b"\n")]
    if fmts:
        lines = [untoken(ln, fmts) for ln in lines]
    out = b"\n".join(ln for ln in lines if ln.strip())
    print(f"# {path}: {n - 1} data sectors, next {nxt} fill {fill}, wraps {wraps}"
          + (" (the oldest lines were overwritten)" if wraps else ""))
    sys.stdout.write(out.decode("utf-8", "replace") + "\n")
    for c in crash:                             # printed after the log, apart from it
        print("# fault record:")
        sys.stdout.write("\n".join(ln.rstrip() for ln in c.decode("ascii", "replace").split("\n")
                                    if ln.strip()) + "\n")


if __name__ == "__main__":
    main()
