#!/usr/bin/env python3
"""Emulator tests that need nothing but the build (for the CI): a disk with a synthetic boot area
(mkdisk.py --synth-idb), generated test files, the emulator, the decoded log, then checks.

  emutest.py [-j N] IMG ELF [NAME ...]   (all the tests by default; exit code 1 when one fails)

-j N: N tests at a time (default: the cores, at most 8; -j 1 one after the other); the results
are printed in the order of the list. The test sets the tests need are made once, before.

Each test: the files of the user volume, the keys, the simulated time, the patterns that must
appear in the log and the emulator's output (in that order) and the ones that must not; an
optional second boot on the disk the first run left, the image chosen as the stock loader does.
Needs mtools, dosfstools and the unicorn module. The disks go to $TMPDIR and are deleted.
"""
import concurrent.futures
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
ALWAYS_FORBID = [r"EXCEPTION", r"!! WRITE OUTSIDE", r"(?i)hard ?fault",
                 r"\?log [0-9a-f]{4}"]          # a token record not made text (src/log.h)
LOW_ORDER = r"!! HIFIACC order"                 # orders 1 and 2, wrong on the device (ETUDE §66,
                                                # §116): never sent, but by the benches (hwtest)


def damage_overlays(disk):
    """one byte changed in each overlay of each image copy on the disk (its table: "NOVL")"""
    d = bytearray(open(disk, "rb").read())
    at = d.find(b"NOVL")
    while at >= 0:
        if at % 512 == 0 and struct.unpack_from("<I", d, at + 4)[0] == 1:
            for i in range(struct.unpack_from("<I", d, at + 12)[0]):
                off = struct.unpack_from("<I", d, at + 16 + 16 * i)[0]
                d[at + off + 100] ^= 0x40
        at = d.find(b"NOVL", at + 4)
    open(disk, "wb").write(d)


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
_INST = [(2000, "MENU", 1500)] + [(4000 + 300 * i, "VOL-") for i in range(7)] + \
        [(6300, "PLAY"), (7600, "PLAY")]
