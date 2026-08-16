# Faithful Timing Core — Game Plan (psxrecomp)

**READ THIS EACH SESSION.** Referenced from CLAUDE.md Rule -1 and from the
auto-memory ([[psxrecomp-build-faithful-core-not-hacks]],
[[precise_irq_slice_state]]). This is the authoritative plan; update the
"Status / Log" section every session.

---

## 0. North star + guardrails (non-negotiable)

Build the **faithful hardware-timing core** of the static recompiler. The PSX
recompiler is being BUILT, not preserved:

- The correct fix is ALWAYS the faithful, class-level core — NEVER a surgical
  per-game patch, symptom workaround, `game.toml` hack, or "make native agree
  with interp even if both are fake."
- Breaking other titles is acceptable; they were built on a faulty ecosystem and
  will be **regenerated**. Backward-compat is NOT a constraint.
- No stubs, no HLE, no interpreter-as-fallback (Architecture A locked). Fix the
  recompiler/runtime and regenerate; never edit `generated/*.c`.
- Don't guess (PRINCIPLES). Confirm every mechanism with the oracle + rings
  BEFORE changing code. Build observability first.
- Confer with ChatGPT via the **Chrome MCP browser at chatgpt.com** — the
  existing "PSX Static Recompiler Debug" chat (the user has Plus logged in).
  NOT the `codex` CLI (usage-limited).

## 1. The problem (diagnosis, confirmed)

A static recompiler charges cycles **block-granular** (instruction count up front
at each block leader) and checks IRQs at **block edges**. Real HW and the
sanctioned dirty-RAM interpreter have a **per-instruction** cycle timeline and
take IRQs at the exact instruction. Games that read timers / poll IRQ-driven
flags in tight loops fork between backends.

Tomba 2 (SCUS-94454) logo→FMV stall is the canonical case. The cascade:
1. Timer1 debounce value-fork @ pc 0x8008592C (frame 1823) — fixed by exact block
   cycle costs ("Fix A", already in tree).
2. **CURRENT BLOCKER:** measured **−8 cycle drift** (native BEHIND interp),
   entering in the BIOS→overlay init transition (func 0x80050B0C subtree). The
   frame-1824 logo-delay wait loop (caller 0x8008AE48 → RCnt reader 0x80085900;
   exits when *0x80102748[=960] < elapsed Timer1) loops ~1557× in interp (reaches
   FMV) vs ~42× native (stuck on logo).
   **Mechanism (located in code_generator.cpp):** when a branch's delay slot is
   ALSO a block leader (`exit_has_delay && !delay_slot_in_block`, ~line 1243), the
   branch block's `instruction_count` excludes the delay slot; on the TAKEN path
   the delay-slot clone runs but its cycle is charged by neither the branch block
   nor the (unentered) delay-slot block → undercount 1/site. ~8 sites = −8.
3. Interrupt take-point granularity (block-edge vs exact instruction) — the
   "precise IRQ slicing" track. PARKED (default off); it is a later correctness
   upgrade, NOT the FMV blocker. Validated design exists (block-leader
   continuations); see §5.

## 2. The target architecture (what "faithful core" means)

Per ChatGPT (validated) + standard practice:
- ONE shared **per-instruction cycle-cost function** `psx_instr_base_cycles(pc,
  insn)` used by BOTH the dirty-interp and the recompiler. No two approximate
  models.
- Recompiler emits **exact** accumulated cycle charges (collapses to a constant
  per pure-compute block); every dynamically executed instruction charged exactly
  once; delay slots owned by the branch bundle.
- **Segmented charge** before any guest-visible time observation (MMIO read/write
  to timers/GPUSTAT/SPUSTAT/DMA/CD/I_STAT/I_MASK, BIOS/device calls, backedges,
  calls/returns) so native and interp observe devices at the same architectural
  boundary.
- **Timers derived on-demand** from a global guest-cycle counter at read time;
  DMA/CD/GPU/IRQ on **scheduled event deadlines** (not per-cycle ticking) so
  compiled stays fast.
- Invariant: *every execution backend may differ in host implementation, but not
  in guest-visible time.* At same-PC convergence points, native cycle total ==
  interp cycle total.
- pc=0 means ONLY a real guest pc=0 / explicit termination — NEVER "dispatcher
  couldn't re-enter." Fail closed + log on undispatchable PCs.

## 3. Phased plan (each phase: confirm → build → regen → run → measure → screenshot)

- **P1 — Cycle-audit observability.** Add a per-function/at-convergence cycle
  audit: record native vs interp cumulative guest cycles at same-PC points; expose
  via TCP/ring. SUCCESS: reproduces the flat −8 and pinpoints the entering site(s).
- **P2 — Delay-slot cycle ownership (the −8).** Fix in code_generator.cpp: branch
  bundle charges its delay slot; not-taken fallthrough → branch_pc+8 (not the
  delay-slot leader); delay-slot-as-standalone-leader charges itself; no
  double-count on the not-taken path. SUCCESS: audit shows −8 → 0; Tomba 2 reaches
  the intro FMV (screenshot). Likely the FMV unblock.
- **P3 — Shared per-instruction cost function.** Single `psx_instr_base_cycles`
  consumed by both backends; recompiler emits exact accumulated charges with
  MMIO/boundary segmentation. SUCCESS: first-divergence hashes identical past frame
  1824 across a longer run; audit stays 0.
- **P4 — On-demand timers + event deadlines.** Timer1/2/0 computed from global
  cycle counter at read; devices on scheduled deadlines. SUCCESS: no perf
  regression; timing-sensitive paths stable.
- **P5 — Precise take-points (fold in parked work).** Re-enable slicing; emit
  EVERY block leader as a CPS continuation (global dispatch → owning func w/
  cpu->pc; never a new entry; fail-closed on undispatchable). SUCCESS: exact-
  instruction IRQ delivery with bounded (one-block) hand-back.
- **P6 — Regression + faithfulness.** Regen + screenshot-smoke ALL titles (BIOS,
  Tomba 1, MMX6, Ape, Tomba 2); delete Tomba2 `overlay_native_block` (must still
  reach FMV/title); calibrate the shared model against Beetle/psx-spx. Pin bump is
  user-gated.

## 3b. Cycle-cost model SOURCE (no clean-room needed — transcribe + verify)

