# LOAD_TIME_ZERO — proposal checklist for loads-toward-0

Branch: `spike/load-time-zero` (worktree `_wt-loadtime-zero`).
Status: **exploration only — no code changes.** This doc is the burn-down
list; each experiment gets its own future session, its own flag, and a
stated kill criterion. Written 2026-07-13/14 from a memory sweep, a code
survey of the live tree, and a ChatGPT consult (project thread
"PSXrecomp workspace", 2026-07-13 — full 8-minute Extra-High answer in
that thread; merged into §1/§4/§5 here).

---

## 1. The frame (get this right before proposing anything)

```
wall_time(load window) = guest_time(window) x host_seconds_per_guest_second
```

Under `turbo_loads` there is **no wall-clock pacing**: the window is
emulation-throughput-bound *by definition*. CD sector deadlines are
scheduled in **guest cycles** (`cdrom.c sector_delay_cycles()`), so turbo
compresses them like everything else; the measured ~2x is the **host's
achievable emulation rate** inside the window, not a CD-cadence wall.
Both we and ChatGPT independently flagged that our earlier "loads are
bounded by authentic CD cadence" phrasing conflates guest-time duration
with the wall-time ceiling. A 2MiB transfer at 2x CD speed is ~6.8s of
guest time; at a 20x turbo multiplier that is ~340ms of wall time with
every VBlank, IRQ, and sector deadline intact.

Terms our first decomposition missed (ChatGPT):
- **Pipeline overlap**: per-sector guest time ≈ max(sector interval,
  guest per-sector processing) — decompression can hide under cadence,
  or dominate it. At 2x the interval is ~6.67ms guest.
- **Explicit game dwell**: fade timers, minimum-display periods, audio
  sync waits — guest time no CD or CPU trick removes.
