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
  table). Superseded for base recovery by `extract_generic.py`'s jal-fit method.

## Validated result (see plan doc for detail)

- **Tomba 1:** 90% of played functions reproduced play-free + ~1964 unplayed
  extra; live-validated in-game (Dwarf Village went native, no runtime compile).
- **Tomba 2:** the clean play-free cache now covers 1187/1856 played entries
  (**64.0%**) and byte-matches 1084 (**58.4% entry+code_crc**). Direct-call
  provenance for frameless leaves added 74 MAIN.EXE hits over the 60.0% baseline.
  The BIN header-table producer recovers a correct, self-consistent common base
  (jal-fit, below): 21 A*.BIN overlays converge on region 0x80108000 (+3996), with
  4 weak-signal files safely skipped.

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
- **SOLVED (2026-07-17) — header-table base recovery via jal-target self-consistency.**
  The old delta-sweep matched the export-table pointers against prologues, but those
  pointers point at mid-function DISPATCH entries, not prologues (measured: 0/29 hit
  a prologue at the true base) — so it scattered by 2-16KB. Root cause & fix:
  `j`/`jal` targets are BASE-INDEPENDENT (`0x80000000 | (imm26<<2)`, since all game
  code is in 0x800xxxxx). At the TRUE base, the maximum number of intra-overlay jal
  targets land on a real 0x27BD prologue. `recover_base()` votes every (jal-target,
  prologue-offset) pair for base `t-o` inside the pointer-containment window; the
  peak is unique and sharp (~2x margin over runner-up). Then the record is emitted
  PAGE-ALIGNED with a FILL prefix (page_base + (file_base - page_base)), because the
  runtime keys shards by a page-aligned region_start and compile_overlays takes
  load_addr verbatim (`phys = load_addr & 0x1FFFFFFF`, no re-align).
  - **Validation:** generic output is byte-identical (base, region envelope, seed
    set) to the live-proven `tomba1_extract.py` hand tool for ALL 22 Tomba 1
    overlays — recovered 0x800E7388 (region 0x800E7000 +904) with no hardcoded
    constants. Tomba 2's 21 A*.BIN overlays converge on 0x80108F9C (region
    0x80108000 +3996). Weak-signal files are skipped (score<4 or <1.5x runner-up),
    which is safe: coverage loss, never wrong execution, per the CRC guard.
  - The hand tools (tomba1/tomba2_extract.py) remain only as the offline oracle for
    this validation; `extract_generic.py` is now the general path.
- Bugs fixed by the sweep: multi-.bin cue (data-track selection), base-EXE
  exclusion, game.toml UTF-8 BOM, normal-mode .ranges format (`F <entry>` w/o crc).

## Next to raise coverage (future session)

1. ~~Direct-call frameless-leaf discovery~~ — DONE. Reachable `jal` targets are
   emitted as trusted `call_root` seeds; same-basic-block constant-register
   `jalr`/tail-`jr` targets are resolved with clobber checks. Tomba 2 vault recall
   rose 60.0% -> 64.0% (+74 MAIN.EXE entries), with 23/23 shards compiling cleanly.
   Continue with statically proven function-pointer/jump-table consumers; the
   first corpus sweep correctly rejected apparent constants clobbered by `lw`.
2. ~~Header-table base recovery~~ — DONE (jal-fit, above).
3. Recover the weak-signal skipped overlays (Tomba 2 A09/A0J/GAME/OPN): likely
   compressed/archived or too few intra-overlay calls; may need a decompressor or
   a lower-confidence seed source before jal-fit can lock a base.
4. Extract the kernel `0x80000000` region once from the BIOS (game-independent).
5. Generalize into a real `tools/` producer (no hardcoded paths; disc + game.toml in).
