#!/usr/bin/env python3
"""Overlay visibility TCP command test.

Exercises `overlay_state` (read) and `overlay_toggle` (flip) on the
in-game debug overlay (build-dbg / PSX_DEBUG_OVERLAY=ON). Mirrors
test_write_ram_multibyte.py: each call opens a fresh connection,
sends one JSON-line request, and reads one brace-balanced response.

Sequence (state must land as expected at every step):
    1. overlay_state    -> visible:false   (cold start; overlay hidden)
    2. overlay_toggle   -> visible:true    (flips to on)
    3. overlay_state    -> visible:true    (round-trip read)
    4. overlay_toggle   -> visible:false   (flips back off)

Assumes the game is already running on 127.0.0.1:4370 (--no-launcher or
PSX_NO_LAUNCHER=1). The test does NOT launch the game itself; the
external harness owns lifecycle.
"""

import json
import socket
import sys
import time


HOST = "127.0.0.1"
PORT = 4370


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


def call(req_id, cmd):
    return send_recv({"id": req_id, "cmd": cmd})


def main() -> int:
    wait_for_server()

    # The overlay boots hidden. If a prior run toggled it and crashed, the
    # next boot starts clean — but only because the process is fresh. To be
    # safe (in case a sibling test left it on) we explicitly force a known
    # starting state by toggling until we see the value we want, then
    # running the four-step sequence relative to that.
    #
    # Cleaner approach: probe, and if visible, toggle once. The four
    # assertions below assume the post-probe state is hidden.
    r = call(1, "overlay_state")
    if not r.get("ok"):
        fail(f"initial overlay_state rejected: {r}")
    if r.get("visible") is not True:
        # already hidden — proceed
        start_visible = False
    else:
        # someone left it on; flip back to establish a known baseline
        rt = call(2, "overlay_toggle")
        if not rt.get("ok") or rt.get("visible") is not False:
            fail(f"could not reset overlay to hidden: {rt}")
        start_visible = False
    print(f"baseline: overlay hidden (started_visible={start_visible})")

    # ---- Step 1: overlay_state -> visible:false ----
    r = call(10, "overlay_state")
    if not r.get("ok"):
        fail(f"overlay_state 1 rejected: {r}")
    if r.get("visible") is not False:
        fail(f"expected visible:false, got {r}")
    ok("step 1: overlay_state -> visible:false")

    # ---- Step 2: overlay_toggle -> visible:true ----
    r = call(11, "overlay_toggle")
    if not r.get("ok"):
        fail(f"overlay_toggle 1 rejected: {r}")
    if r.get("visible") is not True:
        fail(f"expected visible:true after first toggle, got {r}")
    ok("step 2: overlay_toggle -> visible:true")

    # ---- Step 3: overlay_state -> visible:true (round-trip read) ----
    r = call(12, "overlay_state")
    if not r.get("ok"):
        fail(f"overlay_state 2 rejected: {r}")
    if r.get("visible") is not True:
        fail(f"expected visible:true on re-read, got {r}")
    ok("step 3: overlay_state -> visible:true (round-trip)")

    # ---- Step 4: overlay_toggle -> visible:false (flip back) ----
    r = call(13, "overlay_toggle")
    if not r.get("ok"):
        fail(f"overlay_toggle 2 rejected: {r}")
    if r.get("visible") is not False:
        fail(f"expected visible:false after second toggle, got {r}")
    ok("step 4: overlay_toggle -> visible:false")

    # ---- Leave the overlay hidden for the next test in the suite. ----
    r = call(14, "overlay_state")
    if r.get("visible") is not False:
        # Defensive: toggle until hidden.
        call(15, "overlay_toggle")

    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
