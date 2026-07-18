#!/usr/bin/env python3
"""Focused regression checks for play-free overlay call-target discovery."""
import importlib.util
import pathlib
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "compile_overlays", ROOT / "tools" / "compile_overlays.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)

LOAD = 0x80010000


def put(data, offset, word):
    struct.pack_into("<I", data, offset, word)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def main():
    data = bytearray(0x200)

    # A framed root directly calls a frameless leaf.  JAL is the boundary
    # proof; the leaf deliberately has neither a prologue nor a preceding
    # return.
    put(data, 0x00, 0x27BDFFF0)
    put(data, 0x04, jal(LOAD + 0x40))
    put(data, 0x08, 0x00000000)
    put(data, 0x0C, 0x03E00008)
    put(data, 0x10, 0x27BD0010)
    put(data, 0x40, 0x24020001)
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)

    cap = {
        "schema": "psxrecomp overlay capture v2",
        "load_addr": f"0x{LOAD:08X}",
        "size": len(data),
        "executed_pcs": [],
        "dispatch_entry_pcs": [f"0x{LOAD:08X}"],
        "function_entry_pcs": [f"0x{LOAD:08X}"],
        "seeds": [f"0x{LOAD:08X}"],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit["included_reasons"][LOAD + 0x40] == "DIRECT_JAL_TARGET"
    assert f"call_root 0x{LOAD + 0x40:08X}" in seeds

    # A constant-register JR tail-calls backward to a frameless leaf.
    put(data, 0x100, 0x27BDFFF0)
    put(data, 0x104, 0x3C088001)  # lui $t0,0x8001
    put(data, 0x108, 0x25080040)  # addiu $t0,$t0,0x40
    put(data, 0x10C, 0x01000008)  # jr $t0
    put(data, 0x110, 0x27BD0010)
    walk = MOD._walk_overlay_function(
        bytes(data), LOAD, len(data), LOAD + 0x100, LOAD + len(data))
    assert walk["static_indirect_targets"] == {LOAD + 0x40}

    print("ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
