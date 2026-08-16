#!/usr/bin/env python3
"""Structural guards for opt-in Release performance telemetry."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(source, needle, message):
    if needle not in source:
        raise AssertionError(message)


def main():
    main_cpp = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
    interp = (ROOT / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    memory = (ROOT / "runtime/src/memory.c").read_text(encoding="utf-8")
    debug_server = (ROOT / "runtime/src/debug_server.c").read_text(encoding="utf-8")
    field_visual = (ROOT.parent / "tools/native_render_field490_visual.py").read_text(
        encoding="utf-8")

    require(main_cpp, 'std::getenv("PSX_RUNTIME_PERF_DIAG")',
            "performance diagnostics lost their opt-in gate")
    require(main_cpp, 'std::getenv("PSX_RUNTIME_PERF_DIAG_MS")',
            "diagnostic cadence is no longer configurable")
    require(main_cpp, "if (requested < 250) requested = 250;",
            "diagnostic cadence lost its lower bound")
    require(main_cpp, 'std::getenv("PSX_BENCH_WINDOW")',
            "exact benchmark window gate is missing")
    require(main_cpp,
            "s_frame_count == g_runtime_perf.bench_start_frame",
            "benchmark start is not an exact guest-frame boundary")
    require(main_cpp,
            "s_frame_count != g_runtime_perf.bench_end_frame",
            "benchmark end is not an exact guest-frame boundary")
    require(main_cpp, '"[BENCH] window=%llu:%llu',
            "single parseable benchmark summary is missing")
    require(main_cpp, '"provider_poll_ms=%.3f "',
            "benchmark summary omits provider/compiler pickup time")
    require(main_cpp, "g_runtime_perf.bench_reported = true;",
            "benchmark summary lacks its one-shot guard")
    require(debug_server, 'enabled = e && strcmp(e, "1") == 0;',
            "forensic display-ring readbacks are no longer strict opt-in")
    require(debug_server,
            "const size_t event_start = pos;",
            "gl_present_ring lost its per-event truncation boundary")
    require(debug_server,
            "(size_t)written >= bufsz - event_start",
            "gl_present_ring no longer checks snprintf's required length")
    require(debug_server,
            "buf[pos++] = ']';\n    buf[pos++] = '}';",
            "gl_present_ring no longer closes truncated JSON in bounds")
    require(debug_server, '"\\"rejected_incomplete_match_frames\\":%llu,"',
            "native midpoint diagnostics omit complete-match rejection counts")
    require(debug_server, '"\\"candidate_view_unseeded\\":%llu,"',
            "native midpoint diagnostics omit the final Native-view phase gate")
    require(debug_server, '"\\"workload_epoch\\":%llu,"',
            "native midpoint diagnostics omit workload reset epochs")
    require(debug_server, '"\\"last_seal\\":{',
            "native midpoint diagnostics mix live and sealed workload state")
    require(debug_server, '"native_stream_diag"',
            "native stream provenance diagnostics are not registered")
    require(field_visual, 'environment["PSX_DISPLAY_RING"] = "1"',
            "exact-frame visual capture does not enable the display ring")

    begin = main_cpp.index("runtime_perf_frame_begin();")
    diag = main_cpp.index("runtime_perf_diag_tick();", begin)
    capture = main_cpp.index("overlay_autocapture_tick();", diag)
    provider = main_cpp.index("if (cp->poll_main) cp->poll_main();", capture)
    provider_begin = main_cpp.rfind(
        "uint64_t perf_start = runtime_perf_section_begin();", capture, provider)
    provider_end = main_cpp.index(
        "&g_runtime_perf.provider_poll_ticks);", provider)
    stable_pacer = main_cpp.index(
        "frame_pacer_wait_stable(&s_frame_pacer, g_frame_period_ms);", provider)
    recovery_pacer = main_cpp.index(
        "frame_pacer_wait(&s_frame_pacer, g_frame_period_ms);", stable_pacer)
    if not (begin < diag < capture < provider_begin < provider <
            provider_end < stable_pacer < recovery_pacer):
        raise AssertionError("vblank telemetry boundaries are out of order")
    require(main_cpp,
            "s.provider_poll_ticks = g_runtime_perf.provider_poll_ticks;",
            "provider pickup time is not included in snapshots")
    require(main_cpp,
            "end.provider_poll_ticks - start.provider_poll_ticks",
            "benchmark summary does not delta provider pickup time")
    require(main_cpp, "g_dirty_ram_insns_run;",
            "benchmark snapshots lost interpreted-instruction totals")
    require(main_cpp, "g_dirty_window_dispatches;",
            "benchmark snapshots lost dirty-dispatch totals")
    require(main_cpp, "overlay_loader_get_counters(",
            "benchmark snapshots lost existing overlay counters")
    require(main_cpp, "overlay_autocapture_get_status(",
            "benchmark snapshots lost existing capture counters")

    forbidden = (
        "runtime_perf_section_begin",
        "runtime_perf_frame_begin",
        "PSX_BENCH_WINDOW",
        "PSX_RUNTIME_PERF_DIAG_MS",
    )
    for needle in forbidden:
        if needle in interp or needle in memory:
            raise AssertionError(
                f"telemetry regressed into a per-instruction hot path: {needle}")

    print("PASS: runtime perf telemetry remains opt-in and frame-scoped")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, ValueError) as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
