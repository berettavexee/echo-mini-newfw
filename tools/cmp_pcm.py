#!/usr/bin/env python3
"""Compare raw s16le stereo PCM with a reference WAV: lag by cross-correlation, then SNR.

  cmp_pcm.py OUT.raw REF.wav [--max-lag N]
"""
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve

raw = np.frombuffer(open(sys.argv[1], "rb").read(), "<i2").reshape(-1, 2).astype(np.float64)
rate, ref = wavfile.read(sys.argv[2])
ref = ref.astype(np.float64)
if np.abs(ref).max() <= 1.5:
    ref = ref * 32768
maxlag = int(sys.argv[sys.argv.index("--max-lag") + 1]) if "--max-lag" in sys.argv else 20000
n = min(len(raw), len(ref))
a, b = raw[:n, 0], ref[:n, 0]
xc = fftconvolve(a, b[::-1], mode="full")                 # index n-1 = lag 0
mid = n - 1
lags = np.arange(-maxlag, maxlag + 1)
lag = int(lags[np.argmax(xc[mid + lags])])                # raw[i + lag] ~ ref[i]
if lag >= 0:
    x, y = raw[lag:], ref
else:
    x, y = raw, ref[-lag:]
m = min(len(x), len(y))
x, y = x[:m], y[:m]
act = np.abs(y).max(1) > 0                                # ignore digital silence
e = np.sum((x[act] - y[act]) ** 2)
s = np.sum(y[act] ** 2)
print(f"lag {lag} frames, SNR {10 * np.log10(s / max(e, 1e-9)):.1f} dB over {m} frames, "
      f"max error {np.abs(x - y).max():.0f} LSB")
