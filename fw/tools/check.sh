#!/bin/sh
# check.sh [--full]: the automatic checks of the firmware (make check, make check-full).
#   - the image builds, warnings as errors (the stricter WARN of the Makefile on our code);
#   - BSS leaves at least 2 KiB before the DAC ring;
#   - the log formats match their arguments (log.h, LOG_FORMAT_CHECK);
#   - the host tests of log.c and msc.c pass;
#   - ruff (or pyflakes) and codespell find nothing (when installed);
#   - the functions of ours the linker drops (dead code) are listed, not refused.
# --full also builds the test variants (ECHO_HWTEST, ECHO_HWSYN, ECHO_NO_HWFFT, ECHO_FFTCMP,
# ECHO_PROBE, SD_WALK) and the fuzzing harnesses (tools/fuzz/build.sh), so that they do not rot.
# Each build has its own folder under build/check/; build/ itself is left alone.
set -e
cd "$(dirname "$0")/.."
J="-j$(nproc 2>/dev/null || echo 4)"
C=build/check
FAIL=0
mkdir -p $C

say() { printf '\n== %s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*"; FAIL=1; }

# One build: folder name, EXTRA flags
build() {
    if make -s $J BUILD=$C/$1 EXTRA="$2 -Werror" > $C/$1.log 2>&1; then
        printf 'ok    %s\n' "$1"
    else
        bad "build $1 (see fw/$C/$1.log)"
        grep -E 'error|warning' $C/$1.log | head -20
    fi
}

say "image"
build prod ""
if [ -f $C/prod/fw.elf ]; then
    END=$(arm-none-eabi-nm $C/prod/fw.elf | awk '$3 == "_ebss" { print $1 }')
    ROOM=$((0x03020000 - 0x$END))
    if [ $ROOM -lt 2048 ]; then bad "BSS: $ROOM bytes left before the DAC ring"
    else printf 'ok    BSS: %u bytes left before the DAC ring\n' $ROOM; fi
fi

if [ "$1" = "--full" ]; then
    say "test variants"
    for v in ECHO_HWTEST ECHO_HWSYN ECHO_NO_HWFFT ECHO_FFTCMP ECHO_PROBE SD_WALK; do
        build "$v" "-D$v"
    done
fi

say "log formats"
# uint32_t is unsigned long on arm-none-eabi, which %u prints all the same: unsigned int here
N=0
for f in src/*.c; do
    arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -ffreestanding -Wall -Wextra -DLOG_FORMAT_CHECK \
        -U__UINT32_TYPE__ '-D__UINT32_TYPE__=unsigned int' -U__INT32_TYPE__ -D__INT32_TYPE__=int \
        -Isrc -I$C/prod -fsyntax-only "$f" 2> $C/fmt.log || true
    if grep -q 'Wformat' $C/fmt.log; then grep 'Wformat' $C/fmt.log; N=1; fi
done
if [ $N = 0 ]; then echo "ok    log_printf formats"; else bad "log_printf formats"; fi

say "host tests"
for t in log msc; do
    if cc -O1 -Wall -Werror -Isrc -o $C/${t}_test tools/${t}_test.c > $C/${t}_test.log 2>&1 &&
        $C/${t}_test >> $C/${t}_test.log 2>&1; then
        printf 'ok    %s_test\n' $t
    else
        bad "${t}_test (see fw/$C/${t}_test.log)"; tail -5 $C/${t}_test.log
    fi
done

if [ "$1" = "--full" ]; then
    say "fuzzing harnesses"
    for t in ogg vorbis vorbis-heap opus mp3 mp4 aac aacpkt tags fat; do
        if sh tools/fuzz/build.sh $t > $C/fuzz-$t.log 2>&1; then printf 'ok    fuzz %s\n' $t
        else bad "fuzz $t (see fw/$C/fuzz-$t.log)"; tail -5 $C/fuzz-$t.log; fi
    done
fi

say "Python tools and spelling"
# ruff (../ruff.toml) or else pyflakes; codespell (../.codespellrc): either may be missing
if command -v ruff > /dev/null; then
    if (cd .. && ruff check -q fw/tools tools); then echo "ok    ruff"; else bad ruff; fi
elif python3 -m pyflakes --version > /dev/null 2>&1; then
    if python3 -m pyflakes tools/*.py ../tools/*.py; then echo "ok    pyflakes"; else bad pyflakes; fi
else
    echo "skip  ruff, pyflakes (not installed)"
fi
if command -v codespell > /dev/null; then
    if (cd .. && codespell); then echo "ok    codespell"; else bad codespell; fi
else
    echo "skip  codespell (not installed)"
fi

say "dead code (dropped by the linker)"
if [ -f $C/prod/fw.elf ]; then
    arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb $(ls $C/prod/*.o | grep -v alacdec) -T link.ld \
        -Wl,--gc-sections -Wl,--print-gc-sections -nostdlib -lgcc -o $C/gc.elf 2>&1 |
        sed -n "s/.*removing unused section '\.text\.\([^']*\)' in file '[^']*\/\([^']*\)'/\2 \1/p" |
        grep -v -e '^font_ascii' -e '^alacdec' | sort > $C/dead.txt || true
    # fat_file_of: used by SD_WALK only
    grep -v ' fat_file_of$' $C/dead.txt | sed 's/^/      /' || true
    echo "      ($(grep -vc ' fat_file_of$' $C/dead.txt) function(s))"
fi

echo
if [ $FAIL = 0 ]; then echo "check: all good"; else echo "check: FAILED"; fi
exit $FAIL
