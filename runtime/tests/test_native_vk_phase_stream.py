#!/usr/bin/env python3
"""Publication/lifetime boundaries for overlapping Native Vulkan phase execution."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
compiler = (root / 'runtime/src/native_work_compiler.c').read_text(encoding='utf-8')
executor = (root / 'runtime/src/native_vk_executor.c').read_text(encoding='utf-8')
publication = compiler.split('static void native_gpu_publish_prefix(', 1)[1].split('static void native_gpu_discard_phases', 1)[0]
assert publication.index('SDL_LockMutex') < publication.index('phase_stream_ready = 1') < publication.index('SDL_UnlockMutex')
assert 'work->sealed=seal > 0;' in publication
assert 'if (gpu && !gpu->canonical_gpu && !s_native_motion_probe.target_vblank && !s_native_recipe_audit_enabled)' in compiler
assert compiler.index('native_gpu_publish_prefix(gpu, -2)') < compiler.index('native_motion_prepare(audit, staged_views,')
execute = executor.split('static int nvk_execute(', 1)[1].split('/* Everything an execution', 1)[0]
assert 'base_commands = work->published_commands; base_data = work->published_data;' in execute
assert execute.index('if (streaming && ok) {') < execute.index('const GlNativePhaseTextures *textures')
assert 'while (c >= work->published_chunk_count && !work->sealed && !work->cancel_requested' in execute
assert 'SDL_CondWaitTimeout(s_native_gpu_condition, s_native_state_mutex, 2)' in execute
assert execute.index('if (c >= available) break;') < execute.index('nvk_scanout_image(image)')
assert 'if (streaming && ok) ok = nvk_submit_partial(VK_NULL_HANDLE);' in execute
assert 'return 2; /* caller drains submissions before releasing storage */' in execute
submit = executor.split('static int nvk_backend_submit(', 1)[1].split('static int nvk_backend_complete(', 1)[0]
cancel = submit.split('if (ok == 2)', 1)[1].split('return 1;', 1)[0]
assert cancel.index('nvk_backend_abandon();') < cancel.index('work->state = 0;')
assert '"PSX_NATIVE_VK_PHASE_STREAM"' in executor
print('Native Vulkan phase-stream publication guard passed')
