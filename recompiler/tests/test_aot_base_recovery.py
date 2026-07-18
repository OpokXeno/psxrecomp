#!/usr/bin/env python3
"""Focused regression checks for generic AOT overlay base selection."""
import collections
import importlib.util
import pathlib
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "extract_generic", ROOT / "tools" / "aot_overlay_spike" /
    "extract_generic.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)


def main():
    true_base = 0x80106228
    nearby_false = 0x80106220

    # A near-tie without independent evidence fails closed. If a call-dense
    # sibling has already proved the exact true base, that one unique match is
    # enough to resolve the call-sparse file.
    votes = collections.Counter({nearby_false: 13, true_base: 12})
    assert MOD._select_base_vote(votes) is None
    assert MOD._select_base_vote(votes, {true_base}) == (true_base, 12)
    assert MOD._select_base_vote(votes, {true_base, nearby_false}) is None

    # Only JAL is callable-boundary evidence. A nearby unconditional J branch
    # must not participate in base recovery.
    data = bytearray(12)
    struct.pack_into("<I", data, 0, 0x08000000 | ((0x80101234 >> 2) & 0x03FFFFFF))
    struct.pack_into("<I", data, 4, 0x0C000000 | ((0x80105678 >> 2) & 0x03FFFFFF))
    assert MOD.jal_targets(data) == {0x80105678}

    print("ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
