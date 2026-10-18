#!/usr/bin/env python3
"""Live Native Vulkan debug-view test; run against a private gameplay instance.

Requires Pillow and PSX_DEBUG_OVERLAY. Use --port and --output explicitly;
the caller owns the runtime, display, savestate and settings isolation.
"""

import argparse
import json
from pathlib import Path
import socket
import time

from PIL import Image, ImageChops


def run(port: int, output: Path, timeout: float) -> None:
    output.mkdir(parents=True, exist_ok=True)
    sequence = 0

    def call(command, **parameters):
        nonlocal sequence
        sequence += 1
        request = dict(parameters, id=sequence, cmd=command)
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as connection:
            connection.settimeout(timeout)
            connection.sendall(json.dumps(request).encode() + b"\n")
            with connection.makefile("rb") as stream:
                response = json.loads(stream.readline(1 << 20))
        assert response.get("id") == sequence, response
        assert response.get("ok"), response
        return response

    def services():
        response = call("native_vk_services")
        assert not response.get("fallback_reason"), response
        state = response["native_vk_services"]
        assert state["active"], state
        return state

    def change(name, view=0, wire=0):
        before = services()
        call("overlay_widget_action", name="native_depth_view", value=view)
        call("overlay_widget_action", name="native_wireframe", value=wire)
        deadline = time.monotonic() + timeout
        # A guest display update can span several Native journals. Wait for
        # both GPU submissions and visible swaps, not just a few partial works.
        while True:
            current = services()
            if (current["executor"]["works"] >= before["executor"]["works"] + 12 and
                    current["swaps"] >= before["swaps"] + 12):
                break
            assert time.monotonic() < deadline, "Native work stopped advancing"
            time.sleep(0.05)
        assert call("native_depth_diag")["view"] == view
        path = output / f"{name}.png"
        call("vk_present_shot", path=str(path.resolve()))
        with Image.open(path) as image:
            # Exclude the menu bar: it is UI, not part of the diagnostic image.
            pixels = image.convert("RGB").crop((0, 48, image.width, image.height))
        state = services()
        executor = state["executor"]
        assert not executor["failed_works"], executor
        assert not any(executor["refusals"].values()), executor
        assert not state["compose_failures"] and not state["swap_failures"], state
        print(f"Captured {name}: {executor['tier']}, {executor['works']} works", flush=True)
        return pixels

    try:
        # A cold-process gameplay load must not fall back to the legacy packet
        # parser: that silently loses the widened/subpixel geometry on worldmap.
        assert call("native_stream_diag")["stream"]["enabled"], "Native packet routing was not restored"
        baseline = change("baseline")
        colour = change("depth-colour", view=1)
        policy = change("depth-policy", view=2)
        grey = change("depth-grey", view=3)
        wire = change("wireframe", wire=1)
        info = call("vk_info")
        (output / "vk-info.json").write_text(json.dumps(info, indent=2) + "\n")
        (output / "services.json").write_text(json.dumps(services(), indent=2) + "\n")
        for name, image in (("colour", colour), ("policy", policy), ("grey", grey), ("wire", wire)):
            assert ImageChops.difference(baseline, image).getbbox(), f"{name} did not change the image"
        grey_data = grey.tobytes()
        grey_pixels = list(zip(grey_data[0::3], grey_data[1::3], grey_data[2::3]))
        certified = [(r, g, b) for r, g, b in grey_pixels if max(r, g, b) > 16]
        assert len(certified) > 100, "Depth view has no visible certified surfaces"
        assert sum(max(p) - min(p) <= 2 for p in certified) > len(certified) * 0.95, "Depth grey is not greyscale"
        wire_data = wire.tobytes()
        wire_pixels = list(zip(wire_data[0::3], wire_data[1::3], wire_data[2::3]))
        assert sum(g > r + 8 and g > b + 8 for r, g, b in wire_pixels) > 100, "No wireframe edges"
        assert sum(max(p) < 8 for p in wire_pixels) > len(wire_pixels) * 0.5, "Wireframe still fills the scene"
        assert not info["vk_info"]["validation"]["errors"], info
    finally:
        call("overlay_widget_action", name="native_depth_view", value=0)
        call("overlay_widget_action", name="native_wireframe", value=0)
    change("restored")
    print("PASS Native Vulkan depth views and wireframe", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.timeout <= 0:
        parser.error("invalid port or timeout")
    run(args.port, args.output, args.timeout)
