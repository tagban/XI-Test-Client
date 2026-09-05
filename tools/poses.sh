#!/usr/bin/env bash
#
# Render every animation clip a body has, one thumbnail each, into a folder.
#
# The clips are named rather than described - idl0, wlk0, si10 - and there is
# no index of them anywhere in the files, so the only way to learn what one is
# is to play it and look. This does that in bulk, so the naming can be mapped
# to emotes by eye rather than guessed at.
#
#     tools/poses.sh [outdir] [frame]
#
# The frame matters: a clip pinned at one frame shows one moment of it, and an
# emote's characteristic pose is usually near its end rather than its middle.
# Run it twice at different frames when a clip is ambiguous.
#
# Needs MOGHOUSE_FFXI_INSTALL, and the key tables beside the build.
set -euo pipefail

out="${1:-/tmp/poses}"
frame="${2:-7}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install="${MOGHOUSE_FFXI_INSTALL:?set MOGHOUSE_FFXI_INSTALL to the retail install}"
zone="${MOGHOUSE_POSE_ZONE:-$install/ROM/1/31.DAT}"

cd "$root/build-renderer"
export MOGHOUSE_FFXI_KEYTABLE="$root/keys/mzb_key_table.bin"
export MOGHOUSE_FFXI_KEYTABLE2="$root/keys/mmb_key_table2.bin"
export MOGHOUSE_FFXI_INSTALL="$install"

mkdir -p "$out"

# Ask the body what it can do. The look is a stand-in: race and starting
# clothes, since the clips come from the race's own motion files either way.
look="${MOGHOUSE_POSE_LOOK:-1,0,0,8,8,8,8}"
clips=$(MOGHOUSE_CLIPS=1 MOGHOUSE_LOOK="$look" MOGHOUSE_SCREENSHOT=/dev/null \
        ./moghouse-renderer "$zone" 2>&1 |
        sed -n '/^clips/,/^[a-z]* [0-9]/p' | tail -n +2 | tr ' ' '\n' |
        grep -E '^[a-z0-9_]{4}$' | sort -u)

printf '%s\n' "$clips" > "$out/clips.txt"
echo "$(printf '%s\n' "$clips" | wc -l | tr -d ' ') clips -> $out"

for clip in $clips; do
    MOGHOUSE_TIME=720 MOGHOUSE_ANIMATION="$clip" MOGHOUSE_LOOK="$look" \
        MOGHOUSE_CHARACTER_AT="120,0,-88" MOGHOUSE_CAMERA="120,1.2,-84" \
        MOGHOUSE_CAMERA_LOOK="180,-5" MOGHOUSE_FRAME="$frame" \
        MOGHOUSE_SCREENSHOT="$out/$clip.bmp" ./moghouse-renderer "$zone" >/dev/null 2>&1 || true
    if [ -f "$out/$clip.bmp" ]; then
        sips -s format png "$out/$clip.bmp" --out "$out/$clip.png" >/dev/null 2>&1
        sips -c 900 700 --cropOffset 380 930 "$out/$clip.png" --out "$out/$clip.png" >/dev/null 2>&1
        sips -Z 150 "$out/$clip.png" --out "$out/$clip.png" >/dev/null 2>&1
        rm -f "$out/$clip.bmp"
    fi
done
echo done
