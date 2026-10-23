#!/usr/bin/env python3
"""Compares 16-bit stereo PCM (raw or WAV): best lag within +-N frames, SNR, largest error.

  pcmcmp.py GOT REF [--lag 3000]
"""
import sys
import numpy as np


def load(p):
    d = open(p, "rb").read()
    if d[:4] == b"RIFF":
        d = d[d.find(b"data") + 8:]
    return np.frombuffer(d[:len(d) // 4 * 4], "<i2").reshape(-1, 2).astype(np.float64)


def main():
    a = sys.argv[1:]
    maxlag = int(a[a.index("--lag") + 1]) if "--lag" in a else 3000
    g, r = load(a[0]), load(a[1])
    best = None
    for lag in range(-maxlag, maxlag + 1):
        x, y = g[max(0, lag):], r[max(0, -lag):]
        n = min(len(x), len(y), 20000)
        if n < 1000:
            continue
        e = np.sum((x[:n] - y[:n]) ** 2)
        if best is None or e < best[0]:
            best = (e, lag)
    lag = best[1]
    x, y = g[max(0, lag):], r[max(0, -lag):]
    n = min(len(x), len(y))
    e = x[:n] - y[:n]
    snr = 10 * np.log10(np.sum(y[:n] ** 2) / max(np.sum(e ** 2), 1e-9))
    print(f"frames got {len(g)} ref {len(r)}; lag {lag}; SNR {snr:.1f} dB over {n} frames; "
          f"max err {np.max(np.abs(e)):.0f}")


if __name__ == "__main__":
    main()
