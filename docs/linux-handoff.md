# Building MogHouse on Linux

Written on the Mac for whoever picks this up on Linux, in the same spirit as
`docs/macos-handoff.md` was written on Windows for the Mac.

**This has now been done.** On 2026-09-05 it built and ran under WSL2 on Ubuntu
24.04, and the result is the `linux-x64` zip on the v0.2.0 release. What follows
was largely right; the five things it did not predict are in "What the first
build actually needed" below, and the fixes for all of them are in the tree. The
one claim still untested is hardware Vulkan — see the end.

Read `docs/macos-handoff.md` too, particularly "What happened on the Mac". Two
of the bugs found there were not macOS bugs at all, and one of them is very
likely waiting on Linux as well.

## Where this stands

Done already, and none of it needs redoing:

- **The renderer is portable.** `renderer/surface_linux.cpp` already exists
  alongside the Windows and Metal ones, and `renderer/CMakeLists.txt` selects
  it. Dawn picks Vulkan on Linux the same way it picks Metal on macOS.
- **The writable-state problem is fixed.** The client used to write
  `moghouse-settings.json`, `ffxi-server-profiles.json`, `ffxi-install.json`
  and its logs beside the executable, which fails inside a Flatpak because
  `/app` is read-only. `FfxiServerProfileStore.DefaultConfigDirectory()` now
  probes whether that directory is writable and falls back to
  `LocalApplicationData` — `$XDG_DATA_HOME`, which a Flatpak redirects into its
  own sandbox. Writing beside the executable still wins wherever it works, so
  the portable-zip behaviour is unchanged. Verified on macOS by making the
  directory read-only and watching it fall back correctly.
- **Logs now default.** They previously wrote nowhere at all unless
  `MOGHOUSE_LOG` was set, which meant a released build was silent — bad, given
  the README tells testers to attach both logs. They now default into the same
  writable directory, and the renderer still derives its own file by appending
  `.renderer`.
- **`flatpak/com.tagban.MogHouse.yml`** exists, with a `.desktop` and a
  metainfo file. Still UNTESTED — flatpak-builder has not been run.
- **`tools/package-linux.sh`** produces the `dist/linux-x64` tree the manifest
  packages. This one has now been run, and it was right: the only thing it was
  missing was the README the Windows package has always carried, which is why
  its `--version` flag changed nothing. Both are fixed.

Done since: the client compiles, packages and runs. It reads the game's DATs,
reaches a server, logs in, enters the world, and the renderer draws the zone.

## Setting up WSL2

WSL2 is the right first target on this machine: it is x86_64, which is what
Linux players actually run, and unlike an ordinary VM it has real GPU
passthrough — NVIDIA's WSL driver exposes working Vulkan, so the 4060 is
visible to Dawn. That matters, because "does the renderer come up on Vulkan" is
the one question a VM usually cannot answer.

    wsl --install Ubuntu-24.04

24.04 LTS rather than 26.04: the newer one is too fresh for the .NET and
Flatpak packaging to be predictable, and this is not the place to be debugging
the distro.

Flatpak needs systemd, which WSL does not enable by default. In the distro:

    sudo tee /etc/wsl.conf >/dev/null <<'EOF'
    [boot]
    systemd=true
    EOF

then `wsl --shutdown` from PowerShell and start it again.

**Check Vulkan before anything else.** If this does not name the GPU, stop and
fix it first — every later failure will be blamed on the renderer.

    sudo apt update
    sudo apt install -y vulkan-tools mesa-vulkan-drivers
    vulkaninfo --summary

On WSL2 with the NVIDIA driver installed on the *Windows* side (not inside the
distro — installing a Linux NVIDIA driver in WSL breaks it), this should list a
device. `dzn` or `lavapipe` appearing instead means it fell back to a software
or D3D12-translation path, which will run but tells you nothing trustworthy
about real Vulkan.

## The build, in order

Everything below wants these:

This is the list that actually worked on 24.04, rather than the short one this
document used to guess at:

    sudo apt install -y build-essential cmake ninja-build git python3 \
        patchelf pkg-config zip \
        libvulkan-dev vulkan-tools mesa-vulkan-drivers \
        libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxi-dev \
        libxcursor-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev \
        libxinerama-dev libwayland-dev wayland-protocols libdecor-0-dev \
        libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
        libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev \
        libxcb-xfixes0-dev libxcb-randr0-dev libxcb-shm0-dev libxcb-glx0-dev

Two of those names are traps. `libxcb-xfixes0-dev` and `libxcb-randr0-dev` carry
a `0` that the others do not, and apt fails the whole command over one wrong
name. `libx11-xcb-dev` is the one Dawn needs, and its absence does not surface
until several minutes into the build.

There is no `libsdl3-dev` in 24.04 — build SDL3 from source. It must be 3.x,
because the code uses `SDL_OpenAudioDeviceStream` with a callback, which SDL2
does not have. The Mac used 3.4.14 and so did this; matching avoids a variable:

    git clone --depth 1 --branch release-3.4.14 https://github.com/libsdl-org/SDL.git
    cmake -S SDL -B build-sdl3 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF
    cmake --build build-sdl3 && sudo cmake --install build-sdl3 && sudo ldconfig

