/* Dear ImGui Vulkan backend shared by every Native-on-Vulkan ImGui client (the runtime menu and the
 * debug overlay). Each client owns its ImGui context and platform (SDL) backend; this only wires the
 * renderer backend into the presenter's UI render pass. Caller must have its ImGui context current. */
#ifndef PSX_IMGUI_VULKAN_H
#define PSX_IMGUI_VULKAN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes ImGui_ImplVulkan for the presenter's UI layer and builds the font texture. */
bool psx_imgui_vulkan_backend_init(void);
/* Waits for the device to go idle, then shuts the ImGui_ImplVulkan backend down. */
void psx_imgui_vulkan_backend_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
