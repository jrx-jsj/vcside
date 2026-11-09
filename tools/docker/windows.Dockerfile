# Windows build of VCSIDE, cross-compiled with MinGW-w64 (static, a single .exe),
# and its installer, made with NSIS (tools/windows/vcside.nsi).
#   docker build -t vcside-windows -f tools/docker/windows.Dockerfile tools/docker
#   docker run --rm -v "$PWD":/src -w /src vcside-windows sh tools/docker/build-windows.sh
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates python3 xz-utils \
      g++-mingw-w64-x86-64-posix mingw-w64-tools nsis \
 && rm -rf /var/lib/apt/lists/*
