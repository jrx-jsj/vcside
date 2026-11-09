# Linux build of VCSIDE (and a screen-less run with Mesa for test shots).
#   docker build -t vcside-linux -f tools/docker/linux.Dockerfile tools/docker
#   docker run --rm -v "$PWD":/src -w /src vcside-linux sh tools/docker/build-linux.sh
# The X11/Wayland headers are for builds that compile SDL3 themselves (releases).
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates pkg-config python3 xz-utils \
      libsdl3-dev libgl-dev libgl1-mesa-dri xvfb xauth imagemagick dasm fonts-dejavu-core \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
      libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libdrm-dev libgbm-dev libdecor-0-dev libdbus-1-dev libudev-dev \
 && rm -rf /var/lib/apt/lists/*
