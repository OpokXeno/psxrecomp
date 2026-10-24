#ifndef PSX_VK_CORE_H
#define PSX_VK_CORE_H

/* vk_core.c — the one Vulkan instance and device of the process.
 *
 * Every Vulkan consumer (the Vulkan renderer backend, the Native Vulkan
 * executor and presenter, the GL->Vulkan Native presenter, ImGui) runs on
 * this device. Vulkan is loaded at runtime through SDL_Vulkan_LoadLibrary and
 * volk, so the executable has no link-time Vulkan dependency and a machine
 * without a Vulkan driver still starts on OpenGL.
 *
 * Baseline: Vulkan 1.1 with no required device features beyond core 1.1 and
 * VK_KHR_swapchain. Everything newer is an optional capability reported in
 * VkCoreCaps and enabled when present; consumers pick their path from the
 * caps, never from vendor or device ids. PSX_VK_DISABLE (comma list of cap
 * names, see vk_core.c) masks caps off to exercise the old-GPU paths on a
 * modern GPU.
 *
 * Threading: vk_core_init / vk_core_attach_window / vk_core_shutdown run on
 * the main thread while no other thread uses Vulkan. Queue submission and
 * presentation go through vk_core_submit / vk_core_present, which serialize
 * the single queue. Everything else is callable from any thread. */

#include <stddef.h>
#include <stdint.h>

#include "volk.h"
#include "vk_mem_alloc.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkCoreCaps {
    uint32_t api_version;            /* device apiVersion clamped to the instance's */
    uint32_t vendor_id, device_id, driver_version;
    VkPhysicalDeviceType device_type;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t device_uuid[VK_UUID_SIZE];
    uint8_t driver_uuid[VK_UUID_SIZE];

    /* Optional features, enabled on the device when reported here. */
    uint8_t fragment_shader_interlock;   /* VK_EXT_fragment_shader_interlock, pixel ordered */
    uint8_t fragment_stores_and_atomics;
    uint8_t shader_float64;
    uint8_t shader_int64;
    uint8_t sampler_anisotropy;
    uint8_t dual_src_blend;
    uint8_t independent_blend;
    uint8_t timeline_semaphore;          /* core 1.2 feature or VK_KHR_timeline_semaphore */
    uint8_t present_id;                  /* VK_KHR_present_id */
    uint8_t present_wait;                /* VK_KHR_present_wait (implies present_id) */
    uint8_t pipeline_statistics;         /* pipelineStatisticsQuery */
    uint8_t timestamps;                  /* graphics queue timestampValidBits != 0 */
    uint8_t debug_utils;                 /* VK_EXT_debug_utils (object names, labels) */
    uint8_t validation;                  /* validation layer active */

    float timestamp_period_ns;
    float max_sampler_anisotropy;
    uint32_t max_image_dimension_2d;
    uint32_t max_push_constants_size;
    uint32_t max_texel_buffer_elements;
    uint32_t max_storage_buffer_range;
    uint32_t max_uniform_buffer_range;
    uint32_t max_color_attachments;
    VkDeviceSize min_uniform_buffer_offset_alignment;
    VkDeviceSize min_texel_buffer_offset_alignment;
    VkDeviceSize non_coherent_atom_size;
} VkCoreCaps;

/* Restrictions for device selection. All fields optional (zero = none). */
typedef struct VkCoreDeviceRequest {
    /* When device_uuid_count != 0, only a physical device whose deviceUUID
     * equals one of these and whose driverUUID equals driver_uuid qualifies
     * (GL interop: the device that runs the current GL context). */
    const uint8_t (*device_uuids)[VK_UUID_SIZE];
    uint32_t device_uuid_count;
    const uint8_t *driver_uuid;
    /* Extra device extensions the caller requires; a device lacking one is
     * skipped. */
    const char *const *extensions;
    uint32_t extension_count;
    /* File the pipeline cache is loaded from at init and saved to at
     * shutdown. NULL disables persistence. */
    const char *pipeline_cache_path;
} VkCoreDeviceRequest;

/* Loads Vulkan, creates the instance (with the instance extensions SDL needs
 * for a window surface) and the device. Needs no window. Returns 1 on success;
 * on 0 everything is released and vk_core_failure() names the first failing
 * step. A second call while initialized returns 1 without changes when the
 * request is satisfied by the existing device, else 0. */
int  vk_core_init(const VkCoreDeviceRequest *request);

/* Main thread, after vk_core_init, with a window created with
 * SDL_WINDOW_VULKAN. Creates the surface and checks the queue can present to
 * it. Returns 1 on success, 0 (surface released) otherwise. */
int  vk_core_attach_window(struct SDL_Window *window);
void vk_core_detach_window(void);   /* destroys the surface; swapchain must be gone */

