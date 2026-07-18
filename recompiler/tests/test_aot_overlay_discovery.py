#!/usr/bin/env python3
"""Focused regression checks for play-free overlay call-target discovery."""
import argparse
import importlib.util
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "compile_overlays", ROOT / "tools" / "compile_overlays.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)

LOAD = 0x80010000


def put(data, offset, word):
    struct.pack_into("<I", data, offset, word)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def make_psxexe(entry, data):
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    put(header, 0x10, entry)
    put(header, 0x18, LOAD)
    put(header, 0x1C, len(data))
    return bytes(header) + data


def check_composite_call_boundaries():
    data = bytearray(0x100)
    cross_target = LOAD + 0x50
    put(data, 0x00, jal(cross_target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x50, 0x24020001)  # valid MIPS, but no callable boundary
    ranges = ((LOAD, LOAD + 0x40), (LOAD + 0x40, LOAD + 0x100))

    # A direct JAL remains sufficient for frameless leaves inside one producer,
    # but not when an adjacent-producer variant merely puts pointer/data-shaped
    # bytes at the encoded address.
    unscoped = MOD._walk_overlay_function(data, LOAD, len(data), LOAD, LOAD + 0x100)
    assert cross_target in unscoped['direct_jals']
    scoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges)
    assert cross_target not in scoped['direct_jals']
    assert cross_target in scoped['rejected_cross_producer_calls']

    # A bounded frameless body with a real return is independently code-like
    # and remains discoverable across the producer boundary.
    put(data, 0x54, 0x03E00008)
    put(data, 0x58, 0x00000000)
    scoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges)
    assert cross_target in scoped['direct_jals']

    # Independent target-local boundary evidence makes a real cross-producer
    # export safe to retain.
    put(data, 0x50, 0x27BDFFE0)
    scoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges)
    assert cross_target in scoped['direct_jals']


def check_static_discovery_provenance():
    data = bytearray(0x80)
    entry = LOAD + 0x20
    put(data, 0x20, 0x30620008)  # andi v0,v1,8 -- frameless body
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{entry:08X}'],
        'static_discovery_entry_pcs': [f'0x{entry:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert f'call_root 0x{entry:08X}' in seeds
    assert audit['included_reasons'][entry] == 'STATIC_DISCOVERY_ROOT'


def check_static_alias_recipe():
    data = bytearray(0x40)
    put(data, 0x00, 0x03E00008)
    put(data, 0x04, 0x00000000)
    put(data, 0x20, 0x24020001)
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{LOAD + 0x20:08X}'],
        'static_alias_ranges': [{
            'entry': f'0x{LOAD + 0x20:08X}',
            'start': f'0x{LOAD:08X}',
            'end': f'0x{LOAD + 0x2C:08X}',
        }],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert (f'retained_alias 0x{LOAD + 0x20:08X} '
            f'0x{LOAD:08X} 0x{LOAD + 0x2C:08X}') in seeds
    assert audit['static_alias_ranges'] == {
        (LOAD + 0x20, LOAD, LOAD + 0x2C)}


def check_optional_enrichment_fallback():
    direct = LOAD + 0x20
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{direct:08X}'],
        'dispatch_entry_pcs': [f'0x{LOAD:08X}', f'0x{direct:08X}'],
        'static_discovery_entry_pcs': [f'0x{direct:08X}'],
        'seeds': [f'0x{LOAD:08X}', f'0x{direct:08X}'],
        'static_alias_ranges': [{
            'entry': f'0x{direct:08X}',
            'start': f'0x{LOAD:08X}',
            'end': f'0x{LOAD + 0x2C:08X}',
        }],
        '_prior_aliases': [(direct, LOAD, LOAD + 0x2C)],
        'optional_enrichment_fallback_entry_pcs': [f'0x{direct:08X}'],
    }
    fallback = MOD.optional_enrichment_fallback_capture(cap)
    encoded = [f'0x{direct:08X}']
    assert fallback['function_entry_pcs'] == encoded
    assert fallback['dispatch_entry_pcs'] == encoded
    assert fallback['static_discovery_entry_pcs'] == encoded
    assert fallback['seeds'] == encoded
    assert 'static_alias_ranges' not in fallback
    assert '_prior_aliases' not in fallback
    assert 'optional_enrichment_fallback_entry_pcs' not in fallback
    assert MOD.optional_enrichment_fallback_capture(fallback) is None


