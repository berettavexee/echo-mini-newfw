#!/usr/bin/env python3
"""A test set made from one lossless excerpt (testdata/goldberg: CC0, so the files can be shared):
every format and rate we play, then variations of the tags and of the cover art, for the
tag reader of the main core (tags.c) and the decoders of core B.

  mktestset.py OUTDIR [--src FILE.flac] [--seconds N] [--only GROUP,...]

Groups (one folder each in OUTDIR):
  formats/     each container and codec at several rates and depths, with the same plain tags
  tags/        the same tag sets (ascii, unicode, long, title only, none, ReplayGain) in each
               format that carries tags
  id3/         MP3: ID3v2.2, 2.3, 2.4, their text encodings, extended header,
               unsynchronisation, ID3v1 alone and as a fallback, big padding
  containers/  WAV: LIST INFO before / after the samples, an id3 chunk; AIFF: NAME/AUTH, an ID3
               chunk; M4A: moov before / after mdat
  covers/      baseline JPEG pictures of 300x300, 600x600 and 1200x1200 (the usual ones), and PNG
               noise of 1 MB, 8 MB and 17 MB (past the 16 MiB tags.c reads at most), each
               put before the text tags, in FLAC (not 17 MB: a FLAC block holds less than
               16 MiB), MP3, Ogg Vorbis, Opus and M4A

OUTDIR/manifest.json lists each file with its format and the tags it holds ("expect": "tags"
when tags.c should show them, "empty" when, by design, it should not, "any" when only the
absence of a fault matters).
Needs sox, flac, lame, oggenc, opusenc, ffmpeg and the mutagen module (Ogg and MP4 tags);
mpcenc too for Musepack (skipped when missing). The tags of FLAC, MP3, WAV and AIFF are
written here byte by byte, so that the order of the fields is ours.
"""
import argparse
import base64
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "testdata", "goldberg", "variatio4-5s.flac")

# ---- tag sets ----
ASCII = dict(title="Variatio 4", artist="Kimiko Ishizaka", album="Open Goldberg Variations",
             track="5")
TAGSETS = {
    "ascii": ASCII,
    "unicode": dict(title="Variation 4 — à 1 clavier · 第4変奏 "
                    "· Вариация 4 · "
                    "Παραλλαγή 4 \U0001f3b9",
                    artist="石坂紀美子 (Kimiko Ishizaka)",
                    album="Goldberg-Variationen, BWV 988 – Öffnung", track="5/32"),
    # past the 96 / 128 bytes of struct tags, cut inside a multi-byte character
    "long": dict(title="Variatio 4 a 1 Clav. " * 12, artist="é" * 150,
                 album="日本" * 60, track="5"),
    "title-only": dict(title="Variatio 4 (title only)"),
    "none": {},
    "rg": dict(ASCII, rg=dict(REPLAYGAIN_TRACK_GAIN="-6.50 dB", REPLAYGAIN_TRACK_PEAK="0.988525",
                              REPLAYGAIN_ALBUM_GAIN="-7.10 dB",
                              REPLAYGAIN_ALBUM_PEAK="0.999969")),
}
LATIN1 = dict(title="Variation 4 à 1 clavier", artist="Kimiko Ishizaka",
              album="Goldberg-Variationen Öffnung", track="5")
COVERS = {"jpg300": ("jpg", 300), "jpg600": ("jpg", 600), "jpg1200": ("jpg", 1200),
          "1m": ("png", 600), "8m": ("png", 1700), "17m": ("png", 2440)}


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def have(tool):
    return shutil.which(tool) is not None


# ---- audio ----
def pcm(src, dst, rate, bits, seconds):
    """A WAV or AIFF (by the extension) at rate / bits, dithered (repeatably) to 16 bits"""
    run("sox", "-R", src, "-b", str(bits), dst, "trim", "0", str(seconds), "rate", "-v",
        str(rate), *(["dither", "-s"] if bits == 16 else []))


