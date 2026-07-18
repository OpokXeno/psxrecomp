#!/usr/bin/env python3
"""AOT static-coverage recall report + gap manifest.

Answers both "which played manifest entries did static extraction reproduce?"
and "which played entry PCs lie in compiled static code ranges?" The latter is
the effective code-coverage view when the played vault contains fine-grained
runtime fragments but static extraction emits broader functions.

Two ground-truth "needed" sources:
  * a played/coverage VAULT cache dir (.ranges from a full playthrough) — the most
    complete needed-set we have; the primary recall benchmark.
  * a runtime CAPTURES json (executed_pcs from a live session) — what a specific
    drive exercised; a live spot-check.

Recall is reported at three strengths:
  * entry-level      : same function ENTRY address discovered (did we find it?)
  * code-range       : played entry PC lies in an R range compiled by static AOT
  * entry+code_crc   : same entry AND identical bytes (is our shard the real fn?)

Usage:
  coverage_report.py --static <static-cache-dir> --vault <vault-cache-dir>
                     [--bios-dispatch <SCPHxxxx_dispatch.c>]
                     [--captures <runtime_overlay_captures.json>]
                     [--prior-report <gaps.json> --assume-static-superset]
                     --out-md <path.md> --out-json <path.json> [--game <id>]
Cache dirs are scanned recursively for *.ranges; a captures json is the v2 schema.
"""
import argparse, bisect, os, sys, json, re, glob
from collections import defaultdict

MASK = 0x1FFFFFFF

def norm(a): return a & MASK

def region_of(a):
    """Page-region label for grouping (aligns to the overlay window bases)."""
    return norm(a) & 0xFFFFF000

def merge_intervals(intervals):
    merged = []
    for lo, hi in sorted(intervals):
        if not merged or lo > merged[-1][1]:
            merged.append([lo, hi])
        elif hi > merged[-1][1]:
            merged[-1][1] = hi
    return [(lo, hi) for lo, hi in merged]

def parse_ranges_dir(d):
    """Return ({entry_masked: code_crc}, merged compiled-code intervals)."""
    ent = {}
    intervals = []
    for rf in glob.glob(os.path.join(d, '**', '*.ranges'), recursive=True):
        for line in open(rf, errors='ignore'):
            m = re.match(r'F\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)', line)
            if m:
                ent[norm(int(m.group(1), 16))] = int(m.group(2), 16)
                continue
            m = re.match(r'R\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)', line)
            if m:
                lo = norm(int(m.group(1), 16))
                intervals.append((lo, lo + int(m.group(2), 16)))
    return ent, merge_intervals(intervals)

def _c_initializer(text, declaration_pattern):
    """Return the braced initializer following a unique C declaration."""
    match = re.search(declaration_pattern, text)
    if not match:
        raise ValueError(f'missing generated-C declaration: {declaration_pattern}')
    start = text.find('{', match.end())
    if start < 0:
        raise ValueError('generated-C declaration has no initializer')
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == '{':
            depth += 1
        elif text[pos] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:pos]
    raise ValueError('unterminated generated-C initializer')

def parse_bios_dispatch(path):
    """Return base-BIOS dispatch entries and relocated kernel body ranges.

    The generated dispatcher has many unrelated numeric initializers, so parse
    only the two named tables instead of matching address triples globally.
    """
    with open(path, encoding='utf-8', errors='ignore') as generated:
        text = generated.read()
    dispatch = _c_initializer(
        text, r'\bDispatchEntry\s+dispatch_table\s*\[[^]]*\]\s*=')
    bodies = _c_initializer(
        text, r'\bPsxKernelBody\s+psx_bios_kernel_bodies\s*\[[^]]*\]\s*=')
    entries = {
        norm(int(value, 16))
        for value in re.findall(
            r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*func_[0-9A-Fa-f]+\s*\}', dispatch)
    }
    intervals = []
    for _, lo, hi in re.findall(
            r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*'
            r'0x([0-9A-Fa-f]+)u?\s*,\s*0x([0-9A-Fa-f]+)u?\s*\}', bodies):
        lo, hi = norm(int(lo, 16)), norm(int(hi, 16))
        if hi > lo:
            intervals.append((lo, hi))
    if not entries or not intervals:
        raise ValueError('generated BIOS dispatcher contained an empty static table')
    return entries, merge_intervals(intervals)