TESTS = {
    "boot": dict(files={"A.WAV": "wav"}, keys="", ms=7000,          # the log: every 5 s
                 expect=[r"=== boot", r"font: Fusion Pixel", r"ui: folder 0: \d+ entries"]),
    # a short PLAY while the loop is held (a long operation): read by the SysTick interrupt
    # (ETUDE §140), taken once the loop goes on; until 0.104 it was lost
    "keys-stall": dict(files={"A.WAV": "wav"}, keys=keys((2100, "PLAY", 120)), ms=8000,
                       emu_args=["--stall-at", "2000:500"],
                       expect=[r"keys: play \(adc \d+, 1\d\d ms\)", r"player: .*A\.WAV",
                               r"STALL of the loop: 500 ms"]),
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
                      keys=keys((2000, "MENU", 1500), *[(4000 + 300 * i, "VOL-") for i in range(6)],
                                (6000, "PLAY")),
                      expect=[r"ovl: USB from FW1 in \d+ us", r"uac: start", r"screen off",
                              r"uac: exit \(Power 2 s\)", r"power off \(Power 2 s\)"]),
    # the sound card left by BACK held: its overlay was over the directory listing (ETUDE §137),
    # read again for the browser
    "uac-back": dict(files={"A.WAV": "wav"}, ms=12000,
                     emu_args=["--vbus", "1", "--power", "1", "--gpio-at", "1500:2:5:0"],
                     keys=keys((2000, "MENU", 1500), *[(4000 + 300 * i, "VOL-") for i in range(6)],
                               (6000, "PLAY"), (8000, "BACK", 1500)),
                     expect=[r"ovl: USB from FW1 in \d+ us", r"uac: start", r"uac: exit \(BACK\)",
                             r"ui: folder [0-9a-f]+: \d+ entries"]),
    # the USB disk (read only, no host in the emulator) left by BACK: the listing read again
    "usb-disk": dict(files={"A.WAV": "wav"}, ms=20000,
                     emu_args=["--vbus", "1", "--power", "1", "--gpio-at", "1500:2:5:0"],
                     keys=keys((2000, "MENU", 1500), *[(4000 + 300 * i, "VOL-") for i in range(4)],
                               (6000, "PLAY"), (8000, "BACK", 300)),
                     expect=[r"ovl: USB from FW1 in \d+ us", r"usb: read only disk",
                             r"ui: card to be set up again", r"ui: folder [0-9a-f]+: \d+ entries"]),
    # the same with the USB overlay damaged in the image on the eMMC: a message, no fault
    "ovl-bad": dict(files={"A.WAV": "wav"}, ms=12000,
                    emu_args=["--vbus", "1", "--power", "1", "--gpio-at", "1500:2:5:0"],
                    prep=lambda disk, tmp: damage_overlays(disk),
                    keys=keys((2000, "MENU", 1500), *[(4000 + 300 * i, "VOL-") for i in range(6)],
                              (6000, "PLAY")),
                    expect=[r"ovl: USB: FW1: CRC mismatch", r"ui: card to be set up again"],
                    forbid=[r"uac: start"]),
    # the fault tests (ECHO_FAULT builds only: Power alone, core B faults, then is stuck; with
    # VOL- held, core A faults; with VOL+ held, core A is stuck). fault-a: the record written
    # on the internal memory by core A's fault handler, then logged and shown once by the next
    # boot (0.99r32)
    "fault-a": dict(files={"A.WAV": "wav10"}, ms=6000, fault=True,
                    keys=keys((2000, "PLAY", 100), (3900, "VOL-", 600)),
                    emu_args=["--gpio-at", "4000:2:5:1,4100:2:5:0"],
                    expect=[r"fault test: core A faults",
                            r"crash: the last session ended on a fault \(record \d+\)",
                            r"crash: core A: HardFault, exception 3",
                            r"crash: cfsr 00008200 hfsr 40000000 bfar fffffff0$",
                            r"crash: track A\.WAV at \d+ ms"],
                    reboot=dict(ms=5000)),
    # fault-b: core B faults: the same track again 1 s before (0.99r33); then stuck on the same
    # track (0.99r34: seen after 500 ms without a frame): passed over, the next one plays
    "fault-b": dict(files={"A.WAV": "wav10", "B.WAV": "wav10"}, keys=keys((2000, "PLAY", 100)),
                    ms=13000, fault=True,
                    emu_args=["--gpio-at", "4000:2:5:1,4100:2:5:0,7000:2:5:1,7100:2:5:0"],
                    expect=[r"fault test: core B faults", r"player: core B error 4",
                            r"player: core B started again on A\.WAV at \d+ ms",
                            r"ui: queue 1/2: A\.WAV \(0\)",
                            r"fault test: core B stuck",
                            r"player: core B stuck: no frame for \d+ ms",
                            r"player: core B stopped twice on A\.WAV: passed over",
                            r"ui: queue 2/2: B\.WAV \(0\)"]),
    # stall-a: core A stuck (0.99r34): the watchdog's interrupt after 2.7 s, the record, a
    # reboot; the next boot shows it (the watchdog modelled with --cycles only)
    "stall-a": dict(files={"A.WAV": "wav10"}, ms=9000, fault=True,
                    keys=keys((2000, "PLAY", 100), (3900, "VOL+", 600)),
                    emu_args=["--cycles", "--gpio-at", "4000:2:5:1,4100:2:5:0"],
                    expect=[r"watchdog: armed, 2\.7 s", r"fault test: core A stuck",
                            r"WATCHDOG: the loop stuck for 2\.7 s at pc",
                            r"crash: core A: watchdog, exception 24",
                            r"crash: track A\.WAV at \d+ ms"],
                    reboot=dict(ms=5000)),
    # PLAY on the play screen: a pause, a resume, twice (2 s each: more than half the DAC ring),
    # then the track to its end. Until 0.100f1 core B slept on after a resume ("core B stuck"
    # 500 ms later); until 0.100f2 it read the pause's time as laps of the DMA (its write position
    # jumped: stale sound, a track ended early)
    "pause-resume": dict(files={"A.WAV": "wav10"},
                         keys=keys((2000, "PLAY"), (4000, "PLAY"), (6000, "PLAY"),
                                   (8000, "PLAY"), (10000, "PLAY")), ms=19000,
                         expect=[r"audio: paused", r"audio: resumed", r"audio: paused",
                                 r"audio: resumed", r"player: finished after 441000 frames, 94\d\d ms"],
                         forbid=[r"core B stuck", r"core B underruns [1-9]"]),
    # both headphone outputs in, PLAY: the 3.5 mm out (the 4.4 mm still in: no pause), then the
    # 4.4 mm out: a pause (ETUDE §119); in again: no play until PLAY; PLAY, PLAY (paused by
    # hand), out again: nothing more
    "unplug-pause": dict(files={"A.WAV": "wav10"}, ms=13000,
                         emu_args=["--gpio-at", "0:0:13:1,0:2:8:0,4000:0:13:0,5000:2:8:1,"
                                   "6000:2:8:0,8500:2:8:1"],
                         keys=keys((2000, "PLAY"), (7000, "PLAY"), (8000, "PLAY")),
                         expect=[r"keys: jack 3\.5 mm out", r"keys: jack 4\.4 mm out(?:\n[^\n]*){0,3}"
                                 r"ui: headphones out: paused",
                                 r"keys: jack 4\.4 mm in", r"keys: play", r"audio: resumed",
                                 r"keys: play", r"audio: paused", r"keys: jack 4\.4 mm out"],
                         check=lambda w: [] if w.count("headphones out: paused") == 1 and not
                         re.search(r"jack 4\.4 mm in\n(?:(?![^\n]*keys: play)[^\n]*\n)*?"
                                   r"[^\n]*audio: resumed", w)
                         else ["a pause or a play not expected"]),
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
    # the interface's language (ETUDE §112): Settings, Language (its first line) set to the
    # first catalogue (Français), back; after the reboot the setting read, the catalogue loaded
    "lang": dict(files={"A.WAV": "wav"}, ms=8000,
                 keys=keys((2000, "MENU", 1500), (4000, "VOL-"), (4300, "VOL-"), (4600, "VOL-"),
                           (4900, "PLAY"), (5500, "PLAY"), (6100, "BACK")),
                 expect=[r"lang: 3 catalogue\(s\) in FW1", r"lang: fr \(Français\), \d+ strings",
                         r"ui: language fr", r"ui: settings 3: .*language fr",
                         r"lang: fr \(Français\)"],
                 reboot=dict(ms=12000)),
    # Settings, the line Clock (under Date and time) to hidden, back; after the reboot the
    # setting read (ETUDE §120; the clock itself is not set in the emulator: nothing drawn)
    "clock-hide": dict(files={"A.WAV": "wav"}, ms=8000,
                       keys=keys((2000, "MENU", 1500), (4000, "VOL-"), (4300, "VOL-"),
                                 (4600, "VOL-"), (4900, "PLAY"), (5300, "VOL-"), (5600, "VOL-"),
                                 (5900, "PLAY"), (6500, "BACK")),
                       expect=[r"ui: clock hidden", r"ui: settings 3: .*clock hidden"],
                       forbid=[r"ui: settings 3: .*clock shown"],
                       reboot=dict(ms=8000)),
    "install-ok": dict(files={"NEWDATE.IMG": "@NEWDATE.IMG"}, fw2=True,
                       keys=keys(*_INST, (14000, "PLAY")), ms=60000,
                       emu_args=["--expect-fw", "@NEWDATE.IMG"],
                       expect=[r"install: NEWDATE.IMG, \d+ bytes: check 0", r"install: result 0",
                               r"=== boot \d+: ECHO MINI newfw [^ ]+ \(2026-\d\d-\d\d\)",
                               r"FW1: == image", r"FW2: == image", r"boot: FW1"],
                       reboot=dict(ms=3000)),
    # the list with the long names (0.99r37): two images whose 8.3 names would both be
    # NEWFWR~n.IMG, the second chosen (VOL- once), its long name in the log
    "install-long": dict(files={"NEWFW-0.99r37-first-test-image.IMG": "@BADCRC.IMG",
                                "NEWFW-0.99r37-second-test-image-long-name.img": "@BADCRC.IMG"},
                         keys=keys(*_INST[:-1], (8600, "VOL-"), (9600, "PLAY"), (13000, "BACK"),
                                   (14500, "BACK")), ms=18000,
                         expect=[r"install: NEWFW-0\.99r37-second-test-image-long-name\.img, "
                                 r"\d+ bytes: check"],
                         forbid=[r"install: writing", r"install: NEWFWR~"]),
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
# covers: its big Ogg files get 30 s (open_ms), so they last 40 s: shorter, the queue moved on
# by itself during their header's read, and a PLAY held then, lost until 0.104 (ETUDE §140),
# skipped the next file
TESTS["set-covers"]["seconds"] = 40
# the CI's share of the set: each format once (with Unicode tags in each container, Musepack's
# APEv2 included), the high rates, and the cover skip past the 512 KiB window (M4A: a small one)


def check_hwa(text):
    """The ALAC bench (ETUDE §113), against the emulator's model of the filter: on every track
    the coefficients reversed exact, without a timeout; in stream order, caught out on some
    track (beyond order 1, where both orders are the same)"""
    fails, tracks, caught = [], 0, 0
    for m in re.finditer(r"hwa: (\S+): (\d+) predictions, coefficients reversed: (\d+) exact, "
                         r"(\d+) differ; in stream order: \d+ exact, (\d+) differ; \d+ skipped, "
                         r"(\d+) timeouts", text):
        name, n, differ, s_differ, tmo = m.group(1), *(int(m.group(k)) for k in (2, 4, 5, 6))
        tracks += 1
        caught += s_differ > 0
        if differ or tmo or not n:
            fails.append(f"hwa: {name}: {n} predictions, {differ} differ, {tmo} timeouts")
    if tracks < 2:
        fails.append(f"hwa: {tracks} track(s) logged")
    if not caught:
        fails.append("hwa: the stream order never differed (the bench sees nothing?)")
    return fails


