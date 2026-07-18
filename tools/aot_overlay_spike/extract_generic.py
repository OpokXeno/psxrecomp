#!/usr/bin/env python3
"""Generic play-free overlay extractor (AOT spike, v2 — full-discovery producers).

Given a game.toml (disc + load_address + text_size), enumerate the disc and emit
a synthetic overlay_captures.json for compile_overlays.py. Two producer types:

 1. PS-X EXE files (magic "PS-X EXE") — the load address is self-described in the
    header. Run the recompiler in NORMAL mode (full discovery: jr-$ra boundary
    scan finds FRAMELESS functions overlay-mode's prologue-scan misses) to get the
    seed set, then split the body into overlay-floor-clamped window regions.
    This is the ROBUST, high-coverage producer (Tomba2 MAIN.EXE: 62%->69% shards,
    4.6x more functions than prologue-only overlay mode).

 2. Header-table files ({count:u32, ptr[count]:u32} with in-range pointers) —
    prologue-scan seeds at a delta-swept base. LESS robust (base recovery
    unreliable when header ptrs aren't clean function starts); see plan doc.

Usage:
  extract_generic.py --game-toml <path> --recompiler <psxrecomp-game.exe>
                     --out <captures.json> [--tmp <dir>]
Requires extract_overlays.py (same dir chain) for the DiscReader/ISO9660 walker.
"""
import argparse, os, sys, json, base64, struct, subprocess, tempfile, re, binascii
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import extract_overlays as eo
try:
    import tomllib
except ImportError:
    import tomli as tomllib

