# LOAD_TIME_ZERO — proposal checklist for loads-toward-0

Branch: `spike/load-time-zero` (worktree `_wt-loadtime-zero`).
Status: **exploration only — no code changes.** This doc is the burn-down
list; each experiment gets its own future session, its own flag, and a
stated kill criterion. Written 2026-07-13 from a memory sweep, a code
survey of the live tree, and a ChatGPT consult (thread: "psxrecomp
workspace" project chat, 2026-07-13).

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
(We got this wrong in both instant-loads postmortems before writing it
down — the "loads are bounded by authentic CD cadence" phrasing conflates
the two factors.)

That yields exactly three levers, in ascending order of danger:

- **Lever A — cut host cost per guest second.** Timing-invariant, safe by
  construction: the guest sees identical cycles/IRQ phase, only the wall
  clock shrinks. Data shards, overlay coverage in the load path, renderer
  cost in-window, kernel bless (already shipped). Ceiling = memcpy speed;
  if the window's host profile is dominated by shard-able work, 10x+
  in-window rates are plausible.
- **Lever B — cut guest time of the window.** Title-sensitive, the proven
  wedge zone (disc_speed=4x/instant broke MMX6's VSync-callback tick even
  after the response arbiter was correct). The principled shape is the
  never-early + catch-up pacing rule (§4 E4).
- **Lever C — skip guest work entirely.** Twice-proven hardlock class
  (yield pumps r1/r2); state synthesis lives here too.

Corollary: **any Lever-A win multiplies with turbo and is invisible
without it.** A shard replay with cycle-faithful credit does not shorten
the load by one guest cycle — it raises the turbo multiplier.

---

## 2. Prior-attempt ledger (do not re-litigate; verbatim outcomes)

1. **turbo_loads — SHIPPED, production path.** Engage = sustained
   `cdrom_load_in_progress()` + game-started, 20-frame hysteresis
   (`main.cpp:2062-2084`); presents 1-in-30; audio deliberately NOT muted
   (queue capped). ~2x wall on Tomba loads with a healthy overlay cache.
   NOTE (survey 2026-07-13): the primary SDL event pump now runs BEFORE
   the turbo early-return (`main.cpp:2006-2009`), so the historical
   "Not Responding" starvation appears fixed in code — needs live
   re-validation, not re-fixing. Still open: MMX5 dev-tools+turbo boot
   wedge (0xE10 exception storm) — foundation timing bug.
2. **disc_speed=4x/instant — PROVEN UNSAFE as a divisor.** Changes how
   many VBlanks fall between guest state transitions; MMX6 VSync-callback
   tick counter freezes → boot wedge, even with the response arbiter
   (overwrites 24→0) and fixed (non-scaled) cmd latency. `instant` today
   is bounded (`instant_max_per_frame`, default 32 sectors/frame) — an
   IRQ-storm cap, not a safety rule. The postmortem's leaning fix — a
   read-pipeline pacing rule (never deliver a sector EARLIER than its
   authentic 1x arrival; catch up only when the guest is behind) — was
   never built. ⚠ Tree split: the live checkout runs the Ape
   direct-delivery CD model (no arbiter); master's model differs (debt
   clamp at `master:cdrom.c:1633`). Any CD experiment must first name its
   canonical tree.
3. **Instant-loads round 1 (gen-time yield pump) — ABANDONED.** Tomba's
   loader `FUN_80021340` is also the live world-streamer; skipping its
   cooperative yields starves co-thread/IRQ progress → gameplay hardlock.
4. **Instant-loads round 2 (syscall-level yield skip) — PARKED.** By
   ChangeTh-syscall time the guest wrapper has already committed
   green-thread yield bookkeeping; ONE skipped switch is eventually
   fatal. Kept: the `[instant_loads]` inert scaffold (load-grid detector,
   window plumbing, TCP counters) and the measure-first method.
