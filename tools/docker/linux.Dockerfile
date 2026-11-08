# Linux build of VCSIDE (and a screen-less run with Mesa for test shots).
#   docker build -t vcside-linux -f tools/docker/linux.Dockerfile tools/docker
#   docker run --rm -v "$PWD":/src -w /src vcside-linux sh tools/docker/build-linux.sh
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates pkg-config python3 xz-utils \
      libsdl3-dev libgl-dev libgl1-mesa-dri xvfb xauth imagemagick dasm fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*
