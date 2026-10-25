/* Native presentation on Vulkan (see native_vk_presenter.h).
 *
 * Compose (presenter thread) renders the selected endpoint image into the offscreen scene image with
 * the shared present shaders and submits at once, like the GL presenter draws at once; swap acquires a
 * vk_swapchain frame, copies the scene, composites the UI layer and presents. The scene images
 * ping-pong, so the previous composed frame is the TAA history without a copy.
 *
 *   GL presenter                              Vulkan
 *   native_present_select / commit / abort    the same (native_presenter_select.c)
 *   draw into the window framebuffer          draw into scene[cur] (viewport with a negative height keeps
 *                                             GL's bottom-up clip space, so present.vert is shared as is)
 *   glCopyTexSubImage2D history               scene[cur ^ 1]
 *   UI layer (shared textures + fences)       UI layer images, one render pass each, ImGui_ImplVulkan
 *   completion GLsync                         the compose submission is on the queue before compose
 *                                             returns: SOFTWARE_READY (queue order + the barrier every
 *                                             later executor work starts with)
 *   SwapBuffers                               vk_swapchain_acquire / present
 *
 * Not carried over from the GL presenter: the pres_* present-event ring (diagnostic correlation of
 * presents with sampled readbacks) and the debug-overlay window. */

#include "native_vk_presenter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#if !defined(PSX_HAVE_VULKAN)

int native_vk_services_init(XgRenderWorkerServices *worker, XgRenderPresenterServices *presenter,
                            struct SDL_Window *window, char *reason, size_t reason_size)
{
    (void)worker; (void)presenter; (void)window;
    snprintf(reason, reason_size, "built without Vulkan");
    return 0;
}
void native_vk_services_stop_worker(void) {}
void native_vk_services_shutdown(void) {}
int native_vk_services_active(void) { return 0; }
int native_vk_services_trace_control(int finish) { (void)finish; return 0; }
int native_vk_services_json(char *out, int capacity)
{
    return snprintf(out, (size_t)capacity, "{\"active\":0,\"reason\":\"built without Vulkan\"}");
}

#else  /* PSX_HAVE_VULKAN */

#include "native_work_internal.h"
#include "native_vk_executor.h"
#include "vk_core.h"
#include "vk_swapchain.h"
#include "vk_shaders_spv.h"
#include "wayland_presentation.h"

enum { NVKP_SLOTS = 8, NVKP_UBO_BYTES = 4096, NVKP_SLOT_ALIGN = 256, NVKP_UI_LAYERS = 2 };

/* std140 PresentRect / PresentDraw (present_common.glsl, present.frag). */
typedef struct NvkpRect { float uv_rect[4]; } NvkpRect;
typedef struct NvkpDraw {
    float tex_size[2], sharp_scale[2], aa_drawable[2], aa_jitter[2];
    int32_t screenlut_on, sharp;
    float gamma;
    int32_t aa_mode, aa_factor, aa_history_valid, aa_mask_valid, aa_nearest, scanline;
    float scanline_strength, scanline_lines, scanline_scale;
} NvkpDraw;
_Static_assert(sizeof(NvkpRect) == 16, "PresentRect std140 size");
_Static_assert(sizeof(NvkpDraw) == 80, "PresentDraw std140 size");
_Static_assert(NVKP_SLOT_ALIGN + sizeof(NvkpDraw) <= NVKP_UBO_BYTES, "uniform slot");

typedef struct NvkpSlot {
    VkCommandBuffer cmd;
    VkFence fence;
    int submitted;
    VkDescriptorPool pool;
    VkCoreBuffer ubo;
    VkQueryPool timestamps;
    VkCoreBuffer audit;
    VkDeviceSize audit_bytes;
} NvkpSlot;

typedef struct NvkpScene {
    VkCoreImage image;
    VkFramebuffer framebuffer;
    VkDescriptorSet copy_set;
} NvkpScene;

typedef struct NvkpUiLayer {
    VkCoreImage image;
    VkFramebuffer framebuffer;
    VkDescriptorSet set;
    uint32_t width, height;
    int clip[4];
    VkCommandBuffer cmd;
    VkFence done;           /* the layer's render submission */
    int done_pending;
    VkFence consumed[NVKP_SLOTS]; /* every in-flight frame sampling the layer */
    uint8_t consumed_pending[NVKP_SLOTS];
    uint32_t readers;       /* presenter CPU reservations before submission */
} NvkpUiLayer;

/* Endpoint images are immutable within their slot generation. A held image
 * needs another swap/UI composite, not another full-window spatial AA pass. */
typedef struct NvkpComposeKey {
    uint32_t slot, generation, image_index, width, height;
    VkImageView color, mask;
    int32_t rect[4], aa_mode, aa_factor, aa_nearest, mask_valid;
    int32_t lut_on, lut_generation;
} NvkpComposeKey;

static struct {
    int ready, services;
    struct SDL_Window *window;
    VkDescriptorSetLayout dsl_present0, dsl_present1, dsl_tex;
    VkPipelineLayout pl_present, pl_tex, pl_copy;
    VkRenderPass pass_scene, pass_ui;
    VkRenderPass pass_edges;
    VkRenderPass pass_scene_load, pass_edges_load;
    VkPipeline pipe_present, pipe_copy, pipe_blend;
    VkPipeline pipe_edges, pipe_smaa_cached;
    int smaa_prepass, publish_barriers;
    int smaa_damage, smaa_audit, smaa_regions;
    uint64_t partial_composes, partial_pixels, partial_regions, full_pixels, audit_checks, audit_mismatches;
    int limit_postprocess, copy_filter;
    struct { int32_t key[5]; VkPipeline pipeline; } present_pipelines[64];
    uint32_t present_pipeline_count;
    int specialize_present;
    VkSampler sampler_nearest, sampler_linear;
    VkCommandPool pool_presenter, pool_ui;
    VkDescriptorPool static_pool, ui_pool;
    NvkpSlot slots[NVKP_SLOTS];
    uint32_t slot, slot_count;
    VkCoreImage dummy, lut;
    int lut_valid, lut_generation;
    NvkpScene scene[2];
    NvkpScene edges;
    uint32_t scene_w, scene_h;
    int composed;           /* index of the last composed scene, -1 before the first */
    int hist_valid, hist_rect[4];
    unsigned aa_frame;
    int compose_cache, cache_valid;
    NvkpComposeKey cache_key;
    /* what the swap needs from the last compose */
    struct { int valid, phase; uint32_t slot, generation; } pend;
    NvkpUiLayer ui[NVKP_UI_LAYERS];
    int ui_front, ui_back;
    uint64_t composes, compose_failures, swaps, swap_failures, swap_skips, retired;
    uint64_t compose_draws, compose_reuses, image_updates, phase_composes;
    uint64_t gpu_composes, gpu_compose_ns;
    int wait_diagnostics;
    uint64_t compose_wait_ns, ui_consume_wait_ns, scene_rebuilds, ui_updates;
    char reason[160];
} P;

static void nvkp_note(const char *reason) { snprintf(P.reason, sizeof(P.reason), "%s", reason); }
static uint64_t nvkp_clock_ns(void *user_data);

/* Opt-in, bounded trace. The presenter owns the ring and the private Wayland
 * event queue; teardown writes it only after that thread has stopped. No file
 * I/O, readbacks or additional GPU waits occur during measurement. */
enum { NVKP_TRACE_CAP = 16384 };
typedef struct NvkpTraceEvent {
    uint64_t sequence, source, guest_cycle, pixel_digest;
    uint64_t selected_ns, composed_ns, acquire_begin_ns, acquire_end_ns;
    uint64_t present_begin_ns, present_end_ns;
    uint32_t slot, generation, image_index;
    int queued, requested;
    PsxWaylandPresentationEvent feedback;
} NvkpTraceEvent;
static struct {
    FILE *file;
    NvkpTraceEvent *events, *current;
    uint64_t sequence;
    int feedback_active;
} T;
static _Atomic int s_trace_active, s_trace_finish;

int native_vk_services_trace_control(int finish)
{
    if (finish) atomic_store(&s_trace_finish, 1);
    return atomic_load(&s_trace_active);
}

static void nvkp_trace_feedback(const PsxWaylandPresentationEvent *feedback, void *opaque)
{
    (void)opaque;
    if (!T.events || !feedback || !feedback->swap_sequence) return;
    NvkpTraceEvent *event = &T.events[feedback->swap_sequence % NVKP_TRACE_CAP];
    if (event->sequence == feedback->swap_sequence) event->feedback = *feedback;
}

static void nvkp_trace_init(struct SDL_Window *window)
{
    const char *path = getenv("PSX_NATIVE_VK_PRESENT_TRACE");
    if (!path || !path[0]) return;
    T.file = fopen(path, "w");
    if (!T.file) return;
    T.events = calloc(NVKP_TRACE_CAP, sizeof(*T.events));
    if (!T.events) { fclose(T.file); T.file = NULL; return; }
    T.feedback_active = psx_wayland_presentation_init_private(window, nvkp_trace_feedback, NULL);
    atomic_store(&s_trace_finish, 0);
    atomic_store(&s_trace_active, 1);
}

static void nvkp_trace_finish(void)
{
    if (!T.file) return;
    if (T.feedback_active) {
        psx_wayland_presentation_poll();
        psx_wayland_presentation_shutdown();
    }
    fprintf(T.file, "sequence,source,guest_cycle,pixel_digest,slot,generation,image_index,"
        "selected_ns,composed_ns,acquire_begin_ns,acquire_end_ns,present_begin_ns,present_end_ns,"
        "queued,requested,feedback,request_ns,presentation_ns,refresh_sequence,refresh_ns,flags\n");
    const uint64_t first = T.sequence >= NVKP_TRACE_CAP ? T.sequence - NVKP_TRACE_CAP + 1u : 1u;
    for (uint64_t seq = first; seq <= T.sequence; ++seq) {
        const NvkpTraceEvent *e = &T.events[seq % NVKP_TRACE_CAP];
        const PsxWaylandPresentationEvent *f = &e->feedback;
        fprintf(T.file, "%llu,%llu,%llu,%llu,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%d,%llu,%llu,%llu,%u,%u\n",
            (unsigned long long)e->sequence, (unsigned long long)e->source,
            (unsigned long long)e->guest_cycle, (unsigned long long)e->pixel_digest,
            e->slot, e->generation, e->image_index,
            (unsigned long long)e->selected_ns, (unsigned long long)e->composed_ns,
            (unsigned long long)e->acquire_begin_ns, (unsigned long long)e->acquire_end_ns,
            (unsigned long long)e->present_begin_ns, (unsigned long long)e->present_end_ns,
            e->queued, e->requested, f->swap_sequence ? (f->presented ? 1 : 2) : 0,
            (unsigned long long)f->request_ns, (unsigned long long)f->presentation_time_ns,
            (unsigned long long)f->refresh_sequence, f->refresh_ns, f->flags);
    }
    fclose(T.file);
    free(T.events);
    memset(&T, 0, sizeof(T));
    atomic_store(&s_trace_active, 0);
}

