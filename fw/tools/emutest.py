#!/usr/bin/env python3
"""Emulator tests that need nothing but the build (for the CI): a disk with a synthetic boot area
(mkdisk.py --synth-idb), generated test files, the emulator, the decoded log, then checks.

  emutest.py IMG ELF [NAME ...]        (all the tests by default; exit code 1 when one fails)

Each test: the files of the user volume, the keys, the simulated time, the patterns that must
appear in the log and the emulator's output (in that order) and the ones that must not; an
optional second boot on the disk the first run left, the image chosen as the stock loader does.
Needs mtools, dosfstools and the unicorn module. The disks go to $TMPDIR and are deleted.
"""
import json
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
GOLDBERG = os.path.join(TOOLS, "..", "..", "testdata", "goldberg", "variatio4-5s.flac")
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


_LINE, _SKIP = r"^[^\n]*", r"(?:[^\n]*\n)*?[^\n]*"   # a line's start; lines between
_INST = [(2000, "MENU", 1500)] + [(4000 + 300 * i, "VOL-") for i in range(6)] + \
        [(6300, "PLAY"), (7600, "PLAY")]
TESTS = {
    "boot": dict(files={"A.WAV": "wav"}, keys="", ms=7000,          # the log: every 5 s
                 expect=[r"=== boot", r"font: Fusion Pixel", r"ui: folder 0: \d+ entries"]),
    "play-wav": dict(files={"A.WAV": "wav"}, keys=keys((2000, "PLAY", 200)), ms=6500,
                     expect=[r"player: .*A\.WAV", r"player: format: 44100 Hz, 2 ch, 16 bits",
                             r"audio: running", r"player: finished"]),
    "play-flac": dict(files={"A.FLAC": GOLDBERG}, keys=keys((2000, "PLAY", 200)), ms=20000,
                      expect=[r"player: format: 96000 Hz, 2 ch, 24 bits",
                              r"player: finished",
                              r"hifiacc: .*: [1-9]\d* LPC subframes on HIFIACC, 0 in software"]),
    # a blank card (no volume), a card that never answers: the top level says which
    "sd-nofs": dict(files={"A.WAV": "wav"}, sd=True, ms=11000,
                    keys=keys((2000, "BACK"), (3000, "VOL-"), (4000, "PLAY")),
                    expect=[r"ui: card not readable: no FAT32 or exFAT volume"]),
    # a card in at power-on: mounted at once, before the resume lists the top level (0.99r26, §106)
    "sd-boot": dict(files={"A.WAV": "wav"}, sd="fat", ms=9000, keys="",
                    expect=[r"ui: card mounted in \d+ ms: FAT32", r"ui: folder 0: "]),
    "sd-noreply": dict(files={"A.WAV": "wav"}, sd=True, emu_args=["--sd-noreply"], ms=11000,
                       keys=keys((2000, "BACK"), (3000, "VOL-"), (4000, "PLAY")),
                       expect=[r"ui: card not readable: no reply"]),
    # switched on by Power with the cable in, the sound card (no USB host in the emulator: it
    # waits for one), Power pressed (the screen off), then held 2.5 s: the card left, the power
    # off (the charging screen)
    "uac-power": dict(files={"A.WAV": "wav"}, ms=14000,
                      emu_args=["--vbus", "1", "--power", "1",
                                "--gpio-at", "1500:2:5:0,7000:2:5:1,7200:2:5:0,"
                                             "9000:2:5:1,11500:2:5:0"],
                      keys=keys((2000, "MENU", 1500), *[(4000 + 300 * i, "VOL-") for i in range(5)],
                                (6000, "PLAY")),
                      expect=[r"uac: start", r"screen off", r"uac: exit \(Power 2 s\)",
                              r"power off \(Power 2 s\)"]),
    # a track saved paused at position 0 (the state written by the test: the keys cannot pause
    # before anything is heard), then a power on: it comes back paused (PR #9: it was lost, its
    # position 0 read as "nothing to resume")
    "resume-paused-zero": dict(files={"A.WAV": "wav10"}, keys=keys((2000, "PLAY", 100)),
                               ms=7000, reboot=dict(ms=7000, keys="",
                                                    state=dict(playing=0, pos_ms=0)),
                               expect=[r"track [0-9a-f]+ at 0 ms, playing 0",
                                       r"ui: resume: paused at 0 ms of [1-9]\d+"]),
    # a queue made by PLAY held on a folder (two levels), two tracks on, then a reboot: the track
    # playing at once as a queue of its own (0.99r23), the same queue rebuilt meanwhile, the
    # track found in it, then the one after it in that queue
    "resume-tree": dict(files={"ALBUM/CD1/A1.WAV": "wav10", "ALBUM/CD1/A2.WAV": "wav10",
                               "ALBUM/CD1/A3.WAV": "wav10", "ALBUM/CD2/B1.WAV": "wav10",
                               "ALBUM/CD2/B2.WAV": "wav10"},
                        keys=keys((2000, "PLAY", 900), (5000, "PLAY", 900), (7000, "PLAY", 900)),
                        ms=13000,
                        expect=[r"ui: 5 tracks in 2 folders under ALBUM",
                                r"ui: queue 3/5: A3.WAV",               # the log: both boots
                                r"ui: resume: A3.WAV, its tree under ALBUM walked meanwhile",
                                r"ui: queue 1/1: A3.WAV",
                                r"ui: resume: tree queue under ALBUM, track 3/5$",
                                r"ui: queue 4/5: B1.WAV"],
                        reboot=dict(ms=20000)),
    # the same in shuffle (BACK held three times): Y, then Z, BACK to Y; after the reboot Y
    # plays again and Z comes after it (the order kept: the same seed and start)
    "resume-shuffle": dict(files={"ALBUM/CD1/A1.WAV": "wav10", "ALBUM/CD1/A2.WAV": "wav10",
                                  "ALBUM/CD1/A3.WAV": "wav10", "ALBUM/CD2/B1.WAV": "wav10",
                                  "ALBUM/CD2/B2.WAV": "wav10"},
                           keys=keys((2000, "PLAY", 900), (3500, "BACK", 900),
                                     (4500, "BACK", 900), (5500, "BACK", 900),
                                     (6500, "PLAY", 900), (8000, "PLAY", 900), (9000, "BACK")),
                           ms=14000,
                           expect=[_LINE + r"ui: queue (\d)/5: (\S+) \(0\)\n" + _SKIP +
                                   r"ui: queue (\d)/5: (\S+) \(0\)\n" + _SKIP +
                                   r"ui: queue \1/5: \2 \(0\)\n" + _SKIP +
                                   r"ui: resume: tree queue under ALBUM, track \1/5\n" + _SKIP +
                                   r"ui: queue \3/5: \4 \(0\)"],
                           reboot=dict(ms=20000)),
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


MKSET = os.path.join(TOOLS, "mktestset.py")
SET_GROUPS = ("formats", "tags", "id3", "containers", "covers")
for _g in SET_GROUPS:                           # the test set (mktestset.py), one folder a test
    TESTS[f"set-{_g}"] = dict(set=_g)
SET_SECONDS = 3                                 # the files of a set made here (mktestset.py)
# the CI's share of the set: each format once (with Unicode tags in each container, Musepack's
# APEv2 included), the high rates, and the cover skip past the 512 KiB window (M4A: a small one)
TESTS["set-ci"] = dict(set="tags,formats,covers", pick=[
    "tags/wav-unicode.wav", "tags/aiff-unicode.aiff", "tags/flac-unicode.flac",
    "tags/mp3-unicode.mp3", "tags/vorbis-unicode.ogg", "tags/opus-unicode.opus",
    "tags/aac-unicode.m4a", "formats/alac-24-96000.m4a", "formats/flac-24-192000.flac",
    "covers/flac-cover1m.flac", "covers/mp3-cover1m.mp3", "covers/opus-cover1m.opus",
    "covers/aac-coverjpg600.m4a", "tags/mpc-unicode.mpc", "covers/mpc-cover1m.mpc"])
FIELD_MAX = dict(title=96, artist=128, album=128)    # struct tags (tags.h), with the 0


def shown(s, field):
    """What tags.c keeps of s: whole UTF-8 characters within the field, trailing spaces off"""
    out, n = "", 0
    for c in s or "":
        k = len(c.encode())
        if n + k > FIELD_MAX[field] - 1:
            break
        out, n = out + c, n + k
    return out.rstrip(" ")


def open_ms(m):
    """The time given to a file before PLAY held moves on: 1.5 s, more for a big Ogg (its
    whole comment header, cover art included, read through for the tags: ~0.8 s a MB in the
    emulator); the files last longer than that (SET_SECONDS), so the queue never moves on
    by itself"""
    mb = m["size"] / 1e6
    return 1500 + int(mb * (1200 if m["codec"] in ("vorbis", "opus") else 100))


def run_set(group, t, img, elf, tmp):
    """Every file of one folder of the test set played in turn (PLAY on the first, then PLAY
    held for each next one, open_ms() apart); each
    'ui: tags' line checked against the manifest, the format too (PCM, FLAC, ALAC: rate and
    bits; the others: the rate). EMUTEST_SET: a set already made, else one is made here."""
    src = os.environ.get("EMUTEST_SET")
    if not src:
        src = os.path.join(tmp, "set")
        subprocess.run([sys.executable, MKSET, src, "--only", group, "--seconds",
                        str(t.get("seconds", SET_SECONDS))], check=True, stdout=subprocess.DEVNULL)
    man = [m for m in json.load(open(os.path.join(src, "manifest.json")))
           if m["file"] in t["pick"]] if "pick" in t else \
        [m for m in json.load(open(os.path.join(src, "manifest.json")))
         if m["file"].startswith(group + "/")]
    lost = sorted(set(t.get("pick", [])) - {m["file"] for m in man})
    if lost:
        return [f"not in the set: {', '.join(lost)}"], ""
    disk, dump = os.path.join(tmp, "disk.img"), os.path.join(tmp, "after.img")
    log0 = os.path.join(tmp, "NEWFWLOG.TXT")
    open(log0, "wb").write(bytes(256 * 1024))
    files = [f"{log0}=NEWFWLOG.TXT"]
    names = {}
    for i, m in enumerate(man):                 # numbered: before NEWFWLOG.TXT, in this order
        n = f"{i:03d}-{os.path.basename(m['file'])}"
        names[n] = m
        files.append(f"{os.path.join(src, m['file'])}={n}")
    total = sum(m["size"] for m in man) >> 20
    subprocess.run([sys.executable, MKDISK, disk, img, "--synth-idb", "--size-mb",
                    str(max(300, total * 2 + 64))] + files, check=True, stdout=subprocess.DEVNULL)
    ks, at = [(2000, "PLAY", 200)], 2000
    for m in man[:-1]:                          # PLAY held: the next file, once this one plays
        at += open_ms(m)
        ks.append((at, "PLAY", 800))
    ms = at + open_ms(man[-1]) + 6000           # the log is written every 5 s
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--keys",
                          keys(*ks), "--ms", str(ms), "--steps", "300000000000", "--dump-disk",
                          dump], capture_output=True, text=True)
    text = read_log(dump) if os.path.exists(dump) else ""
    fails = [] if text else [f"no log (emulator: {out.stderr.strip()[-300:]!r})"]
    seen, tags, fmt = set(), None, None
    lines = text.splitlines()
    for at, line in enumerate(lines):
        if "ui: tags in " in line:
            tags, fmt = line, None
        elif "player: format: " in line:
            fmt = line
        mq = re.search(r"ui: queue \d+/\d+: (\S+) \((-?\d+)\)", line)
        if not mq or mq.group(1) not in names or mq.group(1) in seen:
            continue
        if fmt is None:                         # a chained track opens while the loop goes on
            for nxt in lines[at + 1:]:          # (0.99r25): its format comes after the queue
                if "ui: tags in " in nxt:       # line, before the next track's tags
                    break
                if "player: format: " in nxt:
                    fmt = nxt
                    break
        m = names[mq.group(1)]
        seen.add(mq.group(1))
        what = f"{m['file']}: "
        if mq.group(2) != "0":
            fails.append(what + f"not opened ({mq.group(2)})")
            continue
        if m["expect"] != "any":
            tg = m["tags"] if m["expect"] == "tags" else {}
            track = int((tg.get("track") or "0").split("/")[0])
            want = "'{}' / '{}' / '{}' / {}".format(*(shown(tg.get(k), k) for k in FIELD_MAX),
                                                     track)
            got = (tags or "").split(" us: ", 1)[-1]
            cut = got.rstrip("\ufffd")             # a log line holds 255 bytes (log.c)
            if got != want and not (len((tags or "").encode()) >= 240 and want.startswith(cut)):
                fails.append(what + f"tags {got!r}, expected {want!r}")
        mf = re.search(r"format: (\d+) Hz, (\d+) ch, (\d+) bits", fmt or "")
        if not mf:
            fails.append(what + "no format line")
        elif int(mf.group(1)) != m["rate"] or int(mf.group(2)) != m["channels"] or (
                m["codec"] in ("wav", "aiff", "flac", "alac") and int(mf.group(3)) != m["bits"]):
            fails.append(what + f"format {mf.group(0)}, expected {m['rate']} Hz, "
                         f"{m['channels']} ch, {m['bits']} bits")
    for n, m in names.items():
        if n not in seen:
            fails.append(f"{m['file']}: never played")
    for pat in ALWAYS_FORBID:
        mm = re.search(pat, text + "\n" + out.stdout, re.M)
        if mm:
            fails.append(f"forbidden: {pat}")
    return fails, text + "\n" + out.stdout


