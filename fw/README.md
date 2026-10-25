# ECHO MINI new firmware — sources

Bare-metal firmware for the SNOWSKY ECHO MINI (Rockchip RKNanoD): the main core (`src/`) and
our codecs on the HiFi core (`hifi/`). The study behind it is `../ETUDE.md` (in French).

## Build

    make [FONT_LANG=ja] [EXTRA=-DSD_WALK]

Needs `arm-none-eabi-gcc` and Python 3. Output: `build/HIFIEC70.IMG` (FW1 image: main-core
code, the pack of our codec images, the font). The codec sources are in `../third_party`.
`EXTRA=-DSD_WALK` builds an emulator test of the microSD card (`src/sdtest.c`).

The font is Fusion Pixel 12px proportional by TakWolf (`font/fusion-pixel-12px`, SIL Open
Font License 1.1, see `OFL.txt` and `LICENSES/` there): Latin, Greek, Cyrillic, kana, CJK
ideographs and Hangul, 37 690 glyphs. `tools/mkfusion.py` converts its BDF into a blob stored
in the image, read glyph by glyph from the eMMC with a cache; printable ASCII is also built
into the code. Any copy of the image must ship with that license.

## Main-core sources

| File | Role |
|---|---|
| `main.c` | start-up, storage, system screens, firmware installer and USB screens |
| `keys.c` | key ladder debouncing; events for the screens (press, long, repeat, chord, plugs in/out) |
| `ui.c` | the main loop: events to the screen shown, drawing, power saving (shared state: `ui_priv.h`) |
| `browser.c` | top level (internal memory, microSD card), folder listings, path, browser screen and keys |
| `queue.c`, `playback.c` | play queue and modes; tracks from the queue, ReplayGain, volume, seek |
| `playscreen.c`, `menu.c`, `uidraw.c` | play screen and its keys; main menu, date and time; drawing helpers |
| `resume.c` | resume state in the last sector of the log file |
| `player.c` | track playback: core B control, input and output rings, gapless chaining, rate changes, core B clock |
| `mp4.c` | MP4 audio (M4A): boxes, sample tables read through small windows, gapless info (iTunSMPB, edit list), seek; the frames sent to core B as a packet stream |
| `audio.c` | the two CS43131 DACs, I2S1, DMAC1 ring in SYSRAM, volume |
| `hifi.c` | core B power, loading of our codec images, start |
| `tags.c` | ID3v2/ID3v1, Vorbis comments, RIFF INFO, AIFF chunks, MP4 `ilst` (iTunes), ReplayGain |
| `fat.c` | FAT32 and exFAT, read only (internal memory and card) |
| `mshc.c`, `emmc.c`, `sd.c` | SD/MMC host controller, eMMC, microSD card |
| `usb.c`, `msc.c` | USB device and mass storage |
| `lcd.c`, `font.c` | 320 x 170 panel, text |
| `log.c` | log ring in PMU-SRAM, written to `NEWFWLOG.TXT` |
| `rtc.c`, `i2c.c`, `board.c` | real-time clock, I2C, clocks and GPIO |

## Core B codecs

Each is an image of its own (`hifi/rt.c`, the runtime, plus one decoder), loaded into the HiFi
core's RAM when a track needs it; the protocol with core A is `src/hifi_proto.h`.

| Image | Formats | Decoder |
|---|---|---|
| `codec_pcm.c` | WAV, AIFF, AIFF-C (uncompressed, 8 to 32 bits) | ours |
| `codec_flac.c` | FLAC | dr_flac (`ext/`) |
| `codec_mp3.c` | MP3 | PacketVideo `pvmp3dec` (AOSP, Apache-2.0), Thumb-2 assembly in `mp3asm/` |
| `codec_vorbis.c` | Ogg Vorbis | Tremor lowmem (BSD) |
| `codec_opus.c` | Opus | libopus 1.5.2 (BSD) |
| `codec_aac.c` | AAC-LC (from M4A, read by `src/mp4.c`) | PacketVideo `aacdec` (AOSP, Apache-2.0), without AAC+ |

## Test in emulation

`tools/emu_fw.py` (Unicorn) loads the image the way the IDB loader does and models the panel,
keys, eMMC, microSD card, DACs, DMA and core B:

    tools/mkdisk.py disk.img build/HIFIEC70.IMG --boot BOOTDUMP.BIN music.flac=A.FLAC
    tools/emu_fw.py build/HIFIEC70.IMG --disk disk.img [--sd card.img] \
        --keys 3000:MENU,3150:NONE --shots 5000:screen.png --dac heard.raw

`mkdisk.py` needs a dump of the device's boot area (`--boot`, or `$ECHO_BOOTDUMP`), which is
not part of this repository. `tools/exfat_add.py` fills an exFAT image made by `mkfs.exfat`;
`tools/readlog.py` decodes `NEWFWLOG.TXT`.

## On the device

Install `build/HIFIEC70.IMG` (renamed, e.g. `NEWFW061.IMG`) from the main menu ("Install a
firmware"), or through the loader's USB mode. Keep fallback images under names that the stock
firmware and RECHO do not install by themselves (not `HIFIECnn.IMG` nor `ECHOMINI.IMG`), e.g.
`STOCK39.IMG`. Recovery without this firmware: hold VOL- while plugging the USB cable at
power-on.