def check_hwi(text):
    """The Imdct36 bench: the end seen as IRQ 7 on each transform, SYNTH's as IRQ 1"""
    fails = []
    m = re.search(r"\((\d+) IRQ 7, (\d+) timeouts\)", text)
    if not m or m.group(1) == "0" or m.group(2) != "0":
        fails.append(f"hwi: {m.group(0) if m else 'no IRQ 7 line'}")
    m = re.search(r"IRQ 1 pending at (\d+) of (\d+) ends", text)
    if not m or m.group(1) != m.group(2) or m.group(1) == "0":
        fails.append(f"hws: {m.group(0) if m else 'no IRQ 1 line'}")
    return fails


# The benches of the accelerators (ETUDE §113): the test image only (make EXTRA=-DECHO_HWTEST),
# not in the default list
# ALAC on HIFIACC (ETUDE §116): orders 8 and 30 (16-bit stereo, 24/96) on it, 2 in software
# (wrong on the device: LOW_ORDER forbidden), each bit-exact against ffmpeg in what the DAC played
TESTS["alac-exact"] = dict(set="alacorder", seconds=2, exact=True, pick=[
    "alacorder/alac-16-44100-stereo-o8.m4a", "alacorder/alac-24-96000-stereo-o30.m4a",
    "alacorder/alac-16-44100-stereo-o2.m4a"],
    check=lambda text: [] if re.search(r"alacacc: \S+: [1-9]\d* predictions on HIFIACC", text)
    else ["no prediction on HIFIACC"])
TESTS["hwt-alac"] = dict(set="alacorder", seconds=1, hwtest=True, check=check_hwa)
TESTS["hwt-imdct"] = dict(set="formats", pick=["formats/mp3-16-44100.mp3"], hwtest=True,
                          check=check_hwi)

TESTS["set-ci"] = dict(set="tags,formats,covers", pick=[
    "tags/wav-unicode.wav", "tags/aiff-unicode.aiff", "tags/flac-unicode.flac",
    "tags/mp3-unicode.mp3", "tags/vorbis-unicode.ogg", "tags/opus-unicode.opus",
    "tags/aac-unicode.m4a", "formats/alac-24-96000.m4a", "formats/flac-24-192000.flac",
    "covers/flac-cover1m.flac", "covers/mp3-cover1m.mp3", "covers/opus-cover1m.opus",
    "covers/aac-coverjpg600.m4a", "tags/mpc-unicode.mpc", "covers/mpc-cover1m.mpc"])
FIELD_MAX = dict(title=96, artist=128, album=128)    # struct tags (tags.h), with the 0
MORE_MAX = dict(albumartist=96, genre=48)           # its library fields (§122)


def shown(s, field):
    """What tags.c keeps of s: whole UTF-8 characters within the field, trailing spaces off"""
    out, n = "", 0
    for c in s or "":
        k = len(c.encode())
        if n + k > {**FIELD_MAX, **MORE_MAX}[field] - 1:
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
    src = t.get("set_dir") or os.environ.get("EMUTEST_SET")
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
    if t.get("exact"):                          # each file whole: the folder plays on by itself
        ks = [(2000, "PLAY", 200)]
        ms = 2000 + 1000 * len(man) * (t.get("seconds", SET_SECONDS) + 1) + 6000
    dac = os.path.join(tmp, "dac.raw")
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--keys",
                          keys(*ks), "--ms", str(ms), "--steps", "300000000000", "--dump-disk",
                          dump] + (["--dac", dac] if t.get("exact") else []),
                         capture_output=True, text=True)
    text = read_log(dump) if os.path.exists(dump) else ""
    fails = [] if text else [f"no log (emulator: {out.stderr.strip()[-300:]!r})"]
    seen, tags, fmt, more = set(), None, None, None
    lines = text.splitlines()
    for at, line in enumerate(lines):
        if "ui: tags in " in line:
            tags, fmt, more = line, None, None
        elif "ui: tags+: " in line:
            more = line
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
            num = lambda v: int(re.match(r"\d*", v or "").group() or 0)  # noqa: E731
            yr = num(tg.get("year"))
            mw = "'{}' / '{}' / {} / {}".format(shown(tg.get("albumartist"), "albumartist"),
                                              shown(tg.get("genre"), "genre"),
                                              yr if 1000 <= yr <= 9999 else 0, num(tg.get("disc")))
            mg = (more or "").split("ui: tags+: ", 1)[-1] if more else "'' / '' / 0 / 0"
            if mg != mw:
                fails.append(what + f"tags+ {mg!r}, expected {mw!r}")
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
    for pat in ALWAYS_FORBID + ([] if t.get("hwtest") else [LOW_ORDER]):
        mm = re.search(pat, text + "\n" + out.stdout, re.M)
        if mm:
            fails.append(f"forbidden: {pat}")
    if t.get("exact"):
        fails += check_exact(man, src, dac)
    if "check" in t:
        fails += t["check"](text)
    return fails, text + "\n" + out.stdout


def check_exact(man, src, dac):
    """Each file decoded by ffmpeg, in the ring's format (16-bit: packed stereo; 20 and 24-bit:
    32-bit words, the data in the high bits; mono duplicated), found whole in what the DMA read
    (--dac): bit-exact, from the first sample to the last"""
    heard = open(dac, "rb").read() if os.path.exists(dac) else b""
    fails = []
    for m in man:
        ref = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i",
                              os.path.join(src, m["file"]), "-ac", "2", "-f",
                              "s16le" if m["bits"] == 16 else "s32le", "-"],
                             capture_output=True, check=True).stdout
        if not ref or heard.find(ref) < 0:
            fb = 4 if m["bits"] == 16 else 8    # where its start is, and how far it agrees
            at = heard.find(ref[fb * 2048:fb * 2048 + 1024]) - fb * 2048
            same = 0
            if at >= 0:
                while same < len(ref) and at + same < len(heard) and \
                        heard[at + same] == ref[same]:
                    same += 1
            fails.append(f"{m['file']}: not bit-exact in what the DAC played ({len(ref)} bytes "
                         f"expected; found from {at}, the same for {same} bytes)")
    return fails


def read_log(dump):
    with open(dump, "rb") as f:
        f.seek(VOL_SECTOR * 512 + 446 + 8)
        off = (VOL_SECTOR + struct.unpack("<I", f.read(4))[0]) * 512
    raw = dump + ".log"
    subprocess.run(["mcopy", "-o", "-i", f"{dump}@@{off}", "::NEWFWLOG.TXT", raw], check=True)
    return subprocess.run([sys.executable, READLOG, raw], capture_output=True, text=True).stdout