- **Seek/spin/command latency** as a term separate from bulk rate
  (Sony's own figures: ~250ms average seek).
- **Host device/event cost**: per-block device stepping, IRQ checks,
  SPU/GPU/event machinery can bottleneck turbo even with native CPU code.
- **Residual wall-clock coupling**: audio queue pressure, SDL pumping,
  present deadlines can silently cap turbo.

Three levers, in ascending order of danger:

- **Lever A — cut host cost per guest second.** Timing-invariant, safe
  by construction. Data shards, overlay coverage, event-horizon
  acceleration (§4 E4), device-batch advancement, renderer cost.
  Any Lever-A win multiplies with turbo and is invisible without it.
- **Lever B — cut guest time of the window.** Title-sensitive, the
  proven wedge zone. ChatGPT, from DuckStation/PPSSPP/Dolphin evidence:
  **there is no universally safe guest-visible CD speedup** — DuckStation
  ships per-game disable traits for read AND seek speedup plus
  MDEC/CDDA exclusions; no public breakage denominator exists.
- **Lever C — skip guest work / synthesize state.** Twice-proven
  hardlock class (yield pumps r1/r2). The only credible TRUE near-zero
  is here: a validated state-transition cache (conditional savestate) —
  explicitly Phase-2, never foundation.

---

## 2. Prior-attempt ledger (do not re-litigate; verbatim outcomes)

1. **turbo_loads — SHIPPED, production path.** Engage = sustained
   `cdrom_load_in_progress()` + game-started, 20-frame hysteresis
   (`main.cpp:2062-2084`); presents 1-in-30; audio deliberately NOT
   muted (queue capped). ~2x wall on Tomba loads with a healthy overlay
   cache. NOTE (survey 2026-07-13): the primary SDL event pump now runs
   BEFORE the turbo early-return (`main.cpp:2006-2009`), so the historic
   "Not Responding" starvation appears fixed in code — needs live
   re-validation, not re-fixing. Still open: MMX5 dev-tools+turbo boot
   wedge (0xE10 exception storm) — foundation timing bug.
2. **disc_speed=4x/instant — PROVEN UNSAFE as a divisor.** Changes how
   many VBlanks fall between guest state transitions; MMX6
   VSync-callback tick counter freezes → boot wedge, even with the
   response arbiter (overwrites 24→0) and fixed (non-scaled) cmd
   latency. `instant` today is bounded (`instant_max_per_frame`,
   default 32/frame) — an IRQ-storm cap, not a safety rule.
   ⚠ Tree split: the live checkout (`feat/vigilante8-widescreen`) runs
   the Ape direct-delivery CD model (no arbiter); master's model differs
   (debt clamp at `master:cdrom.c:1633`). Any CD experiment must first
   name its canonical tree.
3. **Instant-loads round 1 (gen-time yield pump) — ABANDONED.** Tomba's
   loader `FUN_80021340` is also the live world-streamer; skipping its
   cooperative yields starves co-thread/IRQ progress → gameplay hardlock.
4. **Instant-loads round 2 (syscall-level yield skip) — PARKED.** By
   ChangeTh-syscall time the guest wrapper has already committed
   green-thread yield bookkeeping; ONE skipped switch is eventually
   fatal. Kept: the `[instant_loads]` inert scaffold (load-grid
   detector, window plumbing, TCP counters) and the measure-first method.
5. **Data shards — BUILT on `spike/tomba-load-shards` (2a4e574), never
   regen'd/run.** Memoized pure-function replay: read-set/write-set/
   reg-out/cycle-cost capture at emitter-hooked funcs, byte-verified
   replay, recorded cycles credited in slices through the normal
   advance+IRQ path. Purity poisons: MMIO, DMA write, thread switch,
   4MiB budget, staleness. Persists per-shard `.dss`. Known v1 fidelity
   deltas (documented in DATA_SHARDS.md): ISR cycles double-credited;
   IRQ arriving mid-credit observes the completed write-set; GTE/COP0
   not captured. §5 adds ChatGPT's sharper version of that last flaw.

Also relevant, shipped: FMV auto-skip (unpaced + muted + game-native
teardown), HLE boot shell-skip + boot-turbo window (`bios_hle.c`),
kernel bless (byte-verify-then-trust precedent).

---

## 3. Verdicts on the session's naive musings (be honest in future sessions)

- **"o2r equivalent — see it once and shard it"** → this IS the
  data-shards spike, already designed and built; the open work is
  measurement and a first live run. True o2r (AOT asset transformation)
  is unavailable to a recomp — no source-level asset knowledge.
  ChatGPT: the transferable o2r lesson is the **cache identity +
  validation model**, not the timing model; no prior art does
  read-set-replay-with-cycle-credit.
- **"HLE out the decompression"** → dominated by data shards for
  repeated inputs, but ChatGPT identified real niches where HLE-out
  wins: inputs mostly unique (shard hit-rate poor), read-set compare
  approaching decode cost, algorithm SIMD-friendly (bitstream code can
  beat generated C by >>2-5x), or — notably — **the decompressor runs
  from dirty RAM under the interpreter** (overlay region!). Decision
  rule: profile first; shards if inputs repeat + verify cheap; HLE if
  unique inputs or expensive verify; neither if decompression isn't
  host-hot. HLE shares the same temporal-write-visibility flaw as
  shards — it is not automatically more faithful.
- **"Skip decompression" literally (don't charge cycles)** → Lever B/C
  in Lever A's clothes; the cycle credit is what keeps the scheduler,
  IRQ phase, and CD interlocks sane. Any instant-shards mode is a
  Phase-2 per-game enhancement.

---

## 4. The burn-down checklist (merged: ours + ChatGPT's session plan)

Rules of engagement, every experiment: (1) measure FIRST via always-on
rings; (2) behind a flag, default off; (3) one experiment per session;
(4) kill criterion stated before the first line of code; (5) corpus
gate: Tomba load+world-streaming, MMX5 dev-tools boot, MMX6 boot/VSync
progression, repeated load/save cycles, attract demos, XA/CDDA, MDEC
FMV, a seek-heavy path, and (once identified) a direct-CD-hardware
title.

Standard probe report for every load window: guest cycles; wall time;
effective multiplier; CD commands/LBAs/deadline-vs-exposure times; seek
durations + buffer depth; VBlank/IRQ sequence; scheduler switches;
top host CPU consumers; SDL pump gaps; audio queue depth; canonical
architectural state hashes at checkpoints (never raw host structs).

### E0 — `load_probe_v2`: window decomposition  [GATES EVERYTHING]
The decisive question: **why does the machine advance at only ~2x
during a window whose presentation is already suppressed?**
Segment guest time (seek/start, sector intervals, per-sector
processing, explicit waits) and host time (game/BIOS native code,
decompressors, dirty-RAM interp, CD/event machinery, SPU, GPU/present,
scheduler/IRQ checks, OS/UI pump). ChatGPT decision thresholds: if
decompression ≥ ~40% of host time → shards are serious; ≤ ~8% → kill
shards immediately; if CD/event/SPU machinery dominates → do E4; if a
wall-clock limiter is still active → everything else is a distraction.
Prereq: master's LOAD GAME in-exception wedge may block the Load-Game
repro — use area transitions if so.

### E1 — Turbo hardening (Lever A hygiene; ChatGPT rank #1 with E4)
(a) `turbo_event_pump`: re-validate the pump fix live (gate: max UI
pump gap ≲50ms, input recognized <100ms, <2% load regression).
(b) `turbo_audio_sink`: fix at the HOST SINK only — keep SPU/XA/CD
audio emulation exact; consume/discard excess host samples, keep a
small tail, crossfade on turbo exit; NEVER touch guest mute registers
or skip guest audio processing (kill: any guest-state manipulation).
(c) MMX5 dev-tools+turbo 0xE10 storm: root-cause as a foundation
timing bug (kill: any proposal that masks the storm instead of
explaining the first divergence).

### E2 — `turbo_next_event`: event-horizon acceleration  [NEW AXIS — ChatGPT]
Advance through *provably* side-effect-free polling/idle regions
directly to the next scheduled observable event, with exact cycle
credit and identical event ordering. NOT heuristic idle-skipping —
requires proof: no memory writes beyond stack churn, no MMIO reads
whose value changes before the event, no IRQ deliverable earlier,
exact ordering at the destination. Candidates: CD status polls, thread
idle loops, BIOS waits, GPU/SPU completion polls. Companion change:
**batch device advancement to the next deadline** instead of ticking
every device after every recompiled block. This attacks the ~2x
ceiling directly. Gate: ≥20% guest-cycle throughput gain with identical
event ordering + state checkpoints. Kill: <10% gain or ONE divergence.

### E3 — Load-path overlay coverage (Lever A, boring, possibly free)
If E0 shows dirty-RAM interp share >~10% in-window (overlay-region
decompressors are ChatGPT's own HLE-niche flag): coverage-capture the
load path, reshard, re-run E0. Interp is 10-50x native. Zero timing
risk. Kill: interp share already ≤~10% (skip).

### E4 — Data shards: verify-only shadow mode, then replay  [the spike]
Phase 1 `shards_shadow` (ChatGPT's addition): execute the original,
PREDICT the replay result, compare full outputs + live-outs + event
assumptions over a large sample — falsify before trusting. Gate to
proceed: decompressor ≥~20-25% of window host CPU (Amdahl: 20% share
caps the whole-window win at 1.25x), predicted hit rate ≥~80%, verify
cost ≤~20-25% of execution.
Phase 2 `shards_replay`: cold/warm A/B on the same transition. Gate:
>10% AND >100ms load reduction, no divergence across repeated loads +
world streaming.
**Correctness bar before ANY replay (ChatGPT, sharper than our v1):**
temporal write visibility — an atomic write-set + sliced cycle credit
cannot reconstruct what a mid-window IRQ observes. Replay is only
sound when IRQs are disabled across the recorded window OR the
recorded duration is strictly below the next observable device/IRQ
event; otherwise a time-indexed write log is required. Also: function
code/content hash in the key; full relevant entry registers (not just
a0-a3); live-outs beyond v0/v1; no pending exception; no DMA overlap;
code-invalidation for written pages; no reentrancy.
Kill: E0 says <~20% host share; hit rate <80%; verify ≈ execution
cost; any event lands inside a supposedly-atomic interval.

### E5 — Authentic drive backlog / never-early catch-up  [RECLASSIFIED: correctness, not acceleration]
ChatGPT's adversarial verdict on our parked "leaning fix": never-early
+ catch-up **cannot make a correct pipeline faster** — a healthy loader
already receives every sector on its authentic deadline. It only
removes *artificial lateness* if our model restarts sector timing from
guest ACK instead of the continuous physical-drive timeline (real
drives buffer sectors; a guest that services sector N late should find
N+1, N+2 already bufferable). Probe FIRST: measure deadline-vs-exposure
deltas — if baseline already exposes nearly every sector on time, this
has NO acceleration value and is pure drive-model correctness work.
Prereq: resolve the CD-model tree split (Ape direct-delivery vs master
model) — pick ONE canonical model.

### E6 — `cd_seek_speedup` (Lever B, bounded, per-game opt-in)
Probe first: seek/pause/spin share of window guest time. Upper bound =
number_of_seeks x ~250ms — a seek-only hack cannot touch a sequential
2MiB transfer. NOT proven-safe (DuckStation carries per-game disable
traits for seek speedup too; PPSSPP precedent: UMD-delay changes fixed
some titles, broke others). `pause_complete_delay_cycles` is a known
CPU-visible ordering contract (Ape memcard wedge) — stays authentic.
Kill: seek share <10% or <~250ms, or any callback/VBlank divergence.

### E7 — `cd_read_speedup_exp` (Lever B, Phase-2 only)
Per-title, with explicit XA/CDDA/MDEC and timing-sensitive exclusions
(the DuckStation trait architecture). First unexplained wedge ⇒
permanently non-default. Never foundation.

### E8 — Decompressor HLE (only via E4's failure)
Entered only if E0 shows decompression host-hot AND shards fail for an
identifiable reason (low reuse, expensive verify, interp-resident).
Kill: no material advantage over generated C or replay.

### E9 — BIOS CD-subsystem HLE — REJECTED (documented)
A faithful CdRead HLE must still preserve command submission, IRQ
ordering, callbacks, thread wakes, sector cadence, DMA-visible data,
retry behavior — and games bypass the BIOS anyway. The BIOS is already
native code; no host win unless E0 names BIOS CD bookkeeping as hot
(nothing suggests it). Its only credible payoff would be a centralized
validated CD state machine — that's an architecture question, not a
load-time one.

### E10 — Load-transition state cache (Lever C — the only TRUE near-zero; Phase-2)
known pre-load state → cached post-load state (conditional savestate
transition). Skips all intermediate observables by design ⇒ outside
the faithful foundation tier, per-game, opt-in. Validation bar
(ChatGPT): differential execution for THOUSANDS of frames after
restore, through at least one further load/scene transition — matching
the first rendered frame is not enough. Kill: any input, streaming
audio, memcard, or pending CD transaction inside the skipped interval.

**Burn-down order (= ChatGPT's session plan, merged):**
E0 → E1(a,b,c) → E2 → (E3 if E0 says so) → E4-shadow → E4-replay →
E5-probe → E6-probe → {E7, E8, E10 as Phase-2/conditional} — E9 stays
rejected on paper.

---

## 5. ChatGPT consult — key verdicts (2026-07-13 thread)

- **Decomposition check:** our three paths hold with one refinement —
  the foundation-compatible set is (1) advance authentic guest time
  faster, (2) remove host costs at identical guest cycles, (3)
  eliminate *accidental lateness* in the CD pipeline. Only these
  preserve the complete timeline. Everything else changes it.
- **Data shards presumed non-dominant until profiled.** Amdahl table:
  decompressor at 10/20/40/50/70% of window host CPU caps the
  whole-window replay ceiling at 1.11/1.25/1.67/2.0/3.33x. Feelable
  only if ~8 conditions hold simultaneously (§4 E4 gates).
- **Temporal write visibility is the spike's major correctness flaw**
  (§4 E4). Run verify-only shadow mode before any real replay.
- **No universally safe guest-visible CD acceleration exists.** Risk
  ladder: host preload/readahead (safest, host-only) → authentic
  buffering/backlog (safe if accurate) → seek-only (lower risk, NOT
  proven safe) → faster data rate → instant (highest). No public
  breakage denominator; claims like "seek speedup is 99% safe" would
  be invented numbers.
- **Never-early+catch-up is correctness work**, not acceleration (§4 E5).
- **New axis: event-horizon acceleration + device-batch advancement**
  (§4 E2) — the best unexplored host-side candidate; attacks the ~2x
  ceiling cause directly.
- **Turbo audio: fix at the host sink** (§4 E1b); never in guest state.
- **Prior art:** o2r/OTR → take the cache-identity/validation model;
  DuckStation preload = host-side robustness, not guest timing; Dolphin
  warns disabling disc-speed emulation crashes titles; PPSSPP UMD-delay
  history = storage latency is observable behavior. Closest true-zero
  analog = validated deterministic state-transition cache (§4 E10).
- **Plan of record (its words):** fix and profile turbo → prove/reject
  event-horizon acceleration → falsify data shards with verify-only
  instrumentation → treat authentic backlog as correctness work → keep
  seek/read acceleration per-game and non-foundational → reserve true
  near-zero for an explicitly classified load-transition cache.

---

## 6. Facts a future session will want (survey 2026-07-13)

- Live checkout = `feat/vigilante8-widescreen` (05fb36e); master =
  dde268d. CD models differ between them (§2 item 2).
- turbo detection/engage: `main.cpp:2062-2084`; burst present cadence
  `:2174-2180`; event pump `:2006-2009` (before early-returns); second
  pump `:2210` skipped in burst (input freshness only).
- CD scaling: `apply_speed` `cdrom.c:301-309` (XA exempt; divisor 0 =
  bounded instant via `instant_period()`); sector cadence base 451584
  cyc single-speed; `CDROM_IRQ_PRESENT_DELAY=5000` (ISR-teardown
  contract); pause completion deliberately NOT speed-scaled;
  seek-complete ≈ 4x sector period far / 0x800 near.
- Data shards wiring: emitter hook `code_generator.cpp:2211-2216` +
  `jr $ra` finalize `:1706`; chokepoints `memory.c:1135,1241,1363,1399`,
  `traps.c:540`; store `data_shards.c` (494 LOC, no TODOs); config
  `[data_shards] funcs` `config_loader.cpp:716-723`. `.dss` per shard,
  no manifest (doc/impl drift).
- bios_hle: B0 events 0x07-0x0D only; boot = one-shot shell-skip at
  `PSX_SHELL_ENTRY_PHYS` + boot-turbo until game start. A0/C0 observed,
  fall through.
- `_wt-tomba-loadshards-fw` worktree dir is EMPTY/unregistered — the
  spike lives only as branch `spike/tomba-load-shards` (fw) +
  `spike/load-shards` (TombaRecomp). Recreate worktrees before E4.
- `cdrom_bursts` ring (`cdrom.c:311-360`) already measures load
  durations — E0's window boundary source.
