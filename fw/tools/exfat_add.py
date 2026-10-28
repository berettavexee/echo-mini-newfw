#!/usr/bin/env python3
"""Adds directories and files to an exFAT volume image made by mkfs.exfat (test images for the
microSD support, ETUDE §37). Written from Microsoft's public exFAT specification.

  exfat_add.py VOL.img SPEC.json

SPEC: a list of entries {"path": "Dir/Sub/name.ext", "dir": true} or {"path": ..., "src": FILE}
or {"path": ..., "data": "hex"}, optional "fragmented": true (clusters spread, FAT chain) and
"hidden": true. Files are contiguous with the "no FAT chain" flag unless fragmented.
"""
import json, struct, sys


class Vol:
    def __init__(self, path):
        self.f = open(path, "r+b")
        b = self.rd(0, 512)
        assert b[3:11] == b"EXFAT   "
        self.fat_off, self.fat_len, self.heap, self.count, self.root = struct.unpack_from("<5I", b, 80)
        self.bps = 1 << b[108]
        self.cs = self.bps << b[109]
        self.bitmap = self.bitmap_cluster = None
        for off, e in self.entries(self.root, None):
            if e[0] == 0x81:
                self.bitmap_cluster, ln = struct.unpack_from("<IQ", e, 20)
                self.bitmap = bytearray(self.read_chain(self.bitmap_cluster, None)[:ln])
        assert self.bitmap is not None
        self.dirs = {"": (self.root, None)}            # path: (first cluster, contiguous size)

    def rd(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def wr(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def coff(self, c):
        return self.heap * self.bps + (c - 2) * self.cs

    def fat(self, c, v=None):
        o = self.fat_off * self.bps + 4 * c
        if v is None:
            return struct.unpack("<I", self.rd(o, 4))[0]
        self.wr(o, struct.pack("<I", v))

    def chain(self, c, contig_size):
        if contig_size is not None:
            return [c + i for i in range(max(1, -(-contig_size // self.cs)))]
        out = []
        while 2 <= c < 0xFFFFFFF7:
            out.append(c)
            c = self.fat(c)
        return out

    def read_chain(self, c, contig_size):
        return b"".join(self.rd(self.coff(x), self.cs) for x in self.chain(c, contig_size))

    def entries(self, c, contig_size):
        for x in self.chain(c, contig_size):
            blk = self.rd(self.coff(x), self.cs)
            for i in range(0, self.cs, 32):
                yield self.coff(x) + i, blk[i:i + 32]

    def used(self, c):
        return self.bitmap[(c - 2) // 8] >> ((c - 2) % 8) & 1

    def mark(self, c):
        self.bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)

    def alloc(self, n, fragmented):
        got, c = [], 2
        while len(got) < n:
            if not fragmented and got and c != got[-1] + 1:
                got = []
            if not self.used(c):
                got.append(c)
                c += 3 if fragmented else 1               # leave holes: a fragmented file
            else:
                c += 1
            assert c < self.count + 2, "volume full"
        for x in got:
            self.mark(x)
        if fragmented:
            for a, b in zip(got, got[1:] + [0xFFFFFFFF]):
                self.fat(a, b)
        return got

    def flush(self):
        data = bytes(self.bitmap)
        for i, x in enumerate(self.chain(self.bitmap_cluster, None)):
            self.wr(self.coff(x), data[i * self.cs:(i + 1) * self.cs].ljust(self.cs, b"\0"))

    def add(self, parent, name, attr, first, size, contig, hidden):
        u = name.encode("utf-16-le")
        units = len(u) // 2
        h = 0
        for byte in name.upper().encode("utf-16-le"):
            h = ((h >> 1) | (h & 1) << 15) + byte & 0xFFFF
        nname = -(-units // 15)
        ts = 0x5B570000 | 12 << 11                       # 2025-10-23 12:00
        e = [bytearray(32) for _ in range(2 + nname)]
        e[0][0], e[0][1] = 0x85, 1 + nname
        struct.pack_into("<HHIII", e[0], 4, attr | (2 if hidden else 0), 0, ts, ts, ts)
        e[1][0], e[1][1], e[1][3] = 0xC0, 1 | (2 if contig else 0), units
        struct.pack_into("<HHQII Q", e[1], 4, h, 0, size, 0, first, size)
        for i in range(nname):
            e[2 + i][0] = 0xC1
            e[2 + i][2:2 + len(u[30 * i:30 * i + 30])] = u[30 * i:30 * i + 30]
        raw = b"".join(e)
        ck = 0
        for i, byte in enumerate(raw):
            if i not in (2, 3):
                ck = ((ck >> 1) | (ck & 1) << 15) + byte & 0xFFFF
        struct.pack_into("<H", e[0], 2, ck)
        raw = b"".join(e)
        pc, psize = self.dirs[parent]
        free = []
        for off, ent in self.entries(pc, psize):
            if ent[0] < 0x80:
                free.append(off)
                if len(free) == len(e):
                    break
            else:
                free = []
        assert len(free) == len(e), f"directory {parent!r} full"
        for i, off in enumerate(free):
            self.wr(off, raw[32 * i:32 * i + 32])


def main():
    v = Vol(sys.argv[1])
    for item in json.load(open(sys.argv[2])):
        path = item["path"]
        parent, _, name = path.rpartition("/")
        if item.get("dir"):
            n = item.get("clusters", 1)
            cl = v.alloc(n, False)
            for x in cl:
                v.wr(v.coff(x), bytes(v.cs))
            v.add(parent, name, 0x10, cl[0], n * v.cs, True, item.get("hidden"))
            v.dirs[path] = (cl[0], n * v.cs)
            continue
        data = open(item["src"], "rb").read() if "src" in item else bytes.fromhex(item.get("data", ""))
        frag = item.get("fragmented", False)
        first = 0
        if data:
            cl = v.alloc(-(-len(data) // v.cs), frag)
            for i, x in enumerate(cl):
                v.wr(v.coff(x), data[i * v.cs:(i + 1) * v.cs].ljust(v.cs, b"\0"))
            first = cl[0]
        v.add(parent, name, 0x20, first, len(data), not frag and bool(data), item.get("hidden"))
    v.flush()


if __name__ == "__main__":
    main()
