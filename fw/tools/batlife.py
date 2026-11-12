#!/usr/bin/env python3
"""Battery life of the newfw firmware, from its log (docs/BATTERY.md).

  batlife.py extract OUTDIR LOG [LOG ...]   one CSV per session on battery of MIN_HOURS or more
  batlife.py plot DIR                       DIR/battery-life.svg, and the table of docs/BATTERY.md

LOG: NEWFWLOG.TXT, or its text decoded by readlog.py. Several logs may hold the same session
(the log is circular: an older copy has its start, a newer one its end); its pieces are joined by
its boot time (the RTC clock of each battery line minus the uptime). A session's CSV: comment
lines (version, boot time, how it ended, the formats played, the screen's time on), then
"seconds,mV" for each battery line on battery (one a minute).
"""
import glob
import os
import re
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
MIN_HOURS = 6
EMPTY_MV = 3300                                  # the firmware's power off (battery.c)
BAT = re.compile(r"\[\s*([\d.]+)\] battery (\d+) mV, usb (\d).*clock (\d+):(\d+):(\d+)")
TIME = re.compile(r"^\[\s*([\d.]+)\] ")
CODECS = {1: "WAV", 2: "FLAC", 3: "MP3", 4: "Vorbis", 5: "Opus", 6: "AIFF", 7: "AAC", 8: "ALAC",
          9: "MPC"}                             # enum hp_codec (hifi_proto.h)


def log_lines(path):
    data = open(path, "rb").read(16)
    if data.startswith(b"NEWFWLOG"):
        out = subprocess.run([sys.executable, os.path.join(TOOLS, "readlog.py"), path],
                             capture_output=True, check=True).stdout
        return out.decode("utf-8", "replace").splitlines()
    return open(path, encoding="utf-8", errors="replace").read().splitlines()


def sessions(paths):
    """{key: session} for the sessions on battery found in the logs"""
    found = {}
    for path in paths:
        cur = ver = rtc = None
        codec = screen_t = None
        for ln in log_lines(path):
            if "=== boot" in ln:
                m = re.search(r"newfw (\S+) ", ln)
                ver, rtc, cur = (m.group(1) if m else None), None, None
                continue
            m = re.search(r"rtc \(boot\): (\S+ \S+)", ln)
            if m:
                rtc = m.group(1)
            m = BAT.match(ln)
            if m:
                up = float(m.group(1))
                tod = int(m.group(4)) * 3600 + int(m.group(5)) * 60 + int(m.group(6))
                key = round(((tod - up) % 86400) / 120)     # the boot time, to 2 minutes
                cur = found.setdefault(key, dict(pts={}, ver=None, rtc=None, end=None,
                                                 fmt={}, screen={}))
                cur["ver"] = cur["ver"] or ver
                cur["rtc"] = cur["rtc"] or rtc
                cur["pts"][round(up)] = (int(m.group(2)), int(m.group(3)))
                continue
            if cur is None:
                continue
            t = TIME.match(ln)
            t = float(t.group(1)) if t else None
            if "power off" in ln:
                cur["end"] = re.sub(r", battery \d+ mV", "", ln.split("] ", 1)[1]).strip()
            m = re.search(r"player: .*, \d+ bytes, our codec (\d+)", ln)
            if m:
                codec = CODECS.get(int(m.group(1)), "other")
            m = re.search(r"player: (?:finished|stopped|failed) after \d+ frames, (\d+) ms", ln)
            if m and codec and t is not None:
                cur["fmt"][round(t)] = (codec, int(m.group(1)))
            if "screen on" in ln and t is not None:
                screen_t = t
            if "screen off" in ln and t is not None and screen_t is not None:
                cur["screen"][round(screen_t)] = t - screen_t
                screen_t = None
    return found


