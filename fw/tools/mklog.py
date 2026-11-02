#!/usr/bin/env python3
"""Creates NEWFWLOG.TXT at the root of the mounted player (once, on the PC).

  mklog.py [MOUNT_POINT] [--size-kib N]

The firmware never creates files: it only rewrites, in place, the sectors of this file, which keeps
the resume state and the settings (language, brightness, ...) and the log. It must be contiguous
(not fragmented) and 8 KiB to 16 MiB. Default: 1024 KiB, written in one go on a fresh file.
"""
import argparse
import os
import shutil
import subprocess
import sys

NAME = "NEWFWLOG.TXT"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mount", nargs="?", default=".", help="mount point of the player (default: .)")
    ap.add_argument("--size-kib", type=int, default=1024, help="8..16384 (default 1024)")
    ap.add_argument("--force", action="store_true", help="replace an existing file")
    a = ap.parse_args()

    if not 8 <= a.size_kib <= 16384:
        sys.exit("size must be 8..16384 KiB")
    if not os.path.isdir(a.mount):
        sys.exit(f"{a.mount}: not a directory")
    path = os.path.join(a.mount, NAME)
    if os.path.exists(path) and not a.force:
        sys.exit(f"{path} already exists (use --force to replace it)")
    if os.path.exists(path):
        os.remove(path)

    size = a.size_kib * 1024
    with open(path, "wb") as f:                 # spaces: the log reader shows nothing
        chunk = b" " * 65536
        for _ in range(size // len(chunk)):
            f.write(chunk)
        f.write(b" " * (size % len(chunk)))
        f.flush()
        os.fsync(f.fileno())
    if os.path.getsize(path) != size:
        sys.exit("the written size is wrong; the volume may be full")

    if shutil.which("filefrag"):                # only a check: the volume may not support it
        out = subprocess.run(["filefrag", path], capture_output=True, text=True).stdout.strip()
        if out and " 1 extent" not in out:
            print(f"warning: the file may be fragmented ({out}).\n"
                  "Free some space or delete the file, then run it again on an emptier volume.")
    print(f"created {path} ({a.size_kib} KiB). Unmount safely, then restart the player.")


if __name__ == "__main__":
    main()
