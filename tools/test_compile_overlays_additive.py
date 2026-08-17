import base64
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays
import coverage_vault


IDENTITY = compile_overlays.parse_game_identity("01" * 32, "02" * 32)
PAIR_ID = 0x1020304050607080


def region(load_addr, payload, executed=(), dispatch=(), functions=(), seeds=None):
    if seeds is None:
        seeds = dispatch
    return {
        "schema": "psxrecomp overlay capture v2",
        "load_addr": f"0x{load_addr:08X}",
        "size": len(payload),
        "bytes_b64": base64.b64encode(payload).decode("ascii"),
        "executed_pcs": [f"0x{x:08X}" for x in executed],
        "dispatch_entry_pcs": [f"0x{x:08X}" for x in dispatch],
        "function_entry_pcs": [f"0x{x:08X}" for x in functions],
        "seeds": [f"0x{x:08X}" for x in seeds],
    }


def write_pair(directory, name, func_ids, pair_id=PAIR_ID, provenance=None):
    dll = Path(directory) / f"{name}{compile_overlays.overlay_ext()}"
    dll.write_bytes(b"test-dll")
    dll.with_suffix(".ranges").write_text(
        compile_overlays.overlay_ranges_text(
            func_ids, pair_id=pair_id, provenance=provenance,
            identity=IDENTITY),
        encoding="ascii")
    return dll


def write_staged_pair(directory, func_ids):
    dll = Path(directory) / f"staged{compile_overlays.overlay_ext()}"
    ranges = Path(directory) / "staged.ranges"
    dll.write_bytes(b"complete-dll")
    ranges.write_text(
        compile_overlays.overlay_ranges_text(
            func_ids, pair_id=PAIR_ID, identity=IDENTITY),
        encoding="ascii")
    return dll, ranges


