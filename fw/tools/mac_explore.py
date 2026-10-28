#!/usr/bin/env python3
"""Unattended search for HIFIACC's MAC (ETUDE §73) through the USB probe.

    mac_explore.py MOUNT OUTDIR [HOURS]

Known: FUN_CFG (+0x0c) bits 31..29 select the function (MAC = 4, per the vendor's register
names), MAC_CFG (+0x30) holds the term count at bit 18, the 64-bit result is read at +0x34 (low)
and +0x38 (high); the data shares the FFT memory (+0x1000, +0x2000). The plain sequence (the
FFT's: FUN_CFG, then | 3, then 0) gives 0. Each trial here: known operands (a = 1..1024, b = 1)
in both memories and, sometimes, pushed through the input FIFO (+0x40); random values in
+0x00, +0x04, +0x0c (MAC type kept or not), +0x24, +0x30; the result registers read while
started and after the release, with the output FIFO and the memories. Trials where anything
moves (the FFT's own memory writes aside) are written to OUTDIR/hits.jsonl; OUTDIR/summary.txt every 10 minutes.
"""
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, ProbeError  # noqa: E402

H = 0x01060000
A = list(range(1, 1025))
B = [1] * 1024


def pick(rng, n):
    return rng.choice([n, n << 18, n << 16, n << 15, n << 8, n, n - 1, (n - 1) << 18,
                       rng.getrandbits(32), 1 << rng.randrange(32)])


def trial(p, rng):
    n = rng.choice([1, 2, 8, 16, 64, 256, 1024])
    fun_type = rng.choice([4, 4, 4, rng.randrange(8)])
    fun = fun_type << 29 | (rng.getrandbits(29) & ~3 if rng.random() < 0.5 else
                            rng.choice([0, n << 15, n << 18, n, n << 8]))
    start = rng.choice([1, 2, 3])
    w = [(0x00, rng.choice([0x001f0000, 0x01080108, 0x01010101, rng.getrandbits(32)])),
         (0x04, rng.choice([0x01f00000, 0, 1 << rng.randrange(32), rng.getrandbits(32)])),
         (0x24, rng.choice([0, pick(rng, n)])),
         (0x30, pick(rng, n))]
    fifo = rng.random() < 0.3
    p.power_b()
    p.poke(H + 0x1000, A)
    p.poke(H + 0x2000, B)
    for o, v in w:
        p.poke(H + o, [v])
    p.poke(H + 0x0c, [fun])
    p.poke(H + 0x0c, [fun | start])
    if fifo:
        p.poke(H + 0x40, A[:min(n, 32)])
    during = p.peek(H + 0x34, 2)
    rx = p.peek(H + 0xa0, 1)
    regs = p.peek(H, 16)
    p.poke(H + 0x0c, [0])
    after = p.peek(H + 0x34, 2)
    ma, mb = p.peek(H + 0x1000, 16), p.peek(H + 0x2000, 16)
    # the FFT (type 2) rewrites the memories by design: only the result registers count there
    mem = (ma != A[:16] or mb != B[:16]) and fun_type != 2
    moved = any(during) or any(after) or any(rx) or mem
    if not moved:
        return None
    return {"t": time.time(), "n": n, "fun": hex(fun), "start": start, "fifo": fifo,
            "writes": [[hex(o), hex(v)] for o, v in w], "during": during, "after": after,
            "rx": rx, "mem_a": ma[:8], "mem_b": mb[:8], "regs": [hex(v) for v in regs],
            "expect": sum(A[:n])}


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    mount, out = argv[1], argv[2]
    hours = float(argv[3]) if len(argv) > 3 else 8.0
    os.makedirs(out, exist_ok=True)
    rng = random.Random(int(time.time()))
    st = {"trials": 0, "hits": 0, "result_regs": 0, "err": 0}
    t0 = t_sum = time.time()
    p = None
    with open(os.path.join(out, "hits.jsonl"), "a") as log:
        while time.time() - t0 < hours * 3600:
            try:
                if p is None:
                    p = Probe(mount)
                    p.info()
                r = trial(p, rng)
                st["trials"] += 1
                if r:
                    st["hits"] += 1
                    st["result_regs"] += bool(any(r["during"]) or any(r["after"]))
                    log.write(json.dumps(r) + "\n")
                    log.flush()
            except (ProbeError, OSError) as e:
                st["err"] += 1
                log.write(json.dumps({"t": time.time(), "error": str(e)}) + "\n")
                log.flush()
                p = None
                time.sleep(60)
            if time.time() - t_sum > 600:
                t_sum = time.time()
                with open(os.path.join(out, "summary.txt"), "w") as f:
                    f.write("%.2f h: %s\n" % ((t_sum - t0) / 3600, st))
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write("%.2f h (end): %s\n" % ((time.time() - t0) / 3600, st))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
