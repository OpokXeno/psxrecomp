#!/usr/bin/env bash
# test_debug_overlay_hygiene.sh — N13 dual-build hygiene check.
#
# Verifies the PSX_DEBUG_OVERLAY gate contract:
#   1. Debug build (build-dbg) exports the overlay symbols and stages the data dir.
#   2. Release build (build/) contains ZERO overlay symbols, ZERO overlay strings,
#      and no staged debug_overlay dir (project Rule -1: no dead branches shipped).
#   3. Release binary still boots (headless smoke run, stays alive past timeout).
#
# Usage: test_debug_overlay_hygiene.sh [repo_root]
# Exit 0 = all checks pass.

set -u
ROOT="${1:-$(cd "$(dirname "$0")/../../.." && pwd)}"
DBG="$ROOT/build-dbg/XenogearsRecomp"
REL="$ROOT/build/XenogearsRecomp"
FAIL=0

say()  { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; FAIL=1; }

[ -x "$DBG" ] || { fail "debug binary missing: $DBG (build it first)"; }
[ -x "$REL" ] || { fail "release binary missing: $REL (build it first)"; }
[ "$FAIL" -eq 1 ] && exit 1

dbg_syms=$(nm -C --defined-only "$DBG" | grep -cE 'psx_debug_overlay|dbg_data|pugixml' || true)
rel_syms=$(nm -C --defined-only "$REL" | grep -cE 'psx_debug_overlay|dbg_data|pugixml' || true)
rel_strs=$(strings "$REL" | grep -cE 'Xenogears Debug|debug_overlay/data|Map Teleport' || true)

say "debug symbols (expect >0): $dbg_syms"
say "release symbols (expect 0): $rel_syms"
say "release overlay strings (expect 0): $rel_strs"

[ "$dbg_syms" -gt 0 ] || fail "debug build has no overlay symbols"
[ "$rel_syms" -eq 0 ] || fail "release build leaks $rel_syms overlay symbols"
[ "$rel_strs" -eq 0 ] || fail "release build leaks overlay strings"

if [ -d "$ROOT/build/debug_overlay" ]; then
    fail "release build staged debug_overlay/ data dir"
else
    say "release staging: none (ok)"
fi

if [ -f "$ROOT/build-dbg/debug_overlay/data/fields.xml" ]; then
    say "debug staging: fields.xml present (ok)"
else
    fail "debug build missing staged fields.xml"
fi

# Headless boot smoke: Release must stay alive until the timeout kills it.
say "release headless boot smoke (8s)..."
boot_log=$(mktemp)
timeout 8 "$REL" --no-launcher --headless >"$boot_log" 2>&1
ec=$?
if [ "$ec" -eq 124 ]; then
    say "release boots and stays alive (ok)"
else
    fail "release exited early (code $ec) — see $boot_log"
fi
rm -f "$boot_log"

if [ "$FAIL" -eq 0 ]; then
    say "HYGIENE PASS"
else
    say "HYGIENE FAIL"
fi
exit "$FAIL"
