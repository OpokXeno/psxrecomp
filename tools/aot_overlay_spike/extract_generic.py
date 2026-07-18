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
import argparse, os, sys, json, base64, struct, subprocess, tempfile, re
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

def rec(load_addr, data, seeds):
    return {"schema":"psxrecomp overlay capture v2","load_addr":f"0x{load_addr:08X}",
            "size":len(data),"bytes_b64":base64.b64encode(data).decode(),
            "executed_pcs":[],"dispatch_entry_pcs":[f"0x{a:08X}" for a in seeds],
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
    records=[]; np=nh=0
    for p,l,s in sorted(files):
        if s<8 or s>2_000_000: continue
        if exe_base and os.path.basename(p).upper()==exe_base:
            continue   # skip the statically-compiled base executable
        data=dr.read_file_bytes(l,s)
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
            # delta-sweep base recovery (best-effort; see plan doc limitations)
            ptrs=[x for x in struct.unpack_from(f'<{cnt}I',data,4) if x]
            lo0=(min(ptrs)&~0xFFF)
            best=None
            for b in range(lo0-0x4000, lo0+0x1000, 4):
                h=sum(1 for x in ptrs if 0<=x-b<=len(data)-4
                      and (struct.unpack_from('<I',data,x-b)[0]>>16)==0x27BD
                      and (struct.unpack_from('<I',data,x-b)[0]&0x8000))
                if best is None or h>best[1]: best=(b,h)
            base,hits=best
            if hits==0: continue
            records.append(rec(base,data,prologues(data,base)))
            nh+=1
    json.dump(records, open(a.out,'w'))
    print(f"producers: {np} PS-X EXE (full-discovery), {nh} header-table; {len(records)} regions -> {a.out}")

if __name__=='__main__': main()
