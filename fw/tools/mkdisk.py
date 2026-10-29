#!/usr/bin/env python3
"""Test disk for tools/emu_fw.py: the boot area of a real device (BOOTDUMP.BIN, read only, never
copied anywhere else), our firmware in FW1, an MBR at 0x81000 and a FAT32 volume (clusters of
4 KiB, as on the device) at 0x81000 + 64 holding the given files.

  mkdisk.py OUT.img FW.IMG [--boot BOOTDUMP.BIN] [--size-mb 300] FILE[=NAME] ...
  (NAME may hold folders, A/B/C.MP3: they are made)
  (--boot defaults to $ECHO_BOOTDUMP, else BOOTDUMP.BIN in the current directory)
  mkdisk.py OUT.img FW.IMG --vol VOLUME.img        (a FAT32 volume made beforehand)
"""
import os
import struct
import subprocess
import sys
import tempfile


def make_volume(t, size, files):
    vol = os.path.join(t, "vol.img")
    with open(vol, "wb") as f:
        f.truncate(size << 20)
    subprocess.run(["/usr/sbin/mkfs.fat", "-F", "32", "-s", "8", "-S", "512", "-n", "ECHOTEST", vol],
                   check=True, stdout=subprocess.DEVNULL)
    made = set()
    for spec in files:
        src, _, name = spec.partition("=")
        name = name or os.path.basename(src)
        parts = name.split("/")[:-1]            # NAME may be in folders: made as needed
        for k in range(1, len(parts) + 1):
            d = "/".join(parts[:k])
            if d not in made:
                made.add(d)
                subprocess.run(["mmd", "-i", vol, "::" + d], check=True)
        subprocess.run(["mcopy", "-i", vol, src, "::" + name], check=True)
    return open(vol, "rb").read()


def main():
    a = sys.argv[1:]
    boot = os.environ.get("ECHO_BOOTDUMP", "BOOTDUMP.BIN")   # a dump of the device's boot area
    size = 300
    if "--boot" in a:
        i = a.index("--boot"); boot = a[i + 1]; del a[i:i + 2]
    if "--size-mb" in a:
        i = a.index("--size-mb"); size = int(a[i + 1]); del a[i:i + 2]
    volfile = None
    if "--vol" in a:
        i = a.index("--vol"); volfile = a[i + 1]; del a[i:i + 2]
    out, fw, files = a[0], a[1], a[2:]
    with tempfile.TemporaryDirectory() as t:
        if volfile:
            v = open(volfile, "rb").read()
            files = [f"(volume {volfile})"]
        else:
            v = make_volume(t, size, files)
    mbr = bytearray(512)
    mbr[446:462] = bytes([0, 0, 0, 0, 0x0c, 0, 0, 0]) + struct.pack("<II", 64, len(v) // 512)
    mbr[510:512] = b"\x55\xaa"
    with open(out, "wb") as f:
        f.write(open(boot, "rb").read())
        f.seek(0x2000 * 512)
        f.write(open(fw, "rb").read())
        f.seek(0x81000 * 512)
        f.write(mbr)
        f.seek((0x81000 + 64) * 512)
        f.write(v)
    print(f"{out}: {os.path.getsize(out) // 512} sectors, files: {', '.join(files) or '-'}")


if __name__ == "__main__":
    main()
