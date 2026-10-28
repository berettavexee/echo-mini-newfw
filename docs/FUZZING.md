# Fuzzing the decoders and the file readers

A music player reads files it did not make: interrupted copies, damaged cards, odd encoders,
files built to crash a decoder. On this player a decoder fault stops core B (the HiFi core), and a
wild write can corrupt what the other core reads. This page describes the fuzzing bench that tests
our decoders and file readers against such input on the PC, what it found, and how to run it.
The detailed study, in French, is `ETUDE.md` §62 (Ogg, Vorbis, Opus) and §63 (MP3, AAC, MP4,
tags).

The tools are in [`fw/tools/fuzz/`](../fw/tools/fuzz/):

| File | Role |
|---|---|
| `fuzz_host.c` | The fuzzer and the stand-in for core B's runtime |
| `build.sh` | Builds one target (see below) |
| `mkseeds.sh` | Makes the seed files |
| `mkfatseeds.py` | Makes the seed disks of the `fat` target |

## What is tested

The firmware's own code, unchanged, built for the PC:

| Target | Code | Input |
|---|---|---|
| `ogg` | `fw/hifi/oggpage.c`, our Ogg page and packet reader | an Ogg file |
| `vorbis` | `fw/hifi/codec_vorbis.c` + Tremor lowmem | an Ogg Vorbis file |
| `vorbis-heap` | the same with core B's allocator (`fw/hifi/heap.c`), no ASan | an Ogg Vorbis file |
| `opus` | `fw/hifi/codec_opus.c` + libopus 1.5.2, fixed point | an Ogg Opus file |
| `mp3` | `fw/hifi/codec_mp3.c` + pvmp3dec (AOSP) | an MP3 file |
| `mp4` | `fw/src/mp4.c` (core A): the MP4 container, then the whole packet stream, then a seek | an M4A file |
| `aac` | `mp4.c`, then `fw/hifi/codec_aac.c` + pvaacdec (AOSP) on the packet stream it makes | an M4A file |
| `aacpkt` | `codec_aac.c` + pvaacdec alone | a packet stream (`hifi_proto.h`), configuration included |
| `tags` | `fw/src/tags.c` (core A), for every format | any file |
| `fat` | `fw/src/fat.c` (core A): the internal volume (FAT32) and the card (FAT32 or exFAT): mount, root, every folder down to depth 4, reads in every file | a disk, as sparse sectors |

A core B codec is opened as on the device (`codec_open()`), then decodes up to 400 blocks
(`codec_run()`). Core A code reads the file through a stand-in for `fat_pread()`. The `aac`
target goes through the real input ring at the address core B uses, as `player.c` does.

The `fat` input is a disk given as records: a 32-bit sector number (little endian), then the
512 bytes of that sector; the other sectors read as zeros, and those past 2 GiB as a read error.
The same disk serves as the eMMC (`fat_mount`, which searches FAT32 from sector 0, alone or
behind an MBR) and as the card (`fat_mount_sd`: a FAT32 or exFAT volume at sector 0 or in the
MBR). The folders are walked as the firmware does: a folder is listed in full, then its files
read and its subfolders walked (`fat_list` is not reentrant). More than 2,000,000 sector reads
in a run count as a hang: a loop in a cluster chain. `mkfatseeds.py` makes the seeds from real
volumes (mkfs.fat and mtools, mkfs.exfat and `fw/tools/exfat_add.py`): long names with accents
and CJK, folders three deep, a folder of 40 entries over several clusters, empty, small,
multi-cluster and fragmented files, a hidden one; clusters of 512 bytes and 4 KiB (FAT32), 4
and 32 KiB (exFAT), with and without an MBR.

## How it works

- **The runtime** (`rt.h`) is replaced by functions over a memory buffer: the input ring, the
  output ring (discarded), the allocator. The control block `HP_CTL` is mapped at its core B
  address. A codec that waits for input that never comes is reported as a hang.
- **Sanitizers**: AddressSanitizer for out-of-bounds and use-after-free, UndefinedBehaviorSanitizer
  for shifts of 32 bits or more, divisions by zero, misaligned or invalid pointers. Two UBSan
  checks are left out: left shifts of negative values and signed overflow, which Tremor uses on
  purpose and which are well defined on the ARM target.
- **Coverage guidance**, as AFL does, with gcc alone: the code under test is built with
  `-fsanitize-coverage=trace-pc`. Every block executed calls `__sanitizer_cov_trace_pc()`, which
  hashes each (previous block, block) edge into a 64 KiB bitmap shared with the parent. A mutated
  input that reaches an edge never seen before joins the corpus and is mutated further.
- **One process per input**: each run is a `fork()` of the parent. The codecs' static state starts
  fresh every time, and a crash, a sanitizer report or a hang (5 s) only ends the child.
- **Mutations**: bit flips, interesting bytes (0, 0x7f, 0x80, 0xff…), random bytes and words,
  truncation, deleted and duplicated runs. Three positions out of four fall in a "hot" region:
  - the first 12 KiB for Ogg and MP3 (the Vorbis setup header and its codebooks, ID3v2, the
    Xing/LAME frame);
  - for MP4, the `moov` box wherever it is (it holds everything `mp4.c` reads; it may be at the
    end of the file);
  - for `tags`, also the last 512 bytes (ID3v1, APEv2);
  - for `fat`, inside the sectors, and one mutation in 16 in a sector number (a sector moves).
