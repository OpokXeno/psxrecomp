#!/usr/bin/env python3
"""Focused regression checks for generic AOT overlay base selection."""
import collections
import importlib.util
import hashlib
import json
import pathlib
import struct
import sys
import tempfile


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

    # Dense function-pointer tables may recover framed or frameless functions,
    # but only when every target has independent callable-boundary evidence.
    base = 0x80100000
    data = bytearray(0x100)
    # Three adjacent leaves, each immediately after a previous return.
    for off in (0x18, 0x28, 0x38):
        struct.pack_into('<I', data, off-8, 0x03E00008)
        struct.pack_into('<I', data, off-4, 0x00000000)
        struct.pack_into('<I', data, off, 0x24020001)
    for i, off in enumerate((0x18, 0x28, 0x38)):
        struct.pack_into('<I', data, 0x80+i*4, base+off)
    assert MOD.pointer_table_targets(data, base) == {
        base+0x18, base+0x28, base+0x38}
    # An isolated pointer-shaped word is intentionally insufficient.
    struct.pack_into('<I', data, 0x80, 0)
    struct.pack_into('<I', data, 0x84, 0)
    assert MOD.pointer_table_targets(data, base) == set()

    # Exact-hash BIOS resident recipes synthesize only their declared code and
    # data words. Entries must point into code fragments; a different BIOS is a
    # safe empty result rather than a speculative shard.
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        bios = td / 'bios.bin'
        bios.write_bytes(b'test bios')
        manifest = td / 'resident.json'
        manifest.write_text(json.dumps({
            'schema': 'psxrecomp bios resident code v1',
            'images': [{
                'name': 'test helper',
                'bios_sha256': hashlib.sha256(b'test bios').hexdigest(),
                'region_start': '0x8000DF80',
                'region_size': '0x80',
                'code_fragments': [{
                    'address': '0x8000DF80',
                    'words': ['0x03E00008', '0x00000000'],
                }],
                'data_words': [{
                    'address': '0x8000DFFC', 'word': '0x12345678',
                }],
                'dispatch_entry_pcs': ['0x8000DF80'],
            }],
        }), encoding='utf-8')
        resident = MOD.bios_resident_records(str(bios), str(manifest))
        assert len(resident) == 1
        capture = resident[0]
        decoded = __import__('base64').b64decode(capture['bytes_b64'])
        assert struct.unpack_from('<I', decoded, 0)[0] == 0x03E00008
        assert struct.unpack_from('<I', decoded, 0x7C)[0] == 0x12345678
        assert capture['producer_ranges'] == [{
            'start': '0x8000DF80', 'end': '0x8000DF88'}]
        bios.write_bytes(b'different bios')
        assert MOD.bios_resident_records(str(bios), str(manifest)) == []

    print("ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
