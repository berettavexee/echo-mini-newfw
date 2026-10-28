#!/bin/sh
# build.sh ogg|vorbis|opus|vorbis-heap|mp3|mp4|aac|aacpkt|tags|fat [TREMOR_DIR]
# Host builds of the core B Ogg code for fuzz_host.c (docs/FUZZING.md). ogg, vorbis and opus:
# ASan + UBSan (signed shifts and Tremor's deliberate overflows left out), trace-pc coverage on
# the code under test only. vorbis-heap: core B's own hifi/heap.c in place of libc malloc, no
# ASan (it cannot see inside that heap), for faults that only that allocator shows.
# -funsigned-char as on ARM. Output: build/<target>/fuzz next to this script. Paths are relative
# to this script's directory (the repository path may contain spaces): TREMOR_DIR too.
set -e
cd "$(dirname "$0")"
T=$1; FW=../..; TP=../../../third_party; TREM=${2:-$TP/tremor-lowmem}
SAN="-fsanitize=address,undefined -fno-sanitize=shift-base,signed-integer-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer"
COV="-fsanitize-coverage=trace-pc"
CF="${OPT:--O1} -g -w -std=gnu99 -funsigned-char -I$FW/hifi -I$FW/src"
OUT=build/$T; mkdir -p $OUT
REN=""
TREM_SRC=""
for f in bitwise codebook dsp floor1 floor_lookup info mapping0 mdct misc res012; do TREM_SRC="$TREM_SRC $TREM/$f.c"; done
case $T in
ogg)    DEF=-DT_OGG; INC=""; SRC="$FW/hifi/oggpage.c";;
vorbis) DEF="-DT_VORBIS -DECHO_NO_HWFFT -DECHO_NO_FLOOR0 -DECHO_HEAP_ALLOCA"; INC="-I$TREM"
        SRC="$FW/hifi/oggpage.c $FW/hifi/codec_vorbis.c $TREM_SRC";;
vorbis-heap)
        DEF="-DT_VORBIS -DECHO_NO_HWFFT -DREAL_HEAP -DECHO_NO_FLOOR0 -DECHO_HEAP_ALLOCA"; INC="-I$TREM"; SAN=""
        REN="-Dmalloc=hmalloc -Dfree=hfree -Dcalloc=hcalloc -Drealloc=hrealloc"
        SRC="$FW/hifi/oggpage.c $FW/hifi/codec_vorbis.c $FW/hifi/heap.c $TREM_SRC";;
opus)   OD=$TP/opus
        DEF="-DT_OPUS -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DNONTHREADSAFE_PSEUDOSTACK -DCUSTOM_SUPPORT -DGLOBAL_STACK_SIZE=16384 -DDEC_MEM_SIZE=26624 -DECHO_DECODER_ONLY"
        INC="-I$FW/hifi/opus -I$OD/include -I$OD/celt -I$OD/silk -I$OD/silk/fixed -I$OD"
        SRC="$FW/hifi/oggpage.c $FW/hifi/codec_opus.c"
        for v in CELT_SOURCES SILK_SOURCES SILK_SOURCES_FIXED OPUS_SOURCES; do
          SRC="$SRC $(make -s -f - -C $OD print V=$v <<MK
include celt_sources.mk
include silk_sources.mk
include opus_sources.mk
print: ; @echo \$(addprefix $OD/,\$(\$(V)))
MK
)"; done;;
mp4)    DEF=-DT_MP4; INC="-I$FW/src"; SRC="$FW/src/mp4.c";;
tags)   DEF=-DT_TAGS; INC="-I$FW/src"; SRC="$FW/src/tags.c";;
fat)    DEF=-DT_FAT; INC="-I$FW/src"; SRC="$FW/src/fat.c";;
aac|aacpkt)
        PA=$TP/pvaacdec/src
        DEF="-DT_$(echo $T | tr a-z A-Z) -DOSCL_IMPORT_REF= -DOSCL_EXPORT_REF="; INC="-I$FW/src -I$PA"
        SRC="$FW/hifi/codec_aac.c $(ls $PA/*.cpp)"
        [ $T = aac ] && SRC="$FW/src/mp4.c $SRC";;
mp3)    PM=$TP/pvmp3dec
        DEF=-DT_MP3; INC="-I$PM/include -I$PM/src"
        SRC="$FW/hifi/codec_mp3.c $(ls $PM/src/*.cpp)";;
*) echo "usage: build.sh ogg|vorbis|opus|vorbis-heap|mp3|mp4|aac|aacpkt|tags|fat [TREMOR_DIR]"; exit 1;;
esac
OBJS=""
# the PacketVideo decoders are C++ (no exceptions, no RTTI), as in the firmware Makefile, but
# without PV_ARM_GCC_V5: their portable C fixed-point operations instead of the ARM ones
CXXF="${OPT:--O1} -g -w -funsigned-char -fno-exceptions -fno-rtti -Wno-narrowing -DOSCL_UNUSED_ARG(x)=(void)(x)"
# Files built without ASan (UBSan and coverage kept): pipelined loops that read one element past
# a table on their last pass, a value never used (docs/FUZZING.md, "Limits")
NOASAN="trans4m_freq_2_time_fxp.cpp"
SAN_NA=$(echo "$SAN" | sed 's/address,//')
for s in $SRC; do
  case " $NOASAN " in *" $(basename $s) "*) S=$SAN_NA;; *) S=$SAN;; esac
  case $s in
  *.cpp) o=$OUT/$(basename $(dirname $s))_$(basename $s .cpp).o
         # shifts of 32 bits or more: arithmetic on ARM (sign fill), used by the PV decoders on
         # valid files (ms_synt.cpp); not a memory error, so not checked here
         g++ $CXXF $S -fno-sanitize=shift $COV $DEF $INC -c $s -o $o;;
  *)     o=$OUT/$(basename $(dirname $s))_$(basename $s .c).o
         gcc $CF $S $COV $DEF $INC $REN -c $s -o $o;;
  esac
  OBJS="$OBJS $o"
done
gcc $CF $SAN $DEF $INC fuzz_host.c $OBJS -o $OUT/fuzz
echo "$(pwd)/$OUT/fuzz"