# The resume state (resume.c): the last sector of NEWFWLOG.TXT, "NEWFWSTATE 4", then depth, the
# path (DEPTH_MAX 16 clusters, sel and top), track, playing, level, mode, pos_ms and the sum
STATE_FIELDS = dict(track=144, playing=148, level=152, mode=156, pos_ms=160)
STATE_SUM = 164
STATE_LEN, STATE_LEN_SUM = 168, 172             # the track's length (0.99r16), its own sum


def patch_state(disk, fields):
    """Changes fields of the resume state on the disk in place, its sum made again: a state the
    keys cannot reach in a short run (a track paused before any of it was heard, say)"""
    with open(disk, "r+b") as f:
        d = f.read()
        at = d.find(b"NEWFWSTATE 4", VOL_SECTOR * 512)    # in the volume: the firmware's own
        while at >= 0 and at % 512:             # copies before it (its strings may fall on a
            at = d.find(b"NEWFWSTATE 4", at + 1)    # sector's start: 0.102b15)
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
    if "custom" in t:
        return t["custom"](img, elf, tmp)
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
    if "prep" in t:                             # the volume changed before the run (fragmented,
        t["prep"](disk, tmp)                    # a full root...)
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
    between = t["between"](dump, tmp) if "between" in t and os.path.exists(dump) else []
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
    if os.environ.get("EMUTEST_LOG"):           # (the run kept, to look into a failure)
        open(os.environ["EMUTEST_LOG"], "w").write(whole)
    fails, pos = [], 0
    for pat in t.get("expect", []):
        m = re.compile(pat, re.M).search(whole, pos)
        if m:
            pos = m.end()
        else:
            fails.append(f"missing: {pat}")
    fails += between
    if "check" in t:
        fails += t["check"](whole)
    if "disk_check" in t:
        fails += t["disk_check"](dump, tmp) if os.path.exists(dump) else ["no disk after the run"]
    for pat in ([] if t.get("fault") else ALWAYS_FORBID) + t.get("forbid", []) + \
            ([] if t.get("hwtest") else [LOW_ORDER]):
        m = re.search(pat, whole, re.M)
        if m:
            fails.append(f"forbidden: {pat}: {whole[max(0, m.start() - 60):m.end() + 60]!r}")
    return fails, whole


# ---- the library's file created by the firmware (ETUDE §124): ECHO_LIBCREATE images only ----
LIB_SIZE = 512 + (8 << 20)


def lib_header():
    h = b"NEWFWLIB" + struct.pack("<II", 1, 0xffffffff)
    h += bytes(508 - len(h))
    import zlib
    return h + struct.pack("<I", zlib.crc32(h))


def part_offset(disk):
    with open(disk, "rb") as f:
        f.seek(VOL_SECTOR * 512 + 446 + 8)
        return (VOL_SECTOR + struct.unpack("<I", f.read(4))[0]) * 512


def mtools(disk, *cmd):
    return subprocess.run([cmd[0], "-i", f"{disk}@@{part_offset(disk)}", *cmd[1:]],
                          capture_output=True, text=True)


def fsck(disk, tmp, lost_ok=False):
    """fsck.fat -n on the volume: [] when clean (FSInfo's free count unknown or stale allowed:
    the hosts count again); lost_ok: what a cut during the creation leaves, clusters reserved
    that nothing uses (in the first FAT only when the cut came between the two copies)"""
    off = part_offset(disk)
    with open(disk, "rb") as f:
        f.seek(off + 32)
        total = struct.unpack("<I", f.read(4))[0]
        f.seek(off)
        part = os.path.join(tmp, "part.img")
        with open(part, "wb") as o:
            left = total * 512
            while left:
                b = f.read(min(left, 1 << 22))
                o.write(b)
                left -= len(b)
    r = subprocess.run(["/usr/sbin/fsck.fat", "-n", part], capture_output=True, text=True)
    os.remove(part)
    bad = [ln for ln in r.stdout.splitlines()[1:] if ln.strip() and not re.match(
        r"(Checking|Free cluster summary|\s*Auto-correcting|Starting check|Leaving filesystem|"
        r".*: \d+ files,|  Leaving)", ln) and not (lost_ok and re.search(
            r"[Rr]eclaim|unused clusters|orphan|[Ll]ost|FATs differ but appear to be intact|"
            r"Using first FAT", ln))]
    return [f"fsck: {ln.strip()}" for ln in bad][:5]


def lib_file_check(disk, tmp, lost_ok=False):
    """The volume clean, NEWFWLIB.DAT there: its size, one run of clusters, the empty header"""
    from fatimg import Volume
    fails = fsck(disk, tmp, lost_ok)
    v = Volume(disk, VOL_SECTOR)
    e = next((e for e in v.list() if e.name == "NEWFWLIB.DAT"), None)
    if not e:
        return fails + ["no NEWFWLIB.DAT"]
    chain = list(v.chain(e.cluster))
    if e.size != LIB_SIZE or chain != list(range(e.cluster, e.cluster + len(chain))) or \
            len(chain) * v.spc * 512 < LIB_SIZE:
        fails.append(f"NEWFWLIB.DAT: {e.size} bytes, {len(chain)} clusters, not one run")
    elif v.sector(v.heap + (e.cluster - 2) * v.spc) != lib_header():
        fails.append("NEWFWLIB.DAT: not the empty header")
    return fails


def no_lib_check(disk, tmp):
    from fatimg import Volume
    v = Volume(disk, VOL_SECTOR)
    return fsck(disk, tmp) + (["NEWFWLIB.DAT created"] if any(
        e.name == "NEWFWLIB.DAT" for e in v.list()) else [])


def fill_holes(disk, tmp, holes_only_at_start=False):
    """Files of 7 MiB with a file of 1 KiB after each, then the big ones deleted: free runs of
    7 MiB (less than the library's 8 MiB); holes_only_at_start: ten of them, the rest of the
    volume free, FSInfo's next free cluster put back to 2 (the search goes through the holes)"""
    big, pin = os.path.join(tmp, "big.bin"), os.path.join(tmp, "pin.bin")
    open(big, "wb").write(bytes(7 << 20))
    open(pin, "wb").write(b"x" * 1024)
    k = 0
    while not holes_only_at_start or k < 10:
        r1 = mtools(disk, "mcopy", big, f"::B{k:04d}.BIN")
        r2 = mtools(disk, "mcopy", pin, f"::P{k:04d}.BIN")
        if r1.returncode or r2.returncode:
            mtools(disk, "mdel", f"::B{k:04d}.BIN")
            break
        k += 1
    for i in range(k):
        mtools(disk, "mdel", f"::B{i:04d}.BIN")
    if holes_only_at_start:
        off = part_offset(disk)
        with open(disk, "r+b") as f:
            f.seek(off + 48)
            fsinfo = struct.unpack("<H", f.read(2))[0]
            f.seek(off + fsinfo * 512 + 492)
            f.write(struct.pack("<I", 2))


