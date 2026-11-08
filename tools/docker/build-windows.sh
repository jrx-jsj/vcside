#!/bin/sh
# Cross-compiles VCSIDE for Windows x86_64 into build-windows/ (inside vcside-windows).
# SDL3 is built from source and linked statically: vcside.exe + data/ is the whole app.
set -e
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/docker/mingw-w64.cmake -DVCSIDE_VENDORED_SDL=ON
cmake --build build-windows
