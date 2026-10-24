#ifndef PSX_NATIVE_VK_EXECUTOR_H
#define PSX_NATIVE_VK_EXECUTOR_H

/* The Vulkan implementation of the Native GPU executor, CORE tier (destination
 * colour and depth key are input attachments of the subpass that writes them,
 * a subpass self-dependency replaces glTextureBarrier). native_vk_executor.c
 * documents the structure.
 *
 * Features the Vulkan executor does not implement yet are never rendered
 * differently: a work that needs one is refused and counted per feature
 * (NativeVkRefusal).
 *
 * Shadow mode (PSX_NATIVE_VK_SHADOW=1, GL Native only, PSX_NATIVE_GPU_INTERLOCK=0
 * for the GL core tier): after the GL executor has finished a work and its image
 * digests are known, the Vulkan executor runs the same work synchronously on the
 * GL GPU thread and compares every scanout image with GL's. Debug commands
 * native_vk_shadow / native_vk_shadow_dump report the result. The functions
 * below carry no Vulkan types so GL and the debug server can use them in any
 * build; without Vulkan the shadow is never enabled. */

#include <stddef.h>
#include <stdint.h>

struct GlNativeGpuWork;

typedef enum NativeVkRefusal {
    NATIVE_VK_REFUSE_GL_TIER = 0,   /* GL ran the interlock tier: not comparable */
    NATIVE_VK_REFUSE_CANONICAL_GPU, /* PSX_NATIVE_GPU_CANONICAL provenance attachment */
    NATIVE_VK_REFUSE_DEPTH_VIEW,    /* reserved: depth view is now supported */
    NATIVE_VK_REFUSE_WIREFRAME,     /* reserved: wireframe is now supported */
    NATIVE_VK_REFUSE_MIPMAPS,
    NATIVE_VK_REFUSE_GPU_SETUP,     /* compute triangle setup (fp64/int64) */
    NATIVE_VK_REFUSE_TEXEL_LIMIT,   /* phase texture snapshots exceed the texel buffer limit */
    NATIVE_VK_REFUSE_COMMAND,       /* a journal command kind the executor does not know */
    NATIVE_VK_REFUSE_COUNT
} NativeVkRefusal;

/* ---- the executor as the Native backend (Native on Vulkan, no OpenGL context) ----
 *
 * native_vk_backend_start installs the NativeGpuBackend table (g_native_backend), creates
 * the executor objects and starts the GPU thread with the same state machine, streaming
 * and notify semantics as the OpenGL GPU thread (native_gpu_service). Main thread, with
 * vk_core initialised and the neutral Native state begun
 * (gl_renderer_native_neutral_begin). On failure it returns 0 with the reason.
 * native_vk_backend_stop joins the thread, destroys the endpoint images and every
 * executor object; call it before gl_renderer_native_neutral_end.
 *
 * Image handles: NativeImageSlot.color / .mask hold a pointer to an executor image
 * (native_vk_endpoint_views resolves the views); the images stay in VK_IMAGE_LAYOUT_GENERAL
 * (not SHADER_READ_ONLY_OPTIMAL: the scan images are rendered again when recycled and
 * stay valid for every access, so no layout transition is ever missing). Visibility to
 * later submissions is the barrier each work and each presenter frame record first. */
int native_vk_backend_start(char *reason, size_t reason_size);
void native_vk_backend_stop(void);
void native_vk_backend_join(void);   /* join the GPU thread only */
int native_vk_backend_json(char *out, int capacity);

/* 1 when PSX_NATIVE_VK_SHADOW=1 is set and this build has Vulkan. */
int native_vk_shadow_enabled(void);

/* Main thread, before the GL GPU thread starts: brings up vk_core when the
 * GL->Vulkan presenter has not. Returns 1 when the shadow can run; on 0 it
 * stays disabled for the process and native_vk_shadow reports why. */
int native_vk_shadow_init(void);

/* GL GPU thread, per scanout image i whose readback GL has mapped. The bytes
 * are copied. */
void native_vk_shadow_gl_image(unsigned index, const void *pixels, uint32_t width, uint32_t height);

/* GL GPU thread, before the work becomes READY: executes the work on Vulkan
 * and compares. Never modifies the work. */
void native_vk_shadow_run(const struct GlNativeGpuWork *work, int gl_interlock);

/* Native compile thread (state mutex held) from the GL apply_planes: the work
 * became the committed one. */
void native_vk_shadow_apply(const struct GlNativeGpuWork *work);

/* GL GPU thread, before it exits: releases every Vulkan object of the executor. */
void native_vk_shadow_shutdown(void);

/* Debug server. snprintf semantics; the dump returns 1 on success, else 0
 * with the reason in err. */
int native_vk_shadow_json(char *out, int capacity);
int native_vk_shadow_dump(const char *path, char *err, size_t err_size);

#if defined(PSX_HAVE_VULKAN)
#include "vk_core.h"
/* The colour view and the class-mask view (R8; VK_NULL_HANDLE for a CPU-uploaded image) of an
 * endpoint image handle. */
int native_vk_endpoint_views(uint64_t color_handle, VkImageView *color, VkImageView *mask);
/* Half-open pixel bounds relative to the preceding phase in the same work.
 * Zero means no proof: the presenter must refresh the whole image. */
int native_vk_endpoint_damage(uint64_t color_handle, int32_t bounds[4]);
enum { NATIVE_VK_DAMAGE_REGIONS = 16 };
/* Optional fixed source grid. Returns its side length (2 or 4), or zero if
 * the proof is unavailable. Empty cells and unused entries are zeroed. */
int native_vk_endpoint_damage_regions(uint64_t color_handle, int32_t regions[NATIVE_VK_DAMAGE_REGIONS][4]);
#endif

#endif /* PSX_NATIVE_VK_EXECUTOR_H */