def extract(outdir, paths):
    os.makedirs(outdir, exist_ok=True)
    for s in sessions(paths).values():
        allp = sorted(s["pts"].items())
        first = next((i for i, (_, (mv, usb)) in enumerate(allp) if usb == 0), len(allp))
        last = max((i for i, (_, (mv, usb)) in enumerate(allp) if usb == 0), default=-1)
        pts, usb_s, on = [], 0, 0               # time on battery: the cable's time left out
        span_pts = allp[first:last + 1]
        for (t0, (_, usb0)), (t1, (mv, usb1)) in zip(span_pts[:1] + span_pts[:-1], span_pts):
            if usb0 or usb1:
                usb_s += t1 - t0
            else:
                on += t1 - t0
            if not usb1:
                pts.append((on, mv))
        span = pts[-1][0] - pts[0][0] if pts else 0
        if span < MIN_HOURS * 3600 or not s["ver"] or len(pts) < span / 90:
            continue                            # short, or two sessions on one key
        fmt = {}
        for name, ms in s["fmt"].values():
            fmt[name] = fmt.get(name, 0) + ms
        total = sum(fmt.values()) or 1
        mix = ", ".join(f"{n} {100 * ms / total:.0f} %" for n, ms in
                        sorted(fmt.items(), key=lambda x: -x[1]))
        date = (s["rtc"] or "unknown").split()[0]
        name = os.path.join(outdir, f"{date}_{s['ver']}.csv")
        with open(name, "w") as f:
            f.write(f"# version {s['ver']}\n# boot {s['rtc']}\n# end {s['end'] or 'none'}\n")
            f.write(f"# played {total / 3600000:.1f} h: {mix}\n")
            f.write(f"# screen on {sum(s['screen'].values()) / 60:.0f} min\n")
            f.write(f"# cable {usb_s / 60:.0f} min\n")
            f.write("seconds,mV\n")
            for t, mv in pts:
                f.write(f"{t - pts[0][0]},{mv}\n")
        print(f"wrote {name}: {len(pts)} points, {(pts[-1][0] - pts[0][0]) / 3600:.1f} h")


def load(path):
    meta, pts = {}, []
    for ln in open(path):
        if ln.startswith("#"):
            k, _, v = ln[2:].strip().partition(" ")
            meta[k] = v
        elif ln[0].isdigit():
            t, mv = ln.split(",")
            pts.append((int(t), int(mv)))
    return meta, pts


def hours_to(pts, mv):
    for t, v in pts:
        if v <= mv:
            return t / 3600
    return None


# The chart: one line a run, in the reference categorical order (dataviz skill), light and dark
LIGHT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
DARK = ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300", "#9085e9", "#e66767"]


