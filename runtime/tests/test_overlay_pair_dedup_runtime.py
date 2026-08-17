#!/usr/bin/env python3
"""Build and execute the real loader's whole-pair dedup state machine."""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import re
import shutil
import subprocess
import tempfile
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
TESTS = RUNTIME / "tests"
GAME = "PAIR-TEST"
PAIR = 0x1020304050607080
RESIDENT = "psxrecomp bios resident shard v1\n"
GAME_SHA256 = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
MANIFEST_SHA256 = "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"


def codegen_leaf() -> str:
    api = (RUNTIME / "include" / "overlay_api.h")
    hash_header = (RUNTIME / "include" / "overlay_codegen_hash.h")
    version = "0"
    code_hash = "00000000"
    if api.is_file():
        match = re.search(r"PSX_OVERLAY_CODEGEN_VER\s+(\d+)",
                          api.read_text(encoding="utf-8"))
        if match:
            version = match.group(1)
    if hash_header.is_file():
        match = re.search(r"PSX_OVERLAY_CODEGEN_HASH\s+0x([0-9A-Fa-f]{8})",
                          hash_header.read_text(encoding="utf-8"))
        if match:
            code_hash = match.group(1).lower()
    return f"cg{version}_{code_hash}"


def identity_namespace() -> str:
    return f"game_{GAME_SHA256}/manifest_{MANIFEST_SHA256}"


def arch_abi() -> str:
    os_name = {"Windows": "win", "Linux": "linux", "Darwin": "macos"}[
        platform.system()
    ]
    machine = platform.machine().lower()
    if machine in ("amd64", "x86_64", "x64"):
        arch = "x64"
    elif machine in ("arm64", "aarch64"):
        arch = "arm64"
    elif machine in ("i386", "i686", "x86"):
        arch = "x86"
    else:
        arch = "unknown"
    return f"{os_name}-{arch}"


