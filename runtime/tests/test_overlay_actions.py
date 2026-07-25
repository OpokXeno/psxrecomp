#!/usr/bin/env python3
"""N13 debug overlay write-action live test (build-dbg, PSX_DEBUG_OVERLAY=ON).

Drives the three new "Xenogears Debug" window sections (Map Teleport /
Party / Gold & Variables) over TCP via the SAME functions the in-window
widgets call (`overlay_widget_action` with names teleport/party_slot/
party_bitfield/gold/write_var). Every write goes through `psx_write_byte`
to the verified addrs.xml addresses; we never touch `fieldID` and never
call `loadNewField`.

Sequence (each step asserted on the wire):

    (a) Port coordination: poll ping; while answered wait 30s retry x40
        (the launcher may already be running from a prior test); when
        refused launch
        `setsid nohup ./build-dbg/XenogearsRecomp --no-launcher`
        from repo root; wait ping.
    (b) Boot to title (fieldID 490). Read fieldID; if not 490, fire
        teleport(490, 0) and poll until == 490 (or 30 s).
    (c) Fire teleport(1, 0) via overlay_widget_action; poll read_ram
        fieldID until == 1 (≤30 s). window_shot -> PNG exists and
        differs from a pre-teleport baseline.
    (d) 60 s REAL soak: sleep, ping every 10 s, frame counter strictly
        increasing; game still alive.
    (e) Fire teleport(490, 0); poll fieldID == 490 as return proof.
    (f) Party: write party=[0,2,0xFF] via three `party_slot` calls
        (value encoding: (charId << 8) | slot); read_ram of the kernel
        master slots 0x80062590 shows the three u32s. Set bitfield
        0x0005 via `party_bitfield`; read_ram shows it. Restore
        originals after.
    (g) Gold: write 999999 via `gold`; read_ram 0x8006EF58 == 999999
        (u32 LE). Restore.
    (h) Vars: read var[0] (GameProgress) value, write it back unchanged
        (no-op write), read_ram matches.
    (i) Kill game, verify port 4370 free at end.

Each step that writes to RAM captures the original bytes BEFORE the
write so it can restore them. Restores are best-effort but the test
fails if the post-test RAM state is not the same as the pre-test RAM
state for the addresses the spec demands we clean up.
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
import zlib


HOST = "127.0.0.1"
PORT = 4370

ROOT = "/home/pc/xenogears-port/XenogearsRecomp"
BUILD = os.path.join(ROOT, "build-dbg")
EXE   = os.path.join(BUILD, "XenogearsRecomp")
GAME_LOG = "/tmp/w5-game.log"

SHOT_MIN_BYTES = 1024
BOOT_TIMEOUT_S = 60.0
TITLE_POLL_S   = 30.0
LAHAN_POLL_S   = 30.0


def _recv_response(sock, timeout=5.0):
    sock.settimeout(timeout)
    buf = bytearray()
    depth = 0
    in_str = False
    esc = False
    started = False
    pos = 0
    while True:
        chunk = sock.recv(1 << 16)
        if not chunk:
            break
        buf.extend(chunk)
        n = len(buf)
        while pos < n:
            if in_str:
                if esc:
                    esc = False
                    pos += 1
                    continue
                q = buf.find(b'"', pos)
                bs = buf.find(b"\\", pos)
                if q == -1 and bs == -1:
                    pos = n
                    break
                if bs != -1 and (q == -1 or bs < q):
                    esc = True
                    pos = bs + 1
                    continue
                in_str = False
                pos = q + 1
                continue
            c = buf[pos]
            if c == 0x22:
                in_str = True
            elif c == 0x7B:
                depth += 1
                started = True
            elif c == 0x7D:
                depth -= 1
                if started and depth == 0:
                    return json.loads(buf[:pos + 1].decode())
            pos += 1
    return json.loads(buf.decode().strip())


def send_recv(req, timeout=5.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.settimeout(timeout)
        s.connect((HOST, PORT))
        s.sendall((json.dumps(req) + "\n").encode())
        return _recv_response(s, timeout)
    finally:
        s.close()


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def ok(msg):
    print(f"PASS: {msg}")


def wait_for_server(timeout=BOOT_TIMEOUT_S):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            r = send_recv({"id": 1, "cmd": "ping"}, timeout=2.0)
            if r.get("ok"):
                return r
        except (socket.error, OSError, ValueError):
            pass
        time.sleep(0.5)
    fail(f"debug server on {HOST}:{PORT} did not respond within {timeout}s")


def call(req_id, cmd, **extra):
    req = {"id": req_id, "cmd": cmd}
    req.update(extra)
    return send_recv(req)


def _png_ihdr_ok(path):
    try:
        with open(path, "rb") as f:
            sig = f.read(8)
            if sig != b"\x89PNG\r\n\x1a\n":
                return False, "bad signature"
            hdr = f.read(4 + 4 + 13 + 4)
            if len(hdr) != 25:
                return False, "short IHDR"
            length, kind = struct.unpack(">I4s", hdr[:8])
            if kind != b"IHDR":
                return False, "missing IHDR"
            if length != 13:
                return False, "bad IHDR length"
            w, h, depth, ctype = struct.unpack(">IIBB", hdr[8:8 + 10])
            return (w > 0 and h > 0 and depth == 8 and ctype in (2, 6)), \
                   f"IHDR ok ({w}x{h}, depth={depth}, ctype={ctype})"
    except OSError as e:
        return False, f"open failed: {e}"


def _wait_for_file(path, deadline_s=8.0):
    t0 = time.time()
    while time.time() - t0 < deadline_s:
        if os.path.exists(path) and os.path.getsize(path) >= 1:
            return os.path.getsize(path)
        time.sleep(0.05)
    return None


def read_ram_bytes(addr_hex, length):
    """Return raw bytes from read_ram. Length in bytes (returns 2*len hex chars)."""
    r = call(0, "read_ram", addr=addr_hex, len=length)
    if not r.get("ok"):
        fail(f"read_ram rejected: {r}")
    hx = r.get("hex", "")
    if len(hx) != length * 2:
        fail(f"read_ram returned {len(hx)} hex chars for {length} bytes (got {hx[:32]}...)")
    return bytes.fromhex(hx)


def read_ram_u16_le(addr_hex):
    b = read_ram_bytes(addr_hex, 2)
    return b[0] | (b[1] << 8)


def read_ram_u32_le(addr_hex):
    b = read_ram_bytes(addr_hex, 4)
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def field_id():
    """Read fieldID (u16 LE) from 0x8006F94E."""
    return read_ram_u16_le("0x8006F94E")


def field_context_ptr():
    """Read fieldContextPtr (u32 LE) at 0x800B0078."""
    return read_ram_u32_le("0x800B0078")


def wait_for_field_module(timeout_s):
    """Poll fieldContextPtr 0x800B0078 until non-zero (field module is
    the active module and the teleport poll is running). Returns the
    fieldID at the time of first sighting."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        fcp = field_context_ptr()
        if fcp != 0:
            return field_id()
        time.sleep(0.25)
    fail(f"fieldContextPtr 0x800B0078 stayed 0 for {timeout_s}s — field module never loaded")


