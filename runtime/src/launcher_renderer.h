#pragma once

/* Vulkan-enabled launchers expose only the hardware APIs, not Software (which
 * cannot run Native). Keep their 0/1 UI indices separate from the persistent
 * renderer IDs: 0 = software, 1 = OpenGL, 2 = Vulkan. Other titles retain the
 * legacy Software/OpenGL vocabulary and its identity mapping. */
static inline int psx_launcher_renderer_index(int renderer, bool vulkan_offered) {
    return vulkan_offered ? (renderer == 2 ? 1 : 0) : renderer;
}

static inline int psx_launcher_renderer_backend(int index, bool vulkan_offered) {
    return vulkan_offered ? (index == 1 ? 2 : 1) : index;
}
