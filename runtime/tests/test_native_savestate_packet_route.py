#!/usr/bin/env python3
"""Cold-process Native savestate loads must preserve the wide packet route."""

from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
restore = main.split('extern "C" void psx_frontend_on_savestate_loaded(void) {', 1)[1].split('\n}', 1)[0]

# A world/battle snapshot need not execute field authentication after loading.
# Re-enable only the active Native Work owner, after scene/mod restoration;
# do not infer activity from the next-launch renderer preference or require GL.
guard = 'if (g_native_render_selected && !g_native_render_source_failed)\n        guest_render_native_stream_set_enabled(true);'
assert guard in restore
assert restore.index('psx_xenogears_scene_reset();') < restore.index(guard)
assert restore.index('mod_runtime_on_savestate_loaded();') < restore.index(guard)
assert 'g_gl_active' not in restore[:restore.index(guard)]
assert 'g_video_renderer' not in restore
assert 'g_video_next_renderer' not in restore

print('Native savestate packet-route guard passed')