def encode(kind, src, dst, rate, bits, seconds, tmp):
    w = os.path.join(tmp, f"in_{rate}_{bits}.wav")
    if not os.path.exists(w):
        pcm(src, w, rate, bits, seconds)
    if kind in ("wav", "aiff"):
        pcm(src, dst, rate, bits, seconds)
    elif kind == "flac":
        run("flac", "-8", "-s", "-f", "--no-padding", "-o", dst, w)
    elif kind == "mp3":
        run("lame", "--silent", "--noreplaygain", *(["-b", "320"] if rate == 44100 else
                                                    ["-V", "2"]), w, dst)
    elif kind == "vorbis":
        run("oggenc", "-Q", "-q", "6", "-s", "1", "-o", dst, w)
    elif kind == "opus":
        run("opusenc", "--quiet", "--bitrate", "128", "--serial", "1", w, dst)
    elif kind in ("aac", "alac"):
        run("ffmpeg", "-nostdin", "-y", "-i", w, "-map_metadata", "-1", "-fflags", "+bitexact",
            *(["-c:a", "aac", "-b:a", "256k"] if kind == "aac" else
              ["-c:a", "alac", "-sample_fmt", "s16p" if bits == 16 else "s32p"]), dst)
    elif kind == "mpc":
        run("mpcenc", "--silent", "--overwrite", w, dst)
    else:
        raise ValueError(kind)


# ---- ID3 ----
def syncsafe(n):
    return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])


def id3_text(s, enc):
    return bytes([enc]) + {0: lambda: s.encode("latin-1"), 1: lambda: s.encode("utf-16"),
                           2: lambda: s.encode("utf-16-be"), 3: lambda: s.encode("utf-8")}[enc]()


def id3_frames(t, ver, enc, pic=None):
    ids = dict(title="TIT2", artist="TPE1", album="TALB", track="TRCK") if ver > 2 else \
        dict(title="TT2", artist="TP1", album="TAL", track="TRK")
    out = []
    if pic:                                     # first: the fields after it test the skip
        data, fmt = pic
        if ver == 2:
            body = bytes([0]) + (b"JPG" if fmt == "jpg" else b"PNG") + b"\x03\x00" + data
        else:
            body = bytes([0]) + f"image/{'jpeg' if fmt == 'jpg' else 'png'}".encode() + \
                b"\x00\x03\x00" + data
        out.append(("PIC" if ver == 2 else "APIC", body))
    for k in ("title", "artist", "album", "track"):
        if k in t:
            out.append((ids[k], id3_text(t[k], enc)))
    for k, v in t.get("rg", {}).items():
        out.append(("TXX" if ver == 2 else "TXXX", id3_text(k, enc) +
                    (b"\0\0" if enc in (1, 2) else b"\0") + id3_text(v, enc)[1:]))
    return out


def unsync(b):
    out, prev = bytearray(), 0
    for c in b:
        if prev == 0xff and (c >= 0xe0 or c == 0):
            out.append(0)
        out.append(c)
        prev = c
    if prev == 0xff:
        out.append(0)
    return bytes(out)


def id3v2(frames, ver, ext=False, unsynced=False, pad=1024):
    body = b""
    if ext:
        body += b"\x00\x00\x00\x06\x01\x00" if ver == 4 else b"\x00\x00\x00\x06" + bytes(6)
    for fid, data in frames:
        n = len(data)
        size = n.to_bytes(3, "big") if ver == 2 else syncsafe(n) if ver == 4 else \
            struct.pack(">I", n)
        body += fid.encode() + size + (b"" if ver == 2 else b"\0\0") + data
    if unsynced:
        body = unsync(body)
    body += bytes(pad)
    flags = (0x80 if unsynced else 0) | (0x40 if ext else 0)
    return b"ID3" + bytes([ver, 0, flags]) + syncsafe(len(body)) + body


def id3v1(t):
    f = lambda s, n: (s or "").encode("latin-1", "replace")[:n].ljust(n, b"\0")  # noqa: E731
    tr = int((t.get("track") or "0").split("/")[0])
    return b"TAG" + f(t.get("title"), 30) + f(t.get("artist"), 30) + f(t.get("album"), 30) + \
        b"2012" + bytes(28) + b"\0" + bytes([tr]) + b"\xff"


# ---- FLAC metadata, block by block ----
def flac_blocks(path):
    d = open(path, "rb").read()
    assert d[:4] == b"fLaC"
    pos, blocks = 4, []
    while True:
        h = d[pos]
        n = int.from_bytes(d[pos + 1:pos + 4], "big")
        blocks.append((h & 0x7f, d[pos + 4:pos + 4 + n]))
        pos += 4 + n
        if h & 0x80:
            return blocks, d[pos:]


