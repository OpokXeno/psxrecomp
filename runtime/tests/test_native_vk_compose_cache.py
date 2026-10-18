#!/usr/bin/env python3
"""Held Native images may reuse spatial AA without caching UI or TAA history."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / 'runtime/src/native_vk_presenter.c').read_text(encoding='utf-8')
compose = source.split('static bool nvkp_compose(', 1)[1].split('\n/* ---- swap', 1)[0]
key = source.split('typedef struct NvkpComposeKey {', 1)[1].split('} NvkpComposeKey;', 1)[0]
for field in ('slot', 'generation', 'image_index', 'width', 'height', 'color', 'mask',
              'rect', 'aa_mode', 'aa_factor', 'aa_nearest', 'mask_valid', 'lut_on', 'lut_generation'):
    assert field in key, field
assert "P.compose_cache && aa_mode != 3 && P.composed >= 0 && P.cache_valid" in compose
assert '!memcmp(&key, &P.cache_key, sizeof(key))' in compose
assert compose.index('nvkp_lut_update();') < compose.index('goto composed;')
assert compose.index('goto composed;') < compose.index('nvkp_slot_begin(slot)')
assert compose.index('composed: ;') < compose.index('native_register_fence(')
assert compose.index('composed: ;') < compose.index('native_present_commit(&sel, 0u);')
assert 'P.cache_valid = 0;' in source.split('static void nvkp_scene_destroy(void)', 1)[1].split('\n}', 1)[0]
swap = source.split('static void nvkp_swap(', 1)[1].split('/* ---- UI', 1)[0]
assert 'P.ui_front' in swap and 'P.pipe_blend' in swap and 'vk_swapchain_present(' in swap
assert '"PSX_NATIVE_VK_COMPOSE_CACHE"' in source
assert '++P.image_updates;' in compose
assert 'P.phase_composes += sel.image_index != 0u;' in compose
print('Native Vulkan held-compose cache guard passed')
