# AOT Overlay Sharding — Spike Tooling (WIP)

Play-free static overlay extraction: discover + compile overlay shards from the
disc image at build time, so a fresh install ships with overlay coverage instead
of relying entirely on runtime (tcc/gcc) autocompile. See `docs/AOT_OVERLAY_PLAN.md`
for the full design, findings, and honest coverage numbers.

**Status: SPIKE. Provably-correct for clean producers, not exhaustive.** These
scripts carry hardcoded paths (disc, scratchpad) and are Tomba-specific proofs of
concept, not a finished general tool. They exist so a future session can improve
discovery quality, not to be run as-is in a build.

## What each does

- `overlay_determinism.py` — corpus analysis across all games' captured overlays:
  base-address regularity, size/sector alignment, per-function code-CRC stability.
  Establishes that overlay code is deterministic (95-98% stable code-CRC).
- `tomba1_extract.py` — Tomba 1 (SCUS-94236). Enumerates ALL code overlays from
  the disc by header signature ({count:u32, ptr[count]:u32}), NOT just X*.BIN
  (that filter missed INFO.BIN → Dwarf Village ran interpreted/slow). Emits a
  synthetic overlay_captures.json for compile_overlays.py. Position-fixed,
  verbatim, seeds via prologue scan @ region+904.
- `tomba2_extract.py` — Tomba 2 (SCUS-94454). Two producers: MAIN.EXE (a PS-X EXE
  whose header self-describes the load address — ROBUST) and BIN/A*.BIN (header
  table; base recovery via prologue delta-sweep — UNRELIABLE, produces data-as-code).

## Validated result (see plan doc for detail)

- **Tomba 1:** 90% of played functions reproduced play-free + ~1964 unplayed
  extra; live-validated in-game (Dwarf Village went native, no runtime compile).
- **Tomba 2:** 661 functions byte-identical (entry+code_crc) to the shipped/played
  vault — provably real — ALL from the MAIN.EXE producer; the BIN producer yields
  0 usable shards (broken base recovery). Generality is producer-dependent.

## Correctness guarantee (why partial coverage is safe)

Every shard is content-addressed by per-function `code_crc`; the runtime loader
only executes a shard when live RAM byte-matches (`overlay_loader.c` lazy_man_matches).
A mis-positioned / data-as-code shard either fails audit at compile time or never
fires at runtime → coverage loss, never incorrect execution. Whatever static
misses, production autocompile (tcc/gcc) self-heals on first visit.

## Multi-game sweep findings (2026-07-17)

`extract_generic.py` runs the improved extraction from a game.toml. Results:

- **Full-discovery for PS-X-EXE producers WORKS & is validated.** Running the
  recompiler in NORMAL mode (jr-$ra boundary scan) finds frameless functions
  overlay-mode's prologue-scan misses. Tomba2 MAIN.EXE: prologue 710 fns / 62%
  vault -> full-discovery 786 fns / 69% vault (3296 shards, 0 audit fails);
  the tool now emits 3500 discovered seeds vs 1210 prologues.
- **Generality is ENGINE-SPECIFIC, not universal.** The two producer types
  (self-describing PS-X-EXE + {count,ptr[]} header-table) cover the Whoopee Camp
  engine (Tomba 1 header-table; Tomba 2 MAIN.EXE + BIN) but detect ZERO overlays
  on Vigilante 8 (Luxoflux) and Mega Man X6 (Capcom) — those load overlays in a
  different container/format (likely compressed/archived). Each engine family
  needs its own producer. Matches the Legaia-decomp finding: overlay enumeration
  is bespoke per engine.
- **KNOWN REGRESSION — header-table base recovery is unreliable.** The generic
  delta-sweep (find the base where header pointers hit prologues) produces
  scattered/wrong bases: on Tomba 1 it regresses the hand tool's known-correct
  FIXED base (0x800E7000, content @ +904), and on Tomba 2 BIN it decodes data as
  code (6 audit fails). Header-table producers still need a fixed/known base or
  real loader-table RE. Prefer the hand extractors (tomba1_extract.py) for
  header-table games until this is fixed.
- Bugs fixed by the sweep: multi-.bin cue (data-track selection), base-EXE
  exclusion, game.toml UTF-8 BOM, normal-mode .ranges format (`F <entry>` w/o crc).

## Next to raise coverage (future session)

1. Frameless-leaf + indirect-call-table seed discovery (helps every game; would
   lift MAIN.EXE past ~62% and Tomba 1 past 90%).
2. Real loader-table RE for header-table (BIN) base recovery (per-game).
3. Extract the kernel `0x80000000` region once from the BIOS (game-independent).
4. Generalize into a real `tools/` producer (no hardcoded paths; disc + game.toml in).