def vorbis_comment(t, vendor=b"mktestset"):
    items = [f"{k.upper()}={t[k]}" for k in ("title", "artist", "album") if k in t]
    if "track" in t:
        items.append(f"TRACKNUMBER={t['track']}")
    items += [f"{k}={v}" for k, v in t.get("rg", {}).items()]
    b = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(items))
    for s in items:
        s = s.encode()
        b += struct.pack("<I", len(s)) + s
    return b


def flac_picture(data, fmt, side):
    mime = f"image/{'jpeg' if fmt == 'jpg' else 'png'}".encode()
    return struct.pack(">II", 3, len(mime)) + mime + struct.pack(">I", 0) + \
        struct.pack(">IIIII", side, side, 24, 0, len(data)) + data


def flac_tag(path, t, pic=None):
    blocks, frames = flac_blocks(path)
    keep = [b for b in blocks if b[0] in (0, 3)]       # STREAMINFO, SEEKTABLE
    if pic:
        keep.append((6, flac_picture(*pic)))
    if t:
        keep.append((4, vorbis_comment(t)))
    keep.append((1, bytes(4096)))
    out = b"fLaC"
    for i, (typ, body) in enumerate(keep):
        out += bytes([typ | (0x80 if i == len(keep) - 1 else 0)]) + len(body).to_bytes(3, "big")
        out += body
    open(path, "wb").write(out + frames)


# ---- Ogg and MP4, through mutagen ----
def ogg_tag(path, kind, t, pic=None):
    from mutagen.flac import Picture
    from mutagen.oggopus import OggOpus
    from mutagen.oggvorbis import OggVorbis
    f = (OggOpus if kind == "opus" else OggVorbis)(path)
    f.tags.clear()
    if pic:
        p = Picture()
        p.type, p.mime = 3, f"image/{'jpeg' if pic[1] == 'jpg' else 'png'}"
        p.width = p.height = pic[2]
        p.depth, p.data = 24, pic[0]
        f.tags.append(("METADATA_BLOCK_PICTURE", base64.b64encode(p.write()).decode()))
    for k in ("title", "artist", "album"):
        if k in t:
            f.tags.append((k.upper(), t[k]))
    if "track" in t:
        f.tags.append(("TRACKNUMBER", t["track"]))
    for k, v in t.get("rg", {}).items():
        f.tags.append((k, v))
    f.save()


def mp4_tag(path, t, pic=None):
    from mutagen.mp4 import MP4, MP4Cover, MP4FreeForm
    f = MP4(path)
    f.delete()
    f = MP4(path)
    if pic:
        f["covr"] = [MP4Cover(pic[0], MP4Cover.FORMAT_JPEG if pic[1] == "jpg" else
                              MP4Cover.FORMAT_PNG)]
    for k, a in (("title", "\xa9nam"), ("artist", "\xa9ART"), ("album", "\xa9alb")):
        if k in t:
            f[a] = [t[k]]
    if "track" in t:
        n, _, of = t["track"].partition("/")
        f["trkn"] = [(int(n), int(of or 0))]
    for k, v in t.get("rg", {}).items():
        f["----:com.apple.iTunes:" + k.lower()] = [MP4FreeForm(v.encode())]
    f.save()
    if pic:                                     # mutagen puts covr last: first, as for the others
        d = bytearray(open(path, "rb").read())
        i = d.find(b"ilst") - 4
        end, pos, items = i + struct.unpack_from(">I", d, i)[0], i + 8, []
        while pos < end:
            n = struct.unpack_from(">I", d, pos)[0]
            items.append(bytes(d[pos:pos + n]))
            pos += n
        items.sort(key=lambda b: b[4:8] != b"covr")
        d[i + 8:end] = b"".join(items)
        open(path, "wb").write(d)


# ---- WAV and AIFF chunks ----
def riff_chunks(d):
    pos, out = 12, []
    while pos + 8 <= len(d):
        cid, n = d[pos:pos + 4], struct.unpack_from("<I", d, pos + 4)[0]
        out.append((cid, d[pos + 8:pos + 8 + n]))
        pos += 8 + n + (n & 1)
    return out


def chunk(cid, body, be=False):
    return cid + struct.pack(">I" if be else "<I", len(body)) + body + (b"\0" if len(body) & 1
                                                                        else b"")


