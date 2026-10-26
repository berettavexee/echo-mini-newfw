#!/bin/sh
# mkseeds.sh OUTDIR [MUSIC_DIR]
# Seeds for fuzz_host.c (docs/FUZZING.md): OUTDIR/vorbis, opus, mp3 and m4a. Synthetic streams
# made with ffmpeg (libvorbis, libopus, libmp3lame, its aac encoder) at several rates, channel
# counts and qualities; MP3 with and without the Xing/LAME frame and with ID3v2/ID3v1 tags; M4A
# with the moov box at the start and at the end, with tags. With MUSIC_DIR, also a few files of
# each format found there, remuxed without re-encoding (-c copy) and without cover art, 6 s from
# 20 s in. Vorbis, Opus and MP3 seeds are cut to 40,000 bytes (the headers and a few seconds of
# audio, so that each run stays short); M4A seeds are kept whole (their moov box may be at the
# end).
set -e
O=$1; M=$2
[ -n "$O" ] || { echo "usage: mkseeds.sh OUTDIR [MUSIC_DIR]"; exit 1; }
mkdir -p "$O/vorbis" "$O/opus" "$O/mp3" "$O/m4a"
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
src() { echo "-f lavfi -i sine=f=$2:r=$1:d=$3 -f lavfi -i anoisesrc=r=$1:d=$3:a=0.1 -filter_complex amix=inputs=2"; }
i=0
for spec in "44100 2 -b:a 128k" "44100 1 -q:a 4" "48000 2 -b:a 320k" "32000 2 -q:a 7" \
            "22050 1 -b:a 32k" "16000 2 -q:a 9" "11025 1 -b:a 16k" "8000 1 -b:a 8k"; do
  set -- $spec; i=$((i + 1))
  ff $(src $1 440 3) -ac $2 -c:a libmp3lame $3 $4 -id3v2_version 3 -write_id3v1 1 \
     -metadata title="Seed $i" -metadata artist=Fuzz -metadata album=Seeds -metadata track=$i \
     "$O/mp3/syn$i.mp3"
done
ff $(src 44100 440 3) -ac 2 -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0 "$O/mp3/noxing.mp3"
i=0
for spec in "44100 2 128k +faststart" "44100 1 64k -" "48000 2 192k +faststart" "22050 2 48k -" \
            "8000 1 16k -" "96000 2 256k -"; do
  set -- $spec; i=$((i + 1))
  mf=""; [ "$4" != "-" ] && mf="-movflags $4"
  ff $(src $1 330 3) -ac $2 -c:a aac -b:a $3 $mf -metadata title="Seed $i" -metadata artist=Fuzz \
     -metadata album=Seeds -metadata track=$i "$O/m4a/syn$i.m4a"
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
  i=0
  find "$M" -iname "*.mp3" | head -n 6 | while read -r f; do
    i=$((i + 1)); ff -ss 20 -t 6 -i "$f" -map 0:a -c copy "$O/mp3/m$i.mp3"
  done
  i=0
  find "$M" -iname "*.m4a" | head -n 4 | while read -r f; do
    i=$((i + 1)); ff -ss 20 -t 6 -i "$f" -map 0:a -c copy "$O/m4a/m$i.m4a"
  done
fi
for f in "$O"/vorbis/* "$O"/opus/* "$O"/mp3/*; do
  head -c 40000 "$f" > "$f.t" && mv "$f.t" "$f"
done
ls -l "$O/vorbis" "$O/opus" "$O/mp3" "$O/m4a"