/* ---- helpers ------------------------------------------------------------------------------------------------------------- */
static void nvkp_barrier_all(VkCommandBuffer cmd)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = mb.srcAccessMask;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* Producer-side publication avoids an ALL_COMMANDS wait in every consumer.
 * Scene edge reads must also finish before the next edge render overwrites
 * that shared target. UI layers use real consumed fences for that lifetime. */
static void nvkp_publish_color(VkCommandBuffer cmd, int protect_reads)
{
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (protect_reads ? VK_ACCESS_SHADER_READ_BIT : 0u);
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | (protect_reads ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u);
    const VkPipelineStageFlags source = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        (protect_reads ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0u);
    const VkPipelineStageFlags destination = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        (protect_reads ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0u);
    vkCmdPipelineBarrier(cmd, source, destination, 0u, 1u, &barrier, 0u, NULL, 0u, NULL);
}

static void nvkp_release_scene(VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0u, 1u, &barrier, 0u, NULL, 0u, NULL);
}

static VkDescriptorSetLayout nvkp_layout(const VkDescriptorSetLayoutBinding *bindings, uint32_t count)
{
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = count; ci.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    return vk_core_check(vkCreateDescriptorSetLayout(vk_core_device(), &ci, NULL, &layout), "vkCreateDescriptorSetLayout")
        ? layout : VK_NULL_HANDLE;
}

static VkSampler nvkp_sampler(VkFilter filter)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = ci.minFilter = filter;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    return vk_core_check(vkCreateSampler(vk_core_device(), &ci, NULL, &sampler), "vkCreateSampler") ? sampler : VK_NULL_HANDLE;
}

static VkPipeline nvkp_pipeline_specialized(VkPipelineLayout layout, VkRenderPass pass, const uint32_t *vs, size_t vs_bytes,
                                const uint32_t *fs, size_t fs_bytes, int blend, const char *name,
                                const VkSpecializationInfo *specialization)
{
    VkShaderModule vm = vk_core_shader_module(vs, vs_bytes, name);
    VkShaderModule fm = vk_core_shader_module(fs, fs_bytes, name);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo stages[2];
        memset(stages, 0, sizeof(stages));
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vm; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fm; stages[1].pName = "main";
        stages[1].pSpecializationInfo = specialization;
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1u; vp.scissorCount = 1u;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState att;
        memset(&att, 0, sizeof(att));
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (blend) {
            /* The UI layer is premultiplied (ImGui blends into a transparent target). */
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1u; cb.pAttachments = &att;
        const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2u; ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.stageCount = 2u; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb; ci.pDynamicState = &ds; ci.layout = layout; ci.renderPass = pass;
        if (!vk_core_check(vkCreateGraphicsPipelines(vk_core_device(), vk_core_pipeline_cache(), 1u, &ci, NULL, &pipeline),
                           "vkCreateGraphicsPipelines")) pipeline = VK_NULL_HANDLE;
    }
    if (vm) vkDestroyShaderModule(vk_core_device(), vm, NULL);
    if (fm) vkDestroyShaderModule(vk_core_device(), fm, NULL);
    return pipeline;
}

static VkPipeline nvkp_pipeline(VkPipelineLayout layout, VkRenderPass pass, const uint32_t *vs, size_t vs_bytes,
                                const uint32_t *fs, size_t fs_bytes, int blend, const char *name)
{
    return nvkp_pipeline_specialized(layout, pass, vs, vs_bytes, fs, fs_bytes, blend, name, NULL);
}

static VkPipeline nvkp_present_pipeline(const NvkpDraw *draw)
{
    const int cached = P.smaa_prepass && draw->aa_mode == 2;
    const VkPipeline fallback = cached ? P.pipe_smaa_cached : P.pipe_present;
    if (!P.specialize_present) return fallback;
    const int32_t key[5] = { draw->aa_mode, draw->aa_factor, draw->aa_mask_valid, draw->aa_nearest, draw->screenlut_on };
    for (uint32_t i = 0u; i < P.present_pipeline_count; ++i)
        if (!memcmp(P.present_pipelines[i].key, key, sizeof(key))) return P.present_pipelines[i].pipeline;
    if (P.present_pipeline_count == 64u) return fallback;
    VkSpecializationMapEntry entries[5];
    for (uint32_t i = 0u; i < 5u; ++i) entries[i] = (VkSpecializationMapEntry){ i, i * sizeof(key[0]), sizeof(key[0]) };
    const VkSpecializationInfo specialization = { 5u, entries, sizeof(key), key };
    VkPipeline pipeline = nvkp_pipeline_specialized(P.pl_present, P.pass_scene, spv_present_vert, spv_present_vert_size,
        cached ? spv_present_frag_SMAA_CACHED_SPECIALIZED : spv_present_frag_NATIVE_SPECIALIZED,
        cached ? spv_present_frag_SMAA_CACHED_SPECIALIZED_size : spv_present_frag_NATIVE_SPECIALIZED_size,
        0, "native spatial AA", &specialization);
    if (!pipeline) return fallback;
    memcpy(P.present_pipelines[P.present_pipeline_count].key, key, sizeof(key));
    P.present_pipelines[P.present_pipeline_count++].pipeline = pipeline;
    return pipeline;
}

