#!/bin/sh
# Descarga las dependencias de Windows y compila gdhost.exe con MinGW (desde Linux).
# Requiere: g++-mingw-w64-x86-64-posix, cmake, ninja, libboost-dev.
set -e
cd "$(dirname "$0")/.."
mkdir -p external/win && cd external/win
[ -d SDL2-2.30.9 ] || curl -sSL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz | tar xz
[ -d SDL2_mixer-2.8.0 ] || curl -sSL https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.0/SDL2_mixer-devel-2.8.0-mingw.tar.gz | tar xz
if [ ! -d zlib-win ]; then
  curl -sSL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz | tar xz
  cmake -S zlib-1.3.1 -B zlib-1.3.1/build-win -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_INSTALL_PREFIX="$PWD/zlib-win" -DCMAKE_BUILD_TYPE=Release
  ninja -C zlib-1.3.1/build-win install
fi
mkdir -p boost-inc && ln -sfn /usr/include/boost boost-inc/boost
cd ../..
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw64.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR="$PWD/external/win/boost-inc" -DCMAKE_CXX_FLAGS="-Wno-error"
ninja -C build-win gdhost
