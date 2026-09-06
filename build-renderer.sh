#!/usr/bin/env bash
# Build the WebGPU renderer slice. Run build-dawn.sh first.
#
# SDL3 is not vendored for these platforms (the checked-in one is a Windows
# devel package), so install it first - "brew install sdl3" on macOS. On Linux,
# check the distribution actually carries it before assuming: Ubuntu 24.04 does
# not, and SDL3 has to be built from source there. It must be 3.x, because the
# code calls SDL_OpenAudioDeviceStream with a callback and SDL2 has no such
# function.
set -euo pipefail
cd "$(dirname "$0")"

cmake -S renderer -B build-renderer -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDawn_DIR="$PWD/vendor/dawn-install/lib/cmake/Dawn"

cmake --build build-renderer
echo
echo "run it with: ./build-renderer/moghouse-renderer"
echo "it should print which backend it got - on macOS that must say Metal, and"
echo "on Linux Vulkan. A Linux adapter named llvmpipe is Mesa's software"
echo "rasteriser: it draws correctly but says nothing about the real GPU path."