5. **Data shards — BUILT on `spike/tomba-load-shards` (2a4e574), never
   regen'd/run.** Memoized pure-function replay: read-set/write-set/
   reg-out/cycle-cost capture at emitter-hooked funcs, byte-verified
   replay (the compare IS the proof), recorded cycles credited in slices
   through the normal advance+IRQ path. Purity poisons: MMIO, DMA write,
   thread switch, 4MiB budget, staleness. Persists per-shard `.dss`.
   Known v1 fidelity deltas (documented): ISR cycles double-credited;
   IRQ arriving mid-credit observes the completed write-set; GTE/COP0
   state not captured (poison candidates if hit). Design doc:
   `spike/tomba-load-shards:docs/DATA_SHARDS.md`.

Also relevant, shipped: FMV auto-skip (unpaced + muted + game-native
teardown), HLE boot shell-skip + boot-turbo window (`bios_hle.c`),
kernel bless (byte-verify-then-trust precedent).

---

## 3. Verdicts on the session's naive musings (be honest in future sessions)

- **"o2r equivalent — see it once and shard it"** → this IS the
  data-shards spike, already designed and built. Not a new idea; the open
  work is *measurement* (its own Gate 1) and first live run. o2r proper
  (AOT asset transformation) is unavailable to a recomp: we have no
  source-level asset knowledge, so capture-on-encounter is the correct
  analog.
- **"HLE out the decompression"** → almost certainly dominated by data
  shards. In a static recomp the decompressor already runs as native C;
  a hand-written HLE re-implementation buys a small constant on host cost
  but must still charge faithful guest cycles (else Lever-B wedge
  classes), so under turbo it converges to the shard win with far more
  per-game RE and a new correctness surface. Only niche where HLE-out
  wins: a function whose read-set churns (shard always misses) but whose
  algorithm is stable — measure shard hit-rate before even considering.
