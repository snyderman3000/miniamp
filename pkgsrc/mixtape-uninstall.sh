#!/bin/sh
# Run by Mixtape before it erases MiniAmp (you can also run it by hand).
# Stops the background music and, if "Music in games" was on, puts OnionOS's
# original libpadsp.so back in /mnt/SDCARD/miyoo/lib.
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/lib:/config/lib:/customer/lib:$LD_LIBRARY_PATH"
export MA_HOME="$DIR"
"$DIR/miniamp" --uninstall
