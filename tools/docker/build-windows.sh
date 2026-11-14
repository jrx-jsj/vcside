#!/bin/sh
# Cross-compiles VCSIDE for Windows x86_64 (inside vcside-windows).
#   sh tools/docker/build-windows.sh [build folder]
# Default folder: build-windows. SDL3 is built from source and linked statically:
# vcside.exe + data/ is the whole app.
set -e
DIR=${1:-build-windows}
cmake -S . -B "$DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/docker/mingw-w64.cmake -DVCSIDE_VENDORED_SDL=ON
cmake --build "$DIR" -j "${BUILD_JOBS:-4}"  # more jobs run Docker out of memory