static VkRenderPass nvkp_pass_format(VkFormat format, VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final_layout)
{
    VkAttachmentDescription att;
    memset(&att, 0, sizeof(att));
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = load; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = initial; att.finalLayout = final_layout;
    const VkAttachmentReference ref = { 0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1u; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1u; ci.pAttachments = &att;
    ci.subpassCount = 1u; ci.pSubpasses = &sub;
    VkRenderPass pass = VK_NULL_HANDLE;
    return vk_core_check(vkCreateRenderPass(vk_core_device(), &ci, NULL, &pass), "vkCreateRenderPass") ? pass : VK_NULL_HANDLE;
}

static VkRenderPass nvkp_pass(VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final_layout)
{
    return nvkp_pass_format(VK_FORMAT_R8G8B8A8_UNORM, load, initial, final_layout);
}

static VkDescriptorSet nvkp_set_alloc(VkDescriptorPool pool, VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool; ai.descriptorSetCount = 1u; ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    return vk_core_check(vkAllocateDescriptorSets(vk_core_device(), &ai, &set), "vkAllocateDescriptorSets") ? set : VK_NULL_HANDLE;
}

static void nvkp_write_sampler(VkDescriptorSet set, uint32_t binding, VkSampler sampler, VkImageView view, VkImageLayout layout)
{
    VkDescriptorImageInfo info = { sampler, view, layout };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1u;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &info;
    vkUpdateDescriptorSets(vk_core_device(), 1u, &w, 0u, NULL);
}

/* ---- scene images ------------------------------------------------------------------------------------------------------------ */
static void nvkp_scene_destroy(void)
{
    for (unsigned i = 0u; i < 2u; ++i) {
        if (P.scene[i].framebuffer) vkDestroyFramebuffer(vk_core_device(), P.scene[i].framebuffer, NULL);
        vk_core_image_destroy(&P.scene[i].image);
        memset(&P.scene[i], 0, sizeof(P.scene[i]));
    }
    P.scene_w = P.scene_h = 0u;
    if (P.edges.framebuffer) vkDestroyFramebuffer(vk_core_device(), P.edges.framebuffer, NULL);
    vk_core_image_destroy(&P.edges.image); memset(&P.edges, 0, sizeof(P.edges));
    P.hist_valid = 0;
    P.cache_valid = 0;
}

/* Window-sized ping-pong scene images; a resize waits for the device (rare). */
static int nvkp_scene_ensure(uint32_t width, uint32_t height)
{
    if (P.scene[0].image.image && P.scene_w == width && P.scene_h == height) return 1;
    ++P.scene_rebuilds;
    (void)vk_core_device_wait_idle();
    nvkp_scene_destroy();
    if (P.static_pool) (void)vkResetDescriptorPool(vk_core_device(), P.static_pool, 0);
    VkCommandBuffer cmd = vk_core_oneshot_begin();
    if (!cmd) return 0;
    for (unsigned i = 0u; i < 2u; ++i) {
        if (!vk_core_image_create(&P.scene[i].image, VK_FORMAT_R8G8B8A8_UNORM, width, height, 1u,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                (P.smaa_audit ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u), "native present scene")) {
            (void)vk_core_oneshot_end_wait(cmd);
            nvkp_scene_destroy();
            return 0;
        }
        vk_core_image_barrier(cmd, P.scene[i].image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (P.smaa_prepass) {
        if (!vk_core_image_create(&P.edges.image, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, 1u,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "native SMAA edges")) {
            (void)vk_core_oneshot_end_wait(cmd); nvkp_scene_destroy(); return 0;
        }
        vk_core_image_barrier(cmd, P.edges.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (!vk_core_oneshot_end_wait(cmd)) { nvkp_scene_destroy(); return 0; }
    for (unsigned i = 0u; i < 2u; ++i) {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = P.pass_scene; fi.attachmentCount = 1u; fi.pAttachments = &P.scene[i].image.view;
        fi.width = width; fi.height = height; fi.layers = 1u;
        if (!vk_core_check(vkCreateFramebuffer(vk_core_device(), &fi, NULL, &P.scene[i].framebuffer), "vkCreateFramebuffer")) {
            nvkp_scene_destroy();
            return 0;
        }
        P.scene[i].copy_set = nvkp_set_alloc(P.static_pool, P.dsl_tex);
        if (!P.scene[i].copy_set) { nvkp_scene_destroy(); return 0; }
        nvkp_write_sampler(P.scene[i].copy_set, 0u, P.sampler_linear, P.scene[i].image.view, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (P.smaa_prepass) {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = P.pass_edges; fi.attachmentCount = 1u; fi.pAttachments = &P.edges.image.view;
        fi.width = width; fi.height = height; fi.layers = 1u;
        if (!vk_core_check(vkCreateFramebuffer(vk_core_device(), &fi, NULL, &P.edges.framebuffer), "SMAA framebuffer")) {
            nvkp_scene_destroy(); return 0;
        }
    }
    P.scene_w = width; P.scene_h = height;
    P.composed = -1;
    P.hist_valid = 0;
    return 1;
}

/* GL native_presenter_update_lut: the host's screen LUT as a 256x128 image. */
static void nvkp_lut_update(void)
{
    const uint8_t *table = NULL;
    const int generation = gpu_screen_lut_snapshot(&table);
    if (generation == P.lut_generation) return;
    P.lut_generation = generation;
    if (!table) { P.lut_valid = 0; return; }
    if (!P.lut.image &&
        !vk_core_image_create(&P.lut, VK_FORMAT_R8G8B8A8_UNORM, 256u, 128u, 1u,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native screen lut")) {
        P.lut_valid = 0;
        return;
    }
    VkCoreBuffer stage;
    const VkDeviceSize bytes = 256u * 128u * 4u;
    if (!vk_core_buffer_create(&stage, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_CORE_MEMORY_UPLOAD, "native lut staging")) {
        P.lut_valid = 0;
        return;
    }
    uint8_t *dst = stage.mapped;
    for (unsigned i = 0u; i < 256u * 128u; ++i) {
        dst[i * 4u] = table[i * 3u]; dst[i * 4u + 1u] = table[i * 3u + 1u];
        dst[i * 4u + 2u] = table[i * 3u + 2u]; dst[i * 4u + 3u] = 255u;
    }
    vk_core_buffer_flush(&stage, 0, bytes);
    VkCommandBuffer cmd = vk_core_oneshot_begin();
    int ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        nvkp_barrier_all(cmd);
        vk_core_image_barrier(cmd, P.lut.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        region.imageExtent = (VkExtent3D){ 256u, 128u, 1u };
        vkCmdCopyBufferToImage(cmd, stage.buffer, P.lut.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
        nvkp_barrier_all(cmd);
        ok = vk_core_oneshot_end_wait(cmd);
    }
    vk_core_buffer_destroy(&stage);
    P.lut_valid = ok;
}

/* ---- compose ---------------------------------------------------------------------------------------------------------------------- */
static void nvkp_audit_check(NvkpSlot *slot)
{
    if (!slot->audit_bytes) return;
    vk_core_buffer_invalidate(&slot->audit, 0u, slot->audit_bytes * 2u);
    ++P.audit_checks;
    if (memcmp(slot->audit.mapped, (const uint8_t *)slot->audit.mapped + slot->audit_bytes,
               (size_t)slot->audit_bytes)) ++P.audit_mismatches;
    slot->audit_bytes = 0u;
}

/* Damage is relative to the preceding phase of this same immutable endpoint.
 * A jump over phases must include every intervening region. Changing the fit,
 * source generation, LUT or AA settings always requires a complete resolve. */
static int nvkp_damage_rect(const NativePresentSel *sel, const NvkpComposeKey *key, VkRect2D *rect)
{
    if (!P.smaa_damage || !P.smaa_prepass || key->aa_mode != 2 || !P.cache_valid || P.composed < 0 ||
        key->slot != P.cache_key.slot || key->generation != P.cache_key.generation ||
        !P.cache_key.image_index || key->image_index <= P.cache_key.image_index) return 0;
    NvkpComposeKey previous = P.cache_key;
    previous.image_index = key->image_index; previous.color = key->color; previous.mask = key->mask;
    if (memcmp(&previous, key, sizeof(previous)) || !sel->image.width || !sel->image.height) return 0;
    int32_t bounds[4] = { (int32_t)sel->image.width, (int32_t)sel->image.height, 0, 0 };
    for (uint32_t i = P.cache_key.image_index + 1u; i <= key->image_index; ++i) {
        const NativeImageSlot image = native_endpoint_image(&sel->endpoint, i);
        int32_t b[4];
        if (image.width != sel->image.width || image.height != sel->image.height ||
            !native_vk_endpoint_damage(image.color, b)) return 0;
        if (b[2] <= b[0] || b[3] <= b[1]) continue;
        if (bounds[0] > b[0]) bounds[0] = b[0];
        if (bounds[1] > b[1]) bounds[1] = b[1];
        if (bounds[2] < b[2]) bounds[2] = b[2];
        if (bounds[3] < b[3]) bounds[3] = b[3];
    }
    const double sx = (double)key->rect[2] / sel->image.width;
    const double sy = (double)key->rect[3] / sel->image.height;
    /* 32 output-pixel edge search + edge/mask neighbours + bilinear footprint.
     * Keep the margin conservative even when the source is magnified. */
    const int pad = 40 + (int)ceil(sx > sy ? sx : sy);
    int x0 = key->rect[0] + (int)floor(bounds[0] * sx) - pad;
    int y0 = key->rect[1] + (int)floor(bounds[1] * sy) - pad;
    int x1 = key->rect[0] + (int)ceil(bounds[2] * sx) + pad;
    int y1 = key->rect[1] + (int)ceil(bounds[3] * sy) + pad;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)key->width) x1 = (int)key->width;
    if (y1 > (int)key->height) y1 = (int)key->height;
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }
    if ((uint64_t)(x1 - x0) * (y1 - y0) >= (uint64_t)key->width * key->height) return 0;
    *rect = (VkRect2D){ { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
    return 1;
}

/* Refine a proven bounding box without dropping any intervening phase. Each
 * source cell gets the same conservative filter halo as the original box.
 * Overlapping halos are safe: both spatial passes write the same pure result
 * without blending. Count that repeated shading when choosing the cover. */
static unsigned nvkp_damage_regions(const NativePresentSel *sel, const NvkpComposeKey *key,
                                    const VkRect2D *box, VkRect2D output[NATIVE_VK_DAMAGE_REGIONS])
{
    output[0] = *box;
    if (!P.smaa_regions) return 1u;
    const unsigned side = P.smaa_regions == 2 ? 4u : 2u;
    int32_t bounds[NATIVE_VK_DAMAGE_REGIONS][4] = {{0}};
    for (uint32_t i = P.cache_key.image_index + 1u; i <= key->image_index; ++i) {
        const NativeImageSlot image = native_endpoint_image(&sel->endpoint, i);
        int32_t regions[NATIVE_VK_DAMAGE_REGIONS][4];
        if (native_vk_endpoint_damage_regions(image.color, regions) != (int)side) return 1u;
        for (unsigned q = 0u; q < side * side; ++q) {
            const int32_t *r = regions[q]; int32_t *b = bounds[q];
            if (r[0] >= r[2] || r[1] >= r[3]) continue;
            if (b[0] >= b[2] || b[1] >= b[3]) memcpy(b, r, sizeof(bounds[q]));
            else {
                if (b[0] > r[0]) b[0] = r[0]; if (b[1] > r[1]) b[1] = r[1];
                if (b[2] < r[2]) b[2] = r[2]; if (b[3] < r[3]) b[3] = r[3];
            }
        }
    }
    const double sx = (double)key->rect[2] / sel->image.width;
    const double sy = (double)key->rect[3] / sel->image.height;
    const int pad = 40 + (int)ceil(sx > sy ? sx : sy);
    VkRect2D regions[NATIVE_VK_DAMAGE_REGIONS]; unsigned count = 0u;
    uint64_t area = 0u;
    for (unsigned q = 0u; q < side * side; ++q) {
        const int32_t *b = bounds[q];
        if (b[0] >= b[2] || b[1] >= b[3]) continue;
        int x0 = key->rect[0] + (int)floor(b[0] * sx) - pad;
        int y0 = key->rect[1] + (int)floor(b[1] * sy) - pad;
        int x1 = key->rect[0] + (int)ceil(b[2] * sx) + pad;
        int y1 = key->rect[1] + (int)ceil(b[3] * sy) + pad;
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > (int)key->width) x1 = (int)key->width;
        if (y1 > (int)key->height) y1 = (int)key->height;
        if (x0 >= x1 || y0 >= y1) continue;
        regions[count++] = (VkRect2D){ { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
        area += (uint64_t)(x1 - x0) * (y1 - y0);
    }
    if (P.smaa_regions == 2) {
        /* Merge padded cells whenever their bounding rectangle costs no more
         * shading than the two draws. This removes duplicated halos without
         * ever excluding a damaged pixel or increasing the counted area. */
        for (;;) {
            int64_t saving = -1;
            unsigned left = 0u, right = 0u;
            VkRect2D joined = {{0}};
            for (unsigned a = 0u; a < count; ++a) for (unsigned b = a + 1u; b < count; ++b) {
                const VkRect2D *ra = &regions[a], *rb = &regions[b];
                const int32_t x0 = ra->offset.x < rb->offset.x ? ra->offset.x : rb->offset.x;
                const int32_t y0 = ra->offset.y < rb->offset.y ? ra->offset.y : rb->offset.y;
                const int32_t ax1 = ra->offset.x + (int32_t)ra->extent.width;
                const int32_t ay1 = ra->offset.y + (int32_t)ra->extent.height;
                const int32_t bx1 = rb->offset.x + (int32_t)rb->extent.width;
                const int32_t by1 = rb->offset.y + (int32_t)rb->extent.height;
                const int32_t x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
                const int64_t difference = (int64_t)ra->extent.width * ra->extent.height +
                    (int64_t)rb->extent.width * rb->extent.height - (int64_t)(x1 - x0) * (y1 - y0);
                if (difference > saving) {
                    saving = difference; left = a; right = b;
                    joined = (VkRect2D){ {x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)} };
                }
            }
            if (saving < 0) break;
            regions[left] = joined;
            regions[right] = regions[--count];
            area -= (uint64_t)saving;
        }
    }
    if (!count || area >= (uint64_t)box->extent.width * box->extent.height) return 1u;
    memcpy(output, regions, count * sizeof(*regions));
    return count;
}

static int nvkp_slot_begin(NvkpSlot *slot)
{
    if (slot->submitted) {
        const uint64_t started = P.wait_diagnostics ? nvkp_clock_ns(NULL) : 0u;
        if (!vk_core_check(vkWaitForFences(vk_core_device(), 1u, &slot->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return 0;
        nvkp_audit_check(slot);
        if (P.wait_diagnostics) P.compose_wait_ns += nvkp_clock_ns(NULL) - started;
        if (slot->timestamps) {
            uint64_t times[2];
            if (vkGetQueryPoolResults(vk_core_device(), slot->timestamps, 0u, 2u, sizeof(times), times,
                                     sizeof(times[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                ++P.gpu_composes;
                P.gpu_compose_ns += (uint64_t)((times[1] - times[0]) * (double)vk_core_caps()->timestamp_period_ns);
            }
        }
        if (!vk_core_check(vkResetFences(vk_core_device(), 1u, &slot->fence), "vkResetFences")) return 0;
        slot->submitted = 0;
    }
    if (!vk_core_check(vkResetDescriptorPool(vk_core_device(), slot->pool, 0), "vkResetDescriptorPool")) return 0;
    if (!vk_core_check(vkResetCommandBuffer(slot->cmd, 0), "vkResetCommandBuffer")) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk_core_check(vkBeginCommandBuffer(slot->cmd, &bi), "vkBeginCommandBuffer");
}

static bool nvkp_compose(const XgRenderCompiledEndpoint *compiled, uint64_t alpha_numerator, uint64_t alpha_denominator,
                         uint64_t *out_completion_fence, void *user_data)
{
    (void)user_data;
    if (atomic_exchange(&s_trace_finish, 0)) nvkp_trace_finish();
    const uint64_t selected_ns = T.events ? nvkp_clock_ns(NULL) : 0u;
    if (T.feedback_active) psx_wayland_presentation_poll();
    T.current = NULL;
    NativePresentSel sel;
    uint32_t blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_NONE;
    int cmd_open = 0;
    if (!native_present_select(&sel, compiled, alpha_numerator, alpha_denominator, out_completion_fence, P.ready))
        return false;
    if (sel.upload_required && !sel.staged_pixels) {
        blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_STAGING_MISSING;
        goto failed;
    }
    if (sel.upload_required) {
        int ok = g_native_backend->upload_staged(&sel.endpoint.image, (int)sel.endpoint.metadata.storage_width,
                                                 (int)sel.endpoint.metadata.storage_height, sel.staged_pixels);
        if (ok) { free(sel.staged_pixels); sel.staged_pixels = NULL; }
        if (!native_present_upload_result(&sel, ok)) { blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_UPLOAD; goto failed; }
    }
    VkImageView color = VK_NULL_HANDLE, mask = VK_NULL_HANDLE;
    if (!native_vk_endpoint_views(sel.endpoint.image.color, &color, &mask)) {
        blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_UPLOAD;
        goto failed;
    }
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(P.window, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_DRAWABLE;
        goto failed;
    }
    int aa_setting, aa_factor, stretch_num, stretch_den;
    gl_renderer_native_present_settings(&aa_setting, &aa_factor, &stretch_num, &stretch_den);
    int lx, ly, lw, lh;
    native_present_rect(&sel.endpoint.metadata, ww, wh, stretch_num, stretch_den, &lx, &ly, &lw, &lh);
    int aa_nearest;
    const int mask_valid = sel.image.mask_valid && mask != VK_NULL_HANDLE;
    const int aa_mode = native_present_aa_mode(&sel.endpoint.metadata, mask_valid, aa_setting, &aa_nearest);
    if (!sel.endpoint.metadata.render_width && P.limit_postprocess && aa_mode != 0 && lw > 0 && lh > 0) {
        /* Window size must not multiply the cost of every AA pass. Preserve
         * at least the complete internal raster and a full-HD AA canvas;
         * scale to larger/HiDPI windows once, after spatial/temporal resolve.
         * max, rather than min, avoids downsampling either source dimension
         * when the emulated pixels have a non-square display aspect. */
        const uint32_t target_w = sel.image.width > 1920u ? sel.image.width : 1920u;
        const uint32_t target_h = sel.image.height > 1080u ? sel.image.height : 1080u;
        const double ratio_x = (double)target_w / lw, ratio_y = (double)target_h / lh;
        const double ratio = ratio_x > ratio_y ? ratio_x : ratio_y;
        if (ratio < 1.0) {
            const int cw = (int)ceil(ww * ratio) + 2, ch = (int)ceil(wh * ratio) + 2;
            if (cw < ww) ww = cw;
            if (ch < wh) wh = ch;
            native_present_rect(&sel.endpoint.metadata, ww, wh, stretch_num, stretch_den, &lx, &ly, &lw, &lh);
        }
    }
    if (!nvkp_scene_ensure((uint32_t)ww, (uint32_t)wh)) {
        blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_DRAWABLE; goto failed;
    }
    P.copy_filter = aa_mode != 0;
    if (aa_mode == 3 && (lx != P.hist_rect[0] || ly != P.hist_rect[1] || lw != P.hist_rect[2] || lh != P.hist_rect[3]))
        P.hist_valid = 0;
    P.hist_rect[0] = lx; P.hist_rect[1] = ly; P.hist_rect[2] = lw; P.hist_rect[3] = lh;
    if (aa_mode != 3) P.hist_valid = 0;
    if (!sel.endpoint.metadata.depth24 && !sel.endpoint.metadata.disabled) nvkp_lut_update();
    const int lut_on = !sel.endpoint.metadata.depth24 && !sel.endpoint.metadata.disabled && P.lut_valid;

    NvkpComposeKey key;
    memset(&key, 0, sizeof(key));
    key.slot = sel.slot; key.generation = sel.generation; key.image_index = sel.image_index;
    key.width = (uint32_t)ww; key.height = (uint32_t)wh;
    key.color = color; key.mask = mask;
    key.rect[0] = lx; key.rect[1] = ly; key.rect[2] = lw; key.rect[3] = lh;
    key.aa_mode = aa_mode; key.aa_factor = aa_factor;
    key.aa_nearest = aa_nearest; key.mask_valid = mask_valid;
    key.lut_on = lut_on; key.lut_generation = P.lut_generation;
    const int source_changed = !P.cache_valid || key.slot != P.cache_key.slot ||
        key.generation != P.cache_key.generation || key.image_index != P.cache_key.image_index;
    /* TAA still accumulates history/jitter on holds. Spatial modes are pure
     * functions of the selected immutable image, fit, AA and LUT settings.
     * The UI stays live: nvkp_swap composites its latest layer separately. */
    if (P.compose_cache && aa_mode != 3 && P.composed >= 0 && P.cache_valid &&
        !memcmp(&key, &P.cache_key, sizeof(key))) {
        ++P.compose_reuses;
        goto composed;
    }

    NvkpSlot *slot = &P.slots[P.slot];
    if (!nvkp_slot_begin(slot)) { blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW; goto failed; }
    cmd_open = 1;
    VkCommandBuffer cmd = slot->cmd;
    VkRect2D damage;
    const int partial = nvkp_damage_rect(&sel, &key, &damage);
    VkRect2D damage_regions[NATIVE_VK_DAMAGE_REGIONS];
    const unsigned damage_count = partial ? nvkp_damage_regions(&sel, &key, &damage, damage_regions) : 0u;
    const int target = partial ? P.composed : P.composed < 0 ? 0 : P.composed ^ 1;
    const int history = target ^ 1;
    /* Per-draw state. */
    uint8_t *ubo = slot->ubo.mapped;
    memset(ubo, 0, NVKP_UBO_BYTES);
    NvkpRect rect = { { 0.f, 0.f, 1.f, 1.f } };
    memcpy(ubo, &rect, sizeof(rect));
    NvkpDraw draw;
    memset(&draw, 0, sizeof(draw));
    const unsigned phase = P.aa_frame++ % (unsigned)aa_factor + 1u;
    draw.aa_drawable[0] = (float)ww; draw.aa_drawable[1] = (float)wh;
    draw.aa_jitter[0] = native_aa_halton(phase, 2u); draw.aa_jitter[1] = native_aa_halton(phase, 3u);
    draw.screenlut_on = lut_on;
    draw.aa_mode = aa_mode; draw.aa_factor = aa_factor;
    draw.aa_history_valid = aa_mode == 3 && P.hist_valid;
    draw.aa_mask_valid = mask_valid; draw.aa_nearest = aa_nearest;
    const int edge_pass = P.smaa_prepass && aa_mode == 2;
    if (edge_pass) {
        draw.tex_size[0] = (float)lw; draw.tex_size[1] = (float)lh;
        draw.sharp_scale[0] = (float)lx; draw.sharp_scale[1] = (float)ly;
    }
    memcpy(ubo + NVKP_SLOT_ALIGN, &draw, sizeof(draw));
    vk_core_buffer_flush(&slot->ubo, 0, NVKP_UBO_BYTES);

    VkDescriptorSet set0 = nvkp_set_alloc(slot->pool, P.dsl_present0);
    VkDescriptorSet set1 = nvkp_set_alloc(slot->pool, P.dsl_present1);
    if (!set0 || !set1) { blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW; goto failed; }
    const VkImageLayout g = VK_IMAGE_LAYOUT_GENERAL;
    nvkp_write_sampler(set0, 0u, aa_mode == 0 || aa_nearest ? P.sampler_nearest : P.sampler_linear, color, g);
    nvkp_write_sampler(set0, 1u, P.sampler_nearest, lut_on ? P.lut.view : P.dummy.view, g);
    nvkp_write_sampler(set0, 2u, P.sampler_linear, edge_pass ? P.edges.image.view : aa_mode == 3 ? P.scene[history].image.view : P.dummy.view, g);
    nvkp_write_sampler(set0, 3u, P.sampler_nearest, mask_valid ? mask : P.dummy.view, g);
    VkDescriptorBufferInfo infos[2] = { { slot->ubo.buffer, 0, sizeof(NvkpRect) }, { slot->ubo.buffer, 0, sizeof(NvkpDraw) } };
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof(w));
    for (unsigned i = 0u; i < 2u; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set1; w[i].dstBinding = i; w[i].descriptorCount = 1u;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(vk_core_device(), 2u, w, 0u, NULL);

    /* Endpoints and reusable scene targets were published by their producers. */
    if (!P.publish_barriers) nvkp_barrier_all(cmd);
    if (partial) {
        VkMemoryBarrier load = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        load.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        load.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u, 1u, &load, 0u, NULL, 0u, NULL);
    }
    if (slot->timestamps) {
        vkCmdResetQueryPool(cmd, slot->timestamps, 0u, 2u);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->timestamps, 0u);
    }
    const VkClearValue clear = { .color = { .float32 = { 0.f, 0.f, 0.f, 1.f } } };
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = partial ? P.pass_scene_load : P.pass_scene; bi.framebuffer = P.scene[target].framebuffer;
    bi.renderArea.extent = (VkExtent2D){ (uint32_t)ww, (uint32_t)wh };
    bi.clearValueCount = 1u; bi.pClearValues = &clear;
    /* A negative viewport height keeps OpenGL's bottom-up clip space. */
    const VkViewport viewport = { (float)lx, (float)(ly + lh), (float)lw, -(float)lh, 0.f, 1.f };
    const VkRect2D full = { { 0, 0 }, { (uint32_t)ww, (uint32_t)wh } };
    const uint32_t offsets[2] = { 0u, NVKP_SLOT_ALIGN };
    const VkDeviceSize audit_bytes = (VkDeviceSize)ww * wh * 4u;
    int audit = partial && P.smaa_audit;
    if (audit && slot->audit.size < audit_bytes * 2u) {
        vk_core_buffer_destroy(&slot->audit);
        audit = vk_core_buffer_create(&slot->audit, audit_bytes * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_CORE_MEMORY_READBACK, "SMAA damage comparison");
    }
    for (unsigned pass = 0u; pass < (audit ? 2u : 1u); ++pass) {
        if (pass) bi.renderPass = P.pass_scene;
        const VkRect2D *scissors = partial && !pass ? damage_regions : &full;
        const unsigned scissor_count = partial && !pass ? damage_count : 1u;
        if (edge_pass) {
            VkRenderPassBeginInfo edges = bi;
            edges.renderPass = partial && !pass ? P.pass_edges_load : P.pass_edges; edges.framebuffer = P.edges.framebuffer;
            vkCmdBeginRenderPass(cmd, &edges, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe_edges);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl_present, 0u, 1u, &set0, 0u, NULL);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl_present, 1u, 1u, &set1, 2u, offsets);
            for (unsigned region = 0u; region < scissor_count; ++region) {
                vkCmdSetScissor(cmd, 0u, 1u, &scissors[region]);
                vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
            }
            vkCmdEndRenderPass(cmd);
            VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0u, 1u, &barrier, 0u, NULL, 0u, NULL);
        }
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0u, 1u, &viewport);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nvkp_present_pipeline(&draw));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl_present, 0u, 1u, &set0, 0u, NULL);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl_present, 1u, 1u, &set1, 2u, offsets);
        for (unsigned region = 0u; region < scissor_count; ++region) {
            vkCmdSetScissor(cmd, 0u, 1u, &scissors[region]);
            vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
        }
        vkCmdEndRenderPass(cmd);
        if (audit) {
            VkMemoryBarrier copy_barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            copy_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            copy_barrier.dstAccessMask = copy_barrier.srcAccessMask;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0u, 1u, &copy_barrier, 0u, NULL, 0u, NULL);
            VkBufferImageCopy copy = {0};
            copy.bufferOffset = pass * audit_bytes;
            copy.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
            copy.imageExtent = (VkExtent3D){ (uint32_t)ww, (uint32_t)wh, 1u };
            vkCmdCopyImageToBuffer(cmd, P.scene[target].image.image, VK_IMAGE_LAYOUT_GENERAL, slot->audit.buffer, 1u, &copy);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0u, 1u, &copy_barrier, 0u, NULL, 0u, NULL);
            slot->audit_bytes = audit_bytes;
        }
    }
    if (P.publish_barriers) nvkp_publish_color(cmd, 1);
    if (slot->timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->timestamps, 1u);
    if (!vk_core_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) { blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW; goto failed; }
    cmd_open = 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
    if (!vk_core_check(vk_core_submit(1u, &si, slot->fence), "vk_core_submit")) { blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW; goto failed; }
    slot->submitted = 1;
    P.slot = (P.slot + 1u) % P.slot_count;
    P.composed = target;
    P.hist_valid = aa_mode == 3;
    P.cache_key = key;
    P.cache_valid = 1;
    ++P.compose_draws;
    P.full_pixels += (uint64_t)ww * wh;
    if (partial) {
        ++P.partial_composes;
        P.partial_regions += damage_count;
        for (unsigned region = 0u; region < damage_count; ++region)
            P.partial_pixels += (uint64_t)damage_regions[region].extent.width * damage_regions[region].extent.height;
    } else P.partial_pixels += (uint64_t)ww * wh;
    if (source_changed) {
        ++P.image_updates;
        P.phase_composes += sel.image_index != 0u;
    }

composed: ;
    if (T.events) {
        const uint64_t sequence = ++T.sequence;
        T.current = &T.events[sequence % NVKP_TRACE_CAP];
        *T.current = (NvkpTraceEvent){
            .sequence = sequence, .source = compiled->identity.source_sequence,
            .guest_cycle = compiled->identity.guest_cycle, .pixel_digest = sel.image.digest,
            .slot = sel.slot, .generation = sel.generation, .image_index = sel.image_index,
            .selected_ns = selected_ns, .composed_ns = nvkp_clock_ns(NULL),
        };
    }
    /* The submission is on the queue: the endpoint needs no further protection from this compose. */
    const uint64_t handle = native_register_fence(NULL, GL_NATIVE_FENCE_KIND_SOFTWARE_READY, sel.slot, sel.generation);
    if (!handle) { blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_FENCE; goto failed; }
    *out_completion_fence = handle;
    native_present_commit(&sel, 0u);
    ++P.composes;
    SDL_LockMutex(s_native_state_mutex);
    P.pend.valid = 1; P.pend.phase = sel.image_index != 0u; P.pend.slot = sel.slot; P.pend.generation = sel.generation;
    SDL_UnlockMutex(s_native_state_mutex);
    return true;

failed:
    if (cmd_open) (void)vkEndCommandBuffer(P.slots[P.slot].cmd);
    ++P.compose_failures;
    native_present_abort(&sel, blocker);
    return false;
}

/* ---- swap ------------------------------------------------------------------------------------------------------------------------------ */
static void nvkp_swap(void *user_data)
{
    (void)user_data;
    int pending = 0, phase = 0;
    uint32_t slot = 0u, generation = 0u;
    SDL_LockMutex(s_native_state_mutex);
    pending = s_native_pending_present_valid && P.pend.valid;
    phase = P.pend.phase; slot = P.pend.slot; generation = P.pend.generation;
    if (pending && phase && s_native_endpoints[slot].generation == generation &&
        s_native_endpoints[slot].temporal_generation != s_native_temporal_generation) {
        /* A captured rate transition expired the phase between compose and swap. */
        s_native_pipeline_diag.compose_retired_before_swap++;
        s_native_pending_present_valid = 0;
        s_native_pending_present_sequence = 0u;
        P.pend.valid = 0;
        ++P.retired;
        SDL_UnlockMutex(s_native_state_mutex);
        return;
    }
    if (pending) s_native_pipeline_diag.swap_attempts++;
    SDL_UnlockMutex(s_native_state_mutex);
    int ok = 0;
    uint32_t blocker = pending ? GL_RENDERER_NATIVE_PRESENT_BLOCKER_SWAP_REJECTED : GL_RENDERER_NATIVE_PRESENT_BLOCKER_INVALID_ARGUMENT;
    int skipped = 0;
    if (pending && P.composed >= 0) {
        VkSwapFrame frame;
        memset(&frame, 0, sizeof(frame));
        if (T.current) T.current->acquire_begin_ns = nvkp_clock_ns(NULL);
        const VkSwapAcquire got = vk_swapchain_acquire(&frame);
        if (T.current) T.current->acquire_end_ns = nvkp_clock_ns(NULL);
        if (got == VK_SWAP_ACQUIRE_OK) {
            VkCommandBuffer cmd = frame.cmd;
            if (!P.publish_barriers) nvkp_barrier_all(cmd);
            vk_swapchain_begin_render_pass(&frame);
            const VkViewport viewport = { 0.f, 0.f, (float)frame.width, (float)frame.height, 0.f, 1.f };
            const VkRect2D full = { { 0, 0 }, { frame.width, frame.height } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &full);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe_copy);
            VkDescriptorSet scene_set = P.scene[P.composed].copy_set;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl_copy, 0u, 1u, &scene_set, 0u, NULL);
            const int32_t filtered = P.copy_filter && (frame.width != P.scene_w || frame.height != P.scene_h);
            vkCmdPushConstants(cmd, P.pl_copy, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(filtered), &filtered);
            vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
            /* UI layer over the whole window. */
            int ui = -1;
            SDL_LockMutex(s_native_state_mutex);
            if (P.ui_front >= 0 && P.ui[P.ui_front].image.image && P.ui[P.ui_front].set &&
                P.ui[P.ui_front].width == frame.width && P.ui[P.ui_front].height == frame.height)
                ui = P.ui_front;
            const int clip[4] = { ui >= 0 ? P.ui[ui].clip[0] : 0, ui >= 0 ? P.ui[ui].clip[1] : 0,
                                   ui >= 0 ? P.ui[ui].clip[2] : 0, ui >= 0 ? P.ui[ui].clip[3] : 0 };
            if (!clip[2] || !clip[3]) ui = -1;
            if (ui >= 0) ++P.ui[ui].readers;
            VkDescriptorSet ui_set = ui >= 0 ? P.ui[ui].set : VK_NULL_HANDLE;
            SDL_UnlockMutex(s_native_state_mutex);
            const uint32_t consume_slot = frame.slot % NVKP_SLOTS;
            if (ui >= 0 && P.ui[ui].consumed_pending[consume_slot]) {
                /* This frame slot can reuse its fence only after its previous
                 * read completed. The CPU reservation prevents resize/reuse
                 * of the descriptor and image while commands are recorded. */
                NvkpUiLayer *layer = &P.ui[ui];
                const uint64_t started = P.wait_diagnostics ? nvkp_clock_ns(NULL) : 0u;
                const VkResult waited = vkWaitForFences(vk_core_device(), 1u, &layer->consumed[consume_slot], VK_TRUE, UINT64_MAX);
                if (P.wait_diagnostics) P.ui_consume_wait_ns += nvkp_clock_ns(NULL) - started;
                const int reset = waited == VK_SUCCESS && vk_core_check(
                    vkResetFences(vk_core_device(), 1u, &layer->consumed[consume_slot]), "UI consumed fence reset");
                SDL_LockMutex(s_native_state_mutex);
                if (reset) layer->consumed_pending[consume_slot] = 0;
                else { --layer->readers; ui = -1; }
                SDL_UnlockMutex(s_native_state_mutex);
                if (waited != VK_SUCCESS) (void)vk_core_check(waited, "UI consumed fence wait");
            }
            if (ui >= 0 && clip[2] > 0 && clip[3] > 0) {
                const VkRect2D rect = { { clip[0], clip[1] }, { (uint32_t)clip[2], (uint32_t)clip[3] } };
                vkCmdSetScissor(cmd, 0u, 1u, &rect);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe_blend);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl_tex, 0u, 1u, &ui_set, 0u, NULL);
                vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
            } else ui = -1;
            vk_swapchain_end_render_pass(&frame);
            if (P.publish_barriers) nvkp_release_scene(cmd);
            if (T.current) {
                T.current->present_begin_ns = nvkp_clock_ns(NULL);
                if (T.feedback_active)
                    T.current->requested = psx_wayland_presentation_request(T.current->sequence);
            }
            ok = vk_swapchain_present(&frame, 0u, NULL, NULL, 0u, NULL);
            if (T.current) {
                T.current->present_end_ns = nvkp_clock_ns(NULL);
                T.current->queued = ok;
            }
            if (ui >= 0) {
                /* Protect EVERY submitted read. An older signalled fence
                 * alone cannot authorize overwriting a layer read by newer
                 * frames, particularly during maximize/restore transitions. */
                NvkpUiLayer *layer = &P.ui[ui];
                const int submitted = vk_core_check(vk_core_submit(0u, NULL, layer->consumed[consume_slot]), "vk_core_submit");
                SDL_LockMutex(s_native_state_mutex);
                if (submitted) layer->consumed_pending[consume_slot] = 1;
                --layer->readers;
                SDL_UnlockMutex(s_native_state_mutex);
            }
        } else if (got == VK_SWAP_ACQUIRE_SKIP) {
            skipped = 1;   /* minimized: nothing to show, not an error */
        }
        if (!ok && !skipped) blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_SWAP_REJECTED;
    }
    SDL_LockMutex(s_native_state_mutex);
    if (ok) {
        ++P.swaps;
        s_native_pipeline_diag.swap_successes++;
        s_native_pipeline_diag.last_present_blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_NONE;
    } else if (skipped) {
        ++P.swap_skips;
    } else {
        ++P.swap_failures;
        s_native_pipeline_diag.swap_failures++;
        s_native_pipeline_diag.last_present_blocker = blocker;
    }
    s_native_pending_present_sequence = 0u;
    s_native_pending_present_valid = 0;
    P.pend.valid = 0;
    SDL_UnlockMutex(s_native_state_mutex);
}

