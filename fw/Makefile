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

SRCS = src/start.c src/board.c src/lcd.c src/emmc.c src/mshc.c src/sd.c src/sdtest.c src/keys.c src/fat.c src/update.c src/usb.c src/msc.c src/log.c src/i2c.c src/audio.c src/hifi.c src/player.c src/tags.c src/ui.c src/browser.c src/queue.c src/playback.c src/playscreen.c src/menu.c src/resume.c src/uidraw.c src/main.c src/charge.c src/mp4.c src/font.c src/rtc.c build/font_ascii.c
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

build/hifi_pcm.elf: hifi/rt.c hifi/codec_pcm.c hifi/rt.h hifi/link.ld src/hifi_proto.h
	@mkdir -p build
	$(CC) $(HCFLAGS) hifi/rt.c hifi/codec_pcm.c $(HLDFLAGS) -Wl,-Map=build/hifi_pcm.map -o $@
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

# MP3: PacketVideo decoder of AOSP (third_party/pvmp3dec, unmodified, Apache-2.0), compiled as C++
# without exceptions or RTTI. PV_ARM_GCC_V5: its ARM fixed-point operations, and its assembly for
# the polyphase window, the 18-point MDCT and the DCTs (ETUDE §49), ported to Thumb-2 in
# hifi/mp3asm (the AOSP files are ARM mode)
PVMP3_DIR ?= ../third_party/pvmp3dec
PVMP3_SRCS := pvmp3_normalize pvmp3_alias_reduction pvmp3_crc pvmp3_decode_header pvmp3_decode_huff_cw \
              pvmp3_getbits pvmp3_dequantize_sample pvmp3_framedecoder pvmp3_get_main_data_size \
              pvmp3_get_side_info pvmp3_get_scale_factors pvmp3_mpeg2_get_scale_data \
              pvmp3_mpeg2_get_scale_factors pvmp3_mpeg2_stereo_proc pvmp3_huffman_decoding \
              pvmp3_huffman_parsing pvmp3_tables pvmp3_imdct_synth pvmp3_mdct_6 pvmp3_dct_6 \
              pvmp3_poly_phase_synthesis pvmp3_equalizer pvmp3_seek_synch pvmp3_stereo_proc \
              pvmp3_reorder pvmp3_polyphase_filter_window pvmp3_mdct_18 pvmp3_dct_9 pvmp3_dct_16
PVMP3_ASM := pvmp3_polyphase_filter_window pvmp3_mdct_18 pvmp3_dct_9 pvmp3_dct_16
PVMP3_OBJS := $(patsubst %,build/pvmp3/%.o,$(PVMP3_SRCS)) $(patsubst %,build/pvmp3/%_thumb.o,$(PVMP3_ASM))
PVMP3_CFLAGS = -mcpu=cortex-m3 -mthumb -O2 -g -ffreestanding -nostdlib -fno-exceptions -fno-rtti \
               -ffunction-sections -fdata-sections -w "-DOSCL_UNUSED_ARG(x)=(void)(x)" -DPV_ARM_GCC_V5 \
               -I$(PVMP3_DIR)/include -I$(PVMP3_DIR)/src

build/pvmp3/%_thumb.o: hifi/mp3asm/%_thumb.S
	@mkdir -p $(dir $@)
	$(CC) -mcpu=cortex-m3 -mthumb -Wa,-mimplicit-it=always -c $< -o $@

build/pvmp3/%.o: $(PVMP3_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CROSS)g++ $(PVMP3_CFLAGS) -c $< -o $@

build/hifi_mp3.elf: hifi/rt.c hifi/codec_mp3.c hifi/rt.h hifi/link.ld src/hifi_proto.h $(PVMP3_OBJS)
	$(CC) $(HCFLAGS) -I$(PVMP3_DIR)/include -I$(PVMP3_DIR)/src hifi/rt.c hifi/codec_mp3.c $(PVMP3_OBJS) \
	  $(HLDFLAGS) -Wl,-Map=build/hifi_mp3.map -o $@
	$(CROSS)size $@

