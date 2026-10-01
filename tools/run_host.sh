#!/bin/sh
# Runs the desktop build with scripted keys and dumps frames.
#   tools/run_host.sh KEYS_FILE OUT_DIR MAX_FRAMES [DUMP_EVERY]
# KEYS_FILE lines: "<frame> <key>" (key: up down left right a b x y l r l2 r2 start select menu)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
KEYS="$1"; OUT="$2"; MAX="$3"; EVERY="${4:-5}"
mkdir -p "$OUT"
rm -f "$OUT"/*.ppm
python3 - "$KEYS" "$OUT/input.txt" <<'PY'
import sys
codes = dict(up=103, down=108, left=105, right=106, a=57, b=29, x=42, y=56, l=18, r=20, l2=15, r2=14,
             start=28, select=97, menu=1)
ev = []
for line in open(sys.argv[1]):
    line = line.split('#')[0].split()
    if not line: continue
    f, k = int(line[0]), line[1]
    hold = int(line[2]) if len(line) > 2 else 2
    ev.append((f, codes[k], 1)); ev.append((f + hold, codes[k], 0))
ev.sort()
open(sys.argv[2], 'w').write(''.join('%d %d %d\n' % e for e in ev))
PY
MA_HOME="$HERE" MA_DATA="${MA_DATA:-$OUT/data}" MA_ROOT="${MA_ROOT:-$HERE}" M2D_DUMP="$OUT" M2D_DUMP_EVERY="$EVERY" \
M2D_INPUT="$OUT/input.txt" MA_MAX_FRAMES="$MAX" "$HERE/build/miniamp-host"