def fill_root(disk, tmp):
    """The root directory's clusters full: as many 8.3 files of one byte as it has free entries"""
    from fatimg import Volume
    v = Volume(disk, VOL_SECTOR)
    free = 0
    for c in v.chain(v.root):
        for s in range(v.spc):
            b = v.sector(v.heap + (c - 2) * v.spc + s)
            free += sum(1 for i in range(16) if b[32 * i] in (0, 0xe5))
    one = os.path.join(tmp, "one.bin")
    open(one, "wb").write(b"1")
    for i in range(free):
        mtools(disk, "mcopy", one, f"::R{i:05d}.BIN")


def lib_cut(n):
    """A cut after n sectors of the creation (1: its first sector, 35 more: the two FATs, then
    the directory entry, FSInfo): the file at the next boot, created then, or found (the entry
    was written)"""
    return dict(files={"A.WAV": "wav"}, keys="", ms=3000, libcreate=True,
                emu_args=["--cut-user-after", str(n)],
                between=lambda d, tmp: fsck(d, tmp, lost_ok=True),
                reboot=dict(ms=3000),
                expect=[r"library: NEWFWLIB.DAT " + ("created" if n < 36 else "found")],
                check=lambda w: [] if "POWER CUT after" in w else ["no power cut"],
                disk_check=lambda d, tmp: lib_file_check(d, tmp, lost_ok=True))


TESTS.update({
    # a fresh volume: created (clean, its size, one run, the empty header); the next boot finds it
    "lib-create": dict(files={"A.WAV": "wav"}, keys="", ms=3000, libcreate=True,
                       reboot=dict(ms=3000),
                       expect=[r"fatwrite: NEWFWLIB\.DAT: 2049 clusters",
                               r"library: NEWFWLIB\.DAT created, sectors",
                               r"library: NEWFWLIB\.DAT found"],
                       disk_check=lib_file_check),
    # free space only in runs of 7 MiB: nothing written, the volume clean
    "lib-nospace": dict(files={"A.WAV": "wav"}, keys="", ms=3000, libcreate=True,
                        prep=fill_holes, expect=[r"library: NEWFWLIB\.DAT not created \(-3\)"],
                        disk_check=no_lib_check),
    # holes at the start, the search from cluster 2: the run after them
    "lib-holes": dict(files={"A.WAV": "wav"}, keys="", ms=3000, libcreate=True,
                      prep=lambda d, tmp: fill_holes(d, tmp, True),
                      expect=[r"library: NEWFWLIB\.DAT created"], disk_check=lib_file_check),
    # no free entry in the root: one more cluster for it, then the file
    "lib-rootfull": dict(files={"A.WAV": "wav"}, keys="", ms=3000, libcreate=True,
                         prep=fill_root, expect=[r"\(the root grows\)",
                                                 r"library: NEWFWLIB\.DAT created"],
                         disk_check=lib_file_check),
    # a power cut after 0 to 36 sectors written (among them: within the FATs, between the two
    # copies, after the directory entry): a consistent volume, then the file at the next boot
    **{f"lib-cut{n}": lib_cut(n) for n in (0, 1, 2, 17, 18, 34, 35, 36)},
})


# ---- the library's index built by the firmware (ETUDE §125): ECHO_LIBBUILD images only ----
def lib_disks(tmp):
    """A disk with the test set's tags, containers and formats files, and an exFAT card with
    12 of them (some fragmented)"""
    src = os.path.join(tmp, "set")
    subprocess.run([sys.executable, MKSET, src, "--only", "tags,containers,formats", "--seconds",
                    "1"], check=True, stdout=subprocess.DEVNULL)
    log0 = os.path.join(tmp, "NEWFWLOG.TXT")
    open(log0, "wb").write(bytes(256 * 1024))
    files = [f"{log0}=NEWFWLOG.TXT"]
    for root, _, names in os.walk(src):
        for n in sorted(names):
            if n != "manifest.json":
                p = os.path.join(root, n)
                files.append(f"{p}={os.path.relpath(p, src)}")
    disk, card = os.path.join(tmp, "disk.img"), os.path.join(tmp, "card.img")
    subprocess.run([sys.executable, MKDISK, disk, IMG_FOR_DISK[0], "--synth-idb", "--size-mb",
                    "300"] + files, check=True, stdout=subprocess.DEVNULL)
    with open(card, "wb") as f:
        f.truncate(64 << 20)
    subprocess.run(["/usr/sbin/mkfs.exfat", card], check=True, stdout=subprocess.DEVNULL)
    tags = sorted(os.listdir(os.path.join(src, "tags")))[:12]
    spec = [{"path": "Card", "dir": True}] + [
        dict(path=f"Card/{n}", src=os.path.join(src, "tags", n), **({"fragmented": True} if
                                                                    i % 4 == 1 else {}))
        for i, n in enumerate(tags)]
    json.dump(spec, open(os.path.join(tmp, "spec.json"), "w"))
    subprocess.run([sys.executable, os.path.join(TOOLS, "exfat_add.py"), card,
                    os.path.join(tmp, "spec.json")], check=True, stdout=subprocess.DEVNULL)
    return disk, card, src


IMG_FOR_DISK = [None]


def lib_boot(img, elf, disk, card, dump, extra=()):
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", card,
                          "--ms", "16000", "--steps", "30000000000", "--dump-disk", dump] +
                         list(extra), capture_output=True, text=True).stdout
    return out


def lib_compare(dump, disk, card, tmp, tag):
    """NEWFWLIB.DAT of the disk after the run against mklib.py's answers for disk and card"""
    from fatimg import Volume
    v = Volume(dump, VOL_SECTOR)
    e = next((e for e in v.list() if e.name == "NEWFWLIB.DAT"), None)
    if not e:
        return [f"{tag}: no NEWFWLIB.DAT"]
    dev = os.path.join(tmp, f"dev_{tag}.dat")
    open(dev, "wb").write(v.read(e))
    exp = os.path.join(tmp, f"exp_{tag}.txt")
    subprocess.run([sys.executable, os.path.join(TOOLS, "mklib.py"), disk, "--sd", card, "-o",
                    os.path.join(tmp, "pc.dat"), "--expect", exp], check=True,
                   capture_output=True)
    lt = os.path.join(tmp, "library_test")
    if not os.path.exists(lt):
        subprocess.run(["cc", "-O1", "-w", "-I", os.path.join(TOOLS, "..", "src"), "-o", lt,
                        os.path.join(TOOLS, "library_test.c")], check=True)
    r = subprocess.run([lt, dev, exp], capture_output=True, text=True)
    return [] if r.returncode == 0 else [f"{tag}: " + ln for ln in r.stdout.splitlines()[-4:]]


