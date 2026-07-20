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
import time


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


def make_bounded_switch():
    """Ape-shaped cross-register table base and post-JR case bodies."""
    data = bytearray(0x300)
    entry = LOAD + 0x30
    table = LOAD + 0x200
    cases = (LOAD + 0x68, LOAD + 0x88, LOAD + 0x68, LOAD + 0x78)
    put(data, 0x30, 0x27BDFFE0)  # addiu sp,sp,-0x20
    put(data, 0x34, 0xAFBF0018)  # sw ra,0x18(sp)
    put(data, 0x38, 0x3C028001)  # lui v0,0x8001
    put(data, 0x3C, 0x24500200)  # addiu s0,v0,0x200 (register rename)
    put(data, 0x40, 0x24030000)  # addiu v1,zero,0
    put(data, 0x44, 0x2C620004)  # sltiu v0,v1,4
    put(data, 0x48, 0x1040FFFF)  # beq v0,zero,0x48 (reject loop)
    put(data, 0x4C, 0x00000000)
    put(data, 0x50, 0x00031080)  # sll v0,v1,2
    put(data, 0x54, 0x00501021)  # addu v0,v0,s0
    put(data, 0x58, 0x8C420000)  # lw v0,0(v0)
    put(data, 0x5C, 0x00000000)
    put(data, 0x60, 0x00400008)  # jr v0
    put(data, 0x64, 0x00000000)
    put(data, 0x68, 0x03E00008)
    put(data, 0x6C, 0x00000000)
    put(data, 0x78, 0x03E00008)
    put(data, 0x7C, 0x00000000)
    put(data, 0x88, jal(0x80008000))  # external call
    put(data, 0x8C, 0x00000000)
    put(data, 0x90, 0x03E00008)       # call continuation
    put(data, 0x94, 0x00000000)
    for index, target in enumerate(cases):
        put(data, 0x200 + index * 4, target)
    return data, entry, table, set(cases), LOAD + 0x90


def check_bounded_jump_table_discovery():
    data, entry, table, cases, continuation = make_bounded_switch()
    found = MOD._find_jump_table_targets(
        bytes(data), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2)
    assert found == cases
    zero_nops = bytearray(data)
    put(zero_nops, 0x5C, 0x00400008)
    assert MOD._find_jump_table_targets(
        bytes(zero_nops), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x5C, 2) == cases
    two_nops = bytearray(data)
    put(two_nops, 0x60, 0x00000000)
    put(two_nops, 0x64, 0x00400008)
    assert MOD._find_jump_table_targets(
        bytes(two_nops), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x64, 2) == set()
    three_nops = bytearray(two_nops)
    put(three_nops, 0x64, 0x00000000)
    put(three_nops, 0x68, 0x00400008)
    assert MOD._find_jump_table_targets(
        bytes(three_nops), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x68, 2) == set()
    walk = MOD._walk_overlay_function(
        bytes(data), LOAD, len(data), entry, LOAD + 0x100,
        ((LOAD, LOAD + len(data)),), False)
    assert walk['jump_table_targets'] == cases
    assert cases <= walk['visited']
    assert continuation in walk['call_continuations']
    assert continuation in walk['visited']

    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{entry:08X}'],
        'dispatch_entry_pcs': [
            f'0x{addr:08X}' for addr in sorted(cases | {continuation})],
        'producer_ranges': [{
            'start': f'0x{LOAD:08X}',
            'end': f'0x{LOAD + len(data):08X}',
        }],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    for addr in cases | {continuation}:
        assert audit['included_reasons'][addr] == 'DISPATCH_INTERIOR'
        assert f'interior 0x{addr:08X}' in seeds
        assert f'call_root 0x{addr:08X}' not in seeds

    # Every relationship is mandatory; opcode proximity or repeated pointers
    # alone cannot manufacture a switch.
    mismatched_index = bytearray(data)
    put(mismatched_index, 0x44, 0x2C820004)  # sltiu v0,a0,4
    assert MOD._find_jump_table_targets(
        bytes(mismatched_index), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    clobbered_base = bytearray(data)
    put(clobbered_base, 0x40, 0x26100004)  # addiu s0,s0,4
    assert MOD._find_jump_table_targets(
        bytes(clobbered_base), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_jr = bytearray(data)
    put(noncanonical_jr, 0x60, 0x00410008)  # reserved rt field is nonzero
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_jr), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    zero_lw_base = bytearray(data)
    put(zero_lw_base, 0x58, 0x8C020000)  # lw v0,0(zero)
    assert MOD._find_jump_table_targets(
        bytes(zero_lw_base), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    zero_addu_dest = bytearray(data)
    put(zero_addu_dest, 0x54, 0x00500021)  # addu zero,v0,s0
    assert MOD._find_jump_table_targets(
        bytes(zero_addu_dest), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_addu = bytearray(data)
    put(noncanonical_addu, 0x54, 0x00501061)  # nonzero reserved shamt
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_addu), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_sll = bytearray(data)
    put(noncanonical_sll, 0x50, 0x00231080)  # SLL's reserved rs is nonzero
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_sll), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_lui = bytearray(data)
    put(noncanonical_lui, 0x38, 0x3C228001)  # LUI's reserved rs is nonzero
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_lui), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    # A validated case in producer A cannot use producer B as a trampoline
    # back into the protected suffix. Otherwise B's bytes would manufacture a
    # false domination proof for A's table-base and bounds definitions.
    producer_escape = bytearray(data)
    producer_b = LOAD + 0x260
    put(producer_escape, 0x68,
        0x08000000 | ((producer_b >> 2) & 0x03FFFFFF))
    put(producer_escape, 0x6C, 0x00000000)
    put(producer_escape, 0x260,
        0x08000000 | (((LOAD + 0x50) >> 2) & 0x03FFFFFF))
    put(producer_escape, 0x264, 0x00000000)
    assert MOD._find_jump_table_targets(
        bytes(producer_escape), LOAD, len(data), entry, LOAD + 0x300,
        LOAD + 0x60, 2, (LOAD, LOAD + 0x240)) == set()

    ori_base = bytearray(data)
    put(ori_base, 0x3C, 0x34500200)  # ori s0,v0,0x200 is outside grammar
    assert MOD._find_jump_table_targets(
        bytes(ori_base), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    skipped_constant = bytearray(data)
    put(skipped_constant, 0x34, 0x08000000 |
        (((LOAD + 0x40) >> 2) & 0x03FFFFFF))
    assert MOD._find_jump_table_targets(
        bytes(skipped_constant), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    later_inbound = bytearray(data)
    put(later_inbound, 0xA0, 0x08000000 |
        (((LOAD + 0x50) >> 2) & 0x03FFFFFF))
    assert MOD._find_jump_table_targets(
        bytes(later_inbound), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    foreign_target = bytearray(data)
    put(foreign_target, table - LOAD, LOAD + 0x180)
    assert MOD._find_jump_table_targets(
        bytes(foreign_target), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()
    assert MOD._find_jump_table_targets(
        bytes(data), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2, (LOAD, LOAD + 0x180)) == set()


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

    # Even within one nominal producer, a dense run of local pointers is data,
    # not a frameless callable merely because a reachable word looks like JAL.
    for index, offset in enumerate((0x70, 0x80, 0x90)):
        put(data, 0x50 + index * 4, LOAD + offset)
    unscoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100)
    assert cross_target not in unscoped['direct_jals']
    assert cross_target in unscoped['rejected_cross_producer_calls']

    strict = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges,
        allow_cross_producer_calls=False)
    assert cross_target not in strict['direct_jals']
    assert cross_target in strict['rejected_cross_producer_calls']

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


def check_owned_direct_call_is_interior():
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = host + 0x08
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x100, 0x24020001)
    put(data, 0x104, 0x24420001)
    put(data, 0x108, 0x24420001)
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{host:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][target] == 'DISPATCH_INTERIOR'
    assert f'interior 0x{target:08X}' in seeds


def check_discovered_host_owns_same_round_call_target():
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = host + 0x08
    put(data, 0x00, jal(host))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, jal(target))
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x03E00008)
    put(data, 0x14, 0x00000000)
    put(data, 0x100, 0x24020001)
    put(data, 0x104, 0x24420001)
    put(data, 0x108, 0x24420001)
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][host] == 'DIRECT_JAL_TARGET'
    assert audit['included_reasons'][target] == 'DISPATCH_INTERIOR'
    assert f'call_root 0x{host:08X}' in seeds
    assert f'interior 0x{target:08X}' in seeds


