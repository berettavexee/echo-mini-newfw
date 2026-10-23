# Build:  make [FONT_LANG=ja|zh_hans|zh_hant|zh_hk|zh_tw|ko|latin]
# Font: Fusion Pixel 12px proportional (SIL OFL 1.1, font/fusion-pixel-12px); the language
# variant only changes the shape of some CJK ideographs (only ja is kept in font/).
FONT_LANG ?= ja
FONT_BDF  = font/fusion-pixel-12px/fusion-pixel-12px-proportional-$(FONT_LANG).bdf
OUT   ?= build/HIFIEC70.IMG

CROSS  = arm-none-eabi-
CC     = $(CROSS)gcc
CFLAGS = -mcpu=cortex-m3 -mthumb -Os -g -ffreestanding -fno-builtin -nostdlib \
         -ffunction-sections -fdata-sections -Wall -Wextra -Isrc $(EXTRA)
LDFLAGS = -T link.ld -Wl,--gc-sections -Wl,-Map=build/fw.map -nostdlib -lgcc

SRCS = src/start.c src/board.c src/lcd.c src/emmc.c src/mshc.c src/sd.c src/fat.c src/update.c src/usb.c src/msc.c src/log.c src/i2c.c src/audio.c src/hifi.c src/player.c src/tags.c src/ui.c src/main.c src/font.c src/rtc.c build/font_ascii.c
OBJS = $(patsubst %.c,build/%.o,$(notdir $(SRCS)))

all: $(OUT)

build/font.bin build/font_ascii.c: tools/mkfusion.py $(FONT_BDF)
	@mkdir -p build
	python3 tools/mkfusion.py $(FONT_BDF) build/font.bin build/font_ascii.c

build/%.o: src/%.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/font_ascii.o: build/font_ascii.c src/font.h
	$(CC) $(CFLAGS) -c $< -o $@

build/fw.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(CROSS)size $@

# Core B codec images (our runtime + one decoder each), packed into the image
HCFLAGS = -mcpu=cortex-m3 -mthumb -O2 -g -ffreestanding -fno-builtin -nostdlib \
          -ffunction-sections -fdata-sections -Wall -Wextra
HLDFLAGS = -T hifi/link.ld -Wl,--gc-sections -nostdlib -lgcc

build/hifi_wav.elf: hifi/rt.c hifi/codec_wav.c hifi/rt.h hifi/link.ld src/hifi_proto.h
	@mkdir -p build
	$(CC) $(HCFLAGS) hifi/rt.c hifi/codec_wav.c $(HLDFLAGS) -Wl,-Map=build/hifi_wav.map -o $@
	$(CROSS)size $@

build/hifi_flac.elf: hifi/rt.c hifi/codec_flac.c hifi/ext/dr_flac.h hifi/rt.h hifi/link.ld src/hifi_proto.h
	@mkdir -p build
	$(CC) $(HCFLAGS) -Wno-unused-function hifi/rt.c hifi/codec_flac.c $(HLDFLAGS) -Wl,-Map=build/hifi_flac.map -o $@
	$(CROSS)size $@

# Opus: libopus 1.5.2, the variant validated on the device (third_party/opus: official release +
# encoder-only functions stubbed under ECHO_DECODER_ONLY), fixed point, ARM macros
OPUS_DIR ?= ../third_party/opus
include $(OPUS_DIR)/celt_sources.mk
include $(OPUS_DIR)/silk_sources.mk
include $(OPUS_DIR)/opus_sources.mk
OPUS_SRCS := $(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FIXED) $(OPUS_SOURCES)
OPUS_OBJS := $(patsubst %.c,build/opus/%.o,$(OPUS_SRCS))
OPUS_DEFS := -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DNONTHREADSAFE_PSEUDOSTACK \
             -DCUSTOM_SUPPORT -DGLOBAL_STACK_SIZE=16384 -DDEC_MEM_SIZE=26624 \
             -DOPUS_ARM_INLINE_ASM -DECHO_DECODER_ONLY
OPUS_INCS := -Ihifi/opus -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk \
             -I$(OPUS_DIR)/silk/fixed -I$(OPUS_DIR)
OPUS_CFLAGS = -mcpu=cortex-m3 -mthumb -Os -g -std=gnu99 -ffreestanding -nostdlib \
              -ffunction-sections -fdata-sections -w $(OPUS_DEFS) $(OPUS_INCS)

build/opus/%.o: $(OPUS_DIR)/%.c hifi/opus/custom_support.h
	@mkdir -p $(dir $@)
	$(CC) $(OPUS_CFLAGS) -c $< -o $@

build/hifi_opus.elf: hifi/rt.c hifi/oggpage.c hifi/codec_opus.c hifi/rt.h hifi/oggpage.h hifi/link.ld src/hifi_proto.h $(OPUS_OBJS)
	$(CC) $(HCFLAGS) $(OPUS_DEFS) $(OPUS_INCS) hifi/rt.c hifi/oggpage.c hifi/codec_opus.c $(OPUS_OBJS) \
	  $(HLDFLAGS) -Wl,-Map=build/hifi_opus.map -o $@
	$(CROSS)size $@

