#!/usr/bin/env python3
"""A FAT32 and exFAT reader for disk images, as the firmware lists volumes (fw/src/fat.c): long
names in UTF-8 cut to 127 bytes on a character boundary, hidden and system entries skipped, names
starting with a dot skipped, files of 4 GiB or more skipped (exFAT). Used by mklib.py.

  fatimg.py IMAGE [SECTOR]      lists the files (path, cluster, size, mtime)

SECTOR: where the volume is (a boot sector, or an MBR whose first partition is taken, relative to
it); 0 by default.
"""
import struct
import sys


def utf8_cut(s, n=127):
    """s in UTF-8, whole characters within n bytes"""
    b = s.encode("utf-8", "surrogatepass")
    k = min(len(b), n)
    while 0 < k < len(b) and (b[k] & 0xc0) == 0x80:
        k -= 1                                  # the character cut by the limit: dropped
    return b[:k].decode("utf-8", "surrogatepass")


def utf16_name(units):
    out = []
    i = 0
    while i < len(units) and units[i]:
        u = units[i]
        if 0xd800 <= u < 0xdc00 and i + 1 < len(units) and 0xdc00 <= units[i + 1] < 0xe000:
            out.append(chr(0x10000 + ((u - 0xd800) << 10) + (units[i + 1] - 0xdc00)))
            i += 2
            continue
        out.append(chr(u) if not 0xd800 <= u < 0xe000 else "�")
        i += 1
    return utf8_cut("".join(out))


class Entry:
    __slots__ = ("name", "attr", "cluster", "size", "mtime", "contig", "dir_size")

    def __init__(self, name, attr, cluster, size, mtime, contig=0, dir_size=0):
        self.name, self.attr, self.cluster, self.size = name, attr, cluster, size
        self.mtime, self.contig, self.dir_size = mtime, contig, dir_size

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)