static uint64_t nvkp_clock_ns(void *user_data)
{
    (void)user_data;
    return SDL_GetTicksNS();
}

/* ---- UI layer ----------------------------------------------------------------------------------------------------------------------------- */
VkRenderPass native_vk_ui_render_pass(void) { return P.pass_ui; }

static void nvkp_ui_layer_destroy(NvkpUiLayer *layer)
{
    if (layer->framebuffer) vkDestroyFramebuffer(vk_core_device(), layer->framebuffer, NULL);
    vk_core_image_destroy(&layer->image);
    layer->framebuffer = VK_NULL_HANDLE;
    if (layer->set) (void)vkFreeDescriptorSets(vk_core_device(), P.ui_pool, 1u, &layer->set);
    layer->set = VK_NULL_HANDLE;
    layer->width = layer->height = 0u;
}

VkCommandBuffer native_vk_ui_begin_clipped(int width, int height, int content_height)
{
    if (!P.ready || !s_native_state_mutex || width <= 0 || height <= 0) return VK_NULL_HANDLE;
    if (content_height <= 0) return VK_NULL_HANDLE;
    if (content_height > height) content_height = height;
    const uint32_t rows = (uint32_t)content_height;
    const uint32_t rounded = (rows + 63u) & ~63u;
    const uint32_t storage_height = rounded < (uint32_t)height ? rounded : (uint32_t)height;
    SDL_LockMutex(s_native_state_mutex);
    const int back = P.ui_front == 0 ? 1 : 0;
    NvkpUiLayer *layer = &P.ui[back];
    if (layer->readers) { SDL_UnlockMutex(s_native_state_mutex); return VK_NULL_HANDLE; }
    /* Only the UI producer publishes ui_front. This back layer therefore
     * stays private until ui_end; driver waits/allocation must not hold the
     * Native compiler's shared state mutex. */
    SDL_UnlockMutex(s_native_state_mutex);
    int busy = 0;
    for (uint32_t i = 0u; i < NVKP_SLOTS; ++i) if (layer->consumed_pending[i]) {
        if (vkGetFenceStatus(vk_core_device(), layer->consumed[i]) == VK_SUCCESS) {
            (void)vkResetFences(vk_core_device(), 1u, &layer->consumed[i]);
            layer->consumed_pending[i] = 0;
        } else busy = 1;   /* the presenter still reads it */
    }
    if (busy) return VK_NULL_HANDLE;
    if (layer->done_pending) {
        if (vkGetFenceStatus(vk_core_device(), layer->done) != VK_SUCCESS) return VK_NULL_HANDLE;
        (void)vkResetFences(vk_core_device(), 1u, &layer->done);
        layer->done_pending = 0;
    }
    if (!layer->image.image || layer->image.width != (uint32_t)width || layer->image.height < storage_height) {
        nvkp_ui_layer_destroy(layer);
        if (!vk_core_image_create(&layer->image, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)width, storage_height, 1u,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "native ui layer")) return VK_NULL_HANDLE;
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = P.pass_ui; fi.attachmentCount = 1u; fi.pAttachments = &layer->image.view;
        fi.width = (uint32_t)width; fi.height = storage_height; fi.layers = 1u;
        if (!vk_core_check(vkCreateFramebuffer(vk_core_device(), &fi, NULL, &layer->framebuffer), "vkCreateFramebuffer")) {
            nvkp_ui_layer_destroy(layer);
            return VK_NULL_HANDLE;
        }
        layer->set = nvkp_set_alloc(P.ui_pool, P.dsl_tex);
        if (layer->set) nvkp_write_sampler(layer->set, 0u, P.sampler_nearest, layer->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!layer->set) { nvkp_ui_layer_destroy(layer); return VK_NULL_HANDLE; }
    }
    layer->width = (uint32_t)width; layer->height = (uint32_t)height;
    if (!vk_core_check(vkResetCommandBuffer(layer->cmd, 0), "vkResetCommandBuffer")) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_core_check(vkBeginCommandBuffer(layer->cmd, &bi), "vkBeginCommandBuffer")) return VK_NULL_HANDLE;
    nvkp_barrier_all(layer->cmd);
    const VkClearValue clear = { .color = { .float32 = { 0.f, 0.f, 0.f, 0.f } } };
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = P.pass_ui; rp.framebuffer = layer->framebuffer;
    rp.renderArea.extent = (VkExtent2D){ (uint32_t)width, storage_height };
    rp.clearValueCount = 1u; rp.pClearValues = &clear;
    vkCmdBeginRenderPass(layer->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    P.ui_back = back;
    return layer->cmd;
}

