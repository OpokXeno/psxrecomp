#!/usr/bin/env python3
from pathlib import Path

source = (Path(__file__).parents[1] / "launcher" / "launcher.cpp").read_text()
assert 'v == 2 ? "Vulkan"' in source
assert "m.renderer = (m.renderer + 1) % 3" in source
assert "m.renderer == 1" in source  # OpenGL-only controls remain OpenGL-only.
print("Launcher Vulkan-option test passed")