def check_later_discovered_host_retracts_interior_root():
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = LOAD + 0x120
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x100, 0x10000007)  # b target
    put(data, 0x104, 0x00000000)
    put(data, 0x120, jal(host))
    put(data, 0x124, 0x00000000)
    put(data, 0x128, 0x03E00008)
    put(data, 0x12C, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][host] == 'DIRECT_JAL_TARGET'
    assert audit['included_reasons'][target] == 'DISPATCH_INTERIOR'
    assert f'call_root 0x{host:08X}' in seeds
    assert f'interior 0x{target:08X}' in seeds


def check_recompiler_discovered_host_ownership(recompiler):
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = host + 0x08
    put(data, 0x00, jal(host))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, jal(target))
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x03E00008)
    put(data, 0x14, 0x00000000)
    put(data, 0x100, 0x24020001)
    put(data, 0x104, 0x24420001)
    put(data, 0x108, 0x24420001)
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 'discovered_ownership.psx')
        seeds = os.path.join(tmp, 'seeds.txt')
        out = os.path.join(tmp, 'out')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, 'w', encoding='utf-8') as f:
            f.write(f'0x{LOAD:08X}\n')
            f.write(f'interior 0x{target:08X}\n')
        result = subprocess.run(
            [recompiler, psx, '--seeds', seeds, '--out-dir', out, '--overlay'],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob('*_full*.c'))
        ranges = next(pathlib.Path(out).glob('*_full.ranges')).read_text()

    assert f'void func_{host:08X}' in full
    assert f'void func_{target:08X}' in full
    assert f'F {target:08X}' in ranges
    assert f'R {host:08X}' in ranges


def assert_all_interiors_have_final_host(data, seeds, producer_ranges=(),
                                         allow_cross=True):
    roots = []
    interiors = set()
    for seed in seeds:
        if seed.startswith('interior '):
            interiors.add(int(seed.split()[-1], 16))
        elif seed.startswith(('producer_range ', 'cross_call_allow ',
                              'retained_alias ')):
            continue
        else:
            roots.append(int(seed.split()[-1], 16))
    covered = set()
    roots.sort()
    for index, entry in enumerate(roots):
        hard_cap = roots[index + 1] if index + 1 < len(roots) else LOAD + len(data)
        covered.update(MOD._walk_overlay_function(
            bytes(data), LOAD, len(data), entry, hard_cap, producer_ranges,
            allow_cross)['visited'])
    assert interiors <= covered, (interiors - covered)


