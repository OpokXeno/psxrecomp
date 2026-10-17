#ifndef PSX_NATIVE_VK_PRESENT_H
#define PSX_NATIVE_VK_PRESENT_H

/* Vulkan presentation for the Native OpenGL presenter.
 *
 * The Native presenter composes into one of three images that Vulkan
 * allocates and OpenGL imports (GL_EXT_memory_object + GL_EXT_semaphore).
 * Vulkan then blits that image, flipped to top-down, into a swapchain of at
 * least three images and presents it. A FIFO swapchain with three images
 * cannot halve the output the way a double-buffered GL swap does when one
 * frame misses its vblank, and MAILBOX gives tear-free presents at targets
 * the panel cannot divide.
 *
 * Only used when Native is selected. The visible window is then created with
 * SDL_WINDOW_VULKAN and every GL context lives on hidden drawables. When the
 * probe or the window attach fails, the caller keeps the plain GL window
 * path.
 *
 * Threading: nvp_probe, nvp_attach_window and nvp_shutdown run on the main
 * thread while no thread presents. nvp_begin_frame, nvp_present and
 * nvp_release_gl_context run on whichever thread currently has the Native
 * presenter GL context current (never two at once). nvp_set_present_policy
 * and the diagnostics accessors may be called from any thread. */

#include <stdint.h>

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Before the visible window exists, with a GL context of the same driver
 * current. Checks the GL interop extensions, loads Vulkan, and selects the
 * physical device whose device and driver UUIDs match that GL context. Keeps
 * the instance and device for nvp_attach_window. Returns 1 when the
 * interop path is usable, else 0 with everything released. PSX_VK_PRESENT=0
 * in the environment forces 0. */
int nvp_probe(void);

/* Main thread, after nvp_probe returned 1, with the visible window created
 * with SDL_WINDOW_VULKAN. Creates the surface and swapchain and presents one
 * black frame so the window maps. Returns 1 on success; on 0 everything is
 * released (nvp_active() becomes 0) and the caller must recreate the window
 * for OpenGL. */
int nvp_attach_window(struct SDL_Window *window);

/* 1 between a successful nvp_attach_window and nvp_shutdown. */
int nvp_active(void);

/* swap_interval: present_effective_swap_interval() semantics
 * (0 = no vblank wait, N >= 1 = each composed frame shown for N vblanks,
 * -1 = adaptive). user_vsync: the user's VSync choice (0 off, 1 on,
 * -1 adaptive). Applied at the next nvp_begin_frame. */
void nvp_set_present_policy(int swap_interval, int user_vsync);

/* Presenter thread, Native presenter GL context current. Makes the interop
 * images width x height (recreating them on change), waits on the GPU until
 * the next image is free, and returns a GL framebuffer object of the current
 * context to compose into. Calling it again before nvp_present returns the
 * same framebuffer. Returns 0 on failure. */
unsigned int nvp_begin_frame(int width, int height);

/* Presenter thread, same context. Hands the image composed since
 * nvp_begin_frame to Vulkan and presents it. Blocks the way a swap does when
 * the swapchain is full. Returns 1 when the frame was queued for display (or
 * the window is minimized and nothing can be shown), 0 on failure. Without a
 * begun frame it returns 0 and does nothing. */
int nvp_present(void);

/* Any thread (the debug server). Requests that the next presented frame's
 * swapchain image be read back after the blit, that is exactly what the
 * compositor receives. Blocks up to timeout_ms. On success returns 1 and sets
 * *out_rgb to a malloc'd, tightly packed, top-down RGB8 buffer of *out_w x
 * *out_h pixels (the caller frees it). Returns 0 when inactive, unsupported,
 * busy, failed or timed out. */
int nvp_capture_presented(int timeout_ms, unsigned char **out_rgb, int *out_w, int *out_h);

/* Presenter thread, the context that called nvp_begin_frame current. Deletes
 * the GL objects and the interop images; the next nvp_begin_frame recreates
 * them in whatever context is current then. Call before that context is
 * destroyed. */
void nvp_release_gl_context(void);

/* Main thread, after presenting stopped and nvp_release_gl_context ran, and
 * before the window is destroyed. Safe to call when inactive. */
void nvp_shutdown(void);

typedef struct NvpDiagnostics {
    int active;
    int present_mode;          /* VkPresentModeKHR in use, -1 when none */
    int requested_interval;
    int requested_vsync;
    uint32_t swapchain_images;
    uint32_t extent_width;
    uint32_t extent_height;
    uint32_t interop_width;
    uint32_t interop_height;
    uint64_t frames_begun;
    uint64_t frames_presented;  /* nvp_present calls that queued >= 1 image */
    uint64_t images_presented;  /* vkQueuePresentKHR successes (counts repeats) */
    uint64_t acquire_timeouts;
    uint64_t out_of_date;
    uint64_t suboptimal;
    uint64_t swapchain_recreations;
    uint64_t interop_recreations;
    uint64_t minimized_skips;
    uint64_t failures;
    uint64_t last_present_ns;   /* SDL_GetTicksNS() after the last present */
    uint64_t max_present_block_ns; /* longest nvp_present call */
    int last_vk_result;
    int present_wait;           /* 1 when VK_KHR_present_wait bounds the queue */
    int present_depth;          /* max not-yet-displayed images (0 = unbounded) */
    uint64_t present_waits;     /* vkWaitForPresentKHR calls */
    uint64_t present_wait_timeouts;
    uint64_t present_wait_ns_total; /* time blocked in vkWaitForPresentKHR */
    uint64_t present_wait_max_ns;
    uint64_t last_present_id;   /* id of the last image queued with present_id */
} NvpDiagnostics;

void nvp_diagnostics(NvpDiagnostics *out);

/* JSON object with the fields above, for the debug server. Returns the
 * number of characters written (snprintf semantics). */
int nvp_diagnostics_json(char *out, int capacity);

#ifdef __cplusplus
}
#endif

#endif
