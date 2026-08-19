# TCP Debug Server Commands

Protocol: **JSON over newline**, one object per line, responses on same connection.

- Request shape: `{"id": N, "cmd": "<command>", ...params}`
- Success: `{"id": N, "ok": true, ...data}`
- Failure: `{"id": N, "ok": false, "error": "<msg>"}`

There are **two live** servers, both implementing this protocol with overlapping command sets:

| Server | Port | Source |
|---|---|---|
| **Native** (our recompiled runtime) | `4370` | `runtime/src/debug_server.c` |
| **Beetle PSX** (oracle) | `4380` | `runtime/src/beetle_debug_server.c` |

> **DuckStation (port 4371) was retired as the oracle on 2026-05-05** and is no
> longer built from this repository — there is no `duckstation` entry in
> `.gitmodules`. The **D** column in the curated inventory below is historical
> and reflects DuckStation, not Beetle. For authoritative per-command
> native/Beetle coverage, use the generated
> [Complete command index](#complete-command-index-generated) at the bottom of
> this file, which is derived from the two servers' command tables.

The `debug_client.py` CLI can target either, or `compare` two at once to diff state live — that's how divergence hunts work.

```bash
python tools/debug_client.py <cmd> [args]           # native (port 4370)
python tools/debug_client.py --port 4380 <cmd>      # psx-beetle
python tools/debug_client.py --ds <cmd> [args]      # duckstation (port 4371)
python tools/debug_client.py compare <cmd>          # run on both, diff results
```

Commands without a bespoke CLI mapping pass through generically: extra
args of the form `key=value` become JSON fields (ints when numeric, else
strings), so every server command is reachable, e.g.
`debug_client.py --port 4370 gpu_frame_dump frame=14528 count=65536`.

---

## Command inventory

Columns: **N** = native, **D** = DuckStation oracle.

| Command | N | D | Params | Description |
|---|---|---|---|---|
| `ping` | ✓ | ✓ | — | Heartbeat. Native answers immediately from the I/O thread as `{"id":0,"ok":true,"pong":true,"io_thread":true}`; it does not include the current frame |
| `frame` | ✓ | ✓ | — | Current frame number |
| `get_registers` (`regs`) | ✓ | ✓ | — | All 32 GPRs + PC + HI + LO (native also: COP0 SR/Cause/EPC, I_STAT, I_MASK) |
| `read_ram` | ✓ | ✓ | `addr`, `len` | Read bytes from PS1 address space as hex string — up to the full 2 MB in ONE response line. `dump_ram` is an alias (the old chunked multi-line variant is gone: it broke the one-request/one-response protocol and wedged the server) |
| `write_ram` | ✓ | ✓ | `addr`, `hex` | Write an even-length hex string (up to 0x1000 bytes; the request-line limit makes about 4 KB practical). Legacy `addr` + single-byte `val` remains accepted |
| `call_func` | ✓ |   | `addr`, `a0`..`a3` | Invoke a guest function synchronously through `psx_dispatch_call`; returns `v0` and can block the emulation poll while the callee runs |
| `read_scratch` |   | ✓ | `addr`, `len` | Read PS1 scratchpad (0x1F800000 region) |
| `read_vram` / `vram_peek` | ✓¹ | ✓ | `x`, `y`, `w`, `h` | Read 16-bit VRAM pixels (max 128×128) |
| `gpu_state` | ✓ | ✓ | — | Display area, display depth, draw offset, GPUSTAT, clip rect, xfer state |
| `screenshot_hires` |   | ✓ | `path` | PNG of the **supersampled** surface (the present path the window uses), at `display × gr_scale()`. ⚠ `screenshot`/`screenshot_file` capture native 15-bit VRAM and are **blind to anything that only exists in the hi-res mirror** — geometry correction, SSAA edges, perspective UVs — so they show a clean frame while the player sees a broken one. Use this one to verify those. Falls back to the native resolve (and reports `scale: 1`) when no hi-res surface exists |
| `geom_correction` |   | ✓ | — | `[video] geometry_correction` / `perspective_texturing` engagement: enable flag plus free-running `geometry_vertex_hits` and `perspective_triangles` totals. Both enhancements silently fall back to the faithful path on anything they cannot prove is projected geometry, so a zero counter with the flag on means the title never qualifies — sample twice and diff for a rate |
| `sio_state` | ✓ | ✓ | — | SIO registers + (native only) pad/memcard protocol + TX/RX history |
| `irq_state` | ✓ | ✓ | — | `I_STAT`, `I_MASK` (both), plus chain state on native |
| `dma_state` | ✓ | ✓ | — | DPCR, DICR, all 7 channel states (madr/bcr/chcr) |
| `event_state` |   | ✓ | — | EvCB table summary (stub on DS — events are BIOS-level) |
| `overlay_state` | ✓ | ✓ | — | Native reports the Ctrl+F3 developer-overlay visibility flag; Release always reports `visible:false`. The historical DuckStation command reports current overlay info |
| `sljit_async` | ✓ |   | — | Off-thread sljit compile-worker telemetry and recent compile ring |
| `cdrom_sector_dump` | ✓ |   | `offset`, `len` | Dump bytes from the last CD-ROM sector observed by the controller, including LBA/mode metadata |
| `cdrom_sector_history` | ✓ |   | `count`, optional `lba` | Dump newest CD-ROM sector history entries, including raw XA subheader fields, CPU/audio delivery flags, and the first 128 bytes |
| `cdrom_sector_history_clear` | ✓ |   | — | Reset the CD-ROM sector history ring |
| `watch` | ✓ | ✓ | `addr` | Set byte-level memory watchpoint (fires per-frame on change) |
| `unwatch` | ✓ | ✓ | `addr` | Remove memory watchpoint |
| `set_input` | ✓ | ✓ | `buttons`, optional `frames`, optional `lx`, `ly`, `rx`, `ry` | Override pad1 buttons and optional analog axes (PS1 inverted bitmask, 0 = pressed; axes 0-255). Holds until `clear_input` on both backends; pass `frames=N` (beetle) to auto-release after N frames |
| `clear_input` | ✓ | ✓ | — | Remove input and analog axis overrides |
| `turbo` | ✓ |   | `enabled` | Enable/disable TCP-controlled frontend turbo for fast-forward validation |
| `turbo_state` | ✓ |   | — | Query TCP-controlled turbo state |
| `pause` | ✓ |   | — | **REMOVED** — still registered, but always returns an error. Query a ring buffer (`fn_entry_tail`, `wtrace_dump`, `gpu_frame_dump`) instead of synthesizing a snapshot |
| `continue` (`c`) | ✓ |   | — | **REMOVED** — nothing to resume, since `pause` is gone |
| `step` | ✓ |   | — | **REMOVED** — query a ring buffer over the window of interest instead of advancing N frames synchronously |
| `run_to_frame` | ✓ |   | — | **REMOVED** — use `frame_range` / `read_frame_ram` against the live frame ring instead |
| `history` | ✓ | ✓ | — | Ring buffer stats (frames available) |
| `get_frame` | ✓ | ✓ | `frame` | Full frame record from ring buffer |
| `frame_range` | ✓ | ✓ | `start`, `end` | Range query, max 200 frames |
| `frame_timeseries` | ✓ | ✓ | `start`, `end` | Compact timeseries, max 200 frames |
| `set_snapshot` | ✓ | ✓ | `slot`, `addr`, `size` | Configure per-frame RAM snapshot region (slots 0-3) |
| `get_snapshots` | ✓ | ✓ | — | Show snapshot config |
| `screenshot` | ✓ | ✓ | `path` (optional) | Write a **PNG** of the current display to `path` (default `psx_screenshot.png` in the runtime cwd); single metadata response `{path,width,height}`. `screenshot_file` is an alias; the old inline-hex-row `screenshot` is gone (it streamed h+1 response lines per request and poisoned the connection) |
| `first_failure` | ✓ |   | — | Find first divergence point between runs (native-side tracking) |
| `read_frame_ram` | ✓ |   | `addr`, `len`, `frame` | Read RAM **as of a specific frame** (from ring buffer) |
| `wtrace_range` | ✓ |   | `lo`, `hi` | Set RAM-write trace range (ring of 262 144 writes with RA — `WRITE_TRACE_CAP`, `1 << 18`) |
| `wtrace_dump` | ✓ | beetle | optional `addr_lo`, `addr_hi`, `count`, `newest` | Dump RAM-write trace entries as JSON. The address filter is applied server-side over the FULL ring before the emit cap — always pass it when hunting a specific buffer, otherwise you only see the oldest `count` entries of the whole ring |
| `wtrace_clear` | ✓ |   | — | Reset the trace ring |
| `mmio_dump` | ✓ |   | optional `addr`, `count`, `newest` | Dump the always-on MMIO write ring (256K entries, ALL 0x1F801xxx writes — SPU/DMA traffic rolls it in well under a minute of gameplay; for display history use `gp1_dump`) |
| `mmio_clear` | ✓ |   | — | Reset the MMIO write ring |
| `gp1_dump` | ✓ |   | optional `frame_lo`, `frame_hi`, `count`, `newest` | Dump the dedicated ALWAYS-ON GP1 (0x1F801814 display control) ring — 512K entries ≈ 15 min of gameplay (Tomba writes ~10 GP1/frame), survives the general MMIO ring's eviction. Frame filter is server-side over the full ring. Each entry: val + func/pc/cpu_pc/ra/sp/a0/a1/sr/epc/frame |
| `pc_break` |   | ✓² | `addr` | DS execute breakpoint, state captured on hit (via `pc_hit_last`) |
| `pc_unbreak` |   | ✓² | `addr` | Remove an execute breakpoint |
| `pc_break_list` |   | ✓² | — | List active execute breakpoints |
| `pc_hit_last` |   | ✓² | — | Captured state (PC, $ra, all GPRs, COP0) from most recent PC break hit |
| `pc_hit_clear` |   | ✓² | — | Clear the last-hit record |
| `quit` | ✓ |   | — | Shutdown native runtime |

¹ Native `vram_peek` is the legacy name; DS calls it `read_vram`. Same semantics.
² The `pc_*` family is specific to the DS oracle: DuckStation's CPU core honours `CPU::AddBreakpointWithCallback`, while our native runtime dispatches whole recompiled functions (no mid-function PC breaks).

### Boot-time write ranges

Set `PSX_WTRACE_BOOT=lo,hi[;lo,hi...]` before launching a debug-tools build to
retain the first writes to one or more half-open RAM ranges from guest
instruction zero. Addresses may be hexadecimal or decimal; KSEG addresses are
normalized to physical addresses. For example, the Crash Bash investigation
that motivated this option can be reproduced without title-specific code:

```powershell
$env:PSX_WTRACE_BOOT='0x000B3A80,0x000B3B00'
.\CrashBashRecomp.exe
```

<!-- The fork inventory below is retained as merge provenance; its active rows
have been folded into the curated inventory above.
Commands without a bespoke CLI mapping pass through generically: extra
args of the form `key=value` become JSON fields (ints when numeric, else
strings), so every server command is reachable, e.g.
`debug_client.py --port 4370 gpu_frame_dump frame=14528 count=65536`.

---

## Command inventory

Columns: **N** = native, **D** = DuckStation oracle.

| Command | N | D | Params | Description |
|---|---|---|---|---|
| `ping` | ✓ | ✓ | — | Heartbeat. Native answers immediately from the I/O thread as `{"id":0,"ok":true,"pong":true,"io_thread":true}`; it does not include the current frame |
| `frame` | ✓ | ✓ | — | Current frame number |
| `get_registers` (`regs`) | ✓ | ✓ | — | All 32 GPRs + PC + HI + LO (native also: COP0 SR/Cause/EPC, I_STAT, I_MASK) |
| `read_ram` | ✓ | ✓ | `addr`, `len` | Read bytes from PS1 address space as hex string — up to the full 2 MB in ONE response line. `dump_ram` is an alias (the old chunked multi-line variant is gone: it broke the one-request/one-response protocol and wedged the server) |
| `write_ram` | ✓ | ✓ | `addr`, `hex` | Write bytes to PS1 address space. `hex` = even-length hex string (handler cap 0x1000 bytes; the ~8 KB request line limits the effective payload to ~4 KB — larger requests are rejected). Legacy single-byte form `addr`+`val` (one byte) still accepted |
| `call_func` | ✓ |   | `addr`, `a0`..`a3` | Invoke a guest function synchronously via `psx_dispatch_call` (emu thread, vblank safe point; $ra = kernel return sentinel). Returns `v0`. The callee really runs — use with care (long callees block the poll) |
| `read_scratch` |   | ✓ | `addr`, `len` | Read PS1 scratchpad (0x1F800000 region) |
| `read_vram` / `vram_peek` | ✓¹ | ✓ | `x`, `y`, `w`, `h` | Read 16-bit VRAM pixels (max 128×128) |
| `gpu_state` | ✓ | ✓ | — | Display area, display depth, draw offset, GPUSTAT, clip rect, xfer state |
| `sio_state` | ✓ | ✓ | — | SIO registers + (native only) pad/memcard protocol + TX/RX history |
| `irq_state` | ✓ | ✓ | — | `I_STAT`, `I_MASK` (both), plus chain state on native |
| `dma_state` | ✓ | ✓ | — | DPCR, DICR, all 7 channel states (madr/bcr/chcr) |
| `event_state` |   | ✓ | — | EvCB table summary (stub on DS — events are BIOS-level) |
| `overlay_state` | ✓ | ✓ | — | **Native:** in-game developer debug overlay visibility flag (read). Toggled in-game by Ctrl+F3; exists so tests can read the flag without key injection. On Release builds always reports `visible:false` (the API is a static-inline no-op when `PSX_DEBUG_OVERLAY` is off). Response: `{"id":N,"ok":true,"visible":true\|false}`. **DS:** Current overlay info (separate command; only the name overlaps) |
| `sljit_async` | ✓ |   | — | Off-thread sljit compile-worker telemetry: `worker_running`, `async_on`, always-on counters (`enqueued`/`compiled`/`failed`/`queued_now`), `last_compile_ms`/`max_compile_ms` (off-thread compile time), resolved `cache_dir`, and a `recent[]` ring of the last 64 compiles (per-shard `phys`/`crc`/`code_len`/`ms`/`ok`). Query it to prove the worker compiles off the dispatch thread without a frame hitch |
| `cdrom_sector_dump` | ✓ |   | `offset`, `len` | Dump bytes from the last CD-ROM sector observed by the controller, including LBA/mode metadata |
| `cdrom_sector_history` | ✓ |   | `count`, optional `lba` | Dump newest CD-ROM sector history entries, including raw XA subheader fields, CPU/audio delivery flags, and the first 128 bytes |
| `cdrom_sector_history_clear` | ✓ |   | — | Reset the CD-ROM sector history ring |
| `watch` | ✓ | ✓ | `addr` | Set byte-level memory watchpoint (fires per-frame on change) |
| `unwatch` | ✓ | ✓ | `addr` | Remove memory watchpoint |
| `set_input` | ✓ | ✓ | `buttons`, optional `frames`, optional `lx`, `ly`, `rx`, `ry` | Override pad1 buttons and optional analog axes (PS1 inverted bitmask, 0 = pressed; axes 0-255). Holds until `clear_input` on both backends; pass `frames=N` (beetle) to auto-release after N frames |
| `clear_input` | ✓ | ✓ | — | Remove input and analog axis overrides |
| `turbo` | ✓ |   | `enabled` | Enable/disable TCP-controlled frontend turbo for fast-forward validation |
| `turbo_state` | ✓ |   | — | Query TCP-controlled turbo state |
| `pause` |   | ✓ | — | DuckStation only. Native returns an error; query its continuous ring buffers instead |
| `continue` (`c`) |   | ✓ | — | DuckStation only. Native pause/resume was removed |
| `step` |   | ✓ | `[count]` | DuckStation only. Native returns an error; query its continuous ring buffers instead |
| `run_to_frame` |   | ✓ | `frame` | DuckStation only. Native returns an error; use `frame_range` / `read_frame_ram` |
| `history` | ✓ | ✓ | — | Ring buffer stats (frames available) |
| `get_frame` | ✓ | ✓ | `frame` | Full frame record from ring buffer |
| `frame_range` | ✓ | ✓ | `start`, `end` | Range query, max 200 frames |
| `frame_timeseries` | ✓ | ✓ | `start`, `end` | Compact timeseries, max 200 frames |
| `set_snapshot` | ✓ | ✓ | `slot`, `addr`, `size` | Configure per-frame RAM snapshot region (slots 0-3) |
| `get_snapshots` | ✓ | ✓ | — | Show snapshot config |
| `window_shot` | ✓ |   | `path` (optional) | Arm a Debug-build one-shot PNG of the composited window after ImGui rendering; the file appears on the next pre-swap. Release accepts the request but writes no file |
| `window_shot` | ✓ |   | `path` (optional) | **Native (Debug builds only):** arm a one-shot capture of the composited WINDOW (game + overlay, if visible) to PNG. Different from `screenshot` (PS1 display buffer) and `wide_shot` (wide compositor surface): this reads the default framebuffer AFTER the pre_swap hook has drawn the ImGui frame, so overlay pixels are included. The readback runs on the next main-thread pre_swap (next vblank), so the file appears one frame after this command returns. Tests must poll for the file. Response: `{"id":N,"ok":true,"path":"...","armed":true}`. On Release builds the API is a static-inline no-op, so the command compiles and answers `armed:true` but no file is written. |
| `screenshot` | ✓ | ✓ | `path` (optional) | Write a 24-bit BMP of the current display to `path` (default `psx_screenshot.bmp` in the runtime cwd); single metadata response `{path,width,height}`. `screenshot_file` is an alias; the old inline-hex-row `screenshot` is gone (it streamed h+1 response lines per request and poisoned the connection) |
| `first_failure` | ✓ |   | — | Find first divergence point between runs (native-side tracking) |
| `read_frame_ram` | ✓ |   | `addr`, `len`, `frame` | Read RAM **as of a specific frame** (from ring buffer) |
| `wtrace_range` | ✓ |   | `lo`, `hi` | Set RAM-write trace range (ring of 1024 writes with RA) |
| `wtrace_dump` | ✓ | beetle | optional `addr_lo`, `addr_hi`, `count`, `newest` | Dump RAM-write trace entries as JSON. The address filter is applied server-side over the FULL ring before the emit cap — always pass it when hunting a specific buffer, otherwise you only see the oldest `count` entries of the whole ring |
| `wtrace_clear` | ✓ |   | — | Reset the trace ring |
| `mmio_dump` | ✓ |   | optional `addr`, `count`, `newest` | Dump the always-on MMIO write ring (256K entries, ALL 0x1F801xxx writes — SPU/DMA traffic rolls it in well under a minute of gameplay; for display history use `gp1_dump`) |
| `mmio_clear` | ✓ |   | — | Reset the MMIO write ring |
| `gp1_dump` | ✓ |   | optional `frame_lo`, `frame_hi`, `count`, `newest` | Dump the dedicated ALWAYS-ON GP1 (0x1F801814 display control) ring — 512K entries ≈ 15 min of gameplay (Tomba writes ~10 GP1/frame), survives the general MMIO ring's eviction. Frame filter is server-side over the full ring. Each entry: val + func/pc/cpu_pc/ra/sp/a0/a1/sr/epc/frame |
| `pc_break` |   | ✓² | `addr` | DS execute breakpoint, state captured on hit (via `pc_hit_last`) |
| `pc_unbreak` |   | ✓² | `addr` | Remove an execute breakpoint |
| `pc_break_list` |   | ✓² | — | List active execute breakpoints |
| `pc_hit_last` |   | ✓² | — | Captured state (PC, $ra, all GPRs, COP0) from most recent PC break hit |
| `pc_hit_clear` |   | ✓² | — | Clear the last-hit record |
| `overlay_toggle` | ✓ |   | — | Toggle the Debug-build developer overlay; Release remains `visible:false` |
| `overlay_widget_action` | ✓ |   | `name`, `value`, `value2` | Drive the same Xenogears debug-overlay action functions used by its widgets; unavailable in Release builds |
| `quit` | ✓ |   | — | Shutdown native runtime |
| `overlay_toggle` | ✓ |   | — | **Native:** flip the in-game developer debug overlay visibility flag (Debug builds only). Same purpose as the Ctrl+F3 hotkey — exists so tests can drive the flag without key injection. On Release builds the API is a static-inline no-op, so the call is harmless and the response always reports `visible:false`. Response: `{"id":N,"ok":true,"visible":true\|false}` |
| `overlay_widget_action` | ✓ |   | `name`, `value`, `value2` | **Native:** invoke one of the in-window debug overlay widget's action functions (the same code path a checkbox / button / slider click would call). Lets a remote client assert the TOGGLES section is wired to the real runtime setters without synthesizing real mouse clicks (which is impossible over TCP). Does NOT bypass the action path — it calls the same function the widget calls. `name` is one of: `texfilter`, `native_wide`, `aspect_set`, `bd_stretch_on`, `bd_stretch_pct`, `interp`, `supersampling`, `antialiasing`, `screen_model`, `turbo_loads`, `spu_hq`, `window_width`, `dump_event_ring`, `dump_latency_ring`, `dump_starv_ring`, `teleport`, `party_slot`, `party_bitfield`, `gold`, `write_var`, `force_battle`, `camera_write`, `event_jump`, `read_field_id`. `value` and `value2` are interpreted per the name; see the per-name section below. `dump_*` actions ignore both. Response: `{"id":N,"ok":true,"name":"...","value":N,"value2":M}` on success, `{"id":N,"ok":false,"err":"unknown name"}` on bad name. **Debug builds only; Release compiles the command to a static-inline `-1` return.** |

¹ Native `vram_peek` is the legacy name; DS calls it `read_vram`. Same semantics.  
² The `pc_*` family is specific to the DS oracle: DuckStation's CPU core honours `CPU::AddBreakpointWithCallback`, while our native runtime dispatches whole recompiled functions (no mid-function PC breaks).
-->
Connect at any later point and query `wtrace_boot_stats`,
`wtrace_boot_summary`, or `wtrace_boot_dump`. Each retained entry includes the
write address/value/width, guest PC and return address, register context, frame,
and DMA channel. The option is ignored in builds made with debug tools disabled.

---

## Divergence-hunt workflow

When a recompiled-BIOS bug is suspected, the two servers let you find the **first** divergence instead of chasing symptoms. Standard procedure (inherited from v3's `DEBUG.md`):

1. **Sync state via PC + registers, not frame number.** Frames drift after even a single timing glitch. Pause both servers; compare `get_registers` until they match.
2. **Dump both sides fully.** Compare `get_frame`, `gpu_state`, `irq_state`, `dma_state` (DS), `dump_ram` over the same regions.
3. **Byte-level comparison.** Tiny mismatches usually point at one subsystem. Use `debug_client.py compare <cmd>` for automatic diff.
4. **Find the earliest mismatch**, not a later symptom. Ring-buffer queries (`frame_range`, `read_frame_ram`) help locate which frame went wrong.
5. **Trace the write.** Use `watch` to catch the divergent store, or DS's `pc_break` on the suspect function entry. Look at `$ra` in `pc_hit_last` to identify the caller chain.
6. **Classify.** codegen (recompiler generates wrong instruction), runtime (MMIO or kernel simulation wrong), timing (IRQ cadence), or BIOS (real-hardware quirk we didn't model).
7. **Minimal fix** in the correct subsystem. Never hand-deliver state to hide the symptom (see CLAUDE.md §0).

---

## Server send budget + serve-stall telemetry

The server is pumped on the **main thread**: every millisecond it spends
sending a response is a millisecond the emulator does not run. Inline
responses are bounded — 2 s per zero-progress chunk and **15 s total per
response**; a client that exceeds the budget is disconnected (the runtime
never stalls indefinitely). Responses bigger than the budget allows must
use the `*_dump_file` variants, which write to disk instead of the socket.

Cumulative serve-stall is exported as `tcp_send_stall_ms` /
`tcp_clients_dropped` in `psx_freeze_heartbeat.json` (plus per-tick
`tcp_ms` in its ring) and in every wedge/fatal dump header. **Check these
first when diagnosing slow or stalled frames** — on 2026-06-10 two
"attract-idle degradations" turned out to be a TCP client trickle-draining
mega-dumps, throttling the main loop to 6 fps (all 8 watchdog stack
samples inside `WS2_32!send`). A slow-frames wedge with a large
`tcp_send_stall_ms` delta over the same window is observer interference,
not a guest bug.

## Call-contract (bail) telemetry

The dispatch call contract (Bug D family fix, 2026-06-10) guards every
generated continuation: it may only run if the guest actually returned to
the call site ($ra == site return address, $sp == caller's sp at the
call). Violations begin a "bail" unwind that abandons stale C frames and
re-dispatches the guest's true target. Counters in
`psx_freeze_heartbeat.json` and dump headers:

- `bail_first` — contract violations detected (wild returns). Nonzero
  during gameplay means the game executed a wild control transfer (e.g.
  Tomba's dead jumptable case `jal 0x80120B3C`, the chest freeze). A
  small count with the game continuing normally is the fix working.
- `bail_resolved` — unwinds that resolved at an enclosing call site whose
  contract matched (multi-level return).
- `bail_flattened` — unwinds that reached the outermost dispatch loop and
  re-dispatched the wild target on a clean host stack.
- `bail_anomaly` — bail flag observed at exception entry (must stay 0;
  anything else is a runtime bug).

---

## `bios_info` — linked recompiled-BIOS identity (native only)

Reports which BIOS image this build's recompiled C was generated from
(`psx_bios_image`, emitted into the generated dispatch from the BIOS profile)
and whether the loaded ROM matches it: `image_id`, `sha256`, `crc32`, `size`,
`bundled` (redistributable image shipped with the game), the kernel-bless
window, the HLE anchors (`shell_entry_phys` / `deliver_event_ret`; 0 =
structurally unavailable on this BIOS), and `image_wordsum` vs
`loaded_wordsum` with `match`. With the launch identity gate a running
process always reports `match:1`.

- `{"cmd":"bios_info"}`

## `s3_smear_watch` — callee-saved-register smear tripwire (native only)

Latches the first interpreted instruction in a PC window whose execution
changes `$s3` (`runtime/src/dirty_ram_interp.c`). A `jalr`'s exec_one spans
the entire nested native callee, so the latch names the callee that returned
with a clobbered callee-saved register; the insn ring is frozen at the latch.

- `{"cmd":"s3_smear_watch","lo":"<hex>","hi":"<hex>"}` — arm (each arming
  fully re-specifies the watch). Optional `"excl":"<hex insn>"`: exact
  encoding to ignore, so a watched loop's own `$s3` advance (e.g. an
  `addi s3,s3,8` list walk) doesn't trip the latch.
- `{"cmd":"s3_smear_watch"}` — report the latch: `valid`, `pc`, `insn`,
  `s3_old`/`s3_new`, `call_target` (rs at the call site for jr/jalr),
  `frame`.
- `{"cmd":"s3_smear_watch","lo":"0"}` — disarm.

## `callret_watch` — interp JALR call-resolution ring (native only)

64-entry ring (`runtime/src/dirty_ram_interp.c`) recording, for every
interpreted JALR whose call PC lies in a window, which resolution tier ran
the callee and the full post-call outcome — the complement of
`s3_smear_watch`: the tripwire names the callee that came back smeared, this
ring names the return path that let it come back.

- `{"cmd":"callret_watch","lo":"<hex>","hi":"<hex>"}` — arm (resets the ring).
- `{"cmd":"callret_watch"}` — dump (newest last): per entry `cyc`, `f`
  (frame), `pc`, `tgt`, `path` (`CRES_*` tier code, see the enum in
  dirty_ram_interp.c; `|0x100` = finish() escaped), pre-call
  `sp_b`/`ra_b`/`s0_b`/`s3_b`, post-call `pc_a`/`ra_a`/`sp_a`/`s0_a`/
  `s3_a`/`v0_a`, `bail`/`rfe`/`esc`/`in_exc` flags, `dstatic`/`dblocks`/
  `dexc` engine-attribution deltas across the call, `last_func`.
- `{"cmd":"callret_watch","lo":"0"}` — disarm.

## `hle_dump` — BIOS-HLE tier call ring (native only)

Always-on ring (`runtime/src/bios_hle.c`, 16K entries) recording every
A0/B0/C0 kernel-vector dispatch the HLE tier's hook observes, plus the boot
shell-skip event.

The hook is installed when EITHER axis is on (`bios_hle_plan.h`), so a run with
the boot-skip on but kernel calls left to LLE — the default on the bundled
OpenBIOS, which exports no `deliver_event_ret` — reports
`backend: "LLE (recompiled BIOS)"` and still fills the ring with `route: 0`
vector observations plus the one `route: 2` boot entry. Only a run with BOTH axes
off installs no hook and leaves the ring empty; use `bioscall_dump` for LLE-side
vector observation there.

- `{"cmd":"hle_dump"}` — status: `backend` (`HLE (LLE fallback)` /
  `LLE (recompiled BIOS)`), `boot_skip`, `boot_turbo_active`, `total`.
- `{"cmd":"hle_dump","tail":N}` — last N entries: `seq`, `cycle` (guest
  cycle), `vec` (0xA0/0xB0/0xC0, or 0x30000 for the boot skip), `fn` ($t1
  function number), `a0..a3`, `ra`, `v0` (result when HLE-serviced), `route`
  (0 = fell through to LLE, 1 = serviced in HLE, 2 = boot shell-skip).
- Filters: `"fn":N`, `"route":0|1|2`.

---

## Rule when the server can't answer your question

If an inspection need isn't covered by the existing commands, **do not fall back to printf or log files**. Instead:

1. Add a handler in `runtime/src/debug_server.c` (native)
2. Add the matching handler in `runtime/src/beetle_debug_server.c` (Beetle oracle)
   when the question needs a cross-check against hardware behaviour
3. Keep field names parallel between the two
4. Run `python tools/gen_tcp_commands.py` to refresh the generated index, and add
   a row to the curated inventory above if the command needs explaining

> Step 4 used to read "Update this file", and that did not survive contact with
> reality: this document described 47 commands while the servers registered 292.
> The index is now generated from the command tables, and
> `python tools/gen_tcp_commands.py --check` fails when it drifts. Prose in the
> curated inventory is still hand-written and still worth adding.

The TCP server is the canonical instrumentation surface. Rule 3 in `CLAUDE.md` is absolute: **no `fprintf(stderr, …)` in source code, ever, for any reason**.

<!-- The fork's older generic TCP prose is superseded by the upstream sections above.
When a recompiled-BIOS bug is suspected, the two servers let you find the **first** divergence instead of chasing symptoms. Standard procedure (inherited from v3's `DEBUG.md`):

1. **Sync state via PC + registers, not frame number.** Frames drift after even a single timing glitch. Pause both servers; compare `get_registers` until they match.
2. **Dump both sides fully.** Compare `get_frame`, `gpu_state`, `irq_state`, `dma_state` (DS), `dump_ram` over the same regions.
3. **Byte-level comparison.** Tiny mismatches usually point at one subsystem. Use `debug_client.py compare <cmd>` for automatic diff.
4. **Find the earliest mismatch**, not a later symptom. Ring-buffer queries (`frame_range`, `read_frame_ram`) help locate which frame went wrong.
5. **Trace the write.** Use `watch` to catch the divergent store, or DS's `pc_break` on the suspect function entry. Look at `$ra` in `pc_hit_last` to identify the caller chain.
6. **Classify.** codegen (recompiler generates wrong instruction), runtime (MMIO or kernel simulation wrong), timing (IRQ cadence), or BIOS (real-hardware quirk we didn't model).
7. **Minimal fix** in the correct subsystem. Never hand-deliver state to hide the symptom (see CLAUDE.md §0).

---

## Server send budget + serve-stall telemetry

The server is pumped on the **main thread**: every millisecond it spends
sending a response is a millisecond the emulator does not run. Inline
responses are bounded — 2 s per zero-progress chunk and **15 s total per
response**; a client that exceeds the budget is disconnected (the runtime
never stalls indefinitely). Responses bigger than the budget allows must
use the `*_dump_file` variants, which write to disk instead of the socket.

Cumulative serve-stall is exported as `tcp_send_stall_ms` /
`tcp_clients_dropped` in `psx_freeze_heartbeat.json` (plus per-tick
`tcp_ms` in its ring) and in every wedge/fatal dump header. **Check these
first when diagnosing slow or stalled frames** — on 2026-06-10 two
"attract-idle degradations" turned out to be a TCP client trickle-draining
mega-dumps, throttling the main loop to 6 fps (all 8 watchdog stack
samples inside `WS2_32!send`). A slow-frames wedge with a large
`tcp_send_stall_ms` delta over the same window is observer interference,
not a guest bug.

## Call-contract (bail) telemetry

The dispatch call contract (Bug D family fix, 2026-06-10) guards every
generated continuation: it may only run if the guest actually returned to
the call site ($ra == site return address, $sp == caller's sp at the
call). Violations begin a "bail" unwind that abandons stale C frames and
re-dispatches the guest's true target. Counters in
`psx_freeze_heartbeat.json` and dump headers:

- `bail_first` — contract violations detected (wild returns). Nonzero
  during gameplay means the game executed a wild control transfer (e.g.
  Tomba's dead jumptable case `jal 0x80120B3C`, the chest freeze). A
  small count with the game continuing normally is the fix working.
- `bail_resolved` — unwinds that resolved at an enclosing call site whose
  contract matched (multi-level return).
- `bail_flattened` — unwinds that reached the outermost dispatch loop and
  re-dispatched the wild target on a clean host stack.
- `bail_anomaly` — bail flag observed at exception entry (must stay 0;
  anything else is a runtime bug).

---

## `bios_info` — linked recompiled-BIOS identity (native only)

Reports which BIOS image this build's recompiled C was generated from
(`psx_bios_image`, emitted into the generated dispatch from the BIOS profile)
and whether the loaded ROM matches it: `image_id`, `sha256`, `crc32`, `size`,
`bundled` (redistributable image shipped with the game), the kernel-bless
window, the HLE anchors (`shell_entry_phys` / `deliver_event_ret`; 0 =
structurally unavailable on this BIOS), and `image_wordsum` vs
`loaded_wordsum` with `match`. With the launch identity gate a running
process always reports `match:1`.

- `{"cmd":"bios_info"}`

## `s3_smear_watch` — callee-saved-register smear tripwire (native only)

Latches the first interpreted instruction in a PC window whose execution
changes `$s3` (`runtime/src/dirty_ram_interp.c`). A `jalr`'s exec_one spans
the entire nested native callee, so the latch names the callee that returned
with a clobbered callee-saved register; the insn ring is frozen at the latch.

- `{"cmd":"s3_smear_watch","lo":"<hex>","hi":"<hex>"}` — arm (each arming
  fully re-specifies the watch). Optional `"excl":"<hex insn>"`: exact
  encoding to ignore, so a watched loop's own `$s3` advance (e.g. an
  `addi s3,s3,8` list walk) doesn't trip the latch.
- `{"cmd":"s3_smear_watch"}` — report the latch: `valid`, `pc`, `insn`,
  `s3_old`/`s3_new`, `call_target` (rs at the call site for jr/jalr),
  `frame`.
- `{"cmd":"s3_smear_watch","lo":"0"}` — disarm.

## `callret_watch` — interp JALR call-resolution ring (native only)

64-entry ring (`runtime/src/dirty_ram_interp.c`) recording, for every
interpreted JALR whose call PC lies in a window, which resolution tier ran
the callee and the full post-call outcome — the complement of
`s3_smear_watch`: the tripwire names the callee that came back smeared, this
ring names the return path that let it come back.

- `{"cmd":"callret_watch","lo":"<hex>","hi":"<hex>"}` — arm (resets the ring).
- `{"cmd":"callret_watch"}` — dump (newest last): per entry `cyc`, `f`
  (frame), `pc`, `tgt`, `path` (`CRES_*` tier code, see the enum in
  dirty_ram_interp.c; `|0x100` = finish() escaped), pre-call
  `sp_b`/`ra_b`/`s0_b`/`s3_b`, post-call `pc_a`/`ra_a`/`sp_a`/`s0_a`/
  `s3_a`/`v0_a`, `bail`/`rfe`/`esc`/`in_exc` flags, `dstatic`/`dblocks`/
  `dexc` engine-attribution deltas across the call, `last_func`.
- `{"cmd":"callret_watch","lo":"0"}` — disarm.

## `hle_dump` — BIOS-HLE tier call ring (native only)

Always-on ring (`runtime/src/bios_hle.c`, 16K entries) recording every
A0/B0/C0 kernel-vector dispatch the HLE tier's hook observes, plus the boot
shell-skip event. Empty in pure-LLE mode (the tier installs no hook there —
use `bioscall_dump` for LLE-side vector observation).

- `{"cmd":"hle_dump"}` — status: `backend` (`HLE (LLE fallback)` /
  `LLE (recompiled BIOS)`), `boot_skip`, `boot_turbo_active`, `total`.
- `{"cmd":"hle_dump","tail":N}` — last N entries: `seq`, `cycle` (guest
  cycle), `vec` (0xA0/0xB0/0xC0, or 0x30000 for the boot skip), `fn` ($t1
  function number), `a0..a3`, `ra`, `v0` (result when HLE-serviced), `route`
  (0 = fell through to LLE, 1 = serviced in HLE, 2 = boot shell-skip).
- Filters: `"fn":N`, `"route":0|1|2`.

-->

---

## `overlay_state` / `overlay_toggle` — in-game debug overlay (native, Debug only)

The in-game developer overlay (Dear ImGui, toggled with Ctrl+F3) is built
when the runtime CMake gate `PSX_DEBUG_OVERLAY` is `ON` (the `build-dbg`
target). These two commands exist so test harnesses can read and flip
the visibility flag without synthesizing keypresses through the OS:

- `{"cmd":"overlay_state"}` → `{"id":N,"ok":true,"visible":true|false}`
- `{"cmd":"overlay_toggle"}` → `{"id":N,"ok":true,"visible":true|false}` (flips first, then reports the new state)

Params: none.

On **Release** builds the API is a static-inline no-op (the entire
`debug_overlay.cpp` TU is empty and the header collapses every call to
`return false;` / `{}`). The two commands therefore compile and answer
harmlessly — `overlay_state` always reports `visible:false`,
`overlay_toggle` always reports `visible:false` (no flip happened).
This is the intended Release contract: zero new symbols, zero new
behavior, the existing `PSX_DEBUG_OVERLAY` gate covers everything.

The Ctrl+F3 hotkey is bound in `runtime/src/main.cpp` — the SDL event
pump consults `psx_debug_overlay_process_event` ahead of the
F1–F12 savestate block, so Ctrl+F3 is consumed by the overlay while
plain F3 still loads savestate slot 2 (and Shift+F3 still saves slot 2).
The two TCP commands are the automation-friendly equivalent of that
hotkey.

---

## `overlay_widget_action` — drive in-window widget actions (native, Debug only)

The in-window "Xenogears Debug" window (Dear ImGui, four sections:
GPU State / RAM Inspector / Toggles / Rings) renders read-only views
and Toggles checkboxes / sliders. A remote client cannot synthesize a
real mouse click on an ImGui checkbox, so this command exposes each
toggle's action as a function the TCP test can call directly. It is
the **automation-friendly equivalent of the checkbox click** — it does
NOT bypass the action path; it calls the same function the widget
calls. After every call the matching TCP getter (e.g. `gpu_state`,
`ws_nw`, `ws_aspect_get`, `ws_backdrop_stretch`, `gl_interp`) must reflect
the new value; that round-trip is the test contract (see
`tests/test_overlay_widgets.py`).

- `{"cmd":"overlay_widget_action","name":"texfilter","value":1}` → applies
  bilinear texture filter (same path as the "Texture filter" checkbox).
- `{"cmd":"overlay_widget_action","name":"native_wide","value":2}` →
  engages native-wide (2), squash (1), or off (0). Same path as the
  Native-wide radio / buttons.
- `{"cmd":"overlay_widget_action","name":"aspect_set","value":16,"value2":9}`
  → sets widescreen aspect to 16:9 via `gte_set_display_aspect`.
- `{"cmd":"overlay_widget_action","name":"bd_stretch_on","value":1}` and
  `{"cmd":"overlay_widget_action","name":"bd_stretch_pct","value":120}`
  → set the backdrop-stretch on-flag and percent.
- `{"cmd":"overlay_widget_action","name":"interp","value":1}` → toggle
  frame interpolation (uses current host_hz/target_hz from the diag
  helper, same as the checkbox). Re-enabling requires a GL reinit; the
  in-window label notes this.
- `{"cmd":"overlay_widget_action","name":"supersampling","value":2}` →
  set internal-resolution scale 1..8 (live: the GL raster rebuilds at the
  next present — one-frame hitch; falls back to the previous scale if the
  rebuild fails).
- `{"cmd":"overlay_widget_action","name":"window_width","value":1280}` →
  resize the game window (640..7680 px; height follows the configured
  aspect ratio, live — present paths re-read the drawable per frame).
- `{"cmd":"overlay_widget_action","name":"antialiasing","value":1}` →
  toggle the present-path linear filter (live).
- `{"cmd":"overlay_widget_action","name":"screen_model","value":1}` →
  set the scanout color model 0=raw, 1=CRT, 2=composite, 3=trinitron
  (live — CPU scanout LUT + GL present-shader grade on the 15-bit game
  paths; 24-bit/FMV frames pass through, per the documented semantics).
- `{"cmd":"overlay_widget_action","name":"turbo_loads","value":1}` →
  toggle turbo-through-loads (live; same global the `turbo_loads` TCP
  command flips).
- `{"cmd":"overlay_widget_action","name":"spu_hq","value":1}` →
  toggle the high-quality SPU float shadow (live).
- `{"cmd":"overlay_widget_action","name":"dump_event_ring"}` →
  writes `event_ring.json` next to the exe via `event_ring_dump_file`
  (same call the `event_ring_dump` TCP command uses).
- `{"cmd":"overlay_widget_action","name":"dump_latency_ring"}` →
  writes `latency_ring.json` with summary + frames array (same shape
  the `latency` TCP command's response uses).
- `{"cmd":"overlay_widget_action","name":"dump_starv_ring"}` →
  writes `starvation_ring.json` via `starvation_ring_dump`.

### Write-action names (N13 panels)

The "Map Teleport" / "Party" / "Gold & Variables" panels in the
"Xenogears Debug" window expose the **same** write functions over
this command. Every write goes through `psx_write_byte` to the
verified addresses in `debug_overlay/data/addrs.xml`; we never
touch `fieldID` directly and never call `loadNewField` (the field
poll at `0x800784A0` is the only correct caller).

- `{"cmd":"overlay_widget_action","name":"teleport","value":<fieldId>,"value2":<entryPoint>}`
  → fires the verified 7-write recipe. The field-module guard
  (`fieldContextPtr 0x800B0078 != 0`) must be satisfied; otherwise
  the function returns 1 and the recipe is NOT written. The engine
  picks up `fieldMapNumber` (0x8004F34C) and `fieldEntryPoint`
  (0x8006EF66) on the next frame and runs the full completion
  sequence. Response on success: `{"ok":true,...,"value":0}`; on
  refusal: `{"ok":true,...,"value":1}` (the action's own return
  rides in `value` — pass through OK semantics).
- `{"cmd":"overlay_widget_action","name":"party_slot","value":<slot*256+charId>,"value2":<bitfieldBit|-1>}`
  → writes party slot [0..2] to the kernel master slots at
  `0x80062590+slot*4` (u32, low byte = char id, 0xFF = empty), then
  automatically ORs the unlock-bitfield (`0x8006F364`) bits of ALL
  non-empty party members (the camp menu lists members from the
  bitfield; a member without their bit crashes field loading), and
  additionally ORs `bitfieldBit` (0..10) when `value2 >= 0`.
  `slot` and `charId` are packed into `value`:
  `value = (charId << 8) | slot`. NOTE: gameState `currentParty`
  (0x8006F368) is a per-frame copy of the kernel slots (sync at
  0x800A3200) — writing it directly is silently reverted; the kernel
  master is the correct target and gameState follows on the next
  frame.
- `{"cmd":"overlay_widget_action","name":"party_bitfield","value":<u16>}`
  → writes the 2-byte unlock bitfield at `0x8006F364` (LE).
- `{"cmd":"overlay_widget_action","name":"gold","value":<u32>}`
  → writes the 4-byte money value at `0x8006EF58` (LE). The
  `value` is a signed int; pass values up to `2^31-1` (the runtime
  clamps to u32 on the write).
- `{"cmd":"overlay_widget_action","name":"write_var","value":<var>,"value2":<u16>}`
  → writes `fieldVars[var]` at `0x8006EF64+var*2` (LE). `var` is
  0..511; named vars are in `debug_overlay/data/flags.xml` (e.g.
  `var 0 = GameProgress`, `var 2 = FieldEntryPoint`).
- `{"cmd":"overlay_widget_action","name":"read_field_id","value":0}`
  → reads `fieldID` from `0x8006F94E` (LE u16). Returns -1 when
  the field module is not active.
- `{"cmd":"overlay_widget_action","name":"force_battle","value":<u32>}`
  → writes the encounter-trigger gate u32 LE at `0x800B2298`
  (reference-verified: the reference's validation hook writes 0 here
  to disable encounters for deterministic replay). `value=0` disables,
  non-zero arms. **Best-effort:** the actual battle firing still
  requires the field to have encounter data loaded AND the per-field
  countdown to reach zero; those vars' live addresses are not in the
  reference address book, so the action arms the gate but cannot
  guarantee a battle on the next frame. Documented in the in-window
  "Force Battle" panel (Section 8).
- `{"cmd":"overlay_widget_action","name":"camera_write","value":<packed_xy>,"value2":<packed_xy>}`
  → writes 3 x s16 LE to `cameraEye` (0x800AF880) and `cameraAt`
  (0x800AF890). Bit-packing: `value = (ey << 16) | ex`,
  `value2 = (ay << 16) | ax` (all s16). The Z coords are read from
  the overlay's editor state (`s_camera_eye[2]`, `s_camera_at[2]`) so
  the TCP test can either pre-load them via the in-window panel, or
  chain this with `value=0,value2=0` to test the read-only path. Both
  addresses are verified-static (addrs.xml + reference validation at
  0x800af880/0x800af890).
- `{"cmd":"overlay_widget_action","name":"event_jump","value":<eventId>}`
  → applies the event's `varWrites` (via the `write_var` path:
  `fieldVars[var]` at `0x8006EF64+var*2`, LE) then fires the verified
  7-write teleport recipe. `eventId` is the index into the events
  table loaded from `debug_overlay/data/events.xml` (35 entries
  currently; verified=green button, unverified=greyed). The TCP
  action does NOT enforce the verified flag (TCP clients can read the
  panel; the only guard is the field-module-active check inside the
  teleport recipe).

Response on success: `{"id":N,"ok":true,"name":"...","value":N,"value2":M}`.
Response on unknown name: `{"id":N,"ok":false,"err":"unknown name"}`.

On **Release** builds the API is a static-inline no-op returning `-1`,
so the command compiles and answers `ok:false,"err":"unknown name"`
without ever touching runtime state. The entire gate is the existing
`PSX_DEBUG_OVERLAY` flag — no new symbols in Release.

---

## `window_shot` — composited-window capture (native, Debug only)

Arms a one-shot PNG capture of the **composited window** (game + ImGui
overlay, if visible). Three things make this different from the existing
`screenshot` / `wide_shot` commands:

1. **Source surface.** `screenshot` dumps the PS1 display buffer (15-bit
   VRAM, scaled to 24-bit RGB in software). `wide_shot` dumps the
   native-wide compositor surface. `window_shot` reads the SDL window's
   **default framebuffer** — the same buffer the next `SDL_GL_SwapWindow`
   will present to the screen.
2. **Overlay compositing.** Because the pre-swap hook (`gpu_gl_renderer.c`
   → `psx_debug_overlay_pre_swap()`) draws the ImGui frame into the
   default framebuffer BEFORE the swap, `window_shot` captures overlay
   pixels composited over the game image. With the overlay hidden, the
   shot is pixel-identical to a `screenshot` composited into the window's
   letterbox.
3. **One-shot arm, frame-delayed.** The readback happens on the next
   main-thread pre_swap (next vblank), so the PNG file appears one frame
   after this command returns `armed:true`. Tests must poll for the file
   rather than waiting on the response.

- `{"cmd":"window_shot","path":"a.png"}` →
  `{"id":N,"ok":true,"path":"a.png","armed":true}` (file appears next frame)
- `{"cmd":"window_shot"}` → same, default `path` is `window_shot.png`
  next to the runtime exe.

On **Release** builds the API is a static-inline no-op (the same
`PSX_DEBUG_OVERLAY` gate that covers the rest of the overlay). The
command compiles and answers `armed:true`, but no file is ever written.

<!-- The upstream instrumentation rule above supersedes this legacy DuckStation-specific rule.

---

## Rule when the server can't answer your question

If an inspection need isn't covered by the existing commands, **do not fall back to printf or log files**. Instead:

1. Add a handler in `runtime/src/debug_server.c` (native)
2. Add the matching handler in `duckstation/src/core/psxrecomp_debug_server.cpp` (DS), regenerate the patch via the instructions in `tools/duckstation/README.md`
3. Keep field names parallel between the two
4. Update this file

The TCP server is the canonical instrumentation surface. Rule 3 in `CLAUDE.md` is absolute: **no `fprintf(stderr, …)` in source code, ever, for any reason**.
-->

<!-- BEGIN AUTOGENERATED COMMAND INDEX -- edit tools/gen_tcp_commands.py, not this block -->

## Complete command index (generated)

**326 commands registered** — 313 on the native server (`runtime/src/debug_server.c`), 61 on the Beetle server (`runtime/src/beetle_debug_server.c`).

56 of 326 have prose above; **270 are index-only**. An index-only command still works — it just has no description here yet. Send it `{"cmd":"<name>"}` and read the reply, or find its `handle_*` function in the server source.

Regenerate with `python tools/gen_tcp_commands.py`; `--check` fails if this block has drifted from the code.

| Command | Native | Beetle | Described above |
|---|:--:|:--:|:--:|
| `a0_history` | ✓ |  |  |
| `audio_events` | ✓ | ✓ |  |
| `audio_stats` | ✓ | ✓ |  |
| `audio_wav` | ✓ | ✓ |  |
| `autocompile_run` | ✓ |  |  |
| `autocompile_status` | ✓ |  |  |
| `bios_info` | ✓ |  | ✓ |
| `bioscall_dump` | ✓ |  |  |
| `c0_history` | ✓ |  |  |
| `call_focus_dump` | ✓ |  |  |
| `call_focus_reset` | ✓ |  |  |
| `call_focus_stats` | ✓ |  |  |
| `call_func` | ✓ |  | ✓ |
| `callret_watch` | ✓ |  | ✓ |
| `capture_freeze` | ✓ |  |  |
| `capture_quads` | ✓ |  |  |
| `card_buffer_dump` | ✓ |  |  |
| `card_data_writes` | ✓ |  |  |
| `card_data_writes_reset` | ✓ |  |  |
| `card_handoff` | ✓ |  |  |
| `card_mgr_clear` | ✓ |  |  |
| `card_mgr_trace` | ✓ |  |  |
| `card_read_summary` | ✓ |  |  |
| `card_read_summary_reset` | ✓ |  |  |
| `card_trace_dump` | ✓ |  |  |
| `card_txn_dump` | ✓ |  |  |
| `cd_overwrite` | ✓ |  |  |
| `cd_read_log` | ✓ |  |  |
| `cdc_volume` |  | ✓ |  |
| `cdrom_bursts` | ✓ |  |  |
| `cdrom_cmd_dump` |  | ✓ |  |
| `cdrom_cmd_reset` |  | ✓ |  |
| `cdrom_command_history` | ✓ |  |  |
| `cdrom_command_history_clear` | ✓ |  |  |
| `cdrom_instant_rate` | ✓ |  |  |
| `cdrom_sector_dump` | ✓ |  | ✓ |
| `cdrom_sector_history` | ✓ |  | ✓ |
| `cdrom_sector_history_clear` | ✓ |  | ✓ |
| `cdrom_state` | ✓ |  |  |
| `cdrom_timing` | ✓ |  |  |
| `cdrom_trace_clear` | ✓ |  |  |
| `cdrom_trace_dump` | ✓ |  |  |
| `ce_profile` | ✓ |  |  |
| `chain_trace` | ✓ |  |  |
| `clear_input` | ✓ | ✓ | ✓ |
| `continue` | ✓ |  | ✓ |
| `cyc_watch` | ✓ | ✓ |  |
| `cyc_watch_clear` | ✓ | ✓ |  |
| `cyc_watch_dump` | ✓ | ✓ |  |
| `cycles_to_next_event` | ✓ |  |  |
| `d44_ring` | ✓ |  |  |
| `data_shards` | ✓ |  |  |
| `devtrace_ctl` | ✓ | ✓ |  |
| `devtrace_dump` | ✓ | ✓ |  |
| `dirty_block_dump_file` | ✓ |  |  |
| `dirty_block_log` | ✓ |  |  |
| `dirty_break_clear` | ✓ |  |  |
| `dirty_break_range` | ✓ |  |  |
| `dirty_break_state` | ✓ |  |  |
| `dirty_flow_log` | ✓ |  |  |
| `dirty_insn_dump_file` | ✓ |  |  |
| `dirty_insn_gate` | ✓ |  |  |
| `dirty_insn_log` | ✓ |  |  |
| `dirty_ram_stats` | ✓ |  |  |
| `dirty_ram_unsupported` | ✓ |  |  |
| `disp_ring` | ✓ |  |  |
| `dispatch_check` | ✓ |  |  |
| `dispatch_stats` | ✓ |  |  |
| `dispatch_tail` | ✓ |  |  |
| `display_ring_aux` | ✓ |  |  |
| `display_ring_get` | ✓ |  |  |
| `display_ring_stats` | ✓ |  |  |
| `dma_cdrom_history` | ✓ |  |  |
| `dma_state` | ✓ |  | ✓ |
| `dma_trace_clear` | ✓ |  |  |
| `dma_trace_dump` | ✓ |  |  |
| `dump_buffer` | ✓ |  |  |
| `dump_ram` | ✓ | ✓ | ✓ |
| `evcb_snapshot` | ✓ |  |  |
| `evcb_walk_dump` | ✓ |  |  |
| `evcb_walk_stats` | ✓ |  |  |
| `event_ring_clear` | ✓ |  |  |
| `event_ring_dump` | ✓ |  |  |
| `event_ring_tail` | ✓ |  |  |
| `exc_ring` |  | ✓ |  |
| `first_failure` | ✓ | ✓ | ✓ |
| `fmv_state` | ✓ |  |  |
| `fn_clear` | ✓ |  |  |
| `fn_disable` | ✓ |  |  |
| `fn_entry_dump` | ✓ |  |  |
| `fn_entry_tail` | ✓ |  | ✓ |
| `fn_exit_dump` | ✓ |  |  |
| `fn_filter` | ✓ |  |  |
| `fn_stats` | ✓ |  |  |
| `fntrace_arm` | ✓ | ✓ |  |
| `fntrace_arm_clear` | ✓ |  |  |
| `fntrace_armed` | ✓ |  |  |
| `fntrace_arms` |  | ✓ |  |
| `fntrace_clear` | ✓ |  |  |
| `fntrace_disarm` |  | ✓ |  |
| `fntrace_dump` | ✓ | ✓ |  |
| `fntrace_reset` |  | ✓ |  |
| `fntrace_unfiltered` |  | ✓ |  |
| `frame` | ✓ |  | ✓ |
| `frame_fingerprint` | ✓ |  |  |
| `frame_perf` | ✓ |  |  |
| `frame_range` | ✓ | ✓ | ✓ |
| `frame_timeseries` | ✓ | ✓ | ✓ |
| `freeze_check` | ✓ |  |  |
| `game_options` | ✓ |  |  |
| `geom_correction` | ✓ |  | ✓ |
| `get_frame` | ✓ | ✓ | ✓ |
| `get_quads` | ✓ |  |  |
| `get_registers` | ✓ | ✓ | ✓ |
| `get_snapshots` | ✓ | ✓ | ✓ |
| `gl_coh_ring` | ✓ |  |  |
| `gl_fbo_peek` | ✓ |  |  |
| `gl_interp` | ✓ |  |  |
| `gl_native_center_diff` | ✓ |  |  |
| `gl_present_ring` | ✓ |  |  |
| `gl_vram_diff` | ✓ |  |  |
| `gl_wide_fast` | ✓ |  |  |
| `gl_ws_ablate` | ✓ |  |  |
| `gp1_dump` | ✓ |  | ✓ |
| `gpu_frame_dump` | ✓ |  | ✓ |
| `gpu_opcodes` | ✓ |  |  |
| `gpu_ring_stats` | ✓ |  |  |
| `gpu_state` | ✓ |  | ✓ |
| `gte_attribution` | ✓ |  |  |
| `gte_frame_stats` | ✓ |  |  |
| `gte_intpl_dump` | ✓ |  |  |
| `gte_latch_dump` | ✓ |  |  |
| `gte_ring_dump` | ✓ |  |  |
| `gte_state` | ✓ |  |  |
| `history` | ✓ | ✓ | ✓ |
| `hle_dump` | ✓ |  | ✓ |
| `idle_skip` | ✓ |  |  |
| `imask_trace` | ✓ |  |  |
| `input_replay_status` | ✓ |  |  |
| `input_route_append` | ✓ |  |  |
| `input_route_clear` | ✓ |  |  |
| `input_route_start` | ✓ |  |  |
| `input_route_status` | ✓ |  |  |
| `input_route_stop` | ✓ |  |  |
| `insn_freeze` | ✓ |  |  |
| `insn_freeze_snapshot` | ✓ |  |  |
| `insn_freeze_status` | ✓ |  |  |
| `insn_freeze_target` | ✓ |  |  |
| `irq_state` | ✓ |  | ✓ |
| `irqctx_ring` | ✓ |  |  |
| `kernel_bless` | ✓ |  |  |
| `latency` | ✓ |  |  |
| `load_transitions` | ✓ |  |  |
| `lockstep` | ✓ |  |  |
| `lockstep_func` | ✓ |  |  |
| `mc_status` | ✓ |  |  |
| `mdec_state` | ✓ |  |  |
| `mdec_trace` | ✓ |  |  |
| `mdec_trace_clear` | ✓ |  |  |
| `mem_words` | ✓ |  |  |
| `mmio_clear` | ✓ |  | ✓ |
| `mmio_dump` | ✓ |  | ✓ |
| `mmx6_freshfix` | ✓ |  |  |
| `native_display_ring_get` | ✓ |  |  |
| `native_last_motion_diag` | ✓ |  |  |
| `native_midpoint_diag` | ✓ |  |  |
| `native_producer_phase_diag` | ✓ |  |  |
| `native_producer_phase_items` | ✓ |  |  |
| `native_semantic_last` | ✓ |  |  |
| `native_stream_attribution` | ✓ |  |  |
| `native_stream_diag` | ✓ |  |  |
| `native_wave_diag` | ✓ |  |  |
| `ot_frame_dump` | ✓ |  |  |
| `overlay_candidates` | ✓ |  |  |
| `overlay_capture_dump` | ✓ |  |  |
| `overlay_capture_state` | ✓ |  |  |
| `overlay_cps_probe` | ✓ |  |  |
| `overlay_diff_off` | ✓ |  |  |
| `overlay_diff_on` | ✓ |  |  |
| `overlay_dump` | ✓ |  |  |
| `overlay_force_capture` | ✓ |  |  |
| `overlay_fp_dump` | ✓ |  |  |
| `overlay_irq_ratelimit` | ✓ |  |  |
| `overlay_irq_suppress_off` | ✓ |  |  |
| `overlay_irq_suppress_on` | ✓ |  |  |
| `overlay_loader_status` | ✓ |  |  |
| `overlay_native_block` | ✓ |  |  |
| `overlay_native_event_granularity` | ✓ |  |  |
| `overlay_native_off` | ✓ |  |  |
| `overlay_native_on` | ✓ |  |  |
| `overlay_native_ring` | ✓ |  |  |
| `overlay_rescan` | ✓ |  |  |
| `overlay_shadow_detail` | ✓ |  |  |
| `overlay_shadow_dump` | ✓ |  |  |
| `overlay_state` | ✓ |  | ✓ |
| `overlay_toggle` | ✓ |  | ✓ |
| `overlay_widget_action` | ✓ |  | ✓ |
| `pace_state` | ✓ |  |  |
| `pad_status` | ✓ | ✓ |  |
| `parity_ctl` | ✓ | ✓ |  |
| `parity_dump` | ✓ | ✓ |  |
| `pause` | ✓ |  | ✓ |
| `pc_probe_arm` | ✓ |  |  |
| `pc_probe_clear` | ✓ |  |  |
| `pc_probe_dump` | ✓ |  |  |
| `pgxp` | ✓ |  |  |
| `phase_hot` | ✓ |  |  |
| `phase_profile` | ✓ |  |  |
| `ping` | ✓ | ✓ | ✓ |
| `present_ring` | ✓ |  |  |
| `press` | ✓ | ✓ |  |
| `probe_clear` | ✓ |  |  |
| `probe_trace` | ✓ |  |  |
| `quit` | ✓ |  | ✓ |
| `ra_load_watch` | ✓ |  |  |
| `read_frame_ram` | ✓ | ✓ | ✓ |
| `read_ram` | ✓ | ✓ | ✓ |
| `record_frame` | ✓ |  |  |
| `record_frame_dump` | ✓ |  |  |
| `record_reads_dump` | ✓ |  |  |
| `restore_trace` | ✓ |  |  |
| `restore_trace_clear` | ✓ |  |  |
| `restore_trace_window` | ✓ |  |  |
| `rtrace_arm` | ✓ | ✓ |  |
| `rtrace_clear` | ✓ |  |  |
| `rtrace_disarm` |  | ✓ |  |
| `rtrace_disarm_all` |  | ✓ |  |
| `rtrace_dump` | ✓ | ✓ |  |
| `rtrace_ranges` | ✓ | ✓ |  |
| `rtrace_reset` |  | ✓ |  |
| `rtrace_stats` | ✓ | ✓ |  |
| `run_to_frame` | ✓ |  | ✓ |
| `s3_smear_watch` | ✓ |  | ✓ |
| `savestate` | ✓ |  |  |
| `screenshot` | ✓ | ✓ | ✓ |
| `screenshot_file` | ✓ | ✓ | ✓ |
| `screenshot_hires` | ✓ |  | ✓ |
| `set_input` | ✓ | ✓ | ✓ |
| `set_snapshot` | ✓ | ✓ | ✓ |
| `sio_arm_audit` | ✓ |  |  |
| `sio_burst_stats` | ✓ |  |  |
| `sio_ctrl_reg_clear` | ✓ |  |  |
| `sio_ctrl_reg_trace` | ✓ |  |  |
| `sio_ctrl_reg_window` | ✓ |  |  |
| `sio_irq_dump` | ✓ |  |  |
| `sio_irq_window` | ✓ |  |  |
| `sio_pc_trace` | ✓ |  |  |
| `sio_pc_window` | ✓ |  |  |
| `sio_state` | ✓ |  | ✓ |
| `sio_trace` | ✓ | ✓ |  |
| `sio_trace_reset` |  | ✓ |  |
| `sio_trace_window` | ✓ |  |  |
| `sio_write_window` |  | ✓ |  |
| `sp_ring` | ✓ |  |  |
| `spu_events` | ✓ | ✓ |  |
| `spu_events_reset` | ✓ |  |  |
| `spu_ram` | ✓ |  |  |
| `spu_status` | ✓ |  |  |
| `spu_voices` | ✓ | ✓ |  |
| `sreg_trace_clear` | ✓ |  |  |
| `sreg_trace_dump` | ✓ |  |  |
| `sreg_trace_find` | ✓ |  |  |
| `sreg_trace_stats` | ✓ |  |  |
| `stack_profile` | ✓ |  |  |
| `starv_ring` | ✓ |  |  |
| `step` | ✓ |  | ✓ |
| `synth_recurse` | ✓ |  |  |
| `thread_ctx_ring` | ✓ |  |  |
| `thread_trace` | ✓ |  |  |
| `thread_trace_clear` | ✓ |  |  |
| `timers_state` | ✓ |  |  |
| `turbo` | ✓ |  | ✓ |
| `turbo_audio_sink` | ✓ |  |  |
| `turbo_loads` | ✓ |  | ✓ |
| `turbo_state` | ✓ |  | ✓ |
| `unknown_dispatch_log` | ✓ |  |  |
| `unwatch` | ✓ |  | ✓ |
| `vblank_rate` | ✓ |  |  |
| `vk_perf` | ✓ |  |  |
| `vram_peek` | ✓ | ✓ | ✓ |
| `vsync_query_hle` | ✓ |  |  |
| `warm_cd_route` | ✓ |  |  |
| `watch` | ✓ |  | ✓ |
| `wide_full` | ✓ |  |  |
| `wide_shot` | ✓ |  | ✓ |
| `window_shot` | ✓ |  | ✓ |
| `write_ram` | ✓ |  | ✓ |
| `ws_aspect` | ✓ |  |  |
| `ws_aspect_cone_site` | ✓ |  |  |
| `ws_aspect_get` | ✓ |  |  |
| `ws_backdrop_margin` | ✓ |  |  |
| `ws_backdrop_ring` | ✓ |  |  |
| `ws_backdrop_stretch` | ✓ |  |  |
| `ws_census` | ✓ |  |  |
| `ws_dbg_stretch` | ✓ |  |  |
| `ws_dome` | ✓ |  |  |
| `ws_dome_probe` | ✓ |  |  |
| `ws_far_threshold` | ✓ |  |  |
| `ws_hud_mode` | ✓ |  |  |
| `ws_margin` | ✓ |  |  |
| `ws_nw` | ✓ |  |  |
| `wtrace_add` | ✓ |  |  |
| `wtrace_all_dump` | ✓ | ✓ |  |
| `wtrace_all_reset` | ✓ | ✓ |  |
| `wtrace_all_stats` | ✓ | ✓ |  |
| `wtrace_arm` | ✓ | ✓ |  |
| `wtrace_boot_dump` | ✓ |  |  |
| `wtrace_boot_reset` | ✓ |  |  |
| `wtrace_boot_stats` | ✓ |  |  |
| `wtrace_boot_summary` | ✓ |  |  |
| `wtrace_clear` | ✓ |  | ✓ |
| `wtrace_del` | ✓ |  |  |
| `wtrace_disarm` | ✓ | ✓ |  |
| `wtrace_disarm_all` | ✓ | ✓ |  |
| `wtrace_dump` | ✓ | ✓ | ✓ |
| `wtrace_range` | ✓ |  | ✓ |
| `wtrace_ranges` | ✓ | ✓ |  |
| `wtrace_reset` | ✓ | ✓ |  |
| `wtrace_stats` | ✓ | ✓ |  |
| `wtrace_trans_dump` | ✓ |  |  |
| `wtrace_trans_reset` | ✓ |  |  |
| `wtrace_trans_stats` | ✓ |  |  |
| `xg_projected_state` | ✓ |  |  |
| `xlate` | ✓ |  |  |
| `xprobe` | ✓ |  |  |
| `xprobe_arm` | ✓ |  |  |

<!-- END AUTOGENERATED COMMAND INDEX -->
