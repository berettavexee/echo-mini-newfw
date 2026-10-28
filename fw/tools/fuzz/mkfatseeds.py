#!/usr/bin/env python3
"""Seeds for the fat target of fuzz_host.c (docs/FUZZING.md): real FAT32 and exFAT volumes made
with mkfs.fat and mtools, mkfs.exfat and ../exfat_add.py, then kept as sparse sectors (the
records fuzz_host.c reads: a 32-bit sector number, then the 512 bytes; zero sectors left out).

  mkfatseeds.py OUTDIR

Each volume holds folders three deep, long names (accents, CJK, over 13 characters), a folder of
40 entries, empty, small and multi-cluster files, a fragmented one, a hidden one, NEWFWLOG.TXT;
alone (sector 0) or behind an MBR (partition at sector 2048), with clusters of 512 bytes and
of 4 KiB (as on the device); exFAT with clusters of 4 and 32 KiB (exfat_add.py does not grow a
folder past one cluster). Synthetic content only.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXFAT_ADD = os.path.join(HERE, "..", "exfat_add.py")
PART = 2048                     # partition start behind an MBR


def data(n, seed):
    return bytes((seed + i * 7) & 0xff or 1 for i in range(n))


def run(*cmd, env=None):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)


def tree(t):
    """The files of every volume: (path, bytes or None for a folder)"""
    os.makedirs(t, exist_ok=True)
    items = [("Music", None), ("Music/Artiste é", None), ("Music/Artiste é/Album 日本語 long name", None),
             ("Music/Artiste é/Album 日本語 long name/01 Première piste.flac", data(3000, 1)),
             ("Music/Artiste é/Album 日本語 long name/02 two.ogg", data(9000, 2)),
             ("Music/empty.mp3", b""), ("Many", None), ("NEWFWLOG.TXT", b"NEWFWLOG" + bytes(8184)),
             ("a.txt", b"short"), ("BIG.WAV", data(20000, 3))]
    items += [(f"Many/track {i:02} with a long name.opus", data(100 + i, i)) for i in range(40)]
    return items


def fat32(img, spc, size_mb):
    with open(img, "wb") as f:
        f.truncate(size_mb << 20)
    run("/usr/sbin/mkfs.fat", "-F", "32", "-s", str(spc), "-S", "512", "-n", "FUZZ", img)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="C.UTF-8")
    with tempfile.TemporaryDirectory() as t:
        frag = os.path.join(t, "frag")
        for path, d in tree(t):
            if d is None:
                run("mmd", "-i", img, "::" + path, env=env)
                continue
            src = os.path.join(t, "f")
            open(src, "wb").write(d)
            run("mcopy", "-i", img, src, "::" + path, env=env)
            if path == "a.txt":                 # a gap, so that the next big file is split
                open(frag, "wb").write(data(5000, 9))
                run("mcopy", "-i", img, frag, "::gap.bin", env=env)
        open(frag, "wb").write(data(12000, 5))
        run("mdel", "-i", img, "::gap.bin", env=env)
        run("mcopy", "-i", img, frag, "::Fragmented file.flac", env=env)
        run("mattrib", "-i", img, "+h", "::a.txt", env=env)


def exfat(img, size_mb, cluster):
    with open(img, "wb") as f:
        f.truncate(size_mb << 20)
    run("/usr/sbin/mkfs.exfat", "-c", str(cluster), "-L", "FUZZ", img)
    spec, n = [], 0
    with tempfile.TemporaryDirectory() as t:
        for path, d in tree(t):
            if d is None:
                # "Many": 40 entries of 4 slots (160 x 32 bytes), over one cluster at 4 KiB
                spec.append({"path": path, "dir": True,
                             "clusters": -(-160 * 32 // int(cluster[:-1]) // 1024) if path == "Many" else 1})
            else:
                spec.append({"path": path, "data": d.hex(), "fragmented": path == "BIG.WAV",
                             "hidden": path == "a.txt"})
                n += 1
        js = os.path.join(t, "spec.json")
        json.dump(spec, open(js, "w"))
        run(sys.executable, EXFAT_ADD, img, js)


def sparse(img, out, mbr_type=None):
    """img as records; with mbr_type, behind an MBR whose one partition starts at PART"""
    recs = []
    if mbr_type is not None:
        size = os.path.getsize(img) // 512
        mbr = bytearray(512)
        mbr[446:462] = bytes([0, 0, 0, 0, mbr_type, 0, 0, 0]) + struct.pack("<II", PART, size)
        mbr[510:512] = b"\x55\xaa"
        recs.append(struct.pack("<I", 0) + bytes(mbr))
    off = PART if mbr_type is not None else 0
    with open(img, "rb") as f:
        s = 0
        while True:
            b = f.read(512)
            if len(b) < 512:
                break
            if any(b):
                recs.append(struct.pack("<I", off + s) + b)
            s += 1
    open(out, "wb").write(b"".join(recs))
    return len(recs)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        img = os.path.join(t, "v.img")
        for name, make, mbr in [
                ("fat32_s1", lambda: fat32(img, 1, 34), None),
                ("fat32_s1_mbr", lambda: fat32(img, 1, 34), 0x0c),
                ("fat32_s8_mbr", lambda: fat32(img, 8, 260), 0x0c),
                ("exfat_4k", lambda: exfat(img, 16, "4K"), None),
                ("exfat_32k", lambda: exfat(img, 64, "32K"), None),
                ("exfat_4k_mbr", lambda: exfat(img, 64, "4K"), 0x07)]:
            make()
            n = sparse(img, os.path.join(out, name), mbr)
            print(f"{name}: {n} sectors")
            os.remove(img)


if __name__ == "__main__":
    main()
