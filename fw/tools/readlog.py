#!/usr/bin/env python3
"""Prints NEWFWLOG.TXT (the circular log of the newfw firmware) in chronological order.

  readlog.py [NEWFWLOG.TXT]      (default: the file at the root of the mounted player)

Sector 0 is the header "NEWFWLOG 1 next=XXXXXXXX fill=XXX wraps=XXXXXXXX ..." (hex): the next
line goes into sector 'next' after 'fill' bytes. When the log has wrapped, the oldest text starts
in the sector after 'next'. Unwritten space is filled with spaces and is dropped here.
"""
import sys

DEFAULT = "/run/media/beretta/ECHO MINI/NEWFWLOG.TXT"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    d = open(path, "rb").read()
    h = d[:512]
    if not h.startswith(b"NEWFWLOG 1 next="):
        sys.exit(f"{path}: no newfw log header (the firmware has not written it yet)")
    nxt, fill, wraps = int(h[16:24], 16), int(h[30:33], 16), int(h[40:48], 16)
    n = len(d) // 512
    sec = [d[512 * i:512 * (i + 1)] for i in range(n)]
    order = (list(range(nxt + 1, n)) if wraps else []) + list(range(1, nxt + 1))
    order = [i for i in order if not sec[i].startswith(b"NEWFWSTATE")]  # the resume state
    text = b"".join(sec[i][:fill] if i == nxt else sec[i] for i in order)
    lines = [ln.rstrip(b" ") for ln in text.split(b"\n")]
    out = b"\n".join(ln for ln in lines if ln.strip())
    print(f"# {path}: {n - 1} data sectors, next {nxt} fill {fill}, wraps {wraps}"
          + (" (the oldest lines were overwritten)" if wraps else ""))
    sys.stdout.write(out.decode("utf-8", "replace") + "\n")


if __name__ == "__main__":
    main()
