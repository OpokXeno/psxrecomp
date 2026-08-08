# Native Render P13 UI/2D Census

Status: investigation in progress. No UI/2D production cutover is authorized
by this document.

## Evidence

The resident executable `slus_006.64` and the authenticated field overlay
`field5_build_dbg.bin` were inspected with Ghidra.

- `field5_build_dbg.bin` is loaded at `0x8006F000`.
- `FUN_8007554C` (`0x8007554C`) is a field presentation pass. It calls
  `FUN_800805F4`, `FUN_8008004C`, and `DrawOTag` at `0x800758C8`.
- `FUN_80075910` (`0x80075910`) is a second field presentation pass. It calls
  the same `FUN_800805F4` and `FUN_8008004C`, then calls `DrawOTag` at
  `0x800759CC`.
- `FUN_8008004C` (`0x8008004C`) iterates the four field text-box slots. For
  each active slot it calls `FUN_80034888`, links the slot's packet list,
  calls `FUN_8007E1C0`, and calls `FUN_8007DCF8`.
- `FUN_8007E1C0` (`0x8007E1C0`) updates screen-space geometry and links the
  text-box background, eight border sprites, continue arrow, cursor, and
  portrait packets into the ordering table.
- `FUN_8007EE0C` (`0x8007EE0C`) initializes the text-box primitive templates.
  The decomp source has the matching `FieldTextBoxInitializePrimitives`
  layout in `field/dialogue/text_box_render.c`.
- Resident `FUN_80034888` (`0x80034888`) is the shared string-entry renderer
  from `system/system.c`. It mutates string-entry state and emits additional
  packets through `AddPrim`; it is not the Kernel Menu renderer.
- The earlier candidate `0x8003700C`/`0x800370DC` remains rejected for P13.
  Its callers and source match `KernelMenuUpdate`/font debug output, and the
  field overlay has no direct references to those font entry points.

## Primitive Contract

The source-backed text-box initialization identifies these packet classes:

- Flat semi-transparent background tiles.
- Textured sprites for borders, cursor, and continue arrow.
- Textured `POLY_FT4` portrait packets.
- Resident string-entry and letter packets linked by `AddPrim`.

The existing Native IR can represent these as screen-space triangles, but a
production path must preserve the actual OT order, packet addresses, draw
mode/environment state, CLUT/tpage values, and VRAM mutation serial.

The representation is sufficient in principle:

- `XgRenderIrNativePrimitive` carries up to two ordered triangles, which covers
  a TILE, SPRT, or FT4 packet after quad splitting.
- `XgRenderIrMaterialState` carries tpage/depth/blend, CLUT, draw area/offset,
  texture window, mask, and dither state.
- `xg_render_ir_append_native_insertion` carries the packet address and OT
  bucket, while `guest_render_native_stream` consumes the semantic primitive
  at that exact GP0 source address.

The missing adapter is therefore not a new UI scene model. It is a packet-level
translation boundary that must preserve PS1 rectangle sampling and the
environment state between linked OT nodes. The existing world quad builder is
not enough by itself because it has no packet opcode, source address, or
per-command GPU environment input.

## Runtime Census

The read-only capture tool is `tools/native_render_p13_census.py`. It arms the
function-entry ring, runs the replay through the TCP server, and records the
GP0 ring without changing render mode or guest state. The GP0 ring now records
the effective environment beside each command: draw area/offset, texture page,
texture window, blend mode, dither/mask state, and packet-local CLUT/tpage
attributes.

Capture `/tmp/opencode/route32-user-capture.toml` reached the checkpoint at
frame 1858. The first post-checkpoint draw was frame 1859 with 119 GP0
commands: 60 `0x48` polyline commands, 5 `0x2C` FT4 commands, 28 `0x2D`
commands, 6 `0x2E` commands, and environment/copy commands. It contained no
TILE or SPRT packets. The exact text-box function ranges had zero function-ring
hits in this window, so this is field/map presentation evidence, not dialogue
evidence. The GP0 provenance was the resident GPU leaf (`func=0x00000F40`,
`pc=0x8004662C`, `ra=0x800467A0`) and does not identify the `DrawOTag` caller.

The separate Field 5 recording `/tmp/opencode/xg-field5-user-record-20260728-r6/field5.toml`
reached the checkpoint only at frame 2904, with no GP0 commands in the final
captured frames. It therefore cannot prove the dialogue pass either; a new
recording or an extended replay tail was required. It is superseded by the
interactive retail-BIOS replay below.