- **Faithful to the target**: `-funsigned-char` as on ARM (a `char` index that is negative on the
  PC is not on the device). The `vorbis-heap` target keeps core B's own allocator
  (`fw/hifi/heap.c`) in place of the C library's. ASan cannot see inside that allocator, but some
  faults only show with it, for instance freeing an uninitialised pointer.

## Running it

Requirements: Linux, gcc and g++ with libasan and libubsan, GNU make, ffmpeg with libvorbis,
libopus and libmp3lame (for the seeds).

```sh
cd fw/tools/fuzz
./mkseeds.sh seeds                  # synthetic seeds; add a music folder: ./mkseeds.sh seeds ~/Music
./build.sh vorbis                   # or any target of the table above
build/vorbis/fuzz seeds/vorbis out/vorbis 1500     # 25 minutes
FUZZ_STREAM=s.hp4p build/mp4/fuzz -r seeds/m4a/syn1.m4a   # a packet stream, seed for aacpkt
./mkfatseeds.py seeds/fat && ./build.sh fat        # needs mkfs.fat, mtools, mkfs.exfat
FUZZ_V=1 build/fat/fuzz -r seeds/fat/exfat_4k      # the tree the reader sees
```

Several instances can run side by side on the same seeds (each draws its own random sequence):

```sh
for i in 1 2 3 4 5 6; do build/fat/fuzz seeds/fat out/fat$i 7200 > out/fat$i.log & done
```

`mkseeds.sh` encodes a few seconds of tone and noise at several rates, channel counts and
qualities: Vorbis, Opus, MP3 (MPEG-1, 2 and 2.5, CBR and VBR, with and without the Xing frame,
ID3v2 and ID3v1 tags) and M4A (8 to 96 kHz, the `moov` box at the start or at the end). Given a
music folder, it also takes a few files of each format from it. These are remuxed without
re-encoding and without cover art, so that the seed holds audio and not a cover picture. Ogg and
MP3 seeds are cut to 40,000 bytes; M4A seeds are kept whole. Seeds and findings stay out of the repository
(`.gitignore`): they may come from your own music.

Progress is printed every 30 s:

```
[1500 s] 221161 execs (147/s), corpus 99, crashes 325, hangs 0
```

Each crash or hang is saved as `OUTDIR/crash-<hash>` or `OUTDIR/hang-<hash>`. To replay one:

```sh
UBSAN_OPTIONS=print_stacktrace=1 build/vorbis/fuzz -r out/vorbis/crash-1234abcd
FUZZ_V=1 build/vorbis/fuzz -r file.ogg              # format and frames decoded
FUZZ_PCM=out.raw build/vorbis/fuzz -r file.ogg      # the whole decoded output, for comparisons
FUZZ_NOFORK=1 gdb --args build/vorbis-heap/fuzz -r file.ogg
```

`FUZZ_PCM` also serves another check: decoding whole files before and after a change, to make
sure the output stays bit-exact.

## Results: Ogg, Vorbis and Opus (version 0.81, October 2026)

Twenty-five minutes per run, on a 12-thread PC:

| Target | Code | Runs | Crashes | Hangs |
|---|---|---|---|---|
| `ogg` | oggpage.c | 402,498 | 0 | 0 |
| `opus` | libopus 1.5.2 | 80,498 | 0 | 0 |
| `vorbis` | Tremor lowmem as in 0.80 | 54,721 | 5,059 | 4 |
| `vorbis` | 0.81, final (two runs) | 433,945 | 0 | 0 |