/* Waits for the device to go idle, saves the pipeline cache, destroys
 * everything. Idempotent. */
void vk_core_shutdown(void);

int  vk_core_ready(void);
const char *vk_core_failure(void);
const VkCoreCaps *vk_core_caps(void);

VkInstance       vk_core_instance(void);
VkPhysicalDevice vk_core_physical_device(void);
VkDevice         vk_core_device(void);
uint32_t         vk_core_queue_family(void);
VkSurfaceKHR     vk_core_surface(void);
struct SDL_Window *vk_core_window(void);
VmaAllocator     vk_core_allocator(void);
VkPipelineCache  vk_core_pipeline_cache(void);

/* The queue is externally synchronized; these take the queue lock. */
VkResult vk_core_submit(uint32_t count, const VkSubmitInfo *submits, VkFence fence);
VkResult vk_core_present(const VkPresentInfoKHR *info);
/* For code that submits to the queue itself (Dear ImGui's Vulkan backend). Recursive. */
void vk_core_queue_lock(void);
void vk_core_queue_unlock(void);
VkResult vk_core_queue_wait_idle(void);
VkResult vk_core_device_wait_idle(void);

/* Device loss is terminal: vk_core_check records the first failing site and,
 * for VK_ERROR_DEVICE_LOST, halts the process with psx_fatal_halt naming the
 * site and device. Returns 1 for VK_SUCCESS (and the non-error statuses
 * VK_SUBOPTIMAL_KHR, VK_TIMEOUT, VK_NOT_READY, VK_INCOMPLETE), else 0. */
int vk_core_check(VkResult result, const char *site);

/* Resources. memory: where the allocation lives. */
typedef enum VkCoreMemory {
    VK_CORE_MEMORY_DEVICE = 0,   /* device local, not mapped */
    VK_CORE_MEMORY_UPLOAD,       /* host visible, persistently mapped, write combined */
    VK_CORE_MEMORY_READBACK      /* host visible + cached, persistently mapped */
} VkCoreMemory;

typedef struct VkCoreImage {
    VkImage image;
    VkImageView view;            /* 2D view of all mip levels, color aspect */
    VmaAllocation allocation;
    VkFormat format;
    uint32_t width, height, mip_levels;
} VkCoreImage;

typedef struct VkCoreBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    void *mapped;                /* non-NULL for UPLOAD/READBACK */
    VkDeviceSize size;
} VkCoreBuffer;

int  vk_core_image_create(VkCoreImage *out, VkFormat format, uint32_t width,
                          uint32_t height, uint32_t mip_levels,
                          VkImageUsageFlags usage, const char *name);
/* Multiple independent 2D layers share one view and descriptor. */
int  vk_core_image_create_array(VkCoreImage *out, VkFormat format, uint32_t width, uint32_t height,
                                uint32_t mip_levels, uint32_t layers, VkImageUsageFlags usage,
                                const char *name);
void vk_core_image_destroy(VkCoreImage *image);
int  vk_core_buffer_create(VkCoreBuffer *out, VkDeviceSize size,
                           VkBufferUsageFlags usage, VkCoreMemory memory,
                           const char *name);
void vk_core_buffer_destroy(VkCoreBuffer *buffer);
/* Flush/invalidate a mapped range (no-op on coherent memory). */
void vk_core_buffer_flush(const VkCoreBuffer *buffer, VkDeviceSize offset, VkDeviceSize size);
void vk_core_buffer_invalidate(const VkCoreBuffer *buffer, VkDeviceSize offset, VkDeviceSize size);

VkShaderModule vk_core_shader_module(const uint32_t *code, size_t bytes, const char *name);

/* Debug object name (RenderDoc, validation messages); no-op without
 * debug_utils. */
void vk_core_name(VkObjectType type, uint64_t handle, const char *name);

/* One-shot command buffer from an internal, lock-protected pool. end_wait
 * submits it and waits for completion. For init and rare transfers only. */
VkCommandBuffer vk_core_oneshot_begin(void);
int vk_core_oneshot_end_wait(VkCommandBuffer cmd);

/* Records an image layout transition of all mips / the color aspect with
 * access and stage masks derived from the two layouts. */
void vk_core_image_barrier(VkCommandBuffer cmd, VkImage image,
                           VkImageLayout from, VkImageLayout to);

/* Diagnostics for the debug server (snprintf semantics). */
int vk_core_info_json(char *out, int capacity);
/* Validation and debug-utils messages are kept in a ring; count is the
 * number of most recent messages to emit (0 = all kept). */
uint64_t vk_core_validation_message_count(void);
int vk_core_validation_json(char *out, int capacity, int count);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VK_CORE_H */
