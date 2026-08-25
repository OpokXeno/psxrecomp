#!/usr/bin/env python3
import binascii
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("extract_overlays.py")


class ExtractOverlaysTests(unittest.TestCase):
    def test_duplicate_crc_manifest_records_are_counted_separately(self):
        payload = bytes(range(256)) * 8
        crc = f"{binascii.crc32(payload) & 0xFFFFFFFF:08X}"
        record = {
            "crc32": crc,
            "load_addr": "0x801C5000",
            "size": len(payload),
            "start_lba": "0x0",
        }

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            disc = root / "disc.iso"
            manifest = root / "overlay_map.jsonl"
            output = root / "overlays"
            disc.write_bytes(payload)
            manifest.write_text(
                json.dumps(record) + "\n" + json.dumps(record) + "\n",
                encoding="ascii",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--disc",
                    str(disc),
                    "--manifest",
                    str(manifest),
                    "--out",
                    str(output),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertIn("Results: 2 found  0 not found", result.stdout)
            self.assertEqual((output / f"{crc}.bin").read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
