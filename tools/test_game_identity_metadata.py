#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import compile_overlays


ROOT = Path(__file__).resolve().parents[1]

class GameIdentityMetadataTests(unittest.TestCase):
    def test_production_autocompile_passes_runtime_identity_arguments(self) -> None:
        source = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8-sig")
        command_start = source.index("std::string built_tcc_cmd")
        command_end = source.index("ac_cmd = &built_tcc_cmd", command_start)
        command_source = source[command_start:command_end]

        self.assertIn("psx_game_identity_runtime()", command_source)
        self.assertIn("psx_game_identity_format_hex", command_source)
        game_argument = command_source.index("--game-identity-sha256")
        manifest_argument = command_source.index("--manifest-identity-sha256")
        configure = command_source.index("built_tcc_cmd =")
        self.assertLess(configure, game_argument)
        self.assertLess(game_argument, manifest_argument)

    def test_static_dispatch_gates_identity_before_crc_and_native_call(self) -> None:
        identity = compile_overlays.parse_game_identity("12" * 32, "34" * 32)
        variants = [{
            "addr": 0x80010000,
            "crc": 0x12345678,
            "ranges": [(0x80010000, 8)],
            "symbol": "synthetic_native_overlay",
        }]

        source = compile_overlays.generate_overlay_dispatch(variants, identity)

        self.assertIn("PsxGameIdentity", source)
        dispatch = source.index("int psx_overlay_dispatch")
        gate = source.index("psx_game_identity_gate", dispatch)
        crc = source.index("psx_overlay_static_code_matches", dispatch)
        native_call = source.index("synthetic_native_overlay(cpu)", dispatch)
        self.assertLess(gate, crc)
        self.assertLess(crc, native_call)

    def test_static_output_reuse_requires_complete_matching_identity(self) -> None:
        expected = compile_overlays.parse_game_identity(
            "01020304" + ("05" * 28), "10" * 32)
        same_uint32_different_sha = compile_overlays.parse_game_identity(
            "01020304" + ("06" * 28), "10" * 32)

        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "overlays_static.c"
            output.write_text("/* legacy static output */\n", encoding="ascii")
            self.assertFalse(
                compile_overlays.static_output_identity_matches(output, expected))

            output.write_text(
                "/* psxrecomp static identity v1: malformed */\n",
                encoding="ascii",
            )
            self.assertFalse(
                compile_overlays.static_output_identity_matches(output, expected))

            output.write_text(
                compile_overlays.static_identity_metadata(same_uint32_different_sha),
                encoding="ascii",
            )
            self.assertFalse(
                compile_overlays.static_output_identity_matches(output, expected))

            output.write_text(
                compile_overlays.static_identity_metadata(expected),
                encoding="ascii",
            )
            self.assertTrue(
                compile_overlays.static_output_identity_matches(output, expected))

    def test_static_dispatch_rejects_missing_and_full_sha_mismatch(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("C compiler is required for generated dispatch test")
        generated_identity = compile_overlays.parse_game_identity(
            "01020304" + ("05" * 28), "20" * 32)
        runtime_mismatch = "01020304" + ("06" * 28)
        variants = [{
            "addr": 0x80010000,
            "crc": 0x12345678,
            "ranges": [(0x80010000, 8)],
            "symbol": "synthetic_native_overlay",
        }]
        dispatch = compile_overlays.generate_overlay_dispatch(
            variants, generated_identity)
        harness = (
            "#include <stdint.h>\n"
            "typedef struct CPUState { int unused; } CPUState;\n"
            "static int native_calls;\n"
            "void synthetic_native_overlay(CPUState *cpu) { "
            "(void)cpu; native_calls++; }\n"
            "int psx_overlay_static_code_matches(const uint32_t *ranges, "
            "uint32_t count, uint32_t crc) {\n"
            "    (void)ranges; (void)count; (void)crc; return 1;\n"
            "}\n"
            f"{dispatch}\n"
            "int main(void) {\n"
            "    CPUState cpu = {0};\n"
            "    int dispatched = psx_overlay_dispatch(&cpu, 0x80010000u);\n"
            "    return dispatched == EXPECT_NATIVE && "
            "native_calls == EXPECT_NATIVE ? 0 : 1;\n"
            "}\n"
        )
        cases = (
            (
                "matching",
                [
                    generated_identity.game_sha256.hex(),
                    generated_identity.manifest_sha256.hex(),
                ],
                1,
            ),
            (
                "same-uint32-mismatch",
                [runtime_mismatch, generated_identity.manifest_sha256.hex()],
                0,
            ),
            (
                "invalid-runtime",
                ["AA" * 32, generated_identity.manifest_sha256.hex()],
                0,
            ),
            ("partial", [generated_identity.game_sha256.hex()], 0),
            ("missing", [], 0),
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "static_dispatch.c"
            source.write_text(harness, encoding="ascii")
            for name, runtime_identity, expected_native in cases:
                executable = temporary / name
                command = [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-DEXPECT_NATIVE={expected_native}",
                    f"-I{ROOT / 'runtime' / 'include'}",
                ]
                if runtime_identity:
                    command.append(
                        f'-DPSX_GAME_EXTRA_IDENTITY_SHA256="{runtime_identity[0]}"')
                if len(runtime_identity) == 2:
                    command.append(
                        f'-DPSX_GAME_MANIFEST_DIGEST_SHA256="{runtime_identity[1]}"')
                command.extend((
                    str(source),
                    str(ROOT / "runtime" / "src" / "game_identity.c"),
                    "-o",
                    str(executable),
                ))
                compile_result = subprocess.run(
                    command, capture_output=True, text=True, check=False)
                self.assertEqual(
                    compile_result.returncode,
                    0,
                    compile_result.stderr,
                )
                run_result = subprocess.run(
                    [str(executable)], capture_output=True, text=True, check=False)
                self.assertEqual(run_result.returncode, 0, name)

    def test_identity_requires_two_exact_sha256_values(self) -> None:
        valid_game = "00" * 32
        valid_manifest = "11" * 32
        identity = compile_overlays.parse_game_identity(valid_game, valid_manifest)
        self.assertEqual(identity.game_sha256, bytes(32))
        self.assertEqual(identity.manifest_sha256, bytes.fromhex(valid_manifest))
        with self.assertRaises(ValueError):
            compile_overlays.parse_game_identity("", valid_manifest)
        with self.assertRaises(ValueError):
            compile_overlays.parse_game_identity("00" * 31, valid_manifest)
        with self.assertRaises(ValueError):
            compile_overlays.parse_game_identity(valid_game, "GG" * 32)

    def test_manifest_pair_and_cache_namespace_bind_full_identity(self) -> None:
        first = compile_overlays.parse_game_identity("01" * 32, "02" * 32)
        second = compile_overlays.parse_game_identity("01" * 32, "03" * 32)
        functions = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
        first_manifest = compile_overlays.overlay_ranges_text(functions, identity=first)
        second_manifest = compile_overlays.overlay_ranges_text(functions, identity=second)
        self.assertIn("I " + ("01" * 32).upper(), first_manifest)
        self.assertNotEqual(first_manifest, second_manifest)
        self.assertNotEqual(
            compile_overlays.overlay_pair_id("void f(void) {}", functions, identity=first),
            compile_overlays.overlay_pair_id("void f(void) {}", functions, identity=second),
        )
        self.assertNotEqual(
            compile_overlays.identity_cache_namespace(first),
            compile_overlays.identity_cache_namespace(second),
        )

    def test_runtime_shard_manifest_accepts_full_identity_overlay_ranges_text(self) -> None:
        # Given
        identity = compile_overlays.parse_game_identity("01" * 32, "02" * 32)
        functions = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
        manifest = compile_overlays.overlay_ranges_text(functions, identity=identity)

        # When
        parsed = compile_overlays.parse_runtime_shard_manifest(
            manifest, require_pair=False)

        # Then
        self.assertEqual(parsed, (None, [
            (0x80010000, 0x12345678, [(0x80010000, 4)]),
        ]))

    def test_artifact_tuple_is_pair_bound_and_part_of_semantic_identity(self) -> None:
        identity = compile_overlays.parse_game_identity("01" * 32, "02" * 32)
        functions = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
        artifact = (0x80010000, 4, 0x12345678)
        other_artifact = (0x80010000, 8, 0x12345678)

        manifest = compile_overlays.overlay_ranges_text(
            functions, identity=identity, artifact=artifact)
        parsed = compile_overlays.parse_runtime_shard_manifest(
            manifest, require_pair=False, include_artifact=True)

        self.assertIn("A 80010000 4 12345678", manifest)
        self.assertEqual(parsed, (None, functions, artifact))
        self.assertNotEqual(
            compile_overlays.overlay_pair_id(
                "void f(void) {}", functions, identity=identity,
                artifact=artifact),
            compile_overlays.overlay_pair_id(
                "void f(void) {}", functions, identity=identity,
                artifact=other_artifact),
        )

    def test_runtime_shard_manifest_preserves_artifact_arity_when_malformed(self) -> None:
        malformed = "A 80010000 invalid 12345678\n"

        parsed = compile_overlays.parse_runtime_shard_manifest(
            malformed, require_pair=False, include_artifact=True)

        self.assertEqual(parsed, (None, [], None))

    def test_overlay_metadata_exports_raw_arrays_and_loader_rejects_legacy_identity(self) -> None:
        identity = compile_overlays.parse_game_identity("0a" * 32, "0b" * 32)
        source = compile_overlays.add_overlay_identity_export("void f(void) {}\n", identity)
        self.assertIn("uint8_t", source)
        self.assertIn("overlay_game_identity", source)
        self.assertNotIn("0a" * 32, source)

        legacy = "F 80010000 12345678\nR 80010000 4\n"
        self.assertEqual(compile_overlays.parse_runtime_shard_manifest(legacy), (None, []))

        loader = (ROOT / "runtime" / "src" / "overlay_loader.c").read_text(encoding="utf-8")
        self.assertGreaterEqual(loader.count("overlay_game_identity"), 2)
        self.assertIn("psx_game_identity_gate", loader)
        self.assertIn("MANIFEST_IDENTITY", loader)


if __name__ == "__main__":
    unittest.main()