def check_forward_branch_root():
    data = bytearray(0x80)
    target = LOAD + 0x40
    put(data, 0x00, 0x1000000F)  # beq zero,zero,target
    put(data, 0x04, 0x00000000)
    put(data, 0x20, 0x27BDFFF0)  # sibling root that hard-caps the first walk
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x27BD0010)
    put(data, 0x40, 0x24020001)  # forward branch target, frameless
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{LOAD + 0x20:08X}'],
        'static_discovery_entry_pcs': [
            f'0x{LOAD:08X}', f'0x{LOAD + 0x20:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert f'call_root 0x{target:08X}' in seeds
    assert audit['included_reasons'][target] == 'STATIC_BRANCH_ROOT'


def check_padded_return_boundary(recompiler):
    # Exact-entry mode re-verifies every untrusted seed. Psy-Q aligns frameless
    # leaves with NOPs after the prior function's JR delay slot; those NOPs must
    # not erase the otherwise exact return-boundary evidence.
    data = bytearray(0x80)
    put(data, 0x00, 0x03E00008)  # jr $ra
    put(data, 0x04, 0x24020007)  # non-NOP delay slot
    # 0x08..0x10 are alignment NOPs.
    put(data, 0x14, 0x24020001)  # padded frameless leaf
    put(data, 0x18, 0x03E00008)
    put(data, 0x1C, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "padded.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\n0x{LOAD + 0x14:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = next(pathlib.Path(out).glob("*_full.c")).read_text()
        assert f"void func_{LOAD + 0x14:08X}" in full
        ranges = next(pathlib.Path(out).glob("*_full.ranges")).read_text()
        assert f"F {LOAD + 0x14:08X}" in ranges


def check_recompiler_composite_contract(recompiler):
    data = bytearray(0x80)
    target = LOAD + 0x50
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x50, 0x24020001)
    put(data, 0x54, 0x03E00008)
    put(data, 0x58, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "composite.psx")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))

        def generated(allow):
            seeds = os.path.join(tmp, f"seeds_{allow}.txt")
            out = os.path.join(tmp, f"out_{allow}")
            with open(seeds, "w", encoding="utf-8") as f:
                f.write(f"producer_range 0x{LOAD:08X} 0x{LOAD + 0x40:08X}\n")
                f.write(f"producer_range 0x{LOAD + 0x40:08X} 0x{LOAD + 0x80:08X}\n")
                if allow:
                    f.write(f"cross_call_allow 0x{target:08X}\n")
                f.write(f"0x{LOAD:08X}\n")
            result = subprocess.run(
                [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
                capture_output=True, text=True)
            assert result.returncode == 0, result.stderr or result.stdout
            return ''.join(path.read_text() for path in pathlib.Path(out).glob("*_full*.c"))

        assert f"void func_{target:08X}" not in generated(False)
        assert f"void func_{target:08X}" in generated(True)


def check_retained_alias_contract(recompiler):
    # A previously compiled indirect entry retains its overlapping host body
    # without becoming a root that caps the newly discovered ordinary walk.
    data = bytearray(0x80)
    put(data, 0x00, 0x03E00008)
    put(data, 0x04, 0x00000000)
    put(data, 0x20, 0x24020001)
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "retained_alias.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\n")
            f.write(
                f"retained_alias 0x{LOAD + 0x20:08X} "
                f"0x{LOAD:08X} 0x{LOAD + 0x2C:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text() for path in pathlib.Path(out).glob("*_full*.c"))
        assert f"void func_{LOAD + 0x20:08X}" in full
        ranges = next(pathlib.Path(out).glob("*_full.ranges")).read_text()
        assert f"F {LOAD + 0x20:08X}" in ranges
        assert f"R {LOAD:08X} 2C" in ranges


def main():
    default_recompiler = ROOT / "recompiler" / "build" / "psxrecomp-game.exe"
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=str(default_recompiler))
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    check_composite_call_boundaries()
    check_static_discovery_provenance()
    check_static_alias_recipe()
    check_optional_enrichment_fallback()
    check_forward_branch_root()
    check_recompiler_composite_contract(args.recompiler)
    check_retained_alias_contract(args.recompiler)

    data = bytearray(0x200)

    # A framed root directly calls a frameless leaf.  JAL is the boundary
    # proof; the leaf deliberately has neither a prologue nor a preceding
    # return.
    put(data, 0x00, 0x27BDFFF0)
    put(data, 0x04, jal(LOAD + 0x40))
    put(data, 0x08, 0x00000000)
    put(data, 0x0C, 0x03E00008)
    put(data, 0x10, 0x27BD0010)
    put(data, 0x40, 0x24020001)
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)

    cap = {
        "schema": "psxrecomp overlay capture v2",
        "load_addr": f"0x{LOAD:08X}",
        "size": len(data),
        "executed_pcs": [],
        "dispatch_entry_pcs": [f"0x{LOAD:08X}"],
        "function_entry_pcs": [f"0x{LOAD:08X}"],
        "seeds": [f"0x{LOAD:08X}"],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit["included_reasons"][LOAD + 0x40] == "DIRECT_JAL_TARGET"
    assert f"call_root 0x{LOAD + 0x40:08X}" in seeds

    # A constant-register JR tail-calls backward to a frameless leaf.
    put(data, 0x100, 0x27BDFFF0)
    put(data, 0x104, 0x3C088001)  # lui $t0,0x8001
    put(data, 0x108, 0x25080040)  # addiu $t0,$t0,0x40
    put(data, 0x10C, 0x01000008)  # jr $t0
    put(data, 0x110, 0x27BD0010)
    walk = MOD._walk_overlay_function(
        bytes(data), LOAD, len(data), LOAD + 0x100, LOAD + len(data))
    assert walk["static_indirect_targets"] == {LOAD + 0x40}

    check_padded_return_boundary(args.recompiler)

    print("ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