The Ogg reader and libopus showed nothing in any run. The Vorbis decoder needed four rounds of
fixes, each run finding what the previous fixes uncovered. All fixes are written here, from the
list of defects published by [micro-vorbis](https://github.com/esphome-libs/micro-vorbis) and
from libvorbis' own checks; no code was copied. Defects found:

- **Codebook setup** (`codebook.c`): heap overflows in `_make_words` (the tree outgrew its array),
  `marker[33]` indexed with a length of 33, shifts of 32 bits or more, an overflow or endless loop
  in `_book_maptype1_quantvals`, `dim` of 0 or up to 65,535 (decode vectors on core B's 8 KiB
  stack), unchecked allocations, a walk off the Huffman tree returning `-1` as an entry.
- **A failed setup header** (`info.c`): cleaning up freed pointers that were never written. This
  one was reachable by real files. The library has 18 tracks made by a libvorbis beta of 2000
  (floor 0, which this firmware does not decode). Refusing them caused a core B fault; they are
  now refused cleanly.
- **Floor 1** (`floor1.c`): a repeated x in the post list (division by zero), an early end of the
  packet read as `partitions = -1` (then `malloc(-1)`), values past the 256-entry dB table
  (`mult` 3 allows up to 258).
- **Residue** (`res012.c`): partition words past `partitions^dim` indexed past the stage masks.
- **Core B's allocator** (`heap.c`): `malloc(0xffffffff)` wrapped in its size computation and
  returned an 8-byte block.

Each fix was checked three ways:

- all 5,394 saved crashes and hangs now run clean;
- 26 whole tracks of a real library decode bit-exact before and after the fixes;
- on the ARM image, in the emulator, a reference file decodes bit-exact against 0.80.

## Results: MP3, AAC, MP4 and tags (version 0.82, October 2026)

First runs, 25 minutes per target:

| Target | Runs | Crashes | Hangs |
|---|---|---|---|
| `mp4` (core A) | 682,290 | 0 | 0 |
| `tags` (core A) | 614,384 | 0 | 0 |
| `aac` | 213,286 | 1,278 | 0 |
| `aacpkt` | 150,129 | 3,690 | 0 |
| `mp3` | 271,391 | 403 | 0 |

Our core A readers (`mp4.c`, `tags.c`) showed nothing. Every defect was in the two PacketVideo
decoders, or in how we fed them. Three rounds of fixes, all written here and marked
`ECHO MINI (ETUDE §63)`, with FAAD2, ffmpeg and the standard as references:

- **AAC, TNS** (`get_tns.cpp`): a negative start band went to an unsigned variable as a huge
  index. On the 32-bit target it lands 68 bytes before the band table, and `apply_tns` then
  writes with the wrong bounds. Now `max(top - length, 0)`, as the standard says.
- **AAC, pulses** (`getics.cpp`, `pulse_nc.cpp`): `pulse_start_sfb` (6 bits) past the bands, then
  writes past `coef[]`. Now refused, and no pulse goes past the last band.
- **AAC, inverse quantisation** (`huffspec_fxp.cpp`): an off-by-one (8192 let through, index
  1025 of a 1025-entry table), and a maximum that missed values left by a frame that failed part
  way. The maximum is now taken over all 1,024 coefficients.
- **AAC, escape codes** (`unpack_idx.cpp`): an unbounded escape prefix; now 12 bits at most, as
  the standard says.
- **AAC, configuration** (`codec_aac.c`, our code): the decoder's bit reader is not bounded, and
  a program config element read past our 64-byte buffer on the stack. The buffer now has 1 KiB
  of zeros after it.
- **AAC, PNS and intensity stereo** (`pns_intensity_right.cpp`): a window group array of another
  window sequence ran the pointers out of the decoder's memory. Now the loop stops on a group
  that does not progress.
- **MP3, Huffman regions** (`pvmp3_huffman_parsing.cpp`): region indexes up to 24 in a
  23-entry array (they read the next field). Now bounded to 22, as ffmpeg does.
- **MP3, sub-block gain** (`pvmp3_dequantize_sample.cpp`): a window index up to 4 for 3 windows.
  Now bounded to 2.

The TNS and pulse defects wrote out of bounds on the device. The quantisation, escape,
configuration and PNS defects read out of bounds. The two MP3 ones only gave wrong values on
already corrupt frames.

Final runs on 0.82: `aac` 261,817 runs, `aacpkt` 507,574, `mp3` 254,987, all without a crash or a
hang. All 5,443 saved crashes replay clean. The 34 M4A files of a real library and 16 MP3 files
decode bit-exact before and after the fixes. On the ARM image, in the emulator, an MP3 and two
M4A files decode bit-exact against 0.81.

## Results: FAT32 and exFAT (version 0.91, October 2026)

Six instances of the `fat` target for two hours each, on the seeds of `mkfatseeds.py`:
8,955,972 runs, **no crash and no hang**. Coverage stops growing after about 20 minutes (55 to
68 corpus entries per instance): more varied seeds (other cluster sizes, deeper and larger
folders, a damaged FAT) would be the next step.

## Limits

- The PC build is 64-bit and uses the C versions of the ARM assembly macros and routines (Tremor,
  libopus, the MP3 decoder's DCTs and polyphase window). Pointer-size and assembly-only faults are
  not covered: the device and the emulator remain the reference. Against the emulator, the PC
  output is bit-exact for AAC and within 1 LSB for MP3.
- In the PacketVideo decoders, shifts of 32 bits or more are not checked: they happen on valid
  files (`ms_synt.cpp`), and on ARM they are arithmetic (the sign), which is what the code means.
  `trans4m_freq_2_time_fxp.cpp` is built without ASan: its pipelined loops read one element
  before a table on their last pass, a value never used.
- Coverage guidance is simple (no comparison tracing, no dictionaries): checksums or magic values
  deep in a stream are reached slowly. The Ogg page CRC is not checked by our reader, which helps.
- FLAC, ALAC, Musepack, WAV and AIFF have no decoder target yet (their tags are covered by
  `tags`). Each would need a small entry in `build.sh` and `fuzz_host.c`.

## A lesson for the firmware build

While checking these fixes, an image built incrementally hung on a valid file. A field had been
added to a Tremor structure (`codec_internal.h`), but the firmware `Makefile` does not make
Tremor's objects depend on its headers. Only one file was rebuilt, and two others kept the old
layout. A clean build fixed it. When a header of a third-party decoder changes, rebuild from
clean (`rm -rf fw/build`).
