#!/bin/sh
# Builds the Miyoo binary and assembles dist/MiniAmp-miyoo-vX.zip (App/MiniAmp).
set -e
cd "$(dirname "$0")"
VERSION="$(cat VERSION)"
TC="${TOOLCHAIN:-/opt/mini}"
./build.sh miyoo
python3 skin/make_skin.py skins/Graphite.wsz
OUT=pkg/App/MiniAmp
rm -rf pkg && mkdir -p $OUT/lib $OUT/skins $OUT/fonts $OUT/licenses
cp build/miniamp $OUT/
cp build/libpadsp_wrap.so $OUT/lib/
# zlib from the toolchain's sysroot (buildroot or the union toolchain layout)
for z in "$TC/arm-buildroot-linux-gnueabihf/sysroot/usr/lib/libz.so.1" "$TC/arm-linux-gnueabihf/libc/usr/lib/libz.so.1"; do
  if [ -e "$z" ]; then cp -L "$z" $OUT/lib/libz.so.1; break; fi
done
test -f $OUT/lib/libz.so.1
"$TC/bin/arm-linux-gnueabihf-strip" $OUT/lib/libz.so.1
cp skins/Graphite.wsz $OUT/skins/
cp fonts/Rubik-400.ttf fonts/Rubik-700.ttf fonts/ChakraPetch-Bold.ttf fonts/DejaVuSans.ttf $OUT/fonts/
cp pkgsrc/* $OUT/
echo "$VERSION" > $OUT/VERSION
cp LICENSE $OUT/licenses/LICENSE-MiniAmp.txt
cp THIRD_PARTY_NOTICES.md fonts/OFL-Rubik.txt fonts/OFL-ChakraPetch.txt fonts/LICENSE-DejaVu.txt $OUT/licenses/
chmod +x $OUT/launch.sh $OUT/miniamp
ZIP="MiniAmp-miyoo-v$VERSION.zip"
mkdir -p dist
(cd pkg && rm -f "../dist/$ZIP" && zip -qr9 "../dist/$ZIP" App)
du -sh $OUT; ls -la "dist/$ZIP"
