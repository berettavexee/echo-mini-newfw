#!/usr/bin/env python3
"""Emulator tests that need nothing but the build (for the CI): a disk with a synthetic boot area
(mkdisk.py --synth-idb), generated test files, the emulator, the decoded log, then checks.

  emutest.py IMG ELF [NAME ...]        (all the tests by default; exit code 1 when one fails)

Each test: the files of the user volume, the keys, the simulated time, the patterns that must
appear in the log and the emulator's output (in that order) and the ones that must not; an
optional second boot on the disk the first run left, the image chosen as the stock loader does.
Needs mtools, dosfstools and the unicorn module. The disks go to $TMPDIR and are deleted.
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
EMU, MKDISK, READLOG, MKBAD = (os.path.join(TOOLS, n) for n in
                               ("emu_fw.py", "mkdisk.py", "readlog.py", "mkbadimg.py"))
VOL_SECTOR = 0x81000
ALWAYS_FORBID = [r"EXCEPTION", r"!! WRITE OUTSIDE", r"(?i)hard ?fault"]


def keys(*seq):
    out = []
    for t, key, hold in (k + (100,) if len(k) == 2 else k for k in seq):
        out += [f"{t}:{key}"] + ([f"{t + hold}:NONE"] if hold else [])
    return ",".join(out)


def wav(path, seconds=3, rate=44100):
    """A stereo 16-bit sine, 440 Hz left, 660 Hz right, faded in and out"""
    n = seconds * rate
    fade = rate // 20
    frames = bytearray()
    for i in range(n):
        g = min(1.0, i / fade, (n - i) / fade) * 0.25
        frames += struct.pack("<hh", int(32767 * g * math.sin(2 * math.pi * 440 * i / rate)),
                              int(32767 * g * math.sin(2 * math.pi * 660 * i / rate)))
    hdr = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16) + b"data" + struct.pack("<I", len(frames))
    open(path, "wb").write(hdr + frames)


_INST = [(2000, "MENU", 1500)] + [(4000 + 300 * i, "VOL-") for i in range(6)] + \
        [(6300, "PLAY"), (7600, "PLAY")]
TESTS = {
    "boot": dict(files={"A.WAV": "wav"}, keys="", ms=7000,          # the log: every 5 s
                 expect=[r"=== boot", r"font: Fusion Pixel", r"ui: folder 0: \d+ entries"]),
    "play-wav": dict(files={"A.WAV": "wav"}, keys=keys((2000, "PLAY", 200)), ms=6500,
                     expect=[r"player: .*A\.WAV", r"player: format: 44100 Hz, 2 ch, 16 bits",
                             r"audio: running", r"player: finished"]),
    "install-ok": dict(files={"NEWDATE.IMG": "@NEWDATE.IMG"}, fw2=True,
                       keys=keys(*_INST, (14000, "PLAY")), ms=60000,
                       emu_args=["--expect-fw", "@NEWDATE.IMG"],
                       expect=[r"install: NEWDATE.IMG, \d+ bytes: check 0", r"install: result 0",
                               r"=== boot \d+: ECHO MINI newfw [^ ]+ \(2026-\d\d-\d\d\)",
                               r"FW1: == image", r"FW2: == image", r"boot: FW1"],
                       reboot=dict(ms=3000)),
    "install-badcrc": dict(files={"BADCRC.IMG": "@BADCRC.IMG"}, fw2=True,
                           keys=keys(*_INST, (12000, "BACK"), (13500, "BACK")), ms=22000,
                           expect=[r"install: BADCRC.IMG, \d+ bytes: check -?\d+ \(CRC mismatch"],
                           forbid=[r"install: writing", r"firmware area: \d+ sectors"]),
}


def run(name, t, img, elf, tmp):
    disk, dump = os.path.join(tmp, "disk.img"), os.path.join(tmp, "after.img")
    log0 = os.path.join(tmp, "NEWFWLOG.TXT")
    open(log0, "wb").write(bytes(65536))
    if any(v.startswith("@") for v in t["files"].values()) or \
            any(a.startswith("@") for a in t.get("emu_args", [])):
        subprocess.run([sys.executable, MKBAD, img, os.path.join(tmp, "bad")], check=True,
                       stdout=subprocess.DEVNULL)
    at = lambda v: os.path.join(tmp, "bad", v[1:]) if v.startswith("@") else v  # noqa: E731
    files = [f"{log0}=NEWFWLOG.TXT"]
    for dst, src in t["files"].items():
        if src == "wav":
            src = os.path.join(tmp, dst)
            wav(src)
        files.append(f"{at(src)}={dst}")
    subprocess.run([sys.executable, MKDISK, disk, img, "--synth-idb", "--size-mb", "300"] +
                   (["--fw2"] if t.get("fw2") else []) + files, check=True,
                   stdout=subprocess.DEVNULL)
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--keys",
                          t["keys"], "--ms", str(t["ms"]), "--steps", "30000000000",
                          "--dump-disk", dump] + [at(a) for a in t.get("emu_args", [])],
                         capture_output=True, text=True).stdout
    if "reboot" in t and os.path.exists(dump):
        shutil.move(dump, disk)
        out += "\n-- reboot\n" + subprocess.run(
            [sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--boot-disk", "--ms",
             str(t["reboot"]["ms"]), "--steps", "30000000000", "--dump-disk", dump],
            capture_output=True, text=True).stdout
    text = ""
    if os.path.exists(dump):
        with open(dump, "rb") as f:
            f.seek(VOL_SECTOR * 512 + 446 + 8)
            off = (VOL_SECTOR + struct.unpack("<I", f.read(4))[0]) * 512
        raw = os.path.join(tmp, "log.bin")
        subprocess.run(["mcopy", "-o", "-i", f"{dump}@@{off}", "::NEWFWLOG.TXT", raw], check=True)
        text = subprocess.run([sys.executable, READLOG, raw], capture_output=True,
                              text=True).stdout
    whole = text + "\n" + out
    fails, pos = [], 0
    for pat in t.get("expect", []):
        m = re.compile(pat, re.M).search(whole, pos)
        if m:
            pos = m.end()
        else:
            fails.append(f"missing: {pat}")
    for pat in ALWAYS_FORBID + t.get("forbid", []):
        m = re.search(pat, whole, re.M)
        if m:
            fails.append(f"forbidden: {pat}: {whole[max(0, m.start() - 60):m.end() + 60]!r}")
    return fails, whole


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    img, elf, names = sys.argv[1], sys.argv[2], sys.argv[3:] or list(TESTS)
    failed = 0
    for name in names:
        with tempfile.TemporaryDirectory(prefix=f"emutest_{name}_") as tmp:
            fails, whole = run(name, TESTS[name], os.path.abspath(img), os.path.abspath(elf), tmp)
        print(f"{name}: {'FAIL' if fails else 'PASS'}")
        for f in fails:
            print("  " + f[:300])
        if fails and os.environ.get("EMUTEST_VERBOSE"):
            print(whole[-4000:])
        failed += bool(fails)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
