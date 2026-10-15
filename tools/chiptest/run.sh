#!/bin/sh
# Host test of the hook's chip (MI_AO) path: ./tools/chiptest/run.sh
set -e
cd "$(dirname "$0")/../.."
B=build/chiptest; mkdir -p $B
gcc -O2 -shared -fPIC -Isrc src/padsp_wrap.c -o $B/libpadsp_wrap.so -ldl
gcc -O2 -shared -fPIC tools/chiptest/fake_mi_ao.c -o $B/libmi_ao.so
gcc -O2 tools/chiptest/game.c -o $B/game -L$B -lmi_ao -lm -Wl,-rpath,'$ORIGIN'
gcc -O2 -Isrc tools/chiptest/feed.c -o $B/feed -lm
ok=0
for r in "48000 2" "22050 2" "32000 1" "44100 2"; do
  $B/feed
  MA_HOOK_LOG=$B/hook.log LD_PRELOAD=$PWD/$B/libpadsp_wrap.so $B/game $r || ok=1
done
rm -f /tmp/miniamp_bgm.shm
cat $B/hook.log
[ $ok = 0 ] && echo "chip path: all good" || { echo "chip path: FAILED"; exit 1; }
