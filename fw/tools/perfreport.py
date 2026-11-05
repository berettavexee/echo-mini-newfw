#!/usr/bin/env python3
"""The worst cases of the perf lines of a log (ETUDE §97: step 1 of the rework), by format.

  perfreport.py LOG [LOG...]     (NEWFWLOG.TXT from the player or the emulator, or its text)

Each LOG gives a table: per format (codec, rate/bits), the tracks seen, the longest gap of
the audio path (between two own_service calls) with the longest named phase inside it, the
longest gap of the main loops, the lowest fill of the DAC ring and of the input ring (in ms of
sound), the times core B found the input empty and the DAC ring laps lost (a wait longer
than the ring: an underrun the DMA position cannot show); then the longest idle loop gap (no track).
With several logs, a last table takes them all.
"""
import os
import re
import subprocess
import sys

TRACK = re.compile(r"perf: (?P<name>.+) \[(?P<fmt>[^][]+)\]: gap max (?P<gap>\d+) ms \((?P<ph>.+?) (?P<phms>\d+) ms\), "
                   r"loop gap max (?P<loop>\d+) ms \((?P<lph>.+?) (?P<lphms>\d+) ms\); "
                   r"(?:DAC ring never full|DAC min (?P<dac>\d+) ms; in min \d+ B \((?P<inms>\d+) ms\); "
                   r"out min \d+ B max \d+ B; in empty (?P<empty>\d+)(?:; laps lost (?P<laps>\d+))?)")
IDLE = re.compile(r"perf: idle: loop gap max (?P<loop>\d+) ms \((?P<ph>.+?) (?P<phms>\d+) ms\)")


def text_of(path):
    d = open(path, "rb").read()
    if d.startswith(b"NEWFWLOG 1 next="):          # the raw circular file: readlog.py's order
        here = os.path.dirname(os.path.abspath(__file__))
        return subprocess.run([sys.executable, os.path.join(here, "readlog.py"), path],
                              capture_output=True, text=True).stdout
    return d.replace(b"\0", b"").decode("utf-8", "replace")


def worst(lines):
    rows, idle = {}, None
    for line in lines:
        m = TRACK.search(line)
        if m:
            r = rows.setdefault(m["fmt"], dict(n=0, gap=(-1, ""), loop=(-1, ""), dac=None, inms=None,
                                          empty=0, never=0, laps=0))
            r["n"] += 1
            gap, loop = int(m["gap"]), int(m["loop"])
            if gap > r["gap"][0]:
                r["gap"] = (gap, f"{m['ph']} {m['phms']}" if m["ph"] != "-" else "-")
            if loop > r["loop"][0]:
                r["loop"] = (loop, f"{m['lph']} {m['lphms']}" if m["lph"] != "-" else "-")
            if m["dac"] is None:
                r["never"] += 1
            else:
                for k in ("dac", "inms"):
                    v = int(m[k])
                    r[k] = v if r[k] is None else min(r[k], v)
                r["empty"] += int(m["empty"])
                r["laps"] += int(m["laps"] or 0)
            continue
        m = IDLE.search(line)
        if m and (idle is None or int(m["loop"]) > idle[0]):
            idle = (int(m["loop"]), f"{m['ph']} {m['phms']}" if m["ph"] != "-" else "-")
    return rows, idle


def show(title, rows, idle):
    print(f"== {title}")
    if not rows and not idle:
        print("   no perf line")
        return
    print(f"   {'format':16} {'tracks':>6} {'gap max ms':>10} {'(phase ms)':<18} {'loop max':>8} "
          f"{'(phase ms)':<18} {'DAC min':>7} {'in min':>6} {'empty':>5} {'laps':>4}")
    for fmt in sorted(rows):
        r = rows[fmt]
        na = lambda v: "-" if v is None else str(v)  # noqa: E731
        never = f"  ({r['never']} never full)" if r["never"] else ""
        print(f"   {fmt:16} {r['n']:>6} {r['gap'][0]:>10} {r['gap'][1]:<18} {r['loop'][0]:>8} "
              f"{r['loop'][1]:<18} {na(r['dac']):>7} {na(r['inms']):>6} {r['empty']:>5} {r['laps']:>4}{never}")
    if idle:
        print(f"   idle: loop gap max {idle[0]} ms ({idle[1]})")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    every = []
    for path in sys.argv[1:]:
        lines = text_of(path).splitlines()
        every += lines
        show(path, *worst(lines))
    if len(sys.argv) > 2:
        show("all", *worst(every))


if __name__ == "__main__":
    main()
