#ifndef PSX_VK_SWAPCHAIN_H
#define PSX_VK_SWAPCHAIN_H

/* vk_swapchain.c — the window swapchain and present pacing on vk_core.
 *
 * Shared by every Vulkan presenter (Vulkan backend, Native Vulkan presenter,
 * GL->Vulkan Native presenter). Policy as proven in native_vk_present.c:
 * at least three images; FIFO for N >= 1 vblanks per frame, FIFO_RELAXED for
 * adaptive, and with no vblank wait MAILBOX when the user wants tear-free
 * (IMMEDIATE otherwise), falling back to FIFO. With VK_KHR_present_wait the
 * number of queued, not yet displayed images is bounded (PSX_VK_PRESENT_DEPTH
 * 0..3, default 3, 0 = unbounded).
 *
 * Frame protocol: vk_swapchain_acquire returns a frame whose command buffer
 * is begun and whose image is in VK_IMAGE_LAYOUT_UNDEFINED (contents are
 * discarded; the recorder writes every pixel, letterbox included). The
 * recorder either draws inside vk_swapchain_render_pass() on the frame's
 * framebuffer (clear to black, final layout PRESENT_SRC) or moves the image
 * with vk_swapchain_frame_layout() and writes it by transfer.
 * vk_swapchain_present ends the command buffer, moves the image to
 * PRESENT_SRC if needed, submits (waiting on the acquire plus any extra
 * semaphores) and queues the present.
 *
 * Threading: create/destroy on the main thread while nothing presents.
 * acquire/present on one thread at a time (the presenting thread). Policy,
 * capture and diagnostics from any thread. */

#include <stdint.h>

#include "vk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkSwapFrame {
    VkCommandBuffer cmd;
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;   /* for vk_swapchain_render_pass() */
    VkFormat format;
    uint32_t width, height;
    VkImageLayout layout;        /* tracked by vk_swapchain_frame_layout */
    uint32_t image_index;
    uint32_t slot;               /* frame-in-flight slot */
    int submitted;               /* set by vk_swapchain_present once its submit succeeded, so the
                                  * extra signal semaphores are in flight whatever the present
                                  * returns */
} VkSwapFrame;

typedef enum VkSwapAcquire {
    VK_SWAP_ACQUIRE_FAILED = 0,
    VK_SWAP_ACQUIRE_OK = 1,
    VK_SWAP_ACQUIRE_SKIP = 2     /* minimized / zero-sized: nothing to show, not an error */
} VkSwapAcquire;

/* Needs vk_core_attach_window. Presents one black frame so the window maps.
 * Returns 1 on success, 0 with everything released. */
int  vk_swapchain_create(void);
void vk_swapchain_destroy(void);
int  vk_swapchain_active(void);

/* Same semantics as nvp_set_present_policy. Applied at the next acquire. */
void vk_swapchain_set_present_policy(int swap_interval, int user_vsync);

/* Recreates the swapchain when the window size, the policy or the surface
 * changed, then acquires. Blocks the way a swap does while all frames are in
 * flight. */
VkSwapAcquire vk_swapchain_acquire(VkSwapFrame *frame);
void vk_swapchain_frame_layout(VkSwapFrame *frame, VkImageLayout layout);
VkRenderPass vk_swapchain_render_pass(void);   /* compatible with every frame's framebuffer */
/* Begin/end the swapchain render pass on the frame's framebuffer (full
 * extent, cleared to black). Only valid while frame->layout is UNDEFINED;
 * end leaves the image in PRESENT_SRC_KHR and records that in frame->layout. */
void vk_swapchain_begin_render_pass(VkSwapFrame *frame);
void vk_swapchain_end_render_pass(VkSwapFrame *frame);

/* Returns 1 when queued for display, 0 on failure (frame->submitted tells
 * whether the submit still went through, e.g. the swapchain was out of date).
 * The submit waits on the acquire plus extra_waits and signals the present
 * semaphore plus extra_signals (binary semaphores; e.g. handing an interop
 * image back to GL), at most 4 of each. Arrays may be NULL when their count
 * is 0. */
int vk_swapchain_present(VkSwapFrame *frame,
                         uint32_t extra_wait_count, const VkSemaphore *extra_waits,
                         const VkPipelineStageFlags *extra_wait_stages,
                         uint32_t extra_signal_count, const VkSemaphore *extra_signals);

/* Any thread: read back the next presented image (exactly what the
 * compositor receives). Same contract as nvp_capture_presented. */
int vk_swapchain_capture_presented(int timeout_ms, unsigned char **out_rgb,
                                   int *out_w, int *out_h);

typedef struct VkSwapDiagnostics {
    int active;
    int present_mode;            /* VkPresentModeKHR in use, -1 when none */
    int requested_interval;
    int requested_vsync;
    uint32_t images;
    uint32_t extent_width, extent_height;
    uint64_t frames_acquired;
    uint64_t frames_presented;
    uint64_t acquire_timeouts;
    uint64_t out_of_date;
    uint64_t suboptimal;
    uint64_t recreations;
    uint64_t minimized_skips;
    uint64_t failures;
    uint64_t last_present_ns;
    uint64_t max_present_block_ns;
    int last_vk_result;
    int present_wait;
    int present_depth;
    uint64_t present_waits;
    uint64_t present_wait_timeouts;
    uint64_t present_wait_ns_total;
    uint64_t present_wait_max_ns;
    uint64_t last_present_id;
} VkSwapDiagnostics;

void vk_swapchain_diagnostics(VkSwapDiagnostics *out);
int  vk_swapchain_diagnostics_json(char *out, int capacity);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VK_SWAPCHAIN_H */