def read_log(dump):
    with open(dump, "rb") as f:
        f.seek(VOL_SECTOR * 512 + 446 + 8)
        off = (VOL_SECTOR + struct.unpack("<I", f.read(4))[0]) * 512
    raw = dump + ".log"
    subprocess.run(["mcopy", "-o", "-i", f"{dump}@@{off}", "::NEWFWLOG.TXT", raw], check=True)
    return subprocess.run([sys.executable, READLOG, raw], capture_output=True, text=True).stdout


# The resume state (resume.c): the last sector of NEWFWLOG.TXT, "NEWFWSTATE 3", then depth, the
# path (DEPTH_MAX 16 clusters, sel and top), track, playing, level, mode, pos_ms and the sum
STATE_FIELDS = dict(track=144, playing=148, level=152, mode=156, pos_ms=160)
STATE_SUM = 164
STATE_LEN, STATE_LEN_SUM = 168, 172             # the track's length (0.99r16), its own sum


def patch_state(disk, fields):
    """Changes fields of the resume state on the disk in place, its sum made again: a state the
    keys cannot reach in a short run (a track paused before any of it was heard, say)"""
    with open(disk, "r+b") as f:
        d = f.read()
        at = d.find(b"NEWFWSTATE 3")
        while at >= 0 and at % 512:             # the firmware's own copy of the string first
            at = d.find(b"NEWFWSTATE 3", at + 1)
        if at < 0:
            raise RuntimeError("no resume state on the disk")
        s = bytearray(d[at:at + 512])
        for k, v in fields.items():
            struct.pack_into("<I", s, STATE_FIELDS[k], v)
        h = 0x4e465753
        for (w,) in struct.iter_unpack("<I", bytes(s[:STATE_SUM])):
            h = ((h << 5 | h >> 27) & 0xffffffff) ^ w
        old_sum, = struct.unpack_from("<I", s, STATE_SUM)
        ln, ln_sum = struct.unpack_from("<II", s, STATE_LEN)
        struct.pack_into("<I", s, STATE_SUM, h)
        if ln_sum == ~ln & 0xffffffff ^ old_sum:  # a length saved: still valid
            struct.pack_into("<I", s, STATE_LEN_SUM, ~ln & 0xffffffff ^ h)
        f.seek(at)
        f.write(s)