class AdditiveCaptureTests(unittest.TestCase):
    def test_region_coverage_unions_current_and_additive_pair_names(self):
        with tempfile.TemporaryDirectory() as td, mock.patch.object(
                compile_overlays, "_dll_runtime_exports_match",
                return_value=True):
            rows = [
                ("00010000_DEADBEEF", 0x80010000, 0x11111111),
                ("00010000_DEADBEEF_13579BDF", 0x80010004, 0x22222222),
                ("00010000_FEEDFACE_2468ACE0", 0x80010008, 0x33333333),
            ]
            for index, (name, entry, crc) in enumerate(rows):
                write_pair(td, name, [(entry, crc, [(entry, 4)])],
                           PAIR_ID + index)

            self.assertEqual(
                compile_overlays.load_region_coverage(td, 0x00010000),
                {(entry, crc) for _name, entry, crc in rows})

    def test_current_variant_coverage_unions_only_matching_entries(self):
        with tempfile.TemporaryDirectory() as td, mock.patch.object(
                compile_overlays, "_dll_runtime_exports_match",
                return_value=True):
            load = 0x80010000
            payload = b"\0" * 12
            zero_crc = compile_overlays.binascii.crc32(b"\0" * 4) & 0xFFFFFFFF
            one_crc = compile_overlays.binascii.crc32(b"\1" * 4) & 0xFFFFFFFF
            write_pair(td, "00010000_11111111",
                       [(load, zero_crc, [(load, 4)])], PAIR_ID)
            write_pair(td, "00010000_22222222_AAAAAAAA",
                       [(load + 4, zero_crc, [(load + 4, 4)])], PAIR_ID + 1)
            write_pair(td, "00010000_33333333_BBBBBBBB",
                       [(load + 8, one_crc, [(load + 8, 4)])], PAIR_ID + 2)

            ids = compile_overlays.load_region_current_variant_func_ids(
                td, 0x00010000, payload, load, len(payload))
            self.assertEqual({entry for entry, _crc, _ranges in ids},
                             {load, load + 4})

    def test_authority_filter_excludes_supplemental_shards(self):
        with tempfile.TemporaryDirectory() as td, mock.patch.object(
                compile_overlays, "_dll_runtime_exports_match",
                return_value=True):
            load = 0x80010000
            payload = b"\0" * 8
            crc = compile_overlays.binascii.crc32(b"\0" * 4) & 0xFFFFFFFF
            write_pair(td, "00010000_11111111",
                       [(load, crc, [(load, 4)])], PAIR_ID)
            write_pair(td, "00010000_22222222_AAAAAAAA",
                       [(load + 4, crc, [(load + 4, 4)])], PAIR_ID + 1,
                       compile_overlays.HOSTED_MANIFEST_PROVENANCE)

            all_ids = compile_overlays.load_region_current_variant_func_ids(
                td, 0x00010000, payload, load, len(payload))
            authority_ids = compile_overlays.load_region_current_variant_func_ids(
                td, 0x00010000, payload, load, len(payload),
                include_supplemental=False)
            self.assertEqual({entry for entry, _crc, _ranges in all_ids},
                             {load, load + 4})
            self.assertEqual({entry for entry, _crc, _ranges in authority_ids},
                             {load})

    def test_delay_slot_identity_audit_requires_cross_page_slot(self):
        load = 0x80010FF0
        payload = (b"\0" * 12 +
                   (0x10800008).to_bytes(4, "little") +
                   (0x24020001).to_bytes(4, "little"))
        safe = [(load, 0, [(load, len(payload))])]
        unsafe = [(load, 0, [(load, len(payload) - 4)])]
        self.assertEqual(
            compile_overlays.audit_func_id_delay_slots(safe, payload, load), [])
        errors = compile_overlays.audit_func_id_delay_slots(
            unsafe, payload[:-4], load)
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0][1], 0x80010FFC)
        self.assertIn("0x80011000", errors[0][2])

    def test_delay_slot_identity_audit_rejects_nested_control_flow(self):
        load = 0x80020000
        payload = ((0x10000001).to_bytes(4, "little") +
                   (0x03E00008).to_bytes(4, "little") +
                   (0x00000000).to_bytes(4, "little"))
        ids = [(load, 0, [(load, len(payload))])]
        errors = compile_overlays.audit_func_id_delay_slots(ids, payload, load)
        self.assertTrue(any("control transfer in a delay slot" in error[2]
                            for error in errors))

    def test_delay_slot_identity_audit_rejects_reserved_branch_likely(self):
        load = 0x80020000
        payload = ((0x50800001).to_bytes(4, "little") +
                   (0x00000000).to_bytes(4, "little"))
        ids = [(load, 0, [(load, len(payload))])]
        errors = compile_overlays.audit_func_id_delay_slots(ids, payload, load)
        self.assertTrue(any("reserved/unsupported" in error[2]
                            for error in errors))

    def test_runtime_manifest_parse_is_all_or_nothing(self):
        load = 0x80010000
        ids = [(load, 0x12345678, [(load, 8)])]
        valid = compile_overlays.overlay_ranges_text(
            ids, pair_id=PAIR_ID, identity=IDENTITY)
        pair_id, parsed = compile_overlays.parse_runtime_shard_manifest(valid)
        self.assertEqual(pair_id, PAIR_ID)
        self.assertEqual(parsed, ids)

        invalid_manifests = [
            valid.replace(f"R {load:08X} 8", "R 80210000 8"),
            valid.replace(f"F {load:08X} 12345678",
                          f"F {load:08X}"),
            valid + f"F {load + 4:08X} 00000000\nR 80210000 4\n",
        ]
        for manifest in invalid_manifests:
            self.assertEqual(
                compile_overlays.parse_runtime_shard_manifest(manifest),
                (None, []))

    def test_dll_publication_commits_manifest_before_canonical_dll(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / f"00010000_DEADBEEF{compile_overlays.overlay_ext()}"
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
            staged_dll, staged_ranges = write_staged_pair(td, func_ids)
            replacements = []

            real_replace = compile_overlays.os.replace

            def observe_replace(source_path, dest_path):
                replacements.append(Path(dest_path))
                real_replace(source_path, dest_path)

            with mock.patch.object(compile_overlays.os, "replace",
                                   side_effect=observe_replace):
                published = compile_overlays.publish_shard_pair(
                    str(staged_dll), str(staged_ranges), str(out_dll))

            ranges = out_dll.with_suffix(".ranges")
            self.assertTrue(published)
            self.assertEqual(out_dll.read_bytes(), b"complete-dll")
            self.assertTrue(ranges.exists())
            final_commits = [path for path in replacements
                             if path in (ranges, out_dll)]
            self.assertEqual(final_commits[-2:], [ranges, out_dll])
            self.assertFalse(list(Path(td).glob("*.tmp.*")))

    def test_publication_failure_before_commit_leaves_no_pair(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / f"00010000_DEADBEEF{compile_overlays.overlay_ext()}"
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
            staged_dll, staged_ranges = write_staged_pair(td, func_ids)

            with mock.patch.object(compile_overlays.os, "replace",
                                   side_effect=OSError("simulated lock")):
                with self.assertRaisesRegex(OSError, "simulated lock"):
                    compile_overlays.publish_shard_pair(
                        str(staged_dll), str(staged_ranges), str(out_dll))

            self.assertFalse(out_dll.exists())
            self.assertFalse(out_dll.with_suffix(".ranges").exists())

    def test_dll_commit_failure_rolls_back_published_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / f"00010000_DEADBEEF{compile_overlays.overlay_ext()}"
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
            staged_dll, staged_ranges = write_staged_pair(td, func_ids)
            real_replace = compile_overlays.os.replace

            def fail_dll_commit(source_path, dest_path):
                if os.path.abspath(dest_path) == os.path.abspath(out_dll):
                    raise OSError("simulated DLL lock")
                real_replace(source_path, dest_path)

            with mock.patch.object(compile_overlays.os, "replace",
                                   side_effect=fail_dll_commit):
                with self.assertRaisesRegex(OSError, "simulated DLL lock"):
                    compile_overlays.publish_shard_pair(
                        str(staged_dll), str(staged_ranges), str(out_dll))

            self.assertFalse(out_dll.exists())
            self.assertFalse(out_dll.with_suffix(".ranges").exists())
            self.assertFalse(Path(str(out_dll) + ".pair-txn.json").exists())

    def test_coverage_ignores_orphan_and_malformed_manifests(self):
        with tempfile.TemporaryDirectory() as td, mock.patch.object(
                compile_overlays, "_dll_runtime_exports_match",
                return_value=True):
            load = 0x80010000
            crc = 0x12345678
            name = "00010000_DEADBEEF"
            ranges = Path(td) / f"{name}.ranges"
            ranges.write_text(
                compile_overlays.overlay_ranges_text(
                    [(load, crc, [(load, 4)])], pair_id=PAIR_ID,
                    identity=IDENTITY), encoding="ascii")

            self.assertEqual(
                compile_overlays.load_region_coverage(td, 0x00010000), set())
            ranges.with_suffix(compile_overlays.overlay_ext()).write_bytes(b"dll")
            self.assertEqual(
                compile_overlays.load_region_coverage(td, 0x00010000),
                {(load, crc)})
            self.assertEqual(
                compile_overlays.load_shard_func_ids(str(ranges.with_suffix(
                    compile_overlays.overlay_ext()))),
                [(load, crc, [(load, 4)])])
            self.assertEqual(
                compile_overlays.load_region_entry_set(td, 0x00010000),
                {0x00010000})

            ranges.write_text(
                compile_overlays.overlay_ranges_text(
                    [(load, crc, [(load, 4)])], pair_id=PAIR_ID,
                    identity=IDENTITY) + f"F {load + 4:08X}\n",
                encoding="ascii")
            self.assertEqual(
                compile_overlays.load_region_coverage(td, 0x00010000), set())

    def test_vault_unions_evidence_but_preserves_reused_address_variants(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td) / "overlay_captures.json"
            history = Path(str(base) + ".d")
            history.mkdir()
            bytes_a = b"\x01\x02\x03\x04"
            bytes_b = b"\x05\x06\x07\x08"
            (history / "old.json").write_text(json.dumps([
                region(0x80010000, bytes_a, executed=(0x80010000,)),
                region(0x80010000, bytes_b, executed=(0x80010000,)),
            ]), encoding="utf-8")
            base.write_text(json.dumps([
                region(0x80010000, bytes_a, executed=(0x80010004,),
                       dispatch=(0x80010000,)),
            ]), encoding="utf-8")

            captures = coverage_vault._load_list(str(base))
            self.assertEqual(len(captures), 2)
            by_bytes = {base64.b64decode(capture["bytes_b64"]): capture
                        for capture in captures}
            self.assertEqual(set(by_bytes[bytes_a]["executed_pcs"]),
                             {"0x80010000", "0x80010004"})
            self.assertEqual(by_bytes[bytes_a]["dispatch_entry_pcs"],
                             ["0x80010000"])
            self.assertEqual(by_bytes[bytes_b]["executed_pcs"],
                             ["0x80010000"])

            (history / "truncated.json").write_text("[{", encoding="utf-8")
            self.assertEqual(len(coverage_vault._load_list(str(base))), 2)

    def test_vault_accepts_history_only_and_unions_all_evidence_fields(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "source.json"
            source_history = Path(str(source) + ".d")
            source_history.mkdir()
            payload = b"\x11\x22\x33\x44"
            (source_history / "only.json").write_text(json.dumps([
                region(0x80020000, payload, executed=(0x80020000,),
                       dispatch=(0x80020000,), functions=(0x80020004,),
                       seeds=(0x80020008,)),
            ]), encoding="utf-8")
            vault = Path(td) / "vault" / "overlay_captures.json"
            vault.parent.mkdir()
            vault.write_text(json.dumps([
                region(0x80020000, payload, executed=(0x8002000C,),
                       functions=(0x80020010,), seeds=(0x80020014,)),
            ]), encoding="utf-8")

            new_variants, new_pcs = coverage_vault.merge_captures(
                str(vault), str(source))
            self.assertEqual((new_variants, new_pcs), (0, 1))
            merged = json.loads(vault.read_text(encoding="utf-8"))[0]
            self.assertEqual(set(merged["executed_pcs"]),
                             {"0x80020000", "0x8002000C"})
            self.assertEqual(set(merged["function_entry_pcs"]),
                             {"0x80020004", "0x80020010"})
            self.assertEqual(set(merged["seeds"]),
                             {"0x80020008", "0x80020014"})

if __name__ == "__main__":
    unittest.main()
