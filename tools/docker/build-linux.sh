#!/bin/sh
# Builds VCSIDE for Linux and runs the tests (inside vcside-linux).
#   sh tools/docker/build-linux.sh [build folder] [cmake options...]
# Default folder: build-linux. tools/release.sh passes -DVCSIDE_VENDORED_SDL=ON, so SDL3 is
# linked statically and the executable needs nothing beyond glibc and OpenGL.
set -e
DIR=${1:-build-linux}
[ $# -gt 0 ] && shift
cmake -S . -B "$DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$DIR"
"./$DIR/vcside_tests"
