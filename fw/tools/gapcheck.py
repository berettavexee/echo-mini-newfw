#!/usr/bin/env python3
"""Gapless check (ETUDE §105): is the whole recording REF (a 16-bit stereo WAV) found, sample for
sample, in what the DMA played (DAC: raw 16-bit stereo frames, emu_fw.py --dac)?

  gapcheck.py REF.wav DAC.raw

Prints where it starts and how much of it matches in a row; exit code 1 when not all of it."""
import sys
import wave


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with wave.open(sys.argv[1]) as w:
        ref = w.readframes(w.getnframes())
    dac = open(sys.argv[2], "rb").read()
    start = dac.find(ref[:4096])
    if start < 0:
        print("the start of the recording is not in the capture")
        return 1
    n = 0
    while n < len(ref) and start + n < len(dac) and dac[start + n] == ref[n]:
        n += 1
    frames = len(ref) // 4
    print(f"at byte {start}: {n // 4} of {frames} frames in a row" +
          ("" if n == len(ref) else f", the first difference at frame {n // 4}"))
    return 0 if n == len(ref) else 1


if __name__ == "__main__":
    sys.exit(main())