def parse_captures_executed(path):
    """-> set of FUNCTION-ENTRY addrs (masked) the runtime dispatched to.
    Use dispatch/function entries only — executed_pcs is per-basic-block PCs
    (many per function), which is not comparable to the vault's function-entry
    granularity and would pollute the recall denominator."""
    ent = set()
    for cap in json.load(open(path)):
        for key in ('dispatch_entry_pcs', 'function_entry_pcs'):
            for a in cap.get(key, []):
                ent.add(norm(int(a, 16)))
    return ent

def recall(needed_entries, static_ent, static_ranges, needed_crc=None):
    """needed_entries: set; static_ent: {entry->crc}. Returns dict of metrics."""
    covered = set(static_ent)
    hit = needed_entries & covered
    miss = needed_entries - covered
    range_starts = [lo for lo, _ in static_ranges]
    def in_static_range(entry):
        i = bisect.bisect_right(range_starts, entry) - 1
        return i >= 0 and entry < static_ranges[i][1]
    range_hit = {entry for entry in needed_entries if in_static_range(entry)}
    range_miss = needed_entries - range_hit
    out = {'needed': len(needed_entries), 'covered_here': len(hit),
           'missed': len(miss), 'recall_entry': (len(hit)/len(needed_entries) if needed_entries else 0.0),
           'misses': sorted(miss),
           'covered_by_code_range': len(range_hit),
           'missed_code_range': len(range_miss),
           'recall_code_range': (len(range_hit)/len(needed_entries) if needed_entries else 0.0),
           'range_misses': sorted(range_miss)}
    if needed_crc is not None:
        crc_hit = sum(1 for e in hit if static_ent.get(e) == needed_crc.get(e))
        out['covered_entry_crc'] = crc_hit
        out['recall_entry_crc'] = (crc_hit/len(needed_entries) if needed_entries else 0.0)
    return out