def lib_build_test(variant):
    def go(img, elf, tmp):
        IMG_FOR_DISK[0] = img
        disk, card, src = lib_disks(tmp)
        dump = os.path.join(tmp, "after.img")
        whole = lib_boot(img, elf, disk, card, dump)
        text = read_log(dump)
        fails = [] if re.search(r"library: 89 tracks \(89 read, 0 from the index before\)",
                                text) else ["first build: not 89 tracks read"]
        fails += lib_compare(dump, disk, card, tmp, "first")
        if variant == "update":                 # one file deleted, one added, one changed
            shutil.move(dump, disk)
            mtools(disk, "mdel", "::tags/flac-ascii.flac")
            mtools(disk, "mcopy", os.path.join(src, "tags", "mp3-ascii.mp3"), "::new.mp3")
            mtools(disk, "mcopy", "-o", os.path.join(src, "tags", "opus-unicode.opus"),
                   "::tags/opus-ascii.opus")
            whole += lib_boot(img, elf, disk, card, dump)
            text = read_log(dump)
            if not re.search(r"building into half B \(the index before reused\)", text) or \
                    not re.search(r"library: 89 tracks \(2 read, 87 from the index before\)",
                                  text):
                fails.append("update: not 2 read and 87 reused into half B")
            fails += lib_compare(dump, disk, card, tmp, "update")
        if variant == "nochange":               # the same files: the update stops after the count
            shutil.move(dump, disk)
            whole += lib_boot(img, elf, disk, card, dump)
            text = read_log(dump)
            if not re.search(r"library: no change \(89 tracks, the same fingerprint\)", text):
                fails.append("no change: not seen")
            fails += lib_compare(dump, disk, card, tmp, "no change")
        if variant.startswith("cut"):           # a cut during a second build: the first kept
            shutil.move(dump, disk)
            first = os.path.join(tmp, "first.img")
            shutil.copy(disk, first)
            mtools(disk, "mdel", "::tags/flac-ascii.flac")     # (a change: the build writes)
            whole += lib_boot(img, elf, disk, card, dump,
                              ["--cut-user-after", variant[3:]])
            if "POWER CUT" not in whole:
                fails.append("no power cut")
            fails += fsck(dump, tmp)
            fails += lib_compare(dump, first, card, tmp, "after the cut")
            shutil.move(dump, disk)
            whole += lib_boot(img, elf, disk, card, dump)
            fails += lib_compare(dump, disk, card, tmp, "rebuilt")
        return fails, text + "\n" + whole
    return go


LIB_KEYS = keys((2000, "MENU", 1500), (4000, "VOL-"), (4300, "VOL-"), (4600, "VOL-"),
                (4900, "PLAY"),
               *[(5200 + 250 * i, "VOL-") for i in range(12)], (8800, "PLAY"))


def lib_settings(img, elf, tmp):
    """Settings, the line Library (the 13th), PLAY: the update screen; the index compared with
    mklib.py's answers (the production image: the path the user takes)"""
    IMG_FOR_DISK[0] = img
    disk, card, _ = lib_disks(tmp)
    dump = os.path.join(tmp, "after.img")
    whole = lib_boot(img, elf, disk, card, dump, ["--keys", LIB_KEYS])
    text = read_log(dump)
    fails = [] if re.search(r"library: 89 tracks \(89 read, 0 from the index before\)", text) \
        else ["not 89 tracks read"]
    return fails + lib_compare(dump, disk, card, tmp, "settings"), text + "\n" + whole


TESTS["lib-settings"] = dict(custom=lib_settings)


def lib_views(img, elf, tmp):
    """The library's lists (ETUDE §127): the index made by mklib.py put on the disk; BACK to the
    top level (the boot shows the internal root), Library (its first line), Artists, the first artist, its first album, its first track played;
    then BACK to the top of the library, All songs, its third track played. The views' count of
    songs is mklib.py's (the card in)"""
    IMG_FOR_DISK[0] = img
    disk, card, _ = lib_disks(tmp)
    dat, exp = os.path.join(tmp, "NEWFWLIB.DAT"), os.path.join(tmp, "exp.txt")
    subprocess.run([sys.executable, os.path.join(TOOLS, "mklib.py"), disk, "--sd", card, "-o",
                    dat, "--expect", exp], check=True, capture_output=True)
    mtools(disk, "mcopy", dat, "::NEWFWLIB.DAT")
    songs = next(len(ln.split("\t")[4].split(",")) for ln in open(exp, encoding="utf-8")
                 if ln.startswith("view\t1\t9\t0\t"))
    dump = os.path.join(tmp, "after.img")
    k = keys((2000, "BACK"), (2400, "VOL+"), (2800, "PLAY"), (3400, "PLAY"), (4000, "PLAY"),
             (4600, "PLAY"), (5200, "PLAY"), (6800, "MENU"), (7400, "BACK"), (7800, "BACK"),
             (8200, "BACK"), (8600, "VOL-"), (8900, "VOL-"), (9200, "VOL-"), (9500, "VOL-"),
             (9800, "PLAY"), (10400, "VOL-"), (10700, "VOL-"), (11000, "PLAY"))
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", card,
                          "--ms", "13000", "--steps", "30000000000", "--dump-disk", dump,
                          "--keys", k], capture_output=True, text=True).stdout
    text = read_log(dump)
    if os.environ.get("EMUTEST_LOG"):           # (the log kept, to look into a failure)
        open(os.environ["EMUTEST_LOG"], "w").write(text + "\n" + out)
    fails = []
    m = re.search(r"library: views of (\d+) tracks, the card's in, in \d+ ms: (\d+) shown", text)
    if not m or int(m.group(2)) != songs:
        fails.append(f"views: {m.group(0) if m else 'not set up'} (mklib.py: {songs} shown)")
    lists = re.findall(r"ui: (\d+) tracks of the library's list (\d+)\.(\d+)", text)
    if len(lists) < 2 or lists[0][1] != "8" or lists[1][1] != "9" or int(lists[1][0]) != songs:
        fails.append(f"queues from the library: {lists}")
    # the first track of each queue: the album's first; in All songs, one down per VOL- logged
    # between the PLAY that opened it and the PLAY that played (a key may be lost in a load)
    lines = text.splitlines()
    starts = [i for i, ln in enumerate(lines) if "tracks of the library's list" in ln]
    firsts = []
    for i in starts:
        q = next((re.search(r"ui: queue (\d+)/\d+: .* \((-?\d+)\)", ln) for ln in lines[i:]
                  if "ui: queue " in ln), None)
        firsts.append((int(q.group(1)), q.group(2)) if q else None)
    plays = [i for i, ln in enumerate(lines) if "keys: play" in ln and i < starts[-1]] \
        if starts else []
    downs = sum(1 for ln in lines[plays[-2]:plays[-1]] if "keys: vol-" in ln) \
        if len(plays) >= 2 else -1
    if len(firsts) < 2 or firsts[0] != (1, "0") or firsts[1] != (1 + downs, "0"):
        fails.append(f"first tracks played: {firsts} ({downs} VOL- in All songs)")
    return fails, text + "\n" + out