# Vorbis: Tremor lowmem 89a7534 + our patches (third_party/tremor-lowmem), full precision, ARM
# macros and ssat (as HIFIEC56)
TREMOR_DIR ?= ../third_party/tremor-lowmem
TREMOR_SRCS := bitwise.c codebook.c dsp.c floor1.c floor_lookup.c info.c mapping0.c mdct.c misc.c res012.c
TREMOR_OBJS := $(patsubst %.c,build/tremor/%.o,$(TREMOR_SRCS))
TREMOR_DEFS := -DECHO_NO_FLOOR0 -DECHO_HEAP_ALLOCA -D_ARM_ASSEM_ -DECHO_SSAT
TREMOR_CFLAGS = -mcpu=cortex-m3 -mthumb -Os -g -std=gnu99 -ffreestanding -nostdlib \
                -ffunction-sections -fdata-sections -Wa,-mimplicit-it=thumb -w $(TREMOR_DEFS) -I$(TREMOR_DIR)

build/tremor/%.o: $(TREMOR_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TREMOR_CFLAGS) -c $< -o $@

build/hifi_vorbis.elf: hifi/rt.c hifi/heap.c hifi/oggpage.c hifi/codec_vorbis.c hifi/rt.h hifi/oggpage.h hifi/link.ld src/hifi_proto.h $(TREMOR_OBJS)
	$(CC) $(HCFLAGS) -Wa,-mimplicit-it=thumb $(TREMOR_DEFS) -I$(TREMOR_DIR) hifi/rt.c hifi/heap.c hifi/oggpage.c \
	  hifi/codec_vorbis.c $(TREMOR_OBJS) $(HLDFLAGS) -Wl,-Map=build/hifi_vorbis.map -o $@
	$(CROSS)size $@

# MP3: PacketVideo decoder of AOSP (third_party/pvmp3dec, unmodified, Apache-2.0), portable C
# (the AOSP ARM assembly is ARM mode, not Thumb-2), compiled as C++ without exceptions or RTTI
PVMP3_DIR ?= ../third_party/pvmp3dec
PVMP3_SRCS := pvmp3_normalize pvmp3_alias_reduction pvmp3_crc pvmp3_decode_header pvmp3_decode_huff_cw \
              pvmp3_getbits pvmp3_dequantize_sample pvmp3_framedecoder pvmp3_get_main_data_size \
              pvmp3_get_side_info pvmp3_get_scale_factors pvmp3_mpeg2_get_scale_data \
              pvmp3_mpeg2_get_scale_factors pvmp3_mpeg2_stereo_proc pvmp3_huffman_decoding \
              pvmp3_huffman_parsing pvmp3_tables pvmp3_imdct_synth pvmp3_mdct_6 pvmp3_dct_6 \
              pvmp3_poly_phase_synthesis pvmp3_equalizer pvmp3_seek_synch pvmp3_stereo_proc \
              pvmp3_reorder pvmp3_polyphase_filter_window pvmp3_mdct_18 pvmp3_dct_9 pvmp3_dct_16
PVMP3_OBJS := $(patsubst %,build/pvmp3/%.o,$(PVMP3_SRCS))
PVMP3_CFLAGS = -mcpu=cortex-m3 -mthumb -O2 -g -ffreestanding -nostdlib -fno-exceptions -fno-rtti \
               -ffunction-sections -fdata-sections -w "-DOSCL_UNUSED_ARG(x)=(void)(x)" \
               -I$(PVMP3_DIR)/include -I$(PVMP3_DIR)/src

build/pvmp3/%.o: $(PVMP3_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CROSS)g++ $(PVMP3_CFLAGS) -c $< -o $@

build/hifi_mp3.elf: hifi/rt.c hifi/codec_mp3.c hifi/rt.h hifi/link.ld src/hifi_proto.h $(PVMP3_OBJS)
	$(CC) $(HCFLAGS) -I$(PVMP3_DIR)/include -I$(PVMP3_DIR)/src hifi/rt.c hifi/codec_mp3.c $(PVMP3_OBJS) \
	  $(HLDFLAGS) -Wl,-Map=build/hifi_mp3.map -o $@
	$(CROSS)size $@

build/codecs.bin: build/hifi_wav.elf build/hifi_flac.elf build/hifi_mp3.elf build/hifi_vorbis.elf build/hifi_opus.elf tools/mkpack.py
	python3 tools/mkpack.py $@ 1:build/hifi_wav.elf 2:build/hifi_flac.elf 3:build/hifi_mp3.elf \
	  4:build/hifi_vorbis.elf 5:build/hifi_opus.elf

$(OUT): build/fw.elf build/codecs.bin build/font.bin tools/mkimg.py
	python3 tools/mkimg.py build/fw.elf $@ --pack build/codecs.bin --font build/font.bin

clean:
	rm -rf build

.PHONY: all clean