# AAC-LC: PacketVideo decoder of AOSP (third_party/pvaacdec, unmodified, Apache-2.0), C++ without
# exceptions or RTTI, without AAC_PLUS (no SBR, no parametric stereo); -O3 (8 % fewer instructions
# than -O2 on the QEMU bench, ETUDE §54). Portable C fixed-point operations for now.
PVAAC_DIR ?= ../third_party/pvaacdec/src
PVAAC_OBJS := $(patsubst $(PVAAC_DIR)/%.cpp,build/pvaac/%.o,$(wildcard $(PVAAC_DIR)/*.cpp))
PVAAC_CFLAGS = -mcpu=cortex-m3 -mthumb -O3 -g -ffreestanding -nostdlib -fno-exceptions -fno-rtti \
               -ffunction-sections -fdata-sections -w -Wno-narrowing "-DOSCL_UNUSED_ARG(x)=(void)(x)" \
               -DOSCL_IMPORT_REF= -DOSCL_EXPORT_REF= -I$(PVAAC_DIR)

build/pvaac/%.o: $(PVAAC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CROSS)g++ $(PVAAC_CFLAGS) -c $< -o $@

build/hifi_aac.elf: hifi/rt.c hifi/codec_aac.c hifi/rt.h hifi/link.ld src/hifi_proto.h $(PVAAC_OBJS)
	$(CC) $(HCFLAGS) -DOSCL_IMPORT_REF= -I$(PVAAC_DIR) hifi/rt.c hifi/codec_aac.c $(PVAAC_OBJS) \
	  $(HLDFLAGS) -Wl,-Map=build/hifi_aac.map -o $@
	$(CROSS)size $@

# ALAC: Apple's reference decoder (third_party/alac, unmodified, Apache-2.0); the glue is C++
ALAC_DIR ?= ../third_party/alac/codec
ALAC_SRCS := ALACDecoder.cpp ALACBitUtilities.c EndianPortable.c ag_dec.c dp_dec.c matrix_dec.c
ALAC_OBJS := $(patsubst %,build/alac/%.o,$(basename $(ALAC_SRCS)))
ALAC_FLAGS = -mcpu=cortex-m3 -mthumb -O2 -g -ffreestanding -nostdlib -ffunction-sections \
             -fdata-sections -w -DTARGET_RT_LITTLE_ENDIAN=1 -I$(ALAC_DIR)

build/alac/%.o: $(ALAC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALAC_FLAGS) -c $< -o $@

build/alac/%.o: $(ALAC_DIR)/%.cpp hifi/alac_alloc.h
	@mkdir -p $(dir $@)
	$(CROSS)g++ $(ALAC_FLAGS) -fno-exceptions -fno-rtti -include hifi/alac_alloc.h -c $< -o $@

build/alac/codec_alac.o: hifi/codec_alac.cpp hifi/rt.h src/hifi_proto.h
	@mkdir -p $(dir $@)
	$(CROSS)g++ $(ALAC_FLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -c $< -o $@

build/hifi_alac.elf: hifi/rt.c hifi/rt.h hifi/link.ld src/hifi_proto.h build/alac/codec_alac.o $(ALAC_OBJS)
	$(CC) $(HCFLAGS) hifi/rt.c build/alac/codec_alac.o $(ALAC_OBJS) \
	  $(HLDFLAGS) -Wl,-Map=build/hifi_alac.map -o $@
	$(CROSS)size $@

build/codecs.bin: build/hifi_pcm.elf build/hifi_flac.elf build/hifi_mp3.elf build/hifi_vorbis.elf build/hifi_opus.elf build/hifi_aac.elf build/hifi_alac.elf tools/mkpack.py
	python3 tools/mkpack.py $@ 1:build/hifi_pcm.elf 2:build/hifi_flac.elf 3:build/hifi_mp3.elf \
	  4:build/hifi_vorbis.elf 5:build/hifi_opus.elf 7:build/hifi_aac.elf 8:build/hifi_alac.elf

$(OUT): build/fw.elf build/codecs.bin build/font.bin tools/mkimg.py
	python3 tools/mkimg.py build/fw.elf $@ --pack build/codecs.bin --font build/font.bin

clean:
	rm -rf build

.PHONY: all clean
