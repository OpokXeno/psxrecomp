#!/usr/bin/env python3
"""Multi-byte write_ram roundtrip + validation tests.

Exercises the extended write_ram handler that accepts {"hex": "..."} for
multi-byte writes alongside the legacy {"val": "..."} single-byte form.
Assumes the game is already running on 127.0.0.1:4370 (--no-launcher or
PSX_NO_LAUNCHER=1). The test snapshots 16 bytes at 0x801F0000, exercises
the new + legacy paths, and restores the original content on exit.
"""

import json
import socket
import sys
import time


HOST = "127.0.0.1"
PORT = 4370
TEST_ADDR = 0x801F0000
TEST_LEN = 16

PAYLOAD = bytes(range(16))  # 00 01 02 ... 0F — easy to spot in a roundtrip
PAYLOAD_HEX = PAYLOAD.hex()


def _recv_response(sock, timeout=5.0):
    """Read one brace-balanced JSON object from sock, string-aware."""
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
                # Fast-skip to next quote or backslash.
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
            if c == 0x22:        # '"'
                in_str = True
            elif c == 0x7B:      # '{'
                depth += 1
                started = True
            elif c == 0x7D:      # '}'
                depth -= 1
                if started and depth == 0:
                    return json.loads(buf[:pos + 1].decode())
            pos += 1
    return json.loads(buf.decode().strip())


def send_recv(req, timeout=5.0):
    """Open a fresh connection, send one JSON-line request, read one response."""
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


def wait_for_server(timeout=60.0):
    """Poll ping until the server is up or the deadline hits."""
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


def read_ram(addr, length):
    return send_recv({"id": 1, "cmd": "read_ram",
                      "addr": f"0x{addr:08X}", "len": length})


def write_legacy(addr, byte):
    """Single-byte write via the legacy {"val": ...} form."""
    return send_recv({"id": 1, "cmd": "write_ram",
                      "addr": f"0x{addr:08X}", "val": f"0x{byte:02X}"})


def write_hex(addr, hex_str):
    return send_recv({"id": 1, "cmd": "write_ram",
                      "addr": f"0x{addr:08X}", "hex": hex_str})


def main() -> int:
    wait_for_server()

    # Snapshot the original 16 bytes so we can restore them on exit.
    snap = read_ram(TEST_ADDR, TEST_LEN)
    if not snap.get("ok"):
        fail(f"could not snapshot test region: {snap}")
    original_hex = snap["hex"]
    original_bytes = bytes.fromhex(original_hex)
    print(f"snapshot @ 0x{TEST_ADDR:08X} = {original_hex}")

    try:
        # ---- Test 1: multi-byte hex write roundtrip ----
        r = write_hex(TEST_ADDR, PAYLOAD_HEX)
        if not r.get("ok"):
            fail(f"hex write rejected: {r}")
        r2 = read_ram(TEST_ADDR, TEST_LEN)
        if not r2.get("ok"):
            fail(f"read after hex write failed: {r2}")
        if r2["hex"].lower() != PAYLOAD_HEX.lower():
            fail(f"hex roundtrip mismatch: wrote {PAYLOAD_HEX} read {r2['hex']}")
        ok(f"hex write/read roundtrip ({TEST_LEN} bytes) matches")

        # ---- Test 2: odd-length hex rejected ----
        r = write_hex(TEST_ADDR, "abc")  # 3 chars, odd
        if r.get("ok"):
            fail("odd-length hex was accepted (expected rejection)")
        msg = r.get("error", "").lower()
        if "even" not in msg and "at least 2" not in msg:
            fail(f"odd-length error message unexpected: {r}")
        ok(f"odd-length hex rejected: {r['error']}")

        # ---- Test 3: > 0x1000 bytes rejected ----
        too_long = "ab" * (0x1000 + 1)  # 0x1001 bytes = 0x2002 hex chars
        r = write_hex(TEST_ADDR, too_long)
        if r.get("ok"):
            fail(">0x1000 hex was accepted (expected rejection)")
        msg = r.get("error", "").lower()
        if "too long" not in msg and "even" not in msg:
            fail(f"too-long error message unexpected: {r}")
        ok(f">0x1000 byte hex rejected: {r['error']}")

        # ---- Test 4: legacy val form still works ----
        r = write_legacy(TEST_ADDR, 0xAA)
        if not r.get("ok"):
            fail(f"legacy val write rejected: {r}")
        r3 = read_ram(TEST_ADDR, 1)
        if not r3.get("ok"):
            fail(f"read after legacy val write failed: {r3}")
        if r3["hex"].lower() != "aa":
            fail(f"legacy val roundtrip mismatch: wrote AA read {r3['hex']}")
        ok("legacy val form still works")
    finally:
        # Restore the original bytes via the legacy single-byte path.
        restore_ok = True
        for j, b in enumerate(original_bytes):
            r = write_legacy(TEST_ADDR + j, b)
            if not r.get("ok"):
                print(f"WARN: failed to restore byte {j}: {r}")
                restore_ok = False
        snap_after = read_ram(TEST_ADDR, TEST_LEN)
        if snap_after.get("ok") and snap_after["hex"].lower() == original_hex.lower():
            print(f"restored @ 0x{TEST_ADDR:08X} = {original_hex}")
        else:
            print(f"WARN: restore mismatch: was {original_hex} "
                  f"now {snap_after.get('hex')}")
            restore_ok = False
        if not restore_ok:
            print("WARN: restore did not fully complete (check log)")

    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