def group_by_region(addrs):
    g = defaultdict(list)
    for a in addrs: g[region_of(a)].append(a)
    return {r: sorted(v) for r, v in sorted(g.items())}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--static', required=True, help='static shard cache dir (.ranges)')
    ap.add_argument('--bios-dispatch',
                    help='generated base-BIOS dispatch C; adds already-native BIOS '
                         'entries/ranges to separate combined metrics')
    ap.add_argument('--vault', help='played/coverage vault cache dir (.ranges)')
    ap.add_argument('--captures', help='runtime overlay_captures.json (executed_pcs)')
    ap.add_argument('--prior-report', help='roll forward a persisted live gap set')
    ap.add_argument('--assume-static-superset', action='store_true',
                    help='assert current static entries retain every prior static entry')
    ap.add_argument('--game', default='UNKNOWN')
    ap.add_argument('--out-md', required=True)
    ap.add_argument('--out-json', required=True)
    a = ap.parse_args()

    if a.prior_report and not a.assume_static_superset:
        ap.error('--prior-report requires the explicit --assume-static-superset assertion')

    static_ent, static_ranges = parse_ranges_dir(a.static)
    report = {'game': a.game, 'static_entries': len(static_ent), 'sources': {}}
    combined_ent = dict(static_ent)
    combined_ranges = static_ranges
    if a.bios_dispatch:
        try:
            bios_ent, bios_ranges = parse_bios_dispatch(a.bios_dispatch)
        except (OSError, ValueError) as exc:
            ap.error(f'cannot parse --bios-dispatch: {exc}')
        combined_ent.update({entry: None for entry in bios_ent})
        combined_ranges = merge_intervals(static_ranges + bios_ranges)
        report['bios_dispatch'] = a.bios_dispatch
        report['bios_static_entries'] = len(bios_ent)
        report['bios_static_ranges'] = len(bios_ranges)

    md = []
    md.append(f'# AOT static-coverage recall — {a.game}\n')
    md.append('_How much of the played reference set did the play-free static '
              'extractor reproduce, and how much lies in compiled static code?_\n')
    md.append(f'- Static shard cache: `{a.static}`')
    md.append(f'- Static manifest entries: **{len(static_ent)}**\n')
    if a.bios_dispatch:
        md.append(f'- Base BIOS native dispatch entries: **{len(bios_ent)}**; '
                  f'relocated kernel body ranges: **{len(bios_ranges)}**')
        md.append('- Combined metrics below count both the play-free overlay cache '
                  'and the separately generated, live-byte-guarded base BIOS.\n')

    if a.vault:
        vault_ent, _ = parse_ranges_dir(a.vault)
        r = recall(set(vault_ent), static_ent, static_ranges, vault_ent)
        report['sources']['vault'] = {
            k: v for k, v in r.items() if k not in ('misses', 'range_misses')}
        report['vault_misses'] = [f'0x{x|0x80000000:08X}' for x in r['misses']]
        report['vault_range_misses'] = [
            f'0x{x|0x80000000:08X}' for x in r['range_misses']]
        combined = None
        if a.bios_dispatch:
            combined = recall(set(vault_ent), combined_ent, combined_ranges)
            report['sources']['vault_combined_bios'] = {
                k: v for k, v in combined.items() if k not in ('misses', 'range_misses')}
            report['vault_combined_range_misses'] = [
                f'0x{x|0x80000000:08X}' for x in combined['range_misses']]
        md.append('## vs played vault (most complete needed-set)\n')
        md.append(f'- Manifest entries in the full-playthrough vault: **{r["needed"]}**')
        md.append(f'- Discovered by static: **{r["covered_here"]}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Covered by compiled static code ranges: '
                  f'**{r["covered_by_code_range"]}** '
                  f'(**{r["recall_code_range"]*100:.1f}%** code-range recall)')
        md.append('  - Code-range recall answers whether the played entry PC is in '
                  'byte-guarded native code. Exact-entry recall is stricter manifest '
                  'granularity; runtime fragment caches can contain one entry per '
                  'instruction, so it substantially understates broad static shards.')
        md.append(f'- Byte-identical (entry+code_crc): **{r.get("covered_entry_crc",0)}** '
                  f'(**{r.get("recall_entry_crc",0)*100:.1f}%**) '
                  f'_(cg-version differences lower this vs entry-level)_')
        md.append(f'- **MISSED exact entries: {r["missed"]}**')
        md.append(f'- **TRUE CODE-RANGE GAPS: {r["missed_code_range"]}** played '
                  'entry PCs outside all compiled static ranges\n')
        if combined:
            md.append('### Combined with base recompiled BIOS\n')
            md.append(f'- Exact native dispatch entries: **{combined["covered_here"]}** '
                      f'(**{combined["recall_entry"]*100:.1f}%**)')
            md.append(f'- Covered by native code ranges: '
                      f'**{combined["covered_by_code_range"]}** '
                      f'(**{combined["recall_code_range"]*100:.1f}%**)')
            md.append(f'- **COMBINED CODE-RANGE GAPS: '
                      f'{combined["missed_code_range"]}**\n')
            md.append('#### Combined gaps grouped by region\n')
            for reg, addrs in group_by_region(combined['range_misses']).items():
                md.append(f'- region `0x{reg|0x80000000:08X}`: {len(addrs)} misses')
                md.append('  ```')
                for i in range(0, len(addrs), 8):
                    md.append('  ' + ' '.join(
                        f'{x|0x80000000:08X}' for x in addrs[i:i+8]))
                md.append('  ```')
            md.append('')
        md.append('### Code-range gaps grouped by overlay region\n')
        for reg, addrs in group_by_region(r['range_misses']).items():
            md.append(f'- region `0x{reg|0x80000000:08X}`: {len(addrs)} misses')
            md.append('  ```')
            for i in range(0, len(addrs), 8):
                md.append('  ' + ' '.join(f'{x|0x80000000:08X}' for x in addrs[i:i+8]))
            md.append('  ```')
        md.append('')

    if a.captures and os.path.exists(a.captures):
        live = parse_captures_executed(a.captures)
        r = recall(live, static_ent, static_ranges)
        report['sources']['live_session'] = {
            k: v for k, v in r.items() if k not in ('misses', 'range_misses')}
        report['live_misses'] = [f'0x{x|0x80000000:08X}' for x in r['misses']]
        report['live_range_misses'] = [
            f'0x{x|0x80000000:08X}' for x in r['range_misses']]
        combined = None
        if a.bios_dispatch:
            combined = recall(live, combined_ent, combined_ranges)
            report['sources']['live_session_combined_bios'] = {
                k: v for k, v in combined.items() if k not in ('misses', 'range_misses')}
            report['live_combined_range_misses'] = [
                f'0x{x|0x80000000:08X}' for x in combined['range_misses']]
        md.append('## vs live session captures (this drive)\n')
        md.append(f'- Dispatch entries this session exercised: **{r["needed"]}**')
        md.append(f'- Discovered by static: **{r["covered_here"]}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Covered by compiled static code ranges: '
                  f'**{r["covered_by_code_range"]}** '
                  f'(**{r["recall_code_range"]*100:.1f}%** code-range recall)')
        if combined:
            md.append(f'- Including base BIOS native code ranges: '
                      f'**{combined["covered_by_code_range"]}** '
                      f'(**{combined["recall_code_range"]*100:.1f}%**)')
        md.append(f'- **MISSED live: {r["missed"]}**\n')
    elif a.prior_report:
        with open(a.prior_report, encoding='utf-8') as prior_in:
            prior = json.load(prior_in)
        prior_live = prior.get('sources', {}).get('live_session')
        prior_misses = prior.get('live_misses')
        if not isinstance(prior_live, dict) or not isinstance(prior_misses, list):
            ap.error('--prior-report has no persisted live_session/live_misses')
        if len(static_ent) < int(prior.get('static_entries', 0)):
            ap.error('current static entry count is smaller than the prior report; '
                     'the asserted superset cannot hold')
        needed = int(prior_live.get('needed', 0))
        old_miss = {norm(int(x, 16)) for x in prior_misses}
        miss = old_miss - set(static_ent)
        range_starts = [lo for lo, _ in static_ranges]
        def in_static_range(entry):
            i = bisect.bisect_right(range_starts, entry) - 1
            return i >= 0 and entry < static_ranges[i][1]
        range_miss = {entry for entry in old_miss if not in_static_range(entry)}
        covered = needed - len(miss)
        r = {'needed': needed, 'covered_here': covered, 'missed': len(miss),
             'recall_entry': (covered/needed if needed else 0.0),
             'covered_by_code_range': needed - len(range_miss),
             'missed_code_range': len(range_miss),
             'recall_code_range': ((needed - len(range_miss))/needed if needed else 0.0)}
        r['provenance'] = ('rolled forward from the persisted live gap manifest; '
                           'caller asserted the current static entry set is a superset')
        report['sources']['live_session'] = r
        report['live_misses'] = [f'0x{x|0x80000000:08X}' for x in sorted(miss)]
        report['live_range_misses'] = [
            f'0x{x|0x80000000:08X}' for x in sorted(range_miss)]
        combined = None
        if a.bios_dispatch:
            combined_miss = old_miss - set(combined_ent)
            combined_starts = [lo for lo, _ in combined_ranges]
            def in_combined_range(entry):
                i = bisect.bisect_right(combined_starts, entry) - 1
                return i >= 0 and entry < combined_ranges[i][1]
            combined_range_miss = {
                entry for entry in old_miss if not in_combined_range(entry)}
            combined_covered = needed - len(combined_miss)
            combined = {
                'needed': needed,
                'covered_here': combined_covered,
                'missed': len(combined_miss),
                'recall_entry': (combined_covered/needed if needed else 0.0),
                'covered_by_code_range': needed - len(combined_range_miss),
                'missed_code_range': len(combined_range_miss),
                'recall_code_range': (
                    (needed - len(combined_range_miss))/needed if needed else 0.0),
                'provenance': r['provenance']}
            report['sources']['live_session_combined_bios'] = combined
            report['live_combined_range_misses'] = [
                f'0x{x|0x80000000:08X}' for x in sorted(combined_range_miss)]
        md.append('## vs persisted live-session gaps (monotonic roll-forward)\n')
        md.append(f'- Dispatch entries the persisted session exercised: **{needed}**')
        md.append(f'- Discovered by current static: **{covered}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Covered by current static code ranges: '
                  f'**{r["covered_by_code_range"]}** '
                  f'(**{r["recall_code_range"]*100:.1f}%** code-range recall)')
        if combined:
            md.append(f'- Including base BIOS native code ranges: '
                      f'**{combined["covered_by_code_range"]}** '
                      f'(**{combined["recall_code_range"]*100:.1f}%**)')
        md.append(f'- **MISSED live: {len(miss)}**')
        md.append('- Provenance: caller explicitly asserted that the current static '
                  'entry set retains every prior static entry.\n')

    for output_path in (a.out_md, a.out_json):
        output_dir = os.path.dirname(os.path.abspath(output_path))
        os.makedirs(output_dir, exist_ok=True)
    with open(a.out_md, 'w', encoding='utf-8', newline='\n') as out_md:
        out_md.write('\n'.join(md).rstrip('\n') + '\n')
    with open(a.out_json, 'w', encoding='utf-8', newline='\n') as out_json:
        json.dump(report, out_json, indent=1)
        out_json.write('\n')
    print(f'wrote {a.out_md}')
    print(f'wrote {a.out_json}')
    for src, m in report['sources'].items():
        print(f'  {src}: recall_entry={m.get("recall_entry",0)*100:.1f}% '
              f'({m.get("covered_here")}/{m.get("needed")}), '
              f'recall_code_range={m.get("recall_code_range",0)*100:.1f}%, '
              f'missed={m.get("missed")}')

if __name__ == '__main__':
    main()
