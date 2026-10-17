#!/bin/sh
# MiniAmp for Miyoo Mini Plus (OnionOS)
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
LOG="$DIR/log.txt"
[ -f "$LOG" ] && mv "$LOG" "$DIR/log.prev.txt"
echo "===== MiniAmp $(cat "$DIR/VERSION" 2>/dev/null) $(date)" > "$LOG"

# Sound: OnionOS plays OSS (/dev/dsp) audio through its own audioserver
# (miyoo/app/audioserver) when its libpadsp.so (miyoo/lib) is preloaded, as it
# does for the menu and every game. The firmware's copy in /customer/lib
# doesn't work with that audioserver on every model (silent on the Mini v4),
# so it's only the last resort.
ONION_LIB=/mnt/SDCARD/miyoo/lib
mkdir -p "$DIR/data"
is_wrapper() { grep -q MINIAMP_PADSP_WRAPPER "$1" 2>/dev/null; }
if [ -f "$ONION_LIB/libpadsp.so" ] && ! is_wrapper "$ONION_LIB/libpadsp.so"; then
  # keep a copy of OnionOS's library, in case it's ever needed back
  cmp -s "$ONION_LIB/libpadsp.so" "$DIR/data/libpadsp_onion.so" || cp -f "$ONION_LIB/libpadsp.so" "$DIR/data/libpadsp_onion.so"
fi
LD_PRELOAD=
# "Music in games" on: miyoo/lib/libpadsp.so is MiniAmp's wrapper and
# OnionOS's own library is next to it as libpadsp_orig.so
for p in "$ONION_LIB/libpadsp_orig.so" "$ONION_LIB/libpadsp.so" "$DIR/data/libpadsp_onion.so" \
         /mnt/SDCARD/.tmp_update/lib/libpadsp.so /customer/lib/libpadsp.so; do
  if [ -f "$p" ] && ! is_wrapper "$p"; then LD_PRELOAD="$p"; break; fi
done
[ -n "$LD_PRELOAD" ] && export LD_PRELOAD
echo "audio: LD_PRELOAD=${LD_PRELOAD:-none} audioserver=$(ps | grep -c [a]udioserver)" >> "$LOG"
export LD_LIBRARY_PATH="$DIR/lib:/config/lib:/customer/lib:$LD_LIBRARY_PATH"
export MA_HOME="$DIR"
export MA_VERSION="$(cat "$DIR/VERSION" 2>/dev/null)"
# performance numbers in log.txt every 10 seconds (comment out to silence)
export MA_PROFILE=1

"$DIR/miniamp" >> "$LOG" 2>&1
echo "miniamp exited with code $?" >> "$LOG"
sync
