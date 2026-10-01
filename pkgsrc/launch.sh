#!/bin/sh
# MiniAmp for Miyoo Mini Plus (OnionOS)
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
LOG="$DIR/log.txt"
[ -f "$LOG" ] && mv "$LOG" "$DIR/log.prev.txt"
echo "===== MiniAmp $(cat "$DIR/VERSION" 2>/dev/null) $(date)" > "$LOG"

# Sound: OnionOS plays OSS (/dev/dsp) audio through its audioserver when
# libpadsp.so is preloaded.
for p in /customer/lib/libpadsp.so /mnt/SDCARD/miyoo/lib/libpadsp.so /mnt/SDCARD/.tmp_update/lib/libpadsp.so; do
  if [ -f "$p" ]; then export LD_PRELOAD="$p"; break; fi
done
echo "audio: LD_PRELOAD=${LD_PRELOAD:-none} audioserver=$(ps | grep -c [a]udioserver)" >> "$LOG"
export LD_LIBRARY_PATH="$DIR/lib:/config/lib:/customer/lib:$LD_LIBRARY_PATH"
export MA_HOME="$DIR"
# performance numbers in log.txt every 10 seconds (comment out to silence)
export MA_PROFILE=1

"$DIR/miniamp" >> "$LOG" 2>&1
echo "miniamp exited with code $?" >> "$LOG"
sync