Its configure stops on the first missing X11 package it finds and names only
that one, so expect to go round twice if the list above is trimmed.

**1. .NET 10.** Microsoft's apt feed, or the install script into `$HOME`, which
needs no root:

    curl -fsSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0
    export PATH="$HOME/.dotnet:$PATH"

**2. Dawn.** The long pole in wall-clock terms, though far less than
`docs/macos-handoff.md` feared — it took 3m41s at 745% CPU on the Mac.

    ./build-dawn.sh

That fetches Dawn into `vendor/dawn`, builds it, and installs to
`vendor/dawn-install`. Both are gitignored. On Linux it will also want Vulkan
headers and X11/Wayland development packages; add what it asks for rather than
guessing up front.

**3. The renderer.**

    ./build-renderer.sh

Note this uses `-S renderer`, not `-S .` — there is no root `CMakeLists.txt`,
and `docs/macos-handoff.md` was wrong about that until the Mac session fixed
it.

**4. The key tables and the water.** Neither ships in the repo.

    git submodule update --init ffxi-engine
    python3 tools/keytables.py

    # LandSandBoat, for the compression tables and the water source
    git clone --depth 1 https://github.com/LandSandBoat/server.git ~/LandSandBoat
    cd ~/LandSandBoat && git submodule update --init --depth 1 ximeshes && cd -
    python3 tools/makewater.py --root ~/LandSandBoat/ximeshes

`ximeshes` is a submodule of LandSandBoat and a plain clone leaves it empty —
that cost the Mac session a detour. It produced 185 zones of water in 30
seconds, about 55MB.

**5. The package.**

    tools/package-linux.sh --res ~/LandSandBoat/res --zone-data ~/LandSandBoat/data/zones

**6. The Flatpak.**

    sudo apt install -y flatpak flatpak-builder
    flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathubrepo.flatpakrepo
    flatpak install --user flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08
    flatpak-builder --user --install --force-clean build-flatpak flatpak/com.tagban.MogHouse.yml
    flatpak run com.tagban.MogHouse

## Prove the renderer before the client

The standalone renderer is the cheaper thing to debug, and it needs no Flatpak.
Note that `--frames 1` is **not a flag** — `renderer/main.cpp` reads `argv[1]`
as the zone path and takes everything else from the environment, and ignores
unknown arguments silently, so it will just sit in its interactive loop. The
one-frame check is:

    MOGHOUSE_FFXI_KEYTABLE=keys/mzb_key_table.bin \
    MOGHOUSE_FFXI_KEYTABLE2=keys/mmb_key_table2.bin \
    MOGHOUSE_FONT=renderer/assets \
    MOGHOUSE_SUBROOMS=renderer/assets/subrooms.txt \
    MOGHOUSE_SCREENSHOT=/tmp/shot.png MOGHOUSE_SCREENSHOT_AFTER=8 \
      ./build-renderer/moghouse-renderer "<path to a zone DAT>"

A clean run prints an adapter line, a zone line, writes the PNG and exits 0.
On the Mac that read:

    adapter: Apple M4 (Metal)
    window: 1280x720 points, 2560x1440 pixels
    zone f_sa: 41178 triangles

On Linux the adapter line must say **Vulkan**. Zero `webgpu error` is the bar —
all four WGSL modules compiled on Metal with no complaint, which is mild
evidence they are portable, but Vulkan's validation is stricter in different
places than Metal's.

## What the first build actually needed

Five things, none of them predicted here, all now fixed in the tree. Listed so
the next person on a fresh machine recognises them rather than debugging them.

1. **`build-dawn.sh` died in the generate step**, after a full configure, with
   *"the target named dawncpp_module has C++ sources that may use modules, but
   the compiler does not provide a way to discover the import graph"*. Dawn
   decides `DAWN_SUPPORTS_CXX_MODULES` by compiling a module, and GCC accepts
   one under `-fmodules-ts`, so the probe says yes — but CMake cannot scan the
   import graph for GCC, so generation then fails. Nothing here imports the
   module. The script now passes `-DDAWN_SUPPORTS_CXX_MODULES=False`.

2. **Dawn stopped mid-build on a missing `X11/Xlib-xcb.h`.** That is
   `libx11-xcb-dev`. It reads like a Dawn bug and is a missing package.

3. **Two missing includes**, invisible to MSVC and libc++ and fatal on
   libstdc++: `renderer/ffxi/entitynames.cpp` calls `std::memcpy` without
   `<cstring>`, and `native/moghouse_interop/src/moghouse_interop.cpp` calls
   `std::find` without `<algorithm>`. This is the same class of thing
   `fix-missing-includes.ps1` exists for.