- **"Skip decompression" literally (don't charge the cycles)** → that is
  Lever B/C wearing Lever A's clothes; the cycle credit is what keeps
  the green-thread scheduler, IRQ phase, and CD interlocks sane. Any
  "instant shards" mode is a Phase-2 per-game enhancement gated on the
  faithful default proving out.

---

## 4. The burn-down checklist

Rules of engagement, every experiment: (1) measure FIRST via always-on
rings (freeze_check Mcyc/s across the window, `phase_hot set=static`,
`dirty_ram_stats` per_pc, `cdrom_bursts`); (2) behind a flag, default
off; (3) one experiment per session; (4) kill criterion stated before
the first line of code; (5) validate on ≥2 titles (Tomba + MMX6 minimum
— they broke differently every time).

### E0 — Host-cost attribution of a real load window  [MEASUREMENT — GATES EVERYTHING]
Profile a real pig window (area transition or Load Game) on a healthy
overlay cache: where does host time go — sharded-candidate guest funcs,
interp (per_pc), GP0/raster, MDEC, runtime overhead? Output: a table
naming the dominant term with %. No kill criterion (it's a probe), but
every later experiment cites its numbers. Prereq: the master LOAD GAME
in-exception wedge (memcard defer class) may block the Load-Game repro —
use area transitions if so.

### E1 — Data shards, first live run (Lever A)  [the flagship]
Regen+reshard on `spike/tomba-load-shards`, hook the decompressor(s) E0
names, capture cold / replay warm, A/B the same transition.
- Gate 1: shard-able work ≥ ~30% of in-window host cost (from E0), else
  park with numbers.
- Gate 2: purity — decompressor traces clean; if it reads MMIO/DMA move
  the boundary to the per-asset unpack leaf.
- Gate 3: user-feelable — warm wall-time delta on the same load ≥ 25%
  with turbo on; measure cold-run capture overhead too (a capture run
  slower than baseline by >10% is a shipping problem).
- Kill: Gate 1 fails, or replay hit-rate < ~50% on revisited areas
  (read-set churn), or any soak wedge.

### E2 — Load-path overlay coverage (Lever A, boring, probably free win)
If E0 shows interp share > ~10% in-window: coverage-capture the load
path (loader funcs, decompressors in overlay regions), reshard, re-run
E0. Interp is 10-50x native; this raises the turbo multiplier with ZERO
timing risk. Kill: interp share already ≤ ~10% (skip).

### E3 — Turbo hardening (Lever A hygiene)
(a) Re-validate the SDL pump fix under a long burst (survey says fixed;
prove it — window Responding + keyboard input mid-load). (b) Audio
policy during burst: current = play-through with capped queue; consider
short fade-in/out at engage/disengage edges instead. (c) Root-cause the
MMX5 dev-tools+turbo 0xE10 boot wedge — it's a foundation timing bug
that turbo merely exposes; fixing it likely hardens ALL accelerated-time
paths (same family as the MMX6 IRQ-phase regression). (d) Adaptive
present cadence (present on content-change rather than 1-in-30).
Kill: none — these are defects, not bets.

### E4 — CD read-pipeline pacing rule (Lever B, foundation-grade)
The parked leaning-fix from the adaptive-instant postmortem: model the
drive as always-reading-ahead into a host-side buffer, but **never
expose a sector to the guest earlier than its authentic 1x (or SetMode
2x) arrival time; when the guest falls behind (misses its deadline),
deliver immediately on ACK — catch-up semantics**. Guest that paces
itself sees authentic cadence (MMX6-safe by construction); guest that
waits-on-data gets sectors at the protocol floor. This is DuckStation's
read-speedup made adaptive per-consumer instead of global.
- Prereq: resolve the CD-model tree split (Ape direct-delivery vs master
  arbiter) — pick/merge ONE canonical model first.
- Gate: MMX6 boots at effective >1x (the title that killed every prior
  attempt); Tomba + Ape load/save soak; Beetle-oracle divergence check
  on CD IRQ timelines.
- Kill: MMX6 wedges again, or the rule degenerates to 1x for the titles
  that matter (i.e., games always keep up → no catch-up window ever
  opens → zero win; measure how often the guest is actually behind).
- Honest skepticism: round-2's data says games DRAIN queues one step per
  frame — a game that never falls behind gains nothing here. This lever
  pays off only for guests that busy-wait on data-ready. E0/E4's first
  probe must measure "guest-behind time" before building anything.

### E5 — Seek/spin latency-only reduction (Lever B, narrow probe)
Measure first: what fraction of window guest-time is seek/pause/spin
(`seek_complete_delay_cycles`, `pause_complete_delay_cycles`,
initial-read latency) vs sector transfer? If seeks are material, try
scaling ONLY seek-class latencies (keep data rate + command latencies
authentic) — the folklore-safe subset of DuckStation's hacks. Note we
already learned `pause_complete_delay_cycles` is a CPU-visible ordering
contract (Ape memcard wedge) — that one is load-bearing and stays.
- Kill: seeks < ~15% of window guest time (likely for streaming loaders,
  plausible for many-small-file loaders), or any ordering-contract
  violation resurfaces.

### E6 — HLE CD-subsystem carve-out (probably REJECT; write down why)
HLE'ing BIOS CdRead/CdSync doesn't change sector cadence unless it lies
about timing (Lever B in disguise) and doesn't reduce host cost below
the recompiled C it replaces (Lever A already covered). Fails the
HLE-tier bar ("genuine LLE landmine") — there is none here. Revisit only
if E0 shows BIOS CD-driver host overhead dominating, which nothing so
far suggests.

### E7 — Boundary state snapshotting (Lever C — LAST, Phase-2, per-game)
Savestate keyed by load inputs at a load boundary; replay = restore.
Invalidation story is brutal (memcard state, RNG, pad state, audio
position all leak into the snapshot), it's per-game by nature, and it's
the hardlock class. Keep on the list only so future sessions see it was
considered and rejected for foundation work.

Burn-down order: **E0 → E1 (+E2 if E0 says so) → E3 → E4 → E5**, E6/E7
documented-rejected unless E0 overturns assumptions.

---

## 5. ChatGPT consult — findings merge

*(Filled from the 2026-07-13 consult; see thread for full text.)*

<!-- CHATGPT-MERGE -->

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
  contract); pause completion deliberately NOT speed-scaled.
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
  `spike/load-shards` (TombaRecomp). Recreate worktrees before E1.