def check_ownership_invalidation_and_producer_boundaries():
    # R discovers A+B; expanded A discovers X between them. X caps A before B,
    # so the final partition must reconsider B as a root, not retain a stale
    # A-owned interior classification.
    data = bytearray(0x200)
    host = LOAD + 0x100
    middle = LOAD + 0x120
    target = LOAD + 0x140
    put(data, 0x00, jal(host))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, jal(target))
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x03E00008)
    put(data, 0x14, 0x00000000)
    put(data, 0x100, 0x08000000 | (((LOAD + 0x130) >> 2) & 0x03FFFFFF))
    put(data, 0x104, 0x00000000)
    put(data, 0x120, 0x03E00008)
    put(data, 0x124, 0x00000000)
    put(data, 0x130, 0x24420001)
    put(data, 0x134, 0x24420001)
    put(data, 0x138, 0x24420001)
    put(data, 0x13C, 0x24420001)
    put(data, 0x140, jal(middle))
    put(data, 0x144, 0x00000000)
    put(data, 0x148, 0x03E00008)
    put(data, 0x14C, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    root_addrs = sorted(int(seed.split()[-1], 16) for seed in seeds
                        if not seed.startswith(('interior ', 'producer_range ',
                                                'cross_call_allow ',
                                                'retained_alias ')))
    final_predecessor = max(addr for addr in root_addrs if addr < target)
    assert final_predecessor != host
    assert (audit['included_reasons'][target] != 'DISPATCH_INTERIOR' or
            final_predecessor == LOAD + 0x130)
    assert_all_interiors_have_final_host(data, seeds)

    # A and B straddle producer boundaries. The unapproved cross-producer call
    # is rejected entirely; approval makes B a separate root, never an alias
    # owned by A's apparent fallthrough.
    cross = bytearray(0x200)
    a = LOAD + 0x100
    b = LOAD + 0x108
    put(cross, 0x00, jal(a))
    put(cross, 0x04, 0x00000000)
    put(cross, 0x08, jal(b))
    put(cross, 0x0C, 0x00000000)
    put(cross, 0x10, 0x03E00008)
    put(cross, 0x14, 0x00000000)
    put(cross, 0x100, 0x24420001)
    put(cross, 0x104, 0x24420001)
    put(cross, 0x108, 0x03E00008)
    put(cross, 0x10C, 0x00000000)
    raw_ranges = [
        {'start': f'0x{LOAD:08X}', 'end': f'0x{b:08X}'},
        {'start': f'0x{b:08X}', 'end': f'0x{LOAD + len(cross):08X}'},
    ]
    strict_cap = dict(cap, producer_ranges=raw_ranges,
                      strict_producer_ranges=True)
    strict_seeds, strict_audit = MOD.classify_overlay_seeds(
        strict_cap, bytes(cross), LOAD, len(cross), 0, {})
    assert b not in strict_audit['included_reasons']
    assert b in strict_audit['rejected_cross_producer_calls']
    assert not any(seed.endswith(f'0x{b:08X}') and
                   not seed.startswith('producer_range ')
                   for seed in strict_seeds)
    parsed_ranges = [(LOAD, b), (b, LOAD + len(cross))]
    assert_all_interiors_have_final_host(
        cross, strict_seeds, parsed_ranges, False)

    allowed_cap = dict(cap, producer_ranges=raw_ranges,
                       strict_producer_ranges=False)
    allowed_seeds, allowed_audit = MOD.classify_overlay_seeds(
        allowed_cap, bytes(cross), LOAD, len(cross), 0, {})
    assert allowed_audit['included_reasons'][b] == 'DIRECT_JAL_TARGET'
    assert f'call_root 0x{b:08X}' in allowed_seeds
    assert f'interior 0x{b:08X}' not in allowed_seeds
    assert_all_interiors_have_final_host(
        cross, allowed_seeds, parsed_ranges, True)

    crossing_alias_cap = dict(
        strict_cap,
        static_alias_ranges=[{
            'entry': f'0x{a:08X}',
            'start': f'0x{a:08X}',
            'end': f'0x{b + 4:08X}',
        }])
    try:
        MOD.classify_overlay_seeds(
            crossing_alias_cap, bytes(cross), LOAD, len(cross), 0, {})
        assert False, 'cross-producer supplied alias was accepted'
    except RuntimeError as exc:
        assert 'crosses producer boundary' in str(exc)

    # A seed in composite padding has no producer. It must be rejected rather
    # than letting producer_for(None) equate disconnected uncovered gaps.
    gap = LOAD + 0x60
    gap_data = bytearray(0x100)
    put(gap_data, 0x00, 0x03E00008)
    put(gap_data, 0x04, 0x00000000)
    put(gap_data, 0x60, 0x24020001)
    put(gap_data, 0x64, 0x03E00008)
    put(gap_data, 0x68, 0x00000000)
    gap_cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{gap:08X}'],
        'producer_ranges': [
            {'start': f'0x{LOAD:08X}', 'end': f'0x{LOAD + 0x40:08X}'},
            {'start': f'0x{LOAD + 0x80:08X}',
             'end': f'0x{LOAD + len(gap_data):08X}'},
        ],
        'strict_producer_ranges': True,
    }
    gap_seeds, gap_audit = MOD.classify_overlay_seeds(
        gap_cap, bytes(gap_data), LOAD, len(gap_data), 0, {})
    assert gap not in gap_audit['included_reasons']
    assert not any(seed.endswith(f'0x{gap:08X}') and
                   not seed.startswith('producer_range ')
                   for seed in gap_seeds)

    orphan = LOAD + 0x180
    put(cross, 0x180, 0x24420001)
    orphan_cap = dict(cap, dispatch_entry_pcs=[f'0x{orphan:08X}'])
    orphan_seeds, orphan_audit = MOD.classify_overlay_seeds(
        orphan_cap, bytes(cross), LOAD, len(cross), 0, {})
    assert orphan not in orphan_audit['included_reasons']
    assert f'interior 0x{orphan:08X}' not in orphan_seeds
    assert_all_interiors_have_final_host(cross, orphan_seeds)


def check_recompiler_unreachable_jal_not_alias(recompiler):
    # The host's address envelope contains an unreachable JAL-shaped word whose
    # non-dense target looks like code. Only a raw envelope rescan sees it; the
    # reachable analyzer must not export it as an alias.
    data = bytearray(0x100)
    target = LOAD + 0x40
    put(data, 0x00, 0x08000000 | (((LOAD + 0x80) >> 2) & 0x03FFFFFF))
    put(data, 0x04, 0x00000000)
    put(data, 0x20, jal(target))
    put(data, 0x40, 0x24420001)
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)
    put(data, 0x80, 0x03E00008)
    put(data, 0x84, 0x00000000)
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 'unreachable_jal.psx')
        seeds = os.path.join(tmp, 'seeds.txt')
        out = os.path.join(tmp, 'out')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, 'w', encoding='utf-8') as f:
            f.write(f'0x{LOAD:08X}\n')
            f.write(f'interior 0x{target:08X}\n')
        result = subprocess.run(
            [recompiler, psx, '--seeds', seeds, '--out-dir', out, '--overlay'],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob('*_full*.c'))
    assert f'void func_{target:08X}' not in full