TESTS["lib-views"] = dict(custom=lib_views)


def queue_file(img, elf, tmp):
    """The play queue in NEWFWQ.DAT (ETUDE §128): a library album's track played (its list
    written into the file meanwhile); the queue mode (VOL+ and VOL- together): an album added
    (MENU), another put next (PLAY); the queue's screen from the main menu; then a boot: the
    queue read again from the file, its track resumed"""
    IMG_FOR_DISK[0] = img
    disk, card, _ = lib_disks(tmp)
    dat = os.path.join(tmp, "NEWFWLIB.DAT")
    subprocess.run([sys.executable, os.path.join(TOOLS, "mklib.py"), disk, "--sd", card, "-o",
                    dat], check=True, capture_output=True)
    mtools(disk, "mcopy", dat, "::NEWFWLIB.DAT")
    dump = os.path.join(tmp, "after.img")
    k = keys((2000, "BACK"), (2400, "VOL+"), (2800, "PLAY"), (3400, "VOL-"), (3800, "PLAY"),
             (4400, "PLAY"), (5000, "PLAY"), (7000, "MENU"), (7600, "BACK")) + \
        ",8400:VOL+,8600:VOL-,8900:NONE," + \
        keys((9400, "VOL-"), (9800, "MENU"), (10400, "VOL-"), (10800, "PLAY"), (11400, "BACK"),
             (12000, "MENU", 1500), (14000, "VOL-"), (14400, "VOL-"), (14800, "PLAY"),
             (16000, "BACK"))
    shots = os.environ.get("EMUTEST_SHOTS")
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", card,
                          "--ms", "17000", "--steps", "30000000000", "--dump-disk", dump,
                          "--keys", k] + (["--shots", shots] if shots else []),
                         capture_output=True, text=True).stdout
    shutil.move(dump, disk)
    out += "\n-- reboot\n" + subprocess.run(
        [sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", card, "--boot-disk",
         "--ms", "6000", "--steps", "30000000000", "--dump-disk", dump],
        capture_output=True, text=True).stdout
    text = read_log(dump)
    if os.environ.get("EMUTEST_LOG"):
        open(os.environ["EMUTEST_LOG"], "w").write(text + "\n" + out)
    fails = []
    pos = 0
    for pat in (r"ui: (\d+) tracks of the library's list 8\.",
                r"queue: NEWFWQ\.DAT created",
                r"queue: \d+ tracks written into NEWFWQ\.DAT \(in use\)",
                r"keys: chord vol\+-",
                r"queue: \d+ tracks added at \d+",
                r"queue: \d+ tracks put next at \d+",
                r"=== boot",
                r"ui: resume: NEWFWQ\.DAT's queue, track \d+/\d+",
                r"ui: queue \d+/\d+: .* \(0\)"):
        m = re.compile(pat).search(text, pos)
        if not m:
            fails.append(f"missing: {pat}")
        else:
            pos = m.end()
    return fails, text + "\n" + out


TESTS["queue-file"] = dict(custom=queue_file)


def queue_card(img, elf, tmp):
    """A file queue of the card's tracks (§129): a folder of the card played, made the file's by
    an add in the queue mode; then a boot with the same card (it resumes) and one with another
    card holding the same files at the same clusters (another serial): its tracks passed over,
    never read at their clusters"""
    IMG_FOR_DISK[0] = img
    disk, card, src = lib_disks(tmp)
    card2 = os.path.join(tmp, "card2.img")
    with open(card2, "wb") as f:
        f.truncate(64 << 20)
    subprocess.run(["/usr/sbin/mkfs.exfat", card2], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(TOOLS, "exfat_add.py"), card2,
                    os.path.join(tmp, "spec.json")], check=True, stdout=subprocess.DEVNULL)
    from fatimg import Volume
    fails = [] if Volume(card, 0).serial != Volume(card2, 0).serial else ["the same serial"]
    dump = os.path.join(tmp, "after.img")
    k = keys((2000, "BACK"), (2400, "VOL-"), (2800, "PLAY"), (3400, "PLAY"), (4000, "PLAY"),
             (5000, "MENU")) + ",5600:VOL+,5800:VOL-,6100:NONE," + \
        keys((6600, "VOL-"), (7000, "MENU"), (7600, "BACK"))
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", card,
                          "--ms", "9000", "--steps", "30000000000", "--dump-disk", dump,
                          "--keys", k], capture_output=True, text=True).stdout
    shutil.move(dump, disk)
    first = os.path.join(tmp, "first.img")
    shutil.copy(disk, first)
    text1 = read_log(disk)
    for c, tag in ((card, "same card"), (card2, "another card")):
        shutil.copy(first, disk)
        out += f"\n-- reboot, {tag}\n" + subprocess.run(
            [sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", c, "--boot-disk",
             "--ms", "8000", "--steps", "30000000000", "--dump-disk", dump],
            capture_output=True, text=True).stdout
        text = read_log(dump)
        boot = text[text.rfind("=== boot"):]
        played = re.search(r"ui: queue \d+/\d+: .* \(0\)", boot)
        if "NEWFWQ.DAT's queue" not in boot:
            fails.append(f"{tag}: not resumed from the file")
        elif tag == "same card" and not played:
            fails.append("same card: nothing played")
        elif tag == "another card" and (played or "its card is not in, passed over" not in boot):
            fails.append(f"another card: {played.group(0) if played else 'no track passed over'}")
        text1 += "\n" + boot
    for pat in (r"queue: \d+ tracks of the queue written into NEWFWQ\.DAT",
                r"queue: 1 tracks added"):
        if not re.search(pat, text1):
            fails.append(f"missing: {pat}")
    if os.environ.get("EMUTEST_LOG"):
        open(os.environ["EMUTEST_LOG"], "w").write(text1 + "\n" + out)
    return fails, text1 + "\n" + out


TESTS["queue-card"] = dict(custom=queue_card)


