# TCP Debug Server Commands

Protocol: **JSON over newline**, one object per line, responses on same connection.

- Request shape: `{"id": N, "cmd": "<command>", ...params}`
- Success: `{"id": N, "ok": true, ...data}`
- Failure: `{"id": N, "ok": false, "error": "<msg>"}`

There are **two** servers, both implementing this protocol with overlapping command sets:

| Server | Port | Source |
|---|---|---|
| **Native** (our recompiled runtime) | `4370` | `runtime/src/debug_server.c` |
| **DuckStation** (oracle) | `4371` | `duckstation/src/core/psxrecomp_debug_server.cpp` (patched, see `tools/duckstation/psxrecomp_oracle.patch`) |

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
| `ping` / `frame` | ✓ | ✓ | — | Heartbeat + current frame number |
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
| `pause` | ✓ | ✓ | — | Pause emulation |
| `continue` (`c`) | ✓ | ✓ | — | Resume emulation |
| `step` | ✓ | ✓ | `[count]` | Step N frames (default 1) |
| `run_to_frame` | ✓ | ✓ | `frame` | Run until frame number, then pause |
| `history` | ✓ | ✓ | — | Ring buffer stats (frames available) |
| `get_frame` | ✓ | ✓ | `frame` | Full frame record from ring buffer |
| `frame_range` | ✓ | ✓ | `start`, `end` | Range query, max 200 frames |
| `frame_timeseries` | ✓ | ✓ | `start`, `end` | Compact timeseries, max 200 frames |
| `set_snapshot` | ✓ | ✓ | `slot`, `addr`, `size` | Configure per-frame RAM snapshot region (slots 0-3) |
| `get_snapshots` | ✓ | ✓ | — | Show snapshot config |
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
| `quit` | ✓ |   | — | Shutdown native runtime |
| `overlay_toggle` | ✓ |   | — | **Native:** flip the in-game developer debug overlay visibility flag (Debug builds only). Same purpose as the Ctrl+F3 hotkey — exists so tests can drive the flag without key injection. On Release builds the API is a static-inline no-op, so the call is harmless and the response always reports `visible:false`. Response: `{"id":N,"ok":true,"visible":true\|false}` |
| `overlay_widget_action` | ✓ |   | `name`, `value`, `value2` | **Native:** invoke one of the in-window debug overlay widget's action functions (the same code path a checkbox / button / slider click would call). Lets a remote client assert the TOGGLES section is wired to the real runtime setters without synthesizing real mouse clicks (which is impossible over TCP). Does NOT bypass the action path — it calls the same function the widget calls. `name` is one of: `texfilter`, `native_wide`, `aspect_set`, `bd_stretch_on`, `bd_stretch_pct`, `interp`, `supersampling`, `antialiasing`, `screen_model`, `turbo_loads`, `spu_hq`, `window_width`, `dump_event_ring`, `dump_latency_ring`, `dump_starv_ring`, `teleport`, `party_slot`, `party_bitfield`, `gold`, `write_var`, `force_battle`, `camera_write`, `event_jump`, `read_field_id`. `value` and `value2` are interpreted per the name; see the per-name section below. `dump_*` actions ignore both. Response: `{"id":N,"ok":true,"name":"...","value":N,"value2":M}` on success, `{"id":N,"ok":false,"err":"unknown name"}` on bad name. **Debug builds only; Release compiles the command to a static-inline `-1` return.** |

¹ Native `vram_peek` is the legacy name; DS calls it `read_vram`. Same semantics.  
² The `pc_*` family is specific to the DS oracle: DuckStation's CPU core honours `CPU::AddBreakpointWithCallback`, while our native runtime dispatches whole recompiled functions (no mid-function PC breaks).

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

---

## Rule when the server can't answer your question

If an inspection need isn't covered by the existing commands, **do not fall back to printf or log files**. Instead:

1. Add a handler in `runtime/src/debug_server.c` (native)
2. Add the matching handler in `duckstation/src/core/psxrecomp_debug_server.cpp` (DS), regenerate the patch via the instructions in `tools/duckstation/README.md`
3. Keep field names parallel between the two
4. Update this file

The TCP server is the canonical instrumentation surface. Rule 3 in `CLAUDE.md` is absolute: **no `fprintf(stderr, …)` in source code, ever, for any reason**.