VkCommandBuffer native_vk_ui_begin(int width, int height)
{
    return native_vk_ui_begin_clipped(width, height, height);
}

int native_vk_ui_end_rect(int x, int y, int width, int height)
{
    if (!P.ready || P.ui_back < 0) return 0;
    NvkpUiLayer *layer = &P.ui[P.ui_back];
    vkCmdEndRenderPass(layer->cmd);
    if (P.publish_barriers) nvkp_publish_color(layer->cmd, 0);
    int ok = vk_core_check(vkEndCommandBuffer(layer->cmd), "vkEndCommandBuffer");
    if (ok) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1u; si.pCommandBuffers = &layer->cmd;
        ok = vk_core_check(vk_core_submit(1u, &si, layer->done), "vk_core_submit");
        layer->done_pending = ok;
        P.ui_updates += ok != 0;
    }
    int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (int64_t)layer->width) right = layer->width;
    if (bottom > (int64_t)layer->height) bottom = layer->height;
    if (bottom > (int64_t)layer->image.height) bottom = layer->image.height;
    SDL_LockMutex(s_native_state_mutex);
    layer->clip[0] = x; layer->clip[1] = y;
    layer->clip[2] = right > x ? (int)(right - x) : 0;
    layer->clip[3] = bottom > y ? (int)(bottom - y) : 0;
    P.ui_front = ok && layer->clip[2] && layer->clip[3] ? P.ui_back : -1;
    SDL_UnlockMutex(s_native_state_mutex);
    P.ui_back = -1;
    return ok;
}

