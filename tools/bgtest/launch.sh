#!/bin/sh
# Background music test: launch once to start the music, launch again to stop it.
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/log.txt"
PIDF="$DIR/bg.pid"
if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
  kill "$(cat "$PIDF")"
  rm -f "$PIDF"
  echo "===== stopped by launcher $(date)" >> "$LOG"
  exit 0
fi
echo "===== BG music test $(date)" > "$LOG"
for p in /customer/lib/libpadsp.so /mnt/SDCARD/miyoo/lib/libpadsp.so /mnt/SDCARD/.tmp_update/lib/libpadsp.so; do
  if [ -f "$p" ]; then export LD_PRELOAD="$p"; break; fi
done
echo "audio: LD_PRELOAD=${LD_PRELOAD:-none} audioserver=$(ps | grep -c [a]udioserver)" >> "$LOG"
export LD_LIBRARY_PATH="/config/lib:/customer/lib:$LD_LIBRARY_PATH"
"$DIR/bgtest" start "$LOG" "$PIDF"
sleep 1
