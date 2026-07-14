#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/psxrecomp-posix-overlay.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

cache="$tmp/cache"
base="$tmp/tags"
expected="cg12_11111111"
other="cg12_22222222"
mkdir -p "$cache/00000000_11111111.dll" "$base/$expected" "$base/$other"

cc -std=c99 -Wall -Wextra -Werror -fPIC -shared \
  "$root/runtime/tests/overlay_posix_fixture.c" \
  -o "$cache/80010000_DEADBEEF.dll"
: > "$cache/00000000_00000000.dll"
: > "$cache/80010000_DEADBEG0.dll"
: > "$base/$other/80020000_12345678.dll"

cc -std=c99 -Wall -Wextra -Werror \
  -I"$root/runtime/include" \
  "$root/runtime/tests/test_overlay_posix.c" \
  "$root/runtime/src/overlay_posix.c" \
  -ldl -o "$tmp/test_overlay_posix"

"$tmp/test_overlay_posix" "$cache" "$base" "$expected" "$other"