class Volume:
    def __init__(self, path, sector=0):
        self.f = open(path, "rb")
        bs = self.sector(sector)
        if bs[3:11] != b"EXFAT   " and bs[0x52:0x57] != b"FAT32":
            lba = struct.unpack_from("<I", bs, 446 + 8)[0]     # an MBR: its first partition
            sector += lba
            bs = self.sector(sector)
        self.base = sector
        if bs[3:11] == b"EXFAT   ":
            self.exfat = True
            self.serial = struct.unpack_from("<I", bs, 100)[0]
            self.fat_lba = sector + struct.unpack_from("<I", bs, 80)[0]
            self.heap = sector + struct.unpack_from("<I", bs, 88)[0]
            self.root = struct.unpack_from("<I", bs, 96)[0]
            self.spc = 1 << bs[109]
        elif bs[0x52:0x57] == b"FAT32":
            self.exfat = False
            self.serial = struct.unpack_from("<I", bs, 67)[0]
            reserved, nfats = struct.unpack_from("<H", bs, 14)[0], bs[16]
            fatsz = struct.unpack_from("<I", bs, 36)[0]
            self.fat_lba = sector + reserved
            self.heap = self.fat_lba + nfats * fatsz
            self.root = struct.unpack_from("<I", bs, 44)[0]
            self.spc = bs[13]
        else:
            raise ValueError(f"{path}: no FAT32 or exFAT volume at sector {sector}")

    def sector(self, s, n=1):
        self.f.seek(512 * s)
        return self.f.read(512 * n)

    def next_cluster(self, c):
        self.f.seek(512 * self.fat_lba + 4 * c)
        v = struct.unpack("<I", self.f.read(4))[0]
        return v if self.exfat else v & 0x0fffffff

    def chain(self, c, contig=0, size=0):
        """The clusters of a file or directory"""
        if contig:
            for k in range((size + 512 * self.spc - 1) // (512 * self.spc)):
                yield c + k
            return
        guard = 0
        while 2 <= c < 0x0ffffff7 and guard < 1 << 22:
            yield c
            c = self.next_cluster(c)
            guard += 1

    def read(self, e):
        """The bytes of a file"""
        out, left = bytearray(), e.size
        for c in self.chain(e.cluster, e.contig, e.size):
            if left <= 0:
                break
            b = self.sector(self.heap + (c - 2) * self.spc, self.spc)
            out += b[:left]
            left -= len(b)
        return bytes(out)

    def list(self, d=None):
        """The entries of a directory (None: the root)"""
        if self.exfat:
            return self._exfat_list(d)
        return self._fat32_list(self.root if d is None else d.cluster)

    def _dir_bytes(self, cluster, contig=0, size=0):
        for c in self.chain(cluster, contig, size):
            yield from (self.sector(self.heap + (c - 2) * self.spc + k) for k in range(self.spc))

    def _fat32_list(self, cluster):
        pos = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
        lfn, lfn_seq, lfn_ck, lfn_len, out = [], 0, 0, 0, []
        for sec in self._dir_bytes(cluster):
            for i in range(16):
                e = sec[32 * i:32 * i + 32]
                if e[0] == 0:
                    return out
                if e[0] == 0xe5:
                    lfn_seq = 0
                    continue
                if e[11] == 0x0f:
                    seq = e[0] & 0x1f
                    if e[0] & 0x40:
                        if not 1 <= seq <= 20:
                            lfn_seq = 0
                            continue
                        lfn_len, lfn_ck = seq * 13, e[13]
                        lfn = [0] * (lfn_len + 1)
                    elif seq != lfn_seq - 1 or e[13] != lfn_ck:
                        lfn_seq = 0
                        continue
                    lfn_seq = seq
                    for k in range(13):
                        u = struct.unpack_from("<H", e, pos[k])[0]
                        lfn[(seq - 1) * 13 + k] = 0 if u == 0xffff else u
                    continue
                ck = 0
                for b in e[:11]:
                    ck = (((ck & 1) << 7) | (ck >> 1)) + b & 0xff
                have_lfn = lfn_seq == 1 and ck == lfn_ck
                lfn_seq = 0
                if e[11] & 0x0e or e[0] == ord("."):
                    continue
                base = bytes([0xe5 if k == 0 and e[k] == 5 else e[k] for k in range(8)]).rstrip(b" ")
                ext = e[8:11].rstrip(b" ")
                if have_lfn:
                    name = utf16_name(lfn[:lfn_len])
                else:
                    name = (base.lower() if e[12] & 0x08 else base).decode("latin-1")
                    if ext:
                        name += "." + (ext.lower() if e[12] & 0x10 else ext).decode("latin-1")
                if name.startswith("."):
                    continue
                cl = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
                mt = struct.unpack_from("<H", e, 24)[0] << 16 | struct.unpack_from("<H", e, 22)[0]
                out.append(Entry(name, e[11], cl, struct.unpack_from("<I", e, 28)[0], mt))
        return out

    def _exfat_list(self, d):
        cluster, contig, size = (self.root, 0, 0) if d is None else (d.cluster, d.contig, d.dir_size)
        out, left, st = [], 0, None
        for sec in self._dir_bytes(cluster, contig, size):
            for i in range(16):
                e = sec[32 * i:32 * i + 32]
                if e[0] == 0:
                    return out
                if not e[0] & 0x80:
                    left = 0
                    continue
                if e[0] == 0x85:
                    left = e[1] if 2 <= e[1] <= 18 else 0
                    st = dict(attr=struct.unpack_from("<H", e, 4)[0],
                              mtime=struct.unpack_from("<I", e, 12)[0], units=[], stream=0)
                    continue
                if not left:
                    continue
                left -= 1
                if e[0] == 0xc0 and not st["stream"]:
                    a = st["attr"]
                    st.update(stream=1, flags=e[1], nlen=e[3],
                              first=struct.unpack_from("<I", e, 20)[0],
                              length=struct.unpack_from("<Q", e, 24 if a & 0x10 else 8)[0],
                              alloc=struct.unpack_from("<Q", e, 24)[0])
                elif e[0] == 0xc1 and st["stream"]:
                    for k in range(15):
                        if len(st["units"]) < st["nlen"]:
                            st["units"].append(struct.unpack_from("<H", e, 2 + 2 * k)[0])
                if left or not st["stream"] or len(st["units"]) != st["nlen"] or not st["nlen"]:
                    continue
                if st["attr"] & 0x06 or st["length"] >> 32:
                    continue
                name = utf16_name(st["units"])
                if name.startswith("."):
                    continue
                out.append(Entry(name, st["attr"], st["first"], st["length"], st["mtime"],
                                 1 if st["flags"] & 2 else 0, st["alloc"]))
        return out

    def walk(self, d=None, path=""):
        """(path, entry) of every file, depth first"""
        for e in self.list(d):
            p = f"{path}/{e.name}"
            if e.is_dir:
                yield from self.walk(e, p)
            else:
                yield p, e


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    v = Volume(sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 0)
    for p, e in v.walk():
        print(f"{p}\t{e.cluster}\t{e.size}\t{e.mtime:#010x}{' contig' if e.contig else ''}")


if __name__ == "__main__":
    main()
