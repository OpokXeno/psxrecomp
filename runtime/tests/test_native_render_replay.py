#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import sys
import unittest


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "native_render_replay", ROOT / "tools" / "native_render_replay.py"
)
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MOD
SPEC.loader.exec_module(MOD)


class NativeRenderReplayRecordTests(unittest.TestCase):
    def test_run_command_uses_explicit_memcard_dir(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime_state = root / "state"
            memcard_dir = root / "cards"
            memcard_dir.mkdir()
            (memcard_dir / "card1.mcd").write_bytes(b"card1")
            trace = root / "trace.toml"
            build = ROOT / "build-dbg" / "XenogearsRecomp"
            disc = ROOT / "game" / "disc1.cue"
            request = MOD.RunRequest(
                build=build,
                trace=trace,
                runtime_state=runtime_state,
                memcard_dir=MOD.validate_memcard_dir(memcard_dir),
                evidence=root / "evidence.json",
                renderer="opengl",
                disc=disc,
            )

            command = MOD.runtime_command(request)

            self.assertIn("--memcard-dir", command)
            self.assertIn(str(memcard_dir.resolve()), command)
            self.assertNotIn(str(runtime_state / "memcards"), command)

    def test_record_command_uses_explicit_memcard_dir(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime_state = Path(temporary) / "state"
            trace = Path(temporary) / "trace.toml"
            build = ROOT / "build-dbg" / "XenogearsRecomp"
            disc = ROOT / "game" / "disc1.cue"
            request = MOD.RecordRequest(
                build=build,
                trace=trace,
                runtime_state=runtime_state,
                memcard_dir=ROOT,
                renderer="opengl",
                disc=disc,
                max_vblanks=216000,
            )

            command = MOD.runtime_record_command(request)

            self.assertIn("--memcard-dir", command)
            self.assertIn(str(ROOT.resolve()), command)
            self.assertIn(str(runtime_state.resolve()), command)
            self.assertNotIn(str(runtime_state / "memcards"), command)

    def test_validate_memcard_dir_requires_existing_card_image(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            memcard_dir = Path(temporary) / "memcards"
            memcard_dir.mkdir()

            with self.assertRaisesRegex(ValueError, "memcard-dir must contain card1.mcd and/or card2.mcd"):
                MOD.validate_memcard_dir(memcard_dir)

    def test_validate_memcard_dir_returns_resolved_path_for_regular_cards(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            memcard_dir = Path(temporary) / "memcards"
            memcard_dir.mkdir()
            (memcard_dir / "card1.mcd").write_bytes(b"card1")

            validated = MOD.validate_memcard_dir(memcard_dir)

            self.assertEqual(validated, memcard_dir.resolve())
            self.assertTrue((validated / "card1.mcd").is_file())


if __name__ == "__main__":
    unittest.main()
