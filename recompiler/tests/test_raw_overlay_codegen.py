#!/usr/bin/env python3
"""End-to-end contract for authenticated headerless overlay input."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path


GAME_SHA256 = "12" * 32
MANIFEST_SHA256 = "34" * 32
LOAD_ADDRESS = 0x80010000


def invoke(recompiler: Path, raw: Path, seeds: Path, runtime_include: Path,
           output: Path, digest: str, entry: int = LOAD_ADDRESS,
           discovery: str | None = None, load_address: int = LOAD_ADDRESS):
    command = [
            str(recompiler),
            "--raw-image", str(raw),
            "--load-address", hex(load_address),
            "--entry-pc", hex(entry),
            "--input-sha256", digest,
            "--out-stem", "fixture-overlay",
            "--seeds", str(seeds),
            "--out-dir", str(output),
            "--overlay",
            "--game-identity-sha256", GAME_SHA256,
            "--manifest-digest-sha256", MANIFEST_SHA256,
            "--project-root", str(Path(__file__).resolve().parents[2]),
            "--runtime-include", str(runtime_include),
        ]
    if discovery is not None:
        command.extend(("--discovery", discovery))
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True, type=Path)
    args = parser.parse_args()
    recompiler = args.recompiler.resolve()

    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary = Path(temporary_directory)
        raw = temporary / "fixture.bin"
        image = bytearray(0x80)
        struct.pack_into(
            "<IIII", image, 0,
            0x10000001, 0x00000000, 0x03E00008, 0x00000000,
        )
        struct.pack_into(
            "<III", image, 0x40,
            0x27BDFFF0, 0x03E00008, 0x27BD0010,
        )
        struct.pack_into(
            "<IIII", image, 0x50,
            0x27BDFFF0, 0x040E0001, 0x03E00008, 0x00000000,
        )
        struct.pack_into(
            "<II", image, 0x70,
            LOAD_ADDRESS + 4, LOAD_ADDRESS + 4,
        )
        raw.write_bytes(image)
        digest = hashlib.sha256(raw.read_bytes()).hexdigest()
        seeds = temporary / "fixture.seeds"
        seeds.write_text(
            f"call_root 0x{LOAD_ADDRESS:08X}\n"
            f"isolated_root 0x{LOAD_ADDRESS + 8:08X}\n",
            encoding="ascii",
        )

        hash_result = subprocess.run(
            [str(recompiler), "--codegen-hash"],
            capture_output=True,
            text=True,
            check=True,
        )
        runtime_include = temporary / "include"
        runtime_include.mkdir()
        (runtime_include / "overlay_codegen_hash.h").write_text(
            "#pragma once\n"
            f"#define PSX_OVERLAY_CODEGEN_HASH 0x{hash_result.stdout.strip()}u\n",
            encoding="ascii",
        )

        output = temporary / "success"
        result = invoke(recompiler, raw, seeds, runtime_include, output, digest)
        if result.returncode != 0:
            raise AssertionError(result.stderr or result.stdout)
        expected = [
            output / "fixture-overlay_full.c",
            output / "fixture-overlay_full.ranges",
            output / "fixture-overlay_input.json",
        ]
        if not all(path.is_file() for path in expected):
            raise AssertionError("raw overlay generation omitted required outputs")
        provenance = json.loads(expected[2].read_text(encoding="utf-8"))
        if provenance != {
            "schema": "psxrecomp-input-provenance-v1",
            "format": "raw",
            "sha256": digest,
            "load_address": f"0x{LOAD_ADDRESS:08X}",
            "size": len(image),
            "entry_pc": f"0x{LOAD_ADDRESS:08X}",
            "discovery": "reachable",
            "game_identity_sha256": GAME_SHA256,
            "manifest_identity_sha256": MANIFEST_SHA256,
        }:
            raise AssertionError(f"unexpected provenance: {provenance}")
        ranges_text = expected[1].read_text(encoding="ascii")
        if f"F {LOAD_ADDRESS + 8:08X}" not in ranges_text:
            raise AssertionError("isolated root was not emitted in the combined native unit")
        if f"F {LOAD_ADDRESS + 0x40:08X}" in ranges_text:
            raise AssertionError("default overlay discovery swept an unseeded function")

        whole_output = temporary / "whole-image"
        whole = invoke(
            recompiler, raw, seeds, runtime_include, whole_output, digest,
            discovery="whole-image",
        )
        if whole.returncode != 0:
            raise AssertionError(whole.stderr or whole.stdout)
        whole_ranges = (whole_output / "fixture-overlay_full.ranges").read_text(
            encoding="ascii"
        )
        if f"F {LOAD_ADDRESS + 0x40:08X}" not in whole_ranges:
            raise AssertionError("whole-image overlay discovery missed an unseeded function")
        if f"F {LOAD_ADDRESS + 8:08X}" not in whole_ranges:
            raise AssertionError("whole-image discovery discarded an isolated root")
        if f"F {LOAD_ADDRESS + 4:08X}" in whole_ranges:
            raise AssertionError("overlay whole-image promoted a data-table alias")
        invalid_subfield = LOAD_ADDRESS + 0x50
        if f"F {invalid_subfield:08X}" in whole_ranges:
            raise AssertionError("whole-image published invalid REGIMM data as code")
        whole_source = (whole_output / "fixture-overlay_full.c").read_text(
            encoding="utf-8"
        )
        if f"func_{invalid_subfield:08X}" in whole_source:
            raise AssertionError("overlay output retained a fail-closed data stub")
        whole_provenance = json.loads(
            (whole_output / "fixture-overlay_input.json").read_text(encoding="utf-8")
        )
        if whole_provenance.get("discovery") != "whole-image":
            raise AssertionError("whole-image discovery was not recorded in provenance")

        developer_load = 0x80280000
        developer_seeds = temporary / "developer.seeds"
        developer_seeds.write_text(
            f"call_root 0x{developer_load:08X}\n",
            encoding="ascii",
        )
        developer_output = temporary / "developer-aperture"
        developer = invoke(
            recompiler, raw, developer_seeds, runtime_include, developer_output,
            digest, entry=developer_load, discovery="whole-image",
            load_address=developer_load,
        )
        if developer.returncode != 0:
            raise AssertionError(developer.stderr or developer.stdout)
        developer_provenance = json.loads(
            (developer_output / "fixture-overlay_input.json").read_text(encoding="utf-8")
        )
        if developer_provenance.get("load_address") != "0x80280000":
            raise AssertionError(
                f"developer load address was not preserved: {developer_provenance}"
            )

        invalid_discovery_output = temporary / "invalid-discovery"
        invalid_discovery = invoke(
            recompiler, raw, seeds, runtime_include, invalid_discovery_output,
            digest, discovery="invent-functions",
        )
        if invalid_discovery.returncode == 0:
            raise AssertionError("invalid overlay discovery mode was accepted")
        if invalid_discovery_output.exists():
            raise AssertionError("invalid discovery mode published an output directory")

        for name, bad_digest, bad_entry in (
            ("digest-mismatch", "00" * 32, LOAD_ADDRESS),
            ("uppercase-digest", digest.upper(), LOAD_ADDRESS),
            ("entry-outside", digest, LOAD_ADDRESS + len(image)),
        ):
            rejected_output = temporary / name
            rejected = invoke(
                recompiler, raw, seeds, runtime_include, rejected_output,
                bad_digest, bad_entry,
            )
            if rejected.returncode == 0:
                raise AssertionError(f"{name}: invalid raw input was accepted")
            if rejected_output.exists():
                raise AssertionError(f"{name}: rejection published an output directory")

    print("raw overlay codegen contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
