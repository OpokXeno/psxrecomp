#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# How to run: python3 psxrecomp/runtime/tests/test_teleport_position_reset.py

import time
from typing import Final

from test_overlay_actions import fail, field_context_ptr, read_ram_u16_le, teleport


FIELD_POSITION_ADDRS: Final = ("0x8006EF82", "0x8006EF84", "0x8006EF86")
FIELD_TIMEOUT_S: Final = 60.0
FIELD_SETTLE_S: Final = 10.5
EARLY_RETRY_S: Final = 2.0


def read_saved_position() -> tuple[int, int, int]:
    return tuple(read_ram_u16_le(addr) for addr in FIELD_POSITION_ADDRS)


def wait_for_nonzero_saved_position(timeout_s: float) -> tuple[int, int, int]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        position = read_saved_position()
        if any(position):
            return position
        time.sleep(0.05)
    fail("field 1 never saved a non-zero player position")
    raise AssertionError("fail() did not exit")


def wait_for_context_change(source_context: int, timeout_s: float) -> int:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        context = field_context_ptr()
        if context != 0 and context != source_context:
            return context
        time.sleep(0.05)
    fail(f"field context did not change from 0x{source_context:08X}")
    raise AssertionError("fail() did not exit")


def main() -> int:
    # Given a gameplay field that continuously saves the player's position.
    source_context = field_context_ptr()
    if teleport(1, 0) != 0:
        fail("teleport to field 1 was refused")
    if teleport(0, 0) != 2:
        fail("a second teleport was accepted while the first was loading")
    source_context = wait_for_context_change(source_context, FIELD_TIMEOUT_S)
    if teleport(0, 0) != 2:
        fail("a second teleport was accepted before the new field settled")
    time.sleep(EARLY_RETRY_S)
    if teleport(0, 0) != 2:
        fail("a second teleport was accepted during field initialization")
    time.sleep(FIELD_SETTLE_S - EARLY_RETRY_S)
    source_position = wait_for_nonzero_saved_position(FIELD_TIMEOUT_S)

    # When the overlay teleports to a field whose scripts expect a fresh position.
    if teleport(0, 0) != 0:
        fail("teleport to field 0 was refused")
    destination_context = wait_for_context_change(source_context, FIELD_TIMEOUT_S)
    time.sleep(FIELD_SETTLE_S)

    # Then both field contexts load without allowing an overlapping teleport.
    print(
        f"PASS: source position {source_position}; "
        f"destination context 0x{destination_context:08X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
