#!/usr/bin/env python3
"""Guard PSX display enhancements and per-title widescreen offers."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)
MOD_RUNTIME = (ROOT / "runtime" / "src" / "mod_runtime.cpp").read_text(
    encoding="utf-8"
)

for declaration in (
    "bool ws_offered = false;",
    "bool ws_ultrawide_offered = false;",
    "constexpr bool frame_interpolation_offered = false;",
    "constexpr bool skip_fmv_offered = false;",
):
    assert declaration in MAIN, f"PSX launcher capability must default off: {declaration}"

for legacy_route in (
    "frame_interpolation_offered =\n                gc.runtime.video_offer_frame_interpolation;",
    "skip_fmv_offered = gc.runtime.video_offer_skip_fmv;",
):
    assert legacy_route not in MAIN, f"mod-owned offer flag still controls UI: {legacy_route}"

for widescreen_route in (
    "ws_offered = gc.ws_offered;",
    "ws_ultrawide_offered = gc.ws_ultrawide_offered;",
):
    assert widescreen_route in MAIN

assert "gi->widescreen_supported = ws_offered_b ? 1 : 0;" in MAIN
assert "gi->aspect_experimental = ws_offered_b ? 1 : 0;" in MAIN
assert "gi->aspect_mask = (ws_offered_b || ws_ultrawide_offered_b)" in MAIN
assert 'return package.id == "psx.enhancement.pgxp";' in MOD_RUNTIME
assert "launcher_hides_package(*candidate)" in MOD_RUNTIME
assert "launcher_hides_package(*selected)" in MOD_RUNTIME

for trusted_api in (
    "psx_mod_set_fixed_display_aspect",
    "psx_mod_set_adaptive_display_aspect",
    "psx_mod_set_frame_interpolation",
    "psx_mod_set_auto_skip_fmv",
):
    assert trusted_api in HEADER, f"missing trusted mod API: {trusted_api}"

assert MAIN.index("g_auto_skip_fmv = 0;") < MAIN.index("mod_runtime_activate_plugins();")

print("mod-owned PSX display controls guard passed")
