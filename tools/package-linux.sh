#!/usr/bin/env bash
# Build the linux-x64 tree that flatpak/com.tagban.MogHouse.yml packages.
#
# First run on Linux was 2026-09-05, under WSL2 on Ubuntu 24.04; it produced the
# linux-x64 zip attached to the v0.2.0 release. flatpak-builder is still
# untested. Read docs/linux-handoff.md for what that first build needed.
#
# Produces dist/linux-x64/ holding:
#
#   MogHouse XI                    one file: the .NET runtime and every managed
#                                  assembly published inside it
#   libmoghouse_interop.so         the renderer
#   libSDL3.so.0                   unless the runtime provides it
#   README.txt                     what the player is looking at
#   assets/  keys/  res/  zones/
#
# Everything sits in one directory because the renderer looks for its assets
# beside whichever directory its library was loaded from, and the client looks
# for AppContext.BaseDirectory then BaseDirectory/data. One flat directory
# satisfies both, which is why this is simpler than the macOS bundle.
set -euo pipefail

VERSION="0.2.1"
OUTPUT="dist/linux-x64"
RES=""
ZONEDATA=""
NO_WATER=0
NO_BUILD=0
RENDERER_BUILD=""

usage() {
    cat <<'USAGE'
Usage: tools/package-linux.sh [options]

  --version X.Y.Z     stamped into the README
  --output DIR        where the tree is written (default: dist/linux-x64)
  --res DIR           LandSandBoat res/ with compress.dat and decompress.dat
  --zone-data DIR     LandSandBoat data/zones, for zone lines
  --no-water          leave the ~55MB of water surfaces out
  --no-build          use whatever is already in the renderer build tree
  --renderer-build D  the renderer's build tree (default: <root>/build-renderer)
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        --res) RES="$2"; shift 2 ;;
        --zone-data) ZONEDATA="$2"; shift 2 ;;
        --no-water) NO_WATER=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --renderer-build) RENDERER_BUILD="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/$OUTPUT"