def run(name, t, img, elf, tmp):
    if "set" in t:
        return run_set(t["set"], t, img, elf, tmp)
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
        if src.startswith("wav"):               # "wav", or "wav10": its length in seconds
            src = os.path.join(tmp, dst.replace("/", "_"))
            wav(src, int(src_len) if (src_len := t["files"][dst][3:]) else 3)
        files.append(f"{at(src)}={dst}")
    subprocess.run([sys.executable, MKDISK, disk, img, "--synth-idb", "--size-mb", "300"] +
                   (["--fw2"] if t.get("fw2") else []) + files, check=True,
                   stdout=subprocess.DEVNULL)
    sd = []
    if t.get("sd"):                             # a blank card: 64 MiB of zeros (sparse)
        sd = ["--sd", os.path.join(tmp, "sd.img")]
        with open(sd[1], "wb") as f:
            f.truncate(64 << 20)
        if t["sd"] == "fat":                    # or a FAT32 volume on the whole card
            subprocess.run(["/usr/sbin/mkfs.fat", "-F", "32", "-n", "CARD", sd[1]], check=True,
                           stdout=subprocess.DEVNULL)
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--keys",
                          t["keys"], "--ms", str(t["ms"]), "--steps", "30000000000",
                          "--dump-disk", dump] + sd + [at(a) for a in t.get("emu_args", [])],
                         capture_output=True, text=True).stdout
    if "reboot" in t and os.path.exists(dump):
        shutil.move(dump, disk)
        if t["reboot"].get("state"):
            patch_state(disk, t["reboot"]["state"])
        out += "\n-- reboot\n" + subprocess.run(
            [sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--boot-disk", "--ms",
             str(t["reboot"]["ms"]), "--steps", "30000000000", "--dump-disk", dump] +
            (["--keys", t["reboot"]["keys"]] if t["reboot"].get("keys") else []),
            capture_output=True, text=True).stdout
    text = read_log(dump) if os.path.exists(dump) else ""
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