4. **The link failed at the very last step** with a relocation error naming a
   mangled `std::vector` destructor and telling us to recompile with `-fPIC`.
   Both static libraries end up inside `libmoghouse_interop.so`, and Linux will
   not link non-PIC objects into a shared library. `renderer/CMakeLists.txt`
   now sets `CMAKE_POSITION_INDEPENDENT_CODE ON`. Dawn's own static library was
   already PIC, so it needed no rebuild. Windows has no such notion and macOS
   compiles PIC by default, which is why this waited for Linux to find it.

5. **SDL3 is not in Ubuntu 24.04 at all**, which this document suspected. Its
   configure also wants `libxtst-dev`, which nothing else does.

With those, `./build-dawn.sh`, `./build-renderer.sh` and
`tools/package-linux.sh` all run unmodified.

## What is most likely to break

Roughly in order, and honestly flagged rather than predicted.

**Vulkan device selection.** Dawn finding no adapter is the most likely single
failure, and on WSL2 it is usually the driver rather than the code. Check
`vulkaninfo --summary` first, every time, before reading any renderer source.

**A read-only `/app` surprise this fix did not cover.** The config and log
paths are handled, but anything else that writes relative to the executable
would fail the same way. If something dies on first run inside the Flatpak and
works outside it, this is the shape to look for.

**Audio.** SDL3 opens a stream for zone music. The manifest grants
`--socket=pulseaudio`, which covers PulseAudio and PipeWire's Pulse shim. WSL2
has no audio device at all by default, so expect audio to be absent there and
do not read that as a bug — test it on real hardware.

**Fractional scaling.** The Mac had a genuine points-versus-pixels bug that was
invisible until real geometry was on screen: the surface was configured in
points while the drawable was in pixels, so everything rendered at half
resolution and a flat clear colour looked identical either way. It is fixed
now, in a portable way — `SDL_GetWindowSizeInPixels` plus
`SDL_WINDOW_HIGH_PIXEL_DENSITY` — but Linux under fractional scaling is the
other place that split exists, so it is worth confirming rather than assuming.
The `window: WxH points, WxH pixels` line the renderer prints is the check.

## Two bugs from the Mac that are not macOS bugs

Both are already fixed, and both are worth knowing because the same class of
thing will happen again.

**`viewer.cpp` had a conditional whose branches were a `const char*` and a
`bool`.** MSVC accepted it through pointer-to-bool and computed the right
answer by accident; Clang rejected it outright. GCC will likely reject it too
if anything similar remains. The lesson is that "it compiles on Windows" is not
evidence that it is valid C++.

**The renderer segfaulted on every exit.** `~Music()` destroyed an SDL audio
stream *after* `SDL_Quit()` had already torn the audio subsystem down, so it
locked a freed mutex. That is undefined on every platform — Windows simply
survived it. Linux may or may not. If the renderer crashes on quit with a
stack in SDL, this is the shape, and the fix pattern is
`Music::shutdown()` called explicitly before `SDL_Quit`.

## The thing this project keeps relearning

From `docs/macos-handoff.md`, and it earned its place again on the Mac: **two
halves of one client agreeing tells you they were written by the same person,
not that either is right.** Validate against something you did not produce.

It bit once more during the macOS packaging, in a new disguise. The first
x86_64 bundle was built with an **arm64 SDL3 inside it** — because the lookup
fell through to Homebrew's copy, which only exists for one architecture. It
signed cleanly. It verified cleanly. `codesign --verify --deep --strict`
passed. Every check the build could run on itself agreed, and the bundle could
not have loaded on the only machines it was built for.

The fix was not a better lookup — it was adding a check that reads the answer
off the artifact rather than trusting the process that made it:
`package-macos.sh` now runs `lipo -archs` over every binary and refuses to
package a mixed bundle. `package-linux.sh` does the same with `file`. Keep
that check. The build machine is always the one architecture that happens to
work, so this is exactly the bug that cannot be caught by running it locally.

## Where the release stands

macOS is finished: signed, notarized, stapled, verified as a downloader sees
it, for both arm64 and x86_64. `tools/package-macos.sh` builds them and
`tools/notarize-macos.sh` submits and staples.

Linux now has a portable zip — `MogHouse-XI-Alpha-0.2.0-linux-x64.zip`, built
by `tools/package-linux.sh` and attached to the v0.2.0 release. No Flatpak yet,
and no signing, which Linux does not ask for the way macOS does.

The natural next step is still putting the build in CI — free x86_64 runners,
reproducible artifacts, and no dependence on any one machine being booted into
the right OS. Dawn is the only slow part and it caches well.

**The Vulkan question is still open.** The first build ran under WSL2, where the
only adapter offered was `llvmpipe` — Mesa's software rasteriser. It drew the
zone correctly, which proves the Vulkan backend is wired up and the shaders
compile, but says nothing about a real driver. WSL had `/dev/dxg` and
`libd3d12.so` present but no `dzn` ICD installed, so there was no hardware path
to take. A machine with an AMD, Intel or NVIDIA driver still has to confirm
this once, and that remains the one thing CI cannot do either.
