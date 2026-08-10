#!/usr/bin/env python3
"""Structural regression checks for interpreter fallback hot-path guards."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def body(source, name):
    match = re.search(rf"\b(?:static\s+)?(?:inline\s+)?(?:int|void|uint8_t|uint16_t|uint32_t|uint64_t|DWORD|OverlayPreparedImage|PublishItem)\s*\*?\s*(?:WINAPI\s+)?{name}\s*\([^;]*?\)\s*\{{",
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
    oracle_interp = (ROOT / "runtime/src/psx_interpreter.c").read_text(encoding="utf-8")
    capture = (ROOT / "runtime/src/overlay_capture.c").read_text(encoding="utf-8")
    loader = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    autocompile = (ROOT / "runtime/src/autocompile.c").read_text(encoding="utf-8")
    memory = (ROOT / "runtime/src/memory.c").read_text(encoding="utf-8")
    gte = (ROOT / "runtime/src/gte.cpp").read_text(encoding="utf-8")
    traps = (ROOT / "runtime/src/traps.c").read_text(encoding="utf-8")
    interrupts = (ROOT / "runtime/src/interrupts.c").read_text(encoding="utf-8")
    cycles = (ROOT / "runtime/src/psx_cycles.c").read_text(encoding="utf-8")
    cyc_header = (ROOT / "runtime/include/psx_cyc.h").read_text(encoding="utf-8")
    instr_cost = (ROOT / "runtime/include/psx_instr_cost.h").read_text(encoding="utf-8")
    starvation = (ROOT / "runtime/include/starvation_ring.h").read_text(encoding="utf-8")

    record = body(interp, "exec_pc_table_record")
    if "uint32_t *slot = &g_dirty_ram_exec_pc_bitmap[word >> 5]" not in record:
        raise AssertionError("executed-PC recording is not a direct bitmap OR")
    if "pc_table_get_or_insert" in record:
        raise AssertionError("executed-PC recording regressed to hash probing")
    if "sizeof(g_dirty_ram_exec_pc_bitmap)" not in capture:
        raise AssertionError("autocapture does not snapshot the 64 KiB bitmap")
    write_window = body(capture, "write_json_window")
    if ("size += 4u" not in write_window or
            "phys + size <= ram_size - 4u" not in write_window or
            "window_hi" in write_window):
        raise AssertionError(
            "capture delay-slot guard is missing or stops at an artificial window boundary")
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
    for guard in ("first_page - 1u", "last_page + 1u", "exec_pc_bitmap"):
        if guard not in capture:
            raise AssertionError(f"pre-DMA capture lost executed-page halo: {guard}")
    autocap_tick = body(capture, "overlay_autocapture_tick")
    failed_write = autocap_tick.find("if (!sig && job)")
    clear_owner = autocap_tick.find("s_autocap_write_job = NULL", failed_write)
    retry_pending = autocap_tick.find("if (s_autocap_write_job)", clear_owner)
    enabled_gate = autocap_tick.find("if (!s_autocap_enabled || !s_active)")
    if min(failed_write, clear_owner, retry_pending, enabled_gate) < 0 or not (
            failed_write < clear_owner < retry_pending < enabled_gate):
        raise AssertionError("failed periodic capture is not retained and retried before new epochs")
    failed_branch = autocap_tick[failed_write:clear_owner]
    for required in ("job->attempts++", "job->retry_frame", "return;"):
        if required not in failed_branch:
            raise AssertionError("periodic capture failure can discard its immutable evidence epoch")
    retry_branch = autocap_tick[retry_pending:enabled_gate]
    if ("SDL_AtomicGet(&s_autocap_write_state) == 0" not in retry_branch or
            "autocap_write_launch(job)" not in retry_branch or
            "autocap_write_job_free" in retry_branch or "memset(" in retry_branch):
        raise AssertionError("periodic capture retry mutates or frees newer live evidence")
    provider_try = body(capture, "autocap_provider_request_try")
    accepted = provider_try.find("if (cp->request && cp->request())")
    accepted_sig = provider_try.find("s_autocap_sig_at_req =", accepted)
    retry_sig = provider_try.find("s_autocap_provider_sig_pending = 0", accepted_sig)
    retry_delay = provider_try.find("s_autocap_provider_retry_frame = frame +", retry_sig)
    if min(accepted, accepted_sig, retry_sig, retry_delay) < 0 or not (
            accepted < accepted_sig < retry_sig < retry_delay):
        raise AssertionError("compiler spawn failure can mark durable evidence as requested")
    if "s_autocap_provider_sig_pending = sig" not in autocap_tick:
        raise AssertionError("durable capture is not retained until provider request acceptance")
    wait_pending = body(capture, "overlay_capture_wait_pending")
    if "preserve_snapshot_enqueue_owned(job)" not in wait_pending:
        raise AssertionError("shutdown drops a failed periodic capture instead of draining it")
    dispatch_inner = body(interp, "dirty_ram_dispatch_inner")
    if "(!current_page_dirty || next_page != current_page)" not in dispatch_inner:
        raise AssertionError("page fast path does not preserve clean-miss behavior")
    exec_one = body(interp, "exec_one_fetched_unobserved")
    for scope_guard in (
            "pc_phys >= 0x1FC00000u && pc_phys < 0x1FC80000u",
            "pc_phys < 0x00010000u",
            "(in_bios_rom || in_bios_kernel_ram)"):
        if scope_guard not in exec_one:
            raise AssertionError(
                f"load-delay value semantics lost BIOS ownership guard: {scope_guard}")
    exec_one_observed = body(interp, "exec_one_fetched_observed")
    if "exec_one_fetched_inner(cpu, pc, insn, cold_flags, next_pc_out)" not in exec_one_observed:
        raise AssertionError(
            "observed decoder does not reuse the base decoder with preclassified hooks")
    # RFE backend-contract parity: the recompiled rfe and every overlay shard
    # (ABI v12) call psx_rfe_mark_escape after popping SR, and the generated
    # trampoline runs psx_rfe_escape_check after each return. The interpreter
    # must do BOTH — mark at its RFE case and take the escape at the committed
    # transfer — or an interpreted handler whose EPC lands in dirty RAM keeps
    # interpreting inside psx_check_interrupts' window and in_exception never
    # clears (vblank vetoed forever: the dbg-build boot livelock).
    if "psx_rfe_mark_escape();" not in interp:
        raise AssertionError("interpreter RFE does not honor the rfe escape contract")
    rfe_take = dispatch_inner.find("psx_rfe_escape_check(cpu);")
    local_flow = dispatch_inner.find("allow_local_dirty_flow && target != 0")
    if rfe_take < 0 or local_flow < 0 or rfe_take > local_flow:
        raise AssertionError(
            "interpreter does not take a pending rfe escape before local dirty flow")
    loop = dispatch_inner.find("for (int i = 0; i < MAX_INSNS_PER_DISPATCH; i++)")
    baseline_note = dispatch_inner.find(
        "native_render_baseline_note_execution_impl("
    )
    baseline_accounting = dispatch_inner.find(
        "NATIVE_RENDER_BASELINE_INTERPRETER", baseline_note
    )
    auth_hook = dispatch_inner.find(
        "psx_xg_render_auth_cold_hook(", baseline_accounting
    )
    auth_entry = dispatch_inner.find(
        "PSX_XG_RENDER_AUTH_HOOK_ENTRY", auth_hook
    )
    decoder_calls = (
        dispatch_inner.find("exec_one_fetched_observed", baseline_accounting),
        dispatch_inner.find("exec_one_fetched_unobserved", baseline_accounting),
    )
    exec_one = min((position for position in decoder_calls if position >= 0), default=-1)
    if (
        baseline_note < 0
        or baseline_accounting < 0
        or exec_one < 0
        or loop < 0
        or loop > baseline_note
        or baseline_note > baseline_accounting
        or baseline_accounting > exec_one
    ):
        raise AssertionError("interpreter does not count each PC before execution")
    if auth_hook >= 0 and (
        auth_entry < 0
        or not baseline_accounting < auth_hook < auth_entry < exec_one
    ):
            raise AssertionError(
                "interpreter does not keep authenticated lifecycle entry between accounting and execution"
            )
    if "native_render_baseline_note_execution_impl(addr," in dispatch_inner:
        raise AssertionError("interpreter still counts only the dispatch address")

    dispatch = body(loader, "overlay_loader_dispatch")
    if "native_render_baseline_note_execution(" in dispatch:
        raise AssertionError("native dispatch still counts outside true function entries")
    call_entry = body(loader, "overlay_log_call_entry")
    debug_guard = call_entry.find("#ifndef PSX_NO_DEBUG_TOOLS")
    debug_log = call_entry.find("debug_server_log_call_entry_fn(func_addr);")
    shadow_guard = call_entry.find("if (!s_in_shadow)")
    native_note = call_entry.find("native_render_baseline_note_execution(")
    if min(debug_guard, debug_log, shadow_guard, native_note) < 0 or not (
            debug_guard < debug_log < shadow_guard < native_note):
        raise AssertionError("overlay entry callback lost Debug logging or shadow exclusion")
    init = body(loader, "init_callbacks")
    callback = "s_callbacks.log_call_entry       = overlay_log_call_entry;"
    if init.count(callback) != 1:
        raise AssertionError("overlay entry callback is not assigned unconditionally")
    if re.search(r"#ifdef PSX_NO_DEBUG_TOOLS.*?log_call_entry", init, re.S):
        raise AssertionError("Release overlay callback is null")
    cached = dispatch.find("lazy_miss_cached(phys)")
    lookup = dispatch.find("idx_head(phys)")
    record_miss = dispatch.rfind("lazy_miss_record(phys)")
    if min(cached, lookup, record_miss) < 0 or not cached < lookup < record_miss:
        raise AssertionError("negative miss cache is not guarding lookup/recorded at final miss")
    if "lazy_miss_invalidate_loader();" not in body(loader, "overlay_loader_rescan"):
        raise AssertionError("cache rescan does not invalidate negative misses")
    if "lazy_miss_invalidate_loader();" not in body(loader, "load_one_dll_prepared"):
        raise AssertionError("DLL publication does not invalidate negative misses")
    lazy_match = body(loader, "lazy_man_matches")
    if "man_delay_slots_hashed(&lm->fn)" not in lazy_match:
        raise AssertionError("legacy manifests can expose unhashed delay slots")
    rebuild = body(loader, "rebuild_lazy_manifest_index")
    if "overlay_watch_set_range(lo, lm->fn.len[r])" not in rebuild:
        raise AssertionError("unloaded manifest pages are not generation-watched")
    if "manifest_ok = 1" not in rebuild:
        raise AssertionError("strictly parsed manifests are not marked usable")
    if "cache_idx_has_basename" in loader:
        raise AssertionError("immutable repair artifacts are still deduped by logical name")
    scan_one = body(loader, "scan_one_cache_dir")
    for guard in ("cache_name_is_immutable(fd.cFileName)",
                  "cache_idx_has_path(full)", "e->logical_crc = crc",
                  "e->tier = (uint8_t)tier"):
        if guard not in scan_one:
            raise AssertionError(f"artifact-aware cache indexing lost: {guard}")
    has_crc = body(loader, "overlay_loader_has_cached_crc")
    for guard in ("manifest_ok", "!s_cache_idx[i].load_failed",
                  "logical_crc == crc", "lazy_man_crc(",
                  "man_delay_slots_hashed("):
        if guard not in has_crc:
            raise AssertionError(f"cached-CRC query accepts unusable artifacts: {guard}")
    try_load = body(loader, "try_load_region")
    if "lazy_is_loadable(li, region_start, phys, 0)" not in try_load:
        raise AssertionError("exact manifest entries are still gated by dirty-run base")
    if "lazy_is_loadable(li, region_start, phys, 1)" not in try_load:
        raise AssertionError("non-exact range fallback lost region narrowing")
    if "lazy_candidate_preferred" not in try_load:
        raise AssertionError("lazy artifact selection is still directory-order dependent")
    for guard in ("load_failed", "goto retry_artifact"):
        if guard not in try_load and guard not in body(loader, "lazy_is_loadable"):
            raise AssertionError(f"failed artifact can suppress a valid fallback: {guard}")
    range_lookup = body(loader, "overlay_find_by_range")
    for guard in ("s_range_candidate_generation", "range_candidate_preferred"):
        if guard not in range_lookup:
            raise AssertionError(f"loaded range selection lost tier/new-artifact ordering: {guard}")
    ac_poll = body(autocompile, "autocompile_poll_main")
    if "one idempotent batch-end" not in ac_poll or "overlay_loader_rescan();" not in ac_poll:
        raise AssertionError("direct shard handoff is not reconciled into the additive index")
    if ("overlay_loader_commit_published(published->image)" not in ac_poll or
            "overlay_loader_load_published(" in ac_poll):
        raise AssertionError("emulation thread regressed to first-mapping published DLLs")
    watcher = body(autocompile, "watch_thread")
    preparer = body(autocompile, "publish_prepare_thread_main")
    if "out_append(buf, (int)got);" not in watcher:
        raise AssertionError("compile watcher no longer drains child output")
    if ("overlay_loader_prepare_published(item->path)" in watcher or
            "overlay_loader_prepare_published(item->path)" not in preparer):
        raise AssertionError("pipe draining and DLL preparation are not separated")
    if ("s_publish_commit_active" not in preparer or
            "s_publish_commit_active++" not in body(autocompile,
                                                     "publish_ready_pop") or
            "publish_commit_finished();" not in ac_poll):
        raise AssertionError("worker mapping can overlap main-thread loader commit")
    if "AC_PUBLISH_CAP" in autocompile:
        raise AssertionError("live publication queue can silently overflow a fixed path ring")
    prepare_image = body(loader, "overlay_loader_prepare_published")
    commit_image = body(loader, "overlay_loader_commit_published")
    validated = prepare_image.find(
        "published_path_valid(dll_path, canon, sizeof(canon))")
    guard_open = prepare_image.find(
        "CreateFileA(canon, GENERIC_READ, FILE_SHARE_READ")
    first_map = prepare_image.find("LoadLibraryA(canon)")
    if min(validated, guard_open, first_map) < 0 or not (
            validated < guard_open < first_map):
        raise AssertionError(
            "Windows published image must be canonical-validated and "
            "guard-held before its worker first-map")
    if "LoadLibraryA(dll_path)" in prepare_image:
        raise AssertionError(
            "worker first-map regressed to mapping raw (non-canonical) pipe text")
    if ("load_one_dll_prepared(canon, handle)" not in commit_image or
            "overlay_library_close(handle)" not in commit_image):
        raise AssertionError("prepared image commit does not consume every speculative reference")
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
    if "cache_path_equal(s_loaded_paths[i], path)" not in body(
            loader, "dll_already_loaded"):
        raise AssertionError("published/scanned path separator aliases can double-load DLLs")
    scheduler = body(traps, "psx_scheduler_run")
    shadow_sched_fixup = scheduler.find(
        "overlay_loader_shadow_scheduler_escape_fixup();")
    scheduler_reset = scheduler.find("g_psx_dispatch_depth = 0")
    if min(shadow_sched_fixup, scheduler_reset) < 0 or shadow_sched_fixup > scheduler_reset:
        raise AssertionError("scheduler longjmp can bypass shadow cleanup")
    shadow_cleanup = body(loader, "shadow_escape_cleanup")
    for guard in ("gte_precision_speculative_end();",
                  "g_shadow_mmio_watch = s_shadow_saved_mmio_watch;",
                  "s_native_exec  = s_shadow_saved_native_exec;",
                  "s_suppress_irq = s_shadow_saved_supp;",
                  "g_exec_phase   = s_shadow_saved_exec_phase;"):
        if guard not in shadow_cleanup:
            raise AssertionError(f"shadow nonlocal cleanup lost: {guard}")
    for switch_fn, mutation in (
            ("psx_request_thread_switch", "psx_save_context_to_tcb"),
            ("psx_change_thread_fiber", "psx_bind_current_host_thread")):
        switch_body = body(traps, switch_fn)
        bail = switch_body.find(
            "overlay_loader_shadow_native_thread_switch_bail()")
        first_mutation = switch_body.find(mutation)
        if min(bail, first_mutation) < 0 or bail > first_mutation:
            raise AssertionError(
                f"{switch_fn} can mutate scheduler state before shadow bail")
    syscall = body(traps, "psx_syscall")
    for guard in ("switch_result < 0", "return 1;"):
        if guard not in syscall:
            raise AssertionError(f"syscall lost native shadow bail: {guard}")
    shadow_switch_bail = body(
        loader, "overlay_loader_shadow_native_thread_switch_bail")
    for guard in ("s_shadow_scheduler_bail = 1;", "g_psx_call_bail = 1;"):
        if guard not in shadow_switch_bail:
            raise AssertionError(f"native scheduler divergence can escape: {guard}")
    interrupt_poll = body(interrupts, "psx_check_interrupts")
    scheduler_jumps = [m.start() for m in re.finditer(
        r"longjmp\(g_scheduler_jmpbuf", interrupt_poll)]
    if len(scheduler_jumps) != 2:
        raise AssertionError("unexpected direct interrupt scheduler escape count")
    previous_jump = -1
    for jump in scheduler_jumps:
        guard = interrupt_poll.rfind(
            "overlay_loader_shadow_native_thread_switch_bail()", 0, jump)
        if guard <= previous_jump:
            raise AssertionError(
                "direct interrupt scheduler escape bypasses native shadow bail")
        previous_jump = jump
    posix_sweep = body(loader, "posix_abi_sweep_file")
    if not (posix_sweep.find("cache_name_is_immutable(file->name)") <
            posix_sweep.find("psx_overlay_posix_library_open")):
        raise AssertionError("ABI preflight can execute a legacy POSIX constructor")
    abi_sweep = body(loader, "abi_preflight_sweep")
    if not (abi_sweep.find("cache_name_is_immutable(fd.cFileName)") <
            abi_sweep.find("LoadLibraryA(full)")):
        raise AssertionError("ABI preflight can execute a legacy Windows DllMain")

    interp_step = body(interp, "interp_cyc_step")
    if "g_psx_cycle_fast_limit" not in interp_step or "next <= g_psx_cycle_fast_limit" not in interp_step:
        raise AssertionError("interpreter lost its exact pre-deadline one-cycle fast path")
    if "g_ls_replay_active" not in interp_step or "g_event_step_conservative" not in interp_step:
        raise AssertionError("interpreter cycle fast path does not guard exact diagnostic modes")
    if "g_psx_cycle_fast_limit" in cyc_header:
        raise AssertionError("runtime cycle fast path leaked into the overlay-DLL shared header")
    if "PSX_NO_DEBUG_TOOLS" not in starvation or "STARVATION_RING_ENABLED 0" not in starvation:
        raise AssertionError("production still enables the diagnostic starvation ring")
    dep_mask = body(instr_cost, "psx_cyc_dep_res_mask")
    if ("op_roles[64]" not in dep_mask or "special_roles[64]" not in dep_mask or
            "switch (op)" in dep_mask):
        raise AssertionError("load-delay dependency masks regressed to a second instruction decoder")
    gte_execute = body(gte, "gte_execute_impl")
    direct_import = gte_execute.find("gte_import_cpu_state(&gte, cpu);")
    command = gte_execute.find("gte_run_command(&gte, cmd);")
    direct_export = gte_execute.find("gte_export_cpu_state(cpu, &gte);")
    deadline = gte_execute.find("psx_gte_set(cpu, psx_gte_cmd_latency(cmd));")
    if min(direct_import, command, direct_export, deadline) < 0 or not (
            direct_import < command < direct_export < deadline):
        raise AssertionError("GTE direct marshaling or command deadline ordering regressed")
    gte_marshaling = (gte_execute + body(gte, "gte_import_cpu_state") +
                      body(gte, "gte_export_cpu_state"))
    for old_selector in ("gte_mtc2(&gte", "gte_ctc2(&gte",
                         "gte_mfc2(&gte", "gte_cfc2(&gte"):
        if old_selector in gte_marshaling:
            raise AssertionError("GTE command bridge regressed to selector dispatch loops")
    exact_runtime_calls = (
        (interp, "gte_execute_at_tier(cpu, insn & 0x1FFFFFFu, pc,",
         "dirty interpreter"),
        (oracle_interp, "gte_execute_at_tier(cpu, insn & 0x1FFFFFFu, pc,",
         "oracle interpreter"),
        (interrupts,
         "gte_execute_at(cpu, take_insn & 0x01FFFFFFu, take_pc);",
         "IRQ issue-on-take"),
    )
    for source, call, route in exact_runtime_calls:
        if call not in source:
            raise AssertionError(f"{route} GTE path lost its exact guest PC")
    for source, route in ((interp, "dirty interpreter"),
                          (oracle_interp, "oracle interpreter")):
        if "GTE_ATTRIBUTION_TIER_COLD" not in source:
            raise AssertionError(f"{route} GTE path lost cold-tier attribution")
    warm_gte = body(loader, "overlay_gte_execute_at")
    if ("GTE_ATTRIBUTION_TIER_WARM" not in warm_gte or
            "s_callbacks.gte_execute_at       = overlay_gte_execute_at;" not in loader):
        raise AssertionError("overlay GTE callback lost warm-tier attribution")
    for fn in ("psx_cycles_reset_for_boot",):
        if "g_psx_cycle_fast_limit = 0" not in body(cycles, fn):
            raise AssertionError(f"{fn} does not invalidate the inline cycle limit")
    mmio_sync = body(cycles, "psx_devices_mmio_sync")
    if ("psx_devices_service_to_now()" not in mmio_sync or
            "psx_devices_recompute_deadline()" not in mmio_sync):
        raise AssertionError("MMIO catch-up stopped servicing or recomputing deadlines")
    service = body(cycles, "psx_devices_service_to_now")
    if not (service.find("g_psx_cycle_fast_limit = 0") <
            service.find("s_in_device_service = 1") <
            service.find("psx_devices_recompute_deadline()") <
            service.find("psx_in_device_service = 0")):
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
    if "psx_cyc_base(cpu)" not in load_timing:
        raise AssertionError("runtime loads lost their inline base charge path")
    ram_fast = body(memory, "psx_cyc_main_ram_fast_addr")
    for guard in ("g_ls_mode != 0", "g_ls_replay_active", "g_ds_recording",
                  "g_ram_read_watch_active",
                  "g_dma_exec_depth > 0", "addr >= 0xC0000000u",
                  "phys >= 0x00800000u", "RAM_SIZE - width"):
        if guard not in ram_fast:
            raise AssertionError(f"main-RAM value fast path lost guard: {guard}")
    for fn, fallback in (
            ("psx_cyc_load_word_slow", "psx_read_word(addr)"),
            ("psx_cyc_load_half_slow", "psx_read_half(addr)")):
        load_body = body(memory, fn)
        timing = load_body.find("psx_cyc_load_timing")
        fallback_pos = load_body.rfind(fallback)
        if min(timing, fallback_pos) < 0 or timing > fallback_pos:
            raise AssertionError(f"{fn} does not order timing, RAM fast path, fallback")
    byte_body = body(memory, "psx_cyc_load_byte")
    timing = byte_body.find("psx_cyc_load_timing")
    fast = byte_body.find("psx_cyc_main_ram_fast_addr")
    fallback_pos = byte_body.rfind("psx_read_byte(addr)")
    if min(timing, fast, fallback_pos) < 0 or not timing < fast < fallback_pos:
        raise AssertionError("psx_cyc_load_byte does not order timing, RAM fast path, fallback")
    if "psx_cyc_readmem(cpu, addr & 0x1FFFFFFFu, 4u, 1u, 0x20u);" not in body(
            memory, "psx_cyc_lwc2_read"):
        raise AssertionError("GTE load helper lost its block-cycle read path")
    if "defined(PSX_NO_DEBUG_TOOLS) && !defined(PSX_COSIM)" not in memory:
        raise AssertionError("main-RAM value fast path leaks into diagnostic/COSIM builds")

    legacy_gte_exec = body(gte, "gte_execute")
    exact_gte_exec = body(gte, "gte_execute_at")
    if "cpu->pc" in legacy_gte_exec or "false, 0u" not in legacy_gte_exec:
        raise AssertionError("direct GTE execution infers a possibly stale CPUState PC")
    if ("true, guest_pc" not in exact_gte_exec or
            "GTE_ATTRIBUTION_TIER_STATIC" not in exact_gte_exec):
        raise AssertionError("exact GTE execution drops its supplied guest PC")
    gte_exec = body(gte, "gte_execute_impl")
    for always_on in ("gte_attribution_record_execute_in_tier(&site, tier)",
                      "s_gte_caller_ra ="):
        pos = gte_exec.find(always_on)
        begin = gte_exec.rfind("#ifndef PSX_NO_DEBUG_TOOLS", 0, pos)
        end = gte_exec.rfind("#endif", 0, pos)
        if pos < 0 or begin > end:
            raise AssertionError(f"production GTE attribution is debug-gated: {always_on}")
    for probe in ("int16_t intpl_pre_ir[4]", "gte_rtp_record(&gte, cmd)",
                  "gte_intpl_record(&gte, intpl_pre_ir, intpl_pre_fc)"):
        pos = gte_exec.find(probe)
        begin = gte_exec.rfind("#ifndef PSX_NO_DEBUG_TOOLS", 0, pos)
        end = gte_exec.find("#endif", pos)
        if pos < 0 or begin < 0 or end < 0:
            raise AssertionError(f"production GTE path still executes diagnostic probe: {probe}")

    print("PASS: capture/cache and interpreter cycle hot-path guards intact")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
