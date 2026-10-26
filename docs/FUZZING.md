# Fuzzing the core B Ogg decoders

A music player reads files it did not make: interrupted copies, damaged cards, odd encoders,
files built to crash a decoder. On this player a decoder fault stops core B (the HiFi core), and a
wild write can corrupt what the other core reads. This page describes the fuzzing bench that tests
our Ogg code against such input on the PC, what it found, and how to run it. The detailed study,
in French, is `ETUDE.md` §62.

The tools are in [`fw/tools/fuzz/`](../fw/tools/fuzz/):

| File | Role |
|---|---|
| `fuzz_host.c` | The fuzzer and the stand-in for core B's runtime |
| `build.sh` | Builds one target: `ogg`, `vorbis`, `opus` or `vorbis-heap` |
| `mkseeds.sh` | Makes the seed files |

## What is tested

The code that runs on core B, unchanged, built for the PC:

- `fw/hifi/oggpage.c`: our Ogg page and packet reader (target `ogg`);
- `fw/hifi/codec_vorbis.c` with Tremor lowmem (`third_party/tremor-lowmem`), the Vorbis decoder
  (target `vorbis`);
- `fw/hifi/codec_opus.c` with libopus 1.5.2 in fixed point (target `opus`).

Each run opens the input as core B does (`codec_open()`), then decodes up to 400 blocks
(`codec_run()`).

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
  truncation, deleted and duplicated runs. Three positions out of four fall in the first 12 KiB,
  where the identification, comment and setup headers are: the Vorbis setup header holds the
  codebooks, the most complex code to parse.
- **Faithful to the target**: `-funsigned-char` as on ARM (a `char` index that is negative on the
  PC is not on the device). The `vorbis-heap` target keeps core B's own allocator
  (`fw/hifi/heap.c`) in place of the C library's. ASan cannot see inside that allocator, but some
  faults only show with it, for instance freeing an uninitialised pointer.

## Running it

Requirements: Linux, gcc with libasan and libubsan, GNU make, ffmpeg with libvorbis and libopus
(for the seeds).

```sh
cd fw/tools/fuzz
./mkseeds.sh seeds                  # synthetic seeds; add a music folder: ./mkseeds.sh seeds ~/Music
./build.sh vorbis                   # or ogg, opus, vorbis-heap
build/vorbis/fuzz seeds/vorbis out/vorbis 1500     # 25 minutes
```

`mkseeds.sh` encodes a few seconds of tone and noise at several rates, channel counts and
qualities. Given a music folder, it also takes up to 6 Vorbis and 5 Opus files from it. These are
remuxed without re-encoding and without tags or cover art, so that the seed holds audio and not a
cover picture. Every seed is cut to 40,000 bytes. Seeds and findings stay out of the repository
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

## Results (version 0.81, October 2026)

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

## Limits

- The PC build is 64-bit and uses the C versions of the ARM assembly macros. Pointer-size and
  assembly-only faults are not covered: the device and the emulator remain the reference.
- Coverage guidance is simple (no comparison tracing, no dictionaries): checksums or magic values
  deep in a stream are reached slowly. The Ogg page CRC is not checked by our reader, which helps.
- Only the Ogg codecs have a target so far. MP3, FLAC, AAC, ALAC and Musepack would need their
  own small entry in `build.sh` and `fuzz_host.c`.

## A lesson for the firmware build

While checking these fixes, an image built incrementally hung on a valid file. A field had been
added to a Tremor structure (`codec_internal.h`), but the firmware `Makefile` does not make
Tremor's objects depend on its headers. Only one file was rebuilt, and two others kept the old
layout. A clean build fixed it. When a header of a third-party decoder changes, rebuild from
clean (`rm -rf fw/build`).
