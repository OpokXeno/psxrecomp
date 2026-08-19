#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text()
launcher_ui = (root.parent / "recomp-ui" / "src" / "common" /
               "backends" / "imgui" / "launcher_imgui.cpp").read_text()
config_h = (root / "recompiler" / "src" / "config_loader.h").read_text()
config_cpp = (root / "recompiler" / "src" / "config_loader.cpp").read_text()
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text()

assert "runtime/" + "launcher" not in runtime_cmake
assert "Rml" + "Ui" not in runtime_cmake
assert '#include "launcher.h"' not in main

assert "bool                  video_offer_vulkan = false;" in config_h
assert "bool vulkan_offered = false;" in config_h
assert 'video.contains("offer_vulkan")' in config_cpp
assert 'rt.video_offer_vulkan = toml::find<bool>(video, "offer_vulkan");' in config_cpp
assert "bool vulkan_offered = false;" in config_cpp
assert "/*vulkan_offered*/" in config_cpp

assert '"Software"' in main
assert '"OpenGL (Recommended)"' in main
assert '"Vulkan"' in main
assert "gi.renderer_labels      = kPsxRendererLabels;" in main
assert "gi.num_renderers        = vulkan_offered ? 3 : 2;" in main
assert "ls.renderer < 0 || ls.renderer > (vulkan_offered ? 2 : 1)" in main
assert "settings requested Vulkan, but this game does not" in main

assert "int                   video_fps = 30;" in config_h
assert 'video.contains("fps")' in config_cpp
assert '"[video] fps must be 30 or 60"' in config_cpp
assert "set_video_fps(gc.runtime.video_fps);" in main
assert "if (us.has_fps) set_video_fps(us.fps);" in main
assert "seed.fps = g_video_fps" in main
assert "set_video_fps(seed.fps);" in main
assert "ls.fps               = seed.fps;" in main
assert "seed.fps                   = ls.fps;" in main
assert "us.fps = ls.fps;" in main
assert "set_video_fps(ls.fps);" in main
assert 'row_label("FPS", th);' in launcher_ui
assert "launcher_model_cycle_fps(m)" in launcher_ui
assert 'row_label("Aspect ratio", th' in launcher_ui
assert "launcher_model_cycle_aspect(m)" in launcher_ui
assert 'row_label("Renderer", th);' not in launcher_ui
assert 'row_label("Frame interpolation", th);' not in launcher_ui
assert "ls.frame_interp       = seed.frame_interpolation ? 1 : 0;" in main
assert "ls.frame_interp_fps   = seed.frame_interpolation_fps;" in main
assert "seed.frame_interpolation   = ls.frame_interp != 0;" in main
assert "seed.frame_interpolation_fps = ls.frame_interp_fps;" in main
assert "seed.frame_interpolation = g_frame_interpolation != 0;" in main
assert "g_frame_interpolation =" in main
assert "frame_interpolation_offered && seed.frame_interpolation ? 1 : 0;" in main
assert "bool has_frame_interpolation = false;" in config_h
assert 'v.contains("frame_interpolation")' in config_cpp

print("Launcher Vulkan-option test passed")
