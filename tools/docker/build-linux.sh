#!/bin/sh
# Builds VCSIDE for Linux in build-linux/ and runs the tests (inside vcside-linux).
set -e
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
./build-linux/vcside_tests
