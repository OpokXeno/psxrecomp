import base64
import json
import tempfile
import unittest
from unittest import mock
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays
import coverage_vault


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


class AdditiveCaptureTests(unittest.TestCase):
    def test_live_dll_publication_uses_private_output_and_commits_dll_last(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / "00010000_DEADBEEF.dll"
            source = Path(td) / "source.c"
            source.write_text("/* test */", encoding="utf-8")
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
            observed = []

            def fake_compile(_source, private_dll, _includes, **_kwargs):
                private = Path(private_dll)
                self.assertNotEqual(private, out_dll)
                self.assertFalse(out_dll.exists())
                private.write_bytes(b"complete-dll")
                observed.append(private)
                return True

            real_replace = compile_overlays.os.replace

            def observe_replace(source_path, dest_path):
                observed.append(Path(dest_path))
                real_replace(source_path, dest_path)

            with mock.patch.object(compile_overlays, "compile_dll",
                                   side_effect=fake_compile), \
                 mock.patch.object(compile_overlays.os, "replace",
                                   side_effect=observe_replace):
                ok, count = compile_overlays.compile_and_publish_dll(
                    str(source), str(out_dll), [], func_ids)

            self.assertTrue(ok)
            self.assertEqual(count, 1)
            self.assertEqual(out_dll.read_bytes(), b"complete-dll")
            self.assertTrue(out_dll.with_suffix(".ranges").exists())
            self.assertEqual(observed[-1], out_dll)
            self.assertFalse(list(Path(td).glob("*.tmp.*")))

    def test_unions_evidence_but_preserves_reused_address_variants(self):
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

            captures, sources = compile_overlays.load_additive_captures(str(base))

            self.assertEqual(len(sources), 2)
            self.assertEqual(len(captures), 2)
            by_bytes = {base64.b64decode(c["bytes_b64"]): c for c in captures}
            self.assertEqual(by_bytes[bytes_a]["executed_pcs"],
                             [0x80010000, 0x80010004])
            self.assertEqual(by_bytes[bytes_a]["dispatch_entry_pcs"],
                             [0x80010000])
            self.assertEqual(by_bytes[bytes_b]["executed_pcs"],
                             [0x80010000])

            (history / "truncated.json").write_text("[{", encoding="utf-8")
            recovered, _ = compile_overlays.load_additive_captures(str(base))
            self.assertEqual(len(recovered), 2)
            vault_view = coverage_vault._load_list(str(base))
            self.assertEqual(len(vault_view), 2)

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

    def test_malformed_record_does_not_discard_valid_sibling(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td) / "overlay_captures.json"
            payload = b"\x99\x88\x77\x66"
            invalid_b64 = region(0x80031000, payload)
            invalid_b64["bytes_b64"] = "!!!"
            scalar_evidence = region(0x80032000, payload)
            scalar_evidence["executed_pcs"] = 42
            base.write_text(json.dumps([
                {"load_addr": "not-an-address"},
                invalid_b64,
                scalar_evidence,
                region(0x80030000, payload, executed=(0x80030000,)),
            ]), encoding="utf-8")
            captures, _ = compile_overlays.load_additive_captures(str(base))
            self.assertEqual(len(captures), 1)
            self.assertEqual(captures[0]["load_addr"], "0x80030000")



if __name__ == "__main__":
    unittest.main()