def wav_tag(path, t, where="before", id3=False):
    d = open(path, "rb").read()
    chunks = [c for c in riff_chunks(d) if c[0] not in (b"LIST", b"id3 ", b"ID3 ")]
    if id3:
        tag = [chunk(b"id3 ", id3v2(id3_frames(t, 3, 1), 3))]
    else:
        info = b"INFO"
        for k, cid in (("title", b"INAM"), ("artist", b"IART"), ("album", b"IPRD")):
            if k in t:
                info += chunk(cid, t[k].encode() + b"\0")
        tag = [chunk(b"LIST", info)] if t else []
    body = b""
    for cid, c in chunks:
        if cid == b"data" and where == "before":
            body += b"".join(tag)
        body += chunk(cid, c)
    if where != "before":
        body += b"".join(tag)
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body) + 4) + b"WAVE" + body)


def aiff_tag(path, t, id3=False):
    d = open(path, "rb").read()
    pos, chunks = 12, []
    while pos + 8 <= len(d):
        cid, n = d[pos:pos + 4], struct.unpack_from(">I", d, pos + 4)[0]
        if cid not in (b"NAME", b"AUTH", b"ID3 ", b"id3 "):
            chunks.append(chunk(cid, d[pos + 8:pos + 8 + n], True))
        pos += 8 + n + (n & 1)
    if id3:
        tag = [chunk(b"ID3 ", id3v2(id3_frames(t, 4, 3), 4), True)]
    else:
        tag = [chunk(cid, t[k].encode("latin-1"), True) for k, cid in (("title", b"NAME"),
                                                                ("artist", b"AUTH")) if k in t]
    body = chunks[0] + b"".join(tag) + b"".join(chunks[1:])      # after COMM
    open(path, "wb").write(b"FORM" + struct.pack(">I", len(body) + 4) + d[8:12] + body)


# ---- the set ----
EXT = dict(wav="wav", aiff="aiff", flac="flac", mp3="mp3", vorbis="ogg", opus="opus",
           aac="m4a", alac="m4a", mpc="mpc")
FORMATS = [("wav", 44100, 16), ("wav", 96000, 24), ("wav", 192000, 24), ("aiff", 44100, 16),
           ("aiff", 96000, 24), ("flac", 44100, 16), ("flac", 48000, 24), ("flac", 88200, 24),
           ("flac", 96000, 24), ("flac", 176400, 24), ("flac", 192000, 24), ("mp3", 44100, 16),
           ("mp3", 48000, 16), ("vorbis", 44100, 16), ("vorbis", 96000, 16), ("opus", 48000, 16),
           ("aac", 44100, 16), ("alac", 44100, 16), ("alac", 96000, 24), ("mpc", 44100, 16)]
TAGGED = [("flac", 96000, 24), ("mp3", 44100, 16), ("vorbis", 44100, 16), ("opus", 48000, 16),
          ("aac", 44100, 16), ("wav", 44100, 16), ("aiff", 44100, 16)]


def carried(kind, t):
    """The fields a format's tags here hold: WAV LIST INFO no track, AIFF NAME / AUTH
    (Latin-1) no album either"""
    keep = dict(wav=("title", "artist", "album"), aiff=("title", "artist")).get(kind)
    return {k: v for k, v in t.items() if k in keep} if keep else t


def tag(kind, path, t, pic=None, **kw):
    if kind == "flac":
        flac_tag(path, t, pic and (pic[0], pic[1], pic[2]))
    elif kind == "mp3":
        d = open(path, "rb").read()
        open(path, "wb").write(id3v2(id3_frames(t, 4, 3, pic and pic[:2]), 4) + d if t or pic
                               else d)
    elif kind in ("vorbis", "opus"):
        ogg_tag(path, kind, t, pic)
    elif kind in ("aac", "alac"):
        mp4_tag(path, t, pic)
    elif kind == "wav":
        wav_tag(path, t, **kw)
    elif kind == "aiff":
        aiff_tag(path, t, **kw)


def noise_png(side, seed):
    """An RGB PNG of noise, stored (zlib level 0): 3 bytes a pixel, the same at every run"""
    rows = random.Random(seed).randbytes(side * side * 3)
    raw = b"".join(b"\0" + rows[y * side * 3:(y + 1) * side * 3] for y in range(side))
    pc = lambda t, b: struct.pack(">I", len(b)) + t + b + \
        struct.pack(">I", zlib.crc32(t + b))  # noqa: E731
    return b"\x89PNG\r\n\x1a\n" + pc(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0)) + \
        pc(b"IDAT", zlib.compress(raw, 0)) + pc(b"IEND", b"")


