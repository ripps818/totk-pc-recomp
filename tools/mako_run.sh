#!/usr/bin/env bash
# Local helper (branch mako-local, not for upstream): run the game with the MAKO frame-generation Vulkan layer.
#
#   tools/mako_run.sh <game folder> [totk arguments...]      env: MAKO_MULT=2 (output frames per game frame)
#
# MAKO (https://github.com/eugeniosegala/MAKO) is GPL-3.0-or-later software, so nothing of it is copied into this
# repository. The script downloads upstream's release tarball into build/mako (ignored), checks its SHA-256, and starts
# ./totk with the layer enabled through the Vulkan loader's own variables. It does not need mako-launch.
set -euo pipefail
URL=https://github.com/eugeniosegala/MAKO/releases/download/render-v5.0.0/MAKO-Renderer-v5.0.0-linux.tar.xz
SHA=c59eda9681aab614474fc74a99194fe6445190fcea500eed89367e09b299fb9f
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$here/build/mako"; prefix="$work/prefix"
game="${1:?usage: tools/mako_run.sh <game folder> [totk arguments...]}"; shift
[ -x "$game/totk" ] || { echo "no totk binary in $game" >&2; exit 2; }

if [ ! -f "$prefix/lib/libmako-render.so" ]; then
    mkdir -p "$work" "$prefix"
    tar_file="$work/mako.tar.xz"
    [ -f "$tar_file" ] || curl -fL --retry 3 -o "$tar_file" "$URL"
    [ "$(sha256sum "$tar_file" | cut -d' ' -f1)" = "$SHA" ] || { echo "SHA-256 mismatch for $tar_file" >&2; rm -f "$tar_file"; exit 1; }
    tar -xJf "$tar_file" -C "$prefix"
fi

mkdir -p "$work/cfg"
cat > "$work/cfg/conf.toml" <<T
version = 2

[global]
allow_fp16 = true

[[profile]]
name = "totk"
active_in = ["totk"]
frame_generation_provisioned = true
frame_generation_enabled = true
multiplier = ${MAKO_MULT:-2}
scaling_enabled = false
T

cd "$game"
# The game's own frame generation must stay off: two generators in one swapchain do not work.
export ENABLE_MAKO=1 MAKO_CONFIG="$work/cfg/conf.toml" MAKO_PROFILE=totk TOTK_FRAME_GEN=0
export VK_ADD_IMPLICIT_LAYER_PATH="$prefix/share/vulkan/implicit_layer.d${VK_ADD_IMPLICIT_LAYER_PATH:+:$VK_ADD_IMPLICIT_LAYER_PATH}"
exec ./totk "$@"
