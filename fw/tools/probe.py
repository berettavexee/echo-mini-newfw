#!/usr/bin/env python3
"""USB probe client (ETUDE §72): talks to a probe image (make EXTRA=-DECHO_PROBE) through the
mailbox file PROBE.BIN of the device's internal volume, while the device is in USB mode.

The file is opened with O_DIRECT, so that every request goes to the device and every answer
comes from it (no page cache): no driver and no root rights needed.

    probe.py MOUNT info
    probe.py MOUNT peek ADDR [N]          # N 32-bit words (default 1)
    probe.py MOUNT poke ADDR WORD...      # words in hex or decimal
    probe.py MOUNT power-b | run-b | stop-b
    probe.py MOUNT install NAME.IMG       # done when USB mode ends (eject), then a reboot
    probe.py MOUNT create                 # makes PROBE.BIN (then leave and enter USB mode)

As a module: Probe(mount).peek(addr, n), .poke(addr, words), .power_b(), .run_b(), ...
"""
import mmap
import os
import struct
import sys

SECTORS = 8
SIZE = SECTORS * 512
REQ, ANS = 0x31425250, 0x52425250
INFO, PEEK, POKE, POWER_B, RUN_B, STOP_B, INSTALL = range(1, 8)
STATUS = {0: "ok", 1: "bad magic", 2: "unknown command", 3: "address refused", 4: "too long"}
MAX_WORDS = (SIZE - 32) // 4


class ProbeError(Exception):
    pass


class Probe:
    def __init__(self, mount):
        self.path = os.path.join(mount, "PROBE.BIN")
        self.fd = os.open(self.path, os.O_RDWR | os.O_DIRECT | os.O_SYNC)
        self.buf = mmap.mmap(-1, SIZE)          # page aligned, as O_DIRECT wants
        self.seq = struct.unpack("<I", os.urandom(4))[0]

    def close(self):
        os.close(self.fd)

    def call(self, cmd, addr=0, n=0, arg=0, data=b""):
        self.seq = (self.seq + 1) & 0xffffffff
        msg = struct.pack("<8I", REQ, self.seq, cmd, 0, addr, n, arg, 0) + data
        self.buf.seek(0)
        self.buf.write(msg.ljust(SIZE, b"\0"))
        if os.pwritev(self.fd, [self.buf], 0) != SIZE:
            raise ProbeError("short write")
        os.fsync(self.fd)
        if os.preadv(self.fd, [self.buf], 0) != SIZE:
            raise ProbeError("short read")
        magic, seq, rcmd, status, raddr, rn, rarg, count = struct.unpack_from("<8I", self.buf, 0)
        if magic != ANS or seq != self.seq:
            raise ProbeError("no answer (magic %08x, seq %u for %u): is it a probe image, in "
                             "USB mode, with PROBE.BIN made before entering it?" %
                             (magic, seq, self.seq))
        if status:
            raise ProbeError(STATUS.get(status, "status %u" % status))
        return rn, bytes(self.buf[32:32 + 4 * rn])

    def info(self):
        n, d = self.call(INFO)
        return d.split(b"\0")[0].decode()

    def peek(self, addr, n=1):
        out = []
        while n > 0:
            k = min(n, MAX_WORDS)
            rn, d = self.call(PEEK, addr, k)
            out += struct.unpack("<%dI" % rn, d)
            addr += 4 * k
            n -= k
        return out

    def poke(self, addr, words):
        words = list(words)
        while words:
            k = words[:MAX_WORDS]
            self.call(POKE, addr, len(k), data=struct.pack("<%dI" % len(k), *k))
            addr += 4 * len(k)
            words = words[MAX_WORDS:]

    def power_b(self):
        self.call(POWER_B)

    def run_b(self):
        self.call(RUN_B)

    def stop_b(self):
        self.call(STOP_B)

    def install(self, name):
        self.call(INSTALL, data=name.encode()[:12] + b"\0")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    mount, cmd, args = argv[1], argv[2], argv[3:]
    if cmd == "create":
        with open(os.path.join(mount, "PROBE.BIN"), "wb") as f:
            f.write(b"\0" * SIZE)
        os.sync()
        print("PROBE.BIN made: leave USB mode on the device and enter it again")
        return 0
    p = Probe(mount)
    num = lambda s: int(s, 0)
    try:
        if cmd == "info":
            print(p.info())
        elif cmd == "peek":
            addr, n = num(args[0]), num(args[1]) if len(args) > 1 else 1
            for i, w in enumerate(p.peek(addr, n)):
                print("%08x: %08x" % (addr + 4 * i, w))
        elif cmd == "poke":
            p.poke(num(args[0]), [num(a) for a in args[1:]])
        elif cmd in ("power-b", "run-b", "stop-b"):
            getattr(p, cmd.replace("-", "_"))()
        elif cmd == "install":
            p.install(args[0])
            print("install of %s requested: eject the volume (udisksctl unmount/power-off) "
                  "or press BACK; the device installs it and reboots" % args[0])
        else:
            print(__doc__)
            return 1
    except ProbeError as e:
        print("probe:", e)
        return 2
    finally:
        p.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
