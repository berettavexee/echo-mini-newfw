#!/usr/bin/env python3
"""Builds the library index NEWFWLIB.DAT (docs/LIBRARY.md, ETUDE §121-§123).

  mklib.py DISK.IMG [--sd CARD.IMG] -o NEWFWLIB.DAT
  mklib.py --json TRACKS.json -o NEWFWLIB.DAT [--expect EXPECTED.txt]

DISK.IMG: an image of the player's disk (the internal memory's volume at VOL_SECTOR, as the
emulator's disks), or of a volume alone; CARD.IMG: a microSD card's image. Their files' tags are
read with mutagen, with the rules of fw/src/tags.c. --json: a list of tracks (the host test's
fixture: the fields of a track, below); --expect: the answers fw/src/library.c must give for it
(fw/tools/library_test.c). The same tracks always give the same bytes.
"""
import argparse
import json
import os
import struct
import sys
import zlib

HALF = 4 << 20                                  # each half of the file
VOL_SECTOR = 0x81000                            # the internal volume in the emulator's disks
CODECS = {"wav": 1, "flac": 2, "fla": 2, "mp3": 3, "ogg": 4, "oga": 4, "opus": 5, "opu": 5,
          "aiff": 6, "aif": 6, "aifc": 6, "m4a": 7, "m4b": 7, "mp4": 7, "mpc": 9, "mp+": 9,
          "mpp": 9}                             # browser.c's codec_of (m4a: AAC; ALAC is 8)
LIMITS = dict(title=96, artist=128, album=128, albumartist=96, genre=48, file=128)  # with the 0

# ---- the sort key (the same in fw/src/library.c) ----
# U+00C0 to U+017F: two characters each, the base letters ("  ": the character itself)
LATIN = ("a a a a a a aec e e e e i i i i d n o o o o o   o u u u u y thssa a a a a a aec e e e "
         "e i i i i d n o o o o o   o u u u u y thy a a a a a a c c c c c c c c d d d d e e e e "
         "e e e e e e g g g g g g g g h h h h i i i i i i i i i i ijijj j k k k l l l l l l l l "
         "l l n n n n n n n n n o o o o o o oeoer r r r r r s s s s s s s s t t t t t t u u u u "
         "u u u u u u u u w w y y y z z z z z z s ")
GREEK = {0x386: 0x3b1, 0x388: 0x3b5, 0x389: 0x3b7, 0x38a: 0x3b9, 0x38c: 0x3bf, 0x38e: 0x3c5,
         0x38f: 0x3c9, 0x3ac: 0x3b1, 0x3ad: 0x3b5, 0x3ae: 0x3b7, 0x3af: 0x3b9, 0x3cc: 0x3bf,
         0x3cd: 0x3c5, 0x3ce: 0x3c9, 0x3c2: 0x3c3, 0x390: 0x3b9, 0x3b0: 0x3c5, 0x3aa: 0x3b9,
         0x3ab: 0x3c5, 0x3ca: 0x3b9, 0x3cb: 0x3c5}


def fold(c):
    """The folded code points of one character"""
    if 0x41 <= c <= 0x5a:
        return [c + 32]
    if 0xc0 <= c <= 0x17f:
        cell = LATIN[2 * (c - 0xc0):2 * (c - 0xc0) + 2]
        if cell[0] == " ":
            return [c]
        return [ord(x) for x in cell.rstrip(" ")]
    if c in GREEK:
        return [GREEK[c]]
    if 0x391 <= c <= 0x3a9:
        return [c + 32]
    if c == 0x401 or c == 0x451:                 # Ё ё: е
        return [0x435]
    if 0x410 <= c <= 0x42f:
        return [c + 32]
    if 0x400 <= c <= 0x40f:
        return [c + 80]
    return [c]


def sort_key(s, artist=False):
    if artist and len(s) > 4 and s[:4].lower() == "the ":
        s = s[4:]
    k = []
    for ch in s:
        k += fold(ord(ch))
    return k


