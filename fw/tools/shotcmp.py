#!/usr/bin/env python3
"""The same screens from two images, compared pixel for pixel (ETUDE §134: the screen drawn by
DMA against the CPU writing each byte, LCD_NO_DMA).

  shotcmp.py IMG_A ELF_A IMG_B ELF_B [OUTDIR]

A disk with folders and files named in several scripts, the same keys (the top level, a folder,
the menu, the settings, the play screen), screenshots at the same times; exit code 1 when one
differs. $SHOTCMP_EMU: more emu_fw.py options (--no-fast-fill: lcd_fill() run, not drawn by the
emulator at once). Needs mtools, dosfstools and the unicorn module; the disks go to $TMPDIR and are
deleted (the screenshots to OUTDIR, $TMPDIR by default).
"""
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from emutest import keys, wav                                    # noqa: E402

NAMES = ["Été à Paris.wav", "Ελληνικά.wav", "Русский.wav", "東京の夜.wav", "서울의 밤.wav",
         "日本語 ひらがな・カタカナ.wav", "Polski Żółć.wav"]
# the top level; a folder; a track playing (the play screen); the main menu; the play queue;
# the settings; the system screen
KEYS = keys((2500, "VOL-"), (3500, "PLAY"), (5500, "PLAY"), (7000, "MENU", 900),
            (9000, "VOL-"), (9300, "VOL-"), (9600, "PLAY"), (11500, "BACK"), (12000, "MENU", 900),
            (13500, "VOL-"), (13800, "VOL-"), (14100, "VOL-"), (14400, "PLAY"),
            (16500, "BACK"), (17000, "MENU", 900), (18500, "VOL+"), (18800, "VOL+"),
            (19100, "VOL+"), (19400, "PLAY"))
SHOTS = [2400, 5400, 6300, 8500, 11400, 16400, 21000]


def run(img, elf, tmp, out, tag):
    src = os.path.join(tmp, "src")
    if not os.path.isdir(src):
        os.makedirs(src)
        wav(os.path.join(src, "a.wav"), 2)
        open(os.path.join(src, "log"), "wb").write(bytes(256 * 1024))
    disk = os.path.join(tmp, f"{tag}.img")
    files = [f"{os.path.join(src, 'log')}=NEWFWLOG.TXT"]
    files += [f"{os.path.join(src, 'a.wav')}=Musique 音楽/{n}" for n in NAMES]
    subprocess.run([sys.executable, os.path.join(TOOLS, "mkdisk.py"), disk, img, "--synth-idb",
                    "--size-mb", "300"] + files, check=True, stdout=subprocess.DEVNULL)
    shots = ",".join(f"{t}:{os.path.join(out, f'{tag}-{t}.png')}" for t in SHOTS)
    subprocess.run([sys.executable, os.path.join(TOOLS, "emu_fw.py"), img, "--elf", elf, "--disk",
                    disk, "--keys", KEYS, "--ms", str(SHOTS[-1] + 100), "--steps", "30000000000",
                    "--shots", shots] + os.environ.get("SHOTCMP_EMU", "").split(), check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    os.remove(disk)


def main():
    a = sys.argv[1:]
    if len(a) < 4:
        sys.exit(__doc__)
    tmp = tempfile.mkdtemp(prefix="shotcmp_")
    out = a[4] if len(a) > 4 else tmp
    os.makedirs(out, exist_ok=True)
    try:
        run(a[0], a[1], tmp, out, "a")
        run(a[2], a[3], tmp, out, "b")
        bad = 0
        for t in SHOTS:
            pa, pb = (open(os.path.join(out, f"{k}-{t}.png"), "rb").read() for k in "ab")
            same = pa == pb
            bad += not same
            print(f"{'same' if same else 'DIFFERS'}  {t} ms")
        print(f"{len(SHOTS) - bad} of {len(SHOTS)} screens the same ({out})")
        return 1 if bad else 0
    finally:
        shutil.rmtree(os.path.join(tmp, "src"), ignore_errors=True)
        if out != tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
