#!/usr/bin/env python3
"""Native Vulkan must expose supported runtime controls without unsafe debug modes."""

from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
menu = (root / "runtime/src/runtime_menu.cpp").read_text(encoding="utf-8")
canonical = (root / "runtime/src/gpu_native_canonical.c").read_text(encoding="utf-8")
executor = (root / "runtime/src/native_vk_executor.c").read_text(encoding="utf-8")
vertex = (root / "runtime/shaders/native_gpu.vert").read_text(encoding="utf-8")
fragment = (root / "runtime/shaders/native_gpu.frag").read_text(encoding="utf-8")

# Native Vulkan deliberately leaves the legacy facade on software. Consult the
# active host, not gr_backend() or the renderer preference for the next launch.
assert 'psx_video_hardware_rendering_active()' in menu
for label in ("Frame rate", "Rendering", "Advanced"):
    assert f'ImGui::BeginMenu("{label}", hardware_rendering)' in menu
assert 'gr_backend() == 1' not in menu
active = main.split('extern "C" int psx_video_hardware_rendering_active(void) {', 1)[1].split("\n}", 1)[0]
assert 'g_gl_active || g_vk_active || g_native_vk' in active
assert 'g_video_renderer' not in active
assert 'g_video_next_renderer' not in active

# Depth view and wireframe work on both APIs. Only debug mipmap generation is
# still refused; never disable the supported entries or reset their preferences.
advanced = menu.split('ImGui::BeginMenu("Advanced", hardware_rendering)', 1)[1].split('ImGui::EndMenu();', 1)[0]
assert 'ImGui::BeginDisabled(native_vulkan);' in advanced
disabled = advanced.split('ImGui::BeginDisabled(native_vulkan);', 1)[1].split('ImGui::EndDisabled();', 1)[0]
assert '"Generate texture mipmaps (experimental)"' in disabled
for label in ('"Depth view"', '"Wireframe"'):
    assert label in advanced
    assert label not in disabled
assert '"Z-buffer"' not in disabled
assert '"Stretch widescreen backdrops"' not in disabled
restore = main.split('extern "C" int psx_runtime_menu_restore_settings(void) {', 1)[1].split('if (s.has_backdrop_stretch)', 1)[0]
assert 'if (g_native_vk)' in restore
assert 'if (s.has_depth_view) gl_renderer_set_native_depth_view(s.depth_view);' in restore
assert 'if (s.has_wireframe) gl_renderer_set_native_wireframe(s.wireframe);' in restore
assert 'gl_renderer_set_native_depth_view(0);' not in restore
assert 'gl_renderer_set_native_wireframe(0);' not in restore
assert 'gl_renderer_set_debug_mipmaps(0);' in restore

# Both debug views must leave canonical guest VRAM untouched. The wireframe
# shader uses ordinary triangle rasterization on core and interlock devices:
# no optional non-solid polygon modes, geometry shaders or barycentric extension.
assert 'view_plane ? work->depth_view : GL_NATIVE_DEPTH_VIEW_OFF' in executor
assert 'work->wireframe && command->plane != 0u' in executor
assert 'return NATIVE_VK_REFUSE_DEPTH_VIEW' not in executor
assert 'return NATIVE_VK_REFUSE_WIREFRAME' not in executor
assert '!wire && (!draw->hd_texture.valid || (S.core_batch && S.hd_batch && !R.capture_active)) && !command->source;' in executor
assert "S.hd_batch = hd_batch && hd_batch[0] == '1';" in executor
assert 'nvk_set4(R.d.flags, 0, 0, 0, lines ? 1 : 3);' in executor
assert 'const int black = wire &&' in executor
assert 'command->source == 0u' in executor
assert 'S.interlock || R.work->wireframe' in executor
assert 'noperspective out vec3 wire_barycentric' in vertex
assert 'VERTEX_ID%3' in vertex
assert 'noperspective in vec3 wire_barycentric' in fragment
assert 'if(flags.w==3)' in fragment
assert 'dFdx(wire_barycentric)' in fragment
assert 'dFdy(wire_barycentric)' in fragment
assert fragment.index('if(flags.w==3)') < fragment.index('beginInvocationInterlockARB();')
assert 'fillModeNonSolid' not in (root / 'runtime/src/vk_core.c').read_text(encoding='utf-8')

# A newly available texture-filter control must affect the Native GPU, not
# just a legacy GL variable that Vulkan never updates.
assert 'draw->sprite_texture ? R.sprite_filter : R.texture_filter' in executor
assert 'R.texture_filter = gr_texture_filter(); R.sprite_filter = s_sprite_filter;' in executor
enter = canonical.split('int gpu_native_canonical_enter(void) {', 1)[1].split('\n}', 1)[0]
assert 's_free_filter = gr_texture_filter();' in enter
assert enter.index('s_free_filter = gr_texture_filter();') < enter.index('gr_set_backend(GR_BACKEND_NATIVE);')

print("Native Vulkan runtime menu controls guard passed")