def check_recomputed_evidence_and_unconditional_branch():
    # R reaches S, S calls B, and B calls earlier X. Adding X caps R before S,
    # invalidating the only source edge for B. This oscillating partition must
    # fail closed to explicit R rather than retain monotonic B evidence.
    data = bytearray(0x200)
    x = LOAD + 0x80
    s = LOAD + 0x100
    b = LOAD + 0x140
    put(data, 0x00, 0x08000000 | ((s >> 2) & 0x03FFFFFF))
    put(data, 0x04, 0x00000000)
    put(data, 0x80, 0x03E00008)
    put(data, 0x84, 0x00000000)
    put(data, 0x100, jal(b))
    put(data, 0x104, 0x00000000)
    put(data, 0x108, 0x03E00008)
    put(data, 0x10C, 0x00000000)
    put(data, 0x140, jal(x))
    put(data, 0x144, 0x00000000)
    put(data, 0x148, 0x03E00008)
    put(data, 0x14C, 0x00000000)
    cap = {'schema': 'psxrecomp overlay capture v2',
           'function_entry_pcs': [f'0x{LOAD:08X}']}
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert b not in audit['included_reasons']
    assert not any(seed.endswith(f'0x{b:08X}') for seed in seeds)

    # Always-taken beq rN,rN has no PC+8 fallthrough. A JAL-shaped word
    # there is unreachable and cannot manufacture a root or alias.
    branch = bytearray(0x100)
    hidden = LOAD + 0x40
    put(branch, 0x00, (0x04 << 26) | (8 << 21) | (8 << 16) | 0x1F)
    put(branch, 0x04, 0x00000000)
    put(branch, 0x08, jal(hidden))
    put(branch, 0x0C, 0x00000000)
    put(branch, 0x40, 0x03E00008)
    put(branch, 0x44, 0x00000000)
    put(branch, 0x80, 0x03E00008)
    put(branch, 0x84, 0x00000000)
    branch_seeds, branch_audit = MOD.classify_overlay_seeds(
        cap, bytes(branch), LOAD, len(branch), 0, {})
    assert hidden not in branch_audit['included_reasons']
    assert not any(seed.endswith(f'0x{hidden:08X}') for seed in branch_seeds)

    # Never-taken bne rN,rN must not walk its encoded target.
    never = bytearray(0x100)
    put(never, 0x00, (0x05 << 26) | (8 << 21) | (8 << 16) | 0x1F)
    put(never, 0x04, 0x00000000)
    put(never, 0x08, 0x03E00008)
    put(never, 0x0C, 0x00000000)
    put(never, 0x40, 0x03E00008)
    put(never, 0x44, 0x00000000)
    put(never, 0x80, jal(hidden))
    put(never, 0x84, 0x00000000)
    put(never, 0x88, 0x03E00008)
    put(never, 0x8C, 0x00000000)
    never_seeds, never_audit = MOD.classify_overlay_seeds(
        cap, bytes(never), LOAD, len(never), 0, {})
    assert hidden not in never_audit['included_reasons']
    assert not any(seed.endswith(f'0x{hidden:08X}') for seed in never_seeds)

    # All zero-provable branch families use exact direction. Likely-false
    # variants additionally annul their delay slot.
    target = LOAD + 0x80
    assert MOD._classify_cf(LOAD, (0x06 << 26) | 0x1F) == ('j', target)
    assert MOD._classify_cf(LOAD, (0x07 << 26) | 0x1F) == ('branch_never', 0)
    assert MOD._classify_cf(LOAD, (0x01 << 26) | (0x00 << 16) | 0x1F) == (
        'branch_never', 0)  # bltz zero
    assert MOD._classify_cf(LOAD, (0x01 << 26) | (0x01 << 16) | 0x1F) == (
        'j', target)  # bgez zero
    assert MOD._classify_cf(LOAD, (0x01 << 26) | (0x11 << 16) | 0x1F) == (
        'jal', target)  # bal keeps both target and return continuation
    assert MOD._classify_cf(LOAD, (0x15 << 26) | (8 << 21) |
                            (8 << 16) | 0x1F) == ('branch_never_likely', 0)
    assert MOD._classify_cf(LOAD, (0x17 << 26) | 0x1F) == (
        'branch_never_likely', 0)

    # A non-canonical collection of branch-shaped words remains unresolved.
    assert MOD._find_jump_table_targets(
        bytes(branch), LOAD, len(branch), LOAD, LOAD + len(branch),
        LOAD + 0x20, 8) == set()

    # A jump enters at the low-half definition, skipping the preceding LUI.
    # The backward resolver must not combine definitions across that inbound
    # basic-block boundary.
    split = bytearray(0x600)
    put(split, 0x400, 0x03E00008)
    put(split, 0x404, 0x00000000)
    put(split, 0x500, 0x08000000 | (((LOAD + 0x518) >> 2) & 0x03FFFFFF))
    put(split, 0x504, 0x00000000)
    put(split, 0x514, 0x3C088001)
    put(split, 0x518, 0x25080400)
    put(split, 0x51C, 0x01000008)
    put(split, 0x520, 0x00000000)
    split_walk = MOD._walk_overlay_function(
        bytes(split), LOAD, len(split), LOAD + 0x500, LOAD + len(split))
    assert split_walk['static_indirect_targets'] == set()

    # A LUI in a JAL delay slot can be clobbered by the callee before control
    # returns to the low half. Raw adjacency is not reaching-definition proof.
    call_delay = bytearray(0x600)
    put(call_delay, 0x300, 0x03E00008)
    put(call_delay, 0x304, 0x00000000)
    put(call_delay, 0x400, 0x03E00008)
    put(call_delay, 0x404, 0x00000000)
    put(call_delay, 0x500, jal(LOAD + 0x300))
    put(call_delay, 0x504, 0x3C088001)
    put(call_delay, 0x508, 0x25080400)
    put(call_delay, 0x50C, 0x0100F809)
    put(call_delay, 0x510, 0x00000000)
    put(call_delay, 0x514, 0x03E00008)
    put(call_delay, 0x518, 0x00000000)
    call_delay_walk = MOD._walk_overlay_function(
        bytes(call_delay), LOAD, len(call_delay), LOAD + 0x500,
        LOAD + len(call_delay))
    assert call_delay_walk['direct_jals'] == {LOAD + 0x300}
    assert call_delay_walk['static_indirect_targets'] == set()


