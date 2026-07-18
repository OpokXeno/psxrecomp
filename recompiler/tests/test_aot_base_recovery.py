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

    # A JAL target becomes a conservative shard root only when the target is
    # local and has independent callable-boundary evidence.
    rooted = bytearray(0x80)
    struct.pack_into('<I', rooted, 0, 0x0C000000 |
                     (((0x80100000 + 0x40) >> 2) & 0x03FFFFFF))
    struct.pack_into('<I', rooted, 0x40, 0x27BDFFF0)
    assert MOD.direct_jal_roots(rooted, 0x80100000) == [0x80100040]
    struct.pack_into('<I', rooted, 0x40, 0x24020001)
    assert MOD.direct_jal_roots(rooted, 0x80100000) == []

    # A return boundary plus a JAL-shaped source is still not enough when the
    # alleged target begins with a dense run of local pointers. Those words are
    # data even though the first one has a syntactically valid MIPS decode.
    pointer_root = bytearray(0x100)
    struct.pack_into('<I', pointer_root, 0, 0x0C000000 |
                     (((0x80100000 + 0x40) >> 2) & 0x03FFFFFF))
    struct.pack_into('<I', pointer_root, 0x38, 0x03E00008)
    struct.pack_into('<I', pointer_root, 0x3C, 0x00000000)
    for index, target in enumerate((0x60, 0x70, 0x80)):
        struct.pack_into('<I', pointer_root, 0x40 + index * 4,
                         0x80100000 + target)
    assert MOD.direct_jal_roots(pointer_root, 0x80100000) == []

    bounded = bytearray(0x90)
    for off in (0x10, 0x40, 0x70):
        struct.pack_into('<I', bounded, off, 0x27BDFFF0)
    recipe = MOD.bounded_dispatch_fallback(
        bounded, 0x80100000, [0x80100024, 0x80100050])
    assert recipe['function_entry_pcs'] == ['0x80100010', '0x80100040']
    assert recipe['producer_ranges'] == [
        {'start': '0x80100010', 'end': '0x80100040'},
        {'start': '0x80100040', 'end': '0x80100070'},
    ]

    wrapped = MOD.make_psx_exe(b'\x11\x22\x33\x44', 0x80102000,
                               0x80102040)
    assert wrapped[:8] == b'PS-X EXE'
    assert struct.unpack_from('<III', wrapped, 0x10) == (
        0x80102040, 0, 0x80102000)
    assert struct.unpack_from('<I', wrapped, 0x1C)[0] == 4
    assert wrapped[0x800:] == b'\x11\x22\x33\x44'

    # Psy-Q may schedule a global load before allocating the stack frame.  The
    # exact function starts immediately after the previous return, not at the
    # otherwise recognizable addiu-sp instruction eight bytes later.
    base = 0x80100000
    data = bytearray(0x40)
    struct.pack_into('<I', data, 0x00, 0x03E00008)  # jr ra
    struct.pack_into('<I', data, 0x04, 0x00000000)  # delay slot
    struct.pack_into('<I', data, 0x08, 0x3C03800A)  # lui v1,0x800a
    struct.pack_into('<I', data, 0x0C, 0x9463BCC8)  # lhu v1,-0x4338(v1)
    struct.pack_into('<I', data, 0x10, 0x27BDFFE8)  # addiu sp,sp,-0x18
    assert MOD.prologues(data, base) == [base + 0x08]
    # A load through a different register is not sufficient evidence to move
    # the entry; fail closed at the ordinary stack-frame prologue.
    struct.pack_into('<I', data, 0x0C, 0x9443BCC8)  # lhu v1,-0x4338(v0)
    assert MOD.prologues(data, base) == [base + 0x10]

    # Normal-mode discovery must not turn a PS-X body's provenance-free leading
    # pointer table into a function. Keep the declared entry and the additive
    # decoder-derived interior set; those entries have already passed normal
    # mode's return/prologue/call/control-flow discovery and classification.
    data = bytearray(0x100)
    for n in range(4):
        struct.pack_into('<I', data, n * 4, base + 0x40 + n * 4)
    struct.pack_into('<I', data, 0x40, 0x27BDFFF0)
    struct.pack_into('<I', data, 0x44, 0x0C000000 |
                     (((base + 0x80) >> 2) & 0x03FFFFFF))
    struct.pack_into('<I', data, 0x80, 0x24020001)
    assert MOD.filter_full_discovery_seeds(
        data, base, [base, base + 0x40, base + 0x80], base + 0x40) == [
            base + 0x40, base + 0x80]
    assert MOD.filter_full_discovery_seeds(
        data, base, [base, base + 0x40, base + 0x80], base) == [
            base, base + 0x40, base + 0x80]

    entries, aliases = MOD.parse_full_discovery_ranges([
        'F 80100040\n', 'R 80100040 20\n',
        'F 80100048\n', 'R 80100040 20\n'])
    assert entries == [base + 0x40, base + 0x48]
    assert aliases == [(base + 0x48, base + 0x40, base + 0x60)]

    # Dense function-pointer tables may recover framed or frameless functions,
    # but only when every target has independent callable-boundary evidence.
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

    # An unreferenced frameless leaf can still be proven play-free by exact
    # return-to-return boundaries plus a bounded valid CFG.
    leaf = bytearray(0x80)
    struct.pack_into('<I', leaf, 0x00, 0x03E00008)
    struct.pack_into('<I', leaf, 0x04, 0x24020000)  # arbitrary delay slot
    struct.pack_into('<I', leaf, 0x08, 0x24020001)
    struct.pack_into('<I', leaf, 0x0C, 0x03E00008)
    struct.pack_into('<I', leaf, 0x10, 0x00000000)
    assert base + 0x08 in MOD.frameless_leaf_entries(leaf, base)
    # The same positional evidence must not promote a dense pointer table.
    for index, off in enumerate((0x40, 0x50, 0x60)):
        struct.pack_into('<I', leaf, 0x08 + index * 4, base + off)
    assert base + 0x08 not in MOD.frameless_leaf_entries(leaf, base)

    # A strict aligned {id,size} archive can share an independently voted link
    # base across members.  Direct JAL targets are the roots; unrelated padding
    # and prologue-shaped words do not become code merely by being in a member.
    archive_base = 0x800E9060
    members = []
    target_offsets = (0x80, 0x94, 0xB0, 0xD8,
                      0x110, 0x154, 0x1A8, 0x210)
    for ident in range(1, 5):
        body = bytearray(0x240)
        for n, target_off in enumerate(target_offsets):
            target = archive_base + target_off
            struct.pack_into('<I', body, n * 4, 0x0C000000 |
                             ((target >> 2) & 0x03FFFFFF))
            struct.pack_into('<I', body, target_off, 0x27BDFFF0)
        members.append((ident, bytes(body)))
    archive = bytearray(0x800)
    pos = 0x800
    payload = bytearray()
    for n, (ident, body) in enumerate(members):
        struct.pack_into('<II', archive, n * 8, ident, len(body))
        payload.extend(body)
        payload.extend(b'\0' * ((-len(payload)) & 0x7FF))
    archive.extend(payload)
    split = MOD.split_indexed_archive(bytes(archive))
    assert split is not None and [x[0] for x in split] == [1, 2, 3, 4]
    recovered = MOD.recover_indexed_archive(
        bytes(archive), 0x80010000, 0x80200000)
    assert len(recovered) == 4
    assert {x['base'] for x in recovered} == {archive_base}
    assert all(len(x['direct_seeds']) == 8 for x in recovered)

    # Companion HED tables may address a logical sector namespace spanning DAT
    # then BNS. Runs are discovered independently across zero-filled table gaps.
    hed = bytearray(0x800)
    for n in range(4):
        struct.pack_into('<I', hed, n * 4, (1 << 20) | n)
        struct.pack_into('<I', hed, 0x100 + n * 4, (1 << 20) | (4 + n))
    payload_a = b''.join(bytes([n + 1]) * 0x800 for n in range(4))
    payload_b = b''.join(bytes([n + 5]) * 0x800 for n in range(4))

    class FakeDisc:
        blobs = {1: bytes(hed), 2: payload_a, 3: payload_b}

        def read_file_bytes(self, lba, size):
            return self.blobs[lba][:size]

    groups = MOD.hed_companion_members(FakeDisc(), [
        ('ARC.HED', 1, len(hed)), ('ARC.DAT', 2, len(payload_a)),
        ('ARC.BNS', 3, len(payload_b)),
    ])
    assert len(groups) == 1 and len(groups[0]['members']) == 8
    assert [x[1] for x in groups[0]['members']] == [n * 0x800
                                                    for n in range(8)]

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
