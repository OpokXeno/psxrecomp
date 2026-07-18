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
- **Tomba 2:** the clean play-free cache now covers 1264/1856 played entries
  (**68.1%**) and byte-matches 1144 (**61.6% entry+code_crc**). Direct-call
  provenance for frameless leaves added 74 MAIN.EXE hits over the 60.0% baseline;
  recovering the four call-sparse files added another 32 overlay-window hits;
  strict raw-code recovery of CRD/SOP added 43 more played hits, and the first
  audited adjacent-envelope shard added 2 more.
  All 22 A*.BIN overlays converge on 0x80108F9C (region 0x80108000 +3996),
  DEMO/GAME converge on 0x80106228, and OPN resolves to 0x8018A000.

## Correctness guarantee (why partial coverage is safe)

Every shard is content-addressed by per-function `code_crc`; the runtime loader
only executes a shard when live RAM byte-matches (`overlay_loader.c` lazy_man_matches).
A mis-positioned / data-as-code shard either fails audit at compile time or never
fires at runtime → coverage loss, never incorrect execution. Whatever static
misses, production autocompile (tcc/gcc) self-heals on first visit.

## Durable runtime capture history

The canonical `overlay_captures.json` is atomically replaced for live consumers.
With `[runtime] overlay_capture_history = true`, each changed coherent snapshot is
also appended as one independent record to
`overlay_captures.addendum.jsonl` beside the executable. A torn final line does
not invalidate earlier or later records. Merge it into the private additive vault
with:

```sh
python tools/coverage_vault.py merge --vault <vault-dir> \
  --addendum <exe-dir>/overlay_captures.addendum.jsonl
```

Dev configs can additionally set the project-relative
`overlay_capture_persist_dir` to retain immutable per-snapshot JSON files. Both
settings are opt-in; capture artifacts contain game bytes and must stay ignored.

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

## Raw-code and adjacent-envelope recovery (2026-07-18)

Not every position-fixed code file has a pure `{count, ptr[]}` export header.
`recover_raw_base()` applies a stricter unbounded jal/prologue vote: score >= 8,
at least 2x the runner-up, and at least 25% of apparent prologues agreeing on one
RAM-fitting base. This recovers Tomba 2 `CRD.BIN` at `0x8018A000` (32:3) and
`SOP.BIN` at `0x80108F9C` (12:3). Both are byte-identical to multiple played-vault
images; their two shards compile with zero unsupported instructions or bad
targets. It also finds Tomba 1 `OPTSUB00.BIN` (exact played-vault match) and
`DSPSUB2.BIN` (99.96% live-byte match, consistent with runtime fixups), without
touching the Tomba 1 cache.

Runtime dirty-page capture may coalesce directly adjacent files under the earlier
page key. The extractor therefore also emits exact two-producer concatenations.
For Tomba 2, `GAME.BIN` ends exactly where every A*/SOP file begins, producing
`0x80106000` envelope variants in addition to the standalone `0x80108000` shards.
The A01 composite—the live attract scene's measured hot variant—audits cleanly
(459 functions, zero unsupported/bad targets). A clean static-only soak loaded
that exact `00106000_0DC32D96` shard and ran the formerly ~14 fps lava-area attract
demo at 65–67 fps while the runtime process remained BelowNormal. The scene rendered
correctly, with no range-index or lazy-manifest overflow.

`coverage_report.py` reports both exact manifest-entry recall and compiled
code-range recall. The distinction matters: runtime/autocompile vaults may contain
one-entry fragments at consecutive instructions, while static AOT emits one broad
function with the same PCs in its guarded `R` ranges. Exact-entry parity is retained
for continuity; code-range recall is the useful uncovered-code roadmap. At the
current Tomba 2 checkpoint these are 1270/1856 (68.4%) exact entries and 1811/1856
(97.6%) overlay-cache code-range coverage. Pass the generated base BIOS dispatcher
with `--bios-dispatch generated/SCPH1001_dispatch.c` to report separate combined
metrics. The base recompiler already covers relocated kernel bodies with a live-byte
guard; including it raises Tomba 2 to 1852/1856 (99.8%) native code-range coverage.
This avoids generating duplicate overlay shards for code that is already native.

## Next to raise coverage (future session)

1. ~~Direct-call frameless-leaf discovery~~ — DONE. Reachable `jal` targets are
   emitted as trusted `call_root` seeds; same-basic-block constant-register
   `jalr`/tail-`jr` targets are resolved with clobber checks. Exact-entry recall
   first rose by 74 MAIN.EXE entries. Exact-entry boundary verification now also
   retains normal-mode frameless leaves aligned by NOPs after a preceding return;
   this added 13 clean MAIN definitions and closed every remaining MAIN code-range
   gap. The generated-C audit remains zero unsupported / zero bad targets.
   Position-fixed producers also scan dense runs of at least three in-range
   function pointers. A target is promoted only when it independently has a
   prologue or preceding return boundary, so isolated pointer-shaped data and
   mid-function labels remain excluded. Read-only Tomba 1 regression data proves
   this finds its indirect-only leaves at 0x80111BB4/0x80111BCC; all 11 new
   Tomba 2 targets audit cleanly in their standalone image variants.
2. ~~Header-table base recovery~~ — DONE (jal-fit, above).
3. ~~Recover weak-signal A09/A0J/GAME/OPN~~ — DONE. They are ordinary uncompressed
   MIPS, not archives. Counting only callable `jal` targets sharply resolves
   A09/A0J/OPN; GAME's 8-byte near-tie resolves through the exact base independently
   proved by DEMO. The played vault byte-proves GAME at 0x80106228. Ambiguous files
   without one unique trusted match still fail closed.
4. ~~Extract the kernel `0x80000000` region once from the BIOS~~ â€” NOT NEEDED for
   the measured body gaps. The base BIOS recompiler already emits 499 relocated,
   guarded kernel bodies. Of the four combined gaps, A0/B0/C0 are tiny installed
   vector thunks and EE7C is an all-zero runtime-fragment artifact; keep the latter
   interpreted rather than treating padding/data as code.
   this is now the complete 45-PC Tomba 2 code-range gap set.
5. Generalize into a real `tools/` producer (no hardcoded paths; disc + game.toml in).