def frame_count():
    """Read s_frame_count via the dedicated `frame` command (the I/O
    thread ping fast-path returns a smaller response without frame)."""
    r = call(0, "frame")
    if not r.get("ok"):
        fail(f"frame command failed: {r}")
    f = r.get("frame")
    if f is None:
        fail(f"frame response missing 'frame' field: {r}")
    return int(f)


def widget_action(name, value=0, value2=0, req_id=0):
    return call(req_id, "overlay_widget_action", name=name, value=value, value2=value2)


def teleport(field_id_, entry):
    """Fire teleport via the widget action path. Returns the action's rc
    (0 = armed, 1 = refused, <0 = failure)."""
    r = widget_action("teleport", value=field_id_, value2=entry)
    if not r.get("ok"):
        fail(f"teleport widget_action rejected: {r}")
    return r.get("rc", -1)


def wait_for_field(target, timeout_s, label):
    t0 = time.time()
    last = -1
    while time.time() - t0 < timeout_s:
        last = field_id()
        if last == target:
            return last
        time.sleep(0.25)
    fail(f"{label}: fieldID did not reach {target} within {timeout_s}s "
         f"(last={last})")


def coord_port():
    """Port coordination per spec: poll ping; while answered wait 30s
    retry x40; when refused launch the game and wait for it.

    Logic: try ping. If refused, launch immediately. If answered,
    sleep 30s and re-probe (the port may be held by a near-finished
    test run that hasn't released it yet). After 40 * 30s of waiting
    (20 minutes) of being answered, give up: the port is held by an
    orphan, fail.
    """
    for _ in range(40):
        try:
            r = send_recv({"id": 1, "cmd": "ping"}, timeout=2.0)
            if r.get("ok"):
                time.sleep(30.0)  # "while answered wait 30s retry x40"
                continue
        except (socket.error, OSError, ValueError):
            break
    else:
        # Loop completed 40 iterations with the server still answering.
        fail("server still answering after 30s x40 retries (port held by orphan?)")
    print("port free, launching game...")
    if os.path.exists(GAME_LOG):
        try:
            os.remove(GAME_LOG)
        except OSError:
            pass
    subprocess.Popen(
        ["setsid", "nohup", "./build-dbg/XenogearsRecomp", "--no-launcher"],
        stdout=open(GAME_LOG, "w"),
        stderr=subprocess.STDOUT,
        cwd=ROOT,
        start_new_session=True,
    )
    return time.time()