### Interactive retail-BIOS replay

The complete replay `/tmp/opencode/xg-field5-dialogue-menu-20260803.toml`
uses the retail BIOS and covers the initial menu, Field 5 traversal, dialogue,
and menu/submenu input. Its checkpoint is Field 5 at frame 1937 and its full
budget is 4906 VBlanks. The runtime receipt reports 4906 guest VBlank
callbacks, 142 Cross entries, and no prohibited API calls.

Two read-only censuses were taken from that replay:

- `/tmp/opencode/p13-menu-census-20260803.json` captures frame 3887 with 14
  GP0 entries, including a `0x62` TILE command. The function ring records 19
  `FUN_80034888` entries across frames 3809-3845.
- `/tmp/opencode/p13-dialogue-census-v2-20260803.json` captures frame 3810
  with 326 GP0 entries: 32 `0x25`, 27 `0x2C`, 197 `0x2D`, 5 `0x2E`, 8 `0x2F`,
  one `0x62`, five `0x65`, and eight `0x66` commands, plus GPU environment and
  copy commands. This run's function ring records 39 `FUN_80034888` entries
  across frames 3737-3813 with arguments alternating between
  `(0x800C26B0, 0x800BA570, 0, 0x801196C8)` and
  `(0x800C26B0, 0x800C2664, 1, 0x801196C8)`.

Every observed string-entry call has `ra=0x8008044C`. This is the return
address after the `jal FUN_80034888` inside `FUN_8008004C`, not a separate
renderer function entry. Therefore the zero `fn_entry_dump` count for
`FieldTextBoxRender` does not disprove runtime execution of that producer;
the census now records this relationship as
`FieldTextBoxRender.call_SystemStringEntryRender`.

A focused late window, `/tmp/opencode/p13-submenu-census-v2-20260803.json`,
captures frames 4029-4035. It is stable at 111 GP0 commands per frame with
64 `0x2D`, 24 `0x2E`, one `0x2A`, one `0x2C`, and two `0x3A` commands, plus
environment commands. It has no `FUN_80034888` entries and no target function
entries. The input replay places this window after the menu interaction, but
the current trace does not provide enough function attribution to label it as
a specific submenu renderer; it remains a UI packet candidate, not proof of a
separate producer family.

This proves active text-box/string work and packet-level runtime activity, but
not yet complete OT-head capture, packet-address coverage, or Native authority.
No P13 production cutover is authorized.

### Native authority replay

The rebuilt cold-overlay runtime now reports the exact `DrawOTag` observations
from both authenticated sites. Matrix receipt
`/tmp/opencode/p13-dialogue-native-v17-matrix.json` records 847 OT preparations,
3,306,042 walked nodes, 219,988 semantic candidates, and 219,988 staged UI
packets. The Native row consumes 225,482 of 225,502 staged commands, has zero
stage failures and zero Original draws, and records the OT, packet, semantic,
GPU-environment, and VRAM-serial digests. The cold interpreter relevance filter
handles `CAPTURE`/`INTERNAL_OBSERVATION` as the same ABI value and admits only
the exact `DrawOTag` sites `0x800758C8` and `0x800759CC`.

This is runtime authority evidence for the exercised dialogue/menu replay, but
the receipt intentionally publishes metadata and digests rather than raw packet
payloads. It does not authorize a production manifest cutover or claim full P13
coverage.

## Blocking Proof

Static decompilation, the interactive census, and the Native authority replay
now prove the productive producer family and the exact OT adapter route are
active at runtime. The remaining cutover proof is:

- Visual/VRAM equivalence against a baseline replay for the full covered
  dialogue/menu window, including the Native OT adapter output.
- Persisted packet-level evidence sufficient to audit the metadata digests when
  a production manifest entry is proposed.
- Coverage accounting for other 2D producers and FMV, which are outside this
  dialogue/menu proof.

Until that proof exists, do not add `0x8008004C`, `0x80075910`, or
`DrawOTag` to the production Native manifest and do not claim P13 complete.

## Next Investigation

Use the v17 replay receipt as the route-level authority artifact. Next add the
baseline/Native visual and VRAM comparison for this same window, then extend
coverage to other 2D producers and a separate MDEC/FMV proof replay. Keep the
manifest and production cutover fail-closed until those independent comparisons
pass.