def lib_card(img, elf, tmp):
    """The card not in the index (§129): an index made by mklib.py without the card; the
    library's top has "SD card not in the library" first; PLAY on it updates the library (the
    card's files read), the views then hold the card's tracks; the update compared with
    mklib.py's answers for disk and card"""
    IMG_FOR_DISK[0] = img
    disk, card, _ = lib_disks(tmp)
    dat = os.path.join(tmp, "NEWFWLIB.DAT")
    subprocess.run([sys.executable, os.path.join(TOOLS, "mklib.py"), disk, "-o", dat],
                   check=True, capture_output=True)
    mtools(disk, "mcopy", dat, "::NEWFWLIB.DAT")
    dump = os.path.join(tmp, "after.img")
    k = keys((2000, "BACK"), (2400, "VOL+"), (2800, "PLAY"), (4000, "PLAY"))
    shots = os.environ.get("EMUTEST_SHOTS")
    out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--sd", card,
                          "--ms", "16000", "--steps", "30000000000", "--dump-disk", dump,
                          "--keys", k] + (["--shots", shots] if shots else []),
                         capture_output=True, text=True).stdout
    text = read_log(dump)
    if os.environ.get("EMUTEST_LOG"):
        open(os.environ["EMUTEST_LOG"], "w").write(text + "\n" + out)
    fails = []
    pos = 0
    for pat in (r"library: views of \d+ tracks, the card's none",
                r"library: NEWFWLIB\.DAT found",
                r"library: \d+ tracks \(\d+ read, \d+ from the index before\)",
                r"library: views of \d+ tracks, the card's in"):
        m = re.compile(pat).search(text, pos)
        if not m:
            fails.append(f"missing: {pat}")
        else:
            pos = m.end()
    return fails + lib_compare(dump, disk, card, tmp, "card"), text + "\n" + out


TESTS["lib-card"] = dict(custom=lib_card)


def lib_keep(img, elf, tmp):
    """No card readable at an update (§129): the index made with the card (by mklib.py: no
    fingerprint); boots without the card, "Update the library" (the last line of the library's
    top): the card's tracks kept as they are (first a build from the index before, then with
    the same files "no change"); then a file of the internal memory deleted: an update, the
    card's tracks still in it (the index compared with mklib.py's for the disk and the card);
    the card in again: "no change" (nothing read)"""
    IMG_FOR_DISK[0] = img
    disk, card, _ = lib_disks(tmp)
    dat = os.path.join(tmp, "NEWFWLIB.DAT")
    subprocess.run([sys.executable, os.path.join(TOOLS, "mklib.py"), disk, "--sd", card, "-o",
                    dat], check=True, capture_output=True)
    mtools(disk, "mcopy", dat, "::NEWFWLIB.DAT")
    first = keys((2000, "BACK"), (2400, "VOL+"), (2800, "PLAY"),
                 *[(3300 + 300 * i, "VOL-") for i in range(5)], (5200, "PLAY"))
    again = keys(*[(2000 + 300 * i, "VOL-") for i in range(5)], (4000, "PLAY"))  # (resumed
    dump = os.path.join(tmp, "after.img")                                       # on Artists)
    fails, whole = [], ""
    for step in ("build", "same", "changed", "back"):
        if step != "build":
            shutil.move(dump, disk)
        if step == "changed":
            mtools(disk, "mdel", "::tags/flac-ascii.flac")
        out = subprocess.run([sys.executable, EMU, img, "--elf", elf, "--disk", disk, "--ms",
                              "8000", "--steps", "30000000000", "--dump-disk", dump, "--keys",
                              first if step == "build" else again] +
                             (["--sd", card] if step == "back" else []),
                             capture_output=True, text=True).stdout
        text = read_log(dump)
        boot = text[text.rfind("=== boot"):]
        whole += boot + "\n" + out
        if step == "back":                      # the card in again: its tracks as they were
            if "library: no change" not in boot:
                fails.append("back: the card in again, not \"no change\"")
            continue
        if "not readable: its tracks kept as they are" not in boot:
            fails.append(f"{step}: the card's tracks not kept")
        built = re.search(r"library: (\d+) tracks \(0 read, \d+ from the index before\)", boot)
        if step == "same" and "library: no change" not in boot:
            fails.append("same: not \"no change\"")
        if step != "same" and (not built or "no change" in boot):
            fails.append(f"{step}: not built from the index before")
    if os.environ.get("EMUTEST_LOG"):
        open(os.environ["EMUTEST_LOG"], "w").write(whole)
    return fails + lib_compare(dump, disk, card, tmp, "kept"), whole


TESTS["lib-keep"] = dict(custom=lib_keep)

TESTS.update({
    # built from the tags, compared with mklib.py's answers (content: STRS's order differs)
    "libb-build": dict(custom=lib_build_test("build"), libcreate=True),
    # then one file deleted, one added, one changed: 2 read, 87 from the index before, half B
    "libb-update": dict(custom=lib_build_test("update"), libcreate=True),
    # then the same files again: nothing read nor written (the fingerprint)
    "libb-nochange": dict(custom=lib_build_test("nochange"), libcreate=True),
    # a cut during the second build (early, in the strings, late): the first index intact,
    # the volume clean; the next boot builds it
    **{f"libb-cut{n}": dict(custom=lib_build_test(f"cut{n}"), libcreate=True)
       for n in (3, 12, 30)},
})


def make_sets(names, tmp):
    """The test sets of the tests named, made once (one per length of file, the groups of all
    the tests of that length together), unless EMUTEST_SET gives one: {name: set folder}"""
    if os.environ.get("EMUTEST_SET"):
        return {}
    by_len = {}
    for n in names:
        if "set" in TESTS[n]:
            by_len.setdefault(TESTS[n].get("seconds", SET_SECONDS), set()).update(
                TESTS[n]["set"].split(","))
    dirs = {}
    for sec, groups in by_len.items():
        d = os.path.join(tmp, f"set{sec}")
        subprocess.run([sys.executable, MKSET, d, "--only", ",".join(sorted(groups)),
                        "--seconds", str(sec)], check=True, stdout=subprocess.DEVNULL)
        dirs[sec] = d
    return {n: dirs[TESTS[n].get("seconds", SET_SECONDS)] for n in names if "set" in TESTS[n]}


def run_named(name, img, elf, set_dir):
    t = dict(TESTS[name], set_dir=set_dir) if set_dir else TESTS[name]
    with tempfile.TemporaryDirectory(prefix=f"emutest_{name}_") as tmp:
        return run(name, t, img, elf, tmp)


def main():
    args = sys.argv[1:]
    jobs = min(8, os.cpu_count() or 1)
    if args[:1] == ["-j"] and len(args) > 1:
        jobs, args = max(1, int(args[1])), args[2:]
    if len(args) < 2:
        sys.exit(__doc__)
    img, elf = os.path.abspath(args[0]), os.path.abspath(args[1])
    names = args[2:] or [n for n in TESTS if not TESTS[n].get("fault") and
                         not TESTS[n].get("hwtest") and not TESTS[n].get("libcreate")]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="emutest_sets_") as sets_tmp, \
            concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        sets = make_sets(names, sets_tmp)       # (each test runs its emulator as a process)
        futures = [pool.submit(run_named, n, img, elf, sets.get(n)) for n in names]
        for name, fut in zip(names, futures):
            fails, whole = fut.result()
            print(f"{name}: {'FAIL' if fails else 'PASS'}", flush=True)
            for f in fails:
                print("  " + f[:300])
            if os.environ.get("EMUTEST_DUMP"):
                open(os.environ["EMUTEST_DUMP"], "w").write(whole)
            if fails and os.environ.get("EMUTEST_VERBOSE"):
                print(whole[-4000:])
            failed += bool(fails)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
