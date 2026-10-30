# Third-party notices

The firmware image (`HIFIEC70.IMG`, released as `NEWFW0xx.IMG`) contains the following
third-party code and font, each under its own license. The full license texts are in the files
named below; they apply to the image as well as to the sources.

| Component | Used for | Copyright | License | License text |
|---|---|---|---|---|
| libopus 1.5.2 (decoder) | Opus | Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon (2001-2023) | BSD-3-Clause | `third_party/opus/COPYING` |
| Tremor (lowmem branch, patched) | Ogg Vorbis | Xiph.org Foundation (2002) | BSD-3-Clause | `third_party/tremor-lowmem/COPYING` |
| libmpcdec | Musepack | The Musepack Development Team (2005) | BSD-3-Clause | `third_party/libmpcdec/COPYING` |
| pvmp3dec (AOSP), and its ARM assembly ported to Thumb-2 in `fw/hifi/mp3asm/` | MP3 | PacketVideo (1998-2009), The Android Open Source Project (2005-2008) | Apache-2.0 | `third_party/pvmp3dec/NOTICE` |
| pvaacdec (AOSP) | AAC | PacketVideo, The Android Open Source Project (2005-2008) | Apache-2.0 | `third_party/pvaacdec/NOTICE` |
| Apple ALAC decoder, and `fw/hifi/alacdec.c` and `alacdec.h` derived from it | ALAC | Apple Inc. (2011) | Apache-2.0 | `third_party/alac/LICENSE` |
| dr_flac (mackron/dr_libs) | FLAC | David Reid | Public domain (Unlicense) or MIT-0 | end of `fw/hifi/ext/dr_flac.h` |
| Fusion Pixel 12 px font, with Ark Pixel, Cubic 11 and Galmuri glyphs | the text on screen | TakWolf (2021-2022), M+ FONTS PROJECT (2005), COZ (2002-2004), Lee Minseo (2019-2025) | SIL Open Font License 1.1 | `fw/font/fusion-pixel-12px/OFL.txt` and `fw/font/fusion-pixel-12px/LICENSES/` |

The files derived from Apache-2.0 code (`fw/hifi/mp3asm/*.S`, `fw/hifi/alacdec.c`,
`fw/hifi/alacdec.h`) stay under Apache-2.0 and say so in their header, with what was changed for
this firmware.

Everything else in this repository is under the MIT License (`LICENSE`).