int native_vk_ui_end(void)
{
    if (P.ui_back < 0) return 0;
    return native_vk_ui_end_rect(0, 0, (int)P.ui[P.ui_back].width, (int)P.ui[P.ui_back].height);
}

void native_vk_ui_hide(void)
{
    if (!s_native_state_mutex) return;
    SDL_LockMutex(s_native_state_mutex);
    P.ui_front = -1;
    SDL_UnlockMutex(s_native_state_mutex);
}

/* ---- creation and teardown ------------------------------------------------------------------------------------------------------------ */
static void nvkp_destroy(void)
{
    nvkp_trace_finish();
    if (!vk_core_ready()) { memset(&P, 0, sizeof(P)); return; }
    VkDevice dev = vk_core_device();
    (void)vk_core_device_wait_idle();
    nvkp_scene_destroy();
    for (unsigned i = 0u; i < NVKP_UI_LAYERS; ++i) {
        nvkp_ui_layer_destroy(&P.ui[i]);
        if (P.ui[i].done) vkDestroyFence(dev, P.ui[i].done, NULL);
        for (uint32_t j = 0u; j < NVKP_SLOTS; ++j)
            if (P.ui[i].consumed[j]) vkDestroyFence(dev, P.ui[i].consumed[j], NULL);
    }
    for (unsigned i = 0u; i < NVKP_SLOTS; ++i) {
        nvkp_audit_check(&P.slots[i]);
        vk_core_buffer_destroy(&P.slots[i].audit);
        if (P.slots[i].fence) vkDestroyFence(dev, P.slots[i].fence, NULL);
        if (P.slots[i].timestamps) vkDestroyQueryPool(dev, P.slots[i].timestamps, NULL);
        if (P.slots[i].pool) vkDestroyDescriptorPool(dev, P.slots[i].pool, NULL);
        vk_core_buffer_destroy(&P.slots[i].ubo);
    }
    vk_core_image_destroy(&P.dummy);
    vk_core_image_destroy(&P.lut);
    if (P.static_pool) vkDestroyDescriptorPool(dev, P.static_pool, NULL);
    if (P.ui_pool) vkDestroyDescriptorPool(dev, P.ui_pool, NULL);
    if (P.pool_presenter) vkDestroyCommandPool(dev, P.pool_presenter, NULL);
    if (P.pool_ui) vkDestroyCommandPool(dev, P.pool_ui, NULL);
    if (P.pipe_present) vkDestroyPipeline(dev, P.pipe_present, NULL);
    if (P.pipe_edges) vkDestroyPipeline(dev, P.pipe_edges, NULL);
    if (P.pipe_smaa_cached) vkDestroyPipeline(dev, P.pipe_smaa_cached, NULL);
    for (uint32_t i = 0u; i < P.present_pipeline_count; ++i)
        vkDestroyPipeline(dev, P.present_pipelines[i].pipeline, NULL);
    if (P.pipe_copy) vkDestroyPipeline(dev, P.pipe_copy, NULL);
    if (P.pipe_blend) vkDestroyPipeline(dev, P.pipe_blend, NULL);
    if (P.pl_present) vkDestroyPipelineLayout(dev, P.pl_present, NULL);
    if (P.pl_tex) vkDestroyPipelineLayout(dev, P.pl_tex, NULL);
    if (P.pl_copy) vkDestroyPipelineLayout(dev, P.pl_copy, NULL);
    if (P.dsl_present0) vkDestroyDescriptorSetLayout(dev, P.dsl_present0, NULL);
    if (P.dsl_present1) vkDestroyDescriptorSetLayout(dev, P.dsl_present1, NULL);
    if (P.dsl_tex) vkDestroyDescriptorSetLayout(dev, P.dsl_tex, NULL);
    if (P.pass_scene) vkDestroyRenderPass(dev, P.pass_scene, NULL);
    if (P.pass_edges) vkDestroyRenderPass(dev, P.pass_edges, NULL);
    if (P.pass_scene_load) vkDestroyRenderPass(dev, P.pass_scene_load, NULL);
    if (P.pass_edges_load) vkDestroyRenderPass(dev, P.pass_edges_load, NULL);
    if (P.pass_ui) vkDestroyRenderPass(dev, P.pass_ui, NULL);
    if (P.sampler_nearest) vkDestroySampler(dev, P.sampler_nearest, NULL);
    if (P.sampler_linear) vkDestroySampler(dev, P.sampler_linear, NULL);
    memset(&P, 0, sizeof(P));
}