def str_order(s, artist=False):
    """The order of strings: their keys, then their bytes; the empty string last"""
    return (s == "", sort_key(s, artist), s.encode("utf-8", "surrogatepass"))


# ---- the fields ----
def clip(s, field):
    """tags.c's limits: whole UTF-8 characters within the field, trailing spaces off"""
    full = (s or "").encode("utf-8", "surrogatepass")
    n = min(len(full), LIMITS[field] - 1)
    while 0 < n < len(full) and (full[n] & 0xc0) == 0x80:
        n -= 1                                  # the character cut by the limit: dropped
    return full[:n].decode("utf-8", "surrogatepass").rstrip(" ")


def number(v):
    """The leading digits of a value ("5/12": 5), 0 when none"""
    v = str(v or "")
    n = 0
    for ch in v:
        if not ch.isdigit() or not ch.isascii():
            break
        n = n * 10 + int(ch)
    return n & 0xffff


def track_of(d):
    """A track from its fields (the JSON's, or read from a file)"""
    y = number(d.get("year"))
    return dict(vol=int(d.get("vol", 0)), contig=int(d.get("contig", 0)),
                cluster=int(d["cluster"]), size=int(d["size"]), mtime=int(d.get("mtime", 0)),
                codec=int(d["codec"]), title=clip(d.get("title"), "title"),
                artist=clip(d.get("artist"), "artist"), album=clip(d.get("album"), "album"),
                albumartist=clip(d.get("albumartist"), "albumartist"),
                genre=clip(d.get("genre"), "genre"), file=clip(d.get("file"), "file"),
                track=number(d.get("track")), year=y if 1000 <= y <= 9999 else 0,
                disc=min(number(d.get("disc")), 255))


# ---- the index ----
class Strings:
    def __init__(self):
        self.blob, self.at = bytearray(b"\0"), {"": 0}

    def ref(self, s):
        if s not in self.at:
            self.at[s] = len(self.blob)
            self.blob += s.encode("utf-8", "surrogatepass") + b"\0"
        return self.at[s]


