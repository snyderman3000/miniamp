# Building MiniAmp

## Desktop test build

    ./build.sh host

`build/miniamp-host` runs without a window: it dumps frames as PPM files and
reads key presses from a script (see `tools/run_host.sh`). Audio goes to a WAV
file when `MA_WAV_OUT` is set.

    MA_ROOT=~/Music tools/run_host.sh keys.txt out 300

Test the music-in-games mixer (resampling to each game's rate):

    gcc -O2 -Isrc tools/test_mix.c -o build/test_mix -lm && build/test_mix

## Miyoo Mini Plus

Needs the Miyoo Mini toolchain (`TOOLCHAIN`, default `/opt/mini`) and the
SigmaStar MI_GFX/MI_SYS headers and libraries (`MI_SDK`, a folder with `inc/`
and `lib/`, e.g. from steward-fu's SDL2 port).

    MI_SDK=/path/to/mini ./package.sh

The union Miyoo Mini toolchain (`miyoomini-toolchain.tar.xz` from
shauninman/miyoomini-toolchain-buildroot) includes those headers and libraries in
`arm-linux-gnueabihf/libc/usr`, so `MI_SDK` can be a folder whose `inc` and `lib`
link to that sysroot's `include` and `lib`.

This builds `build/miniamp`, regenerates the Graphite skin and writes
`dist/MiniAmp-miyoo-vX.Y.Z.zip`.