def main() -> int:
    boot_t0 = coord_port()
    wait_for_server()
    boot_elapsed = time.time() - boot_t0
    print(f"server up after {boot_elapsed:.1f}s; frame={frame_count()}")

    # ---- (b) Wait for field module to be active, then go to title (490) ----
    print("waiting for field module to become active (fieldContextPtr 0x800B0078 != 0)...")
    cur_field = wait_for_field_module(TITLE_POLL_S)
    print(f"  field module active; fieldID={cur_field}")
    if cur_field != 490:
        print(f"  booting further: teleporting to title (490)...")
        rc = teleport(490, 0)
        if rc != 0:
            fail(f"teleport(490) refused (rc={rc}); field module guard blocked it at boot")
        cur_field = wait_for_field(490, TITLE_POLL_S, "title")
    ok(f"step (b): at title (fieldID=490)")

    # Baseline window_shot (pre-teleport to Lahan).
    base_shot = "/tmp/w5_baseline_title.png"
    if os.path.exists(base_shot):
        os.remove(base_shot)
    r = call(11, "window_shot", path=base_shot)
    if not r.get("ok"):
        fail(f"baseline window_shot rejected: {r}")
    sz = _wait_for_file(base_shot)
    if sz is None or sz < SHOT_MIN_BYTES:
        fail(f"baseline window_shot too small/missing ({sz})")
    png_ok, png_info = _png_ihdr_ok(base_shot)
    if not png_ok:
        fail(f"baseline window_shot invalid: {png_info}")
    ok(f"step (b): baseline window_shot -> {base_shot} ({sz} bytes, {png_info})")

    # ---- (c) Fire teleport to Lahan (1) ----
    print("step (c): pre-teleport gate state:")
    print(f"  fieldMapNumber 0x8004F34C   = {read_ram_u32_le('0x8004F34C')}")
    print(f"  teleportGate1  0x800ADBEC   = {read_ram_u32_le('0x800ADBEC')}")
    print(f"  fieldChangePrev 0x800ADB64  = {read_ram_u32_le('0x800ADB64')}")
    print(f"  teleportArm    0x800ADBC4   = {read_ram_u32_le('0x800ADBC4')}")
    print(f"  teleportMusic  0x8004F308   = {read_ram_u32_le('0x8004F308')}")
    print(f"  teleportAnim   0x800ADB90   = {read_ram_u32_le('0x800ADB90')}")
    print(f"  loadFileIndex  0x8004F330   = {read_ram_u32_le('0x8004F330')}")
    print(f"  fieldEntryPt   0x8006EF66   = {read_ram_u16_le('0x8006EF66')}")
    rc = teleport(1, 0)
    if rc != 0:
        fail(f"teleport(1) refused (rc={rc}) — field module guard at title?")
    print("step (c): post-teleport gate state:")
    print(f"  fieldMapNumber 0x8004F34C   = {read_ram_u32_le('0x8004F34C')}")
    print(f"  teleportGate1  0x800ADBEC   = {read_ram_u32_le('0x800ADBEC')}")
    print(f"  fieldChangePrev 0x800ADB64  = {read_ram_u32_le('0x800ADB64')}")
    print(f"  teleportArm    0x800ADBC4   = {read_ram_u32_le('0x800ADBC4')}")
    print(f"  loadFileIndex  0x8004F330   = {read_ram_u32_le('0x8004F330')}")
    print(f"  fieldEntryPt   0x8006EF66   = {read_ram_u16_le('0x8006EF66')}")
    landed = wait_for_field(1, LAHAN_POLL_S, "lahan")
    ok(f"step (c): fieldID == 1 (Lahan) after teleport (verified {landed})")

    # Capture the new map and confirm it differs from the baseline.
    lahan_shot = "/tmp/w5_lahan.png"
    if os.path.exists(lahan_shot):
        os.remove(lahan_shot)
    r = call(12, "window_shot", path=lahan_shot)
    if not r.get("ok"):
        fail(f"lahan window_shot rejected: {r}")
    sz2 = _wait_for_file(lahan_shot)
    if sz2 is None or sz2 < SHOT_MIN_BYTES:
        fail(f"lahan window_shot too small/missing ({sz2})")
    if os.path.getsize(base_shot) == os.path.getsize(lahan_shot):
        # Identical file size alone is not proof, but a 0-byte diff would
        # be. We assert a hash difference below to make the proof airtight.
        pass
    h1 = _hash_file(base_shot)
    h2 = _hash_file(lahan_shot)
    if h1 == h2:
        fail(f"lahan shot is byte-identical to title baseline (no map change!)")
    ok(f"step (c): window_shot Lahan differs from title baseline "
       f"(base={os.path.getsize(base_shot)}B  lahan={sz2}B  hash distinct)")

    # ---- (d) 60s REAL soak: ping every 10s, frame counter increasing ----
    print("step (d): 60s soak starting...")
    f0 = frame_count()
    print(f"  t=0  frame={f0}")
    for tick in (10, 20, 30, 40, 50, 60):
        time.sleep(10.0)
        r = call(0, "ping")
        if not r.get("ok"):
            fail(f"soak tick t={tick}: ping failed ({r})")
        f = frame_count()
        if f <= f0:
            fail(f"soak tick t={tick}: frame did not advance (f0={f0}, f={f})")
        print(f"  t={tick}  frame={f}  (delta={f - f0})")
        f0 = f
    ok("step (d): 60s soak complete, frame counter strictly increasing, server alive")

    # ---- (e) Teleport back to title (490) ----
    rc = teleport(490, 0)
    if rc != 0:
        fail(f"teleport(490) back refused (rc={rc})")
    cur = wait_for_field(490, LAHAN_POLL_S, "return-to-title")
    ok(f"step (e): fieldID == 490 (title) after return teleport")

    # ---- (f) Party: kernel master slots (0x80062590, 3 x u32) + bitfield ----
    # gameState currentParty (0x8006F368) is a per-frame copy of the kernel
    # slots; the widget writes the master and gameState follows. Empty = 0xFF.
    party_addr = "0x80062590"
    bitf_addr  = "0x8006F364"
    orig_party = read_ram_bytes(party_addr, 12)
    orig_bitf  = read_ram_u16_le(bitf_addr)
    # Party = [0, 2, 0xFF] via three party_slot calls.
    # Encoding: value = (charId << 8) | slot; value2 = bitfieldBit (-1 to skip)
    for slot, chid in ((0, 0), (1, 2), (2, 0xFF)):
        v = (chid << 8) | slot
        r = widget_action("party_slot", value=v, value2=-1)
        if not r.get("ok") or r.get("rc", -1) != 0:
            fail(f"party_slot({slot}, {chid}) rejected: {r}")
    time.sleep(0.2)
    got = read_ram_bytes(party_addr, 12)
    want = bytes([0x00,0,0,0, 0x02,0,0,0, 0xFF,0,0,0])
    if got != want:
        fail(f"kernel party slots not written: got {got.hex()}, expected {want.hex()}")
    ok(f"step (f.1): kernel party slots = {got.hex()} (matches 0/2/FF u32s)")
    # Auto-bitfield: party_slot must OR bits 0+2 into the unlock bitfield.
    bf_auto = read_ram_u16_le(bitf_addr)
    if (bf_auto & 0x0005) != 0x0005:
        fail(f"auto-bitfield missing member bits: got 0x{bf_auto:04X}, want bits 0+2 set")
    ok(f"step (f.1b): unlock bitfield auto-ORed = 0x{bf_auto:04X} (bits 0+2 set)")

    # Set bitfield 0x0005 (bits 0 + 2) via party_bitfield.
    r = widget_action("party_bitfield", value=0x0005)
    if not r.get("ok") or r.get("rc", -1) != 0:
        fail(f"party_bitfield(0x0005) rejected: {r}")
    time.sleep(0.2)
    bf = read_ram_u16_le(bitf_addr)
    if bf != 0x0005:
        fail(f"bitfield not written: got 0x{bf:04X}, expected 0x0005")
    ok(f"step (f.2): party bitfield = 0x{bf:04X} (matches 0x0005)")

    # Restore originals (u32 slots; low byte = char id).
    for slot in range(3):
        chid = orig_party[slot * 4]
        v = (chid << 8) | slot
        r = widget_action("party_slot", value=v, value2=-1)
        if not r.get("ok") or r.get("rc", -1) != 0:
            fail(f"party_slot restore({slot}, {chid}) rejected: {r}")
    r = widget_action("party_bitfield", value=int(orig_bitf))
    if not r.get("ok") or r.get("rc", -1) != 0:
        fail(f"party_bitfield restore rejected: {r}")
    time.sleep(0.2)
    got2 = read_ram_bytes(party_addr, 3)
    bf2  = read_ram_u16_le(bitf_addr)
    if got2 != orig_party or bf2 != orig_bitf:
        fail(f"party restore mismatch: got party={got2.hex()} bf=0x{bf2:04X}, "
             f"expected party={orig_party.hex()} bf=0x{orig_bitf:04X}")
    ok(f"step (f.3): party restored (party={got2.hex()} bf=0x{bf2:04X})")

    # ---- (g) Gold: write 999999, read back, restore ----
    gold_addr = "0x8006EF58"
    orig_gold = read_ram_u32_le(gold_addr)
    r = widget_action("gold", value=999999)
    if not r.get("ok") or r.get("rc", -1) != 0:
        fail(f"gold(999999) rejected: {r}")
    time.sleep(0.2)
    g = read_ram_u32_le(gold_addr)
    if g != 999999:
        fail(f"gold not written: got {g}, expected 999999")
    ok(f"step (g.1): gold = {g} (matches 999999)")
    # Restore: gold widget action is the only writer, so call it again.
    r = widget_action("gold", value=int(orig_gold))
    if not r.get("ok") or r.get("rc", -1) != 0:
        fail(f"gold restore rejected: {r}")
    time.sleep(0.2)
    g2 = read_ram_u32_le(gold_addr)
    if g2 != orig_gold:
        fail(f"gold restore mismatch: got {g2}, expected {orig_gold}")
    ok(f"step (g.2): gold restored ({g2})")

    # ---- (h) Vars: read var[0] (GameProgress), write it back unchanged ----
    var_addr = "0x8006EF64"  # fieldVars[0]
    orig_var = read_ram_u16_le(var_addr)
    r = widget_action("write_var", value=0, value2=int(orig_var))
    if not r.get("ok") or r.get("rc", -1) != 0:
        fail(f"write_var(0, {orig_var}) rejected: {r}")
    time.sleep(0.2)
    v = read_ram_u16_le(var_addr)
    if v != orig_var:
        fail(f"var[0] mismatch after no-op write: got {v}, expected {orig_var}")
    ok(f"step (h): var[0] (GameProgress) round-tripped (0x{v:04X})")

    # ---- (i) Kill game, verify port 4370 free ----
    print("step (i): quitting the game...")
    r = call(99, "quit")
    if not r.get("ok"):
        print(f"  (informational: quit response: {r})")
    # Wait for the port to be released (TCP listen refuses when the process
    # is gone). 30s budget.
    t0 = time.time()
    while time.time() - t0 < 30.0:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1.0)
            s.connect((HOST, PORT))
            s.close()
            time.sleep(0.5)
        except (socket.error, OSError):
            ok("step (i): port 4370 free, game quit cleanly")
            print("ALL TESTS PASSED")
            return 0
    fail("port 4370 still accepting connections 30s after quit")


def _hash_file(path):
    h = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(65536)
            if not b:
                break
            h = zlib.crc32(b, h)
    return h


if __name__ == "__main__":
    sys.exit(main())
