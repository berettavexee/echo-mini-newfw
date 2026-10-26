#!/bin/sh
# mkseeds.sh OUTDIR [MUSIC_DIR]
# Seeds for fuzz_host.c (docs/FUZZING.md): OUTDIR/vorbis and OUTDIR/opus. Synthetic streams made
# with ffmpeg (libvorbis, libopus) at several rates, channel counts and qualities; with MUSIC_DIR,
# also up to 6 Vorbis and 5 Opus files found there, remuxed without re-encoding (-c copy) and
# without tags or cover art, 6 s from 20 s in. Every seed is cut to 40,000 bytes: the headers and
# a few seconds of audio, so that each run stays short.
set -e
O=$1; M=$2
[ -n "$O" ] || { echo "usage: mkseeds.sh OUTDIR [MUSIC_DIR]"; exit 1; }
mkdir -p "$O/vorbis" "$O/opus"
ff() { ffmpeg -loglevel error -y "$@"; }
i=0
for spec in "44100 2 5" "44100 1 0" "48000 2 8" "22050 2 2" "8000 1 -1" "96000 2 4"; do
  set -- $spec; i=$((i + 1))
  ff -f lavfi -i "sine=f=440:r=$1:d=4" -f lavfi -i "anoisesrc=r=$1:d=4:a=0.1" \
     -filter_complex amix=inputs=2 -ac $2 -c:a libvorbis -q:a $3 "$O/vorbis/syn$i.ogg"
done
i=0
for spec in "48000 2 96k" "48000 1 24k" "48000 2 12k" "24000 2 32k"; do
  set -- $spec; i=$((i + 1))
  ff -f lavfi -i "sine=f=330:r=$1:d=4" -f lavfi -i "anoisesrc=r=$1:d=4:a=0.1" \
     -filter_complex amix=inputs=2 -ac $2 -ar 48000 -c:a libopus -b:a $3 "$O/opus/syn$i.opus"
done
if [ -n "$M" ]; then
  i=0
  find "$M" -iname "*.ogg" | head -n 6 | while read -r f; do
    i=$((i + 1)); ff -ss 20 -t 6 -i "$f" -map 0:a -c copy -map_metadata -1 "$O/vorbis/m$i.ogg"
  done
  i=0
  find "$M" -iname "*.opus" | head -n 5 | while read -r f; do
    i=$((i + 1)); ff -ss 20 -t 6 -i "$f" -map 0:a -c copy -map_metadata -1 "$O/opus/m$i.opus"
  done
fi
for f in "$O"/vorbis/* "$O"/opus/*; do
  head -c 40000 "$f" > "$f.t" && mv "$f.t" "$f"
done
ls -l "$O/vorbis" "$O/opus"