def build(tracks, volumes):
    """The half: (bytes, tables for --expect). tracks: track_of() dicts; volumes: {vol: serial}"""
    tracks = [dict(t) for t in tracks]
    for t in tracks:
        t["aa"] = t["albumartist"] or t["artist"]
    artists = sorted({t["aa"] for t in tracks}, key=lambda s: str_order(s, True))
    a_index = {a: i for i, a in enumerate(artists)}
    albums = sorted({(t["aa"], t["album"]) for t in tracks},
                    key=lambda k: (a_index[k[0]], str_order(k[1])))
    al_index = {k: i for i, k in enumerate(albums)}
    genres = sorted({t["genre"] for t in tracks}, key=str_order)
    g_index = {g: i for i, g in enumerate(genres)}
    if max(len(artists), len(albums), len(genres)) > 0xffff:
        raise ValueError("more than 65,535 artists, albums or genres")
    tracks.sort(key=lambda t: (al_index[(t["aa"], t["album"])], t["disc"], t["track"],
                               t["file"].encode("utf-8", "surrogatepass"), t["vol"],
                               t["cluster"]))
    st = Strings()
    for t in tracks:                            # the strings in a fixed order: by track, then
        for k in ("title", "file", "artist"):   # the names
            st.ref(t[k])
    for s in artists + [k[1] for k in albums] + genres:
        st.ref(s)

    trks = bytearray()
    al_first, al_count, al_year = {}, {}, {}
    for i, t in enumerate(tracks):
        al = al_index[(t["aa"], t["album"])]
        al_first.setdefault(al, i)
        al_count[al] = al_count.get(al, 0) + 1
        if t["year"] and (al not in al_year or t["year"] < al_year[al]):
            al_year[al] = t["year"]
        trks += struct.pack("<IIIIIIHHHHBBBB", t["cluster"], t["size"], t["mtime"],
                            st.ref(t["title"]), st.ref(t["file"]), st.ref(t["artist"]), al,
                            g_index[t["genre"]], t["track"], t["year"], t["disc"], t["codec"],
                            t["vol"], t["contig"] & 1)
    if any(c > 0xffff for c in al_count.values()):
        raise ValueError("an album of more than 65,535 tracks")
    albs = bytearray()
    ar_first, ar_count = {}, {}
    for i, (aa, name) in enumerate(albums):
        ar = a_index[aa]
        ar_first.setdefault(ar, i)
        ar_count[ar] = ar_count.get(ar, 0) + 1
        albs += struct.pack("<IIHHHH", st.ref(name), al_first[i], al_count[i], ar,
                            al_year.get(i, 0), 0)
    arts = b"".join(struct.pack("<IHH", st.ref(a), ar_first[i], ar_count[i])
                    for i, a in enumerate(artists))
    g_albums = {g: sorted({al_index[(t["aa"], t["album"])] for t in tracks if t["genre"] == g})
                for g in genres}
    gens, galb = bytearray(), bytearray()
    for g in genres:
        gens += struct.pack("<III", st.ref(g), len(galb) // 2, len(g_albums[g]))
        galb += b"".join(struct.pack("<H", a) for a in g_albums[g])
    song = sorted(range(len(tracks)), key=lambda i: (str_order(tracks[i]["title"] or
                                                               tracks[i]["file"]), i))
    songs = b"".join(struct.pack("<I", i) for i in song)
    clus = sorted(range(len(tracks)), key=lambda i: (tracks[i]["vol"] << 31 | tracks[i]["cluster"], i))
    clu = b"".join(struct.pack("<II", tracks[i]["vol"] << 31 | tracks[i]["cluster"], i) for i in clus)
    vols = b"".join(struct.pack("<B3xII", v, volumes.get(v, 0),
                                sum(1 for t in tracks if t["vol"] == v))
                    for v in sorted(set(volumes) | {t["vol"] for t in tracks}))

    sections = [(b"VOLS", vols, 12), (b"STRS", bytes(st.blob), 0), (b"TRKS", trks, 36),
                (b"ARTS", arts, 8), (b"ALBS", albs, 16), (b"GENS", gens, 12),
                (b"GALB", galb, 2), (b"SONG", songs, 4), (b"CLUS", clu, 8)]
    at = 16 + 16 * len(sections)
    table, body = b"", b""
    for sid, data, size in sections:
        data = bytes(data)
        table += sid + struct.pack("<III", at + len(body), len(data),
                                   len(data) // size if size else len(st.at))
        body += data + bytes(-len(data) % 4)
    half = b"NWLI" + struct.pack("<III", 1, len(sections), at + len(body)) + table + body
    if len(half) > HALF:
        raise ValueError(f"the index takes {len(half)} bytes: more than {HALF}")
    return half, dict(tracks=tracks, artists=artists, albums=albums, genres=genres,
                      g_albums=g_albums, song=song, clus=clus, volumes=volumes, al_first=al_first, al_count=al_count,
                      al_year=al_year, ar_first=ar_first, ar_count=ar_count, a_index=a_index,
                      g_index=g_index, al_index=al_index)


def index_file(half, generation=1):
    """The whole file: header, half A in use, half B empty"""
    crc = zlib.crc32(half)
    tracks = struct.unpack_from("<I", half, 16 + 16 * 2 + 12)[0]       # TRKS's count
    h = b"NEWFWLIB" + struct.pack("<IIIIIIII", 1, 0, generation, len(half), crc, 0, 0, tracks)
    h += bytes(508 - len(h))
    h += struct.pack("<I", zlib.crc32(h))
    return h + half + bytes(HALF - len(half)) + bytes(HALF)


def expect_lines(t):
    """The answers of fw/src/library.c for the index (library_test.c reads them)"""
    out = [f"counts\t{len(t['artists'])}\t{len(t['albums'])}\t{len(t['genres'])}\t"
           f"{len(t['tracks'])}"]
    for i, a in enumerate(t["artists"]):
        out.append(f"artist\t{i}\t{a}\t{t['ar_first'][i]}\t{t['ar_count'][i]}")
    for i, (aa, name) in enumerate(t["albums"]):
        out.append(f"album\t{i}\t{name}\t{t['a_index'][aa]}\t{t['al_year'].get(i, 0)}\t"
                   f"{t['al_first'][i]}\t{t['al_count'][i]}")
    for i, g in enumerate(t["genres"]):
        out.append(f"genre\t{i}\t{g}\t" + ",".join(str(a) for a in t["g_albums"][g]))
    for i, k in enumerate(t["tracks"]):
        out.append("track\t" + "\t".join(str(x) for x in (
            i, k["title"], k["file"], k["artist"], t["al_index"][(k["aa"], k["album"])],
            t["g_index"][k["genre"]], k["track"], k["year"], k["disc"], k["codec"], k["vol"],
            k["contig"], k["cluster"], k["size"], k["mtime"])))
    for i, k in enumerate(t["song"]):
        out.append(f"song\t{i}\t{k}")
    for i, k in enumerate(t["clus"]):
        out.append(f"clus\t{i}\t{t['tracks'][k]['vol']}\t{t['tracks'][k]['cluster']}\t{k}")
    for v in sorted(set(t["volumes"]) | {k["vol"] for k in t["tracks"]}):
        out.append(f"vol\t{v}\t{t['volumes'].get(v, 0)}\t"
                   f"{sum(1 for k in t['tracks'] if k['vol'] == v)}")
    out += view_lines(t)
    names = sorted({k["artist"] for k in t["tracks"]} | {k["album"] for k in t["tracks"]} |
                   {k["title"] for k in t["tracks"]})
    out.append("order\t0\t" + "\t".join(sorted(names, key=str_order)))
    out.append("order\t1\t" + "\t".join(sorted(names, key=lambda s: str_order(s, True))))
    return out


# ---- the views (fw/src/libview.c, the same rules) ----
LV_ARTISTS, LV_ARTIST_ALBUMS, LV_ALBUMS, LV_YEARS, LV_YEAR_ALBUMS, LV_GENRES, LV_GENRE_ALBUMS, \
    LV_TRACKS, LV_SONGS = range(1, 10)


def same_track(a, b, al_a, al_b):
    """The same track on both volumes: album, disc, number, title (file names if one is empty)"""
    if al_a != al_b or a["vol"] == b["vol"] or a["disc"] != b["disc"] or a["track"] != b["track"]:
        return False
    if a["title"] and b["title"]:
        return a["title"] == b["title"]
    return a["file"] == b["file"]


def views(t, card_in):
    """{(list, param): [ids]}: the lists libview.c shows, the card's tracks there or not"""
    tr = t["tracks"]
    al_of = [t["al_index"][(k["aa"], k["album"])] for k in tr]
    hid = [k["vol"] == 1 and not card_in for k in tr]
    if any(k["vol"] == 1 for k in tr):
        for i in range(1, len(tr)):
            if not (tr[i]["vol"] == 1 and not card_in) and same_track(tr[i - 1], tr[i], al_of[i - 1],
                                                                      al_of[i]):
                hid[i if tr[i]["vol"] == 1 else i - 1] = True
    nal = len(t["albums"])
    avis = [any(not hid[k] for k in range(t["al_first"][a], t["al_first"][a] + t["al_count"][a]))
            for a in range(nal)]
    year = [t["al_year"].get(a, 0) for a in range(nal)]
    v = {}
    v[(LV_ALBUMS, 0)] = [a for a in range(nal) if avis[a]]
    v[(LV_ARTISTS, 0)] = []
    for r in range(len(t["artists"])):
        al = [a for a in range(t["ar_first"][r], t["ar_first"][r] + t["ar_count"][r]) if avis[a]]
        v[(LV_ARTIST_ALBUMS, r)] = al
        if al:
            v[(LV_ARTISTS, 0)].append(r)
    ys = sorted({year[a] for a in range(nal) if avis[a]}, key=lambda y: y or 0x10000)
    v[(LV_YEARS, 0)] = ys
    for y in ys:
        v[(LV_YEAR_ALBUMS, y)] = [a for a in range(nal) if avis[a] and year[a] == y]
    v[(LV_GENRES, 0)] = []
    for g, name in enumerate(t["genres"]):
        al = [a for a in t["g_albums"][name] if avis[a]]
        v[(LV_GENRE_ALBUMS, g)] = al
        if al:
            v[(LV_GENRES, 0)].append(g)
    for a in range(nal):
        v[(LV_TRACKS, a)] = [k for k in range(t["al_first"][a], t["al_first"][a] + t["al_count"][a])
                             if not hid[k]]
    v[(LV_SONGS, 0)] = [k for k in t["song"] if not hid[k]]
    return v


def view_lines(t):
    out = []
    for card_in in (1, 0):
        for (lst, param), ids in sorted(views(t, card_in).items()):
            out.append(f"view\t{card_in}\t{lst}\t{param}\t" + ",".join(str(i) for i in ids))
    return out


# ---- tags of a file (mutagen), with tags.c's rules ----
ID3V1_GENRES = ["Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
                "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap",
                "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska", "Death Metal",
                "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
                "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House",
                "Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass", "Soul", "Punk",
                "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic",
                "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance",
                "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
                "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic",
                "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz",
                "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock"]


def genre_name(s):
    """tags.c's genre_set: "(17)", "17": ID3v1's genre 17"""
    s = s or ""
    p = s[1:] if s.startswith("(") else s
    digits = ""
    for ch in p:
        if ch.isascii() and ch.isdigit():
            digits += ch
        else:
            break
    if digits:
        rest = p[len(digits):]
        n = int(digits)
        if n < 80 and (rest == ")" if s.startswith("(") else rest == ""):
            return ID3V1_GENRES[n]
    return s


def first(tags, *keys):
    for k in keys:
        try:
            v = tags[k]
        except (KeyError, ValueError, TypeError):
            continue
        if isinstance(v, list):
            v = v[0] if v else ""
        if hasattr(v, "text"):
            v = v.text[0] if v.text else ""
        return str(v)
    return ""


def riff_info(data, be=False):
    """WAV LIST INFO (INAM, IART, IPRD, IGNR, ICRD) or AIFF NAME / AUTH; an ID3 chunk's bytes"""
    out, id3, pos = {}, None, 12
    while pos + 8 <= len(data):
        cid, n = data[pos:pos + 4], struct.unpack_from(">I" if be else "<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + n]
        if cid in (b"id3 ", b"ID3 "):
            id3 = body
        elif not be and cid == b"LIST" and body[:4] == b"INFO":
            q = 4
            while q + 8 <= len(body):
                sid, m = body[q:q + 4], struct.unpack_from("<I", body, q + 4)[0]
                v = body[q + 8:q + 8 + m].split(b"\0")[0].decode("utf-8", "replace")
                key = {b"INAM": "title", b"IART": "artist", b"IPRD": "album", b"IGNR": "genre",
                       b"ICRD": "year"}.get(sid)
                if key and m <= 512:
                    out[key] = v
                q += 8 + m + (m & 1)
        elif be and cid in (b"NAME", b"AUTH") and n <= 512:
            out.setdefault("title" if cid == b"NAME" else "artist", body.decode("latin-1"))
        pos += 8 + n + (n & 1)
    return out, id3


def id3_fields(tags):
    g = first(tags, "TCON")
    return dict(title=first(tags, "TIT2"), artist=first(tags, "TPE1"), album=first(tags, "TALB"),
                albumartist=first(tags, "TPE2"), genre=genre_name(g), year=first(tags, "TDRC",
                                                                                  "TYER"),
                disc=first(tags, "TPOS"), track=first(tags, "TRCK"))


def file_fields(name, data):
    """The tag fields of a file's bytes"""
    import io
    import mutagen
    ext = name.rsplit(".", 1)[-1].lower()
    if ext in ("wav", "aiff", "aif", "aifc"):
        fields, id3 = riff_info(data, be=ext != "wav")
        if id3:
            from mutagen.id3 import ID3
            try:
                fields.update({k: v for k, v in id3_fields(ID3(io.BytesIO(id3))).items() if v})
            except Exception:
                pass
        return fields
    try:
        f = mutagen.File(io.BytesIO(data))
    except Exception:
        return {}
    if f is None or f.tags is None:
        return {}
    t = f.tags
    if ext == "mp3":
        return id3_fields(t)
    if ext in ("m4a", "m4b", "mp4"):
        g = first(t, "\xa9gen")
        if not g and "gnre" in t and t["gnre"]:
            n = t["gnre"][0]
            g = ID3V1_GENRES[n - 1] if 1 <= n <= 80 else ""
        trk = t.get("trkn", [(0, 0)])[0][0] if "trkn" in t else 0
        disc = t.get("disk", [(0, 0)])[0][0] if "disk" in t else 0
        return dict(title=first(t, "\xa9nam"), artist=first(t, "\xa9ART"),
                    album=first(t, "\xa9alb"), albumartist=first(t, "aART"), genre=g,
                    year=first(t, "\xa9day"), track=trk, disc=disc)
    if ext in ("mpc", "mp+", "mpp"):
        return dict(title=first(t, "Title"), artist=first(t, "Artist"), album=first(t, "Album"),
                    albumartist=first(t, "Album Artist"), genre=genre_name(first(t, "Genre")),
                    year=first(t, "Year"), track=first(t, "Track"), disc=first(t, "Disc"))
    return dict(title=first(t, "title"), artist=first(t, "artist"), album=first(t, "album"),
                albumartist=first(t, "albumartist", "album artist"),
                genre=genre_name(first(t, "genre")), year=first(t, "date", "year"),
                track=first(t, "tracknumber"), disc=first(t, "discnumber"))


def image_tracks(path, sector, vol):
    from fatimg import Volume
    v = Volume(path, sector)
    out = []
    for _, e in v.walk():
        ext = e.name.rsplit(".", 1)[-1].lower() if "." in e.name else ""
        if ext not in CODECS or not e.size:
            continue
        codec = CODECS[ext]                     # (M4A: 7, AAC or ALAC: the player's open says)
        data = v.read(e)
        out.append(track_of(dict(file_fields(e.name, data), vol=vol, contig=e.contig,
                                 cluster=e.cluster, size=e.size, mtime=e.mtime, codec=codec,
                                 file=e.name)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?")
    ap.add_argument("--sd")
    ap.add_argument("--json")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--expect")
    a = ap.parse_args()
    if a.json:
        doc = json.load(open(a.json, encoding="utf-8"))
        tracks = [track_of(d) for d in doc["tracks"]]
        volumes = {int(k): v for k, v in doc.get("volumes", {"0": 0}).items()}
    elif a.image:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        sector = VOL_SECTOR if os.path.getsize(a.image) > VOL_SECTOR * 512 else 0
        from fatimg import Volume
        tracks = image_tracks(a.image, sector, 0)
        volumes = {0: 0}
        if a.sd:
            tracks += image_tracks(a.sd, 0, 1)
            volumes[1] = Volume(a.sd, 0).serial     # the card's volume serial
    else:
        sys.exit(__doc__)
    half, tables = build(tracks, volumes)
    open(a.out, "wb").write(index_file(half))
    if a.expect:
        open(a.expect, "w", encoding="utf-8").write("\n".join(expect_lines(tables)) + "\n")
    print(f"{a.out}: {len(tracks)} tracks, {len(tables['artists'])} artists, "
          f"{len(tables['albums'])} albums, {len(tables['genres'])} genres, {len(half)} bytes")


if __name__ == "__main__":
    main()
