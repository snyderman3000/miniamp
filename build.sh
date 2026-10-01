#!/bin/sh
# Builds MiniAmp.
#   ./build.sh host     desktop test build (build/miniamp-host): frames dumped as PPM, keys scripted
#   ./build.sh miyoo    Miyoo Mini Plus build (build/miniamp)
# The Miyoo build needs the Miyoo toolchain (TOOLCHAIN, default /opt/mini) and
# the MI_GFX/MI_SYS headers and libraries (MI_SDK, see docs/BUILDING.md).
set -e
cd "$(dirname "$0")"
mkdir -p build
SRC="src/main.c src/audio.c src/decode.c src/meta.c src/skin.c src/zip.c src/font.c src/vis.c src/playlist.c third_party/mini2d.c"
WARN="-Wall -Wno-format-truncation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable"
case "${1:-host}" in
host)
  gcc -O2 -g -DM2D_HOST $WARN -Isrc -Ithird_party $SRC -o build/miniamp-host -lz -lm -lpthread
  ;;
miyoo)
  TC="${TOOLCHAIN:-/opt/mini}"
  M="${MI_SDK:-../sdl2/mini}"
  CC="$TC/bin/arm-linux-gnueabihf-gcc"
  $CC -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -ffast-math $WARN -Isrc -Ithird_party -I$M/inc \
      $SRC -o build/miniamp -L$M/lib -lmi_gfx -lmi_sys -lmi_common -lz -lm -lpthread -ldl
  "$TC/bin/arm-linux-gnueabihf-strip" build/miniamp
  ;;
esac
ls -la build/
