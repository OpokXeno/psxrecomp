#!/usr/bin/env bash
# Compile + run the XML loader harness against the staged debug_overlay
# data dir. Mirrors the style of run_overlay_posix_test.sh: build under
# $TMPDIR, run, fail fast on non-zero exit.
set -euo pipefail

# The data dir lives at the game repo root (../../ from the runtime/tests
# script — this script itself is inside psxrecomp/runtime/tests/), and is
# also staged at <build-dbg>/debug_overlay/data by the POST_BUILD step.
# We default to the game repo root (same as runtime.cmake's POST_BUILD
# source) so the harness exercises the EXACT same files the runtime
# reads at startup.
runtime_root="$(cd "$(dirname "$0")/../.." && pwd)"
game_root="$(cd "$runtime_root/.." && pwd)"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/psxrecomp-debug-overlay-data.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

# Default data dir: game repo root, where debug_overlay/data lives.
# Override with $1 to point the harness at a different layout (e.g. the
# build-dbg/debug_overlay/data directory).
data_dir="${1:-$game_root}"

cxx="${CXX:-g++}"
"$cxx" -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter \
    -DPSX_DEBUG_OVERLAY=1 \
    -I"$runtime_root/runtime/src" \
    -I"$runtime_root/runtime/src/third_party/pugixml" \
    "$runtime_root/runtime/tests/test_debug_overlay_data.cpp" \
    "$runtime_root/runtime/src/debug_overlay_data.cpp" \
    "$runtime_root/runtime/src/third_party/pugixml/pugixml.cpp" \
    -o "$tmp/test_debug_overlay_data"

"$tmp/test_debug_overlay_data" "$data_dir"
