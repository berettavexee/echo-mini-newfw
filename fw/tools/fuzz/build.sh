#!/bin/sh
# build.sh ogg|vorbis|opus|vorbis-heap [TREMOR_DIR]
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
CF="-O1 -g -w -std=gnu99 -funsigned-char -I$FW/hifi -I$FW/src"
OUT=build/$T; mkdir -p $OUT
REN=""
TREM_SRC=""
for f in bitwise codebook dsp floor1 floor_lookup info mapping0 mdct misc res012; do TREM_SRC="$TREM_SRC $TREM/$f.c"; done
case $T in
ogg)    DEF=-DT_OGG; INC=""; SRC="$FW/hifi/oggpage.c";;
vorbis) DEF="-DT_VORBIS -DECHO_NO_FLOOR0 -DECHO_HEAP_ALLOCA"; INC="-I$TREM"
        SRC="$FW/hifi/oggpage.c $FW/hifi/codec_vorbis.c $TREM_SRC";;
vorbis-heap)
        DEF="-DT_VORBIS -DREAL_HEAP -DECHO_NO_FLOOR0 -DECHO_HEAP_ALLOCA"; INC="-I$TREM"; SAN=""
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
*) echo "usage: build.sh ogg|vorbis|opus|vorbis-heap [TREMOR_DIR]"; exit 1;;
esac
OBJS=""
for s in $SRC; do
  o=$OUT/$(basename $(dirname $s))_$(basename $s .c).o
  gcc $CF $SAN $COV $DEF $INC $REN -c $s -o $o; OBJS="$OBJS $o"
done
gcc $CF $SAN $DEF $INC fuzz_host.c $OBJS -o $OUT/fuzz
echo "$(pwd)/$OUT/fuzz"