def cover(tmp, name):
    fmt, side = COVERS[name]
    if fmt == "png":
        return noise_png(side, side), fmt, side
    p = os.path.join(tmp, f"cover_{name}.jpg")
    run("ffmpeg", "-nostdin", "-y", "-f", "lavfi", "-i", f"testsrc2=s={side}x{side}",
        "-frames:v", "1", "-fflags", "+bitexact", "-q:v", "3", p)
    return open(p, "rb").read(), fmt, side


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--only", default="formats,tags,id3,containers,covers")
    a = ap.parse_args()
    groups = a.only.split(",")
    missing = [t for t in ("sox", "flac", "lame", "oggenc", "opusenc", "ffmpeg") if not have(t)]
    if missing:
        sys.exit(f"mktestset: missing {', '.join(missing)}")
    os.makedirs(a.out, exist_ok=True)
    man = []
    with tempfile.TemporaryDirectory(prefix="mktestset_") as tmp:
        base = {}

        def fresh(kind, rate, bits):
            key = (kind, rate, bits)
            if key not in base:
                base[key] = os.path.join(tmp, f"{kind}_{rate}_{bits}.{EXT[kind]}")
                encode(kind, a.src, base[key], rate, bits, a.seconds, tmp)
            return base[key]

        def add(group, name, kind, rate, bits, t, expect="tags", pic=None, note="", **kw):
            d = os.path.join(a.out, group)
            os.makedirs(d, exist_ok=True)
            path = os.path.join(d, name)
            shutil.copyfile(fresh(kind, rate, bits), path)
            if t or pic or kw:
                tag(kind, path, t, pic, **kw)
            man.append(dict(file=f"{group}/{name}", codec=kind,
                            rate=48000 if kind == "opus" else rate, bits=bits, channels=2,
                            tags={k: v for k, v in t.items() if k != "rg"},
                            rg=t.get("rg", {}), expect=expect, size=os.path.getsize(path),
                            **({"note": note} if note else {})))

        if "formats" in groups:
            for kind, rate, bits in FORMATS:
                if kind == "mpc" and not have("mpcenc"):
                    print("mktestset: no mpcenc, Musepack skipped")
                    continue
                add("formats", f"{kind}-{bits}-{rate}.{EXT[kind]}", kind, rate, bits,
                    {} if kind == "mpc" else carried(kind, ASCII),
                    expect="tags" if kind != "mpc" else "any")
        if "tags" in groups:
            for kind, rate, bits in TAGGED:
                for name, t in TAGSETS.items():
                    t = carried(kind, LATIN1 if kind == "aiff" and name == "unicode" else t)
                    add("tags", f"{kind}-{name}.{EXT[kind]}", kind, rate, bits, t,
                        expect="tags" if t else "empty")
        if "id3" in groups:
            mp3 = fresh("mp3", 44100, 16)
            d = os.path.join(a.out, "id3")
            os.makedirs(d, exist_ok=True)
            raw = open(mp3, "rb").read()
            U = TAGSETS["unicode"]
            cases = [  # name, tags, bytes before the audio, after it, expect, note
                ("v22-utf16", U, id3v2(id3_frames(U, 2, 1), 2), b"", "tags", ""),
                ("v23-latin1", LATIN1, id3v2(id3_frames(LATIN1, 3, 0), 3), b"", "tags", ""),
                ("v23-utf16", U, id3v2(id3_frames(U, 3, 1), 3), b"", "tags", ""),
                ("v23-exthdr", U, id3v2(id3_frames(U, 3, 1), 3, ext=True), b"", "tags", ""),
                ("v23-unsync", U, id3v2(id3_frames(U, 3, 1), 3, unsynced=True), b"", "empty",
                 "whole-tag unsynchronisation: not read (tags.c)"),
                ("v24-utf8", U, id3v2(id3_frames(U, 4, 3), 4), b"", "tags", ""),
                ("v24-utf16be", U, id3v2(id3_frames(U, 4, 2), 4), b"", "tags", ""),
                ("v24-exthdr", U, id3v2(id3_frames(U, 4, 3), 4, ext=True), b"", "tags", ""),
                ("v24-pad600k", U, id3v2(id3_frames(U, 4, 3), 4, pad=600 * 1024), b"", "tags",
                 "600 KiB of padding after the frames: the audio starts past 512 KiB"),
                ("v1-only", LATIN1, b"", id3v1(LATIN1), "tags", "ID3v1 fields: 30 bytes"),
                ("v24-title-v1", dict(LATIN1, album=LATIN1["album"]),
                 id3v2(id3_frames(dict(title=LATIN1["title"]), 4, 3), 4), id3v1(LATIN1), "tags",
                 "the title from ID3v2, the artist and album from ID3v1 (fallback)"),
                ("v24-rg", TAGSETS["rg"], id3v2(id3_frames(TAGSETS["rg"], 4, 3), 4), b"", "tags",
                 "ReplayGain in TXXX"),
                ("v23-rg-utf16", TAGSETS["rg"], id3v2(id3_frames(TAGSETS["rg"], 3, 1), 3), b"",
                 "tags", "ReplayGain in TXXX, UTF-16"),
            ]
            for name, t, head, tail, expect, note in cases:
                p = os.path.join(d, f"{name}.mp3")
                open(p, "wb").write(head + raw + tail)
                man.append(dict(file=f"id3/{name}.mp3", codec="mp3", rate=44100, bits=16,
                                channels=2, tags={k: v for k, v in t.items() if k != "rg"},
                                rg=t.get("rg", {}), expect=expect, size=os.path.getsize(p),
                                **({"note": note} if note else {})))
        if "containers" in groups:
            U3 = {k: TAGSETS["unicode"][k] for k in ("title", "artist", "album")}
            add("containers", "wav-info-before.wav", "wav", 44100, 16, U3, where="before")
            add("containers", "wav-info-after.wav", "wav", 44100, 16, U3, where="after",
                note="LIST INFO after the data chunk")
            add("containers", "wav-id3.wav", "wav", 96000, 24, TAGSETS["unicode"], id3=True,
                note="an 'id3 ' chunk, ID3v2.3 UTF-16")
            add("containers", "aiff-name-auth.aiff", "aiff", 44100, 16,
                {k: LATIN1[k] for k in ("title", "artist")})
            add("containers", "aiff-id3.aiff", "aiff", 96000, 24, TAGSETS["unicode"], id3=True,
                note="an 'ID3 ' chunk, ID3v2.4 UTF-8")
            for kind in ("aac", "alac"):
                last = fresh(kind, 44100, 16)
                first = os.path.join(tmp, f"{kind}_faststart.m4a")
                run("ffmpeg", "-nostdin", "-y", "-i", last, "-c", "copy", "-map_metadata", "-1",
                    "-fflags", "+bitexact", "-movflags", "+faststart", first)
                for where, src in (("moov-last", last), ("moov-first", first)):
                    p = os.path.join(a.out, "containers", f"{kind}-{where}.m4a")
                    shutil.copyfile(src, p)
                    mp4_tag(p, TAGSETS["unicode"])
                    man.append(dict(file=f"containers/{kind}-{where}.m4a", codec=kind,
                                    rate=44100, bits=16, channels=2, tags=TAGSETS["unicode"],
                                    rg={}, expect="tags", size=os.path.getsize(p)))
        if "covers" in groups:
            for cname in COVERS:
                pic = cover(tmp, cname)
                big = len(pic[0]) > 16 * 1024 * 1024
                for kind, rate, bits in TAGGED[:5]:
                    if kind == "flac" and len(pic[0]) >= 1 << 24:
                        continue                # a FLAC block holds less than 16 MiB
                    ogg = kind in ("vorbis", "opus")
                    add("covers", f"{kind}-cover{cname}.{EXT[kind]}", kind, rate, bits, ASCII,
                        pic=pic, expect="any" if big else "tags",
                        note=f"{len(pic[0])} bytes of {pic[1].upper()} before the text" +
                        (" (base64 in the comment header)" if ogg else "") +
                        (", past the 16 MiB tags.c reads" if big else ""))
    for m in man:
        m["seconds"] = a.seconds
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(man, f, ensure_ascii=False, indent=1)
    total = sum(m["size"] for m in man)
    print(f"{a.out}: {len(man)} files, {total / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
