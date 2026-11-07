# The library index (NEWFWLIB.DAT)

The library (`ETUDE.md` §121) browses the music by its tags: artists, albums, genres, all songs.
It reads an index built in advance, never the files' tags while browsing. This page describes
the index file, version 1.

The index is **deterministic**: the same files, with the same tags at the same places of the
disk, give the same bytes, whoever builds it. `fw/tools/mklib.py` builds it on a PC from a disk
image; the firmware will build the same bytes on the device (step 3), and the emulator compares
the two. `fw/src/library.c` reads it.

## The file

`NEWFWLIB.DAT`, at the root of the internal memory, contiguous (one run of clusters), 512 bytes
plus 8 MiB:

| Offset | Size | Content |
|---|---|---|
| 0 | 512 | the header |
| 512 | 4 MiB | half A |
| 512 + 4 MiB | 4 MiB | half B |

A new index is written into the half not in use, then the header is rewritten to point at it:
a power cut leaves the index before it.

The firmware creates the file when it is missing (`fw/src/fatwrite.c`, `lib_file()` in
`fw/src/library.c`), its header naming no half; it never deletes it: a file of that name with
another size, or fragmented, is left as it is (to be deleted from a computer). The creation
writes, in this order, the file's first sector (in free clusters), its chain in both FATs, its
directory entry (one sector: the file appears whole) and FSInfo (the free count set to
"unknown"); a power cut leaves at most clusters reserved that nothing uses. Only the sectors the
creation planned can be written. It is tested in the emulator (`emutest.py lib-*`, an
`ECHO_LIBCREATE` image): `fsck.fat` clean, the file one run of clusters with the empty header, a
volume whose free space is fragmented (refused, nothing written), holes before a free run, a
full root directory (one more cluster for it), and power cuts at eight points of the creation.

All numbers are little-endian.

### The header

| Offset | Size | Content |
|---|---|---|
| 0 | 8 | `NEWFWLIB` |
| 8 | 4 | version: 1 |
| 12 | 4 | the half in use: 0 (A), 1 (B), `0xffffffff` none |
| 16 | 4 | generation: one more at each new index |
| 20 | 8 | half A: its length in bytes, its CRC-32 |
| 28 | 8 | half B: the same |
| 36 | 472 | zeros |
| 508 | 4 | CRC-32 of bytes 0 to 507 |

CRC-32 is the one of zlib (polynomial `0xedb88320`, initial value and final XOR `0xffffffff`).

## A half

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | `NWLI` |
| 4 | 4 | version: 1 |
| 8 | 4 | the number of sections |
| 12 | 4 | the length of the half's content, in bytes |
| 16 | 16 each | the sections: an identifier (4 bytes), offset (from the half's start), length in bytes, count of entries |

Sections are 4-byte aligned. A reader ignores the sections it does not know: a later version can
add one without breaking the readers of version 1. The sections of version 1:

| Id | Entry | Content |
|---|---|---|
| `VOLS` | 12 bytes | the volumes: number (0 internal, 1 card), 3 zeros, the card's serial (0 for the internal memory), the count of its tracks |
| `STRS` | — | the strings, UTF-8, each ended by a zero byte, without duplicates; offset 0 is the empty string |
| `TRKS` | 36 bytes | the tracks, in album order (below) |
| `ARTS` | 8 bytes | the artists, sorted |
| `ALBS` | 16 bytes | the albums, sorted |
| `GENS` | 12 bytes | the genres, sorted |
| `GALB` | 2 bytes | the albums of each genre: album numbers |
| `SONG` | 4 bytes | the tracks sorted by title: track numbers |

String references are offsets into `STRS`. Numbers of artists, albums and genres are indexes into
their sections (16 bits: at most 65,535 of each).

### A track (`TRKS`, 36 bytes)

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | first cluster |
| 4 | 4 | size in bytes |
| 8 | 4 | last write: FAT date << 16 \| FAT time (exFAT: its timestamp) |
| 12 | 4 | title (string) |
| 16 | 4 | file name (string) |
| 20 | 4 | artist of the track (string) |
| 24 | 2 | album |
| 26 | 2 | genre |
| 28 | 2 | track number (0: none) |
| 30 | 2 | year (0: none) |
| 32 | 1 | disc number (0: none; capped at 255) |
| 33 | 1 | codec (`enum hp_codec`, `fw/src/hifi_proto.h`) |
| 34 | 1 | volume |
| 35 | 1 | flags: bit 0, exFAT "no FAT chain" (the clusters follow each other) |

The volume, the flag, the first cluster and the size are what the firmware needs to open the
file (`struct fat_file`); the last write and the size tell whether a file changed since.

### An artist (`ARTS`, 8 bytes)

Name (string, 4 bytes), its first album (2 bytes), its count of albums (2 bytes): its albums
follow each other in `ALBS`.

### An album (`ALBS`, 16 bytes)

Name (string, 4), its first track (4), its count of tracks (2), its artist (2), its year (2: the
lowest of its tracks' years that are not 0), 2 zeros. Its tracks follow each other in `TRKS`.

### A genre (`GENS`, 12 bytes)

Name (string, 4), its first entry in `GALB` (4), its count of albums (4): the albums with at
least one track of that genre, in album order.

## The fields

The text fields are those of the firmware's tag reader (`fw/src/tags.c`), with its limits: whole
UTF-8 characters within 95 bytes for the title and the album artist, 127 for the artist and the
album, 47 for the genre, 127 for the file name, the trailing spaces taken off. When a file has no
title, its title is empty (the screens show the file name).

## Grouping and order

- **The artist of an album**: the album artist tag, else the track's artist.
- **An album**: its artist and its name (the exact strings). Tracks without an album, for an
  artist: one album with an empty name ("Unknown album" on the screen).
- **A genre**: the exact string; tracks without a genre form one genre with an empty name.
- **The sort key** of a string: its characters, each folded: A-Z to a-z; the Latin letters of
  U+00C0 to U+017F to their base letters (é to e, ß to ss, æ to ae, œ to oe...); Greek and
  Cyrillic capitals to small letters, their accented letters to the plain ones, final sigma to
  sigma; any other character (CJK among them) as it is. For an **artist**, a leading "The "
  (any case) is skipped. Strings compare by their keys, character by character; then by their
  bytes. **The empty string comes last.**
- **Artists**: by their keys. **Albums**: by their artist's place, then by their keys. **Tracks**
  (`TRKS`): by their album's place, then disc, track number, file name (bytes), volume and
  first cluster. **Genres**: by their keys. **`SONG`**: the tracks by the keys of their titles
  (the file name when there is none), then by their place in `TRKS`.

The tables of the folding are in `fw/tools/mklib.py` and `fw/src/library.c`; the host test
(`fw/tools/library_test.c`, in `make check`) checks that both sort alike.

## Tools

- `fw/tools/mklib.py DISK.IMG [--sd CARD.IMG] -o NEWFWLIB.DAT`: the index of the volumes of a disk
  image (the internal memory at the sector the firmware mounts it), and of a card image. The
  tags are read with mutagen, with the rules of `tags.c`.
- `fw/tools/mklib.py --json TRACKS.json -o NEWFWLIB.DAT [--expect EXPECTED.txt]`: the index of a
  list of tracks given in JSON (the host test's fixture), and the answers the reader must give.
- `fw/tools/fatimg.py`: the FAT32 and exFAT reader of `mklib.py`.
