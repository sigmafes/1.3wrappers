#!/bin/sh
# Crea la carpeta portable 13GDPS/ y su .zip a partir de build-win/.
set -e
cd "$(dirname "$0")/.."
OUT=dist/13GDPS
rm -rf dist && mkdir -p "$OUT"
x86_64-w64-mingw32-strip -o "$OUT/13GDPS.exe" build-win/gdhost.exe
cp build-win/SDL2.dll build-win/SDL2_mixer.dll "$OUT/"
cp "$(find /usr -name libwinpthread-1.dll -path '*x86_64*' | head -1)" "$OUT/"
(cd dist && zip -qr 13GDPS.zip 13GDPS)
ls -la dist
