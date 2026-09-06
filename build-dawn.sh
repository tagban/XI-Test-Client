#!/usr/bin/env bash
# Build and install Dawn (WebGPU). macOS and Linux counterpart to build-dawn.bat.
#
# Needs cmake, ninja, git and python3. On Linux it also needs the Vulkan and
# X11/xcb development headers - libvulkan-dev and libx11-xcb-dev at minimum.
# Without the second, the build runs for several minutes and then stops on a
# missing X11/Xlib-xcb.h deep inside dawn_native, which reads as a Dawn problem
# and is not one.
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d vendor/dawn ]; then
    echo "fetching dawn"
    git clone --depth 1 https://dawn.googlesource.com/dawn vendor/dawn
fi

# DAWN_SUPPORTS_CXX_MODULES is declined rather than left to Dawn's own probe.
# The probe compiles a module to decide, and GCC accepts one under -fmodules-ts,
# so it answers yes; CMake then has no way to scan the import graph for GCC and
# generation dies on the dawncpp_module target, after the whole configure has
# already run. Nothing here imports the module, so the honest answer is no. On
# clang the probe would have been right, but this costs it nothing.
cmake -S vendor/dawn -B build-dawn -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_FETCH_DEPENDENCIES=ON \
    -DDAWN_ENABLE_INSTALL=ON \
    -DDAWN_BUILD_SAMPLES=OFF \
    -DDAWN_BUILD_TESTS=OFF \
    -DTINT_BUILD_TESTS=OFF \
    -DTINT_BUILD_CMD_TOOLS=OFF \
    -DDAWN_ENABLE_OPENGLES=OFF \
    -DDAWN_ENABLE_DESKTOP_GL=OFF \
    -DDAWN_SUPPORTS_CXX_MODULES=False

cmake --build build-dawn

# DawnTargets.cmake only exists after installing, so find_package(Dawn) cannot
# use the build tree directly.
cmake --install build-dawn --prefix "$PWD/vendor/dawn-install"
echo "dawn installed to vendor/dawn-install"