def check_t2_shaped_retained_partition_conflict(recompiler):
    # Shape of the causal T2 regression: current ordinary host BA0 ends +0x114,
    # new ordinary roots exist at +0x17C/+0x180, while an old retained alias
    # claims the original host through +0x2A8. The old range must be ignored.
    data = bytearray(0x300)
    old_alias = LOAD + 0x114
    split_a = LOAD + 0x17C
    split_b = LOAD + 0x180
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)
    put(data, 0x17C, 0x24420001)
    put(data, 0x180, 0x03E00008)
    put(data, 0x184, 0x00000000)
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 't2_partition_conflict.psx')
        seed_path = os.path.join(tmp, 'seeds.txt')
        out = os.path.join(tmp, 'out')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(LOAD, data))
        with open(seed_path, 'w', encoding='utf-8') as f:
            f.write(f'0x{LOAD:08X}\n')
            f.write(f'call_root 0x{split_a:08X}\n')
            f.write(f'call_root 0x{split_b:08X}\n')
            f.write(f'retained_alias 0x{old_alias:08X} '
                    f'0x{LOAD:08X} 0x{LOAD + 0x2A8:08X}\n')
        result = subprocess.run(
            [recompiler, psx, '--seeds', seed_path, '--out-dir', out,
             '--overlay'], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        ranges = next(pathlib.Path(out).glob('*_full.ranges')).read_text()
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob('*_full*.c'))
    assert 'ignoring legacy retained_alias' in result.stdout
    assert f'F {old_alias:08X}' not in ranges
    assert f'void func_{old_alias:08X}' not in full
    assert f'F {split_a:08X}' in ranges
    assert f'F {split_b:08X}' in ranges


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
    assert not any(seed.startswith('retained_alias ') for seed in seeds)
    assert f'interior 0x{LOAD + 0x20:08X}' not in seeds
    assert LOAD + 0x20 not in audit['included_reasons']
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
        'static_jump_table_proofs': [{'host_entry': f'0x{LOAD:08X}'}],
        'static_call_continuation_pcs': [f'0x{LOAD + 0x10:08X}'],
        'optional_enrichment_fallback_entry_pcs': [f'0x{direct:08X}'],
    }
    fallback = MOD.optional_enrichment_fallback_capture(cap)
    encoded = [f'0x{direct:08X}']
    assert fallback['function_entry_pcs'] == encoded
    assert fallback['dispatch_entry_pcs'] == encoded
    assert fallback['static_discovery_entry_pcs'] == encoded
    assert fallback['seeds'] == encoded
    assert 'static_alias_ranges' not in fallback
    assert 'static_jump_table_proofs' not in fallback
    assert 'static_call_continuation_pcs' not in fallback
    assert '_prior_aliases' not in fallback
    assert 'optional_enrichment_fallback_entry_pcs' not in fallback
    assert MOD.optional_enrichment_fallback_capture(fallback) is None

    dispatch = LOAD + 0x30
    cap['optional_enrichment_fallback_entry_pcs'] = {
        'function_entry_pcs': [],
        'dispatch_entry_pcs': [f'0x{dispatch:08X}'],
        'static_discovery_entry_pcs': [],
        'producer_ranges': [{
            'start': f'0x{LOAD + 0x20:08X}',
            'end': f'0x{LOAD + 0x40:08X}',
        }],
        'strict_producer_ranges': True,
    }
    fallback = MOD.optional_enrichment_fallback_capture(cap)
    assert fallback['function_entry_pcs'] == []
    assert fallback['dispatch_entry_pcs'] == [f'0x{dispatch:08X}']
    assert fallback['static_discovery_entry_pcs'] == []
    assert fallback['seeds'] == []
    assert fallback['producer_ranges'] == cap[
        'optional_enrichment_fallback_entry_pcs']['producer_ranges']
    assert fallback['strict_producer_ranges'] is True


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


def check_recompiler_pointer_table_call_root(recompiler):
    data = bytearray(0x100)
    target = LOAD + 0x50
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    for index, offset in enumerate((0x70, 0x80, 0x90)):
        put(data, 0x50 + index * 4, LOAD + offset)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "pointer_table.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\ncall_root 0x{target:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob("*_full*.c"))
        assert f"void func_{target:08X}" not in full


