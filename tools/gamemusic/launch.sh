#!/bin/sh
# Game music test: launch once to start (music plays in the menu and in games),
# launch again to stop and put OnionOS's original sound library back.
DIR="$(cd "$(dirname "$0")" && pwd)"
LIB=/mnt/SDCARD/miyoo/lib
LOG="$DIR/log.txt"
PIDF="$DIR/svc.pid"

restore() {
  if [ -f "$LIB/libpadsp_orig.so" ]; then
    cp "$LIB/libpadsp_orig.so" "$LIB/libpadsp.so.tmp" && mv "$LIB/libpadsp.so.tmp" "$LIB/libpadsp.so" && rm -f "$LIB/libpadsp_orig.so"
    echo "original libpadsp.so restored" >> "$LOG"
  fi
}

if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
  kill "$(cat "$PIDF")"
  rm -f "$PIDF"
  echo "===== stopped $(date)" >> "$LOG"
  restore
  cat "$DIR/hook.log" >> "$LOG" 2>/dev/null
  sync
  exit 0
fi

echo "===== Game music test $(date)" > "$LOG"
: > "$DIR/hook.log"
if [ ! -f "$LIB/libpadsp_orig.so" ]; then
  if cmp -s "$LIB/libpadsp.so" "$DIR/libpadsp_wrap.so"; then
    # wrapper already in place without a backup: use the firmware's copy
    cp /customer/lib/libpadsp.so "$LIB/libpadsp_orig.so"
  else
    cp "$LIB/libpadsp.so" "$LIB/libpadsp_orig.so"
  fi
fi
cp "$DIR/libpadsp_wrap.so" "$LIB/libpadsp.so.tmp" && mv "$LIB/libpadsp.so.tmp" "$LIB/libpadsp.so"
echo "installed wrapper; orig md5 $(md5sum "$LIB/libpadsp_orig.so" | cut -d' ' -f1)" >> "$LOG"
unset LD_PRELOAD
"$DIR/gmservice" start "$LOG" "$PIDF"
sync