The HW-intended cycle model is documented AND available as reference source we
already have in-tree (our oracle's own code). Stage-2 = transcribe the NUMBERS
(facts, not GPL-protected expression; also in psx-spx) into OUR shared cost
function, then VERIFY each against Beetle at runtime. Do NOT paste Beetle code
(architecture differs + GPLv2 hygiene); write our own informed by the facts.

Extraction map — `psxrecomp/beetle-psx/mednafen/psx/` (main checkout):
- **CPU base / instruction fetch:** cpu.cpp `ReadInstruction()` (~L534) — icache
  model: `timestamp += 4` cache-disabled (0xA000_0000+), `+3` on cache miss/fill,
  `+1` per fill word, near-0 on hit. For a static recompiler this becomes a
  per-block fetch-cost constant (assume cache-enabled steady state; calibrate).
- **Memory wait-states:** cpu.cpp `ReadMemory()` (~L365) / `WriteMemory()` (~L454)
  — `timestamp += (ReadFudge>>4)&2`, the `lts` delta from `PSX_MemRead*` is the
  region wait-state; `LDAbsorb = lts - timestamp` is the load-delay absorb. Charge
  in OUR psx_read/write path by region (RAM fast, BIOS ROM slow, scratchpad fast,
  MMIO per-device). Split clean from CPU base (don't double-count).
- **Mult/Div latency:** cpu.cpp `MULT_Tab24` (~L101), `muldiv_ts_done` (~L154) —
  mult/div set a completion timestamp; a later MFHI/MFLO stalls until then. Model
  as a documented latency (mult ~6-13 by operand magnitude via MULT_Tab; div/divu
  ~36). Encode as instruction cost + optional stall-on-read.
- **GTE/COP2 per-command cycles:** gte.cpp `GTE_Instruction()` (L1713) returns the
  count via each op fn (DPCS/MVMVA/NCDS/…). Well-known table (also psx-spx):
  RTPS=15 RTPT=23 MVMVA=8 SQR=5 OP=6 AVSZ3=5 AVSZ4=6 NCLIP=8 NCDS=19 NCDT=44
  NCCS=17 NCCT=39 NCS=14 NCT=30 CC=11 CCS? CDP=13 DPCS=8 DPCT=17 DCPL=8 INTPL=8
  GPF=5 GPL=5 (verify each against gte.cpp op-fn returns + psx-spx before use).
- **Timers (already partly faithful):** timer.cpp — divider ratios already used
  (T1 hblank ÷2146 etc.); move to on-demand counter = f(global cycles) at read.

Build order for the model (P3 → Stage-2):
1. Shared header (single source of truth) consumed by interp (runtime) AND
   recompiler (it already includes ../../runtime/include/*.h): identity first
   (cost=1) → regen → prove byte-identical generated cycle charges (zero behaviour
   change) → seam established.
2. Fill real costs from the extraction map, ONE component at a time, each verified
   against Beetle at runtime (native cumulative cycles == Beetle at convergence).
3. Memory wait-states in the psx_read/write path (region table).
DO each transcription with the Beetle source open + a runtime cross-check; a wrong
cycle number CREATES divergence, so verify, don't rush.

## 3c. STAGE 2 — full hardware cycle accuracy (the goal; -8 is DONE/past)

The -8 was backend-disagreement (native vs our interp); FIXED (FMV reached). Stage 2
makes the cycle model match REAL R3000A timing, validated against Beetle. We are NOT
hardware-cycle-accurate yet: model is ~1 cycle/instruction; Beetle charges ~2x.

### The validation breakthrough: DELTA comparison (offset-independent)
Absolute-cycle comparison through boot is meaningless (native is ~121M cycles off
Beetle due to turbo-loads/overlay load-model differences). BUT the cyc_watch
comparator's per-hit DELTAS cancel that offset: between two consecutive hits of the
same anchor (one iteration of identical code), native charged 46 cycles vs Beetle 91
(@0x80017FC4). That ~2x gap IS the cycle-model inaccuracy, measured cleanly. So:
  VALIDATE STAGE 2 BY MATCHING native Δcycles == Beetle Δcycles over identical
  regions (consecutive same-anchor hits, or entry/exit anchor pairs), NOT absolute.
First concrete target: make the 0x80017FC4 inter-hit Δ 46 -> 91 (== Beetle).

### Stage-2 progress log
- #1a data-load cost DONE (2ef47bd): psx_instr_base_cycles +2 per CPU load (LWC2 +1).
  Δ gate @0x80017FC4: native per-iter 46 -> 56 (Beetle 91). FMV still streams (no
  regression). Closed ~10/45. Approximation: no scratchpad-free / region / load-delay
  ABSORB yet — those are refinements (absorb would LOWER native, so it's not the
  remaining 35; the remaining gap is other components below).
- REMAINING ~35 cyc: DISASSEMBLED func_80017FC4 — it is only loads/stores/ALU/branches
  + a countdown delay loop; NO mult/div/GTE/MMIO. So the gap is NOT those, for this fn.
  BUT func_80017FC4 exits via a CPS TAIL-CALL to 0x8001EFFC (no normal return), so the
  single-anchor entry-to-next-entry window SPANS MULTIPLE functions (80017FC4 ->
  8001EFFC -> ... -> re-call). => single-anchor Δ is TOO COARSE for per-component
  attribution; the 56/91 covers code we haven't disassembled.
- TOOLING NEXT (before more cost components): add a TWO-ANCHOR region mode to cyc_watch
  (capture cycles at region START anchor A and END anchor B; report Δ(B−A) per pass) on
  BOTH backends. Then validate the cost model on a KNOWN, fully-disassembled single
  code path (no calls/loops crossing out) — e.g. a leaf function entry→its terminator.
  That gives rigorous per-component attribution instead of an opaque multi-fn window.
  Only then resume adding components (fetch / mult-div-stall / GTE / load-absorb).

### Components to transcribe (from in-tree Beetle + psx-spx; verify each by Δ)
The ~2x gap is dominated by what 1/insn ignores. Implement one at a time, re-measure Δ:
1. **Memory access wait-states (biggest lever).** Real loads/stores cost >1 cycle by
   region (RAM/BIOS-ROM/scratchpad/MMIO). Beetle: cpu.cpp ReadMemory `lts` delta +
   LDAbsorb (load-delay). Charge in the load/store path: interp exec_one's mem ops AND
   the recompiler-emitted cpu->read/write (or a per-load/store charge). Region table.
2. **Instruction fetch / I-cache timing.** Beetle ReadInstruction (+1 hit / +fill on
   miss). For the recompiler, fold a per-block fetch-cost constant.
3. **Mult/Div latency.** Beetle MULT_Tab/muldiv_ts_done: mult ~6-13, div ~36, stall on
   HI/LO read. Encode in psx_instr_base_cycles (+ optional stall-on-read).
4. **GTE/COP2 per-command.** Beetle gte.cpp GTE_Instruction table (RTPS=15, NCDS=19,
   NCDT=44, ...). Encode in psx_instr_base_cycles for COP2 ops.
All land in the single-source psx_instr_base_cycles (opcode costs) + a memory-path
wait-state charger (address-dependent). Both backends consume the same model (seam
already in place). Each component: transcribe -> regen/build -> Δ-compare vs Beetle
on a fixed region -> next.

### Caveats
- Δ-region must be IDENTICAL code on both (a tight loop body, or a pure-compute
  function). Avoid regions that cross turbo-load / overlay / dirty boundaries.
- Relocated BIOS-shell funcs (phys 0x30000-0x5AFFF) dispatch at a different native
  phys — anchor on game-text / BIOS-ROM, or the relocated phys.
- This is a multi-component effort; do it methodically, one validated component at a
  time. The comparator (cyc_watch + cycle_compare.py) is the validation backbone.

## 4. Tooling / oracle
- Runtime TCP port 4500; Beetle oracle 4382. Always-on rings: `event_ring`,
  `wtrace_all` (write trace; `newest=1`). `freeze_check` has slice-trace + cycle
  fields. `PSX_EXIT_HALT=1` halts-and-serves at the pc=0 exit for post-mortem.
- Build runtime: `cmake --build Tomba2Recomp/build-t2 --target psx-runtime`
  (PATH=/c/msys64/mingw64/bin). Recompiler: `cmake --build
  _wt-tomba2/psxrecomp/recompiler/build-t2 --target psxrecomp-game`. Regen:
  `recompiler/build-t2/psxrecomp-game.exe --config game.toml` (rebuild tool first).
- Reference: nocash psx-spx; the dirty-RAM interp is the in-process oracle for
  compiled code; Beetle is the HW oracle.

## 5. Status / Log (update every session)

- **2026-08-16 (World-map Native Smooth temporal mesh resolved and
  user-confirmed):** The remaining 4:3 water/terrain holes came from two
  independent temporal-coverage gaps rather than PS1 rasterization. Native
  Smooth now expands world-space X culling by the measured minimum 8 pixels and
  retains all four quadrants of each selected terrain tile; four deterministic
  margin trials (64/32/16/8) produced the same correction with zero packet-limit
  stops. The final linear seams were caused by source-mesh anchors being built
  but never submitted to the semantic workload, while unmatched previous
  triangles were retired at their stale previous-frame positions. Terrain now
  submits its global 145x145 vertex anchors in bounded batches; unmatched
  projective meshes generate each retired phase toward current anchors while
  retaining prior appearance, and fail closed when any current anchor is
  absent. Exact frame-3040 evidence observed 1,071,149 valid projective input
  vertices, 846,280 generated projective phase vertices, and no shared raster
  vertex conflicts. The full 4,033-VBlank replay passes at 60 FPS with 374
  midpoint presents, zero cancellations, zero GL errors, and zero packet-limit
  stops. The user first reported a 99% reduction after anchor submission and
  accepted the retired-phase result as visually correct. Focused semantic,
  OpenGL, backend, and terrain tests pass in both diagnostic and clean Release
  builds. A subsequent playable `field1 -> worldmap` transition exposed a stale
  host-history edge: a GPU operation can flush the Native queue without
  replacing its presentation history, so an otherwise valid retired candidate
  may have no matching host entry. Retirement is optional host synthesis; the
  renderer now skips only missing/mismatched candidates instead of rejecting
  the authoritative current surface. A focused three-frame OpenGL regression
  reproduces the stale-history sequence and passes without cancellation.
  Evidence:
  `/tmp/opencode/xg-worldmap-43-temporal-coverage-final-evidence.json`,
  `/tmp/opencode/xg-worldmap-43-exact-phase-evidence.json`, and
  `/tmp/opencode/xg-worldmap-43-retired-anchor-phases-evidence.json`.

- **2026-08-15 (Native vertical backbuffer rollback fixed):** A deterministic
  Load Game capture isolated the reported fast animation across the `accessing
  memory card...` text, progress bar, and red arrow. The primitives were not
  independently mis-matched: after an accepted complete-backbuffer-before-GP1
  vertical lag, Native presented the saved current once and then fell back to
  the older GP1 scanout band. Every four-VBlank source interval therefore ran
  midpoint -> future current -> old current instead of advancing monotonically.
  The present path now retains the already-promoted authored Y band until
  GP1(05h) catches up; an unrelated display origin clears the override
  fail-closed. Frames 1665-1704 previously had two distinct current source
  hashes in every four-VBlank block; all ten blocks now have exactly one
  midpoint plus three currents sharing one source hash. The complete replay
  reports PASS with 33 midpoint + 1,039 current presents, a 10,000 ms window of
  32 midpoint + 568 current presents, 33 accepted vertical lags, and zero
  cancellations, GL errors, or midpoint formula failures. The exact-frame
  visual tool now labels its PNG surface as guest VRAM pre-present so it cannot
  be mistaken for host midpoint output. Focused OpenGL/semantic regressions,
  the Release and debug builds, and 35 replay/evidence Python tests pass. The
  user confirmed the corrected load animation visually. Evidence:
  `/tmp/opencode/xg-load-blink-visual-49/manifest.json` (before),
  `/tmp/opencode/xg-load-blink-visual-50/manifest.json` (no-midpoint baseline),
  `/tmp/opencode/xg-load-blink-visual-51/manifest.json` (fixed), and
  `/tmp/opencode/xg-load-blink-replay-52-evidence.json` (complete replay).

- **2026-08-15 (Dialogue UV snap confirmed; global retirement policy
  rejected):** Unkeyed textured primitives accepted small UV and shape changes,
  so a field-dialogue line growing from 88 to 92 pixels rendered the current
  glyph footprint over intermediate-width geometry. Textured UV footprints now
  require exact equality; the user confirmed dialogue reveal is visually
  correct, while a stable-UV unkeyed translation regression retains
  interpolation. A subsequent attempt to reject every partial midpoint with an
  unmatched previous primitive was invalid: normal field workloads contain
  continual primitive churn, so the rule suppressed nearly all 30-to-60 FPS
  interpolation. That policy and its changed expectations were fully reverted;
  partial midpoint eligibility remains intact. The load-screen texture cadence
  remained unresolved at this point and is closed by the later entry above
  without changing frame-wide eligibility. Focused semantic-workload and
  OpenGL presentation regressions
  pass, the full runtime builds, and 35 replay/evidence Python tests pass.

- **2026-08-15 (Field sprite jitter regression fixed and user-confirmed):**
  Projective temporal metadata had accidentally propagated through the shared
  projected-quad helper into field-character billboards, sprite FT4s, actor
  sprites, particles, zoom quads, and minimap markers. Their authored integer
  screen positions then alternated with reprojected subpixel midpoints, producing
  the reported constant 1-2 pixel shake. Those sprite/billboard families now
  retain the established screen-space temporal path (including discrete cel/UV
  snapping); terrain, water, world models, model FT4s, and polygonal environment
  producers retain projective metadata. New sprite and integrated particle
  regressions fail on the accidental propagation and pass after the exclusion.
  The user confirmed the field-character shake is gone. One Field 5 replay
  reports PASS, correctly records zero projective phase vertices for this
  sprite-only workload, sustains a 9,996 ms peak of 299 midpoint + 298 current
  presents, and has zero formula failures, cancellations, GL errors, prohibited
  APIs, or accepted movement above 64 pixels. This also corrects the prior
  interpretation of Field 5's 62,322 projective vertices: they were billboards,
  not evidence that terrain/worldmap reprojection had executed. Evidence:
  `/tmp/opencode/xg-native-60fps-projective-sprite-stable-36/runtime-evidence.json`.

- **2026-08-15 (Native temporal projection corrected; visual confirmation
  pending):** The user confirmed that misplaced terrain, water, and model
  polygons occur during camera/player motion in both 4:3 and widescreen. That
  rules out persistent synthetic wide-margin pixels as the common cause. The
  shared fault was post-projection interpolation: the temporal workload linearly
  averaged already perspective-divided screen X/Y, which is not the projection
  of an intermediate 3D position when depth changes. Native 3D producers now
  preserve bounded pre-divide view X/Y/Z, projection distance and offsets through
  the IR/backend semantic vertex; each temporal phase interpolates those values
  and projects afterward, with endpoint reconstruction validation and the prior
  screen-space path retained for 2D or invalid/missing payloads. A deterministic
  Z=512 -> Z=1536 regression proves the old midpoint (224,184) differs from the
  reprojected midpoint (208,168). Refuted terrain-grid identities and midpoint
  winding-snap heuristics were removed. One OpenBIOS Field 5 replay reports PASS,
  62,322 projectively generated phase vertices, 757 midpoint + 3,432 current
  presents, a 9,999 ms peak of 299 midpoint + 298 current presents, zero formula
  failures, cancellations, GL errors, prohibited APIs, or accepted keyed/unkeyed
  movement above 64 pixels. Focused C tests and 35 replay/evidence tests pass.
  Do not mark the visual artifact resolved until the reported camera angles are
  tested directly. Evidence:
  `/tmp/opencode/xg-native-60fps-projective-clean-35/runtime-evidence.json`.

- **2026-08-15 (Native selective polygon motion bounded):** A Field 5 visual
  report clarified that only some terrain polygons appeared to separate and
  suggested that an individual polygon was moving to the wrong location. New
  retrospective-motion evidence confirmed the mechanism: unkeyed semantics
  admitted a maximum aggregate movement of 1,537 pixels (about 256 pixels per
  vertex) in one source frame, with 759 accepted matches averaging over 64
  pixels per vertex. Unkeyed matching now fails closed when either centroid,
  canonical per-vertex, or Native per-vertex movement exceeds 64 pixels; keyed
  producer identities remain unrestricted. The deterministic replay now has
  zero accepted unkeyed motions over 64 pixels, maximum aggregate movement 384
  pixels, 843 midpoint + 3,343 current presents, a 9,991 ms peak window with
  298 midpoint + 297 current presents, zero cancellations, zero GL errors,
  zero unsupported packets, and zero Original draws. An independent textured
  cutout regression also proved and fixed false mirrored-UV classification
  caused by truncating fractional raster coordinates. Focused renderer tests
  and 30 replay/evidence tests pass. Visual confirmation of the motion bound is
  still required; do not mark the terrain artifact resolved yet. Evidence:
  `/tmp/opencode/xg-native-60fps-motion-histogram-26/runtime-evidence.json` and
  `/tmp/opencode/xg-native-60fps-unkeyed-vertex-motion-limit-28/runtime-evidence.json`.

- **2026-08-15 (Native 30->60 vertical scanout lag):** Field 5 reset-reason
  telemetry proved that every successful Native-wide midpoint was followed by
  a `pending-view-mismatch` reset: 388/388 destructive resets differed only in
  display Y (saved current Y=0 while GP1 still named Y=224); slot, X, width,
  and height were identical. The midpoint path now treats that vertical-only
  difference as the documented complete-backbuffer-before-GP1 lag, presents
  the saved current from its authored Y, and retains semantic history. Slot,
  X, and size mismatches remain fail-closed. The OpenBIOS replay with absolute
  project memcards reports PASS, 766 accepted vertical lags, zero pending
  mismatch resets, `NO_PREVIOUS` 390->2, and a 9,992 ms moving window
  containing 295 midpoint + 294 current presents (~29.52 interpolated/s,
  ~58.95 distinct/s). Focused OpenGL and replay-tool regressions pass.
  Evidence:
  `/tmp/opencode/xg-native-60fps-vertical-lag-pass-17/runtime-evidence.json`.

- **2026-08-15 (Native coverage contract corrected):** Replaced the global
  `unbound=0` objective with an eligible-3D producer contract. Supported GP0
  draws without a producer sidecar already translate to `GpuRenderSemantic`
  and rasterize through OpenGL Native; they are packet-derived, not blocked or
  Original fallback. Runtime evidence now reports producer-bound and
  packet-derived draw totals/histograms separately while retaining the existing
  packet-coverage fields. A source audit confirmed Native-view coverage for the
  active model FT4/FT3, direct sprite, terrain, shadow, decoration, cloud, and
  world-actor paths; intentional screen-space builders remain packet-derived.
  Terrain far-depth widening remains deferred because the authenticated OT and
  `0x7FE` primitive-buffer limits make a simple cull relaxation destructive.
  The earlier active-visual resolver fix remains the measured improvement
  (`239593 -> 235060` packet-derived draws); an exact `memcpy` sidecar
  experiment produced no additional resolved draw and was removed.
  The canonical replay preserved every prior stream counter exactly and now
  classifies 622,428 draws as producer-bound and 235,060 as packet-derived,
  with zero unsupported, Original, or parser draws. It still reports `FAIL`
  solely through the pre-existing `semantic_overflow` condition. Evidence:
  `/tmp/opencode/xg-coverage-contract-1/run-1/runtime-evidence.json`.

- **2026-08-13 (Native semantic 120/240 targets):** Generalized the validated
  Native 30->60 midpoint path to rational phase sets for explicit 60, 120, and
  240 FPS targets. The semantic matcher now computes overflow-safe weighted
  phases and has direct 1/4..3/4 and 1/8..7/8 regressions. Canonical and Native-
  wide OpenGL presentation emit the causal cadence `1/4,2/4 | 3/4,current` or
  `1/8..4/8 | 5/8..7/8,current`, with one completed-swap ring event per phase
  carrying its numerator/denominator. Targets above 60 disable driver vsync and
  use main-context subframe deadlines; guest VBlank, input, audio, and the
  legacy framebuffer interpolation thread remain unchanged. Phase allocation
  is failure-atomic, host-only draws restore authoritative GPU state, stencil
  rebuild covers every active phase, and Native wave displacement metadata is
  phase-specific. `PSX_NATIVE_INTERPOLATION_FPS=60|120|240`,
  `[video] native_interpolation_fps`, the debug overlay, and
  `overlay_widget_action native_interp_fps` expose the target while
  `PSX_SMOOTH_60FPS` remains a boolean eligibility switch. The semantic and
  hidden-context OpenGL focused regressions pass; live Wayland 120/240 telemetry
  remains the next proof step.
- **2026-08-13 (Strict presented-midpoint coordinate proof):** Clarified that
  the OpenGL `A,A,B,C` regression is a missing-duplicate stress case, not the
  steady 30->60 cadence. Its expected
  `current,current,midpoint,current,current` sequence preserves B and C when C
  arrives before B's duplicate VBlank. In the real telemetry run, steady
  eligible intervals alternated `current,midpoint,current,midpoint`; there were
  3,088 transitions in each direction and zero consecutive midpoints. Added
  per-vertex midpoint diagnostics that distinguish changed endpoints, a result
  strictly different from both endpoints, integer-rounding collapse onto an
  endpoint, and failure of the effective-coordinate formula
  `(previous + current) / 2`. Added separate totals accumulated only after a
  midpoint's exact `SDL_GL_SwapWindow` returns. A synthetic regression proves a
  one-unit 16.16 delta is reported as collapsed, while the existing OpenGL
  sequence requires three strict presented midpoint vertices and zero formula
  failures. Both focused tests pass. A second visible Wayland Xenogears run,
  with no screenshots/readbacks and fresh byte-identical root-memory-card
  copies, completed 1,181 midpoint swaps. Those swaps matched 3,112,071
  vertices; 2,513,461 had different endpoint positions, and all 2,513,461 were
  strictly different from both endpoints. There were zero endpoint collapses,
  zero midpoint-formula failures, zero cancellations, and zero GL errors. The
  summed endpoint distance was 3,600,192,307,200 fixed-point units (about 21.86
  pixels per changed vertex on average). This directly proves that real
  midpoint swaps contained recalculated intermediate geometry rather than the
  same endpoint positions. Evidence:
  `.local/debug-artifacts/20260813-strict-midpoints/`.
- **2026-08-13 (Native midpoint completed-swap proof):** Rebuilt the Debug
  runtime with per-swap Native classification and a `swap_completed` marker
  written only after the exact `SDL_GL_SwapWindow` call returns. The existing
  `A,A,B,C` OpenGL regression now requires the ring sequence
  `native-current,native-current,native-midpoint,native-current,native-current`
  and requires every event to be post-swap complete; it passes. Ran one visible,
  interactive Wayland instance with `PSX_SMOOTH_60FPS=1`, Native 16:9, OpenGL
  2x, original game timing, no display ring, no GL pixel probe, no screenshots,
  and isolated byte-identical copies of the repository-root memory cards. The
  complete telemetry history contains 7,788 consecutive events with no gaps:
  4,464 `native-current`, 3,088 `native-midpoint`, 175 CPU, 59 VRAM, and 2 blank.
  Every event has `swap_completed=1`; every midpoint was preceded and followed
  by current (3,088 transitions in each direction), with no consecutive
  midpoints. The active Native interval covered 7,550 swaps in 125.974 seconds
  (59.925 Hz), 40.9% midpoint overall. It included strict alternating stretches
  of 1,081 swaps / 18.019 seconds and 1,069 swaps / 17.818 seconds at about
  59.94 Hz. The workload moved 2,271,106 primitives and changed 10,162,027
  vertex positions, with zero midpoint cancellations, zero GL errors, and final
  600-present frame-period p50/p95 of 16.6833/16.6848 ms. This proves the
  midpoint FBO was selected, composed into the default framebuffer, and its
  swap completed into the Wayland compositor path; it is not merely an internal
  interpolation counter. Wayland exposes no physical scanout feedback here, so
  the evidence deliberately does not claim that every submitted buffer reached
  the monitor scanout. Evidence: `.local/debug-artifacts/20260813-visible-midpoints/`.
- **2026-08-13 (Manual side-by-side Native interpolation A/B):** Ran two
  isolated Wayland instances of the rebuilt Debug executable with identical
  OpenGL 2x, Native 16:9, original game timing, disc, and root-repository
  memory-card contents. The control used `PSX_SMOOTH_60FPS=0`; the comparison
  used `PSX_SMOOTH_60FPS=1`. Both used separate absolute runtime-state,
  overlay-capture, debug-port, and memory-card paths; the root cards and both
  copies ended byte-identical. The user closed both normally and reported no
  perceptible FPS difference. Telemetry only (no image captures used as
  evidence) proves the control produced 8,218 current and zero midpoint
  presents, while the comparison produced 5,349 current plus 2,772 midpoint
  presents (34.1% midpoint), with 648,088 moved primitives, 2,628,089
  position-changed vertices, zero midpoint cancellations, and zero GL errors.
  Both final 600-present windows had 16.6834 ms p50; p95 was 16.6845 ms control
  and 16.6848 ms comparison. This proves midpoint selection and geometry work
  occurred, but it does NOT establish a perceptible improvement or prove that
  the final visible pixels differed. The run also exposed an observability gap:
  `gl_present_ring` serialized the canonical Native enum as `?` and classified
  every Native-wide swap as `wide`, losing current-vs-midpoint identity. The
  ring now records `native-current` and `native-midpoint` per swap for both
  canonical and Native-wide paths. The existing `A,A,B,C` OpenGL regression now
  requires the exact `current,current,midpoint,current,current` ring sequence;
  the Debug executable and focused test rebuilt, and the test passes.
- **2026-08-13 (Manual Native 30->60 visual proof):** Ran the rebuilt Debug
  executable manually with `PSX_SMOOTH_60FPS=1`, `--native-fps original`,
  `--render-mode native`, OpenGL, 16:9, and no replay. A parallel TCP observer
  sampled the existing midpoint/workload, GL-present, and latency rings until
  the user closed the game at frame 6,993, without enabling the expensive
  display ring. The final 2,048 consecutive window presents covered frames
  4,925..6,972 in 34.540 seconds (59.265 presents/s including stalls; 2,048
  distinct frame IDs), with zero GL errors. The final 600-frame latency window
  measured frame-period p50 16.683 ms and p95 16.686 ms; isolated Debug/scene
  transitions produced a 112.715 ms maximum and visible console dips as low as
  36--57 FPS, so the run was not literally locked at 60 for every instant.
  Across the full session Native produced 1,812 midpoint and 4,362 current
  presents. In the sampled motion intervals, midpoint/current counts repeatedly
  alternated 16--17/16--17 per 32--34 presents; cumulative semantic evidence
  reached 250,713 matches, 180,324 moved primitives, 791,596 position-changed
  vertices, and 662,996,189,184 fixed-point position-distance units, with zero
  midpoint cancellations. A final 1920x1080 post-compositor window capture
  confirms the observed output was the live Native-wide scene. Evidence is in
  `.local/debug-artifacts/20260813-034749-interpolation/`; memory-card hashes
  remained unchanged.
- **2026-08-13 (CPS partial-overlay stack leak fixed):** Reproduced Xenogears'
  Native-mode exit near frame 2,130 and extended the terminal report with JSON-
  safe strings, a 256-byte guest-stack window with existing last-writer
  attribution, the detailed dispatch `(target, ra, sp, cycle)` tail, and the
  existing deterministic-TCB save/restore ring. The evidence proved there was
  no thread switch: the active GCC shard exported `0x80081F80` but authenticated
  only `0x84` bytes (`0x80081F80..0x80082003`), while the real function epilogue
  lies at `0x800821D4..0x800821F0`. Its captured final block had no explicit
  control transfer, and overlay CPS codegen emitted a bare C fallthrough, leaving
  `cpu->pc=0` and the prologue's `sp -= 0x28` live. Later valid stack locals then
  overlapped older saved return slots; epilogues restored zero/small values and
  transferred control into low RAM. CPS codegen now tail-transfers to the next
  guest PC whenever a captured function ends without explicit control flow, so
  another native shard or the dirty-RAM interpreter executes the missing suffix
  and its epilogue. The focused cross-page/partial-capture regression passes;
  Debug runtime rebuilt with codegen hash `0x6bba225b`, invalidating the faulty
  cache namespace. Manual play passed the former failure point and remained at
  about 60 FPS through frame 5,212, when the user closed it. Memory-card hashes
  remained unchanged. No guest semantics, renderer behavior, or general
  interpreter fallback changed.
- **2026-08-12 (Native queue boundary and standalone-link closure):** Added
  direct OpenGL regressions for pending Native work across midpoint reset, the
  full-canonical-width copy fast path, and the exact `A,A,B,C` presentation
  sequence. The tests verify reset materializes the queued current image before
  clearing lifecycle state, copy reason 5 drains the queue before reading its
  source, and `A,A,B,C` carries one saved-current debt while C is materialized
  for the following host slot. Made the game-owned renderer's repository-root
  dependency explicit so it can be included from the standalone runtime build;
  BIOS-only runtime/oracle targets receive zeroed, fail-closed Native metadata,
  while the integrated game target retains its validated generated metadata.
  Standalone `psx-runtime`, integrated `XenogearsRecomp`, semantic workload,
  OpenGL mask/order, static-auth, terrain-water, manifest-build, and both
  repositories' `git diff --check` pass. A live in-game 16:9 visual reproduction
  remains pending; no guest timing or canonical VRAM semantics changed.
- **2026-08-12 (Native smooth performance accepted):** Continuous forensic
  display capture is now strict opt-in through `PSX_DISPLAY_RING=1`; normal
  runs avoid the two synchronous GL readbacks per VBlank, while
  `tools/native_render_field490_visual.py` explicitly enables the ring for
  exact-frame evidence. With the ring disabled, the preserved pre-matcher
  process measured `total_ms_avg=16.845`, `emu_cpu_ms_avg=16.141`,
  `prims_avg=2894`, `batches_avg=315.0`, p50 16.684 ms, and p95 17.332 ms; a
  later stable window reached p95 16.685 ms. The rebuilt process measured
  `total_ms_avg=16.842`, `emu_cpu_ms_avg=16.740`, `prims_avg=2808`,
  `batches_avg=305.7`, `cpu_flush_ms_avg=2.020`, p50 16.684 ms, and p95
  16.685 ms, with zero GL errors and retrospective budget exhaustion. The
  retrospective appearance check now rejects a candidate immediately once
  cumulative UV or color distance exceeds its existing limit; equality at
  the limit remains accepted and boundary regressions cover UV 96/97 and
  color 576/577. The first rebuilt `perf` capture inherited active
  `compile_overlays.py`, compiler, and linker children, so it is not valid for
  assigning an isolated percentage to this early exit. A later live A/B also
  found the two processes at different scene loads; the display-ring gate was
  confirmed off in GDB (the old process's `valid=64` statistic was retained
  ring contents), then overlay autocapture and its active compiler trees were
  stopped. The user accepted the end-to-end performance result and cancelled
  further isolated matcher attribution. No guest timing, canonical VRAM, or
  immediate current/midpoint upload mirror semantics changed.
- **2026-08-12 (Native resource invalidation and GL batching performance):**
  Cached authoritative artifact validation and scoped identity to binary/pair/
  provenance, so same-artifact non-render dispatches retain authority while a
  real CRC change still fails closed. Added a conservative per-RAM-word resource
  watch bitmap and replaced the remaining 4,096-slot FT4 packet/descriptor scan
  with bounded packet-start probes plus a reverse descriptor chain. Packet,
  descriptor, shared-descriptor, collision/tombstone, and propagated FT3-copy
  regressions preserve exact invalidation. Native current/midpoint semantic work
  is now queued until VRAM-coherence boundaries; canonical guest work remains
  immediate, and wave, upload/copy/fill, present/peek, transaction, and GP0(E6)
  boundaries drain the queue. In the same manual Native scene, batching reduced
  `batches_avg` from 1,619.5 to 164--183 and `cpu_flush_ms_avg` from 9.79 to
  1.08--1.24 ms. The last pre-direct-invalidation sample measured
  `total_ms_avg=18.575`, `scene_gpu_ms_avg=11.326`,
  `present_gpu_ms_avg=7.255`, p50 16.685 ms, and p95 25.650 ms; `perf` still
  attributed 14.7% to `psx_xg_render_auth_note_code_write`, specifically its
  full FT4 scan. A first bounded-probe profile reduced that symbol to 3.61% but
  left the two FT4 hash lookups at 1.16% and 1.01%; direct per-RAM-word packet
  and descriptor maps then removed all three from the final profile's >=0.5%
  report. In a heavier 3,169--3,495-primitive manual scene, the final build used
  5.474 s CPU over 8 s and measured `total_ms_avg=17.815`,
  `scene_gpu_ms_avg=12.989`, `present_gpu_ms_avg=4.848`, p50 16.685 ms, and p95
  22.513 ms, with zero midpoint cancellations and GL errors. This is valid
  hot-path evidence but not a GPU comparison to the 1,600-primitive baseline.
  Profiles: `/tmp/opencode/xg-smooth-ft4-direct.data` and
  `/tmp/opencode/xg-smooth-ft4-direct-index.data`. Static-auth, semantic
  workload, GL mask/peek/transaction, frame-pacing, telemetry guards, full
  runtime link, and `git diff --check` pass.
- **2026-08-12 (Native midpoint presentation pacing):** Live gameplay telemetry
  ruled out paired/back-to-back midpoint swaps: the main GL path alternated one
  midpoint/current present per host slot. Added position-distance diagnostics to
  the semantic matcher; a five-second gameplay sample reported 892,069 of
  958,347 matched vertices moving (93.0%) and 401,308,712,960 cumulative 16.16
  Manhattan units, proving substantial spatial interpolation rather than only
  color changes. The latency ring instead exposed catch-up pacing: p50 16.68 ms,
  p95 21.96 ms, and 6.94--27.11 ms extrema. `FramePacer` now exposes a stable
  policy used only by active smooth non-FMV presentation; late frames re-anchor
  instead of producing a short debt-repayment interval. Normal emulation and
  Beetle retain bounded debt recovery. Native pacing keys from the smooth
  request, not the intentionally quiesced legacy smooth-effective flag. The pure pacer regression test, runtime
  telemetry structural guard, semantic workload test, OpenGL Native mask/order
  integration test, and full runtime build pass.
- **2026-08-12 (Native GP0 provenance hot-path fix):** `perf record` on live
  smooth gameplay attributed 46.5% of CPU cycles to
  `debug_server_find_last_ram_writer`, reached once per GP0 packet through DMA2
  preflight. The intended O(1) writer index only recorded aligned stores, while
  Xenogears commonly assembles packet words with byte/half stores; misses fell
  through to a 131,072-entry reverse ring scan. The index now records every word
  overlapped by each store and indexed empty slots fail definitively in O(1).
- **2026-08-12 (Native retrospective 30->60 midpoint):** Added fail-closed
  producer identities and a semantic workload matcher for Native source frames;
  duplicate VBlanks now present host-only midpoint geometry while guest timing,
  input, audio, and canonical VRAM remain unchanged. Native and canonical
  midpoint targets preserve mask/order, uploads, copies, wave composition, and
  FMV suspension. Smooth presentation keeps the legacy interpolation paths
  quiescent, respects the configured internal raster scale, and preserves seeded
  Native surfaces plus wave state across live scale rebuild/rollback. A live
  slot-3 pass exposed two integration faults. First, POSIX overlay libraries
  import runtime-owned generated-code ABI symbols, but the executable did not
  export them; `ENABLE_EXPORTS` now supplies the plugin-host contract and a
  Linux CTest verifies `g_psx_resume_seed` in the dynamic symbol table. The
  rebuilt runtime registered 10 functions from 9 loaded images and reached
  `dispatch_native=77240` by VBlank 1077 (`input_replay.active=false`), replacing
  the prior 512-library preflight rejection. Second, unkeyed live gameplay
  recorded 364 primitives per source frame but found zero retrospective
  candidates because authentication `scene_generation` advanced on ordinary
  producer-frame boundaries. Interpolation now has a separate temporal scene
  namespace: auth boundaries preserve it, while relevant code mutation and
  loader mismatch advance it. The root integration set passes 12/12 and the
  standalone semantic/GL peek/transaction set passes 4/4. Live semantic capture
  without replay reached `begun=21163`, `sealed=10581`, and
  `recorded=2022197`. Manual gameplay from the project-root memory card then
  exposed two fail-closed midpoint cancellations. GDB live proved that the
  address-free matcher paired semitransparent fullscreen subtractive overlays
  across alternating framebuffer offsets; retrospective matching now excludes
  semitransparent primitives unless they carry an explicit identity. A second
  live capture proved that interpolation wrote `native_view_x/y` even when
  `native_view_position=0`, creating invalid phantom Native coordinates; those
  fields now remain untouched unless the producer explicitly authored them.
  Optional midpoint GL operations also retain cumulative operation/error
  diagnostics and cancel only the midpoint frame rather than failing the
  authoritative Native packet stream. Final no-replay live proof used
  `--memcard-dir /home/pc/xenogears-port/XenogearsRecomp` and remained healthy
  through VBlank 3639 with `I_MASK=0x24D`, `cancelled=0`, `gl_error_count=0`,
  `total_matched=47694`, `total_moved=47694`, and `midpoint_presents=99`.
- **2026-08-11 (Native-wide PR salvage):** Adapted the production-safe pieces
  of PR #2 to the producer-driven Native renderer without restoring legacy
  mirror heuristics or diagnostics. GP1(05h) real framebuffer changes now clear
  only the retiring Native surface's synthetic side columns; redundant writes
  preserve an active draw band. Axis-aligned fullscreen Native semantics now
  expand flat, Gouraud, and textured two-triangle filters to both revealed
  columns while leaving canonical geometry unchanged. The debug overlay resolves
  `glBindFramebuffer` through SDL for Windows OpenGL 3 compatibility. The
  OpenGL Native mask/order test covers margin invalidation and Gouraud/textured
  edge coverage; Release and debug-tools runtime builds pass.
- **2026-08-11 (Cross-platform live overlay toolchain):** POSIX runtimes now
  spawn overlay compilation asynchronously, retain child output in memory, and
  rescan the native cache on the emulation thread after completion. Source
  checkouts synthesize the GCC/Python command automatically. Linux and Windows
  release packaging now stages pinned Python 3.11.9 + TinyCC 0.9.27, the
  platform-native hash-matched recompiler, runtime headers, and licenses under
  `overlay_toolchain/`; the archive verifier requires that contract. Linux
  staging and relocated TCC compilation were exercised in the glibc 2.31 image;
  the Windows recompiler was cross-built and verified to import only system
  DLLs.
- **2026-08-11 (Live compile frame isolation):** Runtime-triggered overlay
  builds now force one region worker and inherit low OS priority. Successful
  shards emit a flushed publication marker, allowing POSIX and Windows runtimes
  to prepare each image off-thread and commit it on the emulation thread instead
  of waiting for the whole capture batch. Live builds discard successful/cached
  generated C while retaining failed sources for triage; in the observed
  world-map cache those sources accounted for 108 MB of 142 MB. The result
  parser now accepts its documented optional `capacity_fastpath` field. The
  assertion-enabled POSIX publication test and full Linux runtime build pass.
- **2026-08-10 (Real-time Debug execution):** A complete 4,464-VBlank
  Xenogears world-map replay with the project memcard isolated the Debug loss:
  GCC's default `-O0` produced `28--31 FPS` in the fully interpreted tail,
  `-Og` produced `41--45`, and `-O2/-O3` produced `51--58` with
  `PSX_DEBUG_TOOLS=ON`. The identical `-O3` build with tools compiled out held
  `59--61 FPS`, proving the remaining cost was dormant instrumentation rather
  than guest semantics. Debug now retains symbols, assertions, the TCP server,
  and all armable observers while compiling runtime code at `-O3`; disarmed
  cycle/lockstep and function-entry observers take an aggregate fast path.
  Follow-up `perf` passes made the temporary starvation/xprobe diagnostics
  opt-in and cached invariant native-render cutover classification. The final
  interpreted tail measures about `49--59 FPS`, depending on host load.
- **2026-08-10 (Interpreted native-observation hot path):** The complete
  4,464-VBlank Xenogears world-map replay established that fully interpreted
  execution itself sustains `59.5--60.1` FPS in original render mode, while
  native mode fell to `47.9--53.2` FPS because each interpreted instruction
  crossed the runtime/native-renderer boundary for multiple cold-auth
  relevance queries. Added one side-effect-free per-instruction classifier for
  ENTRY/CAPTURE/SOURCE/native PRE/native POST/overlay sites and threaded its
  flags through the observed decoder, preserving PRE/body/POST ordering and
  delay-slot ownership. Also gated the disabled private-capture callback and
  removed duplicate widescreen semantic lookups. The native-mode replay now
  holds `58.5--60.8` FPS in the formerly sustained slow window (frames
  `4155..4407`) and remains approximately 60 FPS through the earlier FMV and
  transitions. No cycle, IRQ, load-delay, or memory behavior changed. Debug and
  Release builds succeed; `53/54` CTests pass in both. The sole failure is the
  existing private-artifact test whose local identity input is absent
  (`identity could not read local input`). Interpreter structural perf guards
  and `git diff --check` pass.

- **2026-08-07 (Native stream family hint):** Added a bounded command-ID to
  resolver-family hint table in `native_renderer/src/xg_render_auth_runtime.c`.
  Hints are keyed by the current producer-resource generation, cleared during
  runtime reset, and fall back to the original family precedence chain when a
  cached family misses. The first implementation used a generic family-dispatch
  helper and was rejected after `perf-v7` showed that helper at `2.10%`; the
  final implementation uses direct hit dispatch and direct fallback calls.
  Release rebuilt successfully. The exact `--no-launcher` replay completed all
  `4,856` VBlanks (`4799` reported frames), retained the same evidence counts
  (`363112` staged, `316181` consumed, `1096275` Native packets, `72` opcode
  `0x40` unbound, zero unsupported), and reproduced the existing `FAIL` evidence
  caused by missing writer provenance/final `effective_render_mode=original`.
  `perf-v8.data` has `42579` samples and zero lost samples; resolver self time
  moved from roughly `1.7--1.9%` in `perf-v6` to `1.2--1.4%`. Added a bounded
  command-ID index with duplicate counting to
  `guest_render_native_stream_reserve_exact()` and centralized entry-removal
  bookkeeping so swaps, reservations, compaction, and visual abandonment keep
  the index valid. The post-index full replay (`runtime-evidence-v10.json`)
  retains the same evidence counts and reaches `4846` reported frames; combat
  still falls to `38.7--51.4` FPS. A follow-up `perf-v11.data` capture has
  `42553` samples with zero lost samples; `native_stream_resolve` is down to
  `1.12--1.24%` self overhead and `reserve_exact` is below the `0.5%` report
  threshold. The current diagnostic build passes `54/54` CTest; CMake still
  warns that BIOS generated sources are stale and that ccache is unavailable.
  The remaining material cost is the native miss/resolver path plus interpreted
  instruction work, not exact reservation lookup or evidence policy.

- **2026-08-07 (Native replay performance pass, provenance retained):** The
  visual regression was traced to the test launch using an isolated memcard
  directory; the corrected replay uses
  `--memcard-dir /home/pc/xenogears-port/XenogearsRecomp` (`card1.mcd` and
  `card2.mcd`) and has no graphical glitches. Replaced the hot reverse scan in
  `debug_server_find_last_ram_writer()` with an exact aligned main-RAM/
  scratchpad cache, retaining the bounded ring scan for non-indexable addresses.
  Then changed Native-auth resource invalidation to logical count/valid clears
  and an epoch-based FT4 table invalidation; no debug/provenance hooks were
  disabled. The same 3,200-VBlank replay retained `228,187` packets,
  `201,653` bound, `12,480` unbound, and zero unsupported packets. CTest is
  `54/54`. Valid `perf` sample duration improved from `73.56 s` (debug baseline)
  to `60.39 s` after the writer cache, `55.59 s` after logical invalidation,
  and `52.39 s` after FT4 epochs; the final run stayed at roughly `59.5–60.8`
  FPS through frame 3,175. Final evidence:
  `/tmp/opencode/xg-perf-epoch-record/runtime-evidence.json`; its `FAIL` status
  is expected because the temporary 3,200-VBlank trace does not reach the
  4,856-VBlank checkpoint. Remaining profile cost is pacing
  (`__vdso_clock_gettime`, `20.25%`), overlay write watching (`7.47%`), and
  diagnostic write tracing (`2.62%`), not the prior writer lookup or bulk
  Native-auth invalidation.

- **2026-08-07 (Release full replay):** Rebuilt `/tmp/opencode/xg-perf-release-build`
  as `RelWithDebInfo` with `PSX_DEBUG_TOOLS=OFF` and `PSX_DEBUG_OVERLAY=OFF`,
  then ran the complete `4,856`-VBlank combat trace with
  `--memcard-dir /home/pc/xenogears-port/XenogearsRecomp`; no early timeout or
  interruption was used. Replay completion is proven by `trace_index=4856/4856`,
  `checkpoint_seen_vblank=1747`, and `stop_reason=CheckpointReached`. The
  evidence is `FAIL` only because the Release build has no writer-provenance
  observer: `72` opcode-`0x40` packets remain unbound, one forced-Original
  fallback is recorded, and final `effective_render_mode=original`. The full
  Release `perf` capture has `77K` samples, `0` lost, and `119.35 s` sample
  duration. The combat slowdown is now attributed to Native miss resolution,
  not debug tooling: `native_stream_resolve.part.0` is `34.22%` inclusive,
  `guest_render_native_stream_resolve_active_miss` `6.66%`, and the FT3/field
  sprite/residual template linear searches dominate that path. FPS is near 60
  through frame ~3,500, drops to ~10–15 around frames 3,850–4,200, then stays
  below 50 through the combat tail. Evidence:
  `/tmp/opencode/xg-perf-release-full-record/runtime-evidence.json` and
  `/tmp/opencode/xg-perf-release-full-record/perf.data`.

- **2026-08-07 (Performance build correction and visual rollback):** The prior
  `build-dbg` was a true CMake `Debug` build (`-g`, no optimization). Its
  pre-rollback performance window `3800:4200` measured `131206.052 ms` wall
  time for 400 frames, with `130878.979 ms` in `guest_work`; pacer, capture,
  provider polling, and GL work were negligible by comparison. Reconfigured
  the same diagnostic-capable build as `RelWithDebInfo` (`-O2 -g -DNDEBUG`,
  `PSX_DEBUG_TOOLS=ON`, `PSX_DEBUG_OVERLAY=ON`) and rebuilt successfully. Per
  user direction, no post-rollback replay was run. The shared-binding
  deduplication and scene-epoch relaxation from the previous entry were
  rolled back after the graphical regression; exact GP0 line/polyline
  semantics and the no-gates shared fallback remain active. CTest is `54/54`.

- **2026-08-07 (Native no-gates shared-packet closure):** The renderer-auth
   strict path is the default when `PSX_OVERLAY_NO_GATES` is absent or `0`;
   setting it to `1` explicitly enables the no-gates diagnostic path. The unsafe overlay-loader
  live-code-CRC bypass remains removed, so native overlay dispatch still
  requires byte identity. The earlier v18/v19 replay reached all `4,856`
  trace states with zero unbound packets, but that result belongs to the
  deduplication variant and is historical, not the current post-rollback
  state. The diagnostic JSON still reported `auth_proof=OBSERVED`,
   `effective_render_mode=original`, and one `forced_original` telemetry
   transition; this pre-existing visual path is not being changed. Exact GP0
   line/polyline semantics and no-gates shared fallback remain active; strict
   mode keeps the original ambiguity rejection.

- **2026-08-08 (Battle fader Native capture):** The battle transition fader at
  `0x800B3878` constructs an overscanned semitransparent `POLY_F4`
  (`-32..320`, `-32..240`) and inserts it into the battle OT. Capture its
  completed packet at `0x800B393C`, before `addPrim`, and resolve it through
  the existing authenticated F4 Native stream path. This avoids the unsafe
  generic shared-packet fallback while ensuring the fader is not omitted from
  Native rendering.

- **2026-08-06 (Overlay artifact authorization generalized):** Separated the
  identity-bound resident runtime descriptor from overlay artifact authority.
  Overlay candidates now require loader-provided authority/pair provenance,
  an in-artifact function range, and a seam declared in
  `xg_render_overlay_ranges.toml`; no Field 5 scene/checkpoint is used as the
  overlay authorization condition. Added code-range invalidation for the
  authenticated overlay candidate and a focused regression test proving an
  `0x801B2000` overlay candidate is accepted without the Field 5 artifact.
  Focused CTest is 3/3 and overlay-range/runtime-variant Python tests are 9/9.
  Full replay remains open: the current offline cache registers 1,467 overlay
  candidates, so the strict packet gate is not yet evidence of complete overlay
  coverage.

- **2026-08-06 (Native widescreen packet-faithful closure, Native-only proof):**
  Completed shared Native-view projection and widened host culling across the
  compatible field/world producer families, including the remaining model FT3
  path. The OpenGL Native compositor now preserves packet-order fills, uploads,
  lines and VRAM copies, including X/Y wrap; retained surfaces reseed before
  presentation when invalidated and framebuffer bases that cross VRAM X=1023
  seed and mirror their wrapped columns correctly. An independent read-only
  subagent audit was completed before end-to-end replay; its savestate and live
  supersampling findings were explicitly left out of this scope. Focused Native
  verification passes: `gpu_gl_mask_order_test`, `xg_host_3d_math`,
  `xg_model_ft4_raw`, `xg_render_auth`, and 29 replay-tool tests. One direct
  Native-only Field 5 replay (no Original/Shadow/Native matrix) passes with
  282,767 Native packets, 30,129 bound, 20,991 state, 231,647 unbound,
  3,296 independent Native VRAM presents, and zero fallback, unsupported,
  Original draws, or parser replay commands. Evidence:
  `/tmp/opencode/xg-native-wide-complete-v8/runtime-evidence.json`. A separate
  Native-only live capture passes at frame 4015, 853x480 (16:9), SHA-256
  `d3ce2dbb5d014f4ca2bb9b083c4bdc957e62ec7a4e2db2a5e5efd447c9802283`;
  manifest: `/tmp/opencode/xg-native-wide-complete-v8/capture/manifest.json`.
  The replay validator now requires at least one independent Native VRAM
  present for command-stream completion and its single-run baseline path no
  longer imposes a contradictory post-checkpoint Cross input on the trace's
  declared neutral observation tail.

- **2026-08-06 (Native widescreen foundation and first source-space slice):**
  Added a producer-driven `XgNativeView` independent from every legacy
  `gpu_ws_*`, GTE-squash, culling rewrite, HUD heuristic, and mirrored-wide
  surface. The shared IR and `GpuRenderSemantic` can now carry an optional
  Native-view position while preserving canonical guest coordinates for packet
  comparison and VRAM. OpenGL owns separate Native-view FBOs, seeds their 4:3
  centre before the first semantic draw, renders Native semantics into the wide
  target, and presents it directly. Packet-derived draws without source-space
  data remain centred safely; they are not stretched.

  `World Horizon` is the first adapter to the generic contract. It projects the
  authenticated source vertices a second time through the shared view, while
  leaving its guest packets, OT links, canonical projection, and material
  untouched. This is a validation slice, not per-producer widescreen logic; all
  later 3D families will populate the same IR fields. The OpenGL test proves a
  source-derived position appears only at its wide coordinate, canonical VRAM
  remains unchanged, and the Native surface swaps directly. Focused CTest is
  9/9, replay Python is 61/61, and the cold 4,528-VBlank Original/Shadow/Native
  regression matrix remains `PASS` with zero Native unsupported, Original, or
  parser-replay commands. Evidence:
  `/tmp/opencode/xg-native-view-regression-v1/evidence.json`.

- **2026-08-06 (Producer-first packet-faithful Native contract):** Native keeps
  authenticated producer semantics as its preferred source, but a complete
  supported GP0 draw without a binding now translates directly to
  `GpuRenderSemantic` and renders through the OpenGL Native backend. The packet
  path never enters `gpu_write_gp0_body()`, `gp0_exec_*`, or parser replay.
  Preflight and submission both reject packet-derived draws unless OpenGL is the
  effective backend; software and Vulkan cannot rasterize this Native path.
  Evidence labels a zero-unbound stream `independent` and a successfully
  translated nonzero-unbound stream `packet-faithful`. Unsupported packets,
  Original draws, parser replay, not-found bindings, and staging failures remain
  hard failures. Focused software rejection, hidden-context OpenGL raster, and
  replay-validator tests pass. The canonical cold OpenGL matrix runs 4,528
  VBlanks per row and reports `status=PASS`; its Native row contains 30,129
  producer-bound packets, 20,991 ordered state packets, 231,647 packet-faithful
  draws, and zero unsupported/Original/parser-replay/not-found/stage failures.
  Evidence: `/tmp/opencode/xg-native-packet-faithful-matrix-v1/evidence.json`.

- **2026-08-05 (Field target/status `0x48` polylines):** Recovered and
  authenticated `RenderFieldTargetPolylines` at
  `0x801CFB48..0x801CFF60` from the runtime Field 5 artifact. The routine
  iterates 32 actor slots, skips slots 15/31 and disabled groups, chooses the
  red/green status color, projects two independent three-vertex source-vector
  sets through `RotTransPers3`, and links both preinitialized monochrome
  polyline packets into OT bucket `+0x80` with `AddPrim`. The contract captures
  GTE projection state and all six source `SVECTOR`s at entry, derives the two
  polylines without consuming guest packet semantics, and publishes only after
  the return seam validates tag length, command/color, all XY words, and the
  variable-length terminator. `GpuRenderSemantic` now has first-class line-list
  topology; bound line commands consume the supplied semantic instead of the
  removed generic guest-packet line decoder. The exact function bytes are
  independently authenticated by SHA-256
  `4a437e0b9997a056107bc06a49e5f978562829128c8eb67be707dac0052dc66a`.
  The unrestricted retail-BIOS Native replay records 909 entry observations,
  688 emitting invocations, 41,280 Native polylines, 82,560 Native line
  segments, and 41,280/41,280 exact comparisons with zero mismatches, blockers,
  or staging failures. Opcode `0x48` unbound falls from 41,280 to zero.
  Evidence: `/tmp/opencode/xg-line48-native-v3/runtime-evidence.json`.
  Focused CTest is 3/3, focused Python tests are 29/29, and `git diff --check`
  passes. Global evidence remains red because Native producer coverage is
  incomplete; no timing-core behavior changed in this pass.

- **2026-08-04 (Field 5 sprite FT4 packet builder):** Authenticated
  `BuildFieldSpriteFt4Packets` at `0x8002675C..0x800269CC` for overlay caller
  returns `0x801CFA14`, `0x801CFA48`, and `0x801D23F4`. The producer derives
  XY, UV, CLUT, TPAGE, material state, and double-buffered packet addresses
  from its source descriptors without consuming guest packet semantics. Native
  publication occurs only after the return hook validates every generated
  packet. The contract is producer-scoped rather than field-scoped: the same
  authenticated builder also runs in attract field 490 and remains eligible
  there. The earlier Field-5-only gate was removed because replay checkpoint
  identity is not rendering authority. The unrestricted retail-BIOS replay
  reaches Field 5 and records 2,568 cutovers, 3,848 Native primitives, and
  3,848/3,848 exact builder comparisons with zero mismatches, blockers, or
  staging failures. Of those bindings, 3,843 are consumed and five remain
  pending at the final evidence cutoff; opcode `0x2D` unbound falls from 22,920
  to 19,077. Evidence:
  `/tmp/opencode/xg-field-sprite-unrestricted-final/runtime-evidence.json`.
  Focused CTest is 2/2, replay Python tests are 61/61, and `git diff --check`
  passes. Global evidence remains red because Native producer coverage is
  incomplete; no timing-core behavior changed in this pass.

- **2026-08-05 (Resident sprite FT4 and expanded field builder):** Closed the
  resident raw FT4 producer through wrapper `0x8001E298`, inner producer
  `0x8001E3D8`, validation seams `0x8001E874`/`0x8001E8D8`, wrapper commit
  `0x8001E2C0`, and direct return `0x8001E988`. Semantics come from the sprite
  descriptor, source vertices, GTE state, and draw state; guest packets are
  comparison-only. Direct invocations publish through standalone submissions,
  while wrapper-scoped invocations stage against the producer's OT bucket
  before the guest transaction becomes active. The unrestricted retail-BIOS
  Native replay records 3,915 invocations, 16,201/16,201 exact comparisons,
  2,793 cutovers, and 16,201 Native primitives with zero mismatches, blockers,
  or staging failures. Opcode `0x2C` unbound falls from 24,837 to 8,742; the
  residual matches the separately identified overlay producer family.
  `BuildFieldSpriteFt4Packets` is now accepted for every invocation of the
  resident producer. A previously unseen flipped descriptor proved that the
  guest decrements UV width/height when a negative pre-flip origin is clamped;
  reproducing that behavior yields 4,066/4,066 exact comparisons, 2,764
  cutovers, and 4,066 Native primitives with zero mismatches or blockers.
  Opcode `0x2D` unbound falls from 20,324 in the pre-fix replay to 18,978.
  Evidence: `/tmp/opencode/xg-parallel-producers-v13/runtime-evidence.json`.

- **2026-08-05 (Overlay FT4 `0x2C`/`0x2E` and zoom closure):** Closed both
  residual raw FT4 opcode families with full-image and producer-scoped
  authority. The `0x2C` image at base `0x801B2000`, size `270340`, SHA-256
  `6b9f505b5ea77f3bb7222e78d2b2550f038fb319db399b7d862b4bd236bb2dbe`
  authenticates producer `0x801E927C..0x801E92C4`, its ten callers, and the
  later semantic writers `0x801E73FC`, `0x801E9218`, and `0x8004A768`.
  Rectangle, static-quad, dynamic-UV, projected-GTE, glyph, material, and OT
  insertion contracts derive exclusively from caller inputs, globals, source
  vectors, GTE state, and `AddPrim` arguments. The integrated cold replay emits
  4,272 direct, 267 rectangle, and 4,203 projected Native primitives, exactly
  covering the 8,742-command overlay residual; combined with the resident
  16,201-primitives contract, opcode `0x2C` unbound is zero.

  Opcode `0x2E` uses two independently authenticated overlay images. The
  projected image SHA-256
  `75c675f9736365dded5373bbd851b4a8c763ba34c167ef223c47032e8068f69f`
  supplies 4,188 Native primitives through material, `RotTransPers4`, and
  `AddPrim` seams. The Field-5 image above supplies another 4,134 through
  authenticated field-template builders and their OT insertions. The zoom
  sidecar derives five templates across two buffers from initializer
  `0x800A663C..0x800A68F0`, updates RGB source-side, and emits 325 primitives
  through normal invocation seams. A late-attach resolver covers the remaining
  290 DMA replays by matching only the ten initializer-derived packet source
  addresses; it never reads template tags or payload. Opcode `0x2E` unbound is
  therefore zero. The unrestricted retail-BIOS cold replay runs 4,528 VBlanks,
  reaches Field 5, records zero substitution blockers and staging failures, and
  retains `status=FAIL` only because other opcode families remain uncovered.
  Evidence: `/tmp/opencode/xg-overlay-producers-integrated-v26/runtime-evidence.json`.
  Focused CTest is 4/4, focused Python tests are 34/34, and `git diff --check`
  passes.

- **2026-08-05 (FT4 replay, GP0 state, and control closure):** Closed the
  remaining 18,996 opcode `0x2D` commands by retaining a source-side semantic
  sidecar for `BuildFieldSpriteFt4Packets` and resolving later DMA replays by
  exact physical `packet + 4` identity. The replay path never reads packet
  payload and invalidates entries when producer authority or material family
  changes. The integrated replay records 3,970/3,970 exact template
  comparisons, zero blockers, and opcode `0x2D` unbound at zero.

  GP0 state commands are now a first-class ordered Native state stream rather
  than fake draw bindings: 7,286 E1 draw-mode, 6,463 E2 texture-window, 1,741
  each E3/E4/E5 draw-area/offset, and 2,497 E6 mask-bit commands, 21,469 total.
  GP0 `00` NOP, `01` cache clear, and `A0` CPU-to-VRAM transfer remain in the
  canonical GP0 machine with their ordering and VRAM side effects, but no
  longer count as missing geometry. Their previous 6,986, 3,781, and 3,760
  unbound counts respectively fall to zero. The unrestricted integrated cold
  replay reaches Field 5 over 4,528 VBlanks with zero stage failures; residual
  unbound is reduced to 3,492 genuine draw commands. Evidence:
  `/tmp/opencode/xg-native-integrated-v28/runtime-evidence.json`. Full CTest is
  54/54, focused Python is 34/34, and `git diff --check` passes.

- **2026-08-05 (Field-5 Native stream complete):** Closed the final 3,492
  genuine draw commands. FT3 producer material now propagates through the
  authenticated `memcpy` path and captures GTE geometry at `0x8002E1B4`,
  reducing residual textured polygons `0x27` (1,090) and `0x28` (221) to zero
  while preserving 37,386/37,386 exact FT3 comparisons. Producer-scoped
  sidecars cover gouraud quads `0x38`/`0x3A`, tiles `0x60`/`0x62`, and sprite
  `0x64` from their pre-projection vertices, colors, rectangle inputs, and OT
  insertion arguments. The last 1,402 opcode `0x2A` commands derive from the
  fullscreen producer at `0x801C6F70` and projected producer at `0x801CF550`,
  with DMA replay resolved by exact source identity. No contract decodes guest
  packet payload as semantic authority.

  The replay evidence gate now treats final-frame bindings as valid in-flight
  work only when the lossless accounting identity holds:
  `total_staged = consumed + superseded + staged_count`. It still rejects any
  unbound, unsupported, not-found, Original draw, stage failure, or accounting
  imbalance; no binding is drained or discarded to pass. The unrestricted
  retail-BIOS cold replay runs 4,528 VBlanks, reaches Field 5, remains effective
  Native with claim `independent`, and reports `status=PASS`: 267,484 bound
  packets, 21,469 ordered state packets, zero unbound/unsupported/original/
  not-found/stage failures, and exact staging accounting
  `229287 = 228398 + 838 + 51`. Evidence:
  `/tmp/opencode/xg-native-integrated-v31/state/run-1/runtime-evidence.json`.
  Full CTest is 54/54, focused Python is 34/34, and `git diff --check` passes.

- **2026-08-04 (Field-model raw FT3 farthest producer):** Authenticated model
  row 5 through constructor `0x8002D984`, post-constructor material seam
  `0x8002DA00`, mode-2 geometry entry `0x8002E484`, and geometry store
  `0x8002E5EC`. The semantic producer derives topology, model vertices,
  transform matrix, UV/CLUT/TPAGE material, maximum `SZ1..SZ3` depth, culling,
  packet cursor, counter, tag, and OT insertion without consuming guest packet
  semantics. A cold Native replay records 4,464 cutovers, 18,972 Native
  primitives, and 37,386/37,386 semantic comparisons with zero payload,
  geometry, tag, OT, cursor, or counter mismatches, zero blockers/staging
  failures, and opcode `0x25` unbound reduced to zero. It separately records
  558 changes to the low RGB bytes, which GP0 raw-texture opcode `0x25` ignores;
  opcode/UV/TPAGE/CLUT remain exact. Evidence:
  `/tmp/opencode/xg-native-ft3-final/runtime-evidence.json`. Focused CTest is
  4/4, replay Python tests are 61/61, and `git diff --check` passes. No
  timing-core behavior changed in this pass.

- **2026-08-04 (Field-model raw FT4 farthest producer):** Authenticated the
  resident `0x8002C700` dispatcher path from Field 5 caller return
  `0x8007519C`, including its caller-local transform matrix at `sp+0x58`, and
  added the `0x8002E688` farthest-depth FT4 seam. Ghidra and the instruction
  sequence at `0x8002E82C..0x8002E888` prove selector 2 takes the maximum of
  `SZ0..SZ3` before the `(ordering_shift + 2)` OT lookup. A cold Native replay
  records 10,602 dispatcher begins, 11,160 cutovers, 114,308 Native
  primitives, and 193,626/193,626 exact payload/geometry/tag/OT comparisons
  with zero mismatches, blockers, or staging failures. Opcode `0x2D` unbound
  falls from 132,255 to 22,920 (109,335 packets, 82.7%). Evidence:
  `/tmp/opencode/xg-native-ft4-farthest/runtime-evidence.json`. Focused CTest
  is 5/5 and replay Python tests are 61/61; the pre-existing full
  `xg_render_static_auth` synthetic-trace expectation remains independently
  red. No timing-core behavior changed in this pass.

- **2026-08-04 (Native direct-stream gate green):** Corrected the Native
  receipt accounting so superseded bindings count as replacements rather than
  unconsumed commands (`total_staged = total_consumed + total_superseded`).
  The Native DMA2 walker now records OT topology for the requested baseline as
  read-only observation while continuing to render through the direct packet
  path. A fresh Disc 1 Field 5 replay with root memcards reports
  `status=PASS`, `effective_render_mode=native`, `native_claim=independent`,
  baseline completeness `2047/2047`, `16674 = 16654 + 20` staged/consumed/
  superseded, zero pending/not-found/stage failures, zero parser replay,
  guest GP0, UI OT adapter, Original draws, or shared presents, 258
  independent FMV frames at `320x224`, and 3,746 independent VRAM presents.
  This is a route-level Native proof; the 272,299 unbound packets still come
  from the guest DMA/OT stream, so independent game-side 3D/2D producers and
  visual equivalence remain open. Focused CTest is `4/4` and the replay Python
  suite is `50 passed`.

- **2026-08-04 (Native independence correction):** The earlier P0/P13 receipts
  were route-level proofs, not proof of an independent Native renderer. Their
  zero-Original-draw result did not exclude generic GP0 executor replay, and
  the first FMV implementation only called the existing presenter through a
  facade. The stream contract now labels parser replay, shared FMV
  presentation, independent FMV presentation, and the UI OT adapter
  separately; a Native receipt cannot pass with parser replay or shared FMV.
  Generic GP0 packets now convert through `gpu_native_semantic_from_gp0()` and
  render through `gr_draw_semantic_immediate()` without re-entering `gp0_exec_*`.
  Fills, copies, and lines use direct backend calls. OpenGL FMV Native now has
  a separate upload surface and presentation operation. The UI OT route remains
  an explicit semantic adapter, not an independent game-side UI producer, and
  is counted separately. The prior v24/v25 receipts are superseded for the
  strict Native claim. A single live replay using the repository-root
  Xenogears memcards now correctly fails the strict renderer gate: the runtime
  reports `status=FAIL`, `effective_render_mode=original`, and
  `native_claim=hybrid`. It records 167,334 staged, 167,284 consumed, 30 still
  staged, 119,712 unbound packets, 286,996 guest GP0 command headers, 4,168
  shared VRAM presents, 5,229 UI OT adapter calls, and 104 independent FMV
  presentation frames. It has zero parser-replay commands, zero Original
  draws, and zero staging failures, which proves only that the current Native
  raster path is fail-closed, not that the whole renderer is independent. The
  run deliberately omitted only the separate post-checkpoint SIO-tail
  assertion; no full matrix was repeated.

- **2026-08-04 (native-render FMV proof replay):** The existing Field 5 replay
  `/tmp/opencode/xg-baseline-p0-v25-native-fmv.json` was rerun with Disc 1,
  repository-root memory cards, and the former Native FMV path. Its old Native
  row passed the route-level gate and recorded 258 real MDEC FMV presentations
  at `320x216` depth24, covering 17,832,960 pixels, but that receipt is not
  valid evidence for the strict Native claim because it used the shared
  presenter and generic GP0 replay. The required Original/Shadow comparison
  remains useful; the optional visual/VRAM digest differences remain open.

- **2026-08-04 (native-render FMV presentation):** The superseded
  implementation added Native-facing callbacks but still delegated to the
  existing OpenGL presenter. It preserved the canonical guest MDEC decode,
  DMA, GP0(A0), RAM, and VRAM mutations and did not inject host video, but it
  was only a shared presentation route. It is retained as historical context;
  the strict replacement is recorded above and uses a separate Native FMV
  upload surface and draw operation.

- **2026-08-04 (native-render P0 pre-GTE gate):** The GP0 Native environment
  fixes now keep raw semantic coordinates, latch `tpage` only for textured
  primitives (using the flat/Gouraud word positions), and observe generic
  Native material submissions under draw suppression. The two-run P0 matrix
  `/tmp/opencode/xg-baseline-p0-v24-pre-gte.json` passes its required
  Original/Shadow comparison and is deterministic for Native. Native reaches
  `complete=true` with field mask `2047`, zero fallback count, zero stage
  failures, 167,304 staged commands, 167,284 consumed commands, and 20
  superseded commands across 600 VBlanks; it records 35,743 material samples.
  GTE attribution remains in the evidence but is diagnostic-only because this
  Native milestone is explicitly pre-GTE. The optional Original/Native visual,
  VRAM, and framebuffer differences remain open; no visual-equivalence or
  production cutover claim is made. The full Debug CTest suite passes 53/53,
  and the Native replay/Python validation suite passes 60 tests. Root memory
  cards were used explicitly and remain outside the replay mutation surface.

- **2026-08-04 (native-render cold UI OT authority):** The replay launcher now
  passes the repository `game.toml` explicitly, so `bios_hle=true` activates the
  authenticated boot-shell skip even when the executable lives in an external
  build directory. Native receipts no longer accept a clean scene reset as
  `effective_render_mode=original`: after zero fallback, zero staging failures,
  nonzero Native staging/consumption, and zero Original draws, the completed
  scene is reported as `native`. The cold overlay interpreter now treats only the
  exact UI `DrawOTag` call sites as relevant `INTERNAL_OBSERVATION` hooks; the
  `CAPTURE` enum alias is handled explicitly. The Field 5 matrix
  `/tmp/opencode/p13-dialogue-native-v17-matrix.json` passes
  Original/Shadow/Native. Its Native row reports 225,502 staged, 225,482
  consumed, 20 superseded, 141,407 generic Native draws, zero Original draws,
  zero stage failures, and `ui_ot.prepare_count=847` with 219,988 candidates and
  219,988 staged UI packets. The receipt carries metadata-only UI OT counters
  and deterministic hashes for OT nodes, packets, semantic payloads, GPU
  environment, and VRAM serial. The second UI DrawOTag site is
  `0x800759CC` (not `0x800759C8`). No UI manifest cutover or FMV Native route was
  added; MDEC/DMA/24-bit presentation remains Original pending a dedicated proof
  replay. The P0 two-repetition command was correctly blocked for this trace
  because it does not request a baseline (`baseline.requested=false`); no visual
  equivalence claim is made from the Task 15 receipt.

- **2026-08-04 (native-render GP0 Native closure, superseded):** The previous
  generic dispatch at GP0 ingress counted a draw-suppressed parser pass followed
  by a second call into `gp0_exec_*` as Native. Its receipt and matrix remain
  useful for packet coverage, but do not prove independent rendering and are
  superseded by the semantic/direct-backend implementation recorded above.

- **2026-08-04 (native-render P13 replay matrix):** The authenticated retail
  replay now passes the complete Original/Shadow/Native matrix with explicit
  `SCPH1001.BIN`, repository-root memory cards, and Disc 1. Shadow clean scene
  closure is correctly distinguished from fallback after the bridge resets its
  effective mode to Original. Native records 5,514 staged commands, 5,494
  consumed, 20 superseded, and zero stage failures; the matrix receipt is
  `/tmp/opencode/p13-dialogue-task15-v3-matrix-20260804.json`. Task 15 validation
  now gates Native on a complete semantic stream rather than requiring legacy
  transaction substitutions. This remains a route-level P13 UI/2D proof only;
  no manifest or production cutover was added.

- **2026-08-03 (native-render P13 UI/2D census):** Ghidra closes the
  productive field-dialogue family at `FUN_8007554C`/`FUN_80075910` ->
  `FUN_8008004C` -> `DrawOTag` (`0x80044BD0`). `FUN_8008004C` updates the four
  `FieldTextBox` slots, links the background/border/arrow/cursor/portrait
  packets from `FUN_8007E1C0`, and invokes resident `FUN_80034888` for string
  entry packets. The former `0x8003700C` candidate is confirmed as Kernel Menu
  debug/font output and is not a production UI producer. Static evidence is
  now sufficient to define the packet contract, but runtime OT order,
  packet-address binding, effective GPU environment, and full Native stream
  coverage are not captured yet. No P13 cutover was added; the census and
  proof gate are recorded in `NATIVE_RENDER_P13_UI_2D_CENSUS.md`.

- **2026-08-03 (native-render P13 runtime census):** Extended the always-on GP0
  ring with per-command effective GPU environment and added
  `tools/native_render_p13_census.py`. A real route reached Field 5 at frame
  1858; its first draw frame had 119 GP0 commands but no TILE/SPRT packets and
  no hits for the text-box producer family, while GP0 provenance remained the
  resident `0x00000F40` GPU leaf. The packet-state observability gap is closed,
  but the replay did not exercise active dialogue. A second Field 5 recording
  reaches the checkpoint at its final frame with no drawable tail. P13 remains
   blocked pending a replay with active text-box slots and packet-address
   binding; no manifest or production cutover was added.

- **2026-08-03 (native-render P13 interactive retail replay):** The new
  `/tmp/opencode/xg-field5-dialogue-menu-20260803.toml` replay uses the retail
  BIOS, reaches Field 5 at frame 1937, and covers dialogue plus menu/submenu
  input through its 4906-VBlank budget. The dialogue census captures frame
  3810 with 326 GP0 commands, including TILE, SPRT, and FT4-family packets,
  and records 39 calls to resident `FUN_80034888` with arguments from the
  active string-entry slots. Each call returns to `0x8008044C`, which Ghidra
  identifies as the instruction after the call inside `FUN_8008004C`; this is
  the runtime call-site binding even though the function-entry ring does not
  emit a separate `FUN_8008004C` entry. The menu census records a TILE command
  and 19 string-entry calls. P13 still lacks complete OT-head/packet-address
  coverage and Native authority, so no manifest or production cutover was
  added. The census tool now emits the derived call-site relationship.

- **2026-08-03 (native-render P13 late UI window):** A focused replay census at
  frames 4029-4035 is stable at 111 GP0 commands per frame, including 64
  `0x2D`, 24 `0x2E`, one `0x2A`, one `0x2C`, and two `0x3A` commands. It has no
  `FUN_80034888` entries or target function entries. The replay input places
  the window after menu interaction, but there is not yet enough function
  attribution to call it a specific submenu renderer. It is retained as a
  packet candidate only; P13 remains without OT-head/packet-address coverage
  and Native authority.

- **2026-08-03 (native-render P12 performance control):** Route193 preserves
  the authoritative Native stream totals (476,478 staged, 476,442 consumed, 32
  superseded, four pending, zero stage failures and zero scene fallback) after
  removing per-primitive GL drains. The wall-time sampler now brackets World
  cutover bodies as Native instead of inheriting their guest hook's static
  phase. Its measured World window is 81.84% static guest, 13.17% interpreter,
  3.88% Native cutover, and 0.80% GP0/GPU. A retail-BIOS, identical-card
  Original control reaches Field 5 at the same VBlank 1859 and reproduces the
  same approximately 35-41 FPS trough, proving it is not a Native-stream
  regression. Direct packet indexing and removal of the remaining fail-hard
  stream barrier produced no measurable improvement and were removed. The full
  root suite passes 52/52, the focused runtime stream/OpenGL suite passes 2/2,
  and the replay Python suite passes 27/27; global auth remains BLOCKED by the
  existing trace overflow/final-context gate.
- **2026-08-03 (native-render P12 auth-proof closure):** Route196
  (`/tmp/opencode/replay196-route32-checkpoint-binding.json`) reruns the same
  retail-BIOS route after separating the first observed gameplay checkpoint
  snapshot from the final replay context. The receipt is `PASS` with
  `effective_render_mode=native`, 476,478 staged, 476,442 consumed, 32
  superseded, four pending, zero stage failures, and zero scene fallback. The
  auth proof is `OBSERVED` with runtime `accepted=true`, tier `cold`,
  `native_permitted=true`, no rejection, 349 exact entry/capture/return
  sequences, 349 completed-proof publications, and `trace_overflowed=false`.
  Field 5 is bound at checkpoint VBlank 1859 and remains valid at evidence
  VBlank 4623 even though the final diagnostic context is 1024. The root CTest
  suite passes 52/52 after the fix. This closes the route-level P12 proof
  regression; it does not close the Disc 1 residual P10 or Native-only P15
  gates.
- **2026-08-03 (native-render P9 coverage clarification):** Rebuilt the actual
  `build-dbg/XenogearsRecomp` target and reran the route with the explicit retail
  BIOS `game/SCPH1001.BIN`, rather than the bundled OpenBIOS. Route199 (cold
  overlay override) and Route200 (no cold override) both reach Field 5 with
  `status=PASS`, `effective_render_mode=native`, zero scene fallback delta,
  and the same authoritative stream totals. Both receipts report
  `world_execution.observed_mask=0x3EF` and family 4 (Clouds) at zero. A
  matching Shadow diagnostic Route201 also reports family 4 at zero, so the
  current route does not exercise Clouds; this is a dynamic-coverage gap, not
  evidence of a failed Clouds cutover. The older Route55 receipt is not
  promoted because it is `FAIL` and predates the rebuilt runtime. Cards retain
  their repository-root hashes.
- **2026-08-03 (native-render P9 Clouds waiver):** Per explicit user direction,
  the fail-closed Clouds Native implementation is accepted as complete for
  migration progress based on its authenticated prepare/commit path and green
  unit tests, despite the absence of a natural route receipt. The missing
  dynamic exercise remains documented as a non-blocking limitation and is not
  reclassified as a global P15 certification result. Work therefore advances
  to the P13 UI/2D census.
- **2026-08-02 (native-render P9 remaining World central cutovers):** Wired
  Terrain/Water `0x8009932C`, Entity Shadows `0x800747DC`, Decorations
  `0x8008615C`, and World Models `0x800848F4` as authenticated pre-body return
  cutovers, plus Actor Sprites at the authenticated resident seam
  `0x8001E2B4`. Each path uses its sealed value preparation, preflights every
  packet/OT/scratch/count/global target, stages the complete semantic set, and
  publishes compatibility state only after accounting succeeds. World Models
  now has a 17-family semantic template sidecar seeded by the authenticated
  `0x8002C8CC` initializer, including exact dispatch-table validation and GTE
  color-store observations; unsafe fallback or artifact mutation invalidates
  it. Actor world-entry/resident stack/RA context and epilogue closure are
  authenticated without replacing the outer loop. Replay JSON now exposes
  Native cutover/primitive counts for all five families. Overlay/resident plans
  were updated for the productive transfers and observations. The central
  integration harness now requires nonempty Native publication for Terrain
  (128 FT3), Entity Shadows (3 FT4), Decorations (1 FT4), all 17 World Model
  primitive families, and Actor body plus shadow (2 FT4), with wrong-caller
  fail-before-write checks and exact replay-counter serialization. The Models
  fixture validates all 17 initializer rows and all four authenticated GTE
  color-store observation sites before its productive cutover. The complete
  Debug build including `psx-runtime` and 51/51 CTests pass; the overlay-range
  pytest is now part of CTest. No game executable or external replay was run.
- **2026-08-02 (native-render P9 World Models Native preparation):** Added a
  sealed, fail-closed preparation/build/finalize boundary for World Models
  producer `0x800848F4` and all four authenticated callers. Ghidra confirms the
  resident `0x8002C700` table has 17 primitive rows, modes 0/2/3/4/5, mode-5
  farthest-depth selection, DPCS source RGB `0x80059598`, and cull mode 3's
  corner/center plus edge-midpoint bounds probes. The implementation captures
  records, transform-chain writebacks, model/group/material/vertex sources, and
  authenticated packet semantic sidecars; reproduces partial packet writes,
  DPCS/raw-command behavior, environment UV generation, counter/cursor/global
  effects, and dynamic OT-link authority; and refuses finalize until semantic
  and packet-side-effect staging are complete. Resident protection now covers
  bounds and environment-template helpers, and the overlay ledger authenticates
  each call-plus-delay pair. Central runtime sidecar observation, transaction
  publication, and return cutover remain unwired because
  `xg_render_auth_runtime.c` was explicitly out of scope. No build, CTest,
  pytest, replay, or other test command was run.
- **2026-08-02 (native-render P9 World Actor Sprite preparation):** Added the
  authenticated local preparation boundary at resident `0x8001E2B4`, after
  the Original matrix preparation and before body `0x8001E3D8` plus optional
  ground-shadow `0x8001E9BC`, with continuation `0x8001E2E0`. The value builder
  now models per-part translation and the shadow producer's X/Z source plane,
  and emits exact masked FT4 payloads. The sealed per-invocation API validates
  actor/data/descriptor/part/context ranges, prepared matrix and OT provenance,
  independent body/shadow packet-capacity gates, packet/OT destinations,
  cursor output, and family-specific scratch halfword effects before exposing
  records. Actor caller authority and all resident body/shadow dependencies are
  in the ledgers. Central runtime transaction publication remains to be wired;
  per user direction, no build, CTest, pytest, replay, or other test command was
  run in this preparation pass.
- **2026-08-02 (native-render P9 Terrain/Water Native preparation):** Added a
  sealed, fail-closed full-body preparation for `FUN_8009932C` without wiring
  the central runtime cutover. Ghidra confirms entry `0x8009932C`, epilogue
  `0x800996D4`, returns `0x80071B38`/`0x80076AEC`/`0x80077A1C`/`0x80078A18`,
  0x20-byte FT3 slots with 7-word tags, full-word OT replacement, a per-cell
  `count < 0x7FE` gate that can finish at `0x7FF`, final count `0x8009D7DC`,
  and masked scratch effects through `0x1F80038F`. The API now
  authenticates the context OT/packet arguments, validates all output ranges
  and record chains, and returns records plus an exact scratch value/write-mask
  ledger and continuation. Terrain Shadow gained fail-closed Native cutover
  accounting. Focused preparation/accounting tests were added but intentionally
  not run per the all-families-before-tests requirement; no build, CTest,
  pytest, or replay was run.
- **2026-08-02 (native-render P9 route32 Shadow closure):** Corrected the final
  Clouds payload mismatch: Ghidra confirms that `FUN_80086798` writes UV2/UV3
  with halfword stores, so their non-semantic upper halves are preserved from
  the destination packet rather than zeroed by the Shadow writeback model. A
  focused regression now seeds nonzero upper halves. The comparable route32
  environment is explicitly retail `game/SCPH1001.BIN`, HLE shell skip, HLE
  deterministic scheduler, and `16:9`; an OpenBIOS/LLE run and a default-4:3
  run were discarded because they do not reproduce the recorded boot/culling.
  `replay38-route32-shadow.json` closes 368/368 Clouds invocations and
  12,486/12,486 FT4 with zero packet, payload, geometry, tag, OT, cursor,
  position, scratch, anchor, or unexpected-write mismatch. Terrain remains
  exact at 434,583/434,583 FT3 and all other World snapshots are byte-identical
  to replay35. The complete build and 50/50 CTests pass; root card hashes remain
  unchanged. The receipt is family-level Shadow evidence, not a global PASS:
  the legacy P7 source aggregator still reports collector blocker 7 and
  overflow on this multi-scene route.
- **2026-08-02 (native-render P9 Terrain/Entity source correction):** World
  Terrain capture now authenticates the 25-entry active-tile table at
  `0x8009D618` and skips the pointer/edge/geometry reads that guest
  `FUN_8009932C` skips for `0xFFFF` entries. Entity terrain-normal capture now
  uses a new exact SF=0 `OuterProduct0` host operation for the guest call at
  `0x80093740 -> 0x8004A4D8`; the previous SF=1 operation shifted the small
  terrain cross product by 12 before `VectorNormal`, explaining the observed
  local-matrix divergence. The later Entity frame-building cross products
  remain SF=1, matching their `OuterProduct12` call at `0x8004A480`. Added
  read-only Entity diagnostics for pending coordinates, chunk/cell addresses,
  and five terrain height samples. Full build, 50/50 CTests, the three overlay
  range tests, and `git diff --check` pass. Replays 25-30 are diagnostic only:
  after rebuilding the previously stale runtime executable, the 4,757-VBlank
  route no longer transitions from Field 5 to Field 1/World under current HLE;
  pure LLE and HLE-with-real-intro also miss the route checkpoint. All attempts
  retained the repository-root memory-card hashes. No new Shadow evidence was
  promoted; the route must be recaptured against the current runtime before
  these fixes can receive natural World certification.
- **2026-08-02 (native-render P9 World atomic cutovers):** Raised the productive
  render transaction to 4096 commands/bindings/pending bindings and 32768
  journal words. Effects now performs complete source/target preflight, stages
  up to 256 exact FT4 bindings, publishes payload/tag/OT compatibility effects,
  and returns before its Original GTE producer; the integrated saturation test
  closes 256/256 bindings and the complete OT chain. Horizon now stages both
  FT4 while retaining SET/RESET DR_TWIN as compatibility commands in the same
  atomic OT journal, preserving `SET_WINDOW -> FT4 -> FT4 -> RESET_WINDOW`.
  Both cutovers preserve COP2, HI/LO, and transient deadlines and fail before
  guest writes on malformed targets. Full build and 38/38 CTests pass. A Shadow
  replay using the repository-root card1/card2 unchanged closed 428/428 Horizon
  and 428/428 Effects invocations with zero component divergence, but its global
  receipt ended FAIL on a later `scene_reset`; that receipt was not promoted.
  Natural Native certification remains blocked by earlier Field/global fallback.
- **2026-08-02 (native-render P9 World Horizon Shadow closure):** Added the
  value-only two-FT4 Horizon builder, bounded authenticated source capture, full
  body/table/epilogue range authority, and a Shadow comparator for geometry,
  payload, tags, OT, and both DR_TWIN commands. The first natural mismatch
  proved that World inherits projection controls across scene boundaries;
  Horizon now snapshots the GTE input controls OFX/OFY/H at its authenticated
  entry while remaining independent of every GTE result register. The final
  deterministic Field 5 -> Field 1 -> Overworld -> Battle receipt closes
  212/212 invocations and 424 primitives with zero divergences or capture
  failures. Generated resident C was refreshed and the full build plus 37/37
  CTests pass. At this checkpoint Native substitution still lacked one atomic
  FT4+DR_TWIN transaction. P9 Effects also gained authenticated caller/initializer/update/
  body/tables, bounded value-only capture, and host rotation/scale/projection.
  The final route closes 212/212 Effects invocations and 559/559 FT4 with zero
  count, geometry, payload, tag, or OT divergence and no GTE-result/packet
  source dependency. At this checkpoint Effects Native still exceeded the
  original 64-binding transaction capacity; the subsequent entry closes it.
- **2026-08-02 (native-render instrumented route capture):** Input recording now
  has an explicit close-completion mode that atomically publishes a replay on
  normal SDL shutdown while retaining the incomplete-only crash path and the
  existing stable-Field-5 certification mode. Recording no longer disables
  additive overlay capture, debug encounter-gate writes preserve the pristine
  executable-page snapshot first, and shutdown can emit a closed P7 model-FT4 /
  P8 sprite-FT4 Shadow comparison receipt. The full Debug build and all 36
  CTests pass; live Field 5 -> Field 1 -> Overworld -> Battle evidence is next.
- **2026-08-01 (native-render P7 Field particles):** Added source-observation
  plan v4 with an observe-only transfer, so warm and dirty-RAM cold execution
  can capture value-only inputs at `FieldInitializeParticlePrimitive` while
  still executing the Original initializer. An authenticated 128-record sidecar
  now supplies particle vertices/material to a full pre-GTE return cutover at
  `FieldParticleRender`; host code reproduces `CompMatrix`, scaling,
  `RotAverage4`, bucket selection, RGB/XY/tag/OT side effects, and semantic FT4
  staging without reading packet payload or mutating GTE state. Focused tests
  cover exact projection/material, code-write invalidation, missing-sidecar
  fallback, warm plan emission, and authenticated descriptor bytes. The current
  Field capture has no active particle controller, so natural replay validation
  remains pending without blocking the fail-closed static implementation.
- **2026-08-01 (native-render P0 GTE provenance wiring):** Production GTE
  execution now records an explicit backend tier at every PC-aware call:
  generated/static calls default to `STATIC`, dirty and oracle interpreter calls
  use `COLD`, and overlay callback calls use `WARM`. The authenticated Xenogears
  producer entry opens a visual-state/producer context only after the entry hook
  is accepted; successful continuation, rejection, scene reset, and test reset
  all close it. Production attribution bounds now cover the observed P0 producer
  volume while reduced-capacity tests retain fail-closed overflow coverage. A
  single Release Original/cold replay passed with 972,350 total GTE executions,
  117,055 inside authenticated producers, zero `UNKNOWN` tier hits, and no
  attribution overflow. The six-run P0 matrix remains intentionally deferred
  until the remaining P0 changes are grouped.
- **2026-07-31 (native-render baseline production evidence, schema v2):**
  Replaced test-injected material digests with typed material observations
  carrying canonical OT/DMA/MMIO provenance, source/container ordinals, final
  submission order, word count, and the complete effective draw material.
  Original GP0 draws publish only outside suppressed semantic side-effect
  replay; mixed Original/semantic observations publish in command order only
  after post-swap checkpoint commit. Deferred retries retain both candidate and
  Original-replay streams and publish only the authoritative winner. Added a
  backend-neutral canonical framebuffer digest and an
  OpenGL implementation that hashes top-down RGBA8 from the authoritative
  `s_hr_fbo` or deferred candidate while normalizing the hidden mask alpha.
  Capture is due on the final auto-finalize frame and is published only after a
  successful canonical present/swap. Runtime JSON and public replay evidence
  now close over every baseline-v2 field; focused runtime, root replay, and
  Python schema suites are green.
- **2026-07-31 (OpenGL semantic PS1 dither parity, RED -> GREEN):**
  Removed the transactional semantic dither rejection and established one
  OpenGL PS1 dither/15-bit endpoint for both Original GP0 and semantic draws.
  RGB888 flat/Gouraud polygon APIs prevent pre-GL truncation; GP0(E1) and the
  semantic material feed the same batch-keyed dither state. Geometry and
  textured fragment shaders consume one literal 4x4 matrix/quantizer;
  modulated textures use the oracle integer `(texel5 * color8) >> 4` order,
  and quantization remains before the existing blend/mask/STP passes. The
  scale-2 hidden-GL regression byte-compares raw VRAM across flat, Gouraud,
  modulated-textured, and STP-semitransparent triangles at offsets `(0,0)` and
  `(3,-2)`, including draw-area clipping and GP0(E6) mask set/check. It and the
  GL mask-order, forensic-readback, and renderer-transaction tests are GREEN
  under `xvfb`.
- **2026-07-26 (IRQ/COP2 post-stall deliverability refresh):**
  Oracle review found that issue-on-take could serialize behind a busy GTE and
  service a device deadline after `hw_deliverable` had been snapshotted. A new
  software-interrupt entry test starts with clear I_STAT, stalls NCLIP until
  cycle 10, raises an unmasked hardware IRQ at cycle 5, and was RED with
  `Cause=0x00000100`. Re-reading SR and recomputing hardware deliverability
  immediately after the COP2 issue sequence makes Cause.IP2 visible to the
  handler while retaining IP0, exactly one NCLIP, MAC0=19, EPC=P, resume=P+4,
  issue cycle 10, and deadline 17. The focused cycle tests, generated emitter
  and overlay contracts, executable overlay-loader test, direct regression,
  and `build-dbg` `psx-runtime` link are GREEN.
  Live slot-8 causal replay independently toggled only issue-on-take: pre-fix
  A1 had 4 determinant/MAC0 mismatches in 65,536 branch records; fixed B had
  0/6,295 across four AC-to-B0 IRQ returns; A2 with issue-on-take disabled
  recreated 1/6,295 (`det=46`, stale `MAC0=-28501042`); restored final B had
  0/6,307 across two AC-to-B0 IRQ returns. Final runtime SHA-256 is
  `7c4ae8e325635773a5f7bccb150d42d3b7f4306c969b79143c2c76db8b2e4246`.
- **2026-07-26 (IRQ/COP2 boundary issue-on-take, RED -> GREEN):**
  A deliverable interrupt whose selected EPC contains a COP2 command-class
  instruction now issues that command through the shared fetch/interlock/GTE
  timing path before exception state is pushed, while COP0.EPC remains the
  original address. The BIOS's documented COP2 EPC+4 rule is unchanged. The
  internal compiled-check callback now returns a redirect flag; both emitters,
  generated dispatch-entry checks, overlay ABI v19, and codegen namespace cg9
  return to the dispatcher instead of falling through after EPC+4. The focused
  regression covers dirty site-1 and compiled-style paths: NCLIP publishes
  MAC0=19 exactly once at cycle 1, handler-visible GTE deadline=8, EPC=P, and
  resume=P+4.
  The unrelated existing absolute capture-history assertion in
  `recompiler_patch_test` remains RED.
- **2026-07-26 (GL forensic-readback ordering defect, fixed):**
  Default-on `disp_ring_capture()` invokes `gl_renderer_fbo_peek()` on every
  present. The old peek/diff paths called `flush_cpu_upload()` then
  `pack_flush()` without draining queued flat/textured primitives. Packing
  could clear dirty ownership before queued work reached the authoritative FBO,
  leaving `s_raw_tex` stale for later texture/CLUT sampling. The hidden SDL/
  OpenGL scale-2 regression queued a white flat dot after a black upload:
  expected `0x7fff`, deterministically got stale `0x0000` twice. Removing only
  the fix hunk made the peek test RED while the mask test remained GREEN;
  restoring it and relinking made both GREEN. The fix calls
  `flush_flat_batch(); flush_tex_batch(); flush_cpu_upload();` before
  pack/readback in both `gl_renderer_fbo_peek()` and `gl_renderer_vram_diff()`.
- **2026-07-26 (OpenGL GP0(E6) delayed-batch ordering defect, fixed):**
  Delayed GL flat/textured batches captured mask-set but consumed the live
  mask-check value, so an E6 transition could execute earlier polygons under
  later state. `gpu_gl_mask_order_test` was deterministically RED: flat
  expected `0x7fff`, got `0x8000`; textured expected `0x8000`, got `0x7fff`.
  The reverse-toggle case was also RED. Minimal fix: drain flat and textured
  queues only when mask-check changes, land pending uploads, and rebuild
  stencils only on enable. The test is now GREEN.
- **2026-07-21 (VLC load-charge batching — shipped; dual still ~22 ms):**
  Runtime-only batch: under `psx_next_service_cycle`, `psx_cyc_charge`
  accumulates into `g_psx_cyc_batch` (no per-insn `psx_cycle_count` store);
  flush at IRQ check / MMIO sync / savestate / advance-past-deadline.
  Absorb/fudge still per-insn; guest totals at barriers unchanged; no MotK
  regen. Dual headless MotK (`PSX_NETPLAY_TIMING=1`, pinned halves): heavy
  25–50 band host med ≈39.7 fps / guest ≈21.9 ms/f / admit ≈3.1 (guest peer
  ≈39.8 / 22.5 / 2.8) — same floor as pre-batch (~21 ms / ~40–42). Counter
  publish was not the dual-peer tax; residual remains load-delay volume /
  LLC under phase-locked VLC. Next: PGO retrain after hot-path edits, or
  accept same-machine lockstep FMV floor.
- **2026-07-21 (MotK FMV host cost — MDEC/IRQ/charge; dual still ~21 ms):**
  Aimed to cut MotK FMV host work so two lockstep peers fit ~16.7 ms/f.
  Shipped bit-exact host opts: MDEC MB output reserve (no per-byte
  ensure_capacity), sparse-column IDCT, ch0 DMA burst feed
  (`mdec_dma_write_words`), sticky-IRQ undeliverable early-out in
  `psx_check_interrupts`, `psx_cyc_charge` pre-deadline bump on compiled
  loads/steps. Dual headless MotK (`PSX_NETPLAY_TIMING=1`, pinned halves):
  band 25–50 fps still guest ≈21 ms/f / fps ≈40–42 (admit ≈2.5–3) — same
  floor as before. HARD_CAP 16K→64K tried, no gain (real CD/timer events
  already shorten the deadline); reverted. Residual is phase-locked dual
  VLC load-delay volume / LLC contention, not MDEC FIFO or present/admit.
  Next: emitter-level load-charge batching for VLC leaves, PGO retrain
  after these hot-path edits, or accept same-machine lockstep FMV floor.
- **2026-07-21 (netplay FMV — lockstep guest inflation, not present/admit):**
  User A/B: two windowed offline MotK intros fine; headless netplay FMV still
  slow vs offline headless. Opt-in `PSX_NETPLAY_TIMING=1` splits the [FPS]
  line into guest ms/f vs admit ms/f. Heavy FMV (~25–50 fps samples): guest
  ≈21 ms/f, admit ≈3 ms/f (median) — frame time is dominated by the guest
  quantum under phase-locked dual MDEC, not Swap and not INPUT_CONFIRM wait.
  Offline headless same stretch is ~17 ms/f (≈57 fps). Pipelined CONFIRM
  tried in recomp-net (tip publish, drain next tick) — no FPS gain on
  localhost (~41→~41.5); reverted. Present-path / half-rate work is a dead
  end for this regression. Next lever: reduce MotK FMV host cost so two
  aligned peers fit a 16.7 ms budget (or accept same-machine lockstep floor).
- **2026-07-21 (netplay FMV — restore present-before-admit):**
  User confirmed early same-machine netplay FMV was ~50–60 and gameplay
  under lockstep stays ~60 — so the rematch-safe `finish→admit→pace→present`
  order was the FMV regression (expensive depth24 CPU present after admit).
  Restored `finish→present→admit/pace` via RAII `NetplayVblankTail` (admit
  on every return path; offline still paces before present). Kept half-rate
  depth24 present skip + UDP `poll()` barrier; no `SDL_Delay(0)`. Verify
  MotK intro FPS + tick-0 arm after rebuild.
- **2026-07-21 (netplay FMV — re-land half-rate depth24 present):**
  Windowed same-machine MotK netplay FMV was back at ~30–40 after the
  rematch-safe `finish→admit→pace→present` order. Re-landed host-only
  half-rate depth24 present: after admit, skip pace+Swap every other
  depth24 vblank (present first, then alternate); admit/barrier unchanged
  (no `SDL_Delay(0)` / present-before-admit). Offline path untouched.
  Rebuild MotK `build-release` + verify intro FPS and tick-0 arm.
- **2026-07-21 (lobby game_version + release pins):**
  WS lobby now carries `game_version` alongside `game_name` (create/list/join).
  Server rejects `version_mismatch` / `game_mismatch`; list can filter by either.
  MotK/MW bake release pins from repo `VERSION` (Release → e.g. `0.1.0`, else
  `dev`) via `PSX_GAME_VERSION` / `SNESRECOMP_BUILD_VERSION`. Clients send the
  pin on create/join and filter the lobby browser. Redeploy lobby server for
  remote matchmaking. Docs: `recomp-net-server/docs/WS_LOBBY.md`.
- **2026-07-21 (portable .pst / boot_state v3 LE wire):**
  Savestate / boot_state version → 3: header + section framing and all
  module snapshots emit little-endian field wires (`pst_wire.h`) — no
  host-struct padding (TimerRegs, DMA async/delayed, SpuVoice, McSlotState,
  CDROM Pending/Queued). Netplay host→guest blob transfer is identical on
  Win/Linux x86_64 and macOS ARM. Old v2 `.pst` files are rejected (recapture).

- **2026-07-20 (netplay match_caps — host settings enforce):**
  Lobby `create` / `set_match_caps` / `start` carry host sim caps
  (aspect, turbo_loads, bios_hle, fast_boot, auto_skip_fmv, input_delay,
  language). Server echoes on join/update/launch; guests apply before boot.
  recomp-net-server + psx_lobby_client + MotK launcher. SNES mirror:
  widescreen/hud/ignore_aspect/input_delay/ws_extra via snes_lobby + MW main.

- **2026-07-20 (launcher: persist controller selection immediately):**
  Device/mode/deadzone now write `settings.toml` on change (not only Launch),
  so Refresh / Quit / soft-return keep the pad. Refresh falls back to saved
  GUID if the dropdown index is stale.

- **2026-07-20 (lobby default → public host):**
  `psx_lobby_default_url` now `ws://netplay.technicallycomputers.ca:8765`
  (match SNES); override still `PSX_NET_LOBBY_URL`. Synced MotK vendored
  `psx_lobby_client.{c,h}` + recomp-net `docs/lobby.md`.

- **2026-07-20 (Metal Warriors H2H: top-edge prop pop):**
  Full-frame present recenters dual cam ~$40 up; spawn/OAM top was only
  −$70/−$70 so platforms popped at Y=0. Spawn −$A8, OAM CMP −144, present
  Y wrap peek for −64..0, dist-limit +64 when vert-widen.

- **2026-07-20 (launcher: controller Refresh rescan):**
  Dashboard Device row (P1 + offline P2) has Refresh — pumps SDL joysticks,
  re-enumerates gamecontrollers, keeps selection by GUID. Offline + netplay.

- **2026-07-20 (launcher: lobby lock emoji via symbol fallback font):**
  Password lobbies showed □ for 🔒 because the primary face has no emoji.
  Load a symbol fallback face through the shared Dear ImGui font atlas so
  missing glyphs resolve.

- **2026-07-20 (launcher lobbies: button order + dblclick join):**
  Lobbies actions: Return to Launcher → Change Player Name → Host Game →
  Join Lobby. Double-click a lobby row joins (same path as Join Lobby,
  including password modal).

- **2026-07-20 (launcher: offline↔netplay switch buttons):**
  Dashboard footer: "Switch to Netplay" beside Launch Game (only when
  `PSX_HAS_RECOMP_NET`); "Switch to Offline" beside Netplay Lobbies.
  Home Netplay tile gated the same way; no-netplay builds skip home chooser.

- **2026-07-20 (netplay load — apply freeze after hash match):**
  Suppressing INPUT at `np_begin_load_apply` deadlocked both peers in
  `netplay_barrier_admit`: tips stopped → `try_admit` never succeeded →
  guest never ran → `savestate_poll` never applied. Fix: keep INPUT during
  APPLYING; suppress only at `np_enter_load_ready` until `hard_resync`+prime.

- **2026-07-20 (netplay load — false peer_disconnect → lobby):**
  Hash-match apply suppresses INPUT for seconds → `peer_disconnected(1500)`
  fired → soft-exit to lobby → rematch. Fix: timeout=0 (BYE-only) while
  `in_load_barrier`; HELLO keepalive every 250 ms during suppress/stall.

- **2026-07-20 (netplay post-load — stale INPUT clobber):**
  2nd+ loads: correct frame, then ~3–5s frozen while FPS lived. Cause: during
  LOAD apply/ready the slower peer kept emitting pre-resync INPUT tips; those
  ticks share ring slots with the new tip (`tick % 128`) and first-wins /
  overwrite races blocked `remotes_ready_for_sim` after `hard_resync`. Fix:
  suppress INPUT sends for the load barrier; reject out-of-window remote ticks;
  host `probe_finish` before sync+prime; re-anchor frame pacer on restore.

- **2026-07-20 (savestate load — force GL present after identical frame):**
  2nd+ load of the same `.pst` left FPS climbing while the picture stayed
  frozen: `gl_renderer_present_vram` / wide early-out skipped `SwapWindow`
  when display rect + present-dirty matched the last swap (common after
  restoring into an already-shown frame). Fix: `gl_renderer_invalidate_present`
  marks all present tiles dirty, clears path latches, resets interp history,
  and forces 8 presents; called from `psx_frontend_on_savestate_loaded`.

- **2026-07-20 (netplay post-load — admit barrier symmetric):**
  Host dropped `LOAD_READY` before `try_admit` (guest did not) → confirm
  wait with barrier already down; keeping remotes let stale tip=D
  first-wins. Fix: both stay in `LOAD_READY` until admit; `hard_resync`
  clears remotes again; sync+prime at mutual ready only.

- **2026-07-20 (netplay post-load — mutual-ready sync):**
  `hard_resync`+prime at apply let the later peer wipe the earlier tip
  (2nd load slower). Sync once at mutual ready. (Remote-keep reverted —
  see admit-barrier note above.)

- **2026-07-20 (netplay post-load — symmetric ready release):**
  Guest cleared `LOAD_READY` on READY ACK while host still had
  `state_stall_sim` → confirm wait / intermittent hitch. Guest now ACKs
  but stays in `LOAD_READY` until `try_admit` succeeds (pre-sends
  INPUT_CONFIRM so host can admit on the same poll as `probe_finish`).
  Ready-probe retransmit 40→8 ms; confirm retransmit 16→4 ms. MotK rebuild.

- **2026-07-20 (netplay post-load resume — frozen picture + FPS):**
  After load, FPS kept climbing while the window stayed on a stale/blank
  frame: (1) present blank-latch skipped redraw after restore; (2) delay
  rings empty after `hard_resync` while peers could still advance during
  APPLYING. Fix: `hard_resync` resets `sim_tick→0` + `prime_delay_inputs`;
  stall admit for APPLYING when `!savestate_pending` and all LOAD_READY;
  `psx_frontend_on_savestate_loaded` forces restage/blank once; skip FPS
  CLI during load barrier. MotK rebuild.

- **2026-07-20 (netplay host-only save/load commands):**
  User `savestate_request_*` refused on netplay guest; F-keys / debug TCP /
  `PSX_LOAD_SLOT` host-only or routed via `psx_netplay_request_*`. Guest
  follow-host sync uses `savestate_request_*_protocol`.

- **2026-07-20 (netplay load — post-restore lockstep rendezvous):**
  Hash-match load applied on each peer at different times after early
  `hard_resync` → rings/ticks drifted → admit hang / starvation. Fix:
  stage load → apply while admit runs → `hard_resync` only after restore →
  LOAD size=0 ready probe until both ACK → then resume. Heartbeat in
  admit barrier. MotK rebuild.

- **2026-07-20 (netplay save hang — coord probe must not stall):**
  Shift+F1 stalled admit before `savestate_poll` could write → deadlock.
  Fix: `STATE_PROBE` with `size==0` (coord) leaves admit running; only
  hash probe (`size!=0`) + chunk transfer stall. Guest retransmit replies
  without re-staging saves. MotK rebuild after sync.

- **2026-07-20 (netplay host-owned saves — hash probe + chunk transfer):**
  recomp-net: `RNET_STATE_MAX` → 8 MiB, `STATE_PROBE`/`PROBE_REPLY`, stall
  admit through probe + transfer; restored `rnet_session_wait_recv`.
  psx_netplay: guest sandbox `saves/netplay/`, host-only F-keys, match-start
  memcard probe, save = coord local write → hash-agree → transfer on miss +
  post-CRC verify; load same pattern + `hard_resync`. MotK `build-release`
  linked; verify Shift+F1 / F1 across LAN.

- **2026-07-20 (MotK title after FMV — leave-depth24 restage):**
  On exit from GP1 depth24, GL/VK `depth24_upload_policy` restaged full
  CPU VRAM as 1555 into the FBO. CPU still held packed RGB888 from MDEC →
  rainbow/static title background (text/overlays still drew as prims).
  V2: skip only framebuffer-sized depth24 transfers (keep texture A0s);
  on leave scissor-clear the skipped FB union (GL) — never blind-restage
  RGB888-as-1555. Char-select shrink-to-left-center still under probe
  (OFX=256 @ 512 CRTC looks correct; may be authored layout / separate).

- **2026-07-20 (MotK 2nd intro right-edge stretch):**
  `depth24_fix_trailing_margin` replicated the last good column when any
  chroma>40 pixel sat in the trailing 8 cols. On the starfield FMV that
  smeared stars into an 8-wide flickering strip. Now requires dense chroma
  (~12% of margin) and black-fills instead of column-replicate. Still no
  CRTC/content_w crop.

- **2026-07-20 (MotK netplay FMV — lockstep floor, not present path):**
  A/B: offline headless FMV ~59; two offline headless concurrent ~59; two
  netplay headless ~38–40. Not dual-CPU contention and not GL present.
  Lockstep (INPUT_CONFIRM frame barrier + same-tick input rendezvous)
  keeps both MDEC peaks aligned. Async confirm / peer drift made FMV
  *worse* (~22–28) via overlapped memory traffic. Barrier now UDP `poll()`
  (not `SDL_Delay(1)`); localhost peers pin to disjoint CPU halves (~45
  in A/B). Pipeline admit rewrite hung tick-0 — reverted.

- **2026-07-20 (MotK netplay FMV — restore pre-rematch vblank order):**
  User: same-machine netplay was 50–60 before rematch playback tweaks.
  Reverted `NetplayVblankGuard` present-before-admit and half-rate depth24
  present. Order is again finish→admit→pace→present. Kept: `s_present_w/h`
  clear (black rematch FMV), depth24 FBO upload skip, trailing-margin
  in-buffer fix, vsync-off while lockstep armed.

- **2026-07-20 (MotK netplay tick-0 hang — revert admit latency hacks):**
  After half-rate present, both peers armed lockstep then sat at frame 0
  until peer_disconnect. Cause: `SDL_Delay(0)` busy-spin starved peer UDP
  on dual localhost; same-call `try_admit` publish when CONFIRM pre-seen
  also unsafe. Reverted both.

- **2026-07-20 (MotK FMV netplay FPS — half-rate depth24 present):**
  Measured: headless dual-peer lockstep holds ~60 guest FPS through intro;
  windowed dual-peer ~30–40. Bottleneck is two GL CPU-presents serializing
  before the guest fiber resumes — not admit/guest. Fix: under netplay +
  depth24, present every other vblank (host-only; admit still every tick).

- **2026-07-20 (MotK FMV netplay FPS — skip depth24 FBO upload queue):**
  MDEC A0 was still queued as 1555 CPU→FBO uploads (`UP_RECTS_MAX`=16),
  force-flushing mid-movie. While `gpu_display_is_depth24()`, do not queue
  GL/VK uploads; on leave, drop queue (no full restage — see leave-depth24
  log above). Alone did not restore
  windowed netplay to offline rates (present cost remained).

- **2026-07-20 (netplay FMV host FPS — present/vsync ordering):**
  Offline MotK intro ~50+; netplay ~30–40 was host path, not guest
  divergence. Fixes (determinism unchanged): (1) force GL/VK swap
  interval 0 while lockstep is armed (restore on soft-exit) so driver
  vsync does not double-block after the wall pacer; (2) move
  `finish_frame`→present→`admit`+pacer so local Swap overlaps the peer's
  guest quantum. `turbo_loads` stays off in netplay.

- **2026-07-19 (MotK FMV right-edge — no present-width crop):**
  Upload-span + `content_w` left-aligned GL crop removed the chroma junk
  but replaced it with a flickering black pillar (span varied per frame,
  especially during lighting). Rework: keep full CRTC width always;
  `depth24_fix_trailing_margin` only replicates the last good column
  into the last 8 RGB cols when chroma junk is detected — in-buffer,
  no viewport shrink. Half-texel nearest UV clamp remains.

- **2026-07-19 (MotK FMV right-edge — trailing margin, not CRTC shrink):**
  Root cause: MotK depth24 crawl is 512×128 CRTC, but ~8 trailing RGB
  columns are stale/black in VRAM; GL edge sampling flickered that strip
  as garbage. Fix (no 2/3 width): track A0 upload span
  (`gpu_depth24_rgb_limit`); blank/crop last 8 cols on short depth24
  bands (`h<240`); `gl_renderer_present(..., content_w)` left-aligned
  UV crop + half-texel nearest clamp; screenshot skips `sync_cpu` on
  depth24 (was clobbering RGB888). MotK release+PGO rebuilt; user-verify
  2nd intro (Star Wars logo) edge + ~50 FPS.

- **2026-07-19 (MotK FMV right-edge / 2/3 revert):**
  Tried depth24 width=(CRTC*2)/3 (512→341) for right-edge junk; MotK
  intros rendered left-shifted with the right of the video clipped —
  confirms the Jul-18 finding (logo is centered in a 512 RGB line).
  Reverted 2/3. Kept: depth24→fmv_frame, short-band without force_4_3
  gate, nearest present on depth24. Right-edge junk needs a different
  fix (not shrinking CRTC width).

- **2026-07-19 (MotK FMV FPS after rematch patches):**
  Rematch video fixed; ~30–40 vs prior ~50+ was not a present-path
  regression. Prior ~50 med was MotK intro PGO (`PSX_PGO=use`); LTO-only
  baseline is ~39. Mistakenly cleared PGO — restored `PSX_PGO=use` with
  existing intro `.gcda`. `fmv_frame` restored (depth24 only forces
  `pin_43` short-band letterbox). Re-train via `scripts/pgo_motk_intro.sh`
  after large rematch edits if profiles go stale.

- **2026-07-19 (netplay rematch: black FMV = stale present tex size):**
  Root cause: `s_present_w/h` survived GL context destroy; rematch FMV same
  size as prior CPU present took `glTexSubImage2D` into a new unallocated
  `s_present_tex` → black. Cleared on shutdown + init_context. Also dropped
  per-frame `flush_cpu_uploads` on depth24 (was cutting intro ~50→~30 FPS);
  depth24 CPU scanout needs neither FBO sync nor upload flush.

- **2026-07-19 (netplay rematch: black FMV, audio OK):**
  Rematch linked and played, but MotK intros had XA audio with black video
  (FPS still dipped ~30–40 → MDEC ran). Earlier mis-attribution to sync_cpu;
  also reset present/FPS/MDEC/iso session statics; `iso_close` before reopen;
  depth24 forces 4:3 pin for short-band letterbox.

- **2026-07-19 (netplay rematch: sticky I_STAT/I_MASK):**
  After cycle-reset fix, rematch still starved: dump meta showed
  `psx_cycle_count=4` with leftover `i_stat=VBlank` + game `i_mask`.
  `interrupts_init` / `memory_init` now clear I_STAT/I_MASK (+ mem_ctrl);
  `starvation_ring_reset` on `session_reboot` so dumps are rematch-clean.

- **2026-07-19 (stick→D-Pad axial deadzone again):**
  Radial+sign for digital stick→D-Pad made left/right fire Up/Down from tiny
  Y drift (jump/crouch). Stick→button sources use per-axis `controller_deadzone`
  again; analog `axes_to_pad_pair` / hybrid stick-detect stay radial.

- **2026-07-19 (netplay rematch: stuck `psx_in_device_service`):**
  Rematch still froze after `lockstep armed`: soft-exit longjmps out of the
  vblank callback while inside `psx_devices_service_to_now`, leaving
  `psx_in_device_service=1`. Every later `psx_advance_cycles` then skips
  device service → no vblanks → hang after tick-0. Fix: clear the guard on
  every scheduler longjmp escape; `psx_cycles_reset_for_boot()` zeros the
  guest clock + deadline bookkeeping at `session_reboot`.

- **2026-07-19 (Digital stick→D-Pad radial deadzone):**
  Stick-as-D-Pad used a per-axis square threshold so centre drift twitched
  movement even with a large launcher deadzone. Stick axis→button sources now
  require radial magnitude past `controller_deadzone` (same idea as
  `axes_to_pad_pair`); triggers stay per-axis. Hybrid stick-detect matches.

- **2026-07-19 (netplay rematch HLE shell-skip latch):**
  After soft-return rematch both peers linked (`lockstep armed`) then froze:
  `s_shell_skipped` stayed set from the first match so HLE boot-skip never
  re-fired and both ran the interactive BIOS shell under netplay.
  `psx_bios_hle_configure` now clears the latch; `cdrom_init` resets boot
  disc speed to 1x; lobby launch uses `input_player=-1` (auto) again.

- **2026-07-19 (netplay rematch session_id + endpoint guard):**
  Rematch HELLO hang: server now allocates a fresh `session_id` on every
  `start`/`launch` (stale BYE/HELLO from the prior UDP session no longer match)
  and refuses start when host/guest endpoints are empty. Client refuses
  `fill_netplay_launch` / `launch_pending` when `peer_hostport` is missing.

- **2026-07-19 (netplay return-to-lobby rematch):**
  Lobby WS stays up across Launch. Window-close / Escape / peer BYE soft-exits
  via `PSX_RUN_RETURN_TO_LOBBY` (scheduler longjmp or pre-entry flag) instead of
  `exit(0)` when the match started from a lobby room. Teardown keeps the WS;
  launcher resumes on `netplay_room` with ready cleared; rematch Launch
  re-enters `session_reboot` (re-init guest + netplay). Server clears ready on
  `start` so both peers must Ready again.

- **2026-07-19 (lobby server → closed-source Rust):**
  Proprietary `recomp-net-server` (Rust) owns WS lobby + privacy/docs;
  removed C `servers/lobby` from open `recomp-net`. Client WS helpers
  vendored at `runtime/src/lobby_ws/`. Default
  `ws://netplay.technicallycomputers.ca:8765`.

- **2026-07-19 (netplay lobby server + launcher menus):**
  Lobby WS+JSON owned by proprietary `recomp-net-server` (was C
  `servers/lobby/` under open recomp-net);
  `psx_lobby_client` + shared launcher home → Offline / Netplay → lobbies table
  (host/join/password). Launch hands `PsxNetplayConfig` to
  `psx_netplay_start` (LAN endpoints from lobby). ICE relay stubbed.

- **2026-07-19 (netplay peer disconnect QoL):**
  Barrier `SDL_PollEvent` on `SDL_QUIT`/Escape → `shutdown_runtime`+exit.
  recomp-net `BYE` (pkt 7) + `rnet_session_peer_disconnected(~1.5s)`;
  `psx_netplay_shutdown` sends BYE. Surviving peer prints and exits instead
  of spinning in admit.

- **2026-07-19 (netplay latch + INPUT_CONFIRM + exclusive capture):**
  Host stages one pad per sim tick (`latched_for_tick`); barrier only
  re-samples via `needs_local_sample`, stalls on `input_desync`
  (INPUT_CONFIRM hash mismatch). Netplay capture is exclusive to the
  assigned PlayerInput (no keyboard-all / all-controllers merge) so peer
  hashes agree. Cleared on `finish_frame` advance. recomp-net: preserve
  early peer INPUT_CONFIRM when activating (wipe raced slower peer into
  permanent stall). Smoke: two headless MotK peers both `lockstep armed`,
  frames advance, no INPUT desync.

- **2026-07-19 (netplay lockstep stall + per-peer input device):**
  True delay-sync gate: pre-scheduler + each vblank `finish_frame` then
  blocking `poll_admit` (guest fiber parks; no free-run on admit fail /
  linking). Auto/`--net-input-player`: host samples P1 device, guest samples
  P2 when assigned (same-PC C40+keyboard); pad blob deadzone normalize.

- **2026-07-19 (netplay pad ownership — session slots always plugged):**
  Host local device → net-slot 0 (sim P1); guest local → net-slot 1 (sim P2).
  While active, SIO is network-only (no local/`override` writes). Both session
  ports stay connected from `psx_netplay_start` through linking so in-game
  2P/VS detect works; `refresh_player_devices` no longer clears them.

- **2026-07-19 (delay-sync netplay bring-up — recomp-net LAN):**
  Wired `recomp-net` into the runtime as CLI/env LAN delay-sync (not GGPO).
  `psx_netplay.{c,h}` + CMake auto-discover `../recomp-net`; vblank owns
  `pump`/`try_admit`/`publish`/`advance`; local pads stage only — publish is
  sole SIO writer while active. Turbo + low-latency re-sample gated off.
  Lobby UI / ICE server deferred. Smoke: two procs with `--netplay --net-slot`.

- **2026-07-19 (MotK title/char-select — savestate PC + flat GEO batch):**
  User still saw ~10 FPS after draw-area reject. Real char-select profile:
  ~30k/s on-screen GP0(68h) starfield dots, `gpu_share`~0.8 (not empty clip).
  Also: `boot_state_load` forced `pc=entry_pc`, so F1 loads desynced (display
  off; false ~60 FPS). Restored saved PC; batched flat GEO tris in
  `gpu_gl_renderer.c`. Char-select Shift+F1: **~60 FPS locked**, gpu_share~0.06.

- **2026-07-19 (MotK title/char-select — GPU draw-area reject re-applied):**
  Shift+F1 title + Arcade char select were &lt;10 FPS: same OT drain as the
  inter-movie cliff — GP0(E3/E4)=(0,0)-(0,0) + thousands of clipped `0x68`
  dots / quads; GL built 2 tris/prim. Re-applied inclusive draw-area reject
  in `gpu.c` (prior revert blamed a “gap race”; crawl wrap was the separate
  24bpp present-width bug). Alone insufficient for live starfield menus.

- **2026-07-19 (MotK intro — native/hot/inline + multi-run PGO):**
  Shipped `-march=native`, `[recompiler] hot_funcs` for VLC
  `0x8006A9F8`/`0x8006CBE4`, Release-inline `debug_server_log_call_entry`
  (cpu_state.h), HIT-inline `psx_icache_fetch` (psx_icache.h), multi-run
  PGO train (`PGO_TRAIN_RUNS`/`PGO_TRAIN_SECS`). Remeasure clean logo still
  **~51 med** (more mid/high-50 samples; not locked 60). Load-delay cycle
  volume remains the ceiling. Inter-movie cliff open. No MotK VSync HLE.

- **2026-07-19 (MotK intro — PGO + advance_cycles host cost):**
  (1) `psx_advance_cycles`: drop per-charge watchdog/PC-sample (moved into
  `service_to_now`, HARD_CAP cadence); (2) `psx_devices_mmio_sync` recomputes
  deadline in-place instead of dirtying `next_service=0` (was forcing service
  on the next insn after every GPU/CD/MDEC MMIO); (3) MotK intro PGO via
  `scripts/pgo_motk_intro.sh` (`-DPSX_PGO=generate|use`, 311 .gcda). Clean
  logo (until inter-movie cliff): **~49–51 med** (was ~39 LTO-only). Crawl
  after gap recovers ~47–50. Inter-movie ~7 FPS GPU cliff still open. No
  MotK VSync HLE.

- **2026-07-19 (MotK intro — VLC host opts + Release LTO):**
  Host-side work on MotK VLC (`0x8006A9F8`/`0x8006CBE4`), not disc cache:
  (1) idle_skip no longer defeats IRQ fast-path; deferred idle GPR snap;
  (2) IRQ mid-path when bits already pending; (3) `psx_slice_block` header
  inline when parked; (4) main-RAM `psx_cyc_load_word`/`half` inlined;
  (5) MotK Release `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE` (-flto).
  Logo window: ~34 med → ~39 med (samples mostly 38–40). Still short of 50;
  load-delay cycle volume remains the ceiling (PSX_LOAD_DELAY=0 → hundreds
  FPS). Inter-movie ~5–7 FPS GPU cliff open. No MotK VSync HLE.

- **2026-07-18 (MotK intro — FMV pace: inline cycle advance):**
  Host FPS still short of 50 with real MDEC/XA. Inlined `psx_advance_cycles`
  (deadline fast path) + merged load fudge/cost into one charge; bit-exact
  DC-only MDEC IDCT. Release FMV ~33 med / ~39 avg (steadier; was dipping to
  teens). Gap ~7. Remaining: static VLC `0x8006A9F8`/`0x8006CBE4` + load-delay
  host cost. Do not revive MotK VSync HLE / HARD_CAP without mdec rising.

- **2026-07-18 (MotK intro — 24bpp CRTC width + short-band present):**
  MotK FMV CRTC is 512 (X1/X2÷5); logo centered in 512 RGB (~86..431).
  Reverted blanket 24-bit mode×2/3 (341 cropped the right). Width = GP1(06h)÷
  dot-clock for 15/24-bit. Short GP1(07h)=128 no longer fills 4:3 — present
  letterboxes src_h/240 (GL/SDL/VK). Inter-movie GPU cliff still open.
  CD-only `spu_render` kept.

- **2026-07-18 (MotK intro FMV — false 60 FPS / no video; rollback):**
  MotK `load_accel.vsync_query` + event-horizon_any / HARD_CAP→564480 /
  in-exception VBlank chunking produced host FPS ~60 with **no MDEC**
  (`mdec_decode_count` stayed 0, display disabled) — guest time raced past the
  STR. A/B: VSync(-1) HLE alone is enough to keep MotK MDEC at 0; disabling
  it restores decode/XA. Reverted those accelerations for MotK; kept sticky
  CD IRQ deadline, load-delay coalesce, MDEC DMA bulk/IDCT skips. Real intro
  with video is again ~30–40 FPS host-bound under load-delay. Do not claim
  MotK FMV pace wins without `fmv_state.mdec_decode_count` rising.

- **2026-07-18 (earlier same day — sticky CD deadline + load-path host cost):**
  `cdrom_cycles_to_irq` sticky presented IRQ → 1-cycle deadline after sync;
  fixed. Load-delay coalesce + inline `psx_advance_cycles`. Necessary but not
  sufficient for MotK FMV ≥50 with video.

- **2026-07-11 (Tomba 2 OpenGL full-attract performance + audio acceptance):**
  Resolved the shared renderer/overlay/capture cascade that made Beach, Whoopee
  FMVs, Mines, and Mine Cart slow. OpenGL now avoids mandatory present readback,
  batches Tomba's painter-ordered blend stream, and suppresses unchanged 30 Hz
  source presents. Overlay dispatch now distinguishes exact lazy entries from
  CPS interior continuations: continuations use the loaded range owner first,
  while exact entries reached inside local dirty flow can publish their cached
  native DLL. The hot `0x80106424`/`0x80106688` FMV helpers consequently dropped
  from ~60 interpreted entries/frame to zero. Small dynamic-text DLL images are
  mapped on a bounded worker (141 Tomba variants, not the 712-DLL vault), and
  auto-capture base64/file I/O now runs from a coherent RAM/seed snapshot on a
  low-priority worker. The audio bridge uses real callback duration, bounded
  P-only correction, and a measured 160 ms reserve inside its existing 250 ms
  ring. Release acceptance: 540 s / 107 five-second records, Beach -> Whoopee ->
  Mines -> Mine Cart -> repeat, guest min 59.76 Hz / median 59.94 Hz, **zero
  output underruns**, zero post-start overflows, and no cache growth (849 DLLs).
  Current-code screenshots visually confirmed Beach, FMV, Mines, and Mine Cart.

- **2026-07-10 (Tomba 2 Whoopee auto-skip dwell + native-wide Beach backdrop):**
  Added an opt-in silent-MDEC post-decode hold so presentation-side FMV
  auto-skip remains unpaced through a preloaded logo's authored release wait;
  the faithful guest timeline is unchanged and the default remains four
  vblanks. Added title-opted native-wide mirror gates for flat primitives and
  the textured pre-shaded backdrop phase. This keeps the canonical 4:3 buffer
  untouched while filling Tomba 2 Beach Town at 16:9 and 21:9 without stretching
  the later 3D foreground. Also fixed JSON escaping in the `fmv_state` path and
  extended its resolved-config reporting. Validated Release build, unattended
  first-attract captures at 4:3/16:9/21:9, and zero unknown dispatches.

- **2026-07-02 (HLE PIVOT implemented — HLE as a first-class swappable tier, gbarecomp model):**
  USER-DIRECTED pivot (supersedes "no HLE" §0; CLAUDE.md amended 2026-07-02, memory
  hle_tier_architecture.md). Built the full stack this session: (1) EMITTER —
  full_function_emitter.cpp now emits a null-by-default `g_psx_bios_hle_hook` consult at
  the top of every psx_dispatch_impl iteration (pre-normalize phys, BEFORE the game/
  dirty-RAM/static backends; handled ⇒ resume at $ra; NULL default = pure LLE,
  dispatch-identical). (2) RUNTIME TIER — runtime/src/bios_hle.c(+.h): v1 call-HLE =
  the B0 event family (DeliverEvent/OpenEvent/CloseEvent/TestEvent/EnableEvent/
  DisableEvent) ground-truthed against the SCPH1001 kernel disassembly (Ghidra,
  0xBFC11644..0xBFC11A84; EvCB [0x120]/[0x124], stride 0x1C), operating on the real
  guest EvCBs, callback delivery via psx_dispatch_call with the kernel-true $ra
  0x1720; everything else (WaitEvent/threads/pads/card/A0/C0) falls through to LLE.
  (3) BOOT HLE — one-shot shell-entry intercept (RAM 0x30000; LoadRunShell's indirect
  call always dispatches): real recompiled kernel init + SYSTEM.CNF + EXE load run
  authentically under boot-turbo; only the shell (boot animation) is skipped. The old
  fast_boot snapshot restore is REMOVED (fast_boot=true now aliases boot-skip only).
  (4) SELECTION — [runtime] bios_hle / bios_hle_keep_intro / hle_scheduler in
  config_loader (+ settings.toml bios_hle mirror), PSX_BIOS_HLE / PSX_BIOS_HLE_KEEP_INTRO
  env, startup banner bios_backend=/bios_boot=. PSX_HLE_SCHEDULER spike folded in
  (default via psx_hle_scheduler_set_default, env wins). (5) OBSERVABILITY — always-on
  16K HLE ring (route LLE/HLE/boot-skip) + hle_dump TCP command (TCP_COMMANDS.md).
  Regen era-consistent: BIOS + Tomba + MMX6 images (emitter changed). NEXT: Tomba
  save+load validation under BOTH backends (user drives); overlay-shard cg-tag refresh
  per title; grow the handler set (UnDeliverEvent, RCnt, A0 libc) with kernel-decompile
  + Beetle checks per handler.
- **2026-07-01 (Tomba pause-menu wedge RESOLVED — stale-recompiler shards, guard shipped):**
  The post-merge Tomba menu wedge (loaded saves only) was NOT the IRQ-resume class the
  prior handoff claimed. Added an always-on exception-EXIT half to irqctx_ring
  (interrupts.c: take_pc/real_epc/exit_pc/exit_reason/same_thread/restored/v1/ra/redirects)
  — it proved every VBLANK resume restores the interrupted GPRs correctly, and the
  0x80016588 "spin" is the normal per-frame vsync wait. Real root cause: autocompiled
  overlay shards were emitted by a recompiler binary (build-t2, 07:39) OLDER than the
  09:28 emitter changes (553d993), yet stamped with the CURRENT cg tag (compile_overlays
  derives the tag from --runtime-include, nothing verified the emitting binary). The
  stale-emitter shards corrupted the display-list task queue (0x801FD800 slots) during
  load-game at Stormy Mountain → menu drew nothing. Repro matrix (compiled/interp ×
  newgame/load), cache-absent A/B, and RAM diff (empty OT 0x8009CA10 vs linked prims
  @0x800Bxxxx) pinned it; recompiler mtime vs emitter commit time was the smoking gun.
  FIXED: rebuilt recompiler, purged + regenerated build-cosim AND build-prod caches —
  menu opens with cache fully enabled. GUARD (class-closing, general): psxrecomp-game
  now bakes the emitter-source hash (shared canonical list
  runtime/codegen_hash_sources.cmake + hash_codegen.cmake) and prints it via
  `--codegen-hash`; compile_overlays.py HARD-FAILS when the binary hash ≠ the tag hash
  (verified positive + negative, both cache and --static modes). Kept the
  same_thread_resume GPR-restore refinement in interrupts.c (ring-verified equal-value
  no-op in practice; faithful). Tooling gaps logged in memory
  (divergence_tooling_gaps_2026_07_01). PENDING: MMX6 cutscene→gameplay gate
  (interrupts.c changed), user validation of prod build (menu + title/save-menu lag —
  lag likely the same stale-shard all-interp fallback + rehash churn).

- **2026-06-27 (device-region MMIO read waits — DONE, branch wt/tomba2-mmio-waits off the
  I-cache tip):** Replaced the placeholder `region = (phys<RAM_SIZE)?3:0` in psx_cyc_readmem
  (memory.c) with the full Beetle MemRW device-region read-wait table (libretro.cpp:859-1131),
  size-aware: main RAM (phys<0x800000) +3; SPU 0x1F801C00-1FFF +36 (32-bit) / +16 (8/16-bit);
  CDC 0x1F801800-180F +6×size; GPU/MDEC/SysControl/FrontIO/SIO/IRQ/DMA/Timers (within
  0x1F801000-113F) +1; BIOS ROM / Expansion-PIO / unmatched +0; scratchpad +0 (early-out).
  Threaded the access size (1/2/4) from psx_cyc_load_word/half/byte + psx_cyc_lwc2_read into
  psx_cyc_readmem (the SPU/CDC waits are width-dependent). The device wait combines with the
  existing +2 completion (+1 LWC2) and fudge exactly as Beetle ReadMemory (LDAbsorb = region +
  completion). RUNTIME-ONLY — psx_cyc_load_* signatures unchanged, so NO emitter regen; just
  rebuild runtime/cyctest. New ruler #2 loops `mmio_timer` (Timer0 read → +3 = 1 dev + 2 compl)
  and `mmio_spu` (32-bit SPU read → +38 = 36 + 2). VALIDATED: cyctest COMPILED (4600) == Beetle
  (4382) EXACT on ALL 15 loops incl. mmio_timer +3 / mmio_spu +38; the 13 prior loops unchanged.
  Tomba 2 boots past the BIOS to its "SCEA Presents" intro splash, total_checks advancing, no
  freeze (the faster-MMIO change did not trigger a device-timing cascade like load=4 did).
  RESIDUAL (documented, unmodeled dynamic axis): DMACycleSteal — Beetle adds the live DMA
  bus-steal count to EVERY read (libretro.cpp:868); non-zero only during active DMA, needs the
  steal count threaded out of the DMA controller, can't be isolated by a static ruler.
  memory.c + gen_testrom.py.

- **2026-06-27 (I-cache fetch — Stage 2 DONE: compiled-path emit + production default-on):**
  Both static emitters now charge the I-cache fetch cost at each cache-line LEADER, BEFORE
  the per-instruction interlock/load (Beetle ReadInstruction order, so a fetch miss clears
  the pending load give-back first). Leader = a block leader / mid-block jump-table target
  (any non-fall-through entry → possibly-cold line) OR a 16-byte-line start (addr&0xC==0);
  intra-line fall-through followers are provably hits (the leader refilled the line to its
  end) so they emit nothing (+0). code_generator (game): `addr` is already the KSEG0 runtime
  PC. full_function_emitter (BIOS): the loop addr is the ROM/compile addr, mapped to the
  RUNTIME guest PC via the existing `relocate_ra` (BIOS main stays in-place KSEG1 0xBFC..
  uncached; kernel Part 2 → 0x500+, shell → 0x80030000+) so the shared TV array evolves
  identically to the interp's cpu->pc and the KSEG1 (>=0xA0000000) uncached test sees the
  true virtual address. The compiled path emits at the SAME address value the interp would
  for the same instruction (they share s_icache_tv), so mixed compiled/interp stays
  consistent. VALIDATED: ruler #2 COMPILED (port 4600, PSX_ICACHE=1) == Beetle (4382) EXACT
  on all 13 loops incl. `icache_miss +14` (was +0 pre-Stage-2); ruler #1 [0x1C5C→0x1CA4]
  steady delta 0 (56==56) AND native now produces the I-cache cold-refill spikes (77/84,
  Beetle's range) that were absent before — exact per-hit magnitude varies run-to-run
  because the two processes free-run (Rule 16), not a bug. Tomba 2 boots past the load wedge
  → intro FMV (jungle) + crisp PS BIOS logo, total_checks advancing, no freeze. Then flipped
  `psx_icache_enabled()` DEFAULT ON (PSX_ICACHE=0 still disables for A/B) — re-validated the
  default-on path (cyctest no-env == +14; Tomba 2 boots clean). psx_icache.c + both emitters.
  NEXT axis: DMA cycle-steal / device-region MMIO load waits (SPU +36 etc.), currently
  unmodeled. Eventually merge wt/tomba2-load-accuracy to master after cross-title regen+smoke.

- **2026-06-27 (I-cache fetch — MODEL built + interp-validated EXACT; Stage 1 of 2):**
  New runtime/src/psx_icache.c: faithful direct-mapped (4 KB / 256-line) instruction-cache
  fetch cost, transcribed from Beetle PS_CPU::ReadInstruction — HIT +0 (no give-back clear),
  KSEG1/uncached +4, cached miss +3 + refill from the missing word to the line end (earlier
  words stay invalid), miss clears the load give-back. Mirrors only the per-word TV tag array.
  Wired into the dirty-RAM interp (exec_one) per instruction, charged BEFORE §1 (Beetle order).
  New ruler #2 loop `icache_miss` (loop top + victim 0x1000 apart alias the same line → refill
  miss every iteration). VALIDATED interp vs Beetle (PSX_FORCE_INTERP=1 PSX_ICACHE=1):
  icache_miss native +14 == Beetle +14; the other 12 loops unchanged at +0 fetch. So the
  hit AND refill-miss costs are MEASURED equal to the oracle on the interp path. Opt-in via
  PSX_ICACHE=1 (default OFF) — charging fetch in only one backend would fork mixed
  compiled/interp timing. Default-off → no production change (Tomba 2 FMV/logo verified
  byte-identical). Commit 958a928.
  STAGE 2 (pending): emit psx_icache_fetch at each cache-line leader in BOTH static emitters
  (code_generator + full_function_emitter), using the RUNTIME address (handle ROM->RAM
  relocated shell code); flip PSX_ICACHE default on so both backends charge it; validate
  ruler #1's cold first-hit spike (Beetle 84/77 vs steady 56) reproduces on the compiled
  path. RISK: the compiled cache state must match Beetle cycle-for-cycle across the whole
  boot to reproduce the cold spike — a careful cache-state-fidelity validation.

- **2026-06-27 (interp-path Δ-ruler — INTERP == Beetle EXACT on all 12 components):**
  Closed the last validation gap: the dirty-RAM INTERPRETER is now MEASURED equal to the
  oracle, not just shared-by-construction. New tooling: `PSX_FORCE_INTERP=1` makes
  `dirty_ram_is_dirty` (memory.c) report all RAM above the kernel window dirty, so the
  dispatcher routes clean compiled game text through the dirty-RAM interpreter (the same path
  overlays take) — no emitter/dispatch change. Launch psx-cyctest with the env set; the test
  ROM runs interpreted (dirty_ram_insns → hundreds of millions). measure.py --port 4600
  (interp) vs 4382 (Beetle): ALL 12 loops match EXACTLY (baseline/alu/load/load2/load_use/
  div/div_spaced/mult/gte_rtps/gte_nclip/gte_read_use/ld_div). Commit b0391bc.
  TWO PROCESS LESSONS (cost real time — now in cyctest README): (1) launch psx-cyctest via
  PowerShell Start-Process — a bash '&' launch fails to boot (pc=0); (2) sample cyc_watch /
  freeze_check AT STEADY STATE — an early query (before the BIOS boots to the EXE entry)
  reports dirty_ram_insns=0 / warm-up values. That premature-sampling artifact was the entire
  "dispatch paradox" I chased (the interp engages only after the EXE entry is reached).
  NEXT axis: I-cache fetch (ruler #1 84/77 cold-refill spikes).

- **2026-06-27 (GTE-read + MFC0 + muldiv give-back — IMPLEMENTED + VALIDATED, ruler #2 100%):**
  Closed the last steady-state divergences. KEY METHODOLOGY FINDING (Rule 15): Beetle's
  cyc_watch must be sampled at STEADY STATE — its boot/warm-up window reports the
  no-give-back value, which is why earlier sessions mis-recorded gte_rtps as "+15 EXACT"
  (true steady = +11). Added ruler #2 probes (load_use, gte_read_use, ld_div) to Δ-gate it.
  Shipped: `psx_gte_read` (MFC2/CFC2: stall to gte_ts_done AND arm ld_absorb=stall/
  ld_which_t=rt give-back; MTC2/CTC2 keep stall-only `psx_gte_stall`); MFC0 arms
  ld_absorb=0/ld_which_t=rt (suppresses a following load's fudge); `psx_muldiv_stall` now
  CONSUMES read_absorb during the MFLO/MFHI stall + the muldiv_ts_done-1 off-by-one — all
  transcribed from Beetle cpu.cpp:1332-1341/1723-1736, in both emitters + the interp.
  **All 12 ruler #2 loops == Beetle at steady state** (gte_rtps 18→14, gte_nclip 11→7,
  gte_read_use 19→14, ld_div 45→49 fixed; the 8 CPU-load/alu/div/mult loops held); ruler #1
  delta 0; Tomba 2 FMV plays (no regression). The R3000A load-delay + GTE/muldiv interlock
  is now hardware-faithful across every micro-benchmark. NEXT axis: I-cache fetch (ruler #1
  84/77 cold-refill spikes). Commits on wt/tomba2-load-accuracy (unpushed).

- **2026-06-27 (load ReadFudge/LDAbsorb — IMPLEMENTED + VALIDATED, both rulers exact):**
  Shipped the shared per-instruction R3000A load-delay interlock. New `runtime/include/
  psx_cyc.h`: §1 base + GPR_DEPRES + DO_LDS (`psx_cyc_step`) as static-inline helpers over
  new CPUState fields `read_absorb[33]/read_absorb_which/read_fudge/ld_which_t/ld_absorb`;
  `psx_cyc_load_word/half/byte` + `psx_cyc_lwc2_read` in memory.c do the Beetle ReadMemory
  timing (clear give-back, +2 fudge iff predecessor committed no load, region RAM +3 +
  completion +2/+1 as the LDAbsorb give-back, scratchpad +0). The pure dep/res classifier
  `psx_cyc_dep_res_mask` (transcribed from Beetle per-opcode GPR_DEP/RES) lives in
  psx_instr_cost.h. Wired into the dirty interp + BOTH static emitters (code_generator game,
  full_function_emitter+strict_translator BIOS); loads now route value reads through the
  UNCHARGED psx_read_* (cpu->read_* rewired in main.cpp; the flat +4 charge_main_ram_read is
  gone). **Δ-validated against Beetle:** ruler #2 `load2` +10 → **+11** == Beetle, every other
  component still exact (alu+1/load+5/div+38/mult+15/gte_rtps+15/gte_nclip+8); ruler #1
  [c5c→ca4] 54 → **56** == Beetle steady-state (84/77 spikes = I-cache cold refill, P2). Tomba2
  boots to the intro FMV (screenshot pixels, no regression). Builds clean (tools/BIOS/game/
  runtime/cyctest). FOLLOW-UP (separate commit): GTE-read/MFC0 give-back + muldiv-stall
  give-back consumption (don't affect rulers; needed for mixed-code faithfulness). Supersedes
  the "MODEL NAILED, impl pending" entry below.

- **2026-06-27 (load ReadFudge/LDAbsorb — MODEL NAILED empirically, impl pending):**
  Derived the last load-path component (the residual on both rulers) by measuring
  Beetle's PER-INSTRUCTION cost via adjacent-PC region cyc_watch. Confirmed:
  fudge = +2 iff the previous instruction committed no pending load (ReadFudge=0x20;
  `(reg>>4)&2` is 0 for all real regs), else 0; region+completion=5 (LDAbsorb excludes
  fudge); the load-delay-slot instruction does NOT absorb (its §1 precedes its DO_LDS),
  the instructions after it do. Per-instruction Beetle data: load = lw7/addiu1/bne0/nop0;
  load2 = lw7/lw6/addiu1/bne0/nop0. Full model + implementation spec in
  `accuracy/load_readfudge_ldabsorb.md`. Implementation is pervasive (per-instruction
  ReadAbsorb + GPR_DEP/RES in both emitters + interp) → a focused next task on a clean
  tree; validate via the ruler-loop anchors (baseline 3 / load 8 / load2 14). No code
  changed this step (read-only empirical derivation).

- **2026-06-27 (interp-path cycle ruler — enabler DONE + validated):** The dirty
  interp now emits `debug_server_cyc_observe(pc)` per instruction (gated), so
  interp-executed PCs are cyc_watch-anchorable (were not). Validated: a live
  Tomba2 overlay interp loop (0x8010724C) records stable cyc_watch hits at 38
  cyc/iter, parity with compiled-PC anchoring; FMV no-regression over 150M+
  interp insns. Commit fc85d8b. This lets interp-side cycle work (the muldiv +
  GTE stalls) be MEASURED, not just by-construction. REMAINING for a fully
  isolated single-component interp ruler: the cyctest harness does not route
  indirect jumps to the dirty interp (a jalr to a scratch dirty address left
  dirty_ram_insns=0 there), so a clean dirty-RAM component loop needs cyctest
  interp-dispatch wiring (or an overlay_cache=off Tomba2 component anchor).

- **2026-06-27 (GTE per-command completion-stall — VALIDATED EXACT):** Modeled
  GTE (COP2) command latency + stall-on-COP2-access. New CPUState.gte_ts_done;
  a GTE command arms it (now + cost-1, serializing back-to-back ops); any COP2
  reg access (MFC2/CFC2/MTC2/CTC2/LWC2/SWC2) stalls to it. Cost table
  (psx_cycles.c) transcribed+verified from beetle gte.cpp op returns (note
  AVSZ4=5 not the psx-spx-doc's 6). Set armed in the shared gte_execute (both
  backends); stall emitted at every COP2 reg-access site in both emitters + the
  interp (offset cancels like muldiv). Added gte_rtps/gte_nclip loops to ruler
  #2: native +15/+8 == Beetle +15/+8 EXACT; all other components unchanged;
  Tomba2 boots to FMV no-regression. Commit ec1fd76. Required regen. UNPUSHED.

- **2026-06-27 (dirty-interp mult/div completion-stall — backend parity):**
  Completed the mult/div stall to the SECOND backend. The dirty-RAM interpreter
  (Tomba2 overlays) charged 0 for mult/div while the compiled emitters already
  set `muldiv_ts_done` + stall MFHI/MFLO — an inter-backend cost inconsistency
  that drifts the shared guest-cycle timeline. `dirty_ram_interp.c` now mirrors
  the compiled emitter exactly via the shared helpers (MULT→`psx_mult_latency_s`,
  MULTU→`_u`, DIV/DIVU→37, MFHI/MFLO→`psx_muldiv_stall`), under
  `PSX_ENABLE_BLOCK_CYCLES`. The interp charges base after exec_one (vs compiled
  "+1 at top") but the set/stall offset cancels (verified algebraically). The
  latency VALUES are already oracle-EXACT on rulers #1/#2 (compiled path); this
  makes the interp apply the identical model. Validated: Tomba2 boots to FMV,
  no regression. Caveat: validation is by-construction + no-regression, NOT a
  direct interp Δ — interp emits no cyc_observe and the testrom isn't an overlay,
  so a true interp-path ruler (interp cyc_observe + force-interp routing) is
  future tooling. Commit 75d5d1a, runtime-only, no regen, UNPUSHED.

- **2026-06-27 (load=4 boot wedge RESOLVED — faithful guest-cycle pad ACK):**
  The oracle-accurate load wait-state (=4) had deterministically wedged Tomba 2
  boot in the BIOS shell (handle s1=104 → 1672-stride table index → wild ptr
  0x8013B608 → RAM corruption → pc=0). Step A (confirm, not hypothesise): the
  proximate runaway was **100% controller (pad) polling** — `sio_irq_dump` showed
  the last 150+ SIO IRQs all `source=pad, delay=4, active_device=PAD, mc_state=0`
  (card idle), NOT the memcard enumeration the handoff guessed. Source-confirmed
  unfaithfulness: the pad fast-path (`sio.c:1386`) armed the access-paced
  `sio_irq_countdown=SIO_IRQ_DELAY_PAD(4)`, decremented once **per SIO register
  access** (sio_tick is only ever called cycles=0), so pad ACK→IRQ7 was
  access-count-paced, not guest-cycle-paced; the faster (accurate) CPU fired it at
  the wrong guest-cycle phase vs the cycle-paced timers/VBLANK → BIOS pad-detect
  state machine diverged. Step B (faithful fix): the pad fast-path now arms the
  **guest-cycle-paced ack scheduler** (`sio_pending_ack`/`sio_ack_remaining =
  BAUD+ACK = 1258 cyc`, driven by `sio_advance`←`psx_advance_cycles`), identical
  to the already-faithful card path. RESULT: load=4 boots **past the wedge to the
  intro FMV** (screenshot-verified, frame 11k+ stable). Ruler #1 native 54 vs
  Beetle 56 = the known load-ReadFudge gap on the load=4 branch, NOT a regression
  (SIO timing can't change CPU instruction cost). Runtime-only (`runtime/src/sio.c`),
  no regen, UNCOMMITTED. Write-up: WEDGE_load4_shell_rootcause.md. Follow-up
  (completeness, non-blocking): axis5 Fix-6 / "1.0e-e2" fully removes the pad
  fast-path so pad+card share one shifter path — needs menu input validation.

- **2026-06-27 (BIOS emitter muldiv stall — ruler #1 now EXACT):** Applied
  per-instruction cycle charging + the mult/div completion-stall to the BIOS
  emitter (full_function_emitter.cpp: +1 at the top of every in-function
  instruction + the 4 inlined orphaned-delay-slot sites; block-up-front charge
  off in per-insn mode) and StrictTranslator (MULTU/DIV/DIVU → psx_muldiv_set,
  MFHI/MFLO → psx_muldiv_stall). RESULT: ruler #1 [0x80001C5C→0x80001CA4] native
  30→56 == Beetle 56, STEADY DELTA 0 — EXACT. Both rulers now match the oracle for
  mult/div (ruler #2 game-side already exact). FMV no regression (i_stat 0x8D,
  full frame). Commit 180b821. The +1-at-top convention cancels the divu-set /
  mflo-stall offset identically to the game emitter (both exact). NEXT: I-cache
  fetch (ruler #1 residual = Beetle's 84-on-cold-hit refill spikes vs native flat
  56); then load wait-state calibration (memory.c +6 → ReadFudge model); then GTE.

- **2026-06-27 (RULER #2 closed + mult/div completion-stall VALIDATED EXACT):**
  Built the full cycle micro-benchmark harness (ruler #2) and used it to land the
  biggest Stage-2 component.
  - **ruler #2 = `tools/cycle_testrom/`**: hand-encoded PS-X EXE of single-component
    isolation loops (baseline/alu/load/load2/div/div_spaced/mult), each measured by
    consecutive-anchor Δ = one iteration; baseline subtraction isolates the cost.
    Both backends boot the SAME synthetic disc (mkpsxiso; license region extracted
    from an OWNED disc via dumpsxiso — LOCAL ONLY, gitignored). Beetle loads it via
    --disc; native via a dedicated psx-cyctest runtime target (boots disc, serial
    CYCT-00101 so disc-identity matches). measure.py compares per-component costs.
  - **Beetle ORACLE costs** (the HW targets): baseline 3, alu +1, load +5, load2
    +11 (2nd load +1 = ReadFudge), div +38 (~36 stall), div_spaced +38 (fillers
    ABSORBED), mult +15 (~13 stall).
  - **MULT/DIV completion-stall IMPLEMENTED + VALIDATED EXACT.** MULT/MULTU/DIV/DIVU
    set CPUState.muldiv_ts_done = now+latency (DIV=37; MULT via MULT_Tab24 14/10/7
    on operand magnitude); MFLO/MFHI stall guest cycles to the deadline
    (psx_muldiv_set/stall in psx_cycles.c). Native previously charged ZERO. Required
    PER-INSTRUCTION cycle charging (PSX_CODEGEN_CYCLE_PER_INSN) — now the DEFAULT on
    this audit branch — so the stall absorbs (the running cycle count must be
    accurate mid-block; block-up-front can't). Game emitter emits set/stall at the
    op sites. RESULT vs oracle: div +38==+38, div_spaced +38==+38 (absorb correct),
    mult +15==+15 — ALL EXACT. Tomba 2 still reaches the FMV (no regression).
  - **Load double-count fix** (earlier today): psx_instr_base_cycles reverted to
    pure execute base (loads=1); memory.c owns the data-access wait-state.
  - Commits 9cec60a, 2b5ad88, 47bcfec, a3e8f28 (+ cyc_watch dedupe). NOT pushed.
  NEXT: (1) calibrate memory.c load wait-state (native +7 vs Beetle +5: flat +6 →
  ~4 + a ReadFudge term). (2) Apply per-instruction mode + muldiv stall to the BIOS
  emitter (full_function_emitter.cpp) + dirty interp → closes ruler #1's div-stall
  gap (still 30 vs 56). (3) GTE per-command cycles (same stall mechanism, gte.cpp
  table). (4) I-cache fetch. Each Δ-gated on the rulers, FMV-verified.

- **2026-06-26 (RULER #1 BUILT + load double-count bug found & fixed):** Built the
  game-independent BIOS-kernel cycle ruler the §3c "TOOLING NEXT" called for, and
  it immediately paid off. Details:
  - **New oracle-model doc `CYCLE_MODEL_BEETLE.md`** — transcribed the full R3000A
    cycle model verbatim from in-tree Beetle cpu.cpp (base +1/insn minus load-delay
    absorb; I-cache fetch +0 hit / +4 KSEG1 / +3+refill miss; ReadMemory loads
    scratchpad=0/region-wait+2, posted stores; mult 6-13 / div 36 stall-on-MFHI/LO;
    GTE per-command table). This is the calibration ground truth.
  - **cyc_watch double-fire FIXED** (debug_server.c): observe was called from BOTH
    the dispatcher (trace_dispatch) AND the function prologue (log_call_entry) at the
    same cycle → every dispatched entry double-recorded. Added (phys,cycle) dedupe.
    Real tooling bug (Rule 15); native deltas were corrupted before this.
  - **Per-block-leader cycle observe ADDED** (full_function_emitter.cpp →
    debug_server_cyc_observe, #ifndef PSX_NO_DEBUG_TOOLS so prod = zero overhead).
    Native previously observed only at FUNCTION ENTRIES; now it samples at EVERY
    compiled block leader, matching Beetle's before-every-instruction sample. This
    lets cyc_watch anchor ANY block-leader PC (interior loop tops, prologue exits) →
    a clean KNOWN-instruction region on both backends. Emitted at normalize_address()
    (runtime phys) so relocated-kernel anchors match.
  - **THE RULER:** BIOS kernel EvCB-search fn at guest 0x80001C5C (relocated ROM,
    identical in every PSX title). Region [0x80001C5C→0x80001CA4] (18-insn prologue,
    contains divu+mflo + 2 RAM loads, no MMIO/GTE). Both backends now record it.
  - **BUG FOUND — loads double-counted.** Native [c5c→ca4] = 34, fully decomposed:
    block c5c advance 20 (=14×1 + **2 loads×3**) + block c9c 2 + **memory.c +6×2 = 12**
    = 34. The Stage-2 #1a commit (2ef47bd) made psx_instr_base_cycles return 3 for
    loads (+2 data-access) WHILE memory.c's charge_main_ram_read already charged +6
    per main-RAM read — the load data-access cost was counted TWICE. The opaque
    0x80017FC4 window hid this because the load over-charge masked the entirely-
    unmodeled divu→mflo stall (~30 cyc Beetle, 0 native). **FIX:** reverted
    psx_instr_base_cycles to pure execute base (loads=1), per the header's own stated
    "data access charged separately in the memory path" contract. memory.c is the
    single address-keyed owner (like Beetle's ReadMemory). Regen BIOS+game, rebuild.
  - **RESULT:** native [c5c→ca4] 34→**30** (exact: 16+2+12), dead stable. Beetle 56
    steady (84 cold = I-cache line refill). FMV still streams (i_stat 0x8D, no
    regression). The remaining −26 gap is now HONEST and decomposed: native is
    missing the divu→mflo execute stall, and memory.c's flat +6/load needs Beetle
    calibration. NEXT: isolate those two components — the BIOS prologue combines
    div+loads in one block (leader anchors can't split them), so the principled next
    step is **ruler #2 (HW test ROM, Amidog)** for hand-crafted single-component
    isolation loops (div-only, load-only), per the user's "do both rulers /
    completeness not convenience" directive. Then Δ-gate each EXECUTE-latency
    component (mult/div, GTE) into psx_instr_base_cycles and CALIBRATE memory.c's
    wait-state per region. All on wt/tomba2-cycle-audit, uncommitted.

- **2026-06-26 (measure: Beetle cycle clock BUILT + VALIDATED):** Added absolute
  guest-cycle exposure to the Beetle oracle (MAIN checkout, additive diagnostic):
  beetle-psx/libretro.cpp accumulates per-frame `timestamp` (CPU->Run slice) into
  `beetle_total_guest_cycles` (+ reset on init) with `extern "C"
  beetle_core_get_guest_cycles()`; runtime/src/beetle_debug_server.c h_ping now
  reports `guest_cycles`. Rebuilt beetle static lib + psx-beetle. VALIDATED (Rule
  0): guest_cycles advances ~565,022 cyc/frame = real PSX rate (33.8688MHz/~59.94).
  Beetle needs the .CUE (not raw .bin). FIRST CROSS-CHECK: native psx_cycle_count
  rate = 565,470 cyc/frame vs Beetle 565,022 (within 0.08%) => gross cycle-rate
  parity confirmed; remaining drift is fine per-instruction-path (needs same-PC
  alignment). Main-checkout Beetle edits are UNCOMMITTED (additive; master has
  other prior uncommitted work — leave for user to manage).
  NEXT (aligned comparator): Beetle has get_registers (PC) but NO run-to/step/pause,
  so same-PC cycle comparison needs a "capture guest_cycles when guest reaches PC X"
  hook on BOTH servers (native has run_to_frame/step; Beetle needs a PC-watch). Then
  diff cycles@PC native vs Beetle to see the residual drift, and Stage-2 cost
  transcription verified against it.
- **2026-06-26 (P3 step 1 DONE — single-source cost seam, identity):** Created
  runtime/include/psx_instr_cost.h `psx_instr_base_cycles(insn)` (identity, 1/insn).
  Routed BOTH backends through it: interp (exec_delay_slot, dirty-dispatch loop,
  precise-slice) + recompiler (code_generator.cpp sums it per block + outside
  delay-slot clone, folding into the compile-time block charge). PROVEN behavior-
  preserving: regen byte-identical (full.c + dispatch.c diff = empty) and Tomba 2
  still streams the FMV (i_stat 0x8D). Commit b00b81f. Stage-2 now edits ONLY this
  one function. ACCURACY_BURNDOWN.md added (all-axes burndown; axis-5 peripherals,
  esp. SIO/controller hybrid-pad bug, flagged weakest), 09a5d45.
  NEXT (measure before Stage-2 costs — don't guess): build the native↔Beetle cycle
  comparator. Feasibility CONFIRMED: beetle_debug_server.c (in worktree) already
  exposes beetle_get_frame_count via the beetle glue — add a parallel
  beetle_get_guest_cycles. Sub-steps: (1) find mednafen's running master-cycle
  timestamp in beetle-psx/mednafen/psx (psx.cpp PSX_Update / the CPU
  pscpu_timestamp_t accumulator — note it's slice-relative, must accumulate to an
  absolute guest-cycle count); (2) add a C accessor through beetle_libretro.cpp +
  a `guest_cycles` debug command; (3) rebuild Beetle static lib + psx-beetle
  (slow: `cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1
  HAVE_LIGHTREC=0 -j8`); (4) comparator: native psx_cycle_count (already in
  freeze_check) vs Beetle guest_cycles at same-PC convergence. THEN transcribe
  Stage-2 costs (mult/div, GTE table, mem wait-states) one at a time, each verified
  by this comparator. Native cycle side already exists; Beetle side is the gap.
- **2026-06-26 (holistic cycle-model audit, post-P2):** Audited ALL cycle-charging
  sites for the dominant class (delay-slot undercount) + cost-model consistency:
  - GAME + OVERLAY emitter (code_generator.cpp `translate_basic_block`): FIXED in
    P2 (block_exec_cycles +1 for outside delay-slot clone). Overlay/alias path
    shares translate_basic_block → covered.
  - BIOS emitter (full_function_emitter.cpp): NO undercount — different model. It
    emits delay slots IN-LINE at their real address (charged by the owning block
    via block_cycles count to next leader) and defers the branch via
    psx_taken_/psx_delay_ flags, rather than emitting an uncounted clone. So the
    delay-slot-is-leader case charges correctly. No change needed.
  - INTERP (exec_one callers): charges psx_advance_cycles(1u) per instruction
    (3 sites). 
  => The cost MODEL is "1 cycle/instruction", duplicated in 3 places (interp hard
  1u; game emitter instruction_count; BIOS emitter leader-to-leader count). They
  agree (Stage-1 backend-equivalent) but are NOT a shared function and NOT HW-
  accurate (Stage-2). recompiler CAN include runtime headers (already includes
  ../../runtime/include/ws_backdrop_detect.h) → a shared psx_instr_base_cycles()
  header is feasible for P3.
  NEXT CORRECTNESS STEPS (deliberate, not tail-of-session):
  1. MEASURE FIRST (don't guess HW costs): native exposes psx_cycle_count already;
     build the Beetle half — add additive guest-cycle exposure to the Beetle oracle
     (main checkout beetle_debug_server.c) + a native-vs-Beetle cycle/first-
     divergence harness (find_divergence.py is STALE/DuckStation-era port 4371 —
     replace with a Beetle 4382 comparator). This is the holistic correctness
     instrument; it makes drift visible for ALL code/titles, FMV being one measure.
  2. P3 shared psx_instr_base_cycles() seam (identity first → byte-identical regen
     proof → then Stage-2 real R3000A costs calibrated against the measure).
  3. P6: regress other titles (breaking is OK per Rule -1; just know), delete
     Tomba2 overlay_native_block.
  COMMIT: f9d50d7 on wt/tomba2 (local, not pushed, not merged to master).
- **2026-06-26 (P2 DONE — Tomba 2 reaches the intro FMV):** Implemented the
  delay-slot cycle-ownership fix in code_generator.cpp (translate_basic_block):
  `block_exec_cycles = instruction_count + (exit branch sits AT end_addr with a
  delay slot outside the block ? 1 : 0)` — charges the always-executed delay-slot
  clone that was previously uncounted (the -8 undercount). Applied to BOTH the
  slice budget and the block cycle charge. Regen + build clean. RESULT: native
  progresses past the frame-1824 logo-delay loop; screen animates; i_stat shows
  CDROM+DMA+SIO active; screenshot = the lush jungle intro FMV. The multi-week
  logo stall is GONE via the faithful fix (no hack, overlay_native_block untouched
  for now). Mechanism was structurally confirmed (instruction_count excludes the
  outside delay-slot clone) before the change; end-to-end confirmed by FMV
  screenshot. NEXT: P3 (shared per-instruction cost fn + MMIO segmentation), P6
  validation (delete overlay_native_block; regress other titles), and Stage-2 HW
  cycle calibration vs Beetle/psx-spx (current model is 1 cycle/insn = backend-
  equivalent, NOT yet hardware-accurate). Apply the same delay-slot fix to the
  BIOS emitter (full_function_emitter.cpp) and overlay/alias paths.
- **2026-06-26 (earlier):** Diagnosis corrected (timing-faithfulness, not take-point).
  Directive persisted (CLAUDE.md Rule -1, memory, MEMORY.md banner). Precise
  slicing root-caused (mid-function clean-text resume not dispatchable) + ChatGPT-
  validated fix (all block leaders = CPS continuations) — PARKED default-off;
  `psx_game_is_function_entry` predicate + slice-trace diagnostics + env toggle
  `PSX_PRECISE_SLICE` left in tree (inert). −8 mechanism located in
  code_generator.cpp (delay-slot-is-leader undercount). Tree builds + boots clean.
  NEXT: P1 (cycle-audit) → P2 (delay-slot ownership fix).