def check_recompiler_pointer_table_alias(recompiler):
    # A function envelope may span unreachable data between a jump and its
    # target.  A JAL-shaped word in that hole must not turn a dense pointer
    # table inside the envelope into an alias function.
    data = bytearray(0x100)
    target = LOAD + 0x40
    put(data, 0x00, 0x08000000 | (((LOAD + 0x80) >> 2) & 0x03FFFFFF))
    put(data, 0x04, 0x00000000)
    put(data, 0x20, jal(target))
    for index, offset in enumerate((0x70, 0x80, 0x90)):
        put(data, 0x40 + index * 4, LOAD + offset)
    put(data, 0x80, 0x03E00008)
    put(data, 0x84, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "pointer_table_alias.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob("*_full*.c"))
        assert "+1 in-function jal-target alias entries" not in result.stdout
        assert f"void func_{target:08X}" not in full


def check_retained_alias_contract(recompiler):
    # Legacy host ranges are ignored. A prior alias can be reintroduced only as
    # an entry candidate and must acquire a host from current exact reachability.
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
        assert f"void func_{LOAD + 0x20:08X}" not in full
        ranges = next(pathlib.Path(out).glob("*_full.ranges")).read_text()
        assert f"F {LOAD + 0x20:08X}" not in ranges


def check_atomic_dll_publication():
    original = MOD._compile_dll_direct
    original_replace = MOD.os.replace
    try:
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "shard.dll")
            ranges = os.path.join(tmp, "shard.ranges")
            with open(final, "wb") as out:
                out.write(b"old-good-shard")
            with open(ranges, "w", encoding="utf-8") as out:
                out.write("OLD-RANGES\n")

            def fail_compile(_src, staged, _includes, **_kwargs):
                with open(staged, "wb") as out:
                    out.write(b"partial")
                with open(os.path.splitext(staged)[0] + ".def", "w") as out:
                    out.write("temporary exports")
                return False

            MOD._compile_dll_direct = fail_compile
            assert not MOD.compile_dll("ignored.c", final, [])
            with open(final, "rb") as built:
                assert built.read() == b"old-good-shard"
            assert pathlib.Path(ranges).read_text() == "OLD-RANGES\n"
            assert not list(pathlib.Path(tmp).glob(".*.tmp.*"))
            assert MOD.compiled_shard_complete(final)

            def good_compile(_src, staged, _includes, **_kwargs):
                with open(staged, "wb") as out:
                    out.write(b"new-complete-shard")
                with open(os.path.splitext(staged)[0] + ".def", "w") as out:
                    out.write("temporary exports")
                return True

            MOD._compile_dll_direct = good_compile
            func_ids = [(0x80010000, 0xDEADBEEF,
                         ((0x80010000, 0x10),))]
            pair_id = MOD.overlay_pair_id("new source", func_ids)
            assert pair_id != MOD.overlay_pair_id("changed source", func_ids)
            bound_source = MOD.add_overlay_pair_export("new source", pair_id)
            assert f"overlay_pair_id(void) {{ return UINT64_C(0x{pair_id:016X}); }}" in bound_source
            assert MOD.compile_dll("ignored.c", final, [],
                                   func_ids=func_ids, pair_id=pair_id)
            with open(final, "rb") as built:
                assert built.read() == b"new-complete-shard"
            manifest = pathlib.Path(ranges).read_text()
            assert f"P {pair_id:016X}\n" in manifest
            assert "F 80010000 DEADBEEF\nR 80010000 10\n" in manifest
            assert MOD.compiled_shard_complete(final)
            assert not list(pathlib.Path(tmp).glob(".*.tmp.*"))

        # Kill a separate writer process AFTER each durable rename. This tests
        # real kernel lock release and recovery, including the hardest point:
        # the final DLL rename committed but journal cleanup never ran.
        module_path = str(ROOT / "tools" / "compile_overlays.py")
        crash_script = r'''
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("co_child", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
real = m.os.replace
count = 0
def crash_after(src, dst):
    global count
    result = real(src, dst)
    count += 1
    if count == int(sys.argv[5]): os._exit(91)
    return result
m.os.replace = crash_after
m.publish_shard_pair(sys.argv[2], sys.argv[3], sys.argv[4])
'''
        for fail_after in range(1, 6):
            with tempfile.TemporaryDirectory() as tmp:
                final = os.path.join(tmp, "shard.dll")
                ranges = os.path.join(tmp, "shard.ranges")
                staged = os.path.join(tmp, ".new.dll")
                staged_ranges = os.path.join(tmp, ".new.ranges")
                pathlib.Path(final).write_bytes(b"OLD-DLL")
                pathlib.Path(ranges).write_text("OLD-RANGES\n")
                pathlib.Path(staged).write_bytes(b"NEW-DLL")
                pathlib.Path(staged_ranges).write_text("NEW-RANGES\n")
                child = subprocess.run(
                    [sys.executable, "-c", crash_script, module_path,
                     staged, staged_ranges, final, str(fail_after)])
                assert child.returncode == 91

                MOD.recover_shard_pair(final)
                expected = ((b"NEW-DLL", "NEW-RANGES\n") if fail_after == 5
                            else (b"OLD-DLL", "OLD-RANGES\n"))
                assert pathlib.Path(final).read_bytes() == expected[0]
                assert pathlib.Path(ranges).read_text() == expected[1]
                assert not list(pathlib.Path(tmp).glob("*.pair-txn.json"))

        # Recovery is restartable if an individual rollback rename faults.
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "shard.dll")
            ranges = os.path.join(tmp, "shard.ranges")
            staged = os.path.join(tmp, ".new.dll")
            staged_ranges = os.path.join(tmp, ".new.ranges")
            pathlib.Path(final).write_bytes(b"OLD-DLL")
            pathlib.Path(ranges).write_text("OLD-RANGES\n")
            pathlib.Path(staged).write_bytes(b"NEW-DLL")
            pathlib.Path(staged_ranges).write_text("NEW-RANGES\n")
            child = subprocess.run(
                [sys.executable, "-c", crash_script, module_path,
                 staged, staged_ranges, final, "3"])
            assert child.returncode == 91
            real_replace = MOD.os.replace
            failed_once = False

            def fail_one_restore(src, dst):
                nonlocal failed_once
                if not failed_once and str(src).endswith('.old-ranges'):
                    failed_once = True
                    raise OSError("injected recovery rename fault")
                return real_replace(src, dst)

            MOD.os.replace = fail_one_restore
            try:
                try:
                    MOD.recover_shard_pair(final)
                    raise AssertionError("recovery fault was not surfaced")
                except OSError as exc:
                    assert "injected recovery" in str(exc)
            finally:
                MOD.os.replace = real_replace
            MOD.recover_shard_pair(final)
            assert pathlib.Path(final).read_bytes() == b"OLD-DLL"
            assert pathlib.Path(ranges).read_text() == "OLD-RANGES\n"

        # Real cross-process writers must serialize on the permanent OS lock.
        publish_script = r'''
import importlib.util, sys
spec = importlib.util.spec_from_file_location("co_child", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.publish_shard_pair(sys.argv[2], sys.argv[3], sys.argv[4])
'''
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "shard.dll")
            ranges = os.path.join(tmp, "shard.ranges")
            pathlib.Path(final).write_bytes(b"DLL-old")
            pathlib.Path(ranges).write_text("RANGES-old")
            writers = []
            for index in range(8):
                staged = os.path.join(tmp, f"new-{index}.dll")
                staged_ranges = os.path.join(tmp, f"new-{index}.ranges")
                pathlib.Path(staged).write_bytes(f"DLL-{index}".encode())
                pathlib.Path(staged_ranges).write_text(f"RANGES-{index}")
                writers.append(subprocess.Popen(
                    [sys.executable, "-c", publish_script, module_path,
                     staged, staged_ranges, final]))
            assert all(writer.wait(timeout=30) == 0 for writer in writers)
            dll_generation = pathlib.Path(final).read_text().removeprefix("DLL-")
            range_generation = pathlib.Path(ranges).read_text().removeprefix("RANGES-")
            assert dll_generation == range_generation
            assert not list(pathlib.Path(tmp).glob("*.pair-txn.json"))

        # Readers must not observe the deliberate manifest-only publication
        # window. Pause a real publisher after rename #4 while it owns the OS
        # lock; completeness and coverage readers must block, then consume the
        # committed pair rather than the transient half-state.
        pause_script = r'''
import importlib.util, os, pathlib, sys, time
spec = importlib.util.spec_from_file_location("co_child", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
real = m.os.replace
count = 0
def pause_after_manifest(src, dst):
    global count
    result = real(src, dst); count += 1
    if count == 4:
        pathlib.Path(sys.argv[5]).write_text("paused")
        time.sleep(1.0)
    return result
m.os.replace = pause_after_manifest
m.publish_shard_pair(sys.argv[2], sys.argv[3], sys.argv[4])
'''
        for reader in ('complete', 'coverage'):
            with tempfile.TemporaryDirectory() as tmp:
                final = os.path.join(tmp, "00010000_DEADBEEF.dll")
                ranges = os.path.splitext(final)[0] + '.ranges'
                staged = os.path.join(tmp, ".new.dll")
                staged_ranges = os.path.join(tmp, ".new.ranges")
                paused = os.path.join(tmp, "paused")
                pathlib.Path(final).write_bytes(b"OLD-DLL")
                pathlib.Path(ranges).write_text(
                    "F 80010004 AAAAAAAA\nR 80010004 4\n")
                pathlib.Path(staged).write_bytes(b"NEW-DLL")
                pathlib.Path(staged_ranges).write_text(
                    "F 80010000 DEADBEEF\nR 80010000 10\n")
                writer = subprocess.Popen(
                    [sys.executable, "-c", pause_script, module_path,
                     staged, staged_ranges, final, paused])
                deadline = time.monotonic() + 10
                while not os.path.exists(paused) and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert os.path.exists(paused), "publisher did not reach pause point"
                started = time.monotonic()
                if reader == 'complete':
                    assert MOD.compiled_shard_complete(final)
                else:
                    assert MOD.load_region_coverage(tmp, 0x10000) == {
                        (0x80010000, 0xDEADBEEF)}
                elapsed = time.monotonic() - started
                assert elapsed >= 0.6, f'{reader} escaped pair lock in {elapsed:.3f}s'
                assert writer.wait(timeout=10) == 0

        if os.name == 'nt':
            # Windows keeps a loaded image mapped even after its canonical name
            # is renamed. Old-backup deletion may therefore return WinError 5;
            # a committed new pair must survive and cleanup must be deferred.
            import _ctypes
            import ctypes
            import shutil
            gcc = r'C:\msys64\mingw64\bin\gcc.exe'
            assert os.path.isfile(gcc), "real loaded-DLL regression needs MinGW gcc"
            with tempfile.TemporaryDirectory() as tmp:
                # The actual host compiler must export the same 64-bit identity
                # serialized in P; this catches width/decorated-name mistakes
                # that a source-string assertion cannot.
                pair_id = 0xFEDCBA9876543210
                pair_source = pathlib.Path(tmp) / "pair.c"
                pair_dll = str(pathlib.Path(tmp) / "pair.dll")
                pair_source.write_text(
                    '#include <stdint.h>\n' +
                    '__declspec(dllexport) int overlay_abi(void) { return 14; }\n' +
                    MOD.add_overlay_pair_export('', pair_id))
                subprocess.run([gcc, '-shared', str(pair_source), '-o', pair_dll],
                               check=True, capture_output=True, text=True)
                pathlib.Path(pair_dll).with_suffix('.ranges').write_text(
                    MOD.overlay_ranges_text([], pair_id))
                pair_lib = ctypes.WinDLL(pair_dll)
                pair_fn = pair_lib.overlay_pair_id
                pair_fn.restype = ctypes.c_uint64
                assert pair_fn() == pair_id
                manifest = MOD.overlay_ranges_text([], pair_id)
                assert f'P {pair_fn():016X}\n' in manifest
                pair_handle = pair_lib._handle
                pair_lib._handle = 0
                _ctypes.FreeLibrary(pair_handle)
                assert MOD.compiled_shard_complete(pair_dll, 14)
                assert not MOD.compiled_shard_complete(
                    pair_dll, 14 | (1 << 16))  # same cg dir, stale flavor
                assert MOD.cached_shard_manifest_status(
                    pair_dll, 14, manifest, pair_id) == 'match'
                assert MOD.cached_shard_manifest_status(
                    pair_dll, 14, manifest, pair_id ^ 1) == 'mismatch'

                source = pathlib.Path(tmp) / "loaded.c"
                final = str(pathlib.Path(tmp) / "shard.dll")
                ranges = str(pathlib.Path(tmp) / "shard.ranges")
                staged = str(pathlib.Path(tmp) / ".new.dll")
                staged_ranges = str(pathlib.Path(tmp) / ".new.ranges")
                source.write_text("__declspec(dllexport) int loaded(void) { return 7; }\n")
                subprocess.run([gcc, '-shared', str(source), '-o', final],
                               check=True, capture_output=True, text=True)
                pathlib.Path(ranges).write_text("OLD-RANGES\n")
                shutil.copyfile(final, staged)
                pathlib.Path(staged_ranges).write_text("NEW-RANGES\n")
                loaded = ctypes.WinDLL(final)
                MOD.publish_shard_pair(staged, staged_ranges, final)
                assert pathlib.Path(ranges).read_text() == "NEW-RANGES\n"
                assert not os.path.exists(final + '.pair-txn.json')
                deferred = list(pathlib.Path(tmp).glob(
                    'shard.dll.pair-txn.*.old-dll'))
                assert deferred, "loaded old DLL backup should require deferred cleanup"
                handle = loaded._handle
                loaded._handle = 0
                _ctypes.FreeLibrary(handle)
                MOD.recover_shard_pair(final)
                assert not [path for path in deferred if path.exists()]

        with tempfile.TemporaryDirectory() as tmp:
            ranges = pathlib.Path(tmp) / "00010000_DEADBEEF.ranges"
            ranges.write_text("F 80010000 DEADBEEF\nR 80010000 10\n")
            assert MOD.load_region_coverage(tmp, 0x10000) == set()
            ranges.with_suffix(MOD.overlay_ext()).write_bytes(b"DLL")
            assert MOD.load_region_coverage(tmp, 0x10000) == {
                (0x80010000, 0xDEADBEEF)}
    finally:
        MOD.os.replace = original_replace
        MOD._compile_dll_direct = original