def run(command: list[str], *, cwd: pathlib.Path | None = None) -> None:
    result = subprocess.run(command, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if result.stdout.strip():
        print(result.stdout.strip())


def manifest(entries: list[tuple[int, int]], provenance: str | None = None,
              artifact: tuple[int, int, int] = (0x80010000, 16, 0x11111111),
              include_identity: bool = True,
              lowercase_identity: bool = False) -> str:
    lines = []
    if provenance:
        lines.append(f"# psxrecomp overlay provenance {provenance}\n")
    lines.append(f"P {PAIR:016X}\n")
    if include_identity:
        game_identity = GAME_SHA256.lower() if lowercase_identity else GAME_SHA256.upper()
        manifest_identity = (MANIFEST_SHA256.lower()
                             if lowercase_identity else MANIFEST_SHA256.upper())
        lines.append(f"I {game_identity} {manifest_identity}\n")
    lines.append(f"A {artifact[0]:08X} {artifact[1]:08X} {artifact[2]:08X}\n")
    for entry, length in entries:
        crc = zlib.crc32(bytes(length)) & 0xFFFFFFFF
        lines.append(f"F {entry:08X} {crc:08X}\n")
        lines.append(f"R {entry:08X} {length:08X}\n")
    return "".join(lines)


def compile_fixture(gcc: str, out: pathlib.Path, *, instance: int,
                    partial: bool = False) -> None:
    command = [gcc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", "-shared",
               f"-I{RUNTIME / 'include'}", f"-DTEST_INSTANCE={instance}"]
    if platform.system() != "Windows":
        command.append("-fPIC")
    if partial:
        command.append("-DTEST_PARTIAL_EXPORTS=1")
    command += [str(TESTS / "overlay_pair_dedup_fixture.c"), "-o", str(out)]
    run(command)


def compile_harness(gcc: str, out: pathlib) -> None:
    platform_defines = ["-D_GNU_SOURCE"] if platform.system() != "Windows" else []
    command = [
        gcc, "-std=c11", "-O0", "-Wall", "-Wextra",
        "-DPSX_NO_DEBUG_TOOLS",
        "-DPSX_OVERLAY_DLL_BUILD",
        "-DPSX_OVERLAY_TEST_CANDIDATE_CAP=4", f"-I{RUNTIME / 'include'}",
        f"-I{ROOT.parent / 'native_renderer' / 'include'}",
        *platform_defines,
        str(RUNTIME / "src" / "overlay_loader.c"),
        str(RUNTIME / "src" / "native_render_baseline.c"),
        str(RUNTIME / "src" / "game_identity.c"),
        str(RUNTIME / "src" / "overlay_path_canon.c"),
        str(RUNTIME / "src" / "overlay_posix.c"),
        str(RUNTIME / "src" / "crc32.c"),
        str(TESTS / "overlay_pair_dedup_harness.c"), "-o", str(out),
        f'-DPSX_GAME_EXTRA_IDENTITY_SHA256="{GAME_SHA256}"',
        f'-DPSX_GAME_MANIFEST_DIGEST_SHA256="{MANIFEST_SHA256}"',
    ]
    if platform.system() != "Windows":
        command += ["-ldl", "-pthread"]
    run(command)


def publish(cache: pathlib.Path, tier: str, filename: str, library: pathlib.Path,
            text: str, *, pending: bool = False, namespaced: bool = True) -> pathlib.Path:
    leaf = cache / GAME
    if namespaced:
        leaf /= identity_namespace()
    leaf /= pathlib.Path(tier) / arch_abi() / codegen_leaf()
    leaf.mkdir(parents=True, exist_ok=True)
    target = leaf / filename
    disk_target = pathlib.Path(f"{target}.pending") if pending else target
    ranges = target.with_suffix(".ranges")
    resident = target.with_suffix(".resident")
    disk_ranges = pathlib.Path(f"{ranges}.pending") if pending else ranges
    disk_resident = pathlib.Path(f"{resident}.pending") if pending else resident
    shutil.copy2(library, disk_target)
    disk_ranges.write_text(text, encoding="ascii", newline="")
    disk_resident.write_text(
        RESIDENT, encoding="ascii", newline="")
    return target


def scenario(tmp: pathlib.Path, harness: pathlib.Path, full_first: pathlib.Path,
             full_second: pathlib.Path,
             partial: pathlib.Path, name: str) -> None:
    ext = ".dll" if platform.system() == "Windows" else ".so"
    cache = tmp / f"cache-{name}"
    two = [(0x80010000, 4), (0x80010004, 4)]
    four = two + [(0x80010008, 4), (0x8001000C, 4)]
    first_tier = "gcc"
    second_tier = "tcc" if name in (
        "cross-tier", "variant-chain", "all-stale") else "gcc"
    first_manifest = manifest(four if name == "alias-at-cap" else two)
    second_manifest = first_manifest
    first_library = partial if name == "partial-first" else full_first
    rejects = name in ("missing-identity", "lowercase-identity", "flat-cache")
    if name == "missing-identity":
        first_manifest = manifest(two, include_identity=False)
        second_manifest = first_manifest
    if name == "lowercase-identity":
        first_manifest = manifest(two, lowercase_identity=True)
        second_manifest = first_manifest
    if name == "manifest-mismatch":
        second_manifest = manifest([(0x80010000, 8), (0x80010004, 4)])
    elif name == "provenance-mismatch":
        second_manifest = manifest(two, "hosted-v1")
    elif name == "artifact-mismatch":
        second_manifest = manifest(two, artifact=(0x80011000, 16, 0x22222222))

    first = publish(cache, first_tier, f"00010000_11111111_AAAAAAAA{ext}",
                    first_library, first_manifest, namespaced=name != "flat-cache")
    second = publish(cache, second_tier, f"00011000_22222222_BBBBBBBB{ext}",
                     full_second, second_manifest, pending=True,
                     namespaced=name != "flat-cache")
    trace = tmp / f"trace-{name}.log"
    os.environ["PSX_PAIR_TEST_TRACE"] = str(trace)
    run([str(harness), str(cache), name, str(first), str(second)])
    observed = trace.read_text(encoding="ascii").splitlines() if trace.exists() else []
    expected = ([] if rejects else ["init 1", "call 1", "flush 1"] if name == "alias-at-cap"
                else ["init 1", "init 2", "call 2", "flush 2"] if name == "variant-chain"
                else ["init 2"] if name == "partial-first"
                else ["init 1", "init 2"])
    if observed != expected:
        raise AssertionError(
            f"{name}: persistent DLL effects {observed!r}, expected {expected!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default=shutil.which("gcc") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.gcc:
        raise SystemExit("gcc/cc is required")
    with tempfile.TemporaryDirectory(
            prefix="psx-pair-dedup-", ignore_cleanup_errors=True) as raw:
        tmp = pathlib.Path(raw)
        ext = ".dll" if platform.system() == "Windows" else ".so"
        exe = ".exe" if platform.system() == "Windows" else ""
        full_first = tmp / f"full-first{ext}"
        full_second = tmp / f"full-second{ext}"
        partial = tmp / f"partial{ext}"
        harness = tmp / f"pair-harness{exe}"
        compile_fixture(args.gcc, full_first, instance=1)
        compile_fixture(args.gcc, full_second, instance=2)
        compile_fixture(args.gcc, partial, instance=1, partial=True)
        compile_harness(args.gcc, harness)
        for name in ("alias-at-cap", "manifest-mismatch",
                       "provenance-mismatch", "artifact-mismatch", "cross-tier",
                       "partial-first", "missing-identity", "lowercase-identity",
                       "flat-cache", "variant-chain", "all-stale"):
            scenario(tmp, harness, full_first, full_second, partial, name)
    print("PASS: executable overlay whole-pair dedup behavior")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
