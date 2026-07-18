#!/usr/bin/env python3
"""Structural regression checks for interpreter fallback hot-path guards."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def body(source, name):
    match = re.search(rf"\b(?:static\s+)?(?:inline\s+)?(?:int|void|uint8_t|uint16_t|uint32_t|uint64_t)\s+{name}\s*\([^;]*?\)\s*\{{",
                      source, re.S)
    if not match:
        raise AssertionError(f"missing function {name}")
    start, depth = match.end(), 1
    for pos in range(start, len(source)):
        depth += source[pos] == "{"
        depth -= source[pos] == "}"
        if depth == 0:
            return source[start:pos]
    raise AssertionError(f"unterminated function {name}")


def main():
    interp = (ROOT / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    capture = (ROOT / "runtime/src/overlay_capture.c").read_text(encoding="utf-8")
    loader = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    memory = (ROOT / "runtime/src/memory.c").read_text(encoding="utf-8")
    cycles = (ROOT / "runtime/src/psx_cycles.c").read_text(encoding="utf-8")
    cyc_header = (ROOT / "runtime/include/psx_cyc.h").read_text(encoding="utf-8")
    starvation = (ROOT / "runtime/include/starvation_ring.h").read_text(encoding="utf-8")

    record = body(interp, "exec_pc_table_record")
    if "uint32_t *slot = &g_dirty_ram_exec_pc_bitmap[word >> 5]" not in record:
        raise AssertionError("executed-PC recording is not a direct bitmap OR")
    if "pc_table_get_or_insert" in record:
        raise AssertionError("executed-PC recording regressed to hash probing")
    if "sizeof(g_dirty_ram_exec_pc_bitmap)" not in capture:
        raise AssertionError("autocapture does not snapshot the 64 KiB bitmap")
    if "g_dirty_ram_exec_pc_table" in capture:
        raise AssertionError("capture still references the removed execution table")
    before_dma = body(capture, "overlay_capture_before_dma")
    commit = before_dma.find("preserve_snapshot_async(evidence_lo, evidence_hi)")
    clear_exec = before_dma.find("memset(&g_dirty_ram_exec_pc_bitmap", commit)
    clear_dispatch = before_dma.find(
        "memset(&g_dirty_ram_dispatch_pc_bitmap", commit)
    if min(commit, clear_exec, clear_dispatch) < 0 or not commit < clear_exec < clear_dispatch:
        raise AssertionError("outgoing variant is not queued before evidence reset")
    if "first_page = lo >> page_shift" not in before_dma:
        raise AssertionError("pre-DMA capture is not scoped to touched whole pages")
    dispatch_inner = body(interp, "dirty_ram_dispatch_inner")
    if "(!current_page_dirty || next_page != current_page)" not in dispatch_inner:
        raise AssertionError("page fast path does not preserve clean-miss behavior")

    dispatch = body(loader, "overlay_loader_dispatch")
    cached = dispatch.find("lazy_miss_cached(phys)")
    lookup = dispatch.find("idx_head(phys)")
    record_miss = dispatch.rfind("lazy_miss_record(phys)")
    if min(cached, lookup, record_miss) < 0 or not cached < lookup < record_miss:
        raise AssertionError("negative miss cache is not guarding lookup/recorded at final miss")
    if "lazy_miss_invalidate_loader();" not in body(loader, "overlay_loader_rescan"):
        raise AssertionError("cache rescan does not invalidate negative misses")
    if "lazy_miss_invalidate_loader();" not in body(loader, "load_one_dll"):
        raise AssertionError("DLL publication does not invalidate negative misses")
    rebuild = body(loader, "rebuild_lazy_manifest_index")
    if "overlay_watch_set_range(lo, lm->fn.len[r])" not in rebuild:
        raise AssertionError("unloaded manifest pages are not generation-watched")
    try_load = body(loader, "try_load_region")
    if "lazy_is_loadable(li, region_start, phys, 0)" not in try_load:
        raise AssertionError("exact manifest entries are still gated by dirty-run base")
    if "lazy_is_loadable(li, region_start, phys, 1)" not in try_load:
        raise AssertionError("non-exact range fallback lost region narrowing")
    watched_write = body(memory, "overlay_watch_note_write")
    if "g_dirty_ram_exec_page_bitmap" not in watched_write:
        raise AssertionError("RAM writes do not clear stale per-page capture evidence")
    if "overlay_capture_before_dma" in watched_write:
        raise AssertionError("universal RAM-write hook regressed to snapshot I/O")
    if "overlay_loader_note_code_write()" not in watched_write:
        raise AssertionError("manifested code writes do not invalidate negative misses")
    if "g_dirty_ram_code_gen++" in watched_write:
        raise AssertionError("watched writes churn the interpreter's global code epoch")
    candidate = body(loader, "overlay_loader_is_candidate")
    if "exact_entry_has(phys)" not in candidate or "idx_head" in candidate:
        raise AssertionError("candidate presence regressed from bitmap to hash probes")

    interp_step = body(interp, "interp_cyc_step")
    if "g_psx_cycle_fast_limit" not in interp_step or "next <= g_psx_cycle_fast_limit" not in interp_step:
        raise AssertionError("interpreter lost its exact pre-deadline one-cycle fast path")
    if "g_ls_replay_active" not in interp_step or "g_event_step_conservative" not in interp_step:
        raise AssertionError("interpreter cycle fast path does not guard exact diagnostic modes")
    if "g_psx_cycle_fast_limit" in cyc_header:
        raise AssertionError("runtime cycle fast path leaked into the overlay-DLL shared header")
    if "PSX_NO_DEBUG_TOOLS" not in starvation or "STARVATION_RING_ENABLED 0" not in starvation:
        raise AssertionError("production still enables the diagnostic starvation ring")
    for fn in ("psx_devices_mmio_sync", "psx_advance_cycles_exact", "psx_cycles_resync_after_restore"):
        if "g_psx_cycle_fast_limit = 0" not in body(cycles, fn):
            raise AssertionError(f"{fn} does not invalidate the inline cycle limit")
    mmio_sync = body(cycles, "psx_devices_mmio_sync")
    if not (mmio_sync.find("psx_devices_service_to_now()") <
            mmio_sync.find("s_next_service_cycle = 0") <
            mmio_sync.find("g_psx_cycle_fast_limit = 0")):
        raise AssertionError("MMIO catch-up can republish a stale inline cycle limit")
    service = body(cycles, "psx_devices_service_to_now")
    if not (service.find("g_psx_cycle_fast_limit = 0") <
            service.find("s_in_device_service = 1") <
            service.find("psx_devices_recompute_deadline()") <
            service.find("s_in_device_service = 0")):
        raise AssertionError("device-service reentrancy can observe a nonzero inline cycle limit")
    load_charge = body(memory, "psx_load_charge_cycles")
    if "next <= g_psx_cycle_fast_limit" not in load_charge or "psx_advance_cycles(cycles)" not in load_charge:
        raise AssertionError("runtime load timing lost its exact deadline fast/fallback paths")
    for guard in ("g_ls_replay_active", "g_event_step_conservative", "next >= psx_cycle_count"):
        if guard not in load_charge:
            raise AssertionError(f"runtime load timing lost guard: {guard}")
    if "!defined(PSX_COSIM) && !STARVATION_RING_ENABLED" not in memory:
        raise AssertionError("runtime load fast path leaks into COSIM/diagnostic builds")
    load_timing = body(memory, "psx_cyc_load_timing")
    if "psx_load_charge_cycles(1u)" not in load_timing or "psx_cyc_base(cpu)" in load_timing:
        raise AssertionError("runtime loads still use the out-of-line one-cycle base charge")
    ram_fast = body(memory, "psx_cyc_main_ram_fast_addr")
    for guard in ("g_ls_mode != 0", "g_ls_replay_active", "g_ds_recording",
                  "g_ram_read_watch_active",
                  "g_dma_exec_depth > 0", "addr >= 0xC0000000u",
                  "phys >= 0x00800000u", "RAM_SIZE - width"):
        if guard not in ram_fast:
            raise AssertionError(f"main-RAM value fast path lost guard: {guard}")
    for fn in ("psx_cyc_load_word", "psx_cyc_load_half", "psx_cyc_load_byte",
               "psx_cyc_lwc2_read"):
        load_body = body(memory, fn)
        timing = load_body.find("psx_cyc_")
        fast = load_body.find("psx_cyc_main_ram_fast_addr")
        fallback = load_body.rfind("psx_read_")
        if min(timing, fast, fallback) < 0 or not timing < fast < fallback:
            raise AssertionError(f"{fn} does not order timing, RAM fast path, fallback")
    if "defined(PSX_NO_DEBUG_TOOLS) && !defined(PSX_COSIM)" not in memory:
        raise AssertionError("main-RAM value fast path leaks into diagnostic/COSIM builds")

    print("PASS: capture/cache and interpreter cycle hot-path guards intact")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