def check_interior_fragment_contract():
    entry = LOAD + 0x20
    good = [(entry, 0x12345678, [(entry, 8)])]
    assert MOD.validate_interior_fragment_ids(entry, good, {entry}) is None
    assert "missing from generated C" in MOD.validate_interior_fragment_ids(
        entry, good, set())
    assert "0 manifest identities" in MOD.validate_interior_fragment_ids(
        entry, [], {entry})
    assert "2 manifest identities" in MOD.validate_interior_fragment_ids(
        entry, good + good, {entry})
    assert "0 ranges" in MOD.validate_interior_fragment_ids(
        entry, [(entry, 0, [])], {entry})
    assert "17 ranges" in MOD.validate_interior_fragment_ids(
        entry, [(entry, 0, [(entry + i * 4, 4) for i in range(17)])],
        {entry})
    assert "outside its guarded ranges" in MOD.validate_interior_fragment_ids(
        entry, [(entry, 0, [(entry + 4, 4)])], {entry})
    assert "fragment entry" in MOD.validate_interior_fragment_ids(
        entry, good + [(entry + 0x40, 0,
                        [(entry + 0x40 + i * 4, 4) for i in range(17)])],
        {entry, entry + 0x40})
    assert "empty guarded range" in MOD.validate_interior_fragment_ids(
        entry, good + [(entry + 0x40, 0, [(entry + 0x40, 0)])],
        {entry, entry + 0x40})
    assert MOD.interior_failure_is_deterministic(
        "generated-c-audit: unsupported")
    assert MOD.interior_failure_is_deterministic(
        "requested-entry-audit: omitted")
    assert not MOD.interior_failure_is_deterministic(
        "fragment cache-key collision/stale pair")

    audit = {
        "included_reasons": {},
        "executed_pcs": set(),
        "producer_ranges": [(LOAD, LOAD + 0x100)],
        "accepted_cross_producer_calls": {LOAD + 0x200},
    }
    data = bytes(0x100)
    assert MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit, set()) is None
    job = MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit, {entry})
    assert job is not None
    assert job["candidates"] == {entry}
    assert job["forced"] == {entry}
    assert job["executed"] == set()
    assert job["producer_ranges"] == ((LOAD, LOAD + 0x100),)
    assert job["cross_call_allow"] == (LOAD + 0x200,)
    assert MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit,
        {LOAD + 0x200}) is None

    with tempfile.TemporaryDirectory() as td:
        dll = pathlib.Path(td) / "fragment.dll"
        ranges = pathlib.Path(td) / "fragment.ranges"
        dll.write_bytes(b"DLL")
        ranges.write_text("EXPECTED\n")
        assert MOD.cached_shard_manifest_status(
            str(dll), None, "EXPECTED\n") == "match"
        assert MOD.cached_shard_manifest_status(
            str(dll), None, "OTHER\n") == "mismatch"
        ranges.unlink()
        assert MOD.cached_shard_manifest_status(
            str(dll), None, "EXPECTED\n") == "missing"

    old_run = MOD.subprocess.run
    seen_seeds = []

    class Args:
        recompiler = "recompiler"
        game_toml = "game.toml"

    class Failed:
        returncode = 1
        stdout = ""
        stderr = "expected stop"

    def stop_after_seeds(cmd, **_kwargs):
        seeds = pathlib.Path(cmd[cmd.index("--seeds") + 1])
        seen_seeds.extend(seeds.read_text().splitlines())
        return Failed()

    try:
        MOD.subprocess.run = stop_after_seeds
        ids, status = MOD.compile_interior_fragment(
            entry, data, LOAD, len(data), LOAD & 0x1FFFFFFF, "unused",
            Args(), {}, {},
            ((LOAD, LOAD + 0x40), (LOAD + 0x80, LOAD + 0x100)),
            (LOAD + 0x200, LOAD + 0x180, LOAD + 0x200))
        assert ids is None and status.startswith("recompiler-error")
    finally:
        MOD.subprocess.run = old_run
    assert seen_seeds == [
        f"producer_range 0x{LOAD:08X} 0x{LOAD + 0x40:08X}",
        f"producer_range 0x{LOAD + 0x80:08X} 0x{LOAD + 0x100:08X}",
        f"cross_call_allow 0x{LOAD + 0x180:08X}",
        f"cross_call_allow 0x{LOAD + 0x200:08X}",
        f"dispatch_root 0x{entry:08X}",
    ]