[ -n "$RENDERER_BUILD" ] || RENDERER_BUILD="$root/build-renderer"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    ! %s\033[0m\n' "$1"; }

# --- the native renderer -----------------------------------------------------

if [ "$NO_BUILD" -eq 0 ]; then
    step "Building the renderer"
    # A cmake that finds no compiler reports success having done nothing, which
    # leaves a stale library in the package - so check the timestamp moved.
    lib="$RENDERER_BUILD/moghouse_interop/libmoghouse_interop.so"
    before=$(stat -c %Y "$lib" 2>/dev/null || echo 0)
    cmake --build "$RENDERER_BUILD"
    after=$(stat -c %Y "$lib" 2>/dev/null || echo 0)
    [ "$after" = "0" ] && { echo "no library at $lib" >&2; exit 1; }
    [ "$before" = "$after" ] && warn "the renderer library did not change - already up to date, or nothing was rebuilt"
fi

native="$RENDERER_BUILD/moghouse_interop"
[ -f "$native/libmoghouse_interop.so" ] || { echo "libmoghouse_interop.so is missing from $native" >&2; exit 1; }

# --- the managed client ------------------------------------------------------

step "Publishing the client"
rm -rf "$out"
mkdir -p "$out"

dotnet publish "$root/src/MogHouse.App/MogHouse.App.csproj" \
    -c Release -r linux-x64 --self-contained true \
    -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -p:EnableCompressionInSingleFile=true \
    -p:DebugType=none -p:GenerateDocumentationFile=false \
    -o "$out" --nologo -v quiet

find "$out" -name '*.pdb' -delete 2>/dev/null || true
[ -f "$out/MogHouse XI" ] || { echo "the published executable is not where it was expected" >&2; exit 1; }
chmod +x "$out/MogHouse XI"

# --- the renderer and its libraries ------------------------------------------

step "Copying the renderer"
cp "$native/libmoghouse_interop.so" "$out/"

# Whether SDL3 has to travel depends on where it came from. The freedesktop
# runtime a Flatpak builds against does not ship SDL3, so a distro SDL3 linked
# from /usr/lib will not be there at runtime - copy it in and make the loader
# look beside itself. If SDL3 came from a Flatpak SDK extension instead, this
# finds nothing and correctly does nothing.
sdl_path=$(ldd "$out/libmoghouse_interop.so" 2>/dev/null | awk '/libSDL3/ {print $3; exit}')
if [ -n "${sdl_path:-}" ] && [ -f "$sdl_path" ]; then
    cp -L "$sdl_path" "$out/$(basename "$sdl_path")"
    if command -v patchelf >/dev/null 2>&1; then
        patchelf --set-rpath '$ORIGIN' "$out/libmoghouse_interop.so"
        echo "    bundled $(basename "$sdl_path") and set rpath to \$ORIGIN"
    else
        warn "patchelf is not installed - libSDL3 was copied but the rpath was not set,"
        warn "so the loader will still look in the system path. Install patchelf."
    fi
else
    warn "SDL3 was not found by ldd - assuming the runtime provides it"
fi

# --- assets ------------------------------------------------------------------

step "Copying assets"
mkdir -p "$out/assets"
cp "$root/renderer/assets/font."* "$out/assets/"
cp "$root/renderer/assets/subrooms.txt" "$out/assets/"
cp "$root/renderer/assets/hidden-models.txt" "$out/assets/"
cp "$root/renderer/assets/burrowers.txt" "$out/assets/"

# --- where bug reports go -----------------------------------------------------

# `/bug` posts to a Discord webhook, and a tester has no way to configure one -
# so a shipped build has to carry it. Taken from MOGHOUSE_BUG_WEBHOOK or from
# the per-user copy, and written into data/ beside the runtime.
#
# In the build, not in the repository. Anything shipped can be taken apart, so
# this URL should be treated as public to anyone holding the client - which is
# survivable for a webhook, because the worst it allows is posting into one
# channel and it is revoked in a click. A repository is different: it is
# permanently searchable and scraped, and history keeps what you delete.
#
# Without one, /bug still writes its local file and says nobody has seen it.
step "Bug report webhook"
webhook="${MOGHOUSE_BUG_WEBHOOK:-}"
if [ -z "$webhook" ] && [ -f "$HOME/Library/Application Support/MogHouse/bug-webhook.txt" ]; then
    webhook="$(cat "$HOME/Library/Application Support/MogHouse/bug-webhook.txt")"
fi
if [ -z "$webhook" ] && [ -f "$HOME/.local/share/MogHouse/bug-webhook.txt" ]; then
    webhook="$(cat "$HOME/.local/share/MogHouse/bug-webhook.txt")"
fi
if [ -n "$webhook" ]; then
    printf '%s\n' "$webhook" > "$out/bug-webhook.txt"
    echo "    bug-webhook.txt (reports will reach the channel)"
else
    warn "No bug webhook: /bug will write its local file and go no further. Set MOGHOUSE_BUG_WEBHOOK to include one."
fi


if [ "$NO_WATER" -eq 1 ]; then
    warn "No water: canals and seas will be dry."
else
    count=0
    if [ -d "$root/renderer/assets/water" ]; then
        count=$(find "$root/renderer/assets/water" -name '*.water' | wc -l | tr -d ' ')
    fi
    if [ "$count" = "0" ]; then
        warn "No .water files - run 'python3 tools/makewater.py --root <LandSandBoat>/ximeshes' first."
    else
        mkdir -p "$out/assets/water"
        cp "$root/renderer/assets/water/"*.water "$out/assets/water/"
        echo "    $count zones of water"
    fi
fi

step "Copying the key tables"
if ls "$root/keys/"*.bin >/dev/null 2>&1; then
    mkdir -p "$out/keys"; cp "$root/keys/"*.bin "$out/keys/"
else
    warn "No key tables - run 'python3 tools/keytables.py'. Without them no zone decrypts."
fi

step "Copying the compression tables"
if [ -n "$RES" ]; then
    mkdir -p "$out/res"
    for table in compress.dat decompress.dat; do
        [ -f "$RES/$table" ] || { echo "$table was not found in $RES" >&2; exit 1; }
        cp "$RES/$table" "$out/res/"
    done
else
    warn "No --res: the client will not be able to connect to any server."
fi

if [ -n "$ZONEDATA" ]; then
    step "Copying zone data"
    [ -d "$ZONEDATA" ] || { echo "no zone data at $ZONEDATA" >&2; exit 1; }
    cp -R "$ZONEDATA" "$out/zones"
else
    warn "No zone data: walking to the edge of a zone will not change zones. Use !zone."
fi

# --- what to do with it ------------------------------------------------------

# The Windows package has carried a README from the start and this one did not,
# so --version was a flag that changed nothing. Same text, less the parts that
# only make sense with a data\ folder and an .exe.
step "Writing README"
cat > "$out/README.txt" <<EOF
MogHouse XI - Alpha $VERSION
============================

A from-scratch Final Fantasy XI client. This is an alpha: it is missing a
great deal, and the parts that are here are the parts that have been built so
far rather than the parts you would miss least.

What you need
-------------

  * A Final Fantasy XI installation, updated to the AUGUST 2026 patch. The
    client finds it and reads the game's own files - models, textures, zones,
    music. Nothing here replaces them and no game data is included. A Wine or
    Proton install is fine; it is the files that matter, not how they got here.
  * A private server to connect to, running that same version, and an account
    on it.
  * A GPU with a working Vulkan driver. Mesa covers AMD and Intel; NVIDIA needs
    its proprietary driver. If the renderer reports an adapter named llvmpipe,
    it has fallen back to software and will be very slow.

This is not backwards compatible. An older install, or an older server, will
not work, and it will not always fail in an obvious way: file ids move between
versions, so the wrong model loads, and packet layouts shift, so fields are
read from the wrong place. If something is odd in a way this README does not
explain, check the version first.

Running it
----------

  1. Unzip anywhere. There is no installer.
  2. Run ./"MogHouse XI" - or mark it executable first if your unzip tool
     dropped the permission bit: chmod +x "MogHouse XI"
  3. Confirm where the game is, enter your server's address, then log in.

Everything travels in this one directory: the client, the renderer
(libmoghouse_interop.so), SDL3, and the files they read. They have to stay
together, because the renderer looks for its assets beside the directory its
own library was loaded from. Delete the folder to remove the client completely.

Settings live in moghouse-settings.json beside the executable, and the servers
you add live beside that. If this directory is not writable - which is the case
inside a Flatpak - both move to \$XDG_DATA_HOME instead. They are plain text and
safe to edit while the client is closed. bodyDrawDistance is the one worth
knowing: 0 draws every character the client can, and a smaller number is how a
machine short of headroom keeps up.

Controls
--------

  WASD          walk                    Shift   run
  Mouse drag    look                    Space   jump
  R             auto-run                Tab     orbit
  M             hold the map north-up   U       back out if collision traps you
  + and -       music volume            P       print position to the log
  / and !       open chat, with the key already typed

Known missing, so you do not report what is already known
---------------------------------------------------------

  * Combat. You can walk, talk, zone and look at the world; you cannot fight.
  * Telepoint and Homepoint crystals are invisible.
  * Some creatures have no model and do not appear.
  * Hair colour is wrong for some faces.
  * There is no full-screen map yet.

If something is wrong
---------------------

There are two logs beside the executable: moghouse.log for the client and
moghouse.log.renderer for the world - or in \$XDG_DATA_HOME, if that is where
the settings went. Both are plain text, and between them they usually say what
happened. Attaching them to a bug report is the single most useful thing you
can do.

Report bugs from inside the game with the link in the top-left corner, or at
the GitHub issues page. There is a Discord link beside it.
EOF

# --- check --------------------------------------------------------------------

# Checked rather than assumed, for the same reason the macOS script checks: a
# tree with one library of the wrong architecture builds and packages cleanly
# and only fails on the machines it was built for.
step "Checking architecture"
bad=0
while IFS= read -r binary; do
    got=$(file -b "$binary" 2>/dev/null || echo "?")
    case "$got" in
        *x86-64*) ;;
        *) warn "$(basename "$binary"): $got"; bad=1 ;;
    esac
done < <(find "$out" -maxdepth 1 -type f \( -name '*.so*' -o -perm -u+x \) 2>/dev/null)
[ "$bad" -eq 1 ] && { echo "refusing to package a tree with mixed architectures" >&2; exit 1; }
echo "    everything is x86-64"

size=$(du -sh "$out" | cut -f1 | tr -d ' ')
echo
printf '\033[32m%s  (%s)\033[0m\n' "$out" "$size"
echo
echo "Next: flatpak-builder --user --install --force-clean \\"
echo "        build-flatpak flatpak/com.tagban.MogHouse.yml"
