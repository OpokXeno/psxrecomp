#!/usr/bin/env python3
"""AOT static-coverage recall report + gap manifest.

Answers: "of every overlay function the game ACTUALLY runs, what fraction did our
play-free static extractor discover?" — and persists the ones it MISSED so we
have a durable reference and a scoreboard for the discovery tooling.

Two ground-truth "needed" sources:
  * a played/coverage VAULT cache dir (.ranges from a full playthrough) — the most
    complete needed-set we have; the primary recall benchmark.
  * a runtime CAPTURES json (executed_pcs from a live session) — what a specific
    drive exercised; a live spot-check.

"Covered" = the entries our static shard cache (.ranges) defines. Recall is
reported at two strengths:
  * entry-level      : same function ENTRY address discovered (did we find it?)
  * entry+code_crc   : same entry AND identical bytes (is our shard the real fn?)

Usage:
  coverage_report.py --static <static-cache-dir> --vault <vault-cache-dir>
                     [--captures <runtime_overlay_captures.json>]
                     --out-md <path.md> --out-json <path.json> [--game <id>]
Cache dirs are scanned recursively for *.ranges; a captures json is the v2 schema.
"""
import argparse, os, sys, json, re, glob
from collections import defaultdict

MASK = 0x1FFFFFFF

def norm(a): return a & MASK

def region_of(a):
    """Page-region label for grouping (aligns to the overlay window bases)."""
    return norm(a) & 0xFFFFF000

def parse_ranges_dir(d):
    """-> dict entry_masked -> code_crc (last wins); also the raw (entry,crc) set."""
    ent = {}
    for rf in glob.glob(os.path.join(d, '**', '*.ranges'), recursive=True):
        for line in open(rf, errors='ignore'):
            m = re.match(r'F\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)', line)
            if m:
                ent[norm(int(m.group(1), 16))] = int(m.group(2), 16)
    return ent

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

def recall(needed_entries, static_ent, needed_crc=None):
    """needed_entries: set; static_ent: {entry->crc}. Returns dict of metrics."""
    covered = set(static_ent)
    hit = needed_entries & covered
    miss = needed_entries - covered
    out = {'needed': len(needed_entries), 'covered_here': len(hit),
           'missed': len(miss), 'recall_entry': (len(hit)/len(needed_entries) if needed_entries else 0.0),
           'misses': sorted(miss)}
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
    ap.add_argument('--vault', help='played/coverage vault cache dir (.ranges)')
    ap.add_argument('--captures', help='runtime overlay_captures.json (executed_pcs)')
    ap.add_argument('--game', default='UNKNOWN')
    ap.add_argument('--out-md', required=True)
    ap.add_argument('--out-json', required=True)
    a = ap.parse_args()

    static_ent = parse_ranges_dir(a.static)
    report = {'game': a.game, 'static_entries': len(static_ent), 'sources': {}}

    md = []
    md.append(f'# AOT static-coverage recall — {a.game}\n')
    md.append('_How much of what the game actually runs did the play-free static '
              'extractor discover? Misses are the gap the runtime autocompile '
              '(or better discovery) must still cover._\n')
    md.append(f'- Static shard cache: `{a.static}`')
    md.append(f'- Static-covered overlay functions: **{len(static_ent)}**\n')

    if a.vault:
        vault_ent = parse_ranges_dir(a.vault)
        r = recall(set(vault_ent), static_ent, vault_ent)
        report['sources']['vault'] = {k: v for k, v in r.items() if k != 'misses'}
        report['vault_misses'] = [f'0x{x|0x80000000:08X}' for x in r['misses']]
        md.append('## vs played vault (most complete needed-set)\n')
        md.append(f'- Functions the full playthrough ran: **{r["needed"]}**')
        md.append(f'- Discovered by static: **{r["covered_here"]}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Byte-identical (entry+code_crc): **{r.get("covered_entry_crc",0)}** '
                  f'(**{r.get("recall_entry_crc",0)*100:.1f}%**) '
                  f'_(cg-version differences lower this vs entry-level)_')
        md.append(f'- **MISSED: {r["missed"]}** functions the game needs but static did not find\n')
        md.append('### Misses grouped by overlay region\n')
        for reg, addrs in group_by_region(r['misses']).items():
            md.append(f'- region `0x{reg|0x80000000:08X}`: {len(addrs)} misses')
            md.append('  ```')
            for i in range(0, len(addrs), 8):
                md.append('  ' + ' '.join(f'{x|0x80000000:08X}' for x in addrs[i:i+8]))
            md.append('  ```')
        md.append('')

    if a.captures and os.path.exists(a.captures):
        live = parse_captures_executed(a.captures)
        r = recall(live, static_ent)
        report['sources']['live_session'] = {k: v for k, v in r.items() if k != 'misses'}
        report['live_misses'] = [f'0x{x|0x80000000:08X}' for x in r['misses']]
        md.append('## vs live session captures (this drive)\n')
        md.append(f'- Functions this session exercised: **{r["needed"]}**')
        md.append(f'- Discovered by static: **{r["covered_here"]}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- **MISSED live: {r["missed"]}**\n')

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
              f'({m.get("covered_here")}/{m.get("needed")}), missed={m.get("missed")}')

if __name__ == '__main__':
    main()
