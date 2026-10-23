# ECHO MINI new firmware — step 3 test image

Bare-metal firmware for the main core of the SNOWSKY ECHO MINI (Rockchip RKNanoD).
Version 0.1 is a hardware bring-up image: boot, clocks, panel, keys, ADC, power, and an
installer for `*.IMG` firmware files. No sound, no USB.

## Build

    make [FONT_LANG=ja]

Needs `arm-none-eabi-gcc` and Python 3. Output: `build/HIFIEC70.IMG`.

The font is Fusion Pixel 12px proportional by TakWolf (`font/fusion-pixel-12px`, SIL Open
Font License 1.1, see `OFL.txt` and `LICENSES/` there): Latin, Greek, Cyrillic, kana, CJK
ideographs and Hangul, 37 690 glyphs. `tools/mkfusion.py` converts its BDF into a blob stored
in the image (header field 0x80), read glyph by glyph from the eMMC with a cache; printable
ASCII is also built into the code. Any copy of the image must ship with that license.

## Test in emulation

`tools/emu_fw.py` (Unicorn) loads the image the way the IDB loader does and models the
panel, ADC, GPIO and eMMC:

    tools/emu_fw.py build/HIFIEC70.IMG --png screen.png
    tools/emu_fw.py build/HIFIEC70.IMG --disk disk.img --keys 3000:MENU,3300:NONE,3600:PLAY,3900:NONE,12000:PLAY,12300:NONE --expect-fw IMAGE

## On the device

- Screen: firmware and clock, panel id, IDB geometry, internal volume, keys, battery, jacks.
- MENU: list of `*.IMG` files at the root of the internal memory; PLAY checks one
  (signature, header copy, size, CRC; no date or version check), PLAY again installs it.
- Power held 2 s: power off. VOL- held 3 s: reboot into the loader's USB mode
  (`rkdeveloptool`), left with the RST button.
- Recovery without this firmware: hold VOL- while plugging the USB cable at power-on.

Keep fallback images under names that the stock firmware and RECHO do not install by
themselves (not `HIFIECnn.IMG` nor `ECHOMINI.IMG`), e.g. `RECHO010.IMG`, `STOCK39.IMG`.
