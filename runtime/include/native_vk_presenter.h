#ifndef PSX_NATIVE_VK_PRESENTER_H
#define PSX_NATIVE_VK_PRESENTER_H

/* Native presentation on Vulkan with no OpenGL context in the process.
 *
 * The three Native roles run on the one vk_core device: the canonical guest VRAM
 * facade (gpu_native_canonical.c, no GPU object), the executor
 * (native_vk_executor.c: NativeGpuBackend, GPU thread) and this presenter:
 * compose renders the selected endpoint image into an offscreen scene image with the
 * shared present.vert / present.frag (the same endpoint and phase selection,
 * interpolation, letterbox, AA / TAA, screen LUT as the GL presenter: the neutral half is
 * native_presenter_select.c), swap acquires a vk_swapchain frame, copies the scene and
 * composites the UI layer (ui_layer.frag) over it, then presents.
 *
 * Main thread: init / shutdown / stop_worker and the UI layer calls. Presenter thread (the
 * dedicated one or the main thread): compose and swap. */

#include <stddef.h>
#include <stdint.h>

#include "xg_render_semantic_presentation.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* vk_core initialised, the window attached and vk_swapchain created. Creates the presenter objects, begins
 * the neutral Native state and starts the executor. Returns 1 and fills the two service tables, or 0
 * with the reason (everything released, nothing running). */
int native_vk_services_init(XgRenderWorkerServices *worker, XgRenderPresenterServices *presenter,
                            struct SDL_Window *window, char *reason, size_t reason_size);
/* Main thread. Joins the executor thread (the presentation host is already destroyed). */
void native_vk_services_stop_worker(void);
/* Main thread. Releases everything native_vk_services_init created. */
void native_vk_services_shutdown(void);
int native_vk_services_active(void);

/* JSON for the debug server (executor, presenter and swapchain counters). */
int native_vk_services_json(char *out, int capacity);
/* Any thread: optionally request trace output on the presenter, then report
 * whether the trace is still active. No waits on the calling thread. */
int native_vk_services_trace_control(int finish);

#if defined(PSX_HAVE_VULKAN)
#include "vk_core.h"
/* UI layer (main thread). begin returns the command buffer to record ImGui draw data into, inside a render
 * pass compatible with native_vk_ui_render_pass() on a transparent RGBA8 target of width x height, or NULL
 * when the layer cannot be rendered this frame (the presenter still reads it; try later). end publishes the
 * layer; only the clip rectangle (window pixels, top-left origin) is composited. */
VkRenderPass native_vk_ui_render_pass(void);
VkCommandBuffer native_vk_ui_begin(int width, int height);
/* Keep window-coordinate projection while allocating/clearing only the rows
 * occupied by UI geometry. Unknown custom draws use content_height=height. */
VkCommandBuffer native_vk_ui_begin_clipped(int width, int height, int content_height);
int native_vk_ui_end_rect(int x, int y, int width, int height);
int native_vk_ui_end(void);
void native_vk_ui_hide(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSX_NATIVE_VK_PRESENTER_H */
