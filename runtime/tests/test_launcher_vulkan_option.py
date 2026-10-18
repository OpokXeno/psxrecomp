#!/usr/bin/env python3
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
launcher_ui = (root.parent / "recomp-ui" / "src" / "common" /
               "backends" / "imgui" / "launcher_imgui.cpp").read_text(encoding="utf-8")
launcher_xg_path = (root.parent / "recomp-ui" / "src" / "common" /
                    "backends" / "imgui" / "launcher_xg.inc")
main = (root / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
config_h = (root / "recompiler" / "src" / "config_loader.h").read_text(encoding="utf-8")
config_cpp = (root / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text(encoding="utf-8")
menu = (root / "runtime" / "src" / "runtime_menu.cpp").read_text(encoding="utf-8")
actions = (root / "runtime" / "src" / "runtime_menu_actions.h").read_text(encoding="utf-8")

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
assert '"Vulkan (Recommended)"' in main
assert "gi->has_renderer = vulkan_offered_b ? 1 : 0;" in main
assert "gi->renderer_labels = vulkan_offered_b ? kPsxGraphicsApiLabels : kPsxRendererLabels;" in main
assert "gi->num_renderers = 2;" in main
# The optional Xenogears-themed launcher uses the same host capability/model.
if launcher_xg_path.is_file():
    launcher_xg = launcher_xg_path.read_text(encoding="utf-8")
    assert 'if (m->has_renderer)' in launcher_xg
    assert 'r.label = ui_text("Renderer"); r.value = launcher_model_renderer_label(m);' in launcher_xg
    assert 'launcher_model_set_renderer(m, (m->s.renderer + dir + n) % n)' in launcher_xg
assert "ls.renderer < 0 || ls.renderer > 1" in main
assert "psx_launcher_renderer_index(seed.renderer, vulkan_offered)" in main
assert "psx_launcher_renderer_index(psx_video_get_renderer(), vulkan_offered)" in main
assert main.count("psx_launcher_renderer_backend(ls.renderer, vulkan_offered)") == 3
assert "settings requested Vulkan, but this game does not" in main
assert re.search(r"#if defined\(PSX_HAVE_VULKAN\)\s+"
                 r"vulkan_offered = gc.vulkan_offered;\s+#endif", main)

# The runtime bar persists the preference without mutating the live renderer.
assert 'ImGui::BeginMenu("Renderer")' in menu
assert 'const char* names[] = {"OpenGL", "Vulkan (Recommended)"};' in menu
assert "renderer == 1 || psx_video_vulkan_offered()" in menu
assert "remember(MENU_RENDERER);" in menu
assert "Restart the game to apply the renderer change" in menu
assert "MENU_RENDERER = 1u << 24" in actions
setter = main.split('extern "C" int psx_video_set_renderer(int renderer) {', 1)[1].split("\n}", 1)[0]
assert "!g_video_vulkan_offered" in setter
assert "g_video_next_renderer = renderer;" in setter
assert "g_video_renderer =" not in setter
assert "gr_set_backend" not in setter
assert "if (changed & MENU_RENDERER) { s.renderer = psx_video_get_renderer(); s.has_renderer = true; }" in main
assert "g_video_next_renderer = g_video_renderer;" in main

assert "int                   video_fps = 30;" in config_h
assert 'video.contains("fps")' in config_cpp
assert '"[video] fps must be between 30 and 240"' in config_cpp
assert "set_video_fps(gc.runtime.video_fps);" in main
assert "if (us.has_fps) set_video_fps(us.fps);" in main
assert "seed.fps = g_video_fps" in main
assert "set_video_fps(seed.fps);" in main
assert "ls.fps               = seed.fps;" in main
assert "seed.fps                   = ls.fps;" in main
assert "us.fps = ls.fps;" in main
assert "set_video_fps(ls.fps);" in main
assert 'row_label_right("FPS", th,' in launcher_ui
assert "launcher_model_set_fps(m, i)" in launcher_ui
assert '"Aspect ratio"' in launcher_ui
assert "launcher_model_cycle_aspect(m)" in launcher_ui
assert 'row_label_right("Renderer", th,' in launcher_ui
assert "launcher_model_set_renderer(m, i)" in launcher_ui
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