static int nvkp_create(struct SDL_Window *window)
{
    memset(&P, 0, sizeof(P));
    P.window = window;
    const char *waits = getenv("PSX_VK_WAIT_DIAGNOSTICS");
    P.wait_diagnostics = waits && waits[0] == '1';
    P.composed = -1;
    P.ui_front = P.ui_back = -1;
    P.lut_generation = -1;
    P.slot_count = 3u;
    const char *slots = getenv("PSX_NATIVE_VK_PRESENT_SLOTS");
    if (slots) {
        unsigned long count = strtoul(slots, NULL, 10);
        if (count >= 3u && count <= NVKP_SLOTS) P.slot_count = (uint32_t)count;
    }
    const char *publish = getenv("PSX_NATIVE_VK_PUBLISH_BARRIERS");
    P.publish_barriers = publish && publish[0] == '1';
    const char *compose_cache = getenv("PSX_NATIVE_VK_COMPOSE_CACHE");
    P.compose_cache = !(compose_cache && compose_cache[0] == '0');
    const char *specialize = getenv("PSX_NATIVE_VK_SPECIALIZE_PRESENT");
    P.specialize_present = !(specialize && specialize[0] == '0');
    const char *postprocess_limit = getenv("PSX_NATIVE_VK_POSTPROCESS_LIMIT");
    P.limit_postprocess = !(postprocess_limit && postprocess_limit[0] == '0');
    const char *prepass = getenv("PSX_NATIVE_VK_SMAA_PREPASS");
    P.smaa_prepass = !(prepass && prepass[0] == '0');
    const char *damage = getenv("PSX_NATIVE_VK_SMAA_DAMAGE");
    const char *audit = getenv("PSX_NATIVE_VK_SMAA_AUDIT");
    const char *regions = getenv("PSX_NATIVE_VK_SMAA_REGIONS");
    P.smaa_damage = damage && damage[0] == '1';
    P.smaa_audit = P.smaa_damage && audit && audit[0] == '1';
    P.smaa_regions = !P.smaa_damage || !regions ? 0 : regions[0] == '2' ? 2 : regions[0] == '1' ? 1 : 0;
    if (P.smaa_prepass) {
        VkFormatProperties format;
        vkGetPhysicalDeviceFormatProperties(vk_core_physical_device(), VK_FORMAT_R16G16B16A16_SFLOAT, &format);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        P.smaa_prepass = (format.optimalTilingFeatures & required) == required;
    }
    VkDevice dev = vk_core_device();
    P.sampler_nearest = nvkp_sampler(VK_FILTER_NEAREST);
    P.sampler_linear = nvkp_sampler(VK_FILTER_LINEAR);
    P.pass_scene = nvkp_pass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    P.pass_ui = nvkp_pass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (P.smaa_prepass) P.pass_edges = nvkp_pass_format(VK_FORMAT_R16G16B16A16_SFLOAT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    if (P.smaa_damage && P.smaa_prepass) {
        P.pass_scene_load = nvkp_pass(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        P.pass_edges_load = nvkp_pass_format(VK_FORMAT_R16G16B16A16_SFLOAT, VK_ATTACHMENT_LOAD_OP_LOAD,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        if (!P.pass_scene_load || !P.pass_edges_load) { nvkp_note("SMAA load passes failed"); return 0; }
    }
    if (!P.sampler_nearest || !P.sampler_linear || !P.pass_scene || !P.pass_ui) { nvkp_note("samplers or render passes failed"); return 0; }
    const VkShaderStageFlags frag = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding present0[4] = {
        { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL }, { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL }, { 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
    };
    const VkDescriptorSetLayoutBinding present1[2] = {
        { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, VK_SHADER_STAGE_VERTEX_BIT | frag, NULL },
        { 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, frag, NULL },
    };
    const VkDescriptorSetLayoutBinding tex = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL };
    P.dsl_present0 = nvkp_layout(present0, 4u);
    P.dsl_present1 = nvkp_layout(present1, 2u);
    P.dsl_tex = nvkp_layout(&tex, 1u);
    if (!P.dsl_present0 || !P.dsl_present1 || !P.dsl_tex) { nvkp_note("descriptor layouts failed"); return 0; }
    VkPipelineLayoutCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout sets[2] = { P.dsl_present0, P.dsl_present1 };
    pci.setLayoutCount = 2u; pci.pSetLayouts = sets;
    if (!vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &P.pl_present), "vkCreatePipelineLayout")) return 0;
    pci.setLayoutCount = 1u; pci.pSetLayouts = &P.dsl_tex;
    if (!vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &P.pl_tex), "vkCreatePipelineLayout")) return 0;
    const VkPushConstantRange resolve_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(int32_t) };
    pci.pushConstantRangeCount = 1u; pci.pPushConstantRanges = &resolve_range;
    if (!vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &P.pl_copy), "scene resolve layout")) return 0;
    P.pipe_present = nvkp_pipeline(P.pl_present, P.pass_scene, spv_present_vert, spv_present_vert_size,
                                   spv_present_frag_NATIVE_GENERIC, spv_present_frag_NATIVE_GENERIC_size, 0, "native present");
    if (P.smaa_prepass) {
        if (!P.pass_edges) return 0;
        P.pipe_edges = nvkp_pipeline(P.pl_present, P.pass_edges, spv_present_vert, spv_present_vert_size,
            spv_present_frag_SMAA_EDGES, spv_present_frag_SMAA_EDGES_size, 0, "native SMAA edges");
        P.pipe_smaa_cached = nvkp_pipeline(P.pl_present, P.pass_scene, spv_present_vert, spv_present_vert_size,
            spv_present_frag_SMAA_CACHED, spv_present_frag_SMAA_CACHED_size, 0, "native SMAA resolve");
        if (!P.pipe_edges || !P.pipe_smaa_cached) return 0;
    }
    VkRenderPass swap_pass = vk_swapchain_render_pass();
    P.pipe_copy = nvkp_pipeline(P.pl_copy, swap_pass, spv_fullscreen_vert_SCENE_RESOLVE, spv_fullscreen_vert_SCENE_RESOLVE_size,
                                spv_ui_layer_frag_SCENE_RESOLVE, spv_ui_layer_frag_SCENE_RESOLVE_size, 0, "native scene resolve");
    P.pipe_blend = nvkp_pipeline(P.pl_tex, swap_pass, spv_fullscreen_vert, spv_fullscreen_vert_size,
                                 spv_ui_layer_frag, spv_ui_layer_frag_size, 1, "native ui composite");
    if (!P.pipe_present || !P.pipe_copy || !P.pipe_blend) { nvkp_note("present pipelines failed"); return 0; }
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vk_core_queue_family();
    if (!vk_core_check(vkCreateCommandPool(dev, &cpi, NULL, &P.pool_presenter), "vkCreateCommandPool") ||
        !vk_core_check(vkCreateCommandPool(dev, &cpi, NULL, &P.pool_ui), "vkCreateCommandPool")) return 0;
    static const VkDescriptorPoolSize slot_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8u }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4u } };
    const char *gpu_timing = getenv("PSX_NATIVE_VK_GPU_TIMING");
    for (unsigned i = 0u; i < NVKP_SLOTS; ++i) {
        NvkpSlot *slot = &P.slots[i];
        if (gpu_timing && gpu_timing[0] == '1' && vk_core_caps()->timestamps) {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2u;
            if (!vk_core_check(vkCreateQueryPool(dev, &qi, NULL, &slot->timestamps), "vkCreateQueryPool")) return 0;
        }
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 4u; dpi.poolSizeCount = 2u; dpi.pPoolSizes = slot_sizes;
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = P.pool_presenter; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!vk_core_check(vkCreateDescriptorPool(dev, &dpi, NULL, &slot->pool), "vkCreateDescriptorPool") ||
            !vk_core_check(vkAllocateCommandBuffers(dev, &ai, &slot->cmd), "vkAllocateCommandBuffers") ||
            !vk_core_check(vkCreateFence(dev, &fi, NULL, &slot->fence), "vkCreateFence") ||
            !vk_core_buffer_create(&slot->ubo, NVKP_UBO_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_CORE_MEMORY_UPLOAD,
                                   "native present uniforms")) return 0;
    }
    static const VkDescriptorPoolSize static_sizes[1] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16u } };
    VkDescriptorPoolCreateInfo sp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    sp.maxSets = 16u; sp.poolSizeCount = 1u; sp.pPoolSizes = static_sizes;
    if (!vk_core_check(vkCreateDescriptorPool(dev, &sp, NULL, &P.static_pool), "vkCreateDescriptorPool")) return 0;
    static const VkDescriptorPoolSize ui_sizes[1] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4u } };
    VkDescriptorPoolCreateInfo up = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    up.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    up.maxSets = 4u; up.poolSizeCount = 1u; up.pPoolSizes = ui_sizes;
    if (!vk_core_check(vkCreateDescriptorPool(dev, &up, NULL, &P.ui_pool), "vkCreateDescriptorPool")) return 0;
    for (unsigned i = 0u; i < NVKP_UI_LAYERS; ++i) {
        NvkpUiLayer *layer = &P.ui[i];
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = P.pool_ui; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!vk_core_check(vkAllocateCommandBuffers(dev, &ai, &layer->cmd), "vkAllocateCommandBuffers") ||
            !vk_core_check(vkCreateFence(dev, &fi, NULL, &layer->done), "vkCreateFence")) return 0;
        for (uint32_t j = 0u; j < NVKP_SLOTS; ++j)
            if (!vk_core_check(vkCreateFence(dev, &fi, NULL, &layer->consumed[j]), "vkCreateFence")) return 0;
    }
    /* A transparent 1x1 image stands in for the unused LUT / history / mask samplers. */
    if (!vk_core_image_create(&P.dummy, VK_FORMAT_R8G8B8A8_UNORM, 1u, 1u, 1u,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native present dummy")) return 0;
    VkCommandBuffer cmd = vk_core_oneshot_begin();
    if (!cmd) return 0;
    VkClearColorValue zero;
    memset(&zero, 0, sizeof(zero));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
    vk_core_image_barrier(cmd, P.dummy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdClearColorImage(cmd, P.dummy.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1u, &range);
    if (P.publish_barriers) nvkp_barrier_all(cmd);
    if (!vk_core_oneshot_end_wait(cmd)) return 0;
    P.ready = 1;
    return 1;
}

/* ---- services ---------------------------------------------------------------------------------------------------------------------------- */
int native_vk_services_init(XgRenderWorkerServices *worker, XgRenderPresenterServices *presenter,
                            struct SDL_Window *window, char *reason, size_t reason_size)
{
    if (P.services) { snprintf(reason, reason_size, "Native Vulkan services already running"); return 0; }
    if (!vk_core_ready() || !vk_swapchain_active()) { snprintf(reason, reason_size, "vk_core or the swapchain is not up"); return 0; }
    if (!nvkp_create(window)) {
        snprintf(reason, reason_size, "presenter: %s", P.reason[0] ? P.reason : vk_core_failure());
        nvkp_destroy();
        return 0;
    }
    if (!gl_renderer_native_neutral_begin(worker)) {
        snprintf(reason, reason_size, "neutral Native state could not start");
        nvkp_destroy();
        return 0;
    }
    char executor_reason[160] = "";
    if (!native_vk_backend_start(executor_reason, sizeof(executor_reason))) {
        snprintf(reason, reason_size, "executor: %s", executor_reason);
        gl_renderer_native_neutral_end();
        nvkp_destroy();
        return 0;
    }
    *presenter = (XgRenderPresenterServices){
        .fence_status = native_fence_status,
        .compose = nvkp_compose,
        .swap_window = nvkp_swap,
        .discard_fence = native_discard_fence,
        .user_data = NULL,
        .owner_token = s_native_backend_generation,
        .clock_ns = nvkp_clock_ns,
        .source_buffer_intervals = 3u,
    };
    P.services = 1;
    nvkp_trace_init(window);
    return 1;
}

void native_vk_services_stop_worker(void)
{
    if (!P.services) return;
    native_prefetch_discard();
    native_phase_pool_stop();
    native_vk_backend_join();
}

void native_vk_services_shutdown(void)
{
    if (!P.services) return;
    native_vk_services_stop_worker();
    (void)vk_core_device_wait_idle();
    native_vk_backend_stop();
    gl_renderer_native_neutral_end();
    nvkp_destroy();
    P.services = 0;
}

int native_vk_services_active(void) { return P.services; }

int native_vk_services_json(char *out, int capacity)
{
    const int display = P.window ? (int)SDL_GetWindowDisplayIndex(P.window) : -1;
#if defined(PSX_SDL3)
    const int primary = (int)SDL_GetPrimaryDisplay();
#else
    const int primary = 0;
#endif
    SDL_DisplayMode mode;
    memset(&mode, 0, sizeof(mode));
    if (display >= 0) (void)SDL_GetCurrentDisplayMode(display, &mode);

    char executor[4096];
    (void)native_vk_backend_json(executor, sizeof(executor));
    VkSwapDiagnostics sw;
    vk_swapchain_diagnostics(&sw);
    int clip[4] = {0};
    uint32_t ui_storage[2] = {0};
    const uint64_t window_flags = P.window ? (uint64_t)SDL_GetWindowFlags(P.window) : 0u;
    if (s_native_state_mutex) {
        SDL_LockMutex(s_native_state_mutex);
        if (P.ui_front >= 0) {
            memcpy(clip, P.ui[P.ui_front].clip, sizeof(clip));
            ui_storage[0] = P.ui[P.ui_front].image.width;
            ui_storage[1] = P.ui[P.ui_front].image.height;
        }
        SDL_UnlockMutex(s_native_state_mutex);
    }
    return snprintf(out, (size_t)capacity,
        "{\"window_on_primary\":%d,\"host_display\":%d,\"host_refresh_hz\":%.3f,\"active\":%d,\"composes\":%llu,\"compose_failures\":%llu,\"swaps\":%llu,\"swap_failures\":%llu,"
        "\"swap_skips\":%llu,\"retired\":%llu,\"scene\":[%u,%u],\"lut\":%d,\"frames_presented\":%llu,"
        "\"compose_draws\":%llu,\"compose_reuses\":%llu,\"image_updates\":%llu,\"phase_composes\":%llu,"
        "\"partial_composes\":%llu,\"partial_pixels\":%llu,\"partial_regions\":%llu,\"full_pixels\":%llu,\"audit_checks\":%llu,\"audit_mismatches\":%llu,"
        "\"gpu_composes\":%llu,\"gpu_compose_ns\":%llu,\"ui_clip\":[%d,%d,%d,%d],\"ui_storage\":[%u,%u],"
        "\"window_maximized\":%d,\"window_fullscreen\":%d,\"window_focused\":%d,\"window_mouse_focused\":%d,"
        "\"compose_wait_ns\":%llu,\"ui_consume_wait_ns\":%llu,\"scene_rebuilds\":%llu,\"ui_updates\":%llu,\"executor\":%s}",
        display == primary, display, (double)mode.refresh_rate, P.services, (unsigned long long)P.composes, (unsigned long long)P.compose_failures, (unsigned long long)P.swaps,
        (unsigned long long)P.swap_failures, (unsigned long long)P.swap_skips, (unsigned long long)P.retired,
        P.scene_w, P.scene_h, P.lut_valid, (unsigned long long)sw.frames_presented,
        (unsigned long long)P.compose_draws, (unsigned long long)P.compose_reuses,
        (unsigned long long)P.image_updates, (unsigned long long)P.phase_composes,
        (unsigned long long)P.partial_composes, (unsigned long long)P.partial_pixels,
        (unsigned long long)P.partial_regions, (unsigned long long)P.full_pixels,
        (unsigned long long)P.audit_checks, (unsigned long long)P.audit_mismatches,
        (unsigned long long)P.gpu_composes, (unsigned long long)P.gpu_compose_ns, clip[0], clip[1], clip[2], clip[3], ui_storage[0], ui_storage[1],
        (window_flags & SDL_WINDOW_MAXIMIZED) != 0u, (window_flags & SDL_WINDOW_FULLSCREEN) != 0u,
        (window_flags & SDL_WINDOW_INPUT_FOCUS) != 0u, (window_flags & SDL_WINDOW_MOUSE_FOCUS) != 0u,
        (unsigned long long)P.compose_wait_ns, (unsigned long long)P.ui_consume_wait_ns,
        (unsigned long long)P.scene_rebuilds, (unsigned long long)P.ui_updates, executor);
}

#endif  /* PSX_HAVE_VULKAN */