def svg(runs):
    w, h, ml, mr, mt, mb = 760, 400, 56, 120, 24, 44
    pw, ph = w - ml - mr, h - mt - mb
    tmax = max(16, max(p[-1][0] for _, _, p in runs) / 3600)
    tmax = int(tmax + 1.999) // 2 * 2
    vlo, vhi = 3200, 4250
    x = lambda t: ml + pw * t / 3600 / tmax                     # noqa: E731
    y = lambda v: mt + ph * (vhi - v) / (vhi - vlo)             # noqa: E731
    css = ["svg{--bg:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--grid:#e4e3df;"
           + ";".join(f"--s{i}:{c}" for i, c in enumerate(LIGHT)) + "}",
           "@media (prefers-color-scheme: dark){svg{--bg:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;"
           "--grid:#3a3936;" + ";".join(f"--s{i}:{c}" for i, c in enumerate(DARK)) + "}}",
           "text{font:12px system-ui,-apple-system,'Segoe UI',sans-serif;fill:var(--ink2)}",
           ".t{fill:var(--ink)}.g{stroke:var(--grid);stroke-width:1}",
           ".l{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}"]
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" '
         f'height="{h}" role="img" aria-label="Battery voltage against time on battery, '
         f'one line a firmware version">',
         "<style>" + "".join(css) + "</style>",
         f'<rect width="{w}" height="{h}" fill="var(--bg)"/>']
    for v in range(vlo, vhi + 1, 100):
        o.append(f'<line class="g" x1="{ml}" x2="{ml + pw}" y1="{y(v):.1f}" y2="{y(v):.1f}"/>')
        if v % 200 == 0:
            o.append(f'<text x="{ml - 8}" y="{y(v) + 4:.1f}" text-anchor="end">'
                     f'{v / 1000:.1f} V</text>')
    for t in range(0, tmax + 1, 2):
        o.append(f'<text x="{x(t * 3600):.1f}" y="{mt + ph + 18}" text-anchor="middle">'
                 f'{t} h</text>')
    o.append(f'<line x1="{ml}" x2="{ml + pw}" y1="{y(EMPTY_MV):.1f}" y2="{y(EMPTY_MV):.1f}" '
             f'stroke="var(--ink2)" stroke-width="1" stroke-dasharray="4 4"/>')
    o.append(f'<text x="{ml + 6}" y="{y(EMPTY_MV) - 6:.1f}">power off, 3.30 V for 30 s</text>')
    o.append(f'<text x="{ml + pw / 2}" y="{h - 8}" text-anchor="middle">time on battery</text>')
    ends = []
    for i, (label, meta, pts) in enumerate(runs):
        step = max(1, len(pts) // 400)
        d = " ".join(f"{'M' if k == 0 else 'L'}{x(t):.1f},{y(v):.1f}"
                     for k, (t, v) in enumerate(pts[::step] + [pts[-1]]))
        o.append(f'<path class="l" stroke="var(--s{i % 8})" d="{d}"><title>{label}</title>'
                 "</path>")
        ex, ey = x(pts[-1][0]), y(pts[-1][1])   # the end: a dot ringed with the surface
        o.append(f'<circle cx="{ex:.1f}" cy="{ey:.1f}" r="4" fill="var(--s{i % 8})" '
                 f'stroke="var(--bg)" stroke-width="2"/>')
        ends.append([ey, label, i])
    ends.sort()                                  # direct labels at the line ends, kept apart
    for k in range(1, len(ends)):
        ends[k][0] = max(ends[k][0], ends[k - 1][0] + 14)
    for ly, label, i in ends:                   # the legend, at the right, in end order
        o.append(f'<line x1="{ml + pw + 12}" x2="{ml + pw + 26}" y1="{ly:.1f}" y2="{ly:.1f}" '
                 f'stroke="var(--s{i % 8})" stroke-width="2" stroke-linecap="round"/>')
        o.append(f'<text class="t" x="{ml + pw + 32}" y="{ly + 4:.1f}">{label}</text>')
    o.append("</svg>")
    return "\n".join(o) + "\n"


def plot(d):
    runs = []
    for p in sorted(glob.glob(os.path.join(d, "*.csv"))):
        meta, pts = load(p)
        runs.append((meta["version"], meta, pts))
    open(os.path.join(d, "battery-life.svg"), "w").write(svg(runs))
    rows = ["| Version | Date | Start | Hours on battery | Hours to 3.6 V | End | Played | "
            "Screen on | Cable |", "|---|---|---|---|---|---|---|---|---|"]
    for label, meta, pts in runs:
        t36 = hours_to(pts, 3600)
        end = meta.get("end", "none")
        end = ("battery empty" if "battery empty" in end else
               "none (log ends)" if end == "none" else end.replace("power off ", "").strip("()"))
        rows.append(f"| {label} | {meta['boot'].split()[0]} | {pts[0][1] / 1000:.2f} V | "
                    f"{pts[-1][0] / 3600:.1f} | {f'{t36:.1f}' if t36 else '-'} | {end} | "
                    f"{meta.get('played', '')} | {meta.get('screen', '').replace('on ', '')} | "
                    f"{meta.get('cable', '0 min')} |")
    table = "\n".join(rows)
    doc = os.path.join(TOOLS, "..", "..", "docs", "BATTERY.md")
    if os.path.exists(doc):
        s = open(doc).read()
        s = re.sub(r"(<!-- runs -->\n)(?:.*?\n)?(<!-- /runs -->)",    # empty or not
                   lambda m: m.group(1) + table + "\n" + m.group(2), s, flags=re.S)
        open(doc, "w").write(s)
    print(table)


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "extract":
        extract(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) == 3 and sys.argv[1] == "plot":
        plot(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
