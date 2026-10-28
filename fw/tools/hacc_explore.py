#!/usr/bin/env python3
"""Unattended exploration of HIFIACC's FFT and MAC through the USB probe (ETUDE §72-73).

    hacc_explore.py MOUNT OUTDIR [HOURS]

Each trial: every HiFi block reset (power-b), a known signal written into the 8 KiB data RAM
(0x01061000), a register combination written (+0x00, +0x04, +0x0c, +0x2c and a few others),
a short wait, then the RAM and the registers read back; the same writes are then played on a
second random input. A plain start fills the RAM with a constant pattern (0x03ff03ff, 0x07ff07ff)
whatever the input, so a trial is kept only when the output depends on the input (the two runs
differ, and the output is not the input). Kept trials are scored against numpy's FFT and IFFT of
the input, complex values only, for several layouts (interleaved, real, split halves, natural or
bit-reversed) and any scale; the score is the lower of the two runs. Inputs are either 32-bit
words (re, im interleaved) or packed 16-bit pairs (re low, im high); outputs are read from the
data RAM and from the two 1 KiB 32-bit RAMs (0x0106a000, 0x0106c000). Half the trials mutate one
of the best configurations so far (one bit of one register, or the size), a hill climb.

Output: OUTDIR/trials.jsonl (kept trials), OUTDIR/summary.txt (rewritten every 10 minutes:
counts, best FFT scores, the register writes that moved the RAM). Survives a probe error
(the device rebooted, USB mode left): waits and retries.
"""
import json
import os
import random
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, ProbeError  # noqa: E402

HACC = 0x01060000
RAM, RAM_WORDS = 0x01061000, 2048
OUTS = [(RAM, RAM_WORDS), (0x0106a000, 256), (0x0106c000, 256)]
REGS = 64                                   # registers +0x00..+0xfc read back
SIZES = [64, 128, 256, 512, 1024]