def check_tcc_runtime_define_parity():
    """TCC shards must use the same release/cycle contract as GCC shards."""
    seen = []
    old_run = MOD.subprocess.run
    old_inc = MOD._bom_free_incdir

    class Result:
        returncode = 0
        stdout = ''
        stderr = ''

    def fake_run(cmd, **_kwargs):
        seen.extend(cmd)
        return Result()

    try:
        MOD.subprocess.run = fake_run
        MOD._bom_free_incdir = lambda path: path
        with tempfile.TemporaryDirectory() as td:
            source = pathlib.Path(td) / 'overlay.c'
            source.write_text('int overlay_test(void) { return 0; }\n')
            assert MOD._compile_dll_tcc(
                str(source), str(pathlib.Path(td) / 'overlay.dll'),
                [td], 3, 'tcc')
            assert b'psx_tcc_ctz' in source.read_bytes()
    finally:
        MOD.subprocess.run = old_run
        MOD._bom_free_incdir = old_inc

    assert '-DPSX_NO_DEBUG_TOOLS' in seen
    assert '-DPSX_ENABLE_BLOCK_CYCLES=1' in seen
    assert '-DPSX_OVERLAY_FLAVOR=3' in seen


def main():
    default_recompiler = ROOT / "recompiler" / "build" / "psxrecomp-game.exe"
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=str(default_recompiler))
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    check_composite_call_boundaries()
    check_bounded_jump_table_discovery()
    check_static_discovery_provenance()
    check_owned_direct_call_is_interior()
    check_discovered_host_owns_same_round_call_target()
    check_later_discovered_host_retracts_interior_root()
    check_recompiler_discovered_host_ownership(args.recompiler)
    check_ownership_invalidation_and_producer_boundaries()
    check_recompiler_unreachable_jal_not_alias(args.recompiler)
    check_recomputed_evidence_and_unconditional_branch()
    check_t2_shaped_retained_partition_conflict(args.recompiler)
    check_static_alias_recipe()
    check_optional_enrichment_fallback()
    check_forward_branch_root()
    check_recompiler_composite_contract(args.recompiler)
    check_recompiler_pointer_table_call_root(args.recompiler)
    check_recompiler_pointer_table_alias(args.recompiler)
    check_retained_alias_contract(args.recompiler)
    check_atomic_dll_publication()
    check_interior_fragment_contract()
    check_tcc_runtime_define_parity()

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