def parse_cue_datatrack(cue_path):
    """Return (bin_path, is_raw) for the FIRST data (MODE) track — the ISO9660
    filesystem lives there. eo.parse_cue picks the LAST FILE, which breaks
    multi-bin cues (separate audio-track .bin files after the data track)."""
    cue_dir=os.path.dirname(os.path.abspath(cue_path))
    cur_bin=None
    with open(cue_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m=re.match(r'\s*FILE\s+"([^"]+)"\s+BINARY', line, re.IGNORECASE)
            if m: cur_bin=os.path.join(cue_dir, m.group(1))
            t=re.match(r'\s*TRACK\s+\d+\s+MODE(\d)/(\d+)', line, re.IGNORECASE)
            if t and cur_bin:
                return cur_bin, (int(t.group(2))==2352)
    return eo.parse_cue(cue_path)   # fallback

def prologues(data, base):
    return [base+off for off in range(0,len(data)-4,4)
            if (struct.unpack_from('<I',data,off)[0]>>16)==0x27BD
            and (struct.unpack_from('<I',data,off)[0]&0x8000)]

def prologue_offsets(data):
    """File offsets whose word is an `addiu $sp,$sp,-N` stack-frame prologue."""
    return [off for off in range(0,len(data)-4,4)
            if (struct.unpack_from('<I',data,off)[0]>>16)==0x27BD
            and (struct.unpack_from('<I',data,off)[0]&0x8000)]

def jal_targets(data):
    """Distinct absolute j/jal targets. These are BASE-INDEPENDENT: the target
    is 0x80000000 | (imm26<<2) regardless of where the file loads, because all
    PSX game code lives in the 0x800xxxxx region (PC[31:28] is always 0x8)."""
    ts=set()
    for off in range(0,len(data)-4,4):
        w=struct.unpack_from('<I',data,off)[0]
        if (w>>26) in (0x02,0x03):                 # j / jal
            ts.add(0x80000000 | ((w & 0x03FFFFFF) << 2))
    return ts

def recover_base(data, ptrs):
    """Recover the verbatim load base of a position-fixed header-table overlay.

    The export-table pointers point at mid-function DISPATCH entries, not
    prologues, so matching them against 0x27BD is useless (0 hits at the true
    base — measured). Instead use jal-target self-consistency: jal targets are
    base-independent, and at the TRUE base the maximum number of intra-overlay
    jal targets land on a real 0x27BD prologue. This peaks uniquely and sharply
    on the correct base (~2x margin over runner-up on Tomba 1's 22 overlays).

    Returns (base, score) or None when the signal is too weak to trust (safe:
    a skipped overlay is coverage loss, never wrong execution — the runtime's
    per-function code_crc dispatch guard rejects any mis-based shard anyway)."""
    import bisect
    from collections import Counter
    nonnull=[p for p in ptrs if p]
    if not nonnull: return None
    n=len(data)
    # Containment: every export pointer must map to a valid in-file offset when
    # the file loads verbatim at `base`  =>  0 <= ptr-base <= n-4.
    win_hi=min(nonnull)                 # base <= min(ptr)
    win_lo=max(nonnull)-(n-4)           # base >= max(ptr)-(n-4)
    if win_hi < win_lo: return None
    P=prologue_offsets(data)
    if not P: return None
    # For each jal target t and prologue offset o, base (t-o) makes t hit a
    # prologue. Tally votes over bases inside the containment window; the peak
    # base is the one the most distinct jal targets agree on.
    hist=Counter()
    for t in jal_targets(data):
        lo_i=bisect.bisect_left(P, t-win_hi)
        hi_i=bisect.bisect_right(P, t-win_lo)
        for o in P[lo_i:hi_i]:
            hist[t-o]+=1
    if not hist: return None
    top=hist.most_common(2)
    base,score=top[0]
    second=top[1][1] if len(top)>1 else 0
    if score < 4 or score < second*1.5:      # demand a clear, unique-ish peak
        return None
    return base, score

def is_psx_exe(data):
    return len(data) >= 0x20 and data[:8] == b'PS-X EXE'

def is_header_table(data):
    if len(data) < 8: return None
    cnt = struct.unpack_from('<I', data, 0)[0]
    if not (1 <= cnt <= 4096) or 4+cnt*4 > len(data): return None
    ptrs = struct.unpack_from(f'<{cnt}I', data, 4)
    inr = sum(1 for p in ptrs if 0x80010000 <= p < 0x80200000 and (p&3)==0)
    nul = sum(1 for p in ptrs if p == 0)
    return cnt if (inr+nul == cnt and inr >= 1) else None

def full_discovery_seeds(data, recompiler, tmp):
    """Run recompiler NORMAL mode on a PS-X EXE; return discovered function entries."""
    exe_path = os.path.join(tmp, 'producer.exe')
    open(exe_path,'wb').write(data)
    out = os.path.join(tmp, 'disc_out'); os.makedirs(out, exist_ok=True)
    try:
        # errors='replace': the recompiler prints non-ASCII (✓) that would raise
        # a decode error under text=True and lose the (already-written) ranges.
        subprocess.run([recompiler, exe_path, '--out-dir', out],
                       capture_output=True, text=True, errors='replace', timeout=300)
    except Exception as e:
        print(f"    full-discovery failed ({e}); falling back to prologue scan"); return None
    seeds=set()
    for rf in [f for f in os.listdir(out) if f.endswith('.ranges')]:
        for line in open(os.path.join(out,rf), errors='ignore'):
            m=re.match(r'F ([0-9A-Fa-f]+)', line)   # normal-mode: "F <entry>" (no crc suffix)
            if m:
                a=int(m.group(1),16); seeds.add(a if a>=0x80000000 else (0x80000000|a))
    return sorted(seeds) or None

def rec(load_addr, data, seeds, dispatch_extra=None):
    # function_entry_pcs = prologue-scanned function starts (walk roots).
    # dispatch_entry_pcs = those PLUS any extra dispatch targets (e.g. the
    # overlay's own export table). compile_overlays promotes clean starts to
    # entries and mid-function dispatch targets to DISPATCH_INTERIOR aliases
    # (impossible_entry_start guards garbage), so passing the game's authoritative
    # dispatch table here recovers indirect-only entries a prologue scan misses.
    disp = sorted(set(seeds) | set(dispatch_extra or ()))
    return {"schema":"psxrecomp overlay capture v2","load_addr":f"0x{load_addr:08X}",
            "size":len(data),"bytes_b64":base64.b64encode(data).decode(),
            "executed_pcs":[],"dispatch_entry_pcs":[f"0x{a:08X}" for a in disp],
            "function_entry_pcs":[f"0x{a:08X}" for a in seeds],
            "seeds":[f"0x{a:08X}" for a in seeds]}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--game-toml', required=True)
    ap.add_argument('--recompiler', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--tmp', default=None)
    a=ap.parse_args()
    doc=tomllib.loads(open(a.game_toml, encoding='utf-8-sig').read())  # utf-8-sig strips BOM
    game=doc.get('game',doc)
    root=os.path.dirname(os.path.abspath(a.game_toml))
    disc_rel=game.get('disc') or doc.get('game',{}).get('disc')
    load_addr=int(str(game.get('load_address','0x80010000')),16)
    text_size=int(str(game.get('text_size','0x00080000')),16)
    floor = (load_addr + text_size) & 0x1FFFFFFF
    floor_page = (floor // 0x1000) * 0x1000        # page-align down
    disc=os.path.join(root, disc_rel)
    print(f"game={game.get('id')} disc={os.path.basename(disc)} load=0x{load_addr:08X} floor=0x{floor_page:08X}")
    bp,raw=parse_cue_datatrack(disc)   # multi-bin safe: pick track-1 data bin
    dr=eo.DiscReader(bp,raw=raw)
    files=list(eo.enumerate_files(dr))
    tmp=a.tmp or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    # The boot EXE (game.toml `exe`) is the STATIC-recompiled base, NOT an overlay.
    exe_base=os.path.basename(str(game.get('exe',''))).upper()
    records=[]; np=nh=0; seen_crc=set()
    for p,l,s in sorted(files):
        if s<8 or s>2_000_000: continue
        if exe_base and os.path.basename(p).upper()==exe_base:
            continue   # skip the statically-compiled base executable
        data=dr.read_file_bytes(l,s)
        # Position-fixed overlays are reused verbatim across AREA folders; dedup
        # by content so identical bytes -> one shard set, not N (same as hand tool).
        dcrc=binascii.crc32(data)&0xFFFFFFFF
        if dcrc in seen_crc: continue
        seen_crc.add(dcrc)
        if is_psx_exe(data):
            t_addr=struct.unpack_from('<I',data,0x18)[0]
            body=data[2048:]
            seeds_all=full_discovery_seeds(data, a.recompiler, tmp)
            if seeds_all is None: seeds_all=prologues(body, t_addr)
            # split body into floor-clamped window regions
            base=t_addr & 0x1FFFFFFF
            end=base+len(body)
            cut = floor_page if base < floor_page < end else None
            spans=[(base, cut)] if cut else [(base,end)]
            if cut: spans.append((cut,end))
            for lo,hi in spans:
                seg=body[lo-base:hi-base]
                va=0x80000000|lo
                sd=[x for x in seeds_all if lo<=(x&0x1FFFFFFF)<hi]
                records.append(rec(va,seg,sd))
            np+=1
            print(f"  [PS-X EXE] {p}: {len(body)}B @0x{0x80000000|base:08X}, full-disc seeds={len(seeds_all)}")
        else:
            cnt=is_header_table(data)
            if not cnt: continue
            # jal-target self-consistency base recovery (robust; supersedes the
            # old prologue-density delta-sweep, which matched export pointers
            # against prologues they never point at -> wrong base by 2-16KB).
            ptrs=struct.unpack_from(f'<{cnt}I',data,4)
            rb=recover_base(data, ptrs)
            if rb is None:
                print(f"  [header-table] {p}: base signal too weak, SKIPPED (safe)")
                continue
            base,score=rb
            seeds=prologues(data,base)   # seed via full-file prologue scan
            # SUPPLEMENT the prologue seeds with the overlay's OWN export/dispatch
            # table (the header pointers) — the game's authoritative list of live
            # entry points, sitting on the disc (play-free). These are exactly the
            # indirect-dispatch-only / interior entries a prologue scan can't see;
            # compile_overlays emits them as DISPATCH_INTERIOR aliases as needed.
            hdr_entries=[p for p in ptrs
                         if p and (p&3)==0 and base <= p < base+len(data)]
            # The runtime keys shards by a PAGE-ALIGNED region_start (the first
            # dirty page of the overlay's contiguous run), and compile_overlays
            # takes the capture's load_addr verbatim as that key (phys = load_addr
            # & 0x1FFFFFFF, no re-align). So we must present a page-aligned base
            # with a FILL prefix bridging page_base -> file base, exactly as the
            # played-vault layout for these position-fixed overlays. Seeds are
            # absolute so they are unaffected; region bytes stay byte-identical.
            page_base = base & ~0xFFF
            fill = base - page_base
            region = b'\x00'*fill + data
            records.append(rec(page_base, region, seeds, dispatch_extra=hdr_entries))
            nh+=1
            extra=len(set(hdr_entries)-set(seeds))
            print(f"  [header-table] {p}: {len(data)}B file@0x{base:08X} "
                  f"region@0x{page_base:08X}(+{fill}) "
                  f"(jal-fit score={score}), prologue seeds={len(seeds)} "
                  f"+{extra} export-table dispatch entries")
    json.dump(records, open(a.out,'w'))
    print(f"producers: {np} PS-X EXE (full-discovery), {nh} header-table; {len(records)} regions -> {a.out}")

if __name__=='__main__': main()