def signal(n, rng):
    """A random complex signal of n points with two tones, in [-1, 1]"""
    t = np.arange(n)
    nrng = np.random.default_rng(rng.getrandbits(32))
    x = 0.3 * np.exp(2j * np.pi * rng.randrange(1, n // 2) * t / n)
    x += 0.2 * np.cos(2 * np.pi * rng.randrange(1, n // 2) * t / n)
    x += 0.2 * (nrng.uniform(-1, 1, n) + 1j * nrng.uniform(-1, 1, n))
    return x


def pack_interleaved(x, amp):
    v = np.empty(2 * len(x), np.int64)
    v[0::2] = np.round(x.real * amp)
    v[1::2] = np.round(x.imag * amp)
    return [int(a) & 0xffffffff for a in v]


def pack16(x, amp):
    re = np.clip(np.round(x.real * amp), -32768, 32767).astype(np.int64) & 0xffff
    im = np.clip(np.round(x.imag * amp), -32768, 32767).astype(np.int64) & 0xffff
    return [int(a) for a in re | im << 16]


def s32(words):
    a = np.array(words, np.int64)
    return np.where(a >= 1 << 31, a - (1 << 32), a)


def score(out_words, x, n):
    """Best normalised correlation of the RAM content with FFT/IFFT of x, over layouts"""
    o = s32(out_words).astype(float)
    refs = {"fft": np.fft.fft(x), "ifft": np.fft.ifft(x)}
    layouts = {}
    w = np.array(out_words, np.int64)
    lo, hi = w & 0xffff, w >> 16
    lo, hi = np.where(lo >= 32768, lo - 65536, lo), np.where(hi >= 32768, hi - 65536, hi)
    if len(w) >= n:
        layouts["p16"] = lo[:n] + 1j * hi[:n]
    if len(o) >= 2 * n:
        layouts["inter"] = o[0:2 * n:2] + 1j * o[1:2 * n:2]
        layouts["inter_swap"] = o[1:2 * n:2] + 1j * o[0:2 * n:2]
    if len(o) >= n:
        layouts["real"] = o[:n] + 0j
    half = len(o) // 2
    if half >= n:
        layouts["split"] = o[:n] + 1j * o[half:half + n]
    best = (0.0, "")
    for rn, r in refs.items():
        for ln, y in layouts.items():
            for perm in ("nat", "bitrev"):
                yy = y if perm == "nat" else y[bitrev(n)]
                den = np.linalg.norm(yy) * np.linalg.norm(r)
                if den == 0:
                    continue
                c = abs(np.vdot(yy, r)) / den
                if c > best[0]:
                    best = (float(c), "%s/%s/%s" % (rn, ln, perm))
    return best


_br = {}


def bitrev(n):
    if n not in _br:
        b = n.bit_length() - 1
        _br[n] = np.array([int(format(i, "0%db" % b)[::-1], 2) for i in range(n)])
    return _br[n]


def candidates(rng):
    """Register writes for one trial: structured single bits first, then random"""
    n = rng.choice(SIZES)
    log2 = n.bit_length() - 1
    w = []
    cfg = rng.choice([0x01080108, 0x001f0000, 0, rng.getrandbits(32)])
    w.append((0x00, cfg))
    mode = rng.choice([1 << rng.randrange(32), rng.getrandbits(32) & 0x01f0ffff,
                       0x01f00000 | (1 << rng.randrange(16)), rng.randrange(16) << 12])
    w.append((0x04, mode))
    for off in rng.sample([0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38], 2):
        w.append((off, rng.choice([n, log2, n << 15, log2 << 16, RAM & 0xffff, 0, 1])))
    top = rng.choice([0xa0000000, 0xe0000000, 0x80000000, 0x20000000, 0x40000000, 0])
    nf = rng.choice([n, log2, n - 1])
    w.append((0x0c, top | nf << 15))
    w.append((0x0c, top | nf << 15 | rng.choice([1, 2, 3])))
    return n, w


def run_once(p, writes, data):
    p.power_b()
    p.poke(RAM, data)
    for off, v in writes:
        p.poke(HACC + off, [v])
    time.sleep(0.005)
    regs = p.peek(HACC, REGS)
    return regs, [p.peek(a, k) for a, k in OUTS]


def mutate(rng, base):
    n, writes, fmt, amp = base
    writes = list(writes)
    r = rng.random()
    if r < 0.15:
        n = rng.choice(SIZES)
    elif r < 0.25:
        fmt = "p16" if fmt == "i32" else "i32"
        amp = 1 << 12 if fmt == "p16" else amp
    else:
        i = rng.randrange(len(writes))
        o, v = writes[i]
        v ^= 1 << rng.randrange(32)
        writes[i] = (o, v)
        if o == 0x0c:                           # keep the arm and start writes alike
            for j, (o2, v2) in enumerate(writes):
                if o2 == 0x0c and j != i:
                    writes[j] = (o2, (v & ~3) | (v2 & 3) if v2 & 3 else v & ~3)
    return n, writes, fmt, amp


def trial(p, rng, sigs, best):
    """None, or a record; sigs counts the input-independent output patterns seen"""
    if best and rng.random() < 0.5:
        r = rng.choice(best[:10])[1]
        n, writes, fmt, amp = mutate(rng, (r["n"], [(int(o, 16), int(v, 16))
                                                     for o, v in r["writes"]], r["fmt"], r["amp"]))
    else:
        n, writes = candidates(rng)
        fmt = rng.choice(["i32", "p16"])
        amp = 1 << 12 if fmt == "p16" else rng.choice([1 << 12, 1 << 16, 1 << 20])
    xs, outs, regs = [], [], None
    for _ in range(2):
        x = signal(n, rng)
        data = pack_interleaved(x, amp) if fmt == "i32" else pack16(x, amp)
        data += [0] * (RAM_WORDS - len(data))
        regs, out = run_once(p, writes, data)
        xs.append((x, data))
        outs.append(out)
    same_as_in = outs[0][0] == xs[0][1] and outs[0][1:] == outs[1][1:]
    if outs[0] == outs[1] or same_as_in:
        k = "input" if same_as_in else "%08x..%08x" % (outs[0][0][0], outs[0][0][-1])
        sigs[k] = sigs.get(k, 0) + 1
        return None
    s, how = 0.0, ""
    for oi, (a, _) in enumerate(OUTS):
        sc = [score(o[oi], x, n) for o, (x, _) in zip(outs, xs)]
        m = min(c[0] for c in sc)
        if m > s:
            s, how = m, "%x:%s" % (a, sc[0][1])
    # chance gives about 2.5 / sqrt(n) (the best of ~40 layouts); a true FFT gives 1.0, so the
    # trials rank by z = score * sqrt(n): about 2.5 for noise, 8 or more for a real transform
    return {"t": time.time(), "n": n, "amp": amp, "fmt": fmt, "score": round(s, 4),
            "z": round(s * n ** 0.5, 2), "how": how,
            "writes": [[hex(o), hex(v)] for o, v in writes],
            "ram_moved": sum(1 for a, b in zip(outs[0][0], xs[0][1]) if a != b),
            "in_head": [hex(v) for v in xs[0][1][:8]],
            "out_head": [hex(v) for v in outs[0][0][:8]],
            "regs": [hex(v) for v in regs[:16]]}


def summary(path, stats, best, movers, sigs):
    with open(path, "w") as f:
        f.write("trials %(trials)d, input-dependent %(kept)d, probe errors %(err)d, "
                "running %(hours).2f h\n" % stats)
        f.write("\ninput-independent outputs (first..last word: count):\n")
        for k, c in sorted(sigs.items(), key=lambda kv: -kv[1])[:15]:
            f.write("%7d %s\n" % (c, k))
        f.write("\nbest trials by z = score * sqrt(n) (noise ~2.5, a real FFT >= 8; score: min of "
                "two random inputs, 1.0 = exact up to a scale):\n")
        for z, rec in best[:15]:
            f.write("z %.1f %.4f %s n=%d %s amp=%d writes=%s\n" % (z, rec["score"], rec["how"], rec["n"], rec["fmt"],
                                                            rec["amp"], rec["writes"]))
        f.write("\nregister writes seen in input-dependent trials (count):\n")
        for k, c in sorted(movers.items(), key=lambda kv: -kv[1])[:40]:
            f.write("%5d %s\n" % (c, k))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    mount, outdir = argv[1], argv[2]
    hours = float(argv[3]) if len(argv) > 3 else 10.0
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(int(time.time()))
    stats = {"trials": 0, "kept": 0, "err": 0, "hours": 0.0}
    best, movers, sigs = [], {}, {}
    t0 = t_sum = time.time()
    p = None
    with open(os.path.join(outdir, "trials.jsonl"), "a") as log:
        while time.time() - t0 < hours * 3600:
            try:
                if p is None:
                    p = Probe(mount)
                    p.info()
                rec = trial(p, rng, sigs, best)
                stats["trials"] += 1
                if rec:
                    stats["kept"] += 1
                    log.write(json.dumps(rec) + "\n")
                    log.flush()
                    best.append((rec["z"], rec))
                    best.sort(key=lambda b: -b[0])
                    del best[30:]
                    for o, v in rec["writes"]:
                        k = "%s=%s" % (o, v)
                        movers[k] = movers.get(k, 0) + 1
            except (ProbeError, OSError) as e:
                stats["err"] += 1
                log.write(json.dumps({"t": time.time(), "error": str(e)}) + "\n")
                log.flush()
                try:
                    p and p.close()
                except OSError:
                    pass
                p = None
                time.sleep(60)
            if time.time() - t_sum > (600 if stats["trials"] > 200 else 30):
                t_sum = time.time()
                stats["hours"] = (t_sum - t0) / 3600
                summary(os.path.join(outdir, "summary.txt"), stats, best, movers, sigs)
    stats["hours"] = (time.time() - t0) / 3600
    summary(os.path.join(outdir, "summary.txt"), stats, best, movers, sigs)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
