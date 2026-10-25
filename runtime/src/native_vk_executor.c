/* The Vulkan Native executor, CORE tier (see native_vk_executor.h).
 *
 * It executes a GlNativeGpuWork journal exactly like the GL executor
 * (gpu_gl_renderer.c native_gpu_service and what it calls) and keeps the GL
 * names of the steps it ports:
 *
 *   GL                                         Vulkan
 *   planes (RGBA8 + R32UI key, FBO)            NvkPlane: two VkCoreImage in GENERAL, one framebuffer
 *   FBO + glTextureBarrier                     draw render pass: both images are colour attachments
 *                                              and input attachments of its one subpass;
 *                                              glTextureBarrier = vkCmdPipelineBarrier with the
 *                                              subpass self-dependency (BY_REGION)
 *   hazard grid / layer scheduler              the same native_gpu_hazard.c decisions
 *   per-draw uniforms                          NvkView/NvkDraw std140 blocks in the upload arena,
 *                                              bound with dynamic offsets
 *   vertex slice VBO, template VBO             upload arena
 *   WORDS texture + glTexSubImage2D            R16UI image + staging copies
 *   phase texture snapshots (TBO)              uniform texel buffer
 *   scanout resolve + R8 mask                  scanout render passes, one draw per source span
 *   PBO readback + digest                      READBACK buffers + native_gpu_image_digest
 *   GLsync                                     VkFence, native_vk_sync_poll is vkGetFenceStatus
 *
 * A work is recorded into one command buffer and submitted once. The whole
 * state that the GL executor keeps in globals (committed planes, recipe
 * bases, spare planes, the words shadow, the uniform values) is kept in S.
 * Everything a single execution uses lives in R.
 *
 * Not implemented, and refused loudly per work (NativeVkRefusal): debug
 * mipmaps and the canonical provenance attachment. Depth view uses the shared
 * shader; wireframe strokes triangle edges without optional device features. Depth
 * occlusion queries and GPU timestamps are diagnostics only and not recorded. */

#include "native_vk_executor.h"
#include "native_resolution.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(PSX_HAVE_VULKAN)

int native_vk_shadow_enabled(void) { return 0; }
int native_vk_shadow_init(void) { return 0; }
void native_vk_shadow_gl_image(unsigned index, const void *pixels, uint32_t width, uint32_t height)
{
    (void)index; (void)pixels; (void)width; (void)height;
}
void native_vk_shadow_run(const struct GlNativeGpuWork *work, int gl_interlock)
{
    (void)work; (void)gl_interlock;
}
void native_vk_shadow_apply(const struct GlNativeGpuWork *work) { (void)work; }
void native_vk_shadow_shutdown(void) {}
int native_vk_shadow_json(char *out, int capacity)
{
    return snprintf(out, (size_t)capacity, "{\"enabled\":0,\"reason\":\"built without Vulkan\"}");
}
int native_vk_shadow_dump(const char *path, char *err, size_t err_size)
{
    (void)path;
    snprintf(err, err_size, "built without Vulkan");
    return 0;
}
int native_vk_backend_start(char *reason, size_t reason_size)
{
    snprintf(reason, reason_size, "built without Vulkan");
    return 0;
}
void native_vk_backend_stop(void) {}
void native_vk_backend_join(void) {}
int native_vk_backend_json(char *out, int capacity)
{
    return snprintf(out, (size_t)capacity, "{\"running\":0,\"reason\":\"built without Vulkan\"}");
}

#else  /* PSX_HAVE_VULKAN */

#include "native_work_internal.h"
#include "hd_texture_cache.h"
#include "vk_core.h"
#include "vk_shaders_spv.h"
#include "native_vk_depth_bounds.h"

/* ---- constants ----------------------------------------------------------- */
enum {
    NVK_ARENA_CHUNK = 16 << 20,
    NVK_SLOT_ALIGN = 256,           /* >= minUniformBufferOffsetAlignment (checked at init) */
    NVK_SLOT = 3 * NVK_SLOT_ALIGN,  /* NvkView at 0, NvkDraw at 256 */
    NVK_SET0_CACHE = 4096,
    NVK_MISMATCH_RING = 16,
    NVK_WORDS_BYTES = VRAM_W * VRAM_H * 2,
    NVK_IMAGES = GL_NATIVE_MOTION_PHASE_CAPACITY + 1u,
    NVK_UNIFORM_DEPTH_ATTACHED = 1, NVK_UNIFORM_DEPTH_READ = 2
};

/* std140 NativeView / NativeDraw of the core variant (native_gpu.frag, native_prelude.glsl). */
typedef struct NvkView { float size[4], raster[4], source_raster[4], raster_pages[4]; } NvkView;
typedef struct NvkDraw {
    int32_t hd_map[4], hd_lim[4], mip_rect[4], sample_lim[4], state[4], flags[4], page[4], window[4];
    float uv_gradient[4];
    int32_t depth_state[4], depth_plane[4], depth_view[4];
    float constant_color[4], attribute_origin[4], attribute_plane[5][4];
    int32_t attribute_dda[5][4];
    int32_t origin[2], depth_origin[2];
    int32_t hd_on, mip_on, u_depth, aa_exempt, texture_filter, anisotropy;
    float filter_strength;
    int32_t decoded_on;
} NvkDraw;
/* std140 NativeDraw of the interlock variant: the core fields, then the parameter-record selectors. */
typedef struct NvkDrawIL {
    int32_t hd_map[4], hd_lim[4], mip_rect[4], sample_lim[4], state[4], flags[4], page[4], window[4];
    float uv_gradient[4];
    int32_t depth_state[4], depth_plane[4], depth_view[4];
    float constant_color[4], attribute_origin[4], attribute_plane[5][4];
    int32_t attribute_dda[5][4];
    int32_t origin[2], depth_origin[2];
    int32_t hd_on, mip_on, parameters_on, parameter_base, parameter_shared;
    int32_t u_depth, aa_exempt, texture_filter, anisotropy;
    float filter_strength;
} NvkDrawIL;
_Static_assert(sizeof(NvkDrawIL) == 440, "NativeDraw interlock std140 size");
_Static_assert(offsetof(NvkDrawIL, hd_on) == offsetof(NvkDraw, hd_on), "interlock draw prefix");
_Static_assert(256 + sizeof(NvkDrawIL) <= NVK_SLOT, "uniform slot");
_Static_assert(sizeof(NvkView) == 64, "NativeView std140 size");
_Static_assert(sizeof(NvkDraw) == 432, "NativeDraw std140 size");
_Static_assert(256 + sizeof(NvkDraw) <= NVK_SLOT, "uniform slot");

/* A plane of the work (RGBA8 colour + R32UI depth key), or a scanout image
 * (RGBA8 + R8 class mask in key). */
typedef struct NvkPlane {
    VkCoreImage color, key;
    VkFramebuffer framebuffer;
    VkFramebuffer capture_framebuffer;
    VkImageView alpha_view;  /* scan: the R8 mask read as alpha (GL mask swizzle A<-R) */
    uint32_t width, height; /* virtual raster extent; color.width/height are physical */
    float raster[4]; /* virtual/physical X,Y; physical viewport origin X,Y */
    uint32_t page_rows, page_pixels; /* shared VRAM aliases start on integer pixels */
    uint64_t layout_generation; /* retired targets belong to one raster layout */
    uint64_t last_used_work;
    int has_key;    /* GL: the plane carries its depth attachment */
    int dirty_all;  /* GL: unknown writes since the last barrier */
    int scan;
    struct GlNativeGpuHazards *hazards;
    uint32_t *depth_max;
    uint32_t depth_columns, depth_rows, depth_scale;
    int depth_zero;
    /* Scanout pixels that may differ from the preceding phase of this work.
     * Valid only for a proven local replay; immutable after publication. */
    int damage_valid;
    int32_t damage_bounds[4];
    int damage_regions_side;
    int32_t damage_regions[NATIVE_VK_DAMAGE_REGIONS][4]; /* half-open scanout pixels */
} NvkPlane;

typedef struct NvkChunk {
    VkCoreBuffer buf;
    VkDeviceSize cursor;
    VkDescriptorSet native_set, scan_set;
    VkBufferView params_view;   /* interlock tier: the chunk as RGBA32UI texels */
} NvkChunk;

/* Resources a descriptor set 0 binds; words and the recipe view are shared. */
typedef struct NvkSetKey {
    VkImageView dest, dest_key, src, src_key, hd, decoded, filtered;
    VkSampler src_sampler, hd_sampler, filtered_sampler;
    VkBufferView recipe, params;
    VkBuffer capture_meta, capture_nodes;
    VkImageView scan_input;
} NvkSetKey;
typedef struct NvkSetEntry { NvkSetKey key; VkDescriptorSet set; } NvkSetEntry;

typedef struct NvkSeed { VkCoreImage image; } NvkSeed;

/* An HD replacement texture with its mip chain (behind a HdTextureCache handle). */
typedef struct NvkHd { VkCoreImage image; } NvkHd;

/* ---- process state ---------------------------------------------------------- */
static struct {
    int ready, tried;
    char reason[160];
    SDL_mutex *mutex;
    VkRenderPass pass_draw, pass_scan_clear, pass_scan_load, pass_il;
    VkDescriptorSetLayout dsl_native0, dsl_native1, dsl_scan0, dsl_scan1, dsl_il0;
    VkPipelineLayout pl_native, pl_scan, pl_il, pl_core_batch;
    VkPipeline pipe_draw[2], pipe_scan, pipe_il, pipe_core_batch[2];
    int core_batch, hd_batch, hd_record_buffer, vertex_footprint, first_vertex_records, lazy_vertices;
    int phase_damage, depth_bounds, shader_damage, damage_regions;
    unsigned damage_tile_shift;
    int capture_enabled, linked_capture, publish_barriers;
    int merge_capture, sort_capture, direct_capture;
    VkRenderPass pass_capture;
    VkPipeline pipe_capture_draw, pipe_capture_count, pipe_capture_resolve;
    VkPipelineLayout pl_capture;
    VkDescriptorSetLayout dsl_capture;
    VkCoreBuffer capture_meta, capture_nodes, capture_readback;
    uint32_t capture_capacity, capture_failed_scene;
    VkFence pace_fences[8];
    uint32_t pace_limit, pace_head, pace_count;
    uint64_t pace_margin_ns;
    int defer_submits, phase_seal;
    struct { VkCommandBuffer cmd; VkFence fence; } pending_submits[64];
    uint32_t pending_head, pending_count;
    VkPipeline pipe_decode;
    VkPipelineLayout pl_decode;
    VkDescriptorSetLayout dsl_decode;
    VkCoreImage decoded_array, filtered_array;
    uint32_t decoded_layer;
    int decode_arrays;
    struct { VkCoreImage image, filtered; int32_t key[5]; uint16_t *source; uint32_t source_words; uint64_t used; int valid; } decoded[64];
    struct { int32_t key[6]; uint32_t entry; } decoded_work[64];
    uint8_t decoded_work_hints[256]; /* one-based indices; 255 is a missing key */
    int32_t decoded_missing_keys[256][6];
    int decode_hints;
    uint32_t decoded_count, decoded_work_count;
    uint64_t decoded_generation;
    uint64_t words_generation, decoded_base_generation;
    int persistent_decode;
    int hardware_aniso;
    VkSampler sampler_aniso[5];
    struct { uint64_t key; VkPipeline pipeline; } materials[256];
    uint32_t material_count;
    int specialize_materials, early_reject, sort_materials, sort_hd_readers, aniso_single_tap, aniso_gather;
    uint32_t submit_draw_limit;
    int clear_spans;
    int interlock;   /* the interlock tier: storage images, batched parameter records */
    int gpu_compute; /* int64 fingerprint; triangle setup additionally requires float64 and interlock */
    VkDescriptorSetLayout dsl_hash;
    VkPipelineLayout pl_hash;
    VkPipeline pipe_hash;
    VkCoreBuffer hash_results;
    /* GPU triangle setup */
    VkDescriptorSetLayout dsl_setup0, dsl_setup1;
    VkPipelineLayout pl_setup;
    VkPipeline pipe_setup;
    uint32_t setup_max_triangles;
    /* the parameter records of the phase templates (device local) */
    VkCoreBuffer tpl_buffer;
    VkBufferView tpl_view;
    VkSampler sampler_clamp, sampler_repeat, sampler_hd;
    VkCommandPool pool;
    VkCommandBuffer *cmds;   /* every command buffer an execution has used so far */
    uint32_t cmd_count, cmd_cap;
    VkFence fence;   /* the one fence of every execution */
    VkCoreImage dummy_color, dummy_uint, words;
    VkCoreBuffer dummy_texels;
    VkBufferView dummy_view, recipe_view;
    VkDescriptorPool *pools;
    uint32_t pool_count, pool_cap, pool_cur;
    NvkChunk *chunks;
    uint32_t chunk_count, chunk_cap, chunk_cur;
    NvkSetEntry *set0;
    uint32_t set0_count;
    uint16_t set0_hints[1024];
    int descriptor_hints;
    NvkSeed *seeds;
    uint32_t seed_count, seed_cap;
    NvkPlane *garbage;
    uint32_t garbage_count, garbage_cap;
    struct NvkHd **hd_garbage;
    uint32_t hd_garbage_count, hd_garbage_cap;
    /* what the GL executor keeps in globals */
    NvkPlane committed[GL_NATIVE_GPU_PHASE_BASE];
    NvkPlane bases[GL_NATIVE_VIEW_TARGET_CAPACITY];
    NvkPlane spare[GL_NATIVE_GPU_PLANES];
    NvkPlane images[NVK_IMAGES];
    NvkPlane work_planes[GL_NATIVE_GPU_PLANES];
    NvkPlane snapshots[GL_NATIVE_GPU_PLANES];
    NvkPlane spare_snapshots[GL_NATIVE_GPU_PLANES];
    uint16_t upload_words[VRAM_W * VRAM_H];
    int upload_valid;   /* the words image equals upload_words */
    int committed_valid;
    const GlNativeGpuWork *pending_work;
    XgPresentationIdentity pending_identity;
    int pending_ok;
    uint64_t layout_generation;
    uint64_t work_sequence;
} S;

/* ---- one execution -------------------------------------------------------------- */
typedef struct NvkBound {
    VkPipeline pipeline;
    VkDescriptorSet set0;
    VkBuffer vbuf;
    VkDeviceSize voff;
    int slot_valid, scissor_valid;
    NvkView v;
    NvkDraw d;
    NvkDrawIL il;
    int32_t scissor[4];
} NvkBound;

typedef struct NvkVertexSource {
    VkBuffer buf;
    VkDeviceSize off;
    const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS];
    uint32_t count;
} NvkVertexSource;

typedef struct NvkCounters {
    uint64_t destination_barriers, skipped_barriers, scheduled_layers, geometry_draws, transfer_draws,
        word_uploads, snapshot_commands, depth_tested_triangles;
} NvkCounters;

static struct {
    const GlNativeGpuWork *work;
    VkCommandBuffer cmd;
    uint32_t cmd_used, since_submit;
    int batchable;   /* the DRAW command in progress is drawn through the interlock batch */
    int recording;
    uint32_t fail_stage, fail_command, fail_kind;
    int32_t fail_code;
    NvkCounters c;
    NvkPlane *pass_plane;
    uint32_t page_y; /* virtual origin of the current viewport */
    NvkBound b;
    NvkView v;
    NvkDraw d;
    NvkSetKey key;
    NvkVertexSource vsrc;
    VkBufferView recipe;
    VkCoreBuffer recipe_buffer;
    int scheduled_run;
    uint8_t reset_word_rows_dummy;
    uint16_t reset_word_rows[VRAM_H];
    NvkPlane *planes;       /* S.work_planes */
    NvkPlane *snapshots;    /* S.snapshots */
    int32_t template_ready;
    NvkVertexSource template_vsrc;
    VkCoreBuffer readback[NVK_IMAGES];
    uint8_t readback_used[NVK_IMAGES];
    struct NvkTemplatePlan *plan;   /* GL work->template_parameters, of this execution */
    int plan_attempted;
    uint32_t hash_groups;   /* groups of the image fingerprint recorded for this work, 0 = none */
    uint32_t submit_query;
    uint8_t scanned_images[NVK_IMAGES];
    int damage_active, damage_region;
    int damage_shader_active;
    VkBuffer damage_buffer;
    uint32_t damage_word_offset;
    uint32_t damage_plane, damage_count;
    int32_t damage_rects[128][4], draw_bounds[4];
    int texture_filter, sprite_filter, anisotropy, filter_strength;
    int capture_active, capture_used, capture_eligible;
    int capture_damage_mask;
    uint32_t capture_order;
    uint64_t capture_reserved;
    VkDescriptorSet capture_set;
    struct { float view[4]; int32_t rect[4]; uint32_t info[4]; } capture_push;
} R;
static int s_retry_phases;

/* Per-phase cost of the executor, in the native_vk_services JSON: CPU phases of nvk_execute and the
 * GPU time between four timestamps (start, after setup, after the journal, end). */
enum { NVK_CPU_PHASES = 6, NVK_GPU_PHASES = 3, NVK_TS = 8, NVK_CAPTURE_TS = 256, NVK_DETAIL_TS = 2048, NVK_SUBMIT_TS = 2048 };
static const char *const s_cpu_phase_names[NVK_CPU_PHASES] = { "begin", "setup", "journal", "scanout", "submit", "complete" };
static const char *const s_gpu_phase_names[NVK_GPU_PHASES] = { "setup", "journal", "scanout_readback" };
static struct {
    uint64_t works, gpu_works;
    uint64_t streamed_works, streamed_chunks;
    uint64_t rendered_phases;
    uint64_t layout_cache_resets, layout_cache_evictions, layout_cache_evicted_bytes;
    uint64_t cleared_spans, cleared_rects;
    uint64_t damage_bounds_pixels, capture_pixels, capture_full_pixels;
    uint64_t core_batches, core_batch_triangles, draw_calls;
    _Atomic uint64_t submissions;
    uint64_t raw_vertex_upload_bytes;
    uint64_t narrow_submits, wide_submits;
    uint64_t batch_draws[3], batch_triangles[3]; /* capture, HD, ordinary guest */
    uint64_t decoded_pages;
    uint64_t decoded_reuses;
    uint64_t decoded_hint_hits, decoded_hint_misses, decoded_negative_hits;
    uint64_t decoded_work_full, decoded_slots_full;
    uint64_t descriptor_hint_hits, descriptor_hint_misses;
    uint64_t aniso_single_tap_triangles;
    uint64_t damage_phases, damage_pixels, damage_full_pixels;
    uint64_t depth_reads_elided;
    uint64_t capture_runs, capture_triangles, capture_retries;
    uint32_t capture_retry_bits;
    int capture_profile;
    uint32_t capture_queries;
    uint64_t capture_gpu_runs, capture_gpu_ns[3];
    uint32_t detail_queries, detail_kind[NVK_DETAIL_TS / 2u];
    uint64_t detail_ns[4], detail_runs[4];
    uint64_t cpu_ns[NVK_CPU_PHASES], cpu_max[NVK_CPU_PHASES];
    uint64_t gpu_ns[NVK_GPU_PHASES], gpu_max[NVK_GPU_PHASES];
    uint64_t base_gpu_ns, phases_gpu_ns;
    VkQueryPool pool;
    VkQueryPool submit_pool;
    uint32_t submit_queries;
    uint64_t timed_submits, submit_gpu_ns, submit_gpu_max_ns, submit_timing_overflow;
    int stamped;
} P;

static void nvk_prof_cpu(unsigned phase, uint64_t ns)
{
    P.cpu_ns[phase] += ns;
    if (ns > P.cpu_max[phase]) P.cpu_max[phase] = ns;
}

static void nvk_prof_stamp(unsigned index)
{
    if (P.pool) vkCmdWriteTimestamp(R.cmd, index ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, P.pool, index);
}

/* Optional bounded GPU scopes. These deliberately serialize timestamps and
 * are diagnostics only; performance runs leave capture profiling disabled. */
static uint32_t nvk_detail_begin(uint32_t kind)
{
    if (!P.pool || !P.capture_profile || !R.recording || P.detail_queries + 2u > NVK_DETAIL_TS) return UINT32_MAX;
    const uint32_t query = P.detail_queries;
    P.detail_queries += 2u;
    P.detail_kind[query / 2u] = kind;
    nvk_prof_stamp(NVK_TS + NVK_CAPTURE_TS + query);
    return query;
}

static void nvk_detail_end(uint32_t query)
{
    if (query != UINT32_MAX) nvk_prof_stamp(NVK_TS + NVK_CAPTURE_TS + query + 1u);
}


/* ---- failures ---------------------------------------------------------------------- */
static int nvk_ok(VkResult result, const char *site)
{
    if (vk_core_check(result, site)) return 1;
    if (!R.fail_code) R.fail_code = (int32_t)result;
    return 0;
}

static void nvk_fail(uint32_t stage, uint32_t command, uint32_t kind)
{
    if (R.fail_stage) return;
    R.fail_stage = stage; R.fail_command = command; R.fail_kind = kind;
}

/* ---- barriers ------------------------------------------------------------------------- */
/* Between any two operations outside a render pass: everything before is
 * visible to everything after. */
static void nvk_barrier_all(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = mb.srcAccessMask;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* glTextureBarrier: the render pass's self-dependency. */
static void nvk_texture_barrier(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT,
                         1, &mb, 0, NULL, 0, NULL);
}

/* ---- upload arena -------------------------------------------------------------------------- */
static uint8_t *nvk_arena(VkDeviceSize size, VkDeviceSize align, uint32_t *chunk, VkDeviceSize *offset)
{
    for (;;) {
        if (S.chunk_cur < S.chunk_count) {
            NvkChunk *c = &S.chunks[S.chunk_cur];
            const VkDeviceSize at = (c->cursor + align - 1u) & ~(align - 1u);
            if (at + size <= c->buf.size) {
                c->cursor = at + size;
                *chunk = S.chunk_cur; *offset = at;
                return (uint8_t *)c->buf.mapped + at;
            }
            ++S.chunk_cur;
            continue;
        }
        if (S.chunk_count == S.chunk_cap) {
            const uint32_t cap = S.chunk_cap ? S.chunk_cap * 2u : 8u;
            NvkChunk *chunks = realloc(S.chunks, cap * sizeof(*chunks));
            if (!chunks) return NULL;
            S.chunks = chunks; S.chunk_cap = cap;
        }
        NvkChunk *c = &S.chunks[S.chunk_count];
        memset(c, 0, sizeof(*c));
        const VkDeviceSize bytes = size + align > NVK_ARENA_CHUNK ? size + align : NVK_ARENA_CHUNK;
        if (!vk_core_buffer_create(&c->buf, bytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_CORE_MEMORY_UPLOAD, "native upload arena")) {
            R.fail_code = R.fail_code ? R.fail_code : (int32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
            return NULL;
        }
        ++S.chunk_count;
    }
}

/* ---- descriptors ---------------------------------------------------------------------------------- */
static VkDescriptorSet nvk_set_alloc(VkDescriptorSetLayout layout)
{
    for (;;) {
        if (S.pool_cur == S.pool_count) {
            if (S.pool_count == S.pool_cap) {
                const uint32_t cap = S.pool_cap ? S.pool_cap * 2u : 8u;
                VkDescriptorPool *pools = realloc(S.pools, cap * sizeof(*pools));
                if (!pools) return VK_NULL_HANDLE;
                S.pools = pools; S.pool_cap = cap;
            }
            static const VkDescriptorPoolSize sizes[] = {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6u * 1024u },
                { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2u * 1024u },
                { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2u * 1024u },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4u * 1024u },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u * 1024u },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u * 1024u },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16u },
            };
            VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            ci.maxSets = 1024u;
            ci.poolSizeCount = 7u;
            ci.pPoolSizes = sizes;
            if (!nvk_ok(vkCreateDescriptorPool(vk_core_device(), &ci, NULL, &S.pools[S.pool_count]),
                        "vkCreateDescriptorPool")) return VK_NULL_HANDLE;
            ++S.pool_count;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = S.pools[S.pool_cur];
        ai.descriptorSetCount = 1u;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(vk_core_device(), &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) { ++S.pool_cur; continue; }
        (void)nvk_ok(r, "vkAllocateDescriptorSets");
        return VK_NULL_HANDLE;
    }
}

/* Set 1 of a chunk: the uniform block ranges of that chunk's buffer. */
static VkDescriptorSet nvk_chunk_set(uint32_t index, int scan)
{
    NvkChunk *c = &S.chunks[index];
    VkDescriptorSet *slot = scan ? &c->scan_set : &c->native_set;
    if (*slot) return *slot;
    VkDescriptorSet set = nvk_set_alloc(scan ? S.dsl_scan1 : S.dsl_native1);
    if (!set) return VK_NULL_HANDLE;
    /* Both tiers share this layout; the interlock tail is eight bytes larger. */
    VkDescriptorBufferInfo info[2] = { { c->buf.buffer, 0, sizeof(NvkView) }, { c->buf.buffer, 0, sizeof(NvkDrawIL) } };
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof(w));
    for (unsigned i = 0u; i < (scan ? 1u : 2u); ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1u;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[i].pBufferInfo = &info[i];
    }
    vkUpdateDescriptorSets(vk_core_device(), scan ? 1u : 2u, w, 0u, NULL);
    *slot = set;
    return set;
}

static VkWriteDescriptorSet nvk_write_image(VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                            const VkDescriptorImageInfo *info)
{
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1u; w.descriptorType = type;
    w.pImageInfo = info;
    return w;
}

/* Hints store indices, never borrowed VkDescriptorSets: pool reset and cache
 * rollover replace the entries. Bounds and the entire key are checked before
 * using an index, including fields omitted from this inexpensive hash. */
static VkDescriptorSet nvk_set0_cached(const NvkSetKey *key, uint32_t *bucket)
{
    *bucket = 0u;
    if (S.descriptor_hints) {
        uint64_t hash = (uint64_t)(uintptr_t)key->dest ^ (uint64_t)(uintptr_t)key->scan_input;
        hash ^= (uint64_t)(uintptr_t)key->hd * UINT64_C(0x9e3779b97f4a7c15);
        hash ^= (uint64_t)(uintptr_t)key->params * UINT64_C(0x85ebca6b);
        hash ^= (uint64_t)(uintptr_t)key->src ^ (uint64_t)(uintptr_t)key->filtered_sampler;
        hash ^= hash >> 33u;
        *bucket = (uint32_t)((hash * UINT64_C(0xff51afd7ed558ccd)) >> 54u);
        const uint32_t hint = S.set0_hints[*bucket];
        if (hint && hint <= S.set0_count && !memcmp(&S.set0[hint - 1u].key, key, sizeof(*key))) {
            ++P.descriptor_hint_hits;
            return S.set0[hint - 1u].set;
        }
        ++P.descriptor_hint_misses;
    }
    for (uint32_t i = S.set0_count; i-- > 0u;)
        if (!memcmp(&S.set0[i].key, key, sizeof(*key))) {
            if (S.descriptor_hints) S.set0_hints[*bucket] = (uint16_t)(i + 1u);
            return S.set0[i].set;
        }
    return VK_NULL_HANDLE;
}

/* Set 0 of the native pipeline for the resources in R.key. */
static VkDescriptorSet nvk_set0(const NvkSetKey *key)
{
    NvkSetKey complete = *key;
    complete.capture_meta = key->capture_meta ? key->capture_meta : S.capture_meta.buffer ? S.capture_meta.buffer : S.dummy_texels.buffer;
    complete.capture_nodes = S.capture_nodes.buffer ? S.capture_nodes.buffer : S.dummy_texels.buffer;
    key = &complete;
    uint32_t bucket;
    const VkDescriptorSet cached = nvk_set0_cached(key, &bucket);
    if (cached) return cached;
    if (S.set0_count == NVK_SET0_CACHE) {
        /* A work with this many distinct resource tuples starts over; the sets
         * it used are not recycled before the pool is reset, so allocate on. */
        S.set0_count = 0u;
    }
    VkDescriptorSet set = nvk_set_alloc(S.dsl_native0);
    if (!set) return VK_NULL_HANDLE;
    const VkImageLayout g = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo words = { S.sampler_clamp, S.words.view, g };
    VkDescriptorImageInfo dest = { VK_NULL_HANDLE, key->dest, g };
    VkDescriptorImageInfo src = { key->src_sampler, key->src, g };
    VkDescriptorImageInfo dest_key = { VK_NULL_HANDLE, key->dest_key, g };
    VkDescriptorImageInfo src_key = { S.sampler_clamp, key->src_key, g };
    VkDescriptorImageInfo hd = { key->hd_sampler, key->hd, g };
    VkDescriptorImageInfo mip = { key->filtered_sampler ? key->filtered_sampler : S.sampler_clamp,
        !S.decode_arrays && key->filtered ? key->filtered : S.dummy_color.view, g };
    VkDescriptorImageInfo filtered = { key->filtered_sampler ? key->filtered_sampler : S.sampler_clamp,
        S.filtered_array.view, g };
    VkWriteDescriptorSet w[14];
    w[0] = nvk_write_image(set, 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &words);
    w[1] = nvk_write_image(set, 1u, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &dest);
    w[2] = nvk_write_image(set, 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src);
    w[3] = nvk_write_image(set, 3u, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &dest_key);
    w[4] = nvk_write_image(set, 4u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_key);
    w[5] = nvk_write_image(set, 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hd);
    w[6] = nvk_write_image(set, 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mip);
    memset(&w[7], 0, sizeof(w[7]));
    w[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[7].dstSet = set; w[7].dstBinding = 8u; w[7].descriptorCount = 1u;
    w[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    w[7].pTexelBufferView = &key->recipe;
    const VkBufferView parameters = key->params ? key->params : S.dummy_view;
    w[8] = w[7]; w[8].dstBinding = 7u; w[8].pTexelBufferView = &parameters;
    VkDescriptorImageInfo decoded = { S.sampler_clamp, !S.decode_arrays && key->decoded ? key->decoded : S.dummy_uint.view, g };
    const VkDescriptorImageInfo decoded_array = { S.sampler_clamp, S.decoded_array.view, g };
    w[9] = nvk_write_image(set, 11u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &decoded);
    const VkDescriptorBufferInfo capture[2] = {
        { key->capture_meta, 0u, VK_WHOLE_SIZE }, { key->capture_nodes, 0u, VK_WHOLE_SIZE }
    };
    for (uint32_t i = 0u; i < 2u; ++i) {
        memset(&w[10u+i], 0, sizeof(w[0]));
        w[10u+i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[10u+i].dstSet = set; w[10u+i].dstBinding = 12u+i; w[10u+i].descriptorCount = 1u;
        w[10u+i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[10u+i].pBufferInfo = &capture[i];
    }
    w[12] = nvk_write_image(set, 14u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &filtered);
    w[13] = nvk_write_image(set, 15u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &decoded_array);
    vkUpdateDescriptorSets(vk_core_device(), 14u, w, 0u, NULL);
    S.set0[S.set0_count].key = *key;
    S.set0[S.set0_count].set = set;
    ++S.set0_count;
    if (S.descriptor_hints) S.set0_hints[bucket] = (uint16_t)S.set0_count;
    return set;
}

/* Set 0 of the interlock pipeline: the same resources, the destination as storage images and the
 * parameter-record texel buffer. */
static VkDescriptorSet nvk_set0_il(const NvkSetKey *key)
{
    uint32_t bucket;
    const VkDescriptorSet cached = nvk_set0_cached(key, &bucket);
    if (cached) return cached;
    if (S.set0_count == NVK_SET0_CACHE) S.set0_count = 0u;
    VkDescriptorSet set = nvk_set_alloc(S.dsl_il0);
    if (!set) return VK_NULL_HANDLE;
    const VkImageLayout g = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo words = { S.sampler_clamp, S.words.view, g };
    VkDescriptorImageInfo src = { key->src_sampler, key->src, g };
    VkDescriptorImageInfo src_key = { S.sampler_clamp, key->src_key, g };
    VkDescriptorImageInfo hd = { key->hd_sampler, key->hd, g };
    VkDescriptorImageInfo mip = { S.sampler_clamp, S.dummy_color.view, g };
    VkDescriptorImageInfo dest = { VK_NULL_HANDLE, key->dest, g };
    VkDescriptorImageInfo dest_key = { VK_NULL_HANDLE, key->dest_key, g };
    VkWriteDescriptorSet w[9];
    w[0] = nvk_write_image(set, 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &words);
    w[1] = nvk_write_image(set, 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src);
    w[2] = nvk_write_image(set, 4u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_key);
    w[3] = nvk_write_image(set, 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hd);
    w[4] = nvk_write_image(set, 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mip);
    w[5] = nvk_write_image(set, 9u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dest);
    w[6] = nvk_write_image(set, 10u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dest_key);
    memset(&w[7], 0, sizeof(w[7]));
    w[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[7].dstSet = set; w[7].dstBinding = 7u; w[7].descriptorCount = 1u;
    w[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    w[7].pTexelBufferView = &key->params;
    w[8] = w[7];
    w[8].dstBinding = 8u;
    w[8].pTexelBufferView = &key->recipe;
    vkUpdateDescriptorSets(vk_core_device(), 9u, w, 0u, NULL);
    S.set0[S.set0_count].key = *key;
    S.set0[S.set0_count].set = set;
    ++S.set0_count;
    if (S.descriptor_hints) S.set0_hints[bucket] = (uint16_t)S.set0_count;
    return set;
}

/* Set 0 of the scanout pipeline: the input plane. */
static VkDescriptorSet nvk_scan_set0(VkImageView input)
{
    NvkSetKey key;
    memset(&key, 0, sizeof(key));
    key.scan_input = input;
    uint32_t bucket;
    const VkDescriptorSet cached = nvk_set0_cached(&key, &bucket);
    if (cached) return cached;
    if (S.set0_count == NVK_SET0_CACHE) S.set0_count = 0u;
    VkDescriptorSet set = nvk_set_alloc(S.dsl_scan0);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorImageInfo info = { S.sampler_clamp, input, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = nvk_write_image(set, 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &info);
    vkUpdateDescriptorSets(vk_core_device(), 1u, &w, 0u, NULL);
    S.set0[S.set0_count].key = key;
    S.set0[S.set0_count].set = set;
    ++S.set0_count;
    if (S.descriptor_hints) S.set0_hints[bucket] = (uint16_t)S.set0_count;
    return set;
}

/* ---- planes ------------------------------------------------------------------------------------------ */
static void nvk_plane_destroy(NvkPlane *p)
{
    if (p->capture_framebuffer) vkDestroyFramebuffer(vk_core_device(), p->capture_framebuffer, NULL);
    if (p->framebuffer) vkDestroyFramebuffer(vk_core_device(), p->framebuffer, NULL);
    if (p->alpha_view) vkDestroyImageView(vk_core_device(), p->alpha_view, NULL);
    vk_core_image_destroy(&p->color);
    vk_core_image_destroy(&p->key);
}

/* GL native_gpu_plane_free. Commands already recorded may still use the
 * images: while recording, destruction waits for the execution to finish. */
static void nvk_plane_free(NvkPlane *p)
{
    free(p->depth_max); p->depth_max = NULL;
    if (p->hazards) {
        free(p->hazards->cell_n); free(p->hazards->cell_index); free(p->hazards->touched);
        free(p->hazards);
    }
    if (p->color.image) {
        if (R.recording) {
            if (S.garbage_count == S.garbage_cap) {
                const uint32_t cap = S.garbage_cap ? S.garbage_cap * 2u : 64u;
                NvkPlane *g = realloc(S.garbage, cap * sizeof(*g));
                if (!g) { R.fail_code = R.fail_code ? R.fail_code : (int32_t)VK_ERROR_OUT_OF_HOST_MEMORY; memset(p, 0, sizeof(*p)); return; }
                S.garbage = g; S.garbage_cap = cap;
            }
            S.garbage[S.garbage_count++] = *p;
        } else nvk_plane_destroy(p);
    }
    memset(p, 0, sizeof(*p));
}

static void nvk_discard_cached_plane(NvkPlane *p)
{
    if (!p->color.image) return;
    ++P.layout_cache_evictions;
    P.layout_cache_evicted_bytes += (uint64_t)p->color.width * p->color.height * (p->scan ? 5u : 8u);
    nvk_plane_free(p);
}

static int nvk_cached_plane_expired(const NvkPlane *p)
{
    /* Keep the recent working set, rather than every peak phase count the
     * session has ever needed. Auxiliary/late frames can briefly need 15
     * phases even when normal play settles back to seven. */
    return p->layout_generation != S.layout_generation ||
        S.work_sequence - p->last_used_work >= 32u;
}

/* Called with s_native_state_mutex held, after execution has completed.
 * Live endpoints keep their images until every presentation fence retires.
 * Their old allocations can then be discarded on a later publication too. */
static void nvk_discard_retired_layouts_locked(void)
{
    for (uint32_t i = 0u; i < GL_NATIVE_ENDPOINT_CAPACITY; ++i) {
        GlNativeEndpointSlot *endpoint = &s_native_endpoints[i];
        if (endpoint->in_use || endpoint->presenter_busy || endpoint->fence_refs) continue;
        for (uint32_t phase = 0u; phase <= GL_NATIVE_MOTION_PHASE_CAPACITY; ++phase) {
            NativeImageSlot *image = phase ? &endpoint->phases[phase - 1u] : &endpoint->image;
            NvkPlane *p = (NvkPlane *)(uintptr_t)image->color;
            if (!p || !nvk_cached_plane_expired(p)) continue;
            nvk_discard_cached_plane(p);
            free(p);
            image->color = image->mask = image->target = 0u;
            image->width = image->height = 0u;
            image->mask_valid = 0;
        }
    }
}

static void nvk_begin_layout(void)
{
    ++S.layout_generation;
    ++P.layout_cache_resets;
    /* A 4K phase target spans the shared VRAM, and can occupy hundreds of
     * MiB. Keeping every unused target from previous aspect layouts can
     * exhaust VRAM even though the newly selected view needs fewer pixels. */
    for (uint32_t i = 0u; i < GL_NATIVE_GPU_PLANES; ++i) {
        nvk_discard_cached_plane(&S.spare[i]);
        nvk_discard_cached_plane(&S.spare_snapshots[i]);
    }
    for (uint32_t i = 0u; i < NVK_IMAGES; ++i) nvk_discard_cached_plane(&S.images[i]);
    SDL_LockMutex(s_native_state_mutex);
    nvk_discard_retired_layouts_locked();
    SDL_UnlockMutex(s_native_state_mutex);
}

static int nvk_plane_create(NvkPlane *p, uint32_t width, uint32_t height, int scan)
{
    memset(p, 0, sizeof(*p));
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (scan ? 0u : S.interlock ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) |
        (!scan && S.capture_enabled ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    if (!width || !height || width > vk_core_caps()->max_image_dimension_2d ||
        height > vk_core_caps()->max_image_dimension_2d) return 0;
    if (!vk_core_image_create(&p->color, VK_FORMAT_R8G8B8A8_UNORM, width, height, 1u, use, "native color") ||
        !vk_core_image_create(&p->key, scan ? VK_FORMAT_R8_UNORM : VK_FORMAT_R32_UINT, width, height, 1u, use,
                              scan ? "native mask" : "native key")) {
        nvk_plane_destroy(p); memset(p, 0, sizeof(*p));
        R.fail_code = R.fail_code ? R.fail_code : (int32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return 0;
    }
    p->width = width; p->height = height; p->scan = scan;
    p->layout_generation = S.layout_generation;
    p->last_used_work = S.work_sequence;
    p->raster[0] = p->raster[1] = 1.f;
    if (scan) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = p->key.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8_UNORM;
        vi.components.r = vi.components.g = vi.components.b = VK_COMPONENT_SWIZZLE_ZERO;
        vi.components.a = VK_COMPONENT_SWIZZLE_R;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        if (!nvk_ok(vkCreateImageView(vk_core_device(), &vi, NULL, &p->alpha_view), "vkCreateImageView")) {
            nvk_plane_destroy(p); memset(p, 0, sizeof(*p));
            return 0;
        }
    }
    VkImageView views[2] = { p->color.view, p->key.view };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = scan ? S.pass_scan_load : S.interlock ? S.pass_il : S.pass_draw;
    /* The interlock pass has no attachments: the framebuffer only gives the draw its size. */
    fi.attachmentCount = !scan && S.interlock ? 0u : 2u;
    fi.pAttachments = views;
    fi.width = width; fi.height = height; fi.layers = 1u;
    if (!nvk_ok(vkCreateFramebuffer(vk_core_device(), &fi, NULL, &p->framebuffer), "vkCreateFramebuffer")) {
        nvk_plane_destroy(p); memset(p, 0, sizeof(*p));
        return 0;
    }
    if (!scan && S.capture_enabled) {
        fi.renderPass = S.pass_capture; fi.attachmentCount = 0u; fi.pAttachments = NULL;
        if (!nvk_ok(vkCreateFramebuffer(vk_core_device(), &fi, NULL, &p->capture_framebuffer), "capture framebuffer")) {
            nvk_plane_destroy(p); memset(p, 0, sizeof(*p)); return 0;
        }
    }
    /* Defined contents: GL leaves a new texture to the driver, which zeroes it. */
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
    VkClearColorValue zero;
    memset(&zero, 0, sizeof(zero));
    vk_core_image_barrier(R.cmd, p->color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vk_core_image_barrier(R.cmd, p->key.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdClearColorImage(R.cmd, p->color.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1u, &range);
    vkCmdClearColorImage(R.cmd, p->key.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1u, &range);
    p->depth_zero = 1;
    nvk_barrier_all();
    return 1;
}

static void nvk_pass_end(void);

/* GL native_gpu_plane_size / native_gpu_mask_size. */
static int nvk_plane_size(NvkPlane *p, uint32_t width, uint32_t height, int scan)
{
    p->last_used_work = S.work_sequence;
    /* A CPU-uploaded endpoint image has no framebuffer: it cannot be rendered again. */
    if (p->color.image && p->framebuffer && p->width == width && p->height == height &&
        p->color.width == width && p->color.height == height && !p->page_rows &&
        p->raster[0] == 1.f && p->raster[1] == 1.f && p->raster[2] == 0.f && p->raster[3] == 0.f) return 1;
    nvk_pass_end();
    nvk_plane_free(p);
    return nvk_plane_create(p, width, height, scan);
}

/* Allocate a host-grid plane without changing any guest/journal coordinates.
 * The viewport can be fractional; the backing allocation conservatively holds it. */
static int nvk_plane_size_mapped(NvkPlane *p, uint32_t width, uint32_t height,
        float rx, float ry, uint32_t page_rows, uint32_t page_pixels) {
    p->last_used_work = S.work_sequence;
    const uint32_t physical_width = (uint32_t)ceil((double)width * rx);
    const uint32_t physical_height = page_rows
        ? (height / page_rows) * page_pixels + (uint32_t)ceil((double)(height % page_rows) * ry)
        : (uint32_t)ceil((double)height * ry);
    const float raster[4] = {1.f / rx, 1.f / ry, 0.f, 0.f};
    if (p->color.image && p->framebuffer && p->width == width && p->height == height &&
        p->color.width == physical_width && p->color.height == physical_height &&
        p->page_rows == page_rows && p->page_pixels == page_pixels &&
        !memcmp(p->raster, raster, sizeof(raster))) return 1;
    nvk_pass_end();
    nvk_plane_free(p);
    if (!nvk_plane_create(p, physical_width, physical_height, 0)) return 0;
    p->width = width; p->height = height;
    p->page_rows = page_rows; p->page_pixels = page_pixels;
    memcpy(p->raster, raster, sizeof(raster));
    return 1;
}

static int nvk_work_plane_size(NvkPlane *p, unsigned index, uint32_t width, uint32_t height) {
    const GlNativeGpuWork *work = R.work;
    if (!work->render_width || !index) return nvk_plane_size(p, width, height, 0);
    return nvk_plane_size_mapped(p, width, height, work->raster_scale[0], work->raster_scale[1],
        work->raster_page_rows * work->scale, work->raster_page_pixels);
}

static int nvk_clone_plane_size(NvkPlane *p, const NvkPlane *source) {
    p->last_used_work = S.work_sequence;
    if (p->color.image && p->framebuffer && p->width == source->width && p->height == source->height &&
        p->color.width == source->color.width && p->color.height == source->color.height &&
        p->page_rows == source->page_rows && p->page_pixels == source->page_pixels &&
        !memcmp(p->raster, source->raster, sizeof(p->raster))) return 1;
    nvk_pass_end();
    nvk_plane_free(p);
    if (!nvk_plane_create(p, source->color.width, source->color.height, 0)) return 0;
    p->width = source->width; p->height = source->height;
    p->page_rows = source->page_rows; p->page_pixels = source->page_pixels;
    memcpy(p->raster, source->raster, sizeof(p->raster));
    return 1;
}

static void nvk_source_raster(const NvkPlane *source) {
    const float raster[4] = { source ? source->width / source->raster[0] / source->color.width : 1.f,
        source ? source->height / source->raster[1] / source->color.height : 1.f,
        source ? source->raster[2] / source->color.width : 0.f,
        source ? source->raster[3] / source->color.height : 0.f };
    memcpy(R.v.source_raster, raster, sizeof(raster));
    R.v.raster_pages[2] = source && source->page_rows ? (float)source->height / source->page_rows : 0.f;
    R.v.raster_pages[3] = source && source->page_rows
        ? ((float)source->page_pixels - source->page_rows / source->raster[1]) / source->color.height : 0.f;
}

static int32_t nvk_raster_edge(const NvkPlane *plane, int32_t value, unsigned axis) {
    const uint32_t origin = axis && plane->page_rows ? R.page_y : 0u;
    const uint32_t pixel = origin ? (origin / plane->page_rows) * plane->page_pixels : 0u;
    return (int32_t)ceil(((double)value - origin) / plane->raster[axis] + pixel - 0.5);
}

/* GL native_gpu_plane_depth: attach (once) and clear-to-far the key. */
static int nvk_plane_depth(NvkPlane *p)
{
    if (p->has_key) return 1;
    if (!p->color.image) return 0;
    nvk_pass_end();
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
    VkClearColorValue zero;
    memset(&zero, 0, sizeof(zero));
    vkCmdClearColorImage(R.cmd, p->key.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1u, &range);
    nvk_barrier_all();
    p->has_key = 1;
    p->depth_zero = 1;
    if (p->depth_max) memset(p->depth_max, 0, (size_t)p->depth_columns * p->depth_rows * sizeof(*p->depth_max));
    p->dirty_all = 1;
    return 1;
}

static void nvk_plane_barrier_clear(NvkPlane *p)
{
    p->dirty_all = 0;
    if (p->hazards) native_gpu_hazards_clear(p->hazards);
}

/* GL native_gpu_blit / native_gpu_blit_depth for a whole plane. */
static void nvk_plane_copy_impl(const NvkPlane *source, NvkPlane *destination, int depth);

static void nvk_plane_copy(const NvkPlane *source, NvkPlane *destination, int depth)
{
    const uint32_t query = nvk_detail_begin(0u);
    nvk_plane_copy_impl(source, destination, depth);
    nvk_detail_end(query);
}

static void nvk_plane_copy_impl(const NvkPlane *source, NvkPlane *destination, int depth)
{
    if (depth) {
        free(destination->depth_max); destination->depth_max = NULL;
        destination->depth_zero = source->depth_zero;
        destination->depth_columns = source->depth_columns; destination->depth_rows = source->depth_rows;
        destination->depth_scale = source->depth_scale;
        if (source->depth_max) {
            const size_t bytes = (size_t)source->depth_columns * source->depth_rows * sizeof(*source->depth_max);
            destination->depth_max = malloc(bytes);
            if (destination->depth_max) memcpy(destination->depth_max, source->depth_max, bytes);
        }
    }
    nvk_pass_end();
    VkImageCopy region;
    memset(&region, 0, sizeof(region));
    region.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    region.dstSubresource = region.srcSubresource;
    region.extent = (VkExtent3D){ destination->color.width, destination->color.height, 1u };
    vkCmdCopyImage(R.cmd, depth ? source->key.image : source->color.image, VK_IMAGE_LAYOUT_GENERAL,
                   depth ? destination->key.image : destination->color.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    nvk_barrier_all();
    destination->dirty_all = 1;
}

/* ---- command buffers -------------------------------------------------------------------------------------------- */
/* A work records into several command buffers: the first part is submitted while the CPU records the rest,
 * so the GPU starts before the journal is fully recorded. They are reset when the next work starts. */
enum { NVK_SUBMIT_DRAWS = 2048, NVK_SUBMIT_DRAWS_IL = 1024 };

static int nvk_require_vertices(void);

static int nvk_cmd_begin(void)
{
    if (R.cmd_used == S.cmd_count) {
        if (S.cmd_count == S.cmd_cap) {
            const uint32_t cap = S.cmd_cap ? S.cmd_cap * 2u : 8u;
            VkCommandBuffer *cmds = realloc(S.cmds, cap * sizeof(*cmds));
            if (!cmds) return 0;
            S.cmds = cmds; S.cmd_cap = cap;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = S.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1u;
        if (!nvk_ok(vkAllocateCommandBuffers(vk_core_device(), &ai, &S.cmds[S.cmd_count]), "vkAllocateCommandBuffers")) return 0;
        ++S.cmd_count;
    }
    R.cmd = S.cmds[R.cmd_used++];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!nvk_ok(vkResetCommandBuffer(R.cmd, 0), "vkResetCommandBuffer") ||
        !nvk_ok(vkBeginCommandBuffer(R.cmd, &bi), "vkBeginCommandBuffer")) return 0;
    R.submit_query = UINT32_MAX;
    if (P.submit_pool) {
        if (R.cmd_used == 1u) {
            P.submit_queries = 0u;
            vkCmdResetQueryPool(R.cmd, P.submit_pool, 0u, NVK_SUBMIT_TS);
        }
        if (P.submit_queries + 2u <= NVK_SUBMIT_TS) {
            R.submit_query = P.submit_queries;
            P.submit_queries += 2u;
            vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, P.submit_pool, R.submit_query);
        } else ++P.submit_timing_overflow;
    }
    return 1;
}

/* Preserve executor FIFO order while letting its CPU record ahead of the
 * bounded GPU queue. Only the final submit (or a full CPU ring) must wait.
 * All command buffers/resources still belong to this one work until its
 * final fence completes; presentation can use already completed endpoints
 * between these submissions. */
static uint32_t nvk_pace_window(void)
{
    if (!S.pace_margin_ns || S.pace_limit < 2u || !R.work) return S.pace_limit;
    const uint64_t deadline = R.work->timing.deadline_ns;
    const uint64_t now = SDL_GetTicksNS();
    /* Keep space for presentation while this work is comfortably early.
     * Restore the configured overlap when its existing deadline approaches;
     * never move that deadline or discard authored phases to meet the target. */
    return deadline > now && deadline - now > S.pace_margin_ns ? 1u : S.pace_limit;
}

/* Optional submit-only worker. Recording and resource allocation stay on the
 * executor thread. A fixed GPU window is replenished as soon as its oldest
 * fence completes, even while that executor is recording the next phase. */
static struct {
    SDL_mutex *mutex;
    SDL_cond *condition;
    SDL_Thread *thread;
    struct { VkCommandBuffer cmd; VkFence fence; } entries[64];
    uint32_t head, count;
    int run;
    VkResult result;
} SQ;

static int nvk_submit_thread(void *unused)
{
    (void)unused;
    SDL_LockMutex(SQ.mutex);
    while (SQ.run) {
        while (SQ.run && (!SQ.count || SQ.result != VK_SUCCESS))
            SDL_CondWait(SQ.condition, SQ.mutex);
        if (!SQ.run) break;
        const VkCommandBuffer cmd = SQ.entries[SQ.head].cmd;
        VkFence fence = SQ.entries[SQ.head].fence;
        SDL_UnlockMutex(SQ.mutex);
        VkResult result = VK_SUCCESS;
        while (S.pace_count >= S.pace_limit) {
            VkFence oldest = S.pace_fences[S.pace_head];
            result = vkWaitForFences(vk_core_device(), 1u, &oldest, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS) break;
            S.pace_head = (S.pace_head + 1u) % S.pace_limit;
            --S.pace_count;
        }
        const int paced = fence == VK_NULL_HANDLE;
        if (result == VK_SUCCESS && paced) {
            fence = S.pace_fences[(S.pace_head + S.pace_count) % S.pace_limit];
            result = vkResetFences(vk_core_device(), 1u, &fence);
        }
        if (result == VK_SUCCESS) {
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1u; si.pCommandBuffers = &cmd;
            result = vk_core_submit(1u, &si, fence);
            if (result == VK_SUCCESS) {
                S.pace_count += paced;
                ++P.submissions;
            }
        }
        SDL_LockMutex(SQ.mutex);
        SQ.result = result;
        SQ.head = (SQ.head + 1u) % 64u;
        --SQ.count;
        if (result != VK_SUCCESS) SQ.count = 0u;
        SDL_CondBroadcast(SQ.condition);
    }
    SDL_UnlockMutex(SQ.mutex);
    return 0;
}

static int nvk_submit_thread_sync(void)
{
    if (!SQ.thread) return 1;
    SDL_LockMutex(SQ.mutex);
    while (SQ.count && SQ.result == VK_SUCCESS)
        SDL_CondWait(SQ.condition, SQ.mutex);
    const VkResult result = SQ.result;
    SDL_UnlockMutex(SQ.mutex);
    return nvk_ok(result, "native submit worker");
}

static int nvk_submit_thread_enqueue(VkCommandBuffer cmd, VkFence fence)
{
    SDL_LockMutex(SQ.mutex);
    while (SQ.count == 64u && SQ.result == VK_SUCCESS)
        SDL_CondWait(SQ.condition, SQ.mutex);
    const VkResult result = SQ.result;
    if (result == VK_SUCCESS) {
        const uint32_t tail = (SQ.head + SQ.count) % 64u;
        SQ.entries[tail].cmd = cmd; SQ.entries[tail].fence = fence;
        ++SQ.count;
        SDL_CondBroadcast(SQ.condition);
    }
    SDL_UnlockMutex(SQ.mutex);
    /* A returned final fence must already have been submitted. All its
     * predecessors retain their buffers/images until that fence completes. */
    return nvk_ok(result, "native submit enqueue") && (!fence || nvk_submit_thread_sync());
}

static void nvk_submit_thread_stop(void)
{
    if (SQ.thread) {
        (void)nvk_submit_thread_sync();
        SDL_LockMutex(SQ.mutex);
        SQ.run = 0;
        SDL_CondBroadcast(SQ.condition);
        SDL_UnlockMutex(SQ.mutex);
        SDL_WaitThread(SQ.thread, NULL);
    }
    if (SQ.condition) SDL_DestroyCond(SQ.condition);
    if (SQ.mutex) SDL_DestroyMutex(SQ.mutex);
    memset(&SQ, 0, sizeof(SQ));
}

static int nvk_pending_submit_drain(int blocking)
{
    while (S.pending_count) {
        while (S.pace_count >= nvk_pace_window()) {
            VkFence oldest = S.pace_fences[S.pace_head];
            VkResult status = vkGetFenceStatus(vk_core_device(), oldest);
            if (status == VK_NOT_READY) {
                if (!blocking) return 1;
                status = vkWaitForFences(vk_core_device(), 1u, &oldest, VK_TRUE, UINT64_MAX);
            }
            if (!nvk_ok(status, "deferred native queue pace")) return 0;
            S.pace_head = (S.pace_head + 1u) % S.pace_limit;
            --S.pace_count;
        }
        const uint32_t head = S.pending_head;
        VkFence fence = S.pending_submits[head].fence;
        const int paced = fence == VK_NULL_HANDLE;
        if (paced) {
            fence = S.pace_fences[(S.pace_head + S.pace_count) % S.pace_limit];
            if (!nvk_ok(vkResetFences(vk_core_device(), 1u, &fence), "deferred native pace reset")) return 0;
        }
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1u; si.pCommandBuffers = &S.pending_submits[head].cmd;
        if (!nvk_ok(vk_core_submit(1u, &si, fence), "deferred native submit")) return 0;
        S.pace_count += paced;
        if (S.pace_margin_ns) {
            if (S.pace_count <= 1u) ++P.narrow_submits;
            else ++P.wide_submits;
        }
        ++P.submissions;
        S.pending_head = (head + 1u) % 64u;
        --S.pending_count;
    }
    return 1;
}

/* Ends and submits the recording so far (outside a render pass) and opens the next command buffer. */
static int nvk_submit_partial(VkFence fence)
{
    nvk_pass_end();
    if (R.submit_query != UINT32_MAX)
        vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, P.submit_pool, R.submit_query + 1u);
    if (!nvk_ok(vkEndCommandBuffer(R.cmd), "vkEndCommandBuffer")) return 0;
    for (uint32_t i = 0u; i < S.chunk_count && S.chunks[i].cursor; ++i)
        vk_core_buffer_flush(&S.chunks[i].buf, 0, S.chunks[i].cursor);
    if (SQ.thread) {
        if (!nvk_submit_thread_enqueue(R.cmd, fence)) return 0;
        R.since_submit = 0u;
        return fence ? 1 : nvk_cmd_begin();
    }
    if (S.defer_submits) {
        if (S.pending_count == 64u && !nvk_pending_submit_drain(1)) return 0;
        const uint32_t tail = (S.pending_head + S.pending_count) % 64u;
        S.pending_submits[tail].cmd = R.cmd;
        S.pending_submits[tail].fence = fence;
        ++S.pending_count;
        if (!nvk_pending_submit_drain(fence != VK_NULL_HANDLE)) return 0;
        R.since_submit = 0u;
        return fence ? 1 : nvk_cmd_begin();
    }
    /* Bound queued executor batches while allowing CPU recording and GPU
     * execution to overlap. Waiting on every batch starves the phase compiler;
     * a small ring allows presentation to interleave without that serialization. */
    while (S.pace_limit && S.pace_count >= nvk_pace_window()) {
        VkFence oldest = S.pace_fences[S.pace_head];
        if (!nvk_ok(vkWaitForFences(vk_core_device(), 1u, &oldest, VK_TRUE, UINT64_MAX), "native queue pace")) return 0;
        S.pace_head = (S.pace_head + 1u) % S.pace_limit;
        --S.pace_count;
    }
    VkFence submitted = fence;
    const int paced = !submitted && S.pace_limit;
    if (paced) {
        submitted = S.pace_fences[(S.pace_head + S.pace_count) % S.pace_limit];
        if (!nvk_ok(vkResetFences(vk_core_device(), 1u, &submitted), "native pace fence reset")) return 0;
    }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1u; si.pCommandBuffers = &R.cmd;
    if (!nvk_ok(vk_core_submit(1u, &si, submitted), "vk_core_submit")) return 0;
    if (paced) ++S.pace_count;
    if (S.pace_margin_ns) {
        if (S.pace_count <= 1u) ++P.narrow_submits;
        else ++P.wide_submits;
    }
    ++P.submissions;
    R.since_submit = 0u;
    return fence ? 1 : nvk_cmd_begin();
}

/* ---- uniform state --------------------------------------------------------------------------------------- */
/* The GL values persist across draws and works (loose uniforms of one program);
 * so do these. Groups are the GL NATIVE_UNIFORM_* / uniform4 group numbers. */
static void nvk_set4(int32_t *dst, int a, int b, int c, int d)
{
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
}

static void nvk_setf4(float *dst, float a, float b, float c, float d)
{
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
}

static void nvk_attribute_uniforms(const GlNativeAttributePlanes *attributes)
{
    if (attributes->integral) {
        for (unsigned a = 0u; a < 5u; ++a) {
            for (unsigned c = 0u; c < 3u; ++c) R.d.attribute_dda[a][c] = (int32_t)attributes->dda[a][c];
            R.d.attribute_dda[a][3] = 0;
        }
    } else {
        for (unsigned a = 0u; a < 5u; ++a) {
            for (unsigned c = 0u; c < 3u; ++c) R.d.attribute_plane[a][c] = (float)attributes->plane[a][c];
            R.d.attribute_plane[a][3] = 0.f;
        }
    }
}

/* Depth uniforms for one GPU draw/transfer (GL native_gpu_depth_uniforms).
 * Guest VRAM (plane 0) never receives a presentation-only depth view. */
static unsigned nvk_depth_uniforms(const NvkPlane *plane, const GlNativeDepthMode *mode, int view_plane)
{
    const GlNativeGpuWork *work = R.work;
    nvk_set4(R.d.depth_view, view_plane ? work->depth_view : GL_NATIVE_DEPTH_VIEW_OFF,
             mode->show, work->depth_view_log[0], work->depth_view_log[1]);
    if (!work->depth_test || !plane->has_key || !view_plane) {
        nvk_set4(R.d.depth_state, 0, 0, 0, 0);
        return 0u;
    }
    nvk_set4(R.d.depth_state, mode->test, mode->write, mode->semi, mode->bias);
    if (mode->test) {
        nvk_set4(R.d.depth_plane, native_depth_encode(mode->plane.n0), native_depth_encode(mode->plane.a / (float)work->scale),
            native_depth_encode(mode->plane.b / (float)work->scale), (int)work->scale);
        R.d.depth_origin[0] = mode->plane.ox * (int)work->scale;
        R.d.depth_origin[1] = mode->plane.oy * (int)work->scale;
    }
    return NVK_UNIFORM_DEPTH_ATTACHED | (mode->test || mode->semi ? NVK_UNIFORM_DEPTH_READ : 0u);
}

/* ---- render pass control --------------------------------------------------------------------------------- */
static int nvk_il_flush(void);
static int nvk_core_batch_flush(void);

static void nvk_pass_end(void)
{
    if (!R.pass_plane) return;
    if (!nvk_core_batch_flush()) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_COMMAND, 0u, 0xFEu);
    if (!nvk_il_flush()) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_COMMAND, 0u, 0xFFu);
    vkCmdEndRenderPass(R.cmd);
    R.pass_plane = NULL;
    /* Draws only write fragment storage and colour attachments. Their next
     * consumers are fragment/compute/transfer operations, never vertex
     * fetch. Keep those dependencies while allowing subsequent vertices to
     * run ahead of the preceding colour/fragment tail. ALL_GRAPHICS also
     * protects earlier vertex reads against later buffer overwrites. */
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | (R.capture_active ? 0u : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(R.cmd, R.capture_active ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 1u, &barrier, 0u, NULL, 0u, NULL);
}

static void nvk_pass_begin(NvkPlane *plane)
{
    if (R.pass_plane == plane) {
        memcpy(R.v.raster, plane->raster, sizeof(R.v.raster));
        R.v.raster[3] = plane->page_rows ? (float)(R.page_y / plane->page_rows * plane->page_pixels) : 0.f;
        R.v.raster_pages[0] = (float)R.page_y;
        R.v.raster_pages[1] = plane->page_rows ? 1.f : 0.f;
        return;
    }
    nvk_pass_end();
    R.page_y = 0u;
    memcpy(R.v.raster, plane->raster, sizeof(R.v.raster));
    R.v.raster_pages[0] = 0.f;
    R.v.raster_pages[1] = plane->page_rows ? 1.f : 0.f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = R.capture_active ? S.pass_capture : S.interlock ? S.pass_il : S.pass_draw;
    bi.framebuffer = R.capture_active ? plane->capture_framebuffer : plane->framebuffer;
    bi.renderArea.extent = (VkExtent2D){ plane->color.width, plane->color.height };
    vkCmdBeginRenderPass(R.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport = { plane->raster[2], plane->raster[3],
        plane->width / plane->raster[0], plane->height / plane->raster[1], 0.f, 1.f };
    vkCmdSetViewport(R.cmd, 0u, 1u, &viewport);
    memset(&R.b, 0, sizeof(R.b));
    R.pass_plane = plane;
}

/* Draw and sample each framebuffer in its own pixel grid. The allocation
 * and residency stay fixed while these per-draw viewport origins change. */
static int nvk_raster_page(NvkPlane *plane, int y)
{
    nvk_pass_begin(plane);
    const uint32_t origin = plane->page_rows && y > 0
        ? (uint32_t)y / plane->page_rows * plane->page_rows : 0u;
    if (origin != R.page_y) {
        if (!nvk_core_batch_flush() || !nvk_il_flush()) return 0;
        R.page_y = origin;
        const float pixel = plane->page_rows ? (float)(origin / plane->page_rows * plane->page_pixels) : 0.f;
        const VkViewport viewport = {0.f, pixel, plane->width / plane->raster[0], plane->height / plane->raster[1], 0.f, 1.f};
        vkCmdSetViewport(R.cmd, 0u, 1u, &viewport);
    }
    R.v.raster_pages[0] = (float)origin;
    R.v.raster[3] = plane->page_rows ? (float)(origin / plane->page_rows * plane->page_pixels) : 0.f;
    return 1;
}

static void nvk_scissor(const NvkPlane *plane, const int32_t s[4])
{
    int32_t x0 = s[0] < 0 ? 0 : s[0], y0 = s[1] < 0 ? 0 : s[1];
    int32_t x1 = s[0] + s[2], y1 = s[1] + s[3];
    if (x1 > (int32_t)plane->width) x1 = (int32_t)plane->width;
    if (y1 > (int32_t)plane->height) y1 = (int32_t)plane->height;
    x0 = nvk_raster_edge(plane, x0, 0u); y0 = nvk_raster_edge(plane, y0, 1u);
    x1 = nvk_raster_edge(plane, x1, 0u); y1 = nvk_raster_edge(plane, y1, 1u);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)plane->color.width) x1 = plane->color.width;
    if (y1 > (int32_t)plane->color.height) y1 = plane->color.height;
    const int32_t value[4] = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    if (R.b.scissor_valid && !memcmp(R.b.scissor, value, sizeof(value))) return;
    const VkRect2D rect = { { value[0], value[1] }, { (uint32_t)value[2], (uint32_t)value[3] } };
    vkCmdSetScissor(R.cmd, 0u, 1u, &rect);
    memcpy(R.b.scissor, value, sizeof(value));
    R.b.scissor_valid = 1;
}


/* ---- interlock tier: batched triangles ---------------------------------------------------------------------------------------- */
/* GL native_gpu_batch_*: the per-triangle parameters of consecutive draws are packed into records of a texel
 * buffer (14 texels per triangle, then 6 per shared record) and drawn together. */
enum { NVK_IL_CAPACITY = 1024, NVK_IL_TRIANGLE_TEXELS = 14, NVK_IL_SHARED_TEXELS = 6 };
/* While a template plan is built, the packed records of the triangles go here instead of the batch. */
static struct {
    int active;
    uint32_t (*texels)[4];
    uint32_t triangles, bias;
    uint8_t *visible;
} TC;
static void nvk_template_plan_free(void);
static struct {
    uint32_t texels[NVK_IL_CAPACITY * NVK_IL_TRIANGLE_TEXELS][4];
    uint32_t shared[NVK_IL_CAPACITY][NVK_IL_SHARED_TEXELS][4];
    uint32_t first[NVK_IL_CAPACITY], count[NVK_IL_CAPACITY];
    uint32_t base, parameter_count, shared_count, draw_count;
    NvkPlane *plane;
    NvkView view;
    NvkSetKey key;
    VkBuffer vbuf;
    VkDeviceSize voff;
    int32_t scissor[4];
} IB;

/* The record of the triangle being drawn, from the uniform state (GL native_gpu_pack_parameters). */
static void nvk_il_pack(uint32_t record[NVK_IL_TRIANGLE_TEXELS][4], uint32_t shared[NVK_IL_SHARED_TEXELS][4])
{
    memcpy(record[0], R.d.state, 16u);
    memcpy(record[1], R.d.page, 16u);
    memcpy(record[2], R.d.depth_state, 16u);
    memcpy(record[3], R.d.sample_lim, 16u);
    memcpy(record[4], R.d.depth_plane, 16u);
    memcpy(record[5], R.d.attribute_origin, 16u);
    memcpy(record[6], R.d.uv_gradient, 16u);
    record[7][0] = (uint32_t)R.d.aa_exempt; record[7][1] = (uint32_t)R.d.texture_filter;
    record[7][2] = (uint32_t)R.d.anisotropy; record[7][3] = (uint32_t)R.d.mip_on;
    record[8][0] = (uint32_t)R.d.u_depth; record[8][1] = (uint32_t)R.d.depth_origin[0];
    record[8][2] = (uint32_t)R.d.depth_origin[1]; record[8][3] = 0u;
    /* Integral attributes use their DDA entries, the others their planes. */
    if (R.d.attribute_origin[2] == 1.f) memcpy(record[9], R.d.attribute_dda, 5u * 16u);
    else memcpy(record[9], R.d.attribute_plane, 5u * 16u);
    memcpy(shared[0], R.d.flags, 16u);
    memcpy(shared[1], R.d.window, 16u);
    memcpy(shared[2], R.d.depth_view, 16u);
    memcpy(shared[3], R.v.size, 16u);
    memcpy(shared[4], R.d.constant_color, 16u);
    shared[5][0] = (uint32_t)R.d.u_depth; shared[5][1] = (uint32_t)R.d.origin[0]; shared[5][2] = (uint32_t)R.d.origin[1];
    memcpy(&shared[5][3], &R.d.filter_strength, 4u);
}

static VkBufferView nvk_chunk_params_view(uint32_t index)
{
    NvkChunk *c = &S.chunks[index];
    if (c->params_view) return c->params_view;
    VkBufferViewCreateInfo bvi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
    bvi.buffer = c->buf.buffer; bvi.format = VK_FORMAT_R32G32B32A32_UINT;
    bvi.range = c->buf.size < NVK_ARENA_CHUNK ? c->buf.size & ~(VkDeviceSize)15u : NVK_ARENA_CHUNK;
    if (!nvk_ok(vkCreateBufferView(vk_core_device(), &bvi, NULL, &c->params_view), "vkCreateBufferView")) c->params_view = VK_NULL_HANDLE;
    return c->params_view;
}

/* Binds the pipeline, resources, uniform slot and vertex buffer of an interlock draw. */
static int nvk_il_bind(const NvkDrawIL *il, const float size[4], const NvkSetKey *key, const NvkView *view_state)
{
    if (R.b.pipeline != S.pipe_il) {
        vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe_il);
        R.b.pipeline = S.pipe_il;
    }
    VkDescriptorSet set0 = nvk_set0_il(key);
    if (!set0) return 0;
    if (R.b.set0 != set0) {
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_il, 0u, 1u, &set0, 0u, NULL);
        R.b.set0 = set0;
    }
    NvkView view = *view_state;
    memcpy(view.size, size, sizeof(view.size));
    if (!R.b.slot_valid || memcmp(&view, &R.b.v, sizeof(view)) || memcmp(il, &R.b.il, sizeof(*il))) {
        uint32_t chunk;
        VkDeviceSize offset;
        uint8_t *slot = nvk_arena(NVK_SLOT, NVK_SLOT_ALIGN, &chunk, &offset);
        if (!slot) return 0;
        /* Only the two declared UBO ranges are consumed. Do not clear their
         * 256-byte alignment gaps in write-combined upload memory per draw. */
        memcpy(slot, &view, sizeof(view));
        memcpy(slot + NVK_SLOT_ALIGN, il, sizeof(*il));
        VkDescriptorSet set1 = nvk_chunk_set(chunk, 0);
        if (!set1) return 0;
        const uint32_t offsets[2] = { (uint32_t)offset, (uint32_t)offset + NVK_SLOT_ALIGN };
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_il, 1u, 1u, &set1, 2u, offsets);
        R.b.v = view; R.b.il = *il; R.b.slot_valid = 1;
    }
    return 1;
}

/* Draws the pending batch of triangles. */
static int nvk_il_flush(void)
{
    if (!IB.draw_count) return 1;
    const uint32_t draw_count = IB.draw_count;
    IB.draw_count = 0u;
    const uint32_t triangle_texels = IB.parameter_count * NVK_IL_TRIANGLE_TEXELS;
    const uint32_t shared_texels = IB.shared_count * NVK_IL_SHARED_TEXELS;
    const VkDeviceSize bytes = (VkDeviceSize)(triangle_texels + shared_texels) * 16u;
    /* The shader finds a record at (index - parameter_base) * 14 texels: start the records at a multiple of 14. */
    enum { RECORD_BYTES = NVK_IL_TRIANGLE_TEXELS * 16 };
    uint32_t chunk;
    VkDeviceSize offset;
    uint8_t *at = nvk_arena(bytes + RECORD_BYTES, 16u, &chunk, &offset);
    if (!at) return 0;
    const VkDeviceSize skew = (RECORD_BYTES - offset % RECORD_BYTES) % RECORD_BYTES;
    at += skew; offset += skew;
    VkBufferView view = nvk_chunk_params_view(chunk);
    if (!view || offset + bytes > S.chunks[chunk].buf.size || offset + bytes > NVK_ARENA_CHUNK) return 0;
    memcpy(at, IB.texels, (size_t)triangle_texels * 16u);
    memcpy(at + (size_t)triangle_texels * 16u, IB.shared, (size_t)shared_texels * 16u);
    NvkSetKey key = IB.key;
    key.params = view;
    NvkDrawIL il;
    memset(&il, 0, sizeof(il));
    il.parameters_on = 1;
    il.parameter_base = (int32_t)IB.base - (int32_t)(offset / RECORD_BYTES);
    il.parameter_shared = (int32_t)(offset / 16u + triangle_texels);
    if (!nvk_il_bind(&il, (const float *)(const void *)IB.shared[0][3], &key, &IB.view)) return 0;
    if (R.b.vbuf != IB.vbuf || R.b.voff != IB.voff) {
        vkCmdBindVertexBuffers(R.cmd, 0u, 1u, &IB.vbuf, &IB.voff);
        R.b.vbuf = IB.vbuf; R.b.voff = IB.voff;
    }
    nvk_scissor(IB.plane, IB.scissor);
    /* Adjacent triangle ranges are one draw. */
    for (uint32_t d = 0u; d < draw_count;) {
        const uint32_t first = IB.first[d];
        uint32_t count = IB.count[d++];
        while (d < draw_count && IB.first[d] == first + count) count += IB.count[d++];
        ++P.draw_calls; vkCmdDraw(R.cmd, count, 1u, first, 0u);
    }
    return 1;
}

/* Appends the current triangle (count vertices from first) to the batch. */
static int nvk_il_add(NvkPlane *plane, const int32_t scissor[4], uint32_t first, uint32_t count)
{
    uint32_t shared[NVK_IL_SHARED_TEXELS][4], record[NVK_IL_TRIANGLE_TEXELS][4];
    nvk_il_pack(record, shared);
    const uint32_t index = first / 3u, records = count / 3u;
    if (IB.draw_count &&
        (IB.plane != plane || memcmp(&IB.view, &R.v, sizeof(R.v)) || memcmp(IB.scissor, scissor, sizeof(IB.scissor)) || memcmp(&IB.key, &R.key, sizeof(R.key)) ||
         IB.vbuf != R.vsrc.buf || IB.voff != R.vsrc.off || index < IB.base ||
         index - IB.base + records > NVK_IL_CAPACITY || IB.draw_count == NVK_IL_CAPACITY ||
         (IB.shared_count == NVK_IL_CAPACITY && memcmp(shared, IB.shared[NVK_IL_CAPACITY - 1u], sizeof(shared)))))
        if (!nvk_il_flush()) return 0;
    if (!IB.draw_count) {
        IB.base = index; IB.parameter_count = IB.shared_count = 0u;
        IB.plane = plane; IB.view = R.v; IB.key = R.key; IB.vbuf = R.vsrc.buf; IB.voff = R.vsrc.off;
        memcpy(IB.scissor, scissor, sizeof(IB.scissor));
    }
    if (!IB.shared_count || memcmp(shared, IB.shared[IB.shared_count - 1u], sizeof(shared)))
        memcpy(IB.shared[IB.shared_count++], shared, sizeof(shared));
    record[8][3] = IB.shared_count - 1u;
    const uint32_t offset = index - IB.base;
    for (uint32_t i = 0u; i < records; ++i)
        memcpy(IB.texels[(size_t)(offset + i) * NVK_IL_TRIANGLE_TEXELS], record, sizeof(record));
    IB.parameter_count = offset + records;
    IB.first[IB.draw_count] = first; IB.count[IB.draw_count] = count;
    ++IB.draw_count;
    return 1;
}

enum { NVK_CORE_BATCH_TRIANGLES = 512, NVK_CORE_TRIANGLE_TEXELS = 12 };
typedef struct NvkCoreBatch {
    NvkView view;
    NvkDraw draw;
    NvkSetKey key;
    NvkPlane *plane;
    int32_t scissor[4];
    int key_attached;
    uint32_t triangles;
    uint32_t records[NVK_CORE_BATCH_TRIANGLES][NVK_CORE_TRIANGLE_TEXELS][4];
    float vertices[NVK_CORE_BATCH_TRIANGLES * 3u][GL_NATIVE_GPU_VERTEX_FLOATS];
} NvkCoreBatch;
static NvkCoreBatch CB;
/* Captured fragments carry their FIFO sequence in record[4].z. Unlike normal
 * destination-reading draws, these batches may be grouped by material even
 * when their coverage overlaps. Flush at every pass/resource boundary. */
static NvkCoreBatch *s_capture_batches[128];
static uint32_t s_capture_batch_count;

static VkPipeline nvk_material_pipeline(int key_attached, int batch, const NvkDraw *draw, const NvkView *view);

static int nvk_core_batch_emit(NvkCoreBatch *batch)
{
    const uint32_t triangles = batch->triangles;
    if (!triangles) return 1;
    batch->triangles = 0u;
    const VkDeviceSize bytes = (VkDeviceSize)triangles * sizeof(batch->records[0]);
    /* Parameter views cover the first arena chunk's worth of texels, including
     * when a larger transfer allocation made an oversized arena buffer. */
    if (S.chunk_cur < S.chunk_count && S.chunks[S.chunk_cur].cursor + bytes + 16u > NVK_ARENA_CHUNK) ++S.chunk_cur;
    uint32_t chunk;
    VkDeviceSize offset;
    uint8_t *records = nvk_arena(bytes, 16u, &chunk, &offset);
    if (!records || offset + bytes > NVK_ARENA_CHUNK) return 0;
    memcpy(records, batch->records, (size_t)bytes);
    NvkSetKey key = batch->key;
    key.params = nvk_chunk_params_view(chunk);
    if (!key.params) return 0;
    const uint32_t record_base = (uint32_t)(offset / 16u);
    const VkDeviceSize vertex_bytes = (VkDeviceSize)triangles * 3u * sizeof(batch->vertices[0]);
    uint8_t *vertices = nvk_arena(vertex_bytes, 16u, &chunk, &offset);
    if (!vertices) return 0;
    memcpy(vertices, batch->vertices, (size_t)vertex_bytes);
    const VkBuffer vertex_buffer = S.chunks[chunk].buf.buffer;
    const VkDeviceSize vertex_offset = offset;
    VkPipeline pipeline = nvk_material_pipeline(batch->key_attached, 1, &batch->draw, &batch->view);
    if (R.b.pipeline != pipeline) {
        vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        R.b.pipeline = pipeline;
    }
    VkDescriptorSet set0 = nvk_set0(&key);
    if (!set0) return 0;
    if (R.b.set0 != set0) {
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_core_batch, 0u, 1u, &set0, 0u, NULL);
        R.b.set0 = set0;
    }
    uint8_t *uniform = nvk_arena(NVK_SLOT, NVK_SLOT_ALIGN, &chunk, &offset);
    if (!uniform) return 0;
    memcpy(uniform, &batch->view, sizeof(batch->view));
    memcpy(uniform + NVK_SLOT_ALIGN, &batch->draw, sizeof(batch->draw));
    VkDescriptorSet set1 = nvk_chunk_set(chunk, 0);
    if (!set1) return 0;
    const uint32_t offsets[2] = { (uint32_t)offset, (uint32_t)offset + NVK_SLOT_ALIGN };
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_core_batch, 1u, 1u, &set1, 2u, offsets);
    R.b.v = batch->view; R.b.d = batch->draw; R.b.slot_valid = 1;
    vkCmdPushConstants(R.cmd, S.pl_core_batch, VK_SHADER_STAGE_VERTEX_BIT, 0u, sizeof(record_base), &record_base);
    if (R.capture_active) vkCmdPushConstants(R.cmd, S.pl_core_batch, VK_SHADER_STAGE_FRAGMENT_BIT,
        4u, sizeof(R.capture_push.rect), R.capture_push.rect);
    vkCmdBindVertexBuffers(R.cmd, 0u, 1u, &vertex_buffer, &vertex_offset);
    R.b.vbuf = vertex_buffer; R.b.voff = vertex_offset;
    nvk_scissor(batch->plane, batch->scissor);
    ++P.draw_calls; vkCmdDraw(R.cmd, triangles * 3u, 1u, 0u, 0u);
    ++P.core_batches; P.core_batch_triangles += triangles;
    const unsigned category = R.capture_active ? 0u : batch->draw.hd_on ? 1u : 2u;
    ++P.batch_draws[category]; P.batch_triangles[category] += triangles;
    return 1;
}

static int nvk_core_batch_flush(void)
{
    if (!nvk_core_batch_emit(&CB)) return 0;
    for (uint32_t i = 0u; i < s_capture_batch_count; ++i)
        if (!nvk_core_batch_emit(s_capture_batches[i])) return 0;
    s_capture_batch_count = 0u;
    return 1;
}

static int nvk_core_batch_matches(const NvkCoreBatch *batch, NvkPlane *plane, int key_attached,
                                   const int32_t scissor[4], const NvkDraw *material)
{
    return batch->plane == plane && batch->key_attached == key_attached &&
        !memcmp(&batch->key, &R.key, sizeof(R.key)) && !memcmp(batch->scissor, scissor, sizeof(batch->scissor)) &&
        !memcmp(&batch->view, &R.v, sizeof(R.v)) && !memcmp(&batch->draw, material, sizeof(*material));
}

static int nvk_core_batch_add(NvkPlane *plane, int key_attached, const int32_t scissor[4], uint32_t first)
{
    NvkDraw material = R.d;
    memset(material.sample_lim, 0, sizeof(material.sample_lim));
    memset(material.uv_gradient, 0, sizeof(material.uv_gradient));
    memset(material.depth_plane, 0, 3u * sizeof(material.depth_plane[0]));
    memset(material.depth_origin, 0, sizeof(material.depth_origin));
    memset(material.attribute_origin, 0, 2u * sizeof(material.attribute_origin[0]));
    memset(material.attribute_plane, 0, sizeof(material.attribute_plane));
    memset(material.attribute_dda, 0, sizeof(material.attribute_dda));
    if (!material.depth_view[0]) memset(material.depth_view, 0, sizeof(material.depth_view));
    if (!material.flags[2]) memset(material.origin, 0, sizeof(material.origin));
    /* Ignore state the selected shader path cannot consume. Stale texture,
     * blend and depth parameters must not split otherwise identical batches. */
    if (!material.state[2]) material.state[3] = 0;
    if (!material.depth_state[0]) { material.depth_plane[3] = 0; material.depth_state[3] = 0; }
    if (!material.texture_filter) material.filter_strength = 0.f;
    if (!material.window[0]) material.window[2] = 0;
    if (!material.window[1]) material.window[3] = 0;
    if (!material.state[0]) {
        memset(material.page, 0, sizeof(material.page));
        memset(material.window, 0, sizeof(material.window));
        material.u_depth = material.texture_filter = material.anisotropy = 0;
        material.filter_strength = 0.f;
        if (material.attribute_origin[3] != 0.f) material.attribute_origin[2] = 0.f;
    } else {
        if (material.state[1]) material.attribute_origin[3] = 0.f;
        if (material.decoded_on) { memset(material.page, 0, sizeof(material.page)); material.u_depth = 0; }
        else if ((material.u_depth & 3) >= 2) material.page[2] = material.page[3] = 0;
    }
    /* Page, palette and texture-window coordinates travel with each triangle.
     * They do not require a new draw when the sampled image is unchanged. */
    memset(material.page, 0, sizeof(material.page));
    memset(material.window, 0, sizeof(material.window));
    if (material.hd_on) {
        memset(material.hd_map, 0, sizeof(material.hd_map));
        memset(material.hd_lim, 0, sizeof(material.hd_lim));
        material.anisotropy = material.texture_filter = 0;
        material.filter_strength = 0.f;
    }
    NvkCoreBatch *batch = &CB;
    if (R.capture_active && S.sort_capture) {
        uint32_t i = 0u;
        while (i < s_capture_batch_count && !nvk_core_batch_matches(s_capture_batches[i], plane, key_attached, scissor, &material)) ++i;
        if (i == 128u) {
            if (!nvk_core_batch_flush()) return 0;
            i = 0u;
        }
        if (i == s_capture_batch_count) {
            if (!s_capture_batches[i]) s_capture_batches[i] = calloc(1u, sizeof(*batch));
            if (!s_capture_batches[i]) return 0;
            s_capture_batches[i]->triangles = 0u;
            ++s_capture_batch_count;
        }
        batch = s_capture_batches[i];
        if (batch->triangles == NVK_CORE_BATCH_TRIANGLES) {
            if (!nvk_core_batch_emit(batch)) return 0;
        }
    } else if (CB.triangles && (CB.triangles == NVK_CORE_BATCH_TRIANGLES ||
        !nvk_core_batch_matches(&CB, plane, key_attached, scissor, &material))) {
        if (!nvk_core_batch_flush()) return 0;
    }
    if (!batch->triangles) {
        batch->plane = plane; batch->key_attached = key_attached; batch->key = R.key;
        batch->view = R.v; batch->draw = material;
        memcpy(batch->scissor, scissor, sizeof(batch->scissor));
    }
    if (!R.vsrc.vertices) return 0;
    uint32_t (*record)[4] = batch->records[batch->triangles];
    memcpy(record[0], R.d.sample_lim, 16u);
    memcpy(record[1], R.d.depth_plane, 16u);
    memcpy(record[2], R.d.attribute_origin, 16u);
    memcpy(record[3], R.d.uv_gradient, 16u);
    memcpy(record[4], R.d.depth_origin, 8u);
    record[4][2] = R.capture_active ? ++R.capture_order : 0u;
    record[4][3] = !R.capture_active && R.damage_shader_active ? R.damage_word_offset + 1u : 0u;
    memcpy(record[5], R.d.attribute_origin[2] == 1.f ? (void *)R.d.attribute_dda : (void *)R.d.attribute_plane, 80u);
    memcpy(record[10], R.d.page, sizeof(R.d.page));
    memcpy(record[11], R.d.window, sizeof(R.d.window));
    if (R.d.hd_on) {
        /* HD sampling uses derivatives of its own coordinates, not the
         * guest anisotropic gradient. The attribute planes consume xyz;
         * their spare w lanes can hold the per-triangle HD clamp. */
        memcpy(record[3], R.d.hd_map, sizeof(R.d.hd_map));
        for (unsigned i = 0u; i < 4u; ++i) record[5u+i][3] = (uint32_t)R.d.hd_lim[i];
    }
    if (R.capture_active && S.direct_capture) {
        const int32_t *r = R.capture_push.rect;
        const int x0 = R.draw_bounds[0] > r[0] ? R.draw_bounds[0] : r[0];
        const int y0 = R.draw_bounds[1] > r[1] ? R.draw_bounds[1] : r[1];
        const int x1 = R.draw_bounds[2] < r[0] + r[2] ? R.draw_bounds[2] : r[0] + r[2];
        const int y1 = R.draw_bounds[3] < r[1] + r[3] ? R.draw_bounds[3] : r[1] + r[3];
        const uint32_t width = x1 > x0 ? (uint32_t)(x1 - x0) : 0u;
        const uint32_t height = y1 > y0 ? (uint32_t)(y1 - y0) : 0u;
        record[4][3] = R.capture_reserved < S.capture_capacity ? (uint32_t)R.capture_reserved : UINT32_MAX;
        record[5][3] = (uint32_t)x0; record[6][3] = (uint32_t)y0;
        record[7][3] = width; record[8][3] = 1u; record[9][3] = height;
        R.capture_reserved += (uint64_t)width * height;
    }
    memcpy(batch->vertices + batch->triangles * 3u, R.vsrc.vertices + first, 3u * sizeof(batch->vertices[0]));
    ++batch->triangles;
    return 1;
}

/* One draw of the native pipeline with the current uniform state and resource key. */
static int nvk_draw_region_page(NvkPlane *plane, int key_attached, const int32_t scissor[4], uint32_t first, uint32_t count)
{
    if (TC.active) {
        uint32_t shared[NVK_IL_SHARED_TEXELS][4], record[NVK_IL_TRIANGLE_TEXELS][4];
        nvk_il_pack(record, shared);
        if (!TC.visible) {   /* one material record, to be copied */
            memcpy(TC.texels, record, sizeof(record));
            memcpy(TC.texels + NVK_IL_TRIANGLE_TEXELS, shared, sizeof(shared));
            return 1;
        }
        const uint32_t index = first / 3u;
        record[8][3] = index;
        memcpy(TC.texels + (size_t)index * NVK_IL_TRIANGLE_TEXELS, record, sizeof(record));
        memcpy(TC.texels + (size_t)TC.triangles * NVK_IL_TRIANGLE_TEXELS + (size_t)index * NVK_IL_SHARED_TEXELS, shared, sizeof(shared));
        TC.visible[index] = 1u;
        return 1;
    }
    if (R.since_submit >= (S.interlock ? NVK_SUBMIT_DRAWS_IL : S.submit_draw_limit) && !nvk_submit_partial(VK_NULL_HANDLE)) return 0;
    ++R.since_submit;
    if (!nvk_raster_page(plane, scissor[1])) return 0;
    if (S.interlock) {
        if (!nvk_require_vertices()) return 0;
        R.key.dest = plane->color.view;
        R.key.dest_key = plane->key.view;
        R.key.recipe = R.recipe;
        R.key.params = VK_NULL_HANDLE;
        if (R.batchable) return nvk_il_add(plane, scissor, first, count);
        /* Anything else draws on its own, from the loose uniforms. */
        if (!nvk_il_flush()) return 0;
        NvkDrawIL il;
        memset(&il, 0, sizeof(il));
        memcpy(&il, &R.d, offsetof(NvkDraw, hd_on) + 2u * sizeof(int32_t));
        il.u_depth = R.d.u_depth; il.aa_exempt = R.d.aa_exempt; il.texture_filter = R.d.texture_filter;
        il.anisotropy = R.d.anisotropy; il.filter_strength = R.d.filter_strength;
        NvkSetKey key = R.key;
        key.params = S.dummy_view;
        if (!nvk_il_bind(&il, R.v.size, &key, &R.v)) return 0;
        if (R.b.vbuf != R.vsrc.buf || R.b.voff != R.vsrc.off) {
            vkCmdBindVertexBuffers(R.cmd, 0u, 1u, &R.vsrc.buf, &R.vsrc.off);
            R.b.vbuf = R.vsrc.buf; R.b.voff = R.vsrc.off;
        }
        nvk_scissor(plane, scissor);
        ++P.draw_calls; vkCmdDraw(R.cmd, count, 1u, first, 0u);
        return 1;
    }
    R.key.dest = plane->color.view; R.key.dest_key = plane->key.view; R.key.recipe = R.recipe; R.key.params = VK_NULL_HANDLE;
    R.key.capture_meta = R.damage_shader_active && !R.capture_active ? R.damage_buffer : VK_NULL_HANDLE;
    if (S.core_batch && R.batchable && count == 3u) return nvk_core_batch_add(plane, key_attached, scissor, first);
    if (!nvk_require_vertices()) return 0;
    if (!nvk_core_batch_flush()) return 0;
    VkPipeline pipeline = nvk_material_pipeline(key_attached, 0, &R.d, &R.v);
    if (R.b.pipeline != pipeline) {
        vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        R.b.pipeline = pipeline;
    }
    R.key.dest = plane->color.view;
    R.key.dest_key = plane->key.view;
    R.key.recipe = R.recipe;
    VkDescriptorSet set0 = nvk_set0(&R.key);
    if (!set0) return 0;
    if (R.b.set0 != set0) {
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_native, 0u, 1u, &set0, 0u, NULL);
        R.b.set0 = set0;
    }
    if (!R.b.slot_valid || memcmp(&R.v, &R.b.v, sizeof(R.v)) || memcmp(&R.d, &R.b.d, sizeof(R.d))) {
        uint32_t chunk;
        VkDeviceSize offset;
        uint8_t *slot = nvk_arena(NVK_SLOT, NVK_SLOT_ALIGN, &chunk, &offset);
        if (!slot) return 0;
        memcpy(slot, &R.v, sizeof(R.v));
        memcpy(slot + NVK_SLOT_ALIGN, &R.d, sizeof(R.d));
        VkDescriptorSet set1 = nvk_chunk_set(chunk, 0);
        if (!set1) return 0;
        const uint32_t offsets[2] = { (uint32_t)offset, (uint32_t)offset + NVK_SLOT_ALIGN };
        vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_native, 1u, 1u, &set1, 2u, offsets);
        R.b.v = R.v; R.b.d = R.d; R.b.slot_valid = 1;
    }
    if (R.b.vbuf != R.vsrc.buf || R.b.voff != R.vsrc.off) {
        vkCmdBindVertexBuffers(R.cmd, 0u, 1u, &R.vsrc.buf, &R.vsrc.off);
        R.b.vbuf = R.vsrc.buf; R.b.voff = R.vsrc.off;
    }
    nvk_scissor(plane, scissor);
    ++P.draw_calls; vkCmdDraw(R.cmd, count, 1u, first, 0u);
    return 1;
}

static int nvk_draw_region(NvkPlane *plane, int key_attached, const int32_t scissor[4], uint32_t first, uint32_t count)
{
    if (!plane->page_rows || TC.active)
        return nvk_draw_region_page(plane, key_attached, scissor, first, count);
    const int end = scissor[1] + scissor[3];
    for (int y = scissor[1] > 0 ? scissor[1] : 0; y < end;) {
        int next = (int)(((uint32_t)y / plane->page_rows + 1u) * plane->page_rows);
        if (next > end) next = end;
        const int32_t clipped[4] = {scissor[0], y, scissor[2], next - y};
        if (!nvk_draw_region_page(plane, key_attached, clipped, first, count)) return 0;
        y = next;
    }
    return 1;
}

static int nvk_damage_intersects(int left, int top, int right, int bottom)
{
    if (!R.damage_active) return 1;
    const uint32_t begin = R.damage_region >= 0 ? (uint32_t)R.damage_region : 0u;
    const uint32_t end = R.damage_region >= 0 ? begin + 1u : R.damage_count;
    for (uint32_t i = begin; i < end; ++i) {
        const int32_t *r = R.damage_rects[i];
        if (left < r[0] + r[2] && right > r[0] && top < r[1] + r[3] && bottom > r[1]) return 1;
    }
    return 0;
}

/* An unconditional rectangular fill does not need the textured fragment
 * program once per damage rectangle. Clear both color attachments in raster
 * order, preserving the exact union of covered pixels and all later hazards.
 * The caller has prepared the same target/depth state as the draw path. */
static int nvk_clear_span(NvkPlane *plane, const GlNativeGpuCommand *command, unsigned depth)
{
    if (!nvk_core_batch_flush() || !nvk_il_flush()) return 0;
    VkClearRect rects[sizeof(R.damage_rects) / sizeof(R.damage_rects[0])];
    uint32_t count = 0u;
    const int damaged = R.damage_active && plane == &R.planes[R.damage_plane];
    const uint32_t begin = damaged && R.damage_region >= 0 ? (uint32_t)R.damage_region : 0u;
    const uint32_t end = !damaged ? 1u : R.damage_region >= 0 ? begin + 1u : R.damage_count;
    for (uint32_t i = begin; i < end; ++i) {
        int x0 = R.draw_bounds[0], y0 = R.draw_bounds[1];
        int x1 = R.draw_bounds[2], y1 = R.draw_bounds[3];
        if (damaged) {
            const int32_t *r = R.damage_rects[i];
            if (x0 < r[0]) x0 = r[0]; if (y0 < r[1]) y0 = r[1];
            if (x1 > r[0] + r[2]) x1 = r[0] + r[2];
            if (y1 > r[1] + r[3]) y1 = r[1] + r[3];
        }
        if (x0 >= x1 || y0 >= y1) continue;
        rects[count++] = (VkClearRect){
            .rect = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } },
            .baseArrayLayer = 0u, .layerCount = 1u,
        };
    }
    if (count) {
        VkClearAttachment clear[2] = {
            { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0u },
            { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 1u },
        };
        float *rgba = clear[0].clearValue.color.float32;
        for (uint32_t i = 0u; i < 4u; ++i) rgba[i] = ((command->color >> (8u * i)) & 255u) / 255.f;
        /* Native alpha encodes mask/AA flags, rather than opacity. */
        if (command->mask & 1u)
            rgba[3] = rgba[3] > .83f || (rgba[3] > .16f && rgba[3] < .5f) ? 1.f : 2.f / 3.f;
        nvk_pass_begin(plane);
        vkCmdClearAttachments(R.cmd, (depth & NVK_UNIFORM_DEPTH_ATTACHED) ? 2u : 1u, clear, count, rects);
        ++P.cleared_spans; P.cleared_rects += count;
    }
    ++R.c.transfer_draws;
    return 1;
}

static int nvk_draw(NvkPlane *plane, int key_attached, const int32_t scissor[4], uint32_t first, uint32_t count)
{
    if (!R.damage_active || plane != &R.planes[R.damage_plane] || (R.capture_active && R.capture_damage_mask) ||
        (R.damage_shader_active && R.batchable && count == 3u))
        return nvk_draw_region(plane, key_attached, scissor, first, count);
    const uint32_t begin = R.damage_region >= 0 ? (uint32_t)R.damage_region : 0u;
    const uint32_t end = R.damage_region >= 0 ? begin + 1u : R.damage_count;
    for (uint32_t i = begin; i < end; ++i) {
        const int32_t *r = R.damage_rects[i];
        int x0 = scissor[0] > r[0] ? scissor[0] : r[0];
        int y0 = scissor[1] > r[1] ? scissor[1] : r[1];
        int x1 = scissor[0] + scissor[2] < r[0] + r[2] ? scissor[0] + scissor[2] : r[0] + r[2];
        int y1 = scissor[1] + scissor[3] < r[1] + r[3] ? scissor[1] + scissor[3] : r[1] + r[3];
        if (x0 >= x1 || y0 >= y1 || x0 >= R.draw_bounds[2] || x1 <= R.draw_bounds[0] ||
            y0 >= R.draw_bounds[3] || y1 <= R.draw_bounds[1]) continue;
        const int32_t clipped[4] = { x0, y0, x1 - x0, y1 - y0 };
        if (!nvk_draw_region(plane, key_attached, clipped, first, count)) return 0;
    }
    return 1;
}

/* ---- target preparation (GL native_gpu_target, core tier) ------------------------------------------------- */
static int32_t s_scissor_value[4];

static int nvk_target(NvkPlane *plane, uint32_t scale, int x, int y, int w, int h, int read_destination,
                      unsigned depth, const int *draw_scissor,
                      const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], uint32_t vertex_count, float offset)
{
    const int read_depth = (depth & NVK_UNIFORM_DEPTH_READ) != 0;
    const int px0 = x > 0 ? x * (int)scale : 0;
    const int py0 = y > 0 ? y * (int)scale : 0;
    const int right = (x + w) * (int)scale, bottom = (y + h) * (int)scale;
    const int px1 = right < (int)plane->width ? right : (int)plane->width;
    const int py1 = bottom < (int)plane->height ? bottom : (int)plane->height;
    R.draw_bounds[0] = px0; R.draw_bounds[1] = py0;
    R.draw_bounds[2] = px1; R.draw_bounds[3] = py1;
    GlNativeGpuShape shapes[2];
    uint32_t shape_count = 0;
    int shape_unknown = 0;
    /* The destination reads below need the plane's pass. */
    nvk_pass_begin(plane);
    if (!R.scheduled_run && !S.interlock) {
        if (vertices && vertex_count) {
            for (uint32_t t = 0; t < vertex_count / 3u; ++t) {
                const int r = native_gpu_shape_tri(&shapes[shape_count], vertices[t * 3u], vertices[t * 3u + 1u],
                                                   vertices[t * 3u + 2u], offset, scale);
                if (r < 0) shape_unknown = 1;
                else if (r) ++shape_count;
            }
        } else if (px0 < px1 && py0 < py1) native_gpu_shape_rect(&shapes[shape_count++], px0, py0, px1, py1);
    }
    if (!S.interlock && (read_destination || read_depth)) {
        int hazard = plane->dirty_all || shape_unknown;
        for (uint32_t i = 0; !hazard && i < shape_count; ++i)
            hazard = native_gpu_hazard_hit(plane->width, plane->height, plane->hazards, &shapes[i], scale);
        if (R.scheduled_run) {
            /* nvk_schedule_run placed the barriers. */
        } else if (hazard) {
            nvk_texture_barrier();
            R.c.destination_barriers++;
            nvk_plane_barrier_clear(plane);
        } else R.c.skipped_barriers++;
    }
    /* Triangle coverage already lies inside its conservative bbox. Use the
     * authored viewport scissor for DRAW runs, the bbox for transfers. */
    s_scissor_value[0] = (draw_scissor ? draw_scissor[0] : x) * (int)scale;
    s_scissor_value[1] = (draw_scissor ? draw_scissor[1] : y) * (int)scale;
    s_scissor_value[2] = (draw_scissor ? draw_scissor[2] : w) * (int)scale;
    s_scissor_value[3] = (draw_scissor ? draw_scissor[3] : h) * (int)scale;
    if (!R.scheduled_run && !S.interlock && !plane->dirty_all) {
        if (shape_unknown) plane->dirty_all = 1;
        for (uint32_t i = 0; i < shape_count && !plane->dirty_all; ++i)
            native_gpu_hazard_add(plane->width, plane->height, &plane->hazards, &plane->dirty_all, &shapes[i]);
    }
    return 1;
}

/* ---- seeds ---------------------------------------------------------------------------------------------------- */
/* GL re-specifies one input texture per SEED; every seed here keeps its image
 * until the execution ends. */
static int nvk_seed(const GlNativeGpuCommand *command, const uint8_t *data, VkImageView *view)
{
    nvk_pass_end();
    if (S.seed_count == S.seed_cap) {
        const uint32_t cap = S.seed_cap ? S.seed_cap * 2u : 16u;
        NvkSeed *seeds = realloc(S.seeds, cap * sizeof(*seeds));
        if (!seeds) return 0;
        memset(seeds + S.seed_cap, 0, (cap - S.seed_cap) * sizeof(*seeds));
        S.seeds = seeds; S.seed_cap = cap;
    }
    NvkSeed *seed = &S.seeds[S.seed_count];
    const VkDeviceSize bytes = (VkDeviceSize)command->w * command->h * 4u;
    /* A slot keeps its image for the next work when the size matches. */
    if (seed->image.image && (seed->image.width != (uint32_t)command->w || seed->image.height != (uint32_t)command->h)) {
        vk_core_image_destroy(&seed->image);
        memset(&seed->image, 0, sizeof(seed->image));
    }
    if (!seed->image.image &&
        !vk_core_image_create(&seed->image, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)command->w, (uint32_t)command->h, 1u,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native seed")) return 0;
    ++S.seed_count;
    uint32_t chunk;
    VkDeviceSize offset;
    uint8_t *stage = nvk_arena(bytes, 16u, &chunk, &offset);
    if (!stage) return 0;
    memcpy(stage, data + command->data, (size_t)bytes);
    vk_core_image_barrier(R.cmd, seed->image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = offset;
    region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    region.imageExtent = (VkExtent3D){ (uint32_t)command->w, (uint32_t)command->h, 1u };
    vkCmdCopyBufferToImage(R.cmd, S.chunks[chunk].buf.buffer, seed->image.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    nvk_barrier_all();
    *view = seed->image.view;
    return 1;
}

/* ---- HD texture replacement (the shared policy of hd_texture_cache.c) ------------------------------------------- */
static HdTextureCache s_hd_cache;

/* Records the upload and the mip chain into the work's command buffer; the
 * image is usable by every later draw of the work and of later works. */
static uint64_t nvk_hd_upload(void *context, uint32_t width, uint32_t height, const uint8_t *rgba)
{
    (void)context;
    nvk_pass_end();
    uint32_t levels = 1u;
    for (uint32_t m = width > height ? width : height; m > 1u; m >>= 1u) ++levels;
    NvkHd *hd = calloc(1u, sizeof(*hd));
    if (!hd) return 0u;
    if (!vk_core_image_create(&hd->image, VK_FORMAT_R8G8B8A8_UNORM, width, height, levels,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native hd")) {
        free(hd);
        return 0u;
    }
    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4u;
    uint32_t chunk;
    VkDeviceSize offset;
    uint8_t *stage = nvk_arena(bytes, 16u, &chunk, &offset);
    if (!stage) { vk_core_image_destroy(&hd->image); free(hd); return 0u; }
    memcpy(stage, rgba, (size_t)bytes);
    vk_core_image_barrier(R.cmd, hd->image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = offset;
    region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    region.imageExtent = (VkExtent3D){ width, height, 1u };
    vkCmdCopyBufferToImage(R.cmd, S.chunks[chunk].buf.buffer, hd->image.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    for (uint32_t level = 1u; level < levels; ++level) {
        nvk_barrier_all();
        const int32_t sw = (int32_t)(width >> (level - 1u) ? width >> (level - 1u) : 1u);
        const int32_t sh = (int32_t)(height >> (level - 1u) ? height >> (level - 1u) : 1u);
        const int32_t dw = (int32_t)(width >> level ? width >> level : 1u);
        const int32_t dh = (int32_t)(height >> level ? height >> level : 1u);
        VkImageBlit blit;
        memset(&blit, 0, sizeof(blit));
        blit.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0u, 1u };
        blit.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u };
        blit.srcOffsets[1] = (VkOffset3D){ sw, sh, 1 };
        blit.dstOffsets[1] = (VkOffset3D){ dw, dh, 1 };
        vkCmdBlitImage(R.cmd, hd->image.image, VK_IMAGE_LAYOUT_GENERAL, hd->image.image, VK_IMAGE_LAYOUT_GENERAL,
                       1u, &blit, VK_FILTER_LINEAR);
    }
    nvk_barrier_all();
    return (uint64_t)(uintptr_t)hd;
}

/* An evicted image may be named by the commands already recorded: it dies when
 * the execution is released. */
static void nvk_hd_destroy(void *context, uint64_t handle)
{
    (void)context;
    NvkHd *hd = (NvkHd *)(uintptr_t)handle;
    if (S.hd_garbage_count == S.hd_garbage_cap) {
        const uint32_t cap = S.hd_garbage_cap ? S.hd_garbage_cap * 2u : 32u;
        NvkHd **grown = realloc(S.hd_garbage, cap * sizeof(*grown));
        if (!grown) { R.fail_code = R.fail_code ? R.fail_code : (int32_t)VK_ERROR_OUT_OF_HOST_MEMORY; return; }
        S.hd_garbage = grown; S.hd_garbage_cap = cap;
    }
    S.hd_garbage[S.hd_garbage_count++] = hd;
}

static void nvk_hd_free_garbage(void)
{
    for (uint32_t i = 0u; i < S.hd_garbage_count; ++i) {
        vk_core_image_destroy(&S.hd_garbage[i]->image);
        free(S.hd_garbage[i]);
    }
    S.hd_garbage_count = 0u;
}

static const NvkHd *nvk_hd_lookup(const GpuRenderHdTexture *hd)
{
    const HdCacheOps ops = { nvk_hd_upload, nvk_hd_destroy, NULL };
    return (const NvkHd *)(uintptr_t)hd_cache_lookup(&s_hd_cache, hd, &ops);
}

/* Immutable phase snapshots may be decoded once per page/CLUT combination.
 * Base-journal VRAM remains on the raw path because writes can change it. */
static int nvk_decoded_source(uint16_t *saved, const uint16_t *words, const int32_t key[6], int copy)
{
    const uint32_t width = 64u << key[4];
    const uint32_t first = width < 1024u - (uint32_t)key[0] ? width : 1024u - (uint32_t)key[0];
    for (uint32_t y = 0u; y < 256u; ++y) {
        const uint16_t *row = words + (((uint32_t)key[1] + y) & 511u) * 1024u;
        uint16_t *out = saved + y * width;
        if (copy) {
            memcpy(out, row + key[0], first * sizeof(*out));
            if (first < width) memcpy(out + first, row, (width - first) * sizeof(*out));
        } else if (memcmp(out, row + key[0], first * sizeof(*out)) ||
            (first < width && memcmp(out + first, row, (width - first) * sizeof(*out)))) return 0;
    }
    const uint32_t palette = key[4] == 0 ? 16u : key[4] == 1 ? 256u : 0u;
    const uint32_t part = palette < 1024u - (uint32_t)key[2] ? palette : 1024u - (uint32_t)key[2];
    const uint16_t *row = words + (uint32_t)key[3] * 1024u;
    saved += width * 256u;
    if (copy) {
        memcpy(saved, row + key[2], part * sizeof(*saved));
        if (part < palette) memcpy(saved + part, row, (palette - part) * sizeof(*saved));
    } else if (memcmp(saved, row + key[2], part * sizeof(*saved)) ||
        (part < palette && memcmp(saved + part, row, (palette - part) * sizeof(*saved)))) return 0;
    return 1;
}

static VkImageView nvk_decoded_page(const XgRenderIrMaterialState *material, uint32_t snapshot, VkImageView *filtered)
{
    *filtered = VK_NULL_HANDLE;
    if (!S.pipe_decode || !material->textured || R.work->wireframe ||
        (!snapshot && (!S.hardware_aniso || !S.upload_valid)) ||
        (snapshot && (!R.work->phase_textures || snapshot > R.work->phase_textures->count))) return VK_NULL_HANDLE;
    if (!snapshot && S.decoded_base_generation != S.words_generation) {
        for (uint32_t i = 0u; i < S.decoded_work_count;)
            if (S.decoded_work[i].key[5] == 0) S.decoded_work[i] = S.decoded_work[--S.decoded_work_count];
            else ++i;
        S.decoded_base_generation = S.words_generation;
        /* Removing base references can make a previously full table admit a
         * missing key. Drop negative hints as well as displaced indices. */
        memset(S.decoded_work_hints, 0, sizeof(S.decoded_work_hints));
    }
    const int depth = material->texture_depth < 2 ? material->texture_depth : 2;
    const int32_t key[6] = { (material->texture_page_x * 64) & 1023, (material->texture_page_y * 256) & 511,
        depth < 2 ? material->clut_x & 1023 : 0, depth < 2 ? material->clut_y & 511 : 0, depth, (int32_t)snapshot };
    /* The work cache is immutable between base-word updates, but this lookup
     * runs for every triangle. A hint avoids scanning up to 64 keys each time.
     * Full-key validation is essential: base invalidation swaps entries, and
     * hash collisions must never reuse a different page or palette. */
    const uint32_t packed = (uint32_t)(key[0] >> 6) | (uint32_t)(key[1] >> 8) << 4u |
        (uint32_t)key[2] << 5u | (uint32_t)key[3] << 15u | (uint32_t)depth << 24u;
    const uint32_t bucket = ((packed ^ snapshot * UINT32_C(0x9e3779b9)) * UINT32_C(0x85ebca6b)) >> 24u;
    uint32_t found = S.decode_hints ? S.decoded_work_hints[bucket] : 0u;
    if (found == UINT8_MAX && !memcmp(S.decoded_missing_keys[bucket], key, sizeof(key))) {
        ++P.decoded_negative_hits;
        return VK_NULL_HANDLE;
    }
    if (found && found <= S.decoded_work_count &&
        !memcmp(S.decoded_work[found - 1u].key, key, sizeof(key))) {
        ++P.decoded_hint_hits;
    } else {
        if (S.decode_hints) ++P.decoded_hint_misses;
        found = 0u;
        for (uint32_t i = 0u; i < S.decoded_work_count; ++i)
            if (!memcmp(S.decoded_work[i].key, key, sizeof(key))) { found = i + 1u; break; }
    }
    if (found) {
        if (S.decode_hints) S.decoded_work_hints[bucket] = (uint8_t)found;
        S.decoded_layer = S.decoded_work[found - 1u].entry;
        *filtered = S.decode_arrays ? (S.hardware_aniso ? S.filtered_array.view : VK_NULL_HANDLE)
            : S.decoded[S.decoded_layer].filtered.view;
        return S.decode_arrays ? S.decoded_array.view : S.decoded[S.decoded_layer].image.view;
    }
    if (S.decoded_work_count == 64u) {
        ++P.decoded_work_full;
        if (S.decode_hints) {
            /* No reference can be admitted until base invalidation or the
             * next work, both of which clear all hints. Preserve the original
             * raw sampling fallback without rescanning 64 keys per triangle. */
            memcpy(S.decoded_missing_keys[bucket], key, sizeof(key));
            S.decoded_work_hints[bucket] = UINT8_MAX;
        }
        return VK_NULL_HANDLE;
    }
    const uint16_t *source = snapshot ? R.work->phase_textures->words + (size_t)(snapshot - 1u) * VRAM_W * VRAM_H : S.upload_words;
    const uint32_t source_words = (64u << depth) * 256u + (depth == 0 ? 16u : depth == 1 ? 256u : 0u);
    uint32_t index = UINT32_MAX, replacement = UINT32_MAX;
    for (uint32_t i = 0u; i < S.decoded_count; ++i) {
        const int same = !memcmp(S.decoded[i].key, key, sizeof(S.decoded[i].key));
        if (S.persistent_decode && same && S.decoded[i].valid && S.decoded[i].source_words == source_words &&
            nvk_decoded_source(S.decoded[i].source, source, key, 0)) { index = i; break; }
        if (S.decoded[i].used != S.decoded_generation && (replacement == UINT32_MAX || same ||
            S.decoded[i].used < S.decoded[replacement].used)) replacement = i;
    }
    if (index != UINT32_MAX) {
        ++P.decoded_reuses;
        goto decoded_ready;
    }
    if (replacement != UINT32_MAX) index = replacement;
    else if (S.decoded_count < 64u) index = S.decoded_count++;
    else { ++P.decoded_slots_full; return VK_NULL_HANDLE; }
    S.decoded[index].valid = 0;
    if (S.persistent_decode) {
        if (S.decoded[index].source_words != source_words) {
            uint16_t *copy = realloc(S.decoded[index].source, source_words * sizeof(*copy));
            if (!copy) return VK_NULL_HANDLE;
            S.decoded[index].source = copy; S.decoded[index].source_words = source_words;
        }
        (void)nvk_decoded_source(S.decoded[index].source, source, key, 1);
    }
    VkCoreImage *image = S.decode_arrays ? &S.decoded_array : &S.decoded[index].image;
    VkCoreImage *color = S.decode_arrays ? &S.filtered_array : &S.decoded[index].filtered;
    nvk_pass_end();
    if (!image->image) {
        if (!vk_core_image_create(image, VK_FORMAT_R32_UINT, 256u, 256u, 1u,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "native decoded palette")) return VK_NULL_HANDLE;
        vk_core_image_barrier(R.cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (S.hardware_aniso && !color->image) {
        if (!vk_core_image_create(color, VK_FORMAT_R8G8B8A8_UNORM, 256u, 256u, 1u,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "native filtered palette")) return VK_NULL_HANDLE;
        vk_core_image_barrier(R.cmd, color->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    VkDescriptorSet set = nvk_set_alloc(S.dsl_decode);
    if (!set) return VK_NULL_HANDLE;
    VkWriteDescriptorSet writes[4];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set; writes[0].dstBinding = 0u; writes[0].descriptorCount = 1u;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; writes[0].pTexelBufferView = &R.recipe;
    const VkDescriptorImageInfo output = { VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL };
    writes[1] = nvk_write_image(set, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output);
    const VkDescriptorImageInfo filtered_output = { VK_NULL_HANDLE, color->view, VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo base_words = { S.sampler_clamp, S.words.view, VK_IMAGE_LAYOUT_GENERAL };
    if (S.hardware_aniso) {
        writes[2] = nvk_write_image(set, 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &filtered_output);
        writes[3] = nvk_write_image(set, 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &base_words);
    }
    vkUpdateDescriptorSets(vk_core_device(), S.hardware_aniso ? 4u : 2u, writes, 0u, NULL);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe_decode);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl_decode, 0u, 1u, &set, 0u, NULL);
    int32_t parameters[7];
    memcpy(parameters, key, sizeof(key)); parameters[6] = (int32_t)index;
    vkCmdPushConstants(R.cmd, S.pl_decode, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(parameters), parameters);
    vkCmdDispatch(R.cmd, 32u, 32u, 1u);
    nvk_barrier_all();
    memcpy(S.decoded[index].key, key, sizeof(S.decoded[index].key));
    S.decoded[index].valid = 1;
    ++P.decoded_pages;
decoded_ready:
    S.decoded_layer = index;
    *filtered = S.decode_arrays ? (S.hardware_aniso ? S.filtered_array.view : VK_NULL_HANDLE) : S.decoded[index].filtered.view;
    S.decoded[index].used = S.decoded_generation;
    memcpy(S.decoded_work[S.decoded_work_count].key, key, sizeof(key));
    S.decoded_work[S.decoded_work_count++].entry = index;
    if (S.decode_hints) S.decoded_work_hints[bucket] = (uint8_t)S.decoded_work_count;
    return S.decode_arrays ? S.decoded_array.view : S.decoded[index].image.view;
}

/* ---- draw / transfer commands (GL native_gpu_render_layer) --------------------------------------------------- */
static int nvk_depth_bounds_ensure(NvkPlane *plane, uint32_t scale)
{
    const uint32_t size = 8u * scale;
    const uint32_t columns = (plane->width + size - 1u) / size, rows = (plane->height + size - 1u) / size;
    if (plane->depth_max && plane->depth_columns == columns && plane->depth_rows == rows && plane->depth_scale == scale) return 1;
    free(plane->depth_max); plane->depth_max = NULL;
    if (!columns || !rows || (uint64_t)columns * rows > 1048576u) return 0;
    plane->depth_max = malloc((size_t)columns * rows * sizeof(*plane->depth_max));
    if (!plane->depth_max) return 0;
    memset(plane->depth_max, plane->depth_zero ? 0 : 255, (size_t)columns * rows * sizeof(*plane->depth_max));
    plane->depth_columns = columns; plane->depth_rows = rows; plane->depth_scale = scale;
    return 1;
}

static void nvk_depth_bounds_rect(NvkPlane *plane, uint32_t scale, int left, int top, int right, int bottom, int unknown)
{
    if (left < 0) left = 0; if (top < 0) top = 0;
    if (right > (int)plane->width) right = (int)plane->width;
    if (bottom > (int)plane->height) bottom = (int)plane->height;
    if (left >= right || top >= bottom) return;
    if (unknown) plane->depth_zero = 0;
    else if (!left && !top && right == (int)plane->width && bottom == (int)plane->height) plane->depth_zero = 1;
    if (!nvk_depth_bounds_ensure(plane, scale)) return;
    const int size = (int)scale * 8;
    for (int y = top / size; y <= (bottom - 1) / size; ++y) for (int x = left / size; x <= (right - 1) / size; ++x) {
        int x1 = (x + 1) * size, y1 = (y + 1) * size;
        if (x1 > (int)plane->width) x1 = plane->width;
        if (y1 > (int)plane->height) y1 = plane->height;
        if (unknown || (left <= x * size && top <= y * size && right >= x1 && bottom >= y1))
            plane->depth_max[(uint32_t)y * plane->depth_columns + (uint32_t)x] = unknown ? UINT32_MAX : 0u;
    }
}

static void nvk_depth_bounds_transfer(NvkPlane *plane, uint32_t scale, const GlNativeGpuCommand *command, int copy_depth)
{
    if (!S.depth_bounds || !R.work->depth_test || !plane->has_key || !command->plane) return;
    if (!copy_depth && ((command->mask & 2u) || command->kind == NATIVE_GPU_SPANS)) return;
    const int x0 = command->x * (int)scale, y0 = command->y * (int)scale;
    const int x1 = (command->x + command->w) * (int)scale, y1 = (command->y + command->h) * (int)scale;
    if (!R.damage_active) { nvk_depth_bounds_rect(plane, scale, x0, y0, x1, y1, copy_depth); return; }
    for (uint32_t i = 0u; i < R.damage_count; ++i) {
        const int32_t *r = R.damage_rects[i];
        nvk_depth_bounds_rect(plane, scale, x0 > r[0] ? x0 : r[0], y0 > r[1] ? y0 : r[1],
            x1 < r[0] + r[2] ? x1 : r[0] + r[2], y1 < r[1] + r[3] ? y1 : r[1] + r[3], copy_depth);
    }
}

static int nvk_depth_bounds_draw(NvkPlane *plane, uint32_t scale, const GlNativeGpuDrawGeometry *g)
{
    if (R.work->render_width || !S.depth_bounds || !R.work->depth_test || !plane->has_key || R.work->depth_view || R.work->wireframe) return 0;
    const GlNativeDepthMode *mode = &g->depth;
    if (!mode->test && mode->write != 4) return 0; /* far writes cannot raise an upper bound */
    const int left = g->left * (int)scale, top = g->top * (int)scale;
    const int right = (g->right + 1) * (int)scale, bottom = (g->bottom + 1) * (int)scale;
    const int available = nvk_depth_bounds_ensure(plane, scale);
    if ((mode->write == GL_NATIVE_DEPTH_WRITE && mode->test) || mode->write == 4) plane->depth_zero = 0;
    if (!available) return 0;
    const uint32_t size = 8u * scale;
    int passes = mode->test != 0;
    for (uint32_t y = (uint32_t)top / size; y <= (uint32_t)(bottom - 1) / size; ++y)
        for (uint32_t x = (uint32_t)left / size; x <= (uint32_t)(right - 1) / size; ++x) {
            if (x >= plane->depth_columns || y >= plane->depth_rows) { passes = 0; continue; }
            const int x0 = left > (int)(x * size) ? left : (int)(x * size);
            const int y0 = top > (int)(y * size) ? top : (int)(y * size);
            const int x1 = right < (int)((x + 1u) * size) ? right : (int)((x + 1u) * size);
            const int y1 = bottom < (int)((y + 1u) * size) ? bottom : (int)((y + 1u) * size);
            uint32_t low = 0u, high = UINT32_MAX;
            if (mode->test && !nvk_depth_interval(mode->plane.n0, mode->plane.a, mode->plane.b,
                    mode->plane.ox, mode->plane.oy, scale, x0, y0, x1, y1, &low, &high)) passes = 0;
            const uint32_t written = mode->write == GL_NATIVE_DEPTH_WRITE && mode->test ? high : mode->write == 4 ? 1u : 0u;
            uint32_t *bound = &plane->depth_max[y * plane->depth_columns + x];
            if (low < native_depth_threshold(*bound)) passes = 0;
            if (written > *bound) *bound = written;
        }
    return passes;
}

static int nvk_render_layer(const GlNativeGpuCommand *command, const uint8_t *captured_data,
                            uint32_t first_vertex, const float (*slice_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS],
                            const GlNativeGpuDrawGeometry *prepared, const uint16_t *layers, uint32_t layer,
                            const uint32_t *selection, uint32_t selection_count, const uint8_t *depth_fast)
{
    const GlNativeGpuWork *work = R.work;
    NvkPlane *plane = &R.planes[command->plane];
    const uint32_t scale = native_gpu_plane_scale(work, command->plane);
    const int wire = work->wireframe && command->plane != 0u;
    if (command->kind == NATIVE_GPU_DRAW) {
        const XgSemanticDrawRecord *draw = &command->draw;
        const int lines = draw->topology == GPU_RENDER_SEMANTIC_LINES;
        const int filter_strength = draw->sprite_texture ? 100 : R.filter_strength;
        const float strength_value = filter_strength / 100.f;
        const int anisotropy_option = command->plane != 0u && !lines && !draw->sprite_texture &&
            draw->screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE ? R.anisotropy : 0;
        int material_valid = 0;
        GlNativeTextureState shared_texture = {0};
        XgRenderIrMaterialState material;
        const GlNativeGpuMeshTriangle *mesh_triangles = command->mesh_count
            ? (const GlNativeGpuMeshTriangle *)(const void *)(captured_data + command->data) : NULL;
        /* Interlock tier: a command without replacement textures starts from zeroed uniforms and its triangles go
         * through the batch; the loose state of the other draws is kept aside (GL native_gpu_parameter_capture). */
        const int batched = (S.interlock || (S.core_batch && R.scheduled_run && !lines)) &&
            !wire && (!draw->hd_texture.valid || (S.core_batch && S.hd_batch && !R.capture_active)) && !command->source;
        NvkView saved_view = R.v;
        NvkDraw saved_draw = R.d;
        if (batched) { memset(&R.v, 0, sizeof(R.v)); memset(&R.d, 0, sizeof(R.d)); }
        R.batchable = batched;
        const uint32_t triangle_count = selection ? selection_count : lines ? draw->line_count : native_gpu_command_triangles(command);
        for (uint32_t selected = 0u; selected < triangle_count; ++selected) {
            const uint32_t t = selection ? selection[selected] : selected;
            if (layers && layers[t] != layer) continue;
            const XgRenderIrMaterialState *m = &draw->primitive.material;
            if (mesh_triangles) {
                const GlNativeTextureState *texture = &mesh_triangles[t].texture;
                if (!material_valid || !native_texture_state_equal(texture, &shared_texture)) {
                    material = draw->primitive.material;
                    native_texture_state_apply(&material, texture);
                    shared_texture = *texture;
                    material_valid = 1;
                }
                m = &material;
            }
            int anisotropy_value = m->textured ? anisotropy_option : 0;
            GlNativeGpuDrawGeometry local;
            const GlNativeGpuDrawGeometry *geometry = prepared ? &prepared[t] : &local;
            if (!prepared) native_gpu_prepare_draw(work, command, captured_data, t, plane->width / scale,
                plane->height / scale, slice_vertices + first_vertex + t * (lines ? 6u : 3u), &local);
            if (!geometry->visible) continue;
            if (!nvk_damage_intersects(geometry->left * (int)scale, geometry->top * (int)scale,
                    (geometry->right + 1) * (int)scale, (geometry->bottom + 1) * (int)scale)) continue;
            const GlNativeAttributePlanes *attributes = &geometry->attributes;
            if (S.aniso_single_tap && !TC.active && batched && anisotropy_value > 1 && slice_vertices) {
                const float (*v)[GL_NATIVE_GPU_VERTEX_FLOATS] = slice_vertices + first_vertex + t * 3u;
                if (v[0][GL_NATIVE_GPU_VERTEX_WEIGHT] == 0.f && v[1][GL_NATIVE_GPU_VERTEX_WEIGHT] == 0.f &&
                    v[2][GL_NATIVE_GPU_VERTEX_WEIGHT] == 0.f) {
                    const float ax = (float)(attributes->plane[0][1] / scale), ay = (float)(attributes->plane[1][1] / scale);
                    const float bx = (float)(attributes->plane[0][2] / scale), by = (float)(attributes->plane[1][2] / scale);
                    /* The major eigenvalue cannot exceed the trace. A margin
                     * below one covers float rounding in the vertex shader:
                     * these affine triangles provably never take extra taps.
                     * Specializing them avoids the loop's register footprint. */
                    const double xx = (double)ax*ax + (double)ay*ay;
                    const double yy = (double)bx*bx + (double)by*by;
                    const double trace = xx + yy;
                    double upper = trace;
                    if (S.aniso_single_tap == 2 && trace >= 0.98 && trace < 1.96) {
                        /* The trace bound can reject a magnified footprint
                         * with two similar axes. Use its major eigenvalue
                         * only in that narrow ambiguous range; retain the
                         * same conservative margin and affine-only guard. */
                        const double xy = (double)ax*bx + (double)ay*by;
                        upper = 0.5 * (trace + sqrt((xx-yy)*(xx-yy) + 4.0*xy*xy));
                    }
                    if (upper < 0.98) { anisotropy_value = 0; ++P.aniso_single_tap_triangles; }
                }
            }
            const int uv_gradient = !lines && (anisotropy_value > 1);
            const GlNativeDepthMode *depth_mode = &geometry->depth;
            const int left = geometry->left, top = geometry->top;
            const int right = geometry->right, bottom = geometry->bottom;
            const int *draw_scissor = geometry->scissor;
            unsigned depth = nvk_depth_uniforms(plane, depth_mode, command->plane != 0u);
            const int bounded = command->plane != 0u ? nvk_depth_bounds_draw(plane, scale, geometry) : 0;
            if (depth_mode->test && (depth_fast ? depth_fast[t] : !R.scheduled_run && bounded)) {
                R.d.depth_state[0] = 2; /* compute/write the key; comparison was proven */
                if (!depth_mode->semi) { depth &= ~NVK_UNIFORM_DEPTH_READ; ++P.depth_reads_elided; }
            }
            const int tested = (depth & NVK_UNIFORM_DEPTH_ATTACHED) && depth_mode->test;
            /* A DRAW may sample another plane of the work as its texture (hd_on 2) or a
             * pack replacement (hd_on 1). The replacement upload is recorded before the
             * draw's render pass opens. */
            const int snapshot = !wire && command->source && command->source < GL_NATIVE_GPU_PHASE_BASE &&
                R.planes[command->source].color.image;
            const NvkHd *hd_image = !wire && !snapshot && command->plane != 0u && m->textured && draw->hd_texture.valid
                ? nvk_hd_lookup(&draw->hd_texture) : NULL;
            VkImageView filtered = VK_NULL_HANDLE;
            const VkImageView decoded = !command->texture_snapshot && (anisotropy_value <= 1 || snapshot || hd_image)
                ? VK_NULL_HANDLE : nvk_decoded_page(m, command->texture_snapshot, &filtered);
            R.key.filtered = filtered;
            const uint32_t aniso_index = anisotropy_value >= 16 ? 4u : anisotropy_value >= 8 ? 3u : anisotropy_value >= 4 ? 2u : anisotropy_value >= 2 ? 1u : 0u;
            R.key.filtered_sampler = filtered ? S.sampler_aniso[aniso_index] : VK_NULL_HANDLE;
            R.d.decoded_on = decoded ? (S.decode_arrays ? 2 : 1) : 0;
            R.key.decoded = decoded ? decoded : S.dummy_uint.view;
            R.c.depth_tested_triangles += tested ? 1u : 0u;
            if (!TC.active && !nvk_target(plane, scale, left, top, right - left + 1, bottom - top + 1,
                    !wire && (m->semi_transparent || m->mask_check), depth, draw_scissor,
                    slice_vertices ? slice_vertices + first_vertex + t * (lines ? 6u : 3u) : NULL,
                    lines ? 6u : 3u, 0.5f / scale - 1.f / 64.f)) return 0;
            if (m->textured && !lines)
                nvk_set4(R.d.sample_lim, attributes->limits[0], attributes->limits[1], attributes->limits[2], attributes->limits[3]);
            nvk_setf4(R.d.attribute_origin, lines ? 0.f : (float)attributes->x, lines ? 0.f : (float)attributes->y,
                lines ? 0.f : attributes->integral ? 1.f : 2.f,
                native_gpu_command_flags(command, captured_data, t).gouraud ? 0.f : 1.f);
            if (!lines) nvk_attribute_uniforms(attributes);
            if (uv_gradient)
                nvk_setf4(R.d.uv_gradient, (float)(attributes->plane[0][1] / scale) * plane->raster[0], (float)(attributes->plane[1][1] / scale) * plane->raster[0],
                    (float)(attributes->plane[0][2] / scale) * plane->raster[1], (float)(attributes->plane[1][2] / scale) * plane->raster[1]);
            nvk_setf4(R.v.size, (float)plane->width / scale, (float)plane->height / scale, 0.5f / scale - 1.f / 64.f, (float)scale);
            nvk_set4(R.d.state, m->textured, m->raw_texture, m->semi_transparent, m->blend_mode);
            R.d.aa_exempt = draw->aa_exempt || draw->screen_space_2d != GPU_RENDER_SCREEN_SPACE_2D_NONE;
            R.d.texture_filter = command->plane != 0u && filter_strength > 0 &&
                (draw->sprite_texture ? R.sprite_filter : R.texture_filter);
            R.d.filter_strength = strength_value;
            R.d.anisotropy = anisotropy_value;
            if (wire) {
                /* PSX line primitives already arrive as thin quads; only
                 * triangle primitives need the shader's edge-only coverage. */
                nvk_set4(R.d.flags, 0, 0, 0, lines ? 1 : 3);
                nvk_setf4(R.d.constant_color, 0.85f, 1.f, 0.85f, 0.f);
            } else nvk_set4(R.d.flags, m->mask_set, m->mask_check, m->dither, 0);
            nvk_set4(R.d.page, decoded && S.decode_arrays ? (int)S.decoded_layer : m->texture_page_x * 64,
                     decoded && S.decode_arrays ? 0 : m->texture_page_y * 256, m->clut_x, m->clut_y);
            nvk_set4(R.d.window, m->texture_window_mask_x, m->texture_window_mask_y,
                     m->texture_window_offset_x, m->texture_window_offset_y);
            R.key.hd = snapshot ? R.planes[command->source].color.view : hd_image ? hd_image->image.view : S.dummy_color.view;
            R.key.hd_sampler = hd_image ? S.sampler_hd : S.sampler_clamp;
            R.d.hd_on = snapshot ? 2 : hd_image ? 1 : 0;
            nvk_source_raster(snapshot ? &R.planes[command->source] : NULL);
            if (snapshot)
                nvk_set4(R.d.hd_map, (int)command->sx, (int)command->sy, (int)command->sw, (int)command->sh);
            else if (hd_image) {
                const GpuRenderHdTexture *hd = &draw->hd_texture;
                nvk_set4(R.d.hd_map, hd->texel_offset_u, hd->texel_offset_v, hd->texel_width, hd->texel_height);
                nvk_set4(R.d.hd_lim, hd->lim[0], hd->lim[1], hd->lim[2], hd->lim[3]);
            }
            R.d.mip_on = 0;
            R.key.src = S.dummy_color.view;
            R.key.src_key = S.dummy_uint.view;
            R.key.src_sampler = S.sampler_clamp;
            R.d.u_depth = m->texture_depth | (command->texture_snapshot << 2u);
            R.d.origin[0] = command->x; R.d.origin[1] = command->y;
            if (!nvk_draw(plane, (depth & NVK_UNIFORM_DEPTH_ATTACHED) != 0, s_scissor_value,
                          first_vertex + t * (lines ? 6u : 3u), lines ? 6u : 3u)) return 0;
            R.c.geometry_draws++;
            if (m->textured && command->plane != 0u)
                ((NvkCounters *)&R.c)->depth_tested_triangles += 0u;
        }
        if (batched) { R.v = saved_view; R.d = saved_draw; }
        R.batchable = 0;
        return 1;
    }
    /* Transfers: seeds, fills and copies of a source plane. */
    R.d.decoded_on = 0;
    R.key.decoded = S.dummy_uint.view;
    R.key.filtered = VK_NULL_HANDLE; R.key.filtered_sampler = VK_NULL_HANDLE;
    VkImageView texture = VK_NULL_HANDLE;
    VkSampler sampler = S.sampler_clamp;
    if (command->kind == NATIVE_GPU_SEED) {
        if (!nvk_seed(command, captured_data, &texture)) return 0;
        sampler = S.sampler_repeat;
    }
    const NvkPlane *source_plane = NULL;
    if (command->kind != NATIVE_GPU_SEED && command->source != UINT32_MAX) {
        if (command->source >= GL_NATIVE_GPU_BASE_SOURCE) {
            const uint32_t slot = command->source - GL_NATIVE_GPU_BASE_SOURCE;
            if (slot >= GL_NATIVE_VIEW_TARGET_CAPACITY) return 0;
            /* A target without storage yet had nothing drawn: its base is the cleared plane. */
            source_plane = S.bases[slot].color.image ? &S.bases[slot] : NULL;
        } else {
            source_plane = &R.snapshots[command->source];
            if (!source_plane->color.image) return 0;
        }
        if (source_plane) texture = source_plane->color.view;
    }
    /* Transfers write far depth, except full-row VIEW copies (mask bit 2),
     * which carry their source rows' keys. */
    const int copy_depth = (command->mask & 4u) && texture &&
        (command->kind == NATIVE_GPU_SPAN || command->kind == NATIVE_GPU_SPANS) &&
        source_plane && source_plane->has_key;
    const GlNativeDepthMode transfer_depth = {0, copy_depth ? 3 : GL_NATIVE_DEPTH_RESET, 0, 0,
        GL_NATIVE_DEPTH_SHOW_NONE, {0}};
    const unsigned depth = nvk_depth_uniforms(plane, &transfer_depth, command->plane != 0u);
    nvk_depth_bounds_transfer(plane, scale, command, copy_depth);
    R.key.src_key = copy_depth ? source_plane->key.view : S.dummy_uint.view;
    if (!nvk_target(plane, scale, command->x, command->y, command->w, command->h, (command->mask & 2u) != 0,
                    depth, NULL, NULL, 0, 0.f)) return 0;
    if (!work->render_width && S.clear_spans && !S.interlock && !R.capture_active && !wire && !work->depth_view &&
        command->kind == NATIVE_GPU_SPAN && command->source == UINT32_MAX && !(command->mask & 2u))
        return nvk_clear_span(plane, command, depth);
    /* Guest pixels are black in wireframe; copies between VIEW planes keep
     * the previously drawn edges, just as on the GL debug path. */
    const int black = wire && (!texture || command->kind == NATIVE_GPU_SEED || command->source == 0u);
    nvk_source_raster(source_plane);
    R.key.src = texture && !black ? texture : S.dummy_color.view;
    R.key.src_sampler = sampler;
    R.key.hd = S.dummy_color.view;
    R.key.hd_sampler = S.sampler_clamp;
    nvk_setf4(R.v.size, (float)plane->width / scale, (float)plane->height / scale, 0.f, (float)scale);
    nvk_set4(R.d.flags, command->mask & 1u, (command->mask >> 1u) & 1u,
             command->kind == NATIVE_GPU_SEED ? 1 : 0, texture && !black ? 2 : 1);
    nvk_setf4(R.d.constant_color, black ? 0.f : (command->color & 255u) / 255.f,
              black ? 0.f : ((command->color >> 8u) & 255u) / 255.f,
              black ? 0.f : ((command->color >> 16u) & 255u) / 255.f, (command->color >> 24u) / 255.f);
    /* hd_on, mip_on, u_depth and origin keep their last draw's values, like the GL loose uniforms. */
    if (!nvk_draw(plane, (depth & NVK_UNIFORM_DEPTH_ATTACHED) != 0, s_scissor_value, first_vertex,
                  native_gpu_command_vertex_count(command))) return 0;
    R.c.transfer_draws++;
    return 1;
}

static int nvk_render_command(const GlNativeGpuCommand *command, const uint8_t *captured_data,
                              uint32_t first_vertex, const float (*slice_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS],
                              const GlNativeGpuDrawGeometry *prepared)
{
    return nvk_render_layer(command, captured_data, first_vertex, slice_vertices, prepared, NULL, 0u, NULL, 0u, NULL);
}

/* ---- layer scheduling (GL native_gpu_schedule_run) -------------------------------------------------------------- */
typedef struct NvkSchedCommand { uint32_t base, vertex; uint16_t lo, hi; const GlNativeGpuDrawGeometry *prepared; } NvkSchedCommand;
static NvkSchedCommand s_sched_cmd[NATIVE_SCHED_CMDS];
static uint16_t *s_sched_max_layer;
static uint32_t s_sched_max_capacity;
static uint32_t *s_sched_emission;
static uint32_t s_sched_emission_capacity;
typedef struct NvkMaterialOrder { uint64_t key, hd_key; uint32_t command, triangle; } NvkMaterialOrder;
static NvkMaterialOrder *s_sched_order;
static uint32_t s_sched_order_capacity;
static uint8_t *s_sched_depth_fast;
static uint32_t s_sched_depth_capacity;
static int nvk_material_order_compare(const void *left, const void *right)
{
    const NvkMaterialOrder *a = left, *b = right;
    if (a->hd_key != b->hd_key) return a->hd_key < b->hd_key ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    if (a->command != b->command) return a->command < b->command ? -1 : 1;
    return a->triangle < b->triangle ? -1 : a->triangle != b->triangle;
}
/* Template geometry/materials are immutable for all phases of one work. Cache
 * only their dependency plan: the leading destination hazard is still tested
 * against each phase's actual preceding writes at emission time. */
typedef struct NvkScheduleCache {
    uint32_t first, end, width, height, scale, prim_count, shape_count, run_max, ncmd, total;
    int has_key, valid;
    GlNativeSchedPrim *primitives;
    GlNativeGpuShape *shapes;
    uint16_t *layers;
    NvkSchedCommand *commands;
} NvkScheduleCache;
static NvkScheduleCache s_template_schedules[64];
static uint32_t s_template_schedule_count;
static uint64_t s_template_schedule_hits;

static void nvk_schedule_cache_clear(void)
{
    for (uint32_t i = 0u; i < s_template_schedule_count; ++i) {
        NvkScheduleCache *cache = &s_template_schedules[i];
        free(cache->primitives); free(cache->shapes); free(cache->layers); free(cache->commands);
        memset(cache, 0, sizeof(*cache));
    }
    s_template_schedule_count = 0u;
}

static int nvk_sched_reserve(void **p, uint32_t *cap, uint32_t need, size_t size)
{
    if (need <= *cap) return 1;
    uint32_t n = *cap ? *cap : 256u;
    while (n < need) n *= 2u;
    void *q = realloc(*p, (size_t)n * size);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

static int nvk_depth_reads(const NvkPlane *plane, const GlNativeDepthMode *mode, int view_plane)
{
    return R.work->depth_test && plane->has_key && view_plane && (mode->test || mode->semi);
}

static int nvk_capture_reserve(const GlNativeGpuWork *work)
{
    if (!S.capture_enabled || !work->scanout) return 0;
    const uint64_t pixels = (uint64_t)work->scanout_width * work->scanout_height * work->scale * work->scale;
    if (!pixels || pixels > 4u * 1024u * 1024u) return 0;
    const VkDeviceSize bytes = (4u + 3u * pixels + (pixels + 255u) / 256u) * sizeof(uint32_t);
    if (bytes > vk_core_caps()->max_storage_buffer_range) return 0;
    if (S.capture_meta.buffer && S.capture_meta.size < bytes) vk_core_buffer_destroy(&S.capture_meta);
    if (!S.capture_meta.buffer && !vk_core_buffer_create(&S.capture_meta, bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_CORE_MEMORY_DEVICE, "native fragment metadata")) return 0;
    if (!S.capture_nodes.buffer) {
        VkDeviceSize capacity = vk_core_caps()->max_storage_buffer_range;
        if (capacity > (128u << 20u)) capacity = 128u << 20u;
        capacity &= ~(VkDeviceSize)15u;
        if (!capacity || !vk_core_buffer_create(&S.capture_nodes, capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_CORE_MEMORY_DEVICE, "native ordered fragments")) return 0;
        S.capture_capacity = (uint32_t)(capacity / 16u);
    }
    if (!S.capture_readback.buffer && !vk_core_buffer_create(&S.capture_readback, 4u,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_CORE_MEMORY_READBACK, "native fragment overflow")) return 0;
    return 1;
}

static void nvk_capture_dispatch(uint32_t stage, uint32_t groups)
{
    R.capture_push.info[0] = stage;
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe_capture_resolve);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl_capture, 0u, 1u, &R.capture_set, 0u, NULL);
    vkCmdPushConstants(R.cmd, S.pl_capture, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        0u, sizeof(R.capture_push), &R.capture_push);
    vkCmdDispatch(R.cmd, groups, 1u, 1u);
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0u, 1u, &barrier, 0u, NULL, 0u, NULL);
}

static int nvk_capture_bind(NvkPlane *plane, uint32_t scale, const int32_t rect[4],
                            VkBuffer meta, VkBuffer nodes, uint32_t capacity)
{
    R.capture_set = nvk_set_alloc(S.dsl_capture);
    if (!R.capture_set) return 0;
    const VkDescriptorBufferInfo buffers[2] = {
        { meta, 0u, VK_WHOLE_SIZE }, { nodes, 0u, VK_WHOLE_SIZE }
    };
    const VkDescriptorImageInfo images[2] = {
        { VK_NULL_HANDLE, plane->color.view, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, plane->key.view, VK_IMAGE_LAYOUT_GENERAL }
    };
    VkWriteDescriptorSet writes[4];
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0u; i < 2u; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = R.capture_set; writes[i].dstBinding = i; writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffers[i];
        writes[i+2u] = nvk_write_image(R.capture_set, i+2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[i]);
    }
    vkUpdateDescriptorSets(vk_core_device(), 4u, writes, 0u, NULL);
    const float view[4] = { (float)plane->width / scale, (float)plane->height / scale, 0.5f / scale - 1.f / 64.f, (float)scale };
    memcpy(R.capture_push.view, view, sizeof(view));
    memcpy(R.capture_push.rect, rect, sizeof(R.capture_push.rect));
    R.capture_push.info[0] = 0u; R.capture_push.info[1] = capacity;
    R.capture_push.info[2] = 0u; R.capture_push.info[3] = R.work->depth_test && plane->has_key;
    return 1;
}

static int nvk_capture_begin(NvkPlane *plane, uint32_t scale, int x, int y, uint32_t width, uint32_t height)
{
    P.capture_full_pixels += (uint64_t)width * height;
    const char *clip = getenv("PSX_NATIVE_VK_CAPTURE_DAMAGE_BOUNDS");
    if (clip && clip[0] == '1' && S.linked_capture && R.damage_active &&
        plane == &R.planes[R.damage_plane] && R.damage_count) {
        int left = x + (int)width, top = y + (int)height, right = x, bottom = y;
        for (uint32_t i = 0u; i < R.damage_count; ++i) {
            const int32_t *r = R.damage_rects[i];
            if (left > r[0]) left = r[0];
            if (top > r[1]) top = r[1];
            if (right < r[0] + r[2]) right = r[0] + r[2];
            if (bottom < r[1] + r[3]) bottom = r[1] + r[3];
        }
        if (left < x) left = x;
        if (top < y) top = y;
        if (right > x + (int)width) right = x + (int)width;
        if (bottom > y + (int)height) bottom = y + (int)height;
        /* Outside this union the fragment shader already discards every
         * invocation. Its heads/counts and the resolve need no storage there.
         * Keep a harmless one-pixel target for an entirely untouched group. */
        if (left >= right || top >= bottom) { width = height = 1u; }
        else { x = left; y = top; width = (uint32_t)(right - left); height = (uint32_t)(bottom - top); }
    }
    P.capture_pixels += (uint64_t)width * height;
    const uint32_t pixels = width * height;
    const uint32_t tile = 1u << S.damage_tile_shift;
    nvk_pass_end();
    if (P.capture_profile && P.capture_queries < NVK_CAPTURE_TS) nvk_prof_stamp(NVK_TS + P.capture_queries);
    R.capture_damage_mask = 0;
    uint32_t damage_columns = 0u;
    if (S.linked_capture && R.damage_active) {
        const uint32_t columns = (plane->width + tile - 1u) / tile, rows = (plane->height + tile - 1u) / tile;
        const VkDeviceSize bytes = ((columns * rows + 31u) / 32u) * sizeof(uint32_t);
        const VkDeviceSize destination = (4u + (VkDeviceSize)pixels * 2u) * sizeof(uint32_t);
        if (destination + bytes <= S.capture_meta.size) {
            uint32_t chunk; VkDeviceSize offset;
            /* Upload arenas may be write-combined device-local memory.
             * Build bits in cached RAM: read/modify/write on the mapped
             * upload pointer can incur uncached device-memory reads. */
            uint32_t mask[2048];
            if (bytes > sizeof(mask)) return 0;
            memset(mask, 0, (size_t)bytes);
            void *upload = nvk_arena(bytes, 16u, &chunk, &offset);
            if (!upload) return 0;
            for (uint32_t i = 0u; i < R.damage_count; ++i) {
                const int32_t *r = R.damage_rects[i];
                const uint32_t left = (uint32_t)r[0] >> S.damage_tile_shift;
                const uint32_t right = (uint32_t)(r[0] + r[2] - 1) >> S.damage_tile_shift;
                const uint32_t top = (uint32_t)r[1] >> S.damage_tile_shift;
                const uint32_t bottom = (uint32_t)(r[1] + r[3] - 1) >> S.damage_tile_shift;
                for (uint32_t ty = top; ty <= bottom; ++ty)
                    for (uint32_t tx = left; tx <= right; ++tx) {
                        const uint32_t bit = ty * columns + tx;
                        mask[bit / 32u] |= 1u << (bit % 32u);
                    }
            }
            memcpy(upload, mask, (size_t)bytes);
            const VkBufferCopy copy = { offset, destination, bytes };
            vkCmdCopyBuffer(R.cmd, S.chunks[chunk].buf.buffer, S.capture_meta.buffer, 1u, &copy);
            damage_columns = columns | (S.damage_tile_shift << 16u);
            R.capture_damage_mask = 1;
        }
    }
    if (!R.capture_used) {
        const uint32_t header[4] = { 0u, 0u, S.capture_capacity, damage_columns };
        vkCmdUpdateBuffer(R.cmd, S.capture_meta.buffer, 0u, sizeof(header), header);
        R.capture_used = 1;
    } else if (S.linked_capture) {
        vkCmdFillBuffer(R.cmd, S.capture_meta.buffer, 4u, 4u, 0u);
        vkCmdUpdateBuffer(R.cmd, S.capture_meta.buffer, 12u, sizeof(damage_columns), &damage_columns);
    }
    if (S.linked_capture) {
        vkCmdFillBuffer(R.cmd, S.capture_meta.buffer, 16u, (VkDeviceSize)pixels * 4u, UINT32_MAX);
        vkCmdFillBuffer(R.cmd, S.capture_meta.buffer, 16u + (VkDeviceSize)pixels * 4u, (VkDeviceSize)pixels * 4u, 0u);
    } else vkCmdFillBuffer(R.cmd, S.capture_meta.buffer, 16u, (VkDeviceSize)pixels * 8u, 0u);
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 1u, &barrier, 0u, NULL, 0u, NULL);
    const int32_t rect[4] = { x, y, (int32_t)width, (int32_t)height };
    if (!nvk_capture_bind(plane, scale, rect, S.capture_meta.buffer, S.capture_nodes.buffer, S.capture_capacity)) return 0;
    R.capture_order = 0u; R.capture_reserved = 0u; R.capture_active = 1;
    if (P.capture_profile && P.capture_queries < NVK_CAPTURE_TS) nvk_prof_stamp(NVK_TS + P.capture_queries + 1u);
    return 1;
}

static void nvk_capture_finish(NvkPlane *plane, int ok, uint32_t triangles)
{
    nvk_pass_end();
    if (P.capture_profile && P.capture_queries < NVK_CAPTURE_TS) nvk_prof_stamp(NVK_TS + P.capture_queries + 2u);
    R.scheduled_run = 0; R.capture_active = 0;
    const uint32_t pixels = (uint32_t)R.capture_push.rect[2] * (uint32_t)R.capture_push.rect[3];
    if (ok) nvk_capture_dispatch(2u, (pixels + 127u) / 128u);
    if (P.capture_profile && P.capture_queries < NVK_CAPTURE_TS) {
        nvk_prof_stamp(NVK_TS + P.capture_queries + 3u);
        P.capture_queries += 4u;
    }
    nvk_plane_barrier_clear(plane);
    ++P.capture_runs; P.capture_triangles += triangles;
    R.c.scheduled_layers++;
}

/* A bounded two-pass fragment list. Counting allocates exact per-pixel spans;
 * capture stores source colours/keys and resolve applies the original FIFO
 * integer rules. Overflow causes a whole phase replay before publication. */
static int nvk_capture_run(unsigned plane_index, const GlNativeGpuCommand *commands, uint32_t first, uint32_t end,
                            uint32_t first_vertex, const uint8_t *data,
                            const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], const GlNativeGpuGeometry *geometry,
                            uint32_t triangles)
{
    if (R.capture_eligible != 1 || R.capture_active || (R.damage_active && !S.linked_capture) || triangles < 32u ||
        plane_index < GL_NATIVE_GPU_PHASE_BASE || !geometry || !geometry->primitives) return -1;
    uint32_t count = 0u;
    for (uint32_t i = first; i < end; ++i) {
        const GlNativeGpuCommand *c = &commands[i];
        if (c->plane != plane_index || c->kind != NATIVE_GPU_DRAW || c->source || c->draw.hd_texture.valid ||
            c->draw.topology != GPU_RENDER_SEMANTIC_TRIANGLES) return -1;
        count += native_gpu_command_vertex_count(c);
    }
    NvkPlane *plane = &R.planes[plane_index];
    if (!plane->capture_framebuffer) return -1;
    const uint32_t scale = native_gpu_plane_scale(R.work, plane_index);
    int left = (int)plane->width, top = (int)plane->height, right = 0, bottom = 0;
    uint32_t visible = 0u, readers = 0u;
    for (uint32_t i = first; i < end; ++i) {
        const uint32_t begin = geometry->offsets[i].primitive, limit = geometry->offsets[i + 1u].primitive;
        for (uint32_t t = begin; t < limit; ++t) {
            const GlNativeGpuDrawGeometry *g = &geometry->primitives[t];
            if (!g->visible) continue;
            ++visible;
            readers += (R.work->depth_test && g->depth.test) || g->depth.semi || commands[i].draw.primitive.material.mask_check;
            if (g->left * (int)scale < left) left = g->left * (int)scale;
            if (g->top * (int)scale < top) top = g->top * (int)scale;
            if ((g->right + 1) * (int)scale > right) right = (g->right + 1) * (int)scale;
            if ((g->bottom + 1) * (int)scale > bottom) bottom = (g->bottom + 1) * (int)scale;
        }
    }
    const int crop_x = R.work->phase_crop_x * scale, crop_y = R.work->phase_crop_y * scale;
    if (left < crop_x) left = crop_x; if (top < crop_y) top = crop_y;
    if (right > crop_x + (int)(R.work->scanout_width * scale)) right = crop_x + R.work->scanout_width * scale;
    if (bottom > crop_y + (int)(R.work->scanout_height * scale)) bottom = crop_y + R.work->scanout_height * scale;
    if (left >= right || top >= bottom) return 1;
    const uint32_t width = (uint32_t)(right - left), height = (uint32_t)(bottom - top);
    const uint32_t pixels = width * height;
    /* Do not scan a mostly empty viewport for a handful of scattered props;
     * dense actor/terrain runs benefit even below the old 128-triangle cutoff. */
    if (visible < 32u || !readers || (uint64_t)pixels > (uint64_t)visible * 2048u) return -1;
    if (S.direct_capture) {
        uint64_t reserved = 0u;
        for (uint32_t i = first; i < end; ++i)
            for (uint32_t t = geometry->offsets[i].primitive; t < geometry->offsets[i+1u].primitive; ++t) {
                const GlNativeGpuDrawGeometry *g = &geometry->primitives[t];
                if (!g->visible) continue;
                int x0 = g->left * (int)scale, y0 = g->top * (int)scale;
                int x1 = (g->right + 1) * (int)scale, y1 = (g->bottom + 1) * (int)scale;
                if (x0 < left) x0 = left; if (y0 < top) y0 = top;
                if (x1 > right) x1 = right; if (y1 > bottom) y1 = bottom;
                if (x1 > x0 && y1 > y0) reserved += (uint64_t)(x1 - x0) * (uint32_t)(y1 - y0);
            }
        if (reserved > S.capture_capacity) return -1;
    }
    const int x = left, y = top;
    if ((uint64_t)x + width > plane->width || (uint64_t)y + height > plane->height) return -1;
    if (!nvk_capture_begin(plane, scale, x, y, width, height)) return 0;
    if (!S.linked_capture) {
    if (!nvk_require_vertices()) return 0;
    nvk_pass_begin(plane);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe_capture_count);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_capture, 0u, 1u, &R.capture_set, 0u, NULL);
    vkCmdPushConstants(R.cmd, S.pl_capture, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        0u, sizeof(R.capture_push), &R.capture_push);
    vkCmdBindVertexBuffers(R.cmd, 0u, 1u, &R.vsrc.buf, &R.vsrc.off);
    const VkRect2D scissor = { { x, y }, { width, height } };
    vkCmdSetScissor(R.cmd, 0u, 1u, &scissor);
    ++P.draw_calls; vkCmdDraw(R.cmd, count, 1u, first_vertex, 0u);
    nvk_pass_end();
    nvk_capture_dispatch(0u, (pixels + 255u) / 256u);
    nvk_capture_dispatch(1u, 1u);
    }
    R.capture_order = 0u;
    R.scheduled_run = 1;
    int ok = 1;
    uint32_t vertex = first_vertex;
    for (uint32_t i = first; i < end && ok; ++i) {
        ok = nvk_render_command(&commands[i], data, vertex, vertices,
            geometry->primitives + geometry->offsets[i].primitive);
        vertex += native_gpu_command_vertex_count(&commands[i]);
    }
    nvk_capture_finish(plane, ok, triangles);
    return ok;
}

/* 1 rendered, 0 failed, -1 not applicable (nothing was drawn). */
static int nvk_schedule_run_impl(unsigned plane_index, const GlNativeGpuCommand *commands, uint32_t first, uint32_t end,
                            uint32_t first_vertex, const uint8_t *captured_data,
                             const float (*slice_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS],
                             const GlNativeGpuGeometry *geometry, NvkScheduleCache *cached, int optimize_depth);

static int nvk_schedule_run(unsigned plane_index, const GlNativeGpuCommand *commands, uint32_t first, uint32_t end,
                            uint32_t first_vertex, const uint8_t *captured_data,
                             const float (*slice_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS],
                             const GlNativeGpuGeometry *geometry, NvkScheduleCache *cached, int optimize_depth)
{
    uint32_t kind = 1u;
    if (P.capture_profile)
        for (uint32_t i = first; i < end; ++i) if (commands[i].draw.hd_texture.valid) { kind = 3u; break; }
    const uint32_t query = nvk_detail_begin(kind);
    const int result = nvk_schedule_run_impl(plane_index, commands, first, end, first_vertex, captured_data, slice_vertices, geometry, cached, optimize_depth);
    nvk_detail_end(query);
    return result;
}

static int nvk_schedule_run_impl(unsigned plane_index, const GlNativeGpuCommand *commands, uint32_t first, uint32_t end,
                            uint32_t first_vertex, const uint8_t *captured_data,
                             const float (*slice_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS],
                             const GlNativeGpuGeometry *geometry, NvkScheduleCache *cached, int optimize_depth)
{
    if (S.interlock || R.work->wireframe) return -1;   /* debug edges keep command order */
    if (R.capture_active) {
        uint32_t vertex = first_vertex;
        int ok = 1;
        for (uint32_t i = first; i < end && ok; ++i) {
            if (commands[i].plane == plane_index)
                ok = nvk_render_command(&commands[i], captured_data, vertex, slice_vertices,
                    geometry && geometry->primitives ? geometry->primitives + geometry->offsets[i].primitive : NULL);
            vertex += native_gpu_command_vertex_count(&commands[i]);
        }
        return ok;
    }
    const GlNativeGpuWork *work = R.work;
    NvkPlane *plane = &R.planes[plane_index];
    const uint32_t scale = native_gpu_plane_scale(work, plane_index);
    NativeSchedState *sched = &s_sched; /* native_sched_query reads this global; GL runs on this thread too */
    if (!slice_vertices || !plane->color.image || !scale || end - first > NATIVE_SCHED_CMDS) return -1;
    const uint32_t cell_size = NATIVE_SCHED_CELL * scale;
    const uint32_t columns = (plane->width + cell_size - 1u) / cell_size;
    const uint32_t rows = (plane->height + cell_size - 1u) / cell_size;
    if (!columns || !rows || columns * rows > 1u << 20) return -1;
    uint32_t total = 0u, ncmd = 0u, vertex = first_vertex;
    uint32_t command_indices[NATIVE_SCHED_CMDS];
    int need_geo = 0;
    for (uint32_t d = first; d < end; ++d) {
        const GlNativeGpuCommand *command = &commands[d];
        if (command->plane == plane_index) {
            command_indices[ncmd] = d;
            const uint32_t n = command->draw.topology == GPU_RENDER_SEMANTIC_LINES ? command->draw.line_count : native_gpu_command_triangles(command);
            s_sched_cmd[ncmd].base = total; s_sched_cmd[ncmd].vertex = vertex;
            s_sched_cmd[ncmd].lo = NATIVE_SCHED_NONE; s_sched_cmd[ncmd].hi = 0u;
            s_sched_cmd[ncmd].prepared = geometry && geometry->primitives ? geometry->primitives + geometry->offsets[d].primitive : NULL;
            need_geo |= !s_sched_cmd[ncmd].prepared;
            total += n; ++ncmd;
        }
        vertex += native_gpu_command_vertex_count(command);
    }
    if (!total) return 1;
    if (total > NATIVE_SCHED_PRIMS) return -1;
    const int captured = nvk_capture_run(plane_index, commands, first, end, first_vertex, captured_data,
                                        slice_vertices, geometry, total);
    if (captured >= 0) return captured;
    if (!nvk_sched_reserve((void **)&s_sched_depth_fast, &s_sched_depth_capacity, total, sizeof(*s_sched_depth_fast))) return -1;
    memset(s_sched_depth_fast, 0, total * sizeof(*s_sched_depth_fast));
    const uint32_t cells = columns * rows;
    if (!nvk_sched_reserve((void **)&s_sched_max_layer, &s_sched_max_capacity, cells, sizeof(*s_sched_max_layer))) return -1;
    if (!nvk_sched_reserve((void **)&sched->prim, &sched->prim_cap, total, sizeof(*sched->prim)) ||
        !nvk_sched_reserve((void **)&sched->layer, &sched->layer_cap, total, sizeof(*sched->layer))) return -1;
    if ((need_geo && !nvk_sched_reserve((void **)&sched->geo, &sched->geo_cap, total, sizeof(*sched->geo))) ||
        !nvk_sched_reserve((void **)&sched->shape, &sched->shape_cap, 2u * total, sizeof(*sched->shape)) ||
        !nvk_sched_reserve((void **)&sched->shape_layer, &sched->shape_layer_cap, 2u * total, sizeof(*sched->shape_layer)) ||
        !nvk_sched_reserve((void **)&sched->stamp, &sched->stamp_cap, 2u * total, sizeof(*sched->stamp))) return -1;
    if (cells > sched->cell_cap) {
        free(sched->cell_head); free(sched->touched);
        sched->cell_head = malloc((size_t)cells * sizeof(*sched->cell_head));
        sched->touched = malloc((size_t)cells * sizeof(*sched->touched));
        if (!sched->cell_head || !sched->touched) {
            free(sched->cell_head); free(sched->touched);
            sched->cell_head = NULL; sched->touched = NULL; sched->cell_cap = 0u;
            return -1;
        }
        for (uint32_t i = 0; i < cells; ++i) sched->cell_head[i] = -1;
        sched->cell_cap = cells;
    }
    /* Planning: geometry, reads and layers, without recording anything. */
    const float offset = 0.5f / scale - 1.f / 64.f;
    uint32_t prim_count = 0u, shape_count = 0u, touched = 0u, nodes = 0u, run_max = 0u, p = 0u;
    int global = 0, failed = 0;
    sched->query = 0u;
    if (cached && cached->valid == 1 && cached->total == total && cached->ncmd == ncmd) {
        prim_count = cached->prim_count; shape_count = cached->shape_count; run_max = cached->run_max;
        memcpy(sched->prim, cached->primitives, (size_t)prim_count * sizeof(*sched->prim));
        memcpy(sched->shape, cached->shapes, (size_t)shape_count * sizeof(*sched->shape));
        memcpy(sched->layer, cached->layers, (size_t)total * sizeof(*sched->layer));
        memcpy(s_sched_cmd, cached->commands, (size_t)ncmd * sizeof(*s_sched_cmd));
        ++s_template_schedule_hits;
        goto plan_ready;
    }
    for (uint32_t d = first, ci = 0u; d < end && !failed; ++d) {
        const GlNativeGpuCommand *command = &commands[d];
        if (command->plane != plane_index) continue;
        const XgSemanticDrawRecord *draw = &command->draw;
        const int lines = draw->topology == GPU_RENDER_SEMANTIC_LINES;
        const uint32_t per = lines ? 6u : 3u, vstart = s_sched_cmd[ci].vertex;
        const GlNativeGpuMeshTriangle *mesh = command->mesh_count
            ? (const GlNativeGpuMeshTriangle *)(const void *)(captured_data + command->data) : NULL;
        int material_valid = 0;
        GlNativeTextureState shared_texture = {0};
        XgRenderIrMaterialState material;
        for (uint32_t t = 0; t < (lines ? draw->line_count : native_gpu_command_triangles(command)) && !failed; ++t, ++p) {
            const XgRenderIrMaterialState *m = &draw->primitive.material;
            if (mesh) {
                const GlNativeTextureState *texture = &mesh[t].texture;
                if (!material_valid || !native_texture_state_equal(texture, &shared_texture)) {
                    material = draw->primitive.material;
                    native_texture_state_apply(&material, texture);
                    shared_texture = *texture;
                    material_valid = 1;
                }
                m = &material;
            }
            const GlNativeGpuDrawGeometry *g;
            if (s_sched_cmd[ci].prepared) g = &s_sched_cmd[ci].prepared[t];
            else {
                GlNativeGpuDrawGeometry *out = &sched->geo[p];
                native_gpu_prepare_draw(work, command, captured_data, t, plane->width / scale, plane->height / scale,
                                        slice_vertices + vstart + t * per, out);
                g = out;
            }
            sched->layer[p] = NATIVE_SCHED_NONE;
            if (!g->visible) continue;
            if (!nvk_damage_intersects(g->left * (int)scale, g->top * (int)scale,
                    (g->right + 1) * (int)scale, (g->bottom + 1) * (int)scale)) continue;
            GlNativeSchedPrim *pr = &sched->prim[prim_count++];
            pr->shape = shape_count; pr->count = 0u; pr->unknown = 0u;
            const int fast_depth = optimize_depth && command->plane != 0u && nvk_depth_bounds_draw(plane, scale, g);
            s_sched_depth_fast[p] = fast_depth != 0;
            pr->reads = (m->semi_transparent || m->mask_check ||
                (nvk_depth_reads(plane, &g->depth, command->plane != 0u) && (!fast_depth || g->depth.semi))) != 0;
            for (uint32_t k = 0; k < per / 3u; ++k) {
                const float (*v)[GL_NATIVE_GPU_VERTEX_FLOATS] = slice_vertices + vstart + t * per + k * 3u;
                const int r = native_gpu_shape_tri(&sched->shape[shape_count], v[0], v[1], v[2], offset, scale);
                if (r < 0) pr->unknown = 1u;
                else if (r) { ++pr->count; ++shape_count; }
            }
            uint32_t layer = 0u;
            if (pr->unknown || global) { if (prim_count > 1u) layer = run_max + pr->reads; }
            else for (uint32_t k = 0; k < pr->count; ++k)
                layer = native_sched_query_bounded(plane->width, plane->height, columns, rows, cell_size, &sched->shape[pr->shape + k], pr->reads, scale, layer, s_sched_max_layer);
            if (pr->unknown) global = 1;
            if (layer > NATIVE_SCHED_LAYERS) { failed = 1; break; }
            if (layer > run_max) run_max = layer;
            sched->layer[p] = (uint16_t)layer;
            if (layer < s_sched_cmd[ci].lo) s_sched_cmd[ci].lo = (uint16_t)layer;
            if (layer > s_sched_cmd[ci].hi) s_sched_cmd[ci].hi = (uint16_t)layer;
            for (uint32_t k = 0; k < pr->count; ++k) {
                const uint32_t si = pr->shape + k;
                int c[4];
                sched->shape_layer[si] = (uint16_t)layer;
                sched->stamp[si] = 0u;
                if (!native_gpu_shape_cells_sized(plane->width, plane->height, columns, rows, cell_size, &sched->shape[si], c)) continue;
                const uint32_t need = nodes + (uint32_t)((c[2] - c[0] + 1) * (c[3] - c[1] + 1));
                if (!nvk_sched_reserve((void **)&sched->node_shape, &sched->node_cap, need, sizeof(*sched->node_shape)) ||
                    !nvk_sched_reserve((void **)&sched->node_next, &sched->node_next_cap, need, sizeof(*sched->node_next))) {
                    failed = 1;
                    break;
                }
                for (int y = c[1]; y <= c[3]; ++y)
                    for (int x = c[0]; x <= c[2]; ++x) {
                        const uint32_t cell = (uint32_t)y * columns + (uint32_t)x;
                        if (sched->cell_head[cell] < 0) {
                            sched->touched[touched++] = cell;
                            s_sched_max_layer[cell] = (uint16_t)layer;
                        } else if (layer > s_sched_max_layer[cell]) s_sched_max_layer[cell] = (uint16_t)layer;
                            sched->node_shape[nodes] = si;
                            sched->node_next[nodes] = sched->cell_head[cell];
                            sched->cell_head[cell] = (int32_t)nodes++;
                    }
            }
        }
        ++ci;
    }
    for (uint32_t i = 0; i < touched; ++i) sched->cell_head[sched->touched[i]] = -1;
    if (failed) return -1;
    if (!prim_count) return 1;
    if (cached && !cached->valid && !cached->primitives) {
        cached->primitives = malloc((size_t)prim_count * sizeof(*cached->primitives));
        cached->shapes = malloc((size_t)(shape_count ? shape_count : 1u) * sizeof(*cached->shapes));
        cached->layers = malloc((size_t)total * sizeof(*cached->layers));
        cached->commands = malloc((size_t)ncmd * sizeof(*cached->commands));
        if (cached->primitives && cached->shapes && cached->layers && cached->commands) {
            memcpy(cached->primitives, sched->prim, (size_t)prim_count * sizeof(*sched->prim));
            memcpy(cached->shapes, sched->shape, (size_t)shape_count * sizeof(*sched->shape));
            memcpy(cached->layers, sched->layer, (size_t)total * sizeof(*sched->layer));
            memcpy(cached->commands, s_sched_cmd, (size_t)ncmd * sizeof(*s_sched_cmd));
            cached->prim_count = prim_count; cached->shape_count = shape_count; cached->run_max = run_max;
            cached->ncmd = ncmd; cached->total = total; cached->valid = 1;
        } else {
            free(cached->primitives); free(cached->shapes); free(cached->layers); free(cached->commands);
            cached->primitives = NULL; cached->shapes = NULL; cached->layers = NULL; cached->commands = NULL;
            cached->valid = -1;
        }
    }
plan_ready: ;
    /* Stable counting-sort by layer, then command/triangle FIFO. Emit each
     * selected primitive once instead of rescanning every mesh for every
     * layer. Damage rectangles can then be grouped without multiplying that
     * scan or breaking material batches at every triangle. */
    if (!nvk_sched_reserve((void **)&s_sched_emission, &s_sched_emission_capacity,
                          3u * total, sizeof(*s_sched_emission))) return -1;
    uint32_t offsets[NATIVE_SCHED_LAYERS + 2u] = {0};
    uint32_t cursors[NATIVE_SCHED_LAYERS + 1u];
    uint32_t *emit_command = s_sched_emission, *emit_triangle = s_sched_emission + total;
    uint32_t *emit_reads = s_sched_emission + 2u * total;
    for (uint32_t i = 0u; i < total; ++i)
        if (sched->layer[i] != NATIVE_SCHED_NONE) ++offsets[sched->layer[i] + 1u];
    for (uint32_t i = 1u; i <= run_max + 1u; ++i) offsets[i] += offsets[i - 1u];
    memcpy(cursors, offsets, (run_max + 1u) * sizeof(*cursors));
    uint32_t primitive = 0u;
    for (uint32_t ci = 0u; ci < ncmd; ++ci) {
        const uint32_t begin = s_sched_cmd[ci].base;
        const uint32_t limit = ci + 1u < ncmd ? s_sched_cmd[ci + 1u].base : total;
        for (uint32_t i = begin; i < limit; ++i) if (sched->layer[i] != NATIVE_SCHED_NONE) {
            const uint32_t at = cursors[sched->layer[i]]++;
            emit_command[at] = ci; emit_triangle[at] = i - begin;
            emit_reads[at] = sched->prim[primitive++].reads;
        }
    }
    /* Readers sharing a layer cannot overlap: the dependency planner would
     * otherwise have placed the later reader in a higher layer. Sort only
     * contiguous reader segments; write-only/nonlocal draws remain FIFO
     * boundaries. Immutable HD replacements may optionally join these same
     * proven-disjoint reader segments, grouped by their texture identity. */
    if (S.sort_materials && !work->depth_view && nvk_sched_reserve((void **)&s_sched_order, &s_sched_order_capacity,
                                              total, sizeof(*s_sched_order))) {
        for (uint32_t at = 0u; at < offsets[run_max + 1u]; ++at) {
            const uint32_t ci = emit_command[at], t = emit_triangle[at];
            const GlNativeGpuCommand *command = &commands[command_indices[ci]];
            const GpuRenderHdTexture *hd = &command->draw.hd_texture;
            if (command->source || (hd->valid && !S.sort_hd_readers)) emit_reads[at] = 0u;
            const GlNativeTextureState texture = command->mesh_count
                ? ((const GlNativeGpuMeshTriangle *)(const void *)(captured_data + command->data))[t].texture
                : native_texture_state(&command->draw.primitive.material);
            const GlNativeGpuDrawGeometry *g = s_sched_cmd[ci].prepared ? &s_sched_cmd[ci].prepared[t]
                : &sched->geo[s_sched_cmd[ci].base + t];
            const uint64_t guest_key = hd->valid && S.sort_hd_readers ? 0u :
                (uint64_t)texture.tpage | ((uint64_t)texture.clut_x << 16u) | ((uint64_t)texture.clut_y << 26u);
            const uint64_t key = guest_key | ((uint64_t)texture.shading << 36u) |
                ((uint64_t)g->depth.write << 44u) | ((uint64_t)g->depth.bias << 48u) |
                ((uint64_t)(g->attributes.integral != 0) << 53u) | ((uint64_t)command->texture_snapshot << 54u);
            const uint64_t hd_key = hd->valid && S.sort_hd_readers ?
                ((uint64_t)hd->texture_hash << 32u) | hd->palette_hash : 0u;
            s_sched_order[at] = (NvkMaterialOrder){ .key = key, .hd_key = hd_key, .command = ci, .triangle = t };
        }
        for (uint32_t layer = 0u; layer <= run_max; ++layer) {
            uint32_t at = offsets[layer];
            while (at < offsets[layer + 1u]) {
                if (!emit_reads[at]) { ++at; continue; }
                const uint32_t begin = at++;
                while (at < offsets[layer + 1u] && emit_reads[at]) ++at;
                qsort(s_sched_order + begin, at - begin, sizeof(*s_sched_order), nvk_material_order_compare);
                for (uint32_t i = begin; i < at; ++i) {
                    emit_command[i] = s_sched_order[i].command;
                    emit_triangle[i] = s_sched_order[i].triangle;
                }
            }
        }
    }
    /* Emission. A layer-0 read that may see writes from before the run needs
     * one leading barrier; nvk_target stays out of the way. */
    int pre = 0;
    for (uint32_t i = 0, q = 0; i < prim_count && !pre; ++i) {
        while (q < total && sched->layer[q] == NATIVE_SCHED_NONE) ++q;
        const GlNativeSchedPrim *pr = &sched->prim[i];
        if (sched->layer[q++] || !pr->reads) continue;
        pre = plane->dirty_all || pr->unknown;
        for (uint32_t k = 0; !pre && k < pr->count; ++k)
            pre = native_gpu_hazard_hit(plane->width, plane->height, plane->hazards, &sched->shape[pr->shape + k], scale);
    }
    int ok = 1;
    R.scheduled_run = 1;
    nvk_pass_begin(plane);
    for (uint32_t layer = 0; layer <= run_max && ok; ++layer) {
        if (layer ? 1 : pre) {
            if (!nvk_core_batch_flush()) { ok = 0; break; }
            nvk_texture_barrier();
            R.c.destination_barriers++;
            nvk_plane_barrier_clear(plane);
        }
        const uint32_t regions = R.damage_active && !R.damage_shader_active ? R.damage_count : 1u;
        for (uint32_t region = 0u; region < regions && ok; ++region) {
            R.damage_region = R.damage_active && !R.damage_shader_active ? (int)region : -1;
            for (uint32_t at = offsets[layer]; at < offsets[layer + 1u] && ok;) {
                const uint32_t ci = emit_command[at], begin = at++;
                while (at < offsets[layer + 1u] && emit_command[at] == ci) ++at;
                const GlNativeGpuDrawGeometry *prepared = s_sched_cmd[ci].prepared
                    ? s_sched_cmd[ci].prepared : sched->geo + s_sched_cmd[ci].base;
                int visible = !R.damage_active;
                for (uint32_t i = begin; !visible && i < at; ++i) {
                    const GlNativeGpuDrawGeometry *g = &prepared[emit_triangle[i]];
                    visible = g->visible && nvk_damage_intersects(g->left * (int)scale, g->top * (int)scale,
                        (g->right + 1) * (int)scale, (g->bottom + 1) * (int)scale);
                }
                if (visible) ok = nvk_render_layer(&commands[command_indices[ci]], captured_data,
                    s_sched_cmd[ci].vertex, slice_vertices, prepared, NULL, 0u,
                    emit_triangle + begin, at - begin, s_sched_depth_fast + s_sched_cmd[ci].base);
            }
        }
    }
    if (!nvk_core_batch_flush()) ok = 0;
    R.damage_region = -1;
    R.scheduled_run = 0;
    R.c.scheduled_layers += run_max + 1u;
    /* Only the last layer is unprotected by a barrier. */
    for (uint32_t i = 0, q = 0; i < prim_count && !plane->dirty_all; ++i) {
        while (q < total && sched->layer[q] == NATIVE_SCHED_NONE) ++q;
        const GlNativeSchedPrim *pr = &sched->prim[i];
        if (sched->layer[q++] != run_max) continue;
        if (pr->unknown) plane->dirty_all = 1;
        for (uint32_t k = 0; k < pr->count && !plane->dirty_all; ++k)
            native_gpu_hazard_add(plane->width, plane->height, &plane->hazards, &plane->dirty_all, &sched->shape[pr->shape + k]);
    }
    return ok;
}

/* Cache draws [first, end) replayed on one phase plane, as one scheduled run. */
static int nvk_schedule_templates(const GlNativePhaseTemplates *cache, uint32_t plane, uint32_t first, uint32_t end)
{
    static GlNativeGpuCommand *commands;
    static uint32_t capacity;
    if (!nvk_sched_reserve((void **)&commands, &capacity, cache->count, sizeof(*commands))) return -1;
    for (uint32_t d = first; d < end; ++d) {
        commands[d] = cache->commands[d];
        commands[d].plane = plane;
    }
    NvkScheduleCache *plan = NULL;
    const char *option = getenv("PSX_NATIVE_VK_SCHEDULE_CACHE");
    const NvkPlane *target = &R.planes[plane];
    const uint32_t scale = native_gpu_plane_scale(R.work, plane);
    if (!S.interlock && !(option && option[0] == '0')) {
        for (uint32_t i = 0u; i < s_template_schedule_count; ++i) {
            NvkScheduleCache *candidate = &s_template_schedules[i];
            if (candidate->first == first && candidate->end == end && candidate->width == target->width &&
                candidate->height == target->height && candidate->has_key == target->has_key && candidate->scale == scale) {
                plan = candidate; break;
            }
        }
        if (R.damage_active && plan && plan->valid != 1) plan = NULL;
        if (!plan && !R.damage_active && s_template_schedule_count < sizeof(s_template_schedules) / sizeof(s_template_schedules[0])) {
            plan = &s_template_schedules[s_template_schedule_count++];
            plan->first = first; plan->end = end; plan->width = target->width; plan->height = target->height;
            plan->has_key = target->has_key; plan->scale = scale;
        }
    }
    return nvk_schedule_run(plane, commands, first, end, cache->geometry->offsets[first].vertex, cache->data,
        cache->geometry->vertices, cache->geometry, plan, 0);
}

/* ---- vertex data ----------------------------------------------------------------------------------------------------- */
static int nvk_upload_vertices(const float (*vertices)[GL_NATIVE_GPU_VERTEX_FLOATS], uint32_t count, VkBuffer *buf, VkDeviceSize *off)
{
    if (!count) { *buf = VK_NULL_HANDLE; *off = 0; return 1; }
    uint32_t chunk;
    VkDeviceSize offset;
    const size_t bytes = (size_t)count * sizeof(*vertices);
    uint8_t *dst = nvk_arena(bytes, 16u, &chunk, &offset);
    if (!dst) return 0;
    memcpy(dst, vertices, bytes);
    P.raw_vertex_upload_bytes += bytes;
    *buf = S.chunks[chunk].buf.buffer;
    *off = offset;
    return 1;
}

/* Core batches own their packed vertices. Upload the original slice only for
 * a draw that actually binds it (including count passes and fallback draws).
 * The CPU slice stays alive until all of its draws have been recorded. */
static int nvk_require_vertices(void)
{
    if (R.vsrc.buf) return 1;
    return R.vsrc.vertices && R.vsrc.count &&
        nvk_upload_vertices(R.vsrc.vertices, R.vsrc.count, &R.vsrc.buf, &R.vsrc.off);
}

/* ---- words ------------------------------------------------------------------------------------------------------------- */
/* Copies the rectangle of the upload shadow into the words image. */
static int nvk_words_copy(int left, int top, int right, int bottom)
{
    ++S.words_generation;
    nvk_pass_end();
    const uint32_t w = (uint32_t)(right - left), h = (uint32_t)(bottom - top);
    uint32_t chunk;
    VkDeviceSize offset;
    uint16_t *dst = (uint16_t *)nvk_arena((VkDeviceSize)w * h * 2u, 16u, &chunk, &offset);
    if (!dst) return 0;
    for (uint32_t y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * w, S.upload_words + (size_t)(top + (int)y) * VRAM_W + left, (size_t)w * 2u);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = offset;
    region.bufferRowLength = w;
    region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    region.imageOffset = (VkOffset3D){ left, top, 0 };
    region.imageExtent = (VkExtent3D){ w, h, 1u };
    vkCmdCopyBufferToImage(R.cmd, S.chunks[chunk].buf.buffer, S.words.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    nvk_barrier_all();
    return 1;
}

/* The words image holds the shadow's contents between works. Upload only the bounding rectangle in which the
 * work's initial words differ from it (GL native_gpu_sync_initial_words). */
static int nvk_sync_initial_words(const void *words)
{
    const size_t row_bytes = VRAM_W * sizeof(uint16_t);
    const uint8_t *input = words;
    uint32_t left = 0u, top = 0u, right = VRAM_W, bottom = VRAM_H;
    if (S.upload_valid) {
        if (!memcmp(S.upload_words, words, NVK_WORDS_BYTES)) return 1;
        while (top < bottom && !memcmp(S.upload_words + top * VRAM_W, input + top * row_bytes, row_bytes)) ++top;
        while (bottom > top && !memcmp(S.upload_words + (bottom - 1u) * VRAM_W, input + (bottom - 1u) * row_bytes, row_bytes)) --bottom;
        left = VRAM_W; right = 0u;
        for (uint32_t row = 0u; row < bottom - top && (left || right < VRAM_W); ++row) {
            const uint32_t y = row == 0u ? top : row == 1u ? bottom - 1u : top + row - 1u;
            const uint8_t *old_row = (const uint8_t *)(S.upload_words + y * VRAM_W);
            const uint8_t *new_row = input + y * row_bytes;
            if (left && memcmp(old_row, new_row, left * sizeof(uint16_t)) != 0) {
                uint32_t first = 0u;
                while (first < left && !memcmp(old_row + first * 2u, new_row + first * 2u, 64u * 2u)) first += 64u;
                left = first;
            }
            if (right < VRAM_W && memcmp(old_row + right * 2u, new_row + right * 2u, (VRAM_W - right) * 2u) != 0) {
                uint32_t end = VRAM_W;
                while (end > right && !memcmp(old_row + (end - 64u) * 2u, new_row + (end - 64u) * 2u, 64u * 2u)) end -= 64u;
                right = end;
            }
        }
    }
    for (uint32_t y = top; y < bottom; ++y)
        memcpy(S.upload_words + y * VRAM_W + left, input + y * row_bytes + left * 2u, (right - left) * 2u);
    S.upload_valid = 1;
    return nvk_words_copy((int)left, (int)top, (int)right, (int)bottom);
}


/* ---- interlock tier: static phase parameters (GL native_gpu_template_parameters_prepare) ---------------------------------- */
/* The geometry of a phase template cache is the same for every phase: its triangles' parameter records are
 * built once per work, uploaded once, and each phase replays them as draw ranges. When the cache was prepared
 * for GPU setup, native_setup.comp computes the geometry-dependent records from the raw vertices. */
typedef struct NvkTemplateCommand {
    uint32_t first, count, visible;
    unsigned depth;
    int left, top, right, bottom, scissor[4];
    uint64_t depth_tested;
} NvkTemplateCommand;
typedef struct NvkTemplatePlan {
    NvkTemplateCommand *commands;
    uint8_t *visible;
    uint32_t triangles;
} NvkTemplatePlan;

typedef struct NvkSetupTriangle { int32_t xyuv[3][4], rgbz[3][4], config[4], depth[4]; } NvkSetupTriangle;
_Static_assert(sizeof(NvkSetupTriangle) == 128u, "GPU setup std430 stride");

static int nvk_template_upload(const uint32_t (*texels)[4], uint32_t texel_count)
{
    const VkDeviceSize bytes = (VkDeviceSize)texel_count * 16u;
    if (!S.tpl_buffer.buffer || S.tpl_buffer.size < bytes) {
        if (S.tpl_view) vkDestroyBufferView(vk_core_device(), S.tpl_view, NULL);
        S.tpl_view = VK_NULL_HANDLE;
        vk_core_buffer_destroy(&S.tpl_buffer);
        memset(&S.tpl_buffer, 0, sizeof(S.tpl_buffer));
        if (!vk_core_buffer_create(&S.tpl_buffer, bytes + (bytes >> 2),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                VK_CORE_MEMORY_DEVICE, "native template parameters")) return 0;
        VkBufferViewCreateInfo bvi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
        bvi.buffer = S.tpl_buffer.buffer; bvi.format = VK_FORMAT_R32G32B32A32_UINT; bvi.range = VK_WHOLE_SIZE;
        if (!nvk_ok(vkCreateBufferView(vk_core_device(), &bvi, NULL, &S.tpl_view), "vkCreateBufferView")) return 0;
    }
    uint32_t chunk;
    VkDeviceSize offset;
    uint8_t *stage = nvk_arena(bytes, 16u, &chunk, &offset);
    if (!stage) return 0;
    memcpy(stage, texels, (size_t)bytes);
    nvk_pass_end();
    const VkBufferCopy region = { offset, 0u, bytes };
    vkCmdCopyBuffer(R.cmd, S.chunks[chunk].buf.buffer, S.tpl_buffer.buffer, 1u, &region);
    nvk_barrier_all();
    return 1;
}

/* GL native_gpu_setup_dispatch: the raw vertices of every template triangle in, the shader's records patched in the buffer. */
static int nvk_setup_dispatch(const GlNativePhaseTemplates *cache, uint32_t triangles)
{
    const GlNativeGpuWork *work = R.work;
    if (triangles > S.setup_max_triangles) return 0;
    uint32_t chunk;
    VkDeviceSize offset;
    uint8_t *mem = nvk_arena((VkDeviceSize)triangles * sizeof(NvkSetupTriangle), 256u, &chunk, &offset);
    if (!mem) return 0;
    NvkSetupTriangle *inputs = (NvkSetupTriangle *)(void *)mem;
    memset(inputs, 0, (size_t)triangles * sizeof(*inputs));
    for (uint32_t c = 0u; c < cache->count; ++c) {
        const GlNativeGpuCommand *command = &cache->commands[c];
        const XgSemanticDrawRecord *draw = &command->draw;
        const XgRenderIrMaterialState *m = &draw->primitive.material;
        const uint32_t first = cache->geometry->offsets[c].vertex / 3u;
        for (uint32_t t = 0u; t < native_gpu_command_triangles(command); ++t) {
            NvkSetupTriangle *out = &inputs[first + t];
            const XgRenderIrTriangle *tri = native_gpu_command_triangle(command, cache->data, t);
            const GlNativeTriangleFlags flags = native_gpu_command_flags(command, cache->data, t);
            for (unsigned v = 0u; v < 3u; ++v) {
                const XgRenderIrVertex *iv = &tri->vertices[v];
                out->xyuv[v][0] = iv->x; out->xyuv[v][1] = iv->y;
                out->xyuv[v][2] = iv->u; out->xyuv[v][3] = iv->v;
                out->rgbz[v][0] = iv->r; out->rgbz[v][1] = iv->g; out->rgbz[v][2] = iv->b;
                out->rgbz[v][3] = native_vertex_depth_q12(iv);
            }
            out->config[0] = (int32_t)work->scale; out->config[1] = flags.gouraud;
            out->config[2] = m->draw_offset_x; out->config[3] = m->draw_offset_y - command->y;
            const unsigned policy = draw->primitive.depth_policy;
            out->depth[0] = !draw->screen_space_2d && !draw->native_view_effect && policy <= GPU_RENDER_DEPTH_TEST_WRITE ? (int)policy : 0;
            out->depth[1] = draw->primitive.depth_bias <= 16u ? draw->primitive.depth_bias : 0;
            out->depth[2] = work->depth_test;
            out->depth[3] = flags.semi_transparent;
        }
    }
    uint32_t uchunk;
    VkDeviceSize uoffset;
    uint8_t *uniform = nvk_arena(NVK_SLOT_ALIGN, NVK_SLOT_ALIGN, &uchunk, &uoffset);
    if (!uniform) return 0;
    memset(uniform, 0, NVK_SLOT_ALIGN);
    memcpy(uniform, &triangles, sizeof(triangles));
    VkDescriptorSet set0 = nvk_set_alloc(S.dsl_setup0), set1 = nvk_set_alloc(S.dsl_setup1);
    if (!set0 || !set1) return 0;
    const VkDescriptorBufferInfo in = { S.chunks[chunk].buf.buffer, offset, (VkDeviceSize)triangles * sizeof(NvkSetupTriangle) };
    const VkDescriptorBufferInfo out = { S.tpl_buffer.buffer, 0, VK_WHOLE_SIZE };
    const VkDescriptorBufferInfo ubo = { S.chunks[uchunk].buf.buffer, uoffset, 16u };
    VkWriteDescriptorSet w[3];
    memset(w, 0, sizeof(w));
    for (unsigned i = 0u; i < 3u; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].descriptorCount = 1u;
    }
    w[0].dstSet = set0; w[0].dstBinding = 0u; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &in;
    w[1].dstSet = set0; w[1].dstBinding = 1u; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &out;
    w[2].dstSet = set1; w[2].dstBinding = 0u; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[2].pBufferInfo = &ubo;
    vkUpdateDescriptorSets(vk_core_device(), 3u, w, 0u, NULL);
    nvk_pass_end();
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe_setup);
    const VkDescriptorSet sets[2] = { set0, set1 };
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl_setup, 0u, 2u, sets, 0u, NULL);
    vkCmdDispatch(R.cmd, (triangles + 63u) / 64u, 1u, 1u);
    nvk_barrier_all();
    ((GlNativeGpuWork *)(void *)work)->timing.gpu_setup_triangles += triangles;
    return 1;
}

/* GPU setup replaces every triangle-local attribute/depth plane: capture the remaining command/material
 * parameters once per texture state and copy those records to every triangle of the state. */
static int nvk_template_materials(const GlNativeGpuCommand *draw, const GlNativePhaseTemplates *cache,
                                  GlNativeGpuGeometryOffset offset, NvkTemplateCommand *info,
                                  uint32_t (*texels)[4], uint32_t triangles, uint8_t *visible)
{
    struct Material {
        GlNativeTextureState texture;
        uint32_t record[NVK_IL_TRIANGLE_TEXELS][4], shared[NVK_IL_SHARED_TEXELS][4];
        uint64_t tested;
    } materials[64];
    uint64_t valid = 0u;
    const GlNativeGpuMeshTriangle *mesh = draw->mesh_count
        ? (const GlNativeGpuMeshTriangle *)(const void *)(cache->data + draw->data) : NULL;
    uint32_t scratch[NVK_IL_TRIANGLE_TEXELS + NVK_IL_SHARED_TEXELS][4];
    for (uint32_t t = 0u; t < info->count; ++t) {
        const GlNativeGpuDrawGeometry *geometry = &cache->geometry->primitives[offset.primitive + t];
        if (!geometry->visible) continue;
        const uint32_t index = info->first + t;
        const GlNativeTextureState texture = mesh ? mesh[t].texture : native_texture_state(&draw->draw.primitive.material);
        const uint32_t hash = (uint32_t)texture.tpage * 2654435761u + (uint32_t)texture.clut_x * 17u +
            (uint32_t)texture.clut_y * 97u + texture.shading;
        const uint32_t slot = (hash ^ (hash >> 16u)) & 63u;
        struct Material *material = &materials[slot];
        if (!(valid & (UINT64_C(1) << slot)) || !native_texture_state_equal(&material->texture, &texture)) {
            GlNativeGpuCommand one = *draw;
            if (mesh) { one.mesh_count = 1u; one.data += (uint32_t)((size_t)t * sizeof(*mesh)); }
            else { one.draw.primitive.triangles[0] = draw->draw.primitive.triangles[t]; one.draw.primitive.triangle_count = 1u; }
            const uint64_t old_tested = R.c.depth_tested_triangles;
            TC.texels = scratch; TC.triangles = 1u; TC.bias = index; TC.visible = NULL;
            if (!nvk_render_command(&one, cache->data, offset.vertex + t * 3u, cache->geometry->vertices, geometry)) return 0;
            material->texture = texture;
            material->tested = R.c.depth_tested_triangles - old_tested;
            memcpy(material->record, scratch, sizeof(material->record));
            memcpy(material->shared, scratch + NVK_IL_TRIANGLE_TEXELS, sizeof(material->shared));
            valid |= UINT64_C(1) << slot;
        }
        visible[index] = 1u;
        material->record[8][3] = index;
        memcpy(texels + (size_t)index * NVK_IL_TRIANGLE_TEXELS, material->record, sizeof(material->record));
        memcpy(texels + (size_t)triangles * NVK_IL_TRIANGLE_TEXELS + (size_t)index * NVK_IL_SHARED_TEXELS,
               material->shared, sizeof(material->shared));
        info->depth_tested += material->tested;
    }
    return 1;
}

static void nvk_template_plan_free(void)
{
    if (!R.plan) return;
    free(R.plan->commands); free(R.plan->visible); free(R.plan);
    R.plan = NULL;
}

/* Builds this execution's plan on the first template command; without one the templates replay triangle by triangle. */
static void nvk_template_prepare(uint32_t plane_index)
{
    if (R.plan_attempted) return;
    R.plan_attempted = 1;
    const GlNativeGpuWork *work = R.work;
    const GlNativePhaseTemplates *cache = work->phase_templates;
    const char *option = getenv("PSX_NATIVE_STATIC_PHASE_PARAMETERS");
    if (!cache || !S.interlock || work->wireframe || gl_renderer_debug_mipmaps() || s_native_depth_stats_enabled ||
        (option && option[0] == '0')) return;
    for (uint32_t i = 0u; i < cache->count; ++i)
        if (cache->commands[i].kind != NATIVE_GPU_DRAW || cache->commands[i].source ||
            cache->commands[i].draw.hd_texture.valid || cache->commands[i].draw.topology != GPU_RENDER_SEMANTIC_TRIANGLES) return;
    const uint32_t triangles = cache->geometry->offsets[cache->count].vertex / 3u;
    if (!triangles || triangles > UINT32_MAX / (NVK_IL_TRIANGLE_TEXELS + NVK_IL_SHARED_TEXELS)) return;
    const uint32_t texel_count = triangles * (NVK_IL_TRIANGLE_TEXELS + NVK_IL_SHARED_TEXELS);
    if (texel_count > vk_core_caps()->max_texel_buffer_elements) return;
    const int gpu_setup = cache->geometry->gpu_setup;
    if (gpu_setup && !S.pipe_setup) return;
    NvkTemplatePlan *plan = calloc(1u, sizeof(*plan));
    uint32_t (*texels)[4] = calloc(texel_count, sizeof(*texels));
    if (plan) {
        plan->commands = calloc(cache->count, sizeof(*plan->commands));
        plan->visible = calloc(triangles, sizeof(*plan->visible));
        plan->triangles = triangles;
    }
    if (!plan || !plan->commands || !plan->visible || !texels) goto failed;
    nvk_pass_end();
    const NvkCounters saved = R.c;
    TC.active = 1;
    for (uint32_t i = 0u; i < cache->count; ++i) {
        GlNativeGpuCommand draw = cache->commands[i];
        draw.plane = plane_index;
        const GlNativeGpuGeometryOffset offset = cache->geometry->offsets[i];
        NvkTemplateCommand *info = &plan->commands[i];
        info->first = offset.vertex / 3u;
        info->count = (cache->geometry->offsets[i + 1u].vertex - offset.vertex) / 3u;
        const uint64_t old_tested = R.c.depth_tested_triangles;
        int ok;
        if (gpu_setup) ok = nvk_template_materials(&draw, cache, offset, info, texels, triangles, plan->visible);
        else {
            TC.texels = texels; TC.triangles = triangles; TC.bias = 0u; TC.visible = plan->visible;
            ok = nvk_render_command(&draw, cache->data, offset.vertex, cache->geometry->vertices,
                                    cache->geometry->primitives + offset.primitive);
            info->depth_tested = R.c.depth_tested_triangles - old_tested;
        }
        R.c = saved;
        if (!ok) { TC.active = 0; goto failed; }
        for (uint32_t t = 0u; t < info->count; ++t) {
            if (!plan->visible[info->first + t]) continue;
            const GlNativeGpuDrawGeometry *g = &cache->geometry->primitives[offset.primitive + t];
            if (!info->visible++) {
                info->left = g->left; info->top = g->top; info->right = g->right; info->bottom = g->bottom;
                memcpy(info->scissor, g->scissor, sizeof(info->scissor));
            } else {
                if (g->left < info->left) info->left = g->left;
                if (g->top < info->top) info->top = g->top;
                if (g->right > info->right) info->right = g->right;
                if (g->bottom > info->bottom) info->bottom = g->bottom;
            }
        }
        info->depth = work->depth_test && R.planes[plane_index].has_key ? NVK_UNIFORM_DEPTH_ATTACHED | NVK_UNIFORM_DEPTH_READ : 0u;
    }
    TC.active = 0;
    if (!nvk_template_upload(texels, texel_count)) goto failed;
    if (gpu_setup && !nvk_setup_dispatch(cache, triangles)) goto failed;
    R.plan = plan;
    free(texels);
    return;
failed:
    if (plan) { free(plan->commands); free(plan->visible); free(plan); }
    free(texels);
}

/* GL native_gpu_template_render: the visible triangles of one cache draw, as ranges of the plan's records. */
static int nvk_template_render(const GlNativeGpuCommand *command)
{
    const NvkTemplatePlan *plan = R.plan;
    const NvkTemplateCommand *info = &plan->commands[command->data];
    if (!info->visible) return 1;
    if (!nvk_require_vertices()) return 0;
    NvkPlane *plane = &R.planes[command->plane];
    const uint32_t scale = native_gpu_plane_scale(R.work, command->plane);
    if (R.since_submit >= NVK_SUBMIT_DRAWS_IL && !nvk_submit_partial(VK_NULL_HANDLE)) return 0;
    if (!nvk_target(plane, scale, info->left, info->top, info->right - info->left + 1, info->bottom - info->top + 1, 0,
                    info->depth, info->scissor, NULL, 0, 0.f)) return 0;
    if (!nvk_il_flush() || !nvk_raster_page(plane, s_scissor_value[1])) return 0;
    memset(&R.key, 0, sizeof(R.key));
    R.key.src_sampler = R.key.hd_sampler = S.sampler_clamp;
    R.key.src = R.key.hd = S.dummy_color.view;
    R.key.src_key = S.dummy_uint.view;
    R.key.dest = plane->color.view;
    R.key.dest_key = plane->key.view;
    R.key.recipe = R.recipe;
    R.key.params = S.tpl_view;
    NvkDrawIL il;
    memset(&il, 0, sizeof(il));
    il.parameters_on = 1;
    il.parameter_shared = (int32_t)(plan->triangles * NVK_IL_TRIANGLE_TEXELS);
    const float size[4] = { (float)plane->width / scale, (float)plane->height / scale, 0.5f / scale - 1.f / 64.f, (float)scale };
    if (!nvk_il_bind(&il, size, &R.key, &R.v)) return 0;
    if (R.b.vbuf != R.vsrc.buf || R.b.voff != R.vsrc.off) {
        vkCmdBindVertexBuffers(R.cmd, 0u, 1u, &R.vsrc.buf, &R.vsrc.off);
        R.b.vbuf = R.vsrc.buf; R.b.voff = R.vsrc.off;
    }
    nvk_scissor(plane, s_scissor_value);
    for (uint32_t t = 0u; t < info->count;) {
        if (!plan->visible[info->first + t]) { ++t; continue; }
        const uint32_t first = t++;
        while (t < info->count && plan->visible[info->first + t]) ++t;
        ++P.draw_calls; vkCmdDraw(R.cmd, (t - first) * 3u, 1u, (info->first + first) * 3u, 0u);
        ++R.since_submit;
    }
    R.c.geometry_draws += info->visible;
    R.c.depth_tested_triangles += info->depth_tested;
    return 1;
}

/* ---- one slice of the journal (GL native_gpu_service, QUEUED/DISPATCHING) -------------------------------------------------- */
static uint32_t nvk_capture_group(const GlNativeGpuCommand *commands, uint32_t first, uint32_t limit,
                                  const GlNativeGpuGeometry *geometry, int32_t rect[4], uint32_t *triangles)
{
    if (!S.merge_capture || !S.linked_capture || R.capture_eligible != 1 || R.capture_active ||
        !geometry || !geometry->primitives) return first;
    const uint32_t plane_index = commands[first].plane;
    if (plane_index < GL_NATIVE_GPU_PHASE_BASE || plane_index >= GL_NATIVE_GPU_PLANES) return first;
    const NvkPlane *plane = &R.planes[plane_index];
    if (!plane->capture_framebuffer) return first;
    const uint32_t scale = native_gpu_plane_scale(R.work, plane_index);
    const int x0 = R.work->phase_crop_x * scale, y0 = R.work->phase_crop_y * scale;
    const int x1 = x0 + R.work->scanout_width * scale, y1 = y0 + R.work->scanout_height * scale;
    int left = x1, top = y1, right = x0, bottom = y0, templates = 0;
    uint64_t fragments = 0u;
    uint32_t end = first, visible = 0u;
    for (; end < limit; ++end) {
        const GlNativeGpuCommand *command = &commands[end];
        const GlNativeGpuGeometry *g = geometry;
        uint32_t index = end;
        if (command->plane != plane_index) break;
        if (command->kind == NATIVE_GPU_TEMPLATE) {
            const GlNativePhaseTemplates *cache = R.work->phase_templates;
            if (!cache || command->data >= cache->count || !cache->geometry || !cache->geometry->primitives) break;
            index = command->data; command = &cache->commands[index]; g = cache->geometry;
        }
        if (command->kind != NATIVE_GPU_DRAW || command->source || command->draw.hd_texture.valid ||
            command->draw.topology != GPU_RENDER_SEMANTIC_TRIANGLES) break;
        uint64_t added = 0u;
        uint32_t count = 0u;
        int l = left, t = top, r = right, b = bottom;
        for (uint32_t p = g->offsets[index].primitive; p < g->offsets[index + 1u].primitive; ++p) {
            const GlNativeGpuDrawGeometry *primitive = &g->primitives[p];
            if (!primitive->visible) continue;
            int px0 = primitive->left * (int)scale, py0 = primitive->top * (int)scale;
            int px1 = (primitive->right + 1) * (int)scale, py1 = (primitive->bottom + 1) * (int)scale;
            if (px0 < x0) px0 = x0; if (py0 < y0) py0 = y0;
            if (px1 > x1) px1 = x1; if (py1 > y1) py1 = y1;
            if (px0 >= px1 || py0 >= py1) continue;
            added += (uint64_t)(px1 - px0) * (uint32_t)(py1 - py0);
            ++count;
            if (px0 < l) l = px0; if (py0 < t) t = py0;
            if (px1 > r) r = px1; if (py1 > b) b = py1;
        }
        if (added > S.capture_capacity || fragments > S.capture_capacity - added) break;
        fragments += added; visible += count;
        left = l; top = t; right = r; bottom = b;
        templates |= commands[end].kind == NATIVE_GPU_TEMPLATE;
    }
    if (!templates || visible < 128u || left >= right || top >= bottom) return first;
    rect[0] = left; rect[1] = top; rect[2] = right - left; rect[3] = bottom - top;
    *triangles = visible;
    return end;
}

static int nvk_slice(const GlNativeGpuCommand *commands, const uint8_t *captured_data, uint32_t cursor,
                     uint32_t command_limit, const GlNativeGpuGeometry *geometry)
{
    const GlNativeGpuWork *work = R.work;
    float (*slice_vertices)[GL_NATIVE_GPU_VERTEX_FLOATS] = NULL;
    int borrowed = 0, ok = 1;
    uint32_t count = 0u;
    if (geometry) {
        const uint32_t first = geometry->offsets[cursor].vertex;
        count = geometry->offsets[command_limit].vertex - first;
        slice_vertices = count ? geometry->vertices + first : NULL;
        borrowed = 1;
    } else if (!native_gpu_build_vertex_slice(work, commands, captured_data, cursor, command_limit, &slice_vertices, &count)) {
        nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_VERTICES, cursor, 0u);
        return 0;
    }
    R.vsrc = (NvkVertexSource){ .vertices = slice_vertices, .count = count };
    if (!S.lazy_vertices && !nvk_upload_vertices(slice_vertices, count, &R.vsrc.buf, &R.vsrc.off)) {
        nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_VERTICES, cursor, 1u);
        if (!borrowed) free(slice_vertices);
        return 0;
    }
    uint32_t first_vertex = 0u;
    int template_vertices_bound = 0;
    for (uint32_t i = cursor; i < command_limit && ok; ++i) {
        const GlNativeGpuCommand *command = &commands[i];
        const int template_draw = command->kind == NATIVE_GPU_TEMPLATE;
        if (template_draw != template_vertices_bound) {
            R.b.vbuf = VK_NULL_HANDLE;
            template_vertices_bound = template_draw;
        }
        const uint32_t command_first_vertex = first_vertex;
        first_vertex += native_gpu_command_vertex_count(command);
        int32_t capture_rect[4]; uint32_t capture_triangles = 0u;
        const uint32_t capture_end = nvk_capture_group(commands, i, command_limit, geometry, capture_rect, &capture_triangles);
        if (capture_end > i) {
            const NvkVertexSource saved = R.vsrc;
            NvkPlane *target = &R.planes[command->plane];
            ok = nvk_capture_begin(target, native_gpu_plane_scale(R.work, command->plane),
                capture_rect[0], capture_rect[1], (uint32_t)capture_rect[2], (uint32_t)capture_rect[3]);
            if (ok) {
                R.scheduled_run = 1;
                ok = nvk_slice(commands, captured_data, i, capture_end, geometry);
                nvk_capture_finish(target, ok, capture_triangles);
            }
            R.vsrc = saved;
            R.b.vbuf = VK_NULL_HANDLE;
            first_vertex = command_first_vertex;
            for (uint32_t j = i; j < capture_end; ++j) first_vertex += native_gpu_command_vertex_count(&commands[j]);
            i = capture_end - 1u;
            continue;
        }
        if (command->kind == NATIVE_GPU_TEMPLATE) {
            const GlNativePhaseTemplates *cache = work->phase_templates;
            if (!cache || command->data >= cache->count) { ok = 0; break; }
            uint32_t end = i + 1u;
            while (end < command_limit && end - i < NATIVE_SCHED_CMDS && commands[end].kind == NATIVE_GPU_TEMPLATE &&
                   commands[end].plane == command->plane && commands[end].data == command->data + (end - i)) ++end;
            int scheduled = -1;
            const NvkVertexSource saved = R.vsrc;
            R.vsrc = R.template_vsrc;
            if (!R.plan_attempted) nvk_template_prepare(command->plane);
            if (R.plan && command->data < cache->count) {
                ok = nvk_template_render(command);
            } else if (command->data + (end - i) <= cache->count &&
                (scheduled = nvk_schedule_templates(cache, command->plane, command->data, command->data + (end - i))) >= 0) {
                ok = scheduled;
                i = end - 1u;
            } else {
                GlNativeGpuCommand draw = cache->commands[command->data];
                draw.plane = command->plane;
                const GlNativeGpuGeometryOffset offset = cache->geometry->offsets[command->data];
                ok = nvk_render_command(&draw, cache->data, offset.vertex, cache->geometry->vertices,
                    cache->geometry->gpu_setup ? NULL : cache->geometry->primitives + offset.primitive);
            }
            R.template_vsrc = R.vsrc;
            R.vsrc = saved;
        } else if (command->kind == NATIVE_GPU_PROVENANCE_CLEAR || command->kind == NATIVE_GPU_CANONICAL_READBACK) {
            ok = 0;
        } else if (command->kind == NATIVE_GPU_WORDS || command->kind == NATIVE_GPU_WORDS_RESET) {
            int left = VRAM_W, top = VRAM_H, right = 0, bottom = 0;
            /* No draw observes the intermediate patches in this run. Keep their
             * exact FIFO writes in the shadow, then upload the bounding rectangle. */
            do {
                command = &commands[i];
                const int rows = command->h ? command->h : 1;
                if (command->kind == NATIVE_GPU_WORDS_RESET) {
                    /* Restore the word blocks patched since the phase baseline. */
                    for (unsigned y = 0u; y < VRAM_H; ++y) {
                        const uint16_t mask = R.reset_word_rows[y];
                        if (!mask) continue;
                        R.reset_word_rows[y] = 0u;
                        for (unsigned block = 0u; block < GPU_VRAM_REGION_WORDS_PER_ROW;) {
                            if (!(mask & (1u << block))) { ++block; continue; }
                            const unsigned first = block++;
                            while (block < GPU_VRAM_REGION_WORDS_PER_ROW && (mask & (1u << block))) ++block;
                            const size_t offset = (size_t)y * VRAM_W + first * 64u;
                            memcpy(S.upload_words + offset, work->words + offset, (block - first) * 64u * sizeof(uint16_t));
                            if ((int)(first * 64u) < left) left = first * 64u;
                            if ((int)(block * 64u) > right) right = block * 64u;
                        }
                        if ((int)y < top) top = y;
                        if ((int)y + 1 > bottom) bottom = y + 1u;
                    }
                } else {
                    memcpy(S.upload_words + command->y * VRAM_W + command->x,
                           captured_data + command->data, (size_t)command->w * rows * sizeof(uint16_t));
                    const unsigned first = (unsigned)command->x / 64u;
                    const unsigned last = ((unsigned)command->x + command->w - 1u) / 64u;
                    const uint16_t blocks = (uint16_t)(((1u << (last - first + 1u)) - 1u) << first);
                    for (int y = command->y; y < command->y + rows; ++y) R.reset_word_rows[y] |= blocks;
                    if (command->x < left) left = command->x;
                    if (command->y < top) top = command->y;
                    if (command->x + command->w > right) right = command->x + command->w;
                    if (command->y + rows > bottom) bottom = command->y + rows;
                }
                if (i + 1u == command_limit || (commands[i + 1u].kind != NATIVE_GPU_WORDS &&
                    commands[i + 1u].kind != NATIVE_GPU_WORDS_RESET)) break;
                ++i;
            } while (1);
            if (left < right && top < bottom) {
                R.c.word_uploads++;
                ok = nvk_words_copy(left, top, right, bottom);
            }
        } else if (command->kind == NATIVE_GPU_BASE) {
            /* Capture the target's VIEW plane, in FIFO order, as the base of the
             * snapshot recipe that begins at this declaration. */
            NvkPlane *source = command->plane < GL_NATIVE_GPU_PHASE_BASE ? &R.planes[command->plane] : NULL;
            if (source && source->color.image && command->source < GL_NATIVE_VIEW_TARGET_CAPACITY) {
                NvkPlane *base = &S.bases[command->source];
                ok = nvk_clone_plane_size(base, source);
                if (ok) nvk_plane_copy(source, base, 0);
                if (ok && source->has_key) {
                    ok = nvk_plane_depth(base);
                    if (ok) nvk_plane_copy(source, base, 1);
                }
            }
        } else if (command->kind == NATIVE_GPU_SNAPSHOT) {
            R.c.snapshot_commands++;
            /* plane 0: every VIEW domain; otherwise that one plane. */
            for (unsigned j = 0; j < GL_NATIVE_GPU_PLANES && ok; ++j)
                if (R.planes[j].color.image && (command->plane ? j == command->plane : j < GL_NATIVE_GPU_PHASE_BASE)) {
                    if (!R.snapshots[j].color.image && S.spare_snapshots[j].color.image) {
                        R.snapshots[j] = S.spare_snapshots[j];
                        memset(&S.spare_snapshots[j], 0, sizeof(S.spare_snapshots[j]));
                    }
                    ok = nvk_clone_plane_size(&R.snapshots[j], &R.planes[j]);
                    if (ok) nvk_plane_copy(&R.planes[j], &R.snapshots[j], 0);
                    if (ok && R.planes[j].has_key) {
                        ok = nvk_plane_depth(&R.snapshots[j]);
                        if (ok) nvk_plane_copy(&R.planes[j], &R.snapshots[j], 1);
                    }
                }
        } else if (command->kind == NATIVE_GPU_DRAW) {
            /* A pure DRAW run only samples the fixed raw-word texture and each
             * draw's own destination: planes are independent, FIFO within each. */
            uint32_t end = i;
            uint8_t planes[GL_NATIVE_GPU_PLANES] = {0};
            first_vertex = command_first_vertex;
            while (end < command_limit && end - i < 2048u && commands[end].kind == NATIVE_GPU_DRAW) {
                if (S.capture_enabled && end > i &&
                    (commands[end].draw.hd_texture.valid != command->draw.hd_texture.valid ||
                     (commands[end].source != 0u) != (command->source != 0u) ||
                     commands[end].draw.primitive.material.textured != command->draw.primitive.material.textured)) break;
                planes[commands[end].plane] = 1u;
                first_vertex += native_gpu_command_vertex_count(&commands[end]);
                ++end;
            }
            for (unsigned plane = 0u; plane < GL_NATIVE_GPU_PLANES && ok; ++plane) {
                if (!planes[plane]) continue;
                uint32_t vertex = command_first_vertex;
                const int scheduled = nvk_schedule_run(plane, commands, i, end, command_first_vertex, captured_data,
                    slice_vertices, geometry, NULL, 1);
                if (scheduled >= 0) { ok = scheduled; continue; }
                for (uint32_t draw = i; draw < end && ok; ++draw) {
                    if (commands[draw].plane == plane)
                        ok = nvk_render_command(&commands[draw], captured_data, vertex, slice_vertices,
                            geometry && geometry->primitives ? geometry->primitives + geometry->offsets[draw].primitive : NULL);
                    vertex += native_gpu_command_vertex_count(&commands[draw]);
                }
            }
            i = end - 1u;
        } else {
            ok = nvk_render_command(command, captured_data, command_first_vertex, slice_vertices,
                geometry && geometry->primitives ? geometry->primitives + geometry->offsets[i].primitive : NULL);
        }
        if (!ok) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_COMMAND, (uint32_t)(command - commands), command->kind);
    }
    if (!borrowed) free(slice_vertices);
    return ok;
}

/* ---- scanout (GL native_gpu_scanout_*) ---------------------------------------------------------------------------------------- */
static int nvk_scanout_image_impl(unsigned image);

static int nvk_scanout_image(unsigned image)
{
    const uint32_t query = nvk_detail_begin(2u);
    const int result = nvk_scanout_image_impl(image);
    nvk_detail_end(query);
    if (result) R.scanned_images[image] = 1u;
    return result;
}

static int nvk_scanout_image_impl(unsigned image)
{
    const GlNativeGpuWork *work = R.work;
    NvkPlane *output = &S.images[image];
    const uint32_t output_width = work->render_width ? work->render_width : work->scanout_width * work->scale;
    const uint32_t output_height = work->render_height ? work->render_height : work->scanout_height * work->scale;
    const float output_scale_x = (float)output_width / work->scanout_width;
    const float output_scale_y = (float)output_height / work->scanout_height;
    if (!nvk_plane_size(output, output_width, output_height, 1)) {
        nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_SCANOUT, image, 0u);
        return 0;
    }
    int background = work->disabled;
    if (!image && work->scanout_canonical_width < work->scanout_width)
        for (unsigned row = 0; row < work->scanout_height; ++row) background |= work->row_planes[row] == 0;
    nvk_pass_end();
    const VkClearValue clear[2] = { { .color = { .float32 = { 0.f, 0.f, 0.f, 1.f } } },
                                    { .color = { .float32 = { 1.f, 0.f, 0.f, 1.f } } } };
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = background ? S.pass_scan_clear : S.pass_scan_load;
    bi.framebuffer = output->framebuffer;
    bi.renderArea.extent = (VkExtent2D){ output->width, output->height };
    bi.clearValueCount = 2u;
    bi.pClearValues = clear;
    vkCmdBeginRenderPass(R.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport = { 0.f, 0.f, (float)output->width, (float)output->height, 0.f, 1.f };
    vkCmdSetViewport(R.cmd, 0u, 1u, &viewport);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe_scan);
    int ok = 1;
    for (unsigned y = 0; y < work->scanout_height && !work->disabled && ok;) {
        const int index = image ? (int)(GL_NATIVE_GPU_PHASE_BASE + image - 1u) : work->row_planes[y];
        const NvkPlane *input = &R.planes[index];
        const unsigned source_scale = native_gpu_plane_scale(work, index);
        const unsigned sy = image ? y + work->phase_crop_y : (y + work->scanout_y) & (VRAM_H - 1);
        unsigned rows = 1u;
        while (y + rows < work->scanout_height && (image || (sy + rows < VRAM_H && work->row_planes[y + rows] == index)) &&
            (!input->page_rows || (sy + rows) * source_scale / input->page_rows == sy * source_scale / input->page_rows)) ++rows;
        const unsigned width = index == 0 ? work->scanout_canonical_width : work->scanout_width;
        for (unsigned x = 0; x < width && ok;) {
            const unsigned source_width = input->width / source_scale;
            const unsigned sx = (x + (image ? work->phase_crop_x : index == 0 ? work->scanout_x : 0u)) % source_width;
            const unsigned dx = x + (index == 0 ? work->scanout_offset : 0u);
            const unsigned span = width - x > source_width - sx ? source_width - sx : width - x;
            const float source_scale_x = source_scale / input->raster[0];
            const float source_scale_y = source_scale / input->raster[1];
            const uint32_t virtual_y = sy * source_scale;
            const uint32_t page = input->page_rows ? virtual_y / input->page_rows : 0u;
            const float physical_y = page * input->page_pixels +
                (virtual_y - page * input->page_rows) / input->raster[1];
            const float mapping[4] = {
                sx * source_scale_x + input->raster[2] - dx * source_scale_x,
                physical_y - y * source_scale_y,
                source_scale_x / output_scale_x, source_scale_y / output_scale_y };
            uint32_t chunk;
            VkDeviceSize offset;
            uint8_t *slot = nvk_arena(NVK_SLOT_ALIGN, NVK_SLOT_ALIGN, &chunk, &offset);
            VkDescriptorSet set0 = nvk_scan_set0(input->color.view);
            VkDescriptorSet set1 = slot ? nvk_chunk_set(chunk, 1) : VK_NULL_HANDLE;
            if (!slot || !set0 || !set1) { ok = 0; break; }
            memset(slot, 0, NVK_SLOT_ALIGN);
            memcpy(slot, mapping, sizeof(mapping));
            const uint32_t dynamic = (uint32_t)offset;
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_scan, 0u, 1u, &set0, 0u, NULL);
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl_scan, 1u, 1u, &set1, 1u, &dynamic);
            const uint32_t x0 = native_resolution_edge(dx, work->scanout_width, output_width);
            const uint32_t y0 = native_resolution_edge(y, work->scanout_height, output_height);
            const int32_t rect[4] = { (int32_t)x0, (int32_t)y0,
                (int32_t)(native_resolution_edge(dx + span, work->scanout_width, output_width) - x0),
                (int32_t)(native_resolution_edge(y + rows, work->scanout_height, output_height) - y0) };
            const VkRect2D scissor = { { rect[0], rect[1] }, { (uint32_t)rect[2], (uint32_t)rect[3] } };
            vkCmdSetScissor(R.cmd, 0u, 1u, &scissor);
            ++P.draw_calls; vkCmdDraw(R.cmd, 3u, 1u, 0u, 0u);
            x += span;
        }
        y += rows;
    }
    vkCmdEndRenderPass(R.cmd);
    nvk_barrier_all();
    return ok;
}

static uint64_t native_image_mix64(uint64_t x)
{
    x = (x ^ (x >> 30u)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27u)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31u);
}

/* GL native_gpu_image_hash_dispatch: the order-sensitive fingerprint of the scanout image, one group result per workgroup. */
static int nvk_hash_record(const NvkPlane *image)
{
    const char *option = getenv("PSX_NATIVE_GPU_IMAGE_HASH");
    if (!S.pipe_hash || (option && option[0] == '0')) return 1;
    const uint64_t pixels = (uint64_t)image->width * image->height;
    if (!pixels || pixels > UINT32_MAX) return 1;
    const char *group_option = getenv("PSX_NATIVE_VK_HASH_GROUPS");
    uint32_t limit = group_option ? (uint32_t)strtoul(group_option, NULL, 10) : 128u;
    if (!limit || limit > 1024u) limit = 128u;
    const uint32_t groups = pixels > (uint64_t)limit * 128u ? limit : (uint32_t)((pixels + 127u) / 128u);
    VkDescriptorSet set = nvk_set_alloc(S.dsl_hash);
    if (!set) return 0;
    VkDescriptorBufferInfo buffer = { S.hash_results.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo source = { S.sampler_clamp, image->color.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof(w));
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set; w[0].dstBinding = 0u; w[0].descriptorCount = 1u;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &buffer;
    w[1] = nvk_write_image(set, 9u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source);
    vkUpdateDescriptorSets(vk_core_device(), 2u, w, 0u, NULL);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe_hash);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl_hash, 0u, 1u, &set, 0u, NULL);
    vkCmdDispatch(R.cmd, groups, 1u, 1u);
    R.hash_groups = groups;
    return 1;
}

static int nvk_readback_record(unsigned image)
{
    const NvkPlane *output = &S.images[image];
    const VkDeviceSize bytes = (VkDeviceSize)output->width * output->height * 4u;
    if (R.readback[image].buffer && R.readback[image].size != bytes) {
        vk_core_buffer_destroy(&R.readback[image]);
        memset(&R.readback[image], 0, sizeof(R.readback[image]));
    }
    if (!R.readback[image].buffer &&
        !vk_core_buffer_create(&R.readback[image], bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_CORE_MEMORY_READBACK,
                               "native readback"))
        return 0;
    R.readback_used[image] = 1;
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    region.imageExtent = (VkExtent3D){ output->width, output->height, 1u };
    vkCmdCopyImageToBuffer(R.cmd, output->color.image, VK_IMAGE_LAYOUT_GENERAL, R.readback[image].buffer, 1u, &region);
    return 1;
}

/* ---- fences -------------------------------------------------------------------------------------------------------------------- */
/* Non-blocking: the sync_poll of the NativeGpuBackend table. */
static XgRenderFenceStatus native_vk_sync_poll(void *sync)
{
    const VkResult r = vkGetFenceStatus(vk_core_device(), (VkFence)sync);
    if (r == VK_SUCCESS) return XG_RENDER_FENCE_READY;
    if (r == VK_NOT_READY) return XG_RENDER_FENCE_PENDING;
    (void)vk_core_check(r, "vkGetFenceStatus");
    return XG_RENDER_FENCE_FAILED;
}

/* ---- initialisation ---------------------------------------------------------------------------------------------------------------- */
static void nvk_note(const char *reason)
{
    snprintf(S.reason, sizeof(S.reason), "%s", reason);
}

static VkRenderPass nvk_make_pass(int draw, int clear)
{
    VkAttachmentDescription att[2];
    memset(att, 0, sizeof(att));
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[1].format = draw ? VK_FORMAT_R32_UINT : VK_FORMAT_R8_UNORM;
    for (unsigned i = 0u; i < 2u; ++i) {
        att[i].samples = VK_SAMPLE_COUNT_1_BIT;
        att[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[i].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        att[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    const VkAttachmentReference refs[2] = { { 0u, VK_IMAGE_LAYOUT_GENERAL }, { 1u, VK_IMAGE_LAYOUT_GENERAL } };
    VkSubpassDescription sub;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 2u;
    sub.pColorAttachments = refs;
    if (draw) { sub.inputAttachmentCount = 2u; sub.pInputAttachments = refs; }
    /* The destination reads of one draw see the earlier draws' writes only
     * behind this dependency (nvk_texture_barrier). */
    VkSubpassDependency dep;
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = 0u; dep.dstSubpass = 0u;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 2u; ci.pAttachments = att;
    ci.subpassCount = 1u; ci.pSubpasses = &sub;
    if (draw) { ci.dependencyCount = 1u; ci.pDependencies = &dep; }
    VkRenderPass pass = VK_NULL_HANDLE;
    return vk_core_check(vkCreateRenderPass(vk_core_device(), &ci, NULL, &pass), "vkCreateRenderPass") ? pass : VK_NULL_HANDLE;
}

/* The interlock pass: no attachments, the shader writes the planes as storage images. */
static VkRenderPass nvk_make_il_pass(void)
{
    VkSubpassDescription sub;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.subpassCount = 1u; ci.pSubpasses = &sub;
    VkRenderPass pass = VK_NULL_HANDLE;
    return vk_core_check(vkCreateRenderPass(vk_core_device(), &ci, NULL, &pass), "vkCreateRenderPass") ? pass : VK_NULL_HANDLE;
}

static VkDescriptorSetLayout nvk_make_layout(const VkDescriptorSetLayoutBinding *bindings, uint32_t count)
{
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = count; ci.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    return vk_core_check(vkCreateDescriptorSetLayout(vk_core_device(), &ci, NULL, &layout), "vkCreateDescriptorSetLayout")
        ? layout : VK_NULL_HANDLE;
}

static VkPipeline nvk_make_pipeline_specialized(VkPipelineLayout layout, VkRenderPass pass, const uint32_t *vs, size_t vs_bytes,
                                    const uint32_t *fs, size_t fs_bytes, int vertex_input, int key_written /* -1: no attachments */,
                                    const char *name, const VkSpecializationInfo *specialization)
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
        const VkBool32 first_records = S.first_vertex_records ? VK_TRUE : VK_FALSE;
        const VkSpecializationMapEntry first_entry = { 0u, 0u, sizeof(first_records) };
        const VkSpecializationInfo vertex_options = { 1u, &first_entry, sizeof(first_records), &first_records };
        stages[0].pSpecializationInfo = &vertex_options;
        VkVertexInputBindingDescription binding = { 0u, GL_NATIVE_GPU_VERTEX_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
        const VkVertexInputAttributeDescription attrs[4] = {
            { 0u, 0u, VK_FORMAT_R32G32_SFLOAT, 0u },
            { 1u, 0u, VK_FORMAT_R32G32_SFLOAT, 2u * sizeof(float) },
            { 2u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 4u * sizeof(float) },
            { 3u, 0u, VK_FORMAT_R32_SFLOAT, GL_NATIVE_GPU_VERTEX_WEIGHT * sizeof(float) },
        };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        if (vertex_input) {
            vi.vertexBindingDescriptionCount = 1u; vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 4u; vi.pVertexAttributeDescriptions = attrs;
        }
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1u; vp.scissorCount = 1u;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend[2];
        memset(blend, 0, sizeof(blend));
        blend[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend[1].colorWriteMask = key_written ? blend[0].colorWriteMask : 0u;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = key_written < 0 ? 0u : 2u; cb.pAttachments = blend;
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

static VkPipeline nvk_make_pipeline(VkPipelineLayout layout, VkRenderPass pass, const uint32_t *vs, size_t vs_bytes,
                                    const uint32_t *fs, size_t fs_bytes, int vertex_input, int key_written,
                                    const char *name)
{
    return nvk_make_pipeline_specialized(layout, pass, vs, vs_bytes, fs, fs_bytes, vertex_input, key_written, name, NULL);
}

static VkPipeline nvk_material_pipeline(int key_attached, int batch, const NvkDraw *draw, const NvkView *view)
{
    const VkPipeline fallback = R.capture_active ? S.pipe_capture_draw : batch ? S.pipe_core_batch[key_attached != 0] : S.pipe_draw[key_attached != 0];
    if (!S.specialize_materials || draw->depth_view[0] || R.work->wireframe) return fallback;
    const uint64_t key = ((uint32_t)draw->state[0] & 1u) | (((uint32_t)draw->state[1] & 1u) << 1u) |
        (((uint32_t)draw->state[2] & 1u) << 2u) | (((uint32_t)draw->flags[2] & 1u) << 3u) |
        (((uint32_t)draw->texture_filter & 1u) << 4u) | (((uint32_t)draw->anisotropy & 31u) << 5u) |
        (((uint32_t)draw->hd_on & 3u) << 10u) | (((uint32_t)draw->flags[3] & 3u) << 12u) |
        (((uint32_t)draw->depth_state[0] & 1u) << 14u) | (((uint32_t)draw->depth_state[2] & 1u) << 15u) |
        (((uint32_t)draw->flags[1] & 1u) << 16u) | (((uint32_t)draw->flags[0] & 1u) << 17u) |
        (((uint32_t)draw->mip_on & 1u) << 18u) | ((uint32_t)(key_attached != 0) << 19u) |
        (((uint32_t)draw->state[3] & 3u) << 20u) | (((uint32_t)draw->u_depth & 3u) << 22u) |
        ((uint32_t)((draw->u_depth >> 2) != 0) << 24u) |
        ((uint64_t)(uint32_t)draw->attribute_origin[2] << 25u) |
        ((uint64_t)(uint32_t)draw->attribute_origin[3] << 27u) |
        ((uint64_t)(uint32_t)draw->depth_state[1] << 28u) |
        ((uint64_t)(uint32_t)view->size[3] << 32u) |
        ((uint64_t)(uint32_t)draw->depth_plane[3] << 40u) |
        ((uint64_t)(uint32_t)draw->depth_state[3] << 48u) | ((uint64_t)(draw->decoded_on != 0) << 56u) |
        ((uint64_t)S.early_reject << 57u) | ((uint64_t)((draw->depth_state[0] & 2) != 0) << 58u) |
        ((uint64_t)(R.capture_active != 0) << 59u) | ((uint64_t)S.aniso_gather << 60u) |
        ((uint64_t)(R.damage_shader_active != 0) << 61u) | ((uint64_t)S.hardware_aniso << 62u) | ((uint64_t)(batch != 0) << 63u);
    for (uint32_t i = 0u; i < S.material_count; ++i)
        if (S.materials[i].key == key) return S.materials[i].pipeline;
    if (S.material_count == sizeof(S.materials) / sizeof(S.materials[0])) return fallback;
    int32_t values[27] = { draw->state[0], draw->state[1], draw->state[2], draw->state[3],
        draw->flags[0], draw->flags[1], draw->flags[2], draw->flags[3], draw->hd_on, draw->mip_on,
        draw->texture_filter, draw->anisotropy, draw->depth_state[0], draw->depth_state[2], draw->u_depth & 3, (draw->u_depth >> 2) != 0,
        0, 0, 0, draw->depth_plane[3], draw->depth_state[1], draw->depth_state[3], draw->decoded_on, S.early_reject, S.aniso_gather, R.damage_shader_active != 0, S.hardware_aniso };
    memcpy(&values[16], &draw->attribute_origin[2], sizeof(float));
    memcpy(&values[17], &draw->attribute_origin[3], sizeof(float));
    memcpy(&values[18], &view->size[3], sizeof(float));
    VkSpecializationMapEntry entries[27];
    for (uint32_t i = 0u; i < 27u; ++i) entries[i] = (VkSpecializationMapEntry){ i, i * sizeof(values[0]), sizeof(values[0]) };
    const VkSpecializationInfo info = { 27u, entries, sizeof(values), values };
    VkPipeline pipeline = batch && draw->hd_on && S.hd_record_buffer && !R.capture_active
        ? nvk_make_pipeline_specialized(S.pl_core_batch, S.pass_draw,
            spv_native_gpu_vert_CORE_BATCH_INDEX, spv_native_gpu_vert_CORE_BATCH_INDEX_size,
            spv_native_gpu_frag_CORE_BATCH_INDEX_SPECIALIZED, spv_native_gpu_frag_CORE_BATCH_INDEX_SPECIALIZED_size,
            1, key_attached, "native HD record buffer", &info)
        : R.capture_active && S.linked_capture && S.vertex_footprint ? nvk_make_pipeline_specialized(S.pl_core_batch, S.pass_capture,
        spv_native_gpu_vert_CORE_BATCH_FOOTPRINT, spv_native_gpu_vert_CORE_BATCH_FOOTPRINT_size,
        spv_native_gpu_frag_CAPTURE_LINKED_FOOTPRINT_SPECIALIZED, spv_native_gpu_frag_CAPTURE_LINKED_FOOTPRINT_SPECIALIZED_size,
        1, -1, "native linked fragment footprint", &info)
        : R.capture_active && S.linked_capture ? nvk_make_pipeline_specialized(S.pl_core_batch, S.pass_capture,
        spv_native_gpu_vert_CORE_BATCH, spv_native_gpu_vert_CORE_BATCH_size,
        spv_native_gpu_frag_CAPTURE_LINKED_SPECIALIZED, spv_native_gpu_frag_CAPTURE_LINKED_SPECIALIZED_size,
        1, -1, "native linked fragment capture", &info)
        : R.capture_active ? nvk_make_pipeline_specialized(S.pl_core_batch, S.pass_capture,
        spv_native_gpu_vert_CORE_BATCH, spv_native_gpu_vert_CORE_BATCH_size,
        spv_native_gpu_frag_CAPTURE_SPECIALIZED, spv_native_gpu_frag_CAPTURE_SPECIALIZED_size,
        1, -1, "native ordered fragments", &info)
        : batch && S.vertex_footprint ? nvk_make_pipeline_specialized(S.pl_core_batch, S.pass_draw,
        spv_native_gpu_vert_CORE_BATCH_FOOTPRINT, spv_native_gpu_vert_CORE_BATCH_FOOTPRINT_size,
        spv_native_gpu_frag_CORE_BATCH_FOOTPRINT_SPECIALIZED, spv_native_gpu_frag_CORE_BATCH_FOOTPRINT_SPECIALIZED_size,
        1, key_attached != 0, "native specialized core footprint", &info)
        : batch ? nvk_make_pipeline_specialized(S.pl_core_batch, S.pass_draw,
        spv_native_gpu_vert_CORE_BATCH, spv_native_gpu_vert_CORE_BATCH_size,
        spv_native_gpu_frag_CORE_BATCH_SPECIALIZED, spv_native_gpu_frag_CORE_BATCH_SPECIALIZED_size,
        1, key_attached != 0, "native specialized core batch", &info)
        : nvk_make_pipeline_specialized(S.pl_native, S.pass_draw,
        spv_native_gpu_vert, spv_native_gpu_vert_size,
        spv_native_gpu_frag_SPECIALIZED, spv_native_gpu_frag_SPECIALIZED_size,
        1, key_attached != 0, "native specialized material", &info);
    if (!pipeline) return fallback;
    S.materials[S.material_count].key = key;
    S.materials[S.material_count++].pipeline = pipeline;
    return pipeline;
}

static int nvk_make_sampler(VkSampler *out, VkSamplerAddressMode mode)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = mode;
    ci.maxLod = 0.f;
    return vk_core_check(vkCreateSampler(vk_core_device(), &ci, NULL, out), "vkCreateSampler");
}

/* HD replacements: the GL texture parameters of hd_gl_upload (linear, linear mips, clamp). */
static int nvk_make_hd_sampler(VkSampler *out)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    return vk_core_check(vkCreateSampler(vk_core_device(), &ci, NULL, out), "vkCreateSampler");
}

static void nvk_teardown(void)
{
    nvk_submit_thread_stop();
    if (!vk_core_ready() || !S.tried) { memset(&S.ready, 0, sizeof(S.ready)); return; }
    VkDevice dev = vk_core_device();
    (void)vk_core_device_wait_idle();
    nvk_schedule_cache_clear();
    free(s_sched_max_layer); s_sched_max_layer = NULL; s_sched_max_capacity = 0u;
    free(s_sched_emission); s_sched_emission = NULL; s_sched_emission_capacity = 0u;
    free(s_sched_order); s_sched_order = NULL; s_sched_order_capacity = 0u;
    free(s_sched_depth_fast); s_sched_depth_fast = NULL; s_sched_depth_capacity = 0u;
    for (uint32_t i = 0u; i < 128u; ++i) {
        free(s_capture_batches[i]); s_capture_batches[i] = NULL;
    }
    s_capture_batch_count = 0u;
    R.recording = 0;
    for (uint32_t i = 0u; i < GL_NATIVE_GPU_PHASE_BASE; ++i) nvk_plane_free(&S.committed[i]);
    for (uint32_t i = 0u; i < GL_NATIVE_VIEW_TARGET_CAPACITY; ++i) nvk_plane_free(&S.bases[i]);
    for (uint32_t i = 0u; i < GL_NATIVE_GPU_PLANES; ++i) {
        nvk_plane_free(&S.spare[i]); nvk_plane_free(&S.work_planes[i]);
        nvk_plane_free(&S.snapshots[i]); nvk_plane_free(&S.spare_snapshots[i]);
    }
    for (uint32_t i = 0u; i < NVK_IMAGES; ++i) nvk_plane_free(&S.images[i]);
    for (uint32_t i = 0u; i < S.garbage_count; ++i) nvk_plane_destroy(&S.garbage[i]);
    free(S.garbage);
    {
        const HdCacheOps ops = { nvk_hd_upload, nvk_hd_destroy, NULL };
        hd_cache_clear(&s_hd_cache, &ops);
        free(s_hd_cache.entries); free(s_hd_cache.scratch);
        memset(&s_hd_cache, 0, sizeof(s_hd_cache));
        nvk_hd_free_garbage();
        free(S.hd_garbage);
    }
    for (uint32_t i = 0u; i < S.seed_cap; ++i) vk_core_image_destroy(&S.seeds[i].image);
    free(S.seeds);
    for (uint32_t i = 0u; i < S.chunk_count; ++i) {
        if (S.chunks[i].params_view) vkDestroyBufferView(dev, S.chunks[i].params_view, NULL);
        vk_core_buffer_destroy(&S.chunks[i].buf);
    }
    free(S.chunks);
    for (uint32_t i = 0u; i < S.pool_count; ++i) vkDestroyDescriptorPool(dev, S.pools[i], NULL);
    free(S.pools);
    free(S.set0);
    if (S.dummy_view) vkDestroyBufferView(dev, S.dummy_view, NULL);
    vk_core_buffer_destroy(&S.dummy_texels);
    vk_core_image_destroy(&S.dummy_color); vk_core_image_destroy(&S.dummy_uint); vk_core_image_destroy(&S.words);
    if (S.fence) vkDestroyFence(dev, S.fence, NULL);
    if (S.recipe_view) vkDestroyBufferView(dev, S.recipe_view, NULL);
    vk_core_buffer_destroy(&R.recipe_buffer);
    for (unsigned i = 0u; i < NVK_IMAGES; ++i) vk_core_buffer_destroy(&R.readback[i]);
    if (P.pool) { vkDestroyQueryPool(dev, P.pool, NULL); P.pool = VK_NULL_HANDLE; }
    if (P.submit_pool) { vkDestroyQueryPool(dev, P.submit_pool, NULL); P.submit_pool = VK_NULL_HANDLE; }
    if (S.pool) vkDestroyCommandPool(dev, S.pool, NULL);
    if (S.sampler_clamp) vkDestroySampler(dev, S.sampler_clamp, NULL);
    for (uint32_t i = 0u; i < 5u; ++i) if (S.sampler_aniso[i]) vkDestroySampler(dev, S.sampler_aniso[i], NULL);
    if (S.sampler_repeat) vkDestroySampler(dev, S.sampler_repeat, NULL);
    if (S.sampler_hd) vkDestroySampler(dev, S.sampler_hd, NULL);
    for (unsigned i = 0u; i < 2u; ++i) if (S.pipe_draw[i]) vkDestroyPipeline(dev, S.pipe_draw[i], NULL);
    for (unsigned i = 0u; i < 2u; ++i) if (S.pipe_core_batch[i]) vkDestroyPipeline(dev, S.pipe_core_batch[i], NULL);
    for (uint32_t i = 0u; i < S.material_count; ++i) vkDestroyPipeline(dev, S.materials[i].pipeline, NULL);
    for (uint32_t i = 0u; i < sizeof(S.decoded) / sizeof(S.decoded[0]); ++i) {
        vk_core_image_destroy(&S.decoded[i].image); vk_core_image_destroy(&S.decoded[i].filtered); free(S.decoded[i].source);
    }
    for (uint32_t i = 0u; i < 8u; ++i)
        if (S.pace_fences[i]) vkDestroyFence(dev, S.pace_fences[i], NULL);
    vk_core_image_destroy(&S.decoded_array); vk_core_image_destroy(&S.filtered_array);
    if (S.pipe_decode) vkDestroyPipeline(dev, S.pipe_decode, NULL);
    if (S.pl_decode) vkDestroyPipelineLayout(dev, S.pl_decode, NULL);
    if (S.dsl_decode) vkDestroyDescriptorSetLayout(dev, S.dsl_decode, NULL);
    if (S.pipe_capture_draw) vkDestroyPipeline(dev, S.pipe_capture_draw, NULL);
    if (S.pipe_capture_count) vkDestroyPipeline(dev, S.pipe_capture_count, NULL);
    if (S.pipe_capture_resolve) vkDestroyPipeline(dev, S.pipe_capture_resolve, NULL);
    if (S.pl_capture) vkDestroyPipelineLayout(dev, S.pl_capture, NULL);
    if (S.dsl_capture) vkDestroyDescriptorSetLayout(dev, S.dsl_capture, NULL);
    if (S.pass_capture) vkDestroyRenderPass(dev, S.pass_capture, NULL);
    vk_core_buffer_destroy(&S.capture_meta); vk_core_buffer_destroy(&S.capture_nodes); vk_core_buffer_destroy(&S.capture_readback);
    if (S.pipe_scan) vkDestroyPipeline(dev, S.pipe_scan, NULL);
    if (S.pipe_il) vkDestroyPipeline(dev, S.pipe_il, NULL);
    if (S.pipe_hash) vkDestroyPipeline(dev, S.pipe_hash, NULL);
    if (S.pl_hash) vkDestroyPipelineLayout(dev, S.pl_hash, NULL);
    if (S.dsl_hash) vkDestroyDescriptorSetLayout(dev, S.dsl_hash, NULL);
    vk_core_buffer_destroy(&S.hash_results);
    SDL_AtomicSet(&s_native_gpu_setup_ready, 0);
    s_native_gpu_snapshot_capacity = 0u;
    if (S.pipe_setup) vkDestroyPipeline(dev, S.pipe_setup, NULL);
    if (S.pl_setup) vkDestroyPipelineLayout(dev, S.pl_setup, NULL);
    if (S.dsl_setup0) vkDestroyDescriptorSetLayout(dev, S.dsl_setup0, NULL);
    if (S.dsl_setup1) vkDestroyDescriptorSetLayout(dev, S.dsl_setup1, NULL);
    if (S.tpl_view) vkDestroyBufferView(dev, S.tpl_view, NULL);
    vk_core_buffer_destroy(&S.tpl_buffer);
    if (S.pl_il) vkDestroyPipelineLayout(dev, S.pl_il, NULL);
    if (S.pl_native) vkDestroyPipelineLayout(dev, S.pl_native, NULL);
    if (S.pl_core_batch) vkDestroyPipelineLayout(dev, S.pl_core_batch, NULL);
    if (S.pl_scan) vkDestroyPipelineLayout(dev, S.pl_scan, NULL);
    VkDescriptorSetLayout layouts[5] = { S.dsl_native0, S.dsl_native1, S.dsl_scan0, S.dsl_scan1, S.dsl_il0 };
    for (unsigned i = 0u; i < 5u; ++i) if (layouts[i]) vkDestroyDescriptorSetLayout(dev, layouts[i], NULL);
    VkRenderPass passes[4] = { S.pass_draw, S.pass_scan_clear, S.pass_scan_load, S.pass_il };
    for (unsigned i = 0u; i < 4u; ++i) if (passes[i]) vkDestroyRenderPass(dev, passes[i], NULL);
    SDL_mutex *mutex = S.mutex;
    memset(&S, 0, sizeof(S));
    S.mutex = mutex;
}

/* Creates the objects of the executor; the first command buffer initialises the
 * dummies and the words image. */
static int nvk_init(void)
{
    const VkCoreCaps *caps = vk_core_caps();
    if (caps->min_uniform_buffer_offset_alignment > NVK_SLOT_ALIGN) { nvk_note("uniform offset alignment above 256"); return 0; }
    if (caps->max_texel_buffer_elements < 524288u) { nvk_note("texel buffers smaller than one VRAM snapshot"); return 0; }
    VkDevice dev = vk_core_device();
    const char *tier = getenv("PSX_NATIVE_GPU_INTERLOCK");
    S.interlock = caps->fragment_shader_interlock && caps->fragment_stores_and_atomics &&
        caps->max_texel_buffer_elements >= NVK_ARENA_CHUNK / 16u && !(tier && tier[0] == '0');
    const char *specialize = getenv("PSX_NATIVE_VK_SPECIALIZE");
    S.specialize_materials = !S.interlock && !(specialize && specialize[0] == '0');
    const char *hardware_aniso = getenv("PSX_NATIVE_VK_HARDWARE_ANISO");
    S.hardware_aniso = S.specialize_materials && caps->sampler_anisotropy && caps->max_sampler_anisotropy >= 16.f &&
        !(hardware_aniso && hardware_aniso[0] == '0');
    if (S.hardware_aniso) {
        VkFormatProperties format;
        vkGetPhysicalDeviceFormatProperties(vk_core_physical_device(), VK_FORMAT_R8G8B8A8_UNORM, &format);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        S.hardware_aniso = (format.optimalTilingFeatures & required) == required;
    }
    const char *early = getenv("PSX_NATIVE_VK_EARLY_REJECT");
    S.early_reject = early && early[0] == '1';
    const char *single_tap = getenv("PSX_NATIVE_VK_ANISO_SINGLE_TAP");
    S.aniso_single_tap = single_tap && single_tap[0] == '2' ? 2 : single_tap && single_tap[0] == '1' ? 1 : 0;
    const char *gather = getenv("PSX_NATIVE_VK_ANISO_GATHER");
    S.aniso_gather = gather && gather[0] == '1';
    const char *sort = getenv("PSX_NATIVE_VK_SORT_MATERIALS");
    S.sort_materials = sort && sort[0] == '1';
    const char *sort_hd = getenv("PSX_NATIVE_VK_SORT_HD_READERS");
    S.sort_hd_readers = sort_hd && sort_hd[0] == '1';
    const char *core_batch = getenv("PSX_NATIVE_VK_CORE_BATCH");
    S.core_batch = !S.interlock && caps->max_texel_buffer_elements >= NVK_ARENA_CHUNK / 16u && !(core_batch && core_batch[0] == '0');
    const char *publish = getenv("PSX_NATIVE_VK_PUBLISH_BARRIERS");
    S.publish_barriers = publish && publish[0] == '1';
    const char *queue_ahead = getenv("PSX_NATIVE_VK_QUEUE_AHEAD");
    if (S.core_batch && queue_ahead && queue_ahead[0] >= '1' && queue_ahead[0] <= '8') {
        S.pace_limit = (uint32_t)(queue_ahead[0] - '0');
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (uint32_t i = 0u; i < S.pace_limit; ++i)
            if (!nvk_ok(vkCreateFence(dev, &fi, NULL, &S.pace_fences[i]), "native pace fence")) return 0;
    }
    const char *defer_submits = getenv("PSX_NATIVE_VK_DEFER_SUBMITS");
    S.defer_submits = S.pace_limit && defer_submits && defer_submits[0] == '1';
    const char *pace_margin = getenv("PSX_NATIVE_VK_QUEUE_MARGIN_MS");
    if (S.pace_limit > 1u && pace_margin) {
        const unsigned long margin = strtoul(pace_margin, NULL, 10);
        if (margin > 0u && margin <= 1000u) S.pace_margin_ns = (uint64_t)margin * 1000000u;
    }
    const char *phase_seal = getenv("PSX_NATIVE_VK_PHASE_SEAL");
    S.phase_seal = phase_seal && phase_seal[0] == '1';
    const char *hd_batch = getenv("PSX_NATIVE_VK_HD_BATCH");
    S.hd_batch = hd_batch && hd_batch[0] == '1';
    const char *hd_records = getenv("PSX_NATIVE_VK_HD_RECORD_BUFFER");
    S.hd_record_buffer = hd_records && hd_records[0] == '1';
    const char *capture = getenv("PSX_NATIVE_VK_FRAGMENT_LISTS");
    S.capture_enabled = S.core_batch && caps->fragment_stores_and_atomics && capture && capture[0] == '1';
    const char *merge_capture = getenv("PSX_NATIVE_VK_MERGE_CAPTURE");
    S.merge_capture = merge_capture && merge_capture[0] == '1';
    const char *sort_capture = getenv("PSX_NATIVE_VK_SORT_CAPTURE");
    S.sort_capture = sort_capture && sort_capture[0] == '1';
    const char *direct_capture = getenv("PSX_NATIVE_VK_DIRECT_CAPTURE");
    S.direct_capture = direct_capture && direct_capture[0] == '1';
    if (S.capture_enabled) {
        VkFormatProperties rgba, depth;
        vkGetPhysicalDeviceFormatProperties(vk_core_physical_device(), VK_FORMAT_R8G8B8A8_UNORM, &rgba);
        vkGetPhysicalDeviceFormatProperties(vk_core_physical_device(), VK_FORMAT_R32_UINT, &depth);
        S.capture_enabled = (rgba.optimalTilingFeatures & depth.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        const char *linked = getenv("PSX_NATIVE_VK_LINKED_FRAGMENTS");
        if (linked && linked[0] == '1') {
            VkPhysicalDeviceSubgroupProperties subgroup = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
            VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            properties.pNext = &subgroup;
            vkGetPhysicalDeviceProperties2(vk_core_physical_device(), &properties);
            const VkSubgroupFeatureFlags required = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
            S.linked_capture = (subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                (subgroup.supportedOperations & required) == required;
        }
    }
    const char *footprint = getenv("PSX_NATIVE_VK_VERTEX_FOOTPRINT");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk_core_physical_device(), &properties);
    const char *first_records = getenv("PSX_NATIVE_VK_FIRST_VERTEX_RECORDS");
    S.first_vertex_records = first_records && first_records[0] == '1';
    const char *lazy_vertices = getenv("PSX_NATIVE_VK_LAZY_VERTICES");
    S.lazy_vertices = S.core_batch && lazy_vertices && lazy_vertices[0] == '1';
    S.vertex_footprint = properties.limits.maxVertexOutputComponents >= 72u &&
        properties.limits.maxFragmentInputComponents >= 72u && !(footprint && footprint[0] == '0');
    const char *tile = getenv("PSX_NATIVE_VK_DAMAGE_TILE");
    S.damage_tile_shift = tile && !strcmp(tile, "16") ? 4u : tile && !strcmp(tile, "32") ? 5u : 6u;
    const char *damage = getenv("PSX_NATIVE_VK_PHASE_DAMAGE");
    S.phase_damage = !(damage && damage[0] == '0');
    const char *shader_damage = getenv("PSX_NATIVE_VK_SHADER_DAMAGE");
    S.shader_damage = !(shader_damage && shader_damage[0] == '0');
    const char *regions = getenv("PSX_NATIVE_VK_SMAA_REGIONS");
    S.damage_regions = regions && regions[0] == '2' ? 4 : regions && regions[0] == '1' ? 2 : 0;
    const char *depth_bounds = getenv("PSX_NATIVE_VK_DEPTH_BOUNDS");
    S.depth_bounds = !S.interlock && depth_bounds && depth_bounds[0] == '1';
    S.submit_draw_limit = NVK_SUBMIT_DRAWS;
    const char *clear_spans = getenv("PSX_NATIVE_VK_CLEAR_SPANS");
    S.clear_spans = clear_spans && clear_spans[0] == '1';
    const char *submit_limit = getenv("PSX_NATIVE_VK_SUBMIT_DRAWS");
    if (submit_limit) {
        const unsigned long limit = strtoul(submit_limit, NULL, 10);
        if (limit >= 64u && limit <= 16384u) S.submit_draw_limit = (uint32_t)limit;
    }
    /* Phase works share one texture-snapshot buffer (GL GL_MAX_TEXTURE_BUFFER_SIZE / one VRAM). */
    s_native_gpu_snapshot_capacity = caps->max_texel_buffer_elements / (VRAM_W * VRAM_H);
    S.pass_draw = nvk_make_pass(1, 0);
    S.pass_scan_clear = nvk_make_pass(0, 1);
    S.pass_scan_load = nvk_make_pass(0, 0);
    if (S.capture_enabled) {
        S.pass_capture = nvk_make_il_pass();
        if (!S.pass_capture) return 0;
    }
    if (!S.pass_draw || !S.pass_scan_clear || !S.pass_scan_load) { nvk_note("render pass creation failed"); return 0; }
    const VkShaderStageFlags frag = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding native0[14] = {
        { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 1u, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1u, frag, NULL },
        { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 3u, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1u, frag, NULL },
        { 4u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 8u, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u, frag, NULL },
        { 7u, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u, VK_SHADER_STAGE_VERTEX_BIT | frag, NULL },
        { 11u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 12u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, frag, NULL },
        { 13u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, frag, NULL },
        { 14u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
        { 15u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL },
    };
    const VkDescriptorSetLayoutBinding native1[2] = {
        { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, VK_SHADER_STAGE_VERTEX_BIT | frag, NULL },
        { 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, frag, NULL },
    };
    const VkDescriptorSetLayoutBinding scan0 = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, frag, NULL };
    const VkDescriptorSetLayoutBinding scan1 = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, frag, NULL };
    S.dsl_native0 = nvk_make_layout(native0, 14u);
    S.dsl_native1 = nvk_make_layout(native1, 2u);
    S.dsl_scan0 = nvk_make_layout(&scan0, 1u);
    S.dsl_scan1 = nvk_make_layout(&scan1, 1u);
    if (!S.dsl_native0 || !S.dsl_native1 || !S.dsl_scan0 || !S.dsl_scan1) { nvk_note("descriptor layout creation failed"); return 0; }
    VkPipelineLayoutCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout native_sets[2] = { S.dsl_native0, S.dsl_native1 };
    pci.setLayoutCount = 2u; pci.pSetLayouts = native_sets;
    /* Identical push ranges keep loose and batched descriptor bindings
     * compatible when an HD/transfer draw interrupts a material batch. */
    const VkPushConstantRange batch_range[2] = {
        { VK_SHADER_STAGE_VERTEX_BIT, 0u, sizeof(uint32_t) },
        { VK_SHADER_STAGE_FRAGMENT_BIT, 4u, 4u * sizeof(int32_t) }
    };
    pci.pushConstantRangeCount = 2u; pci.pPushConstantRanges = batch_range;
    if (!vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &S.pl_native), "vkCreatePipelineLayout")) return 0;
    if (S.core_batch) {
        if (!vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &S.pl_core_batch), "vkCreatePipelineLayout")) return 0;
    }
    pci.pushConstantRangeCount = 0u; pci.pPushConstantRanges = NULL;
    VkDescriptorSetLayout scan_sets[2] = { S.dsl_scan0, S.dsl_scan1 };
    pci.pSetLayouts = scan_sets;
    if (!vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &S.pl_scan), "vkCreatePipelineLayout")) return 0;
    S.pipe_draw[0] = nvk_make_pipeline(S.pl_native, S.pass_draw, spv_native_gpu_vert, spv_native_gpu_vert_size,
                                       spv_native_gpu_frag, spv_native_gpu_frag_size, 1, 0, "native draw");
    S.pipe_draw[1] = nvk_make_pipeline(S.pl_native, S.pass_draw, spv_native_gpu_vert, spv_native_gpu_vert_size,
                                       spv_native_gpu_frag, spv_native_gpu_frag_size, 1, 1, "native draw key");
    if (S.core_batch) for (unsigned key = 0u; key < 2u; ++key) {
        S.pipe_core_batch[key] = S.vertex_footprint ? nvk_make_pipeline(S.pl_core_batch, S.pass_draw,
            spv_native_gpu_vert_CORE_BATCH_FOOTPRINT, spv_native_gpu_vert_CORE_BATCH_FOOTPRINT_size,
            spv_native_gpu_frag_CORE_BATCH_FOOTPRINT, spv_native_gpu_frag_CORE_BATCH_FOOTPRINT_size, 1, key, "native core footprint")
            : nvk_make_pipeline(S.pl_core_batch, S.pass_draw,
            spv_native_gpu_vert_CORE_BATCH, spv_native_gpu_vert_CORE_BATCH_size,
            spv_native_gpu_frag_CORE_BATCH, spv_native_gpu_frag_CORE_BATCH_size, 1, key, "native core batch");
        if (!S.pipe_core_batch[key]) return 0;
    }
    if (S.capture_enabled) {
        const VkDescriptorSetLayoutBinding bindings[4] = {
            { 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL }
        };
        S.dsl_capture = nvk_make_layout(bindings, 4u);
        VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        const VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0u, 48u };
        layout.setLayoutCount = 1u; layout.pSetLayouts = &S.dsl_capture;
        layout.pushConstantRangeCount = 1u; layout.pPushConstantRanges = &range;
        if (!S.dsl_capture || !vk_core_check(vkCreatePipelineLayout(dev, &layout, NULL, &S.pl_capture), "capture layout")) return 0;
        S.pipe_capture_count = nvk_make_pipeline(S.pl_capture, S.pass_capture,
            spv_native_capture_count_vert, spv_native_capture_count_vert_size,
            spv_native_capture_count_frag, spv_native_capture_count_frag_size, 1, -1, "native coverage count");
        S.pipe_capture_draw = S.linked_capture && S.vertex_footprint ? nvk_make_pipeline(S.pl_core_batch, S.pass_capture,
            spv_native_gpu_vert_CORE_BATCH_FOOTPRINT, spv_native_gpu_vert_CORE_BATCH_FOOTPRINT_size,
            spv_native_gpu_frag_CAPTURE_LINKED_FOOTPRINT, spv_native_gpu_frag_CAPTURE_LINKED_FOOTPRINT_size, 1, -1, "native linked footprint")
            : S.linked_capture ? nvk_make_pipeline(S.pl_core_batch, S.pass_capture,
            spv_native_gpu_vert_CORE_BATCH, spv_native_gpu_vert_CORE_BATCH_size,
            spv_native_gpu_frag_CAPTURE_LINKED, spv_native_gpu_frag_CAPTURE_LINKED_size, 1, -1, "native linked fragment capture")
            : nvk_make_pipeline(S.pl_core_batch, S.pass_capture,
            spv_native_gpu_vert_CORE_BATCH, spv_native_gpu_vert_CORE_BATCH_size,
            spv_native_gpu_frag_CAPTURE, spv_native_gpu_frag_CAPTURE_size, 1, -1, "native fragment capture");
        VkShaderModule module = S.linked_capture
            ? vk_core_shader_module(spv_native_capture_linked_comp, spv_native_capture_linked_comp_size, "native linked resolve")
            : vk_core_shader_module(spv_native_capture_resolve_comp, spv_native_capture_resolve_comp_size, "native ordered resolve");
        VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        const char *short_option = getenv("PSX_NATIVE_VK_CAPTURE_SHORT_LISTS");
        const VkBool32 short_lists = short_option && short_option[0] == '1';
        const VkSpecializationMapEntry short_entry = { 0u, 0u, sizeof(short_lists) };
        const VkSpecializationInfo short_info = { 1u, &short_entry, sizeof(short_lists), &short_lists };
        pi.stage.pSpecializationInfo = S.linked_capture ? &short_info : NULL;
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = module; pi.stage.pName = "main";
        pi.layout = S.pl_capture;
        const int ok = module && vk_core_check(vkCreateComputePipelines(dev, vk_core_pipeline_cache(), 1u, &pi, NULL, &S.pipe_capture_resolve), "capture resolve pipeline");
        if (module) vkDestroyShaderModule(dev, module, NULL);
        if (!ok || !S.pipe_capture_count || !S.pipe_capture_draw) return 0;
    }
    if (S.interlock) {
        const VkShaderStageFlags fs = VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding il0[9] = {
            { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, fs, NULL },
            { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, fs, NULL },
            { 4u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, fs, NULL },
            { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, fs, NULL },
            { 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, fs, NULL },
            { 7u, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u, fs, NULL },
            { 8u, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u, fs, NULL },
            { 9u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, fs, NULL },
            { 10u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, fs, NULL },
        };
        S.dsl_il0 = nvk_make_layout(il0, 9u);
        S.pass_il = nvk_make_il_pass();
        VkDescriptorSetLayout il_sets[2] = { S.dsl_il0, S.dsl_native1 };
        pci.pSetLayouts = il_sets;
        if (!S.dsl_il0 || !S.pass_il || !vk_core_check(vkCreatePipelineLayout(dev, &pci, NULL, &S.pl_il), "vkCreatePipelineLayout")) {
            nvk_note("interlock layout creation failed");
            return 0;
        }
        const char *generic = getenv("PSX_NATIVE_GENERIC_RASTER");
        S.pipe_il = generic && generic[0] == '1'
            ? nvk_make_pipeline(S.pl_il, S.pass_il, spv_native_gpu_vert_INTERLOCK, spv_native_gpu_vert_INTERLOCK_size,
                                spv_native_gpu_frag_INTERLOCK_GENERIC_RASTER, spv_native_gpu_frag_INTERLOCK_GENERIC_RASTER_size,
                                1, -1, "native interlock generic")
            : nvk_make_pipeline(S.pl_il, S.pass_il, spv_native_gpu_vert_INTERLOCK, spv_native_gpu_vert_INTERLOCK_size,
                                spv_native_gpu_frag_INTERLOCK, spv_native_gpu_frag_INTERLOCK_size, 1, -1, "native interlock");
        if (!S.pipe_il) { nvk_note("interlock pipeline creation failed"); return 0; }
    }
    /* Fingerprinting only needs compute/int64, not fragment interlock. Keep
     * core-tier scanout GPU-resident too instead of downloading every frame. */
    S.gpu_compute = caps->shader_int64;
    if (S.gpu_compute) {
        const VkDescriptorSetLayoutBinding hash[2] = {
            { 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 9u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        };
        S.dsl_hash = nvk_make_layout(hash, 2u);
        VkPipelineLayoutCreateInfo hci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        hci.setLayoutCount = 1u; hci.pSetLayouts = &S.dsl_hash;
        VkShaderModule module = vk_core_shader_module(spv_image_hash_comp, spv_image_hash_comp_size, "image hash");
        if (S.dsl_hash && module && vk_core_check(vkCreatePipelineLayout(dev, &hci, NULL, &S.pl_hash), "vkCreatePipelineLayout") &&
            vk_core_buffer_create(&S.hash_results, 1024u * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_CORE_MEMORY_READBACK,
                                  "native image hash")) {
            VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = module; pi.stage.pName = "main";
            pi.layout = S.pl_hash;
            if (!vk_core_check(vkCreateComputePipelines(dev, vk_core_pipeline_cache(), 1u, &pi, NULL, &S.pipe_hash),
                               "vkCreateComputePipelines")) S.pipe_hash = VK_NULL_HANDLE;
        }
        if (module) vkDestroyShaderModule(dev, module, NULL);
        if (!S.pipe_hash) { nvk_note("image fingerprint pipeline creation failed"); return 0; }
        if (S.interlock && caps->shader_float64) {
            /* GPU triangle setup (GL native_gpu_setup_init) */
            const VkDescriptorSetLayoutBinding setup0[2] = {
                { 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                { 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            };
            const VkDescriptorSetLayoutBinding setup1[1] = {
                { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            };
            S.dsl_setup0 = nvk_make_layout(setup0, 2u);
            S.dsl_setup1 = nvk_make_layout(setup1, 1u);
            const VkDescriptorSetLayout setup_layouts[2] = { S.dsl_setup0, S.dsl_setup1 };
            VkPipelineLayoutCreateInfo sci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            sci.setLayoutCount = 2u; sci.pSetLayouts = setup_layouts;
            VkShaderModule smodule = vk_core_shader_module(spv_native_setup_comp, spv_native_setup_comp_size, "native setup");
            if (S.dsl_setup0 && S.dsl_setup1 && smodule &&
                vk_core_check(vkCreatePipelineLayout(dev, &sci, NULL, &S.pl_setup), "vkCreatePipelineLayout")) {
                VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = smodule; pi.stage.pName = "main";
                pi.layout = S.pl_setup;
                if (!vk_core_check(vkCreateComputePipelines(dev, vk_core_pipeline_cache(), 1u, &pi, NULL, &S.pipe_setup),
                                   "vkCreateComputePipelines")) S.pipe_setup = VK_NULL_HANDLE;
            }
            if (smodule) vkDestroyShaderModule(dev, smodule, NULL);
            if (S.pipe_setup) {
                const VkDeviceSize limit = vk_core_caps()->max_storage_buffer_range;
                S.setup_max_triangles = (uint32_t)(limit / 128u);
                if (S.setup_max_triangles > 65535u * 64u) S.setup_max_triangles = 65535u * 64u;
                const char *option = getenv("PSX_NATIVE_GPU_SETUP");
                if (S.setup_max_triangles && !(option && option[0] == '0')) SDL_AtomicSet(&s_native_gpu_setup_ready, 1);
            }
        }
    }
    S.pipe_scan = nvk_make_pipeline(S.pl_scan, S.pass_scan_load, spv_fullscreen_vert, spv_fullscreen_vert_size,
                                    spv_scanout_resolve_frag, spv_scanout_resolve_frag_size, 0, 1, "native scanout");
    const char *arrays = getenv("PSX_NATIVE_VK_DECODE_ARRAYS");
    S.decode_arrays = arrays && arrays[0] == '1';
    const char *decode = getenv("PSX_NATIVE_VK_DECODE_TEXTURES");
    const char *persistent_decode = getenv("PSX_NATIVE_VK_PERSISTENT_DECODE");
    S.persistent_decode = S.hardware_aniso || (persistent_decode && persistent_decode[0] == '1');
    const char *decode_hints = getenv("PSX_NATIVE_VK_DECODE_HINTS");
    S.decode_hints = decode_hints && decode_hints[0] == '1';
    const char *descriptor_hints = getenv("PSX_NATIVE_VK_DESCRIPTOR_HINTS");
    S.descriptor_hints = descriptor_hints && descriptor_hints[0] == '1';
    if (!S.interlock && !(decode && decode[0] == '0')) {
        const VkDescriptorSetLayoutBinding bindings[4] = {
            { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, NULL }
        };
        S.dsl_decode = nvk_make_layout(bindings, S.hardware_aniso ? 4u : 2u);
        VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        const VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0u, 7u * sizeof(int32_t) };
        layout.setLayoutCount = 1u; layout.pSetLayouts = &S.dsl_decode;
        layout.pushConstantRangeCount = 1u; layout.pPushConstantRanges = &range;
        VkShaderModule module = S.decode_arrays && S.hardware_aniso
            ? vk_core_shader_module(spv_native_decode_comp_FILTERED_ARRAY, spv_native_decode_comp_FILTERED_ARRAY_size, "native filtered layer decode")
            : S.decode_arrays ? vk_core_shader_module(spv_native_decode_comp_ARRAY, spv_native_decode_comp_ARRAY_size, "native layer decode")
            : S.hardware_aniso ? vk_core_shader_module(spv_native_decode_comp_FILTERED, spv_native_decode_comp_FILTERED_size, "native filterable palette decode")
            : vk_core_shader_module(spv_native_decode_comp, spv_native_decode_comp_size, "native palette decode");
        if (!S.dsl_decode || !module || !vk_core_check(vkCreatePipelineLayout(dev, &layout, NULL, &S.pl_decode), "vkCreatePipelineLayout")) {
            if (module) vkDestroyShaderModule(dev, module, NULL);
            return 0;
        }
        VkComputePipelineCreateInfo pipeline = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = module; pipeline.stage.pName = "main";
        pipeline.layout = S.pl_decode;
        const int ok = vk_core_check(vkCreateComputePipelines(dev, vk_core_pipeline_cache(), 1u, &pipeline, NULL, &S.pipe_decode), "vkCreateComputePipelines");
        vkDestroyShaderModule(dev, module, NULL);
        if (!ok) return 0;
    }
    if (!S.pipe_draw[0] || !S.pipe_draw[1] || !S.pipe_scan) { nvk_note("pipeline creation failed"); return 0; }
    if (!nvk_make_sampler(&S.sampler_clamp, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) ||
        !nvk_make_sampler(&S.sampler_repeat, VK_SAMPLER_ADDRESS_MODE_REPEAT) ||
        !nvk_make_hd_sampler(&S.sampler_hd)) return 0;
    if (S.hardware_aniso) for (uint32_t i = 0u; i < 5u; ++i) {
        VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.anisotropyEnable = i != 0u;
        sampler.maxAnisotropy = (float)(1u << i);
        sampler.maxLod = 0.f;
        if (!vk_core_check(vkCreateSampler(dev, &sampler, NULL, &S.sampler_aniso[i]), "native anisotropic sampler")) return 0;
    }
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vk_core_queue_family();
    if (!vk_core_check(vkCreateCommandPool(dev, &cpi, NULL, &S.pool), "vkCreateCommandPool")) return 0;
    if (!vk_core_image_create(&S.dummy_color, VK_FORMAT_R8G8B8A8_UNORM, 1u, 1u, 1u,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native dummy color") ||
        !vk_core_image_create(&S.dummy_uint, VK_FORMAT_R32_UINT, 1u, 1u, 1u,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native dummy uint") ||
        !vk_core_image_create(&S.words, VK_FORMAT_R16_UINT, VRAM_W, VRAM_H, 1u,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native words")) return 0;
    /* Layers retain the original 256x256 sampling coordinates. A 2D atlas
     * would change anisotropic filter rounding when adding a tile origin. */
    const uint32_t decode_size = S.decode_arrays ? 256u : 1u, decode_layers = S.decode_arrays ? 64u : 2u;
    if (!S.interlock &&
        (!vk_core_image_create_array(&S.decoded_array, VK_FORMAT_R32_UINT, decode_size, decode_size, 1u, decode_layers,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "native decoded layers") ||
         !vk_core_image_create_array(&S.filtered_array, VK_FORMAT_R8G8B8A8_UNORM, decode_size, decode_size, 1u, decode_layers,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "native filtered layers"))) return 0;
    if (!vk_core_buffer_create(&S.dummy_texels, 16u, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_CORE_MEMORY_UPLOAD, "native dummy texels"))
        return 0;
    memset(S.dummy_texels.mapped, 0, 16u);
    vk_core_buffer_flush(&S.dummy_texels, 0, 16u);
    VkBufferViewCreateInfo bvi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
    bvi.buffer = S.dummy_texels.buffer; bvi.format = VK_FORMAT_R16_UINT; bvi.range = VK_WHOLE_SIZE;
    if (!vk_core_check(vkCreateBufferView(dev, &bvi, NULL, &S.dummy_view), "vkCreateBufferView")) return 0;
    const char *gpu_timing = getenv("PSX_NATIVE_VK_GPU_TIMING");
    const char *submit_timing = getenv("PSX_NATIVE_VK_SUBMIT_TIMING");
    if (submit_timing && submit_timing[0] == '1' && caps->timestamps && caps->timestamp_period_ns > 0.f) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NVK_SUBMIT_TS;
        if (!vk_core_check(vkCreateQueryPool(dev, &qi, NULL, &P.submit_pool), "submit timing pool"))
            P.submit_pool = VK_NULL_HANDLE;
    }
    if (gpu_timing && gpu_timing[0] == '1' && caps->timestamps && caps->timestamp_period_ns > 0.f) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        const char *profile = getenv("PSX_NATIVE_VK_PROFILE_CAPTURE");
        P.capture_profile = profile && profile[0] == '1';
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NVK_TS + (P.capture_profile ? NVK_CAPTURE_TS + NVK_DETAIL_TS : 0u);
        if (!vk_core_check(vkCreateQueryPool(dev, &qi, NULL, &P.pool), "vkCreateQueryPool")) P.pool = VK_NULL_HANDLE;
    }
    S.set0 = malloc(NVK_SET0_CACHE * sizeof(*S.set0));
    if (!S.set0) return 0;
    VkCommandBuffer cmd = vk_core_oneshot_begin();
    if (!cmd) return 0;
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
    VkClearColorValue zero;
    memset(&zero, 0, sizeof(zero));
    VkImage images[4] = { S.dummy_color.image, S.dummy_uint.image, S.words.image, VK_NULL_HANDLE };
    for (unsigned i = 0u; i < 3u; ++i) {
        vk_core_image_barrier(cmd, images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1u, &range);
    }
    if (S.decoded_array.image) {
        vk_core_image_barrier(cmd, S.decoded_array.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vk_core_image_barrier(cmd, S.filtered_array.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (!vk_core_oneshot_end_wait(cmd)) return 0;
    const char *submit_thread = getenv("PSX_NATIVE_VK_SUBMIT_THREAD");
    if (S.defer_submits && !S.pace_margin_ns && submit_thread && submit_thread[0] == '1') {
        SQ.mutex = SDL_CreateMutex();
        SQ.condition = SDL_CreateCond();
        if (!SQ.mutex || !SQ.condition) return 0;
        SQ.run = 1;
        SQ.thread = SDL_CreateThread(nvk_submit_thread, "native-vk-submit", NULL);
        if (!SQ.thread) return 0;
    }
    return 1;
}

/* ---- one execution ---------------------------------------------------------------------------------------------------------------------- */
typedef struct NvkResult {
    int ok;
    uint64_t digests[NVK_IMAGES];
    const uint8_t *pixels[NVK_IMAGES];
} NvkResult;

static void nvk_planes_to_spare(void)
{
    for (unsigned i = 0u; i < GL_NATIVE_GPU_PLANES; ++i) {
        if (!S.work_planes[i].color.image) continue;
        nvk_plane_free(&S.spare[i]);
        S.spare[i] = S.work_planes[i];
        memset(&S.work_planes[i], 0, sizeof(S.work_planes[i]));
    }
}

/* The CPU needs the pixels of image 0 (digest), of the other images only for an audit or probe. */
static int s_backend_mode;
static int nvk_wants_readback(unsigned image)
{
    return !s_backend_mode || (image == 0u && (!R.hash_groups || gl_renderer_native_guest_reference_enabled())) ||
           s_native_recipe_audit_enabled || s_native_phase_hashes_enabled || s_native_motion_probe.target_vblank;
}

static int nvk_capture_work_eligible(const GlNativeGpuWork *work)
{
    if (!S.capture_enabled || s_retry_phases || !s_backend_mode || work->canonical_gpu || work->wireframe ||
        work->depth_view || S.depth_bounds || (S.phase_damage && !S.linked_capture) || !work->phase_textures ||
        s_native_motion_probe.target_vblank || s_native_recipe_audit_enabled ||
        S.capture_failed_scene == work->identity.scene_generation || !S.capture_meta.buffer) return 0;
    for (uint32_t i = 0u; i < work->published_chunk_count; ++i) {
        const GlNativeGpuChunk *chunk = &work->phase_chunks[i];
        if (!chunk->count || !chunk->geometry) return 0;
        const GlNativeGpuCommand *seed = &chunk->commands[0];
        if (seed->kind != NATIVE_GPU_SPAN || (seed->mask & 2u) || seed->x != work->phase_crop_x ||
            seed->y != work->phase_crop_y || seed->w != work->scanout_width || seed->h != work->scanout_height ||
            (seed->source != UINT32_MAX && seed->source < GL_NATIVE_GPU_BASE_SOURCE)) return 0;
        for (uint32_t j = 1u; j < chunk->count; ++j) {
            const GlNativeGpuCommand *command = &chunk->commands[j];
            const uint8_t *data = chunk->data;
            if (command->kind == NATIVE_GPU_TEMPLATE) {
                if (!work->phase_templates || command->data >= work->phase_templates->count) return 0;
                data = work->phase_templates->data;
                command = &work->phase_templates->commands[command->data];
            }
            if (command->kind != NATIVE_GPU_DRAW || command->source) return 0;
            if (!command->texture_snapshot) {
                const uint32_t count = command->draw.topology == GPU_RENDER_SEMANTIC_LINES ? 0u : native_gpu_command_triangles(command);
                for (uint32_t t = 0u; t < count; ++t)
                    if (native_gpu_command_flags(command, data, t).textured) return 0;
            }
        }
    }
    return 1;
}

/* A phase is pixel-local: immutable texture reads and destination reads at
 * gl_FragCoord only. Reuse the previous phase outside conservative coverage
 * of changed draws, and replay the ENTIRE ordered journal inside that damage.
 * Unsupported/nonlocal transfers and structural changes retain full replay. */
static int nvk_phase_command_equal(const GlNativeGpuCommand *a, const GlNativeGpuCommand *b, int draw)
{
    return a->kind == b->kind && a->source == b->source && a->color == b->color && a->mask == b->mask &&
        a->mesh_count == b->mesh_count && a->texture_snapshot == b->texture_snapshot &&
        !memcmp(&a->x, &b->x, offsetof(GlNativeGpuCommand, draw) - offsetof(GlNativeGpuCommand, x)) &&
        (!draw || !memcmp(&a->draw, &b->draw, sizeof(a->draw)));
}

static void nvk_damage_mark(uint8_t *tiles, uint32_t columns, const int32_t clip[4],
                            int left, int top, int right, int bottom)
{
    const int tile = 1 << S.damage_tile_shift;
    if (left < clip[0]) left = clip[0]; if (top < clip[1]) top = clip[1];
    if (right > clip[2]) right = clip[2]; if (bottom > clip[3]) bottom = clip[3];
    if (left >= right || top >= bottom) return;
    for (int y = top / tile; y <= (bottom - 1) / tile; ++y)
        for (int x = left / tile; x <= (right - 1) / tile; ++x) tiles[(uint32_t)y * columns + (uint32_t)x] = 1u;
}

static void nvk_damage_primitive(uint8_t *tiles, uint32_t columns, const int32_t clip[4],
                                 uint32_t scale, const GlNativeGpuDrawGeometry *geometry)
{
    if (geometry->visible) nvk_damage_mark(tiles, columns, clip, geometry->left * (int)scale,
        geometry->top * (int)scale, (geometry->right + 1) * (int)scale, (geometry->bottom + 1) * (int)scale);
}

static int nvk_phase_damage(const GlNativeGpuChunk *previous, const GlNativeGpuChunk *current)
{
    if (R.work->render_width) return 0;
    const uint32_t tile = 1u << S.damage_tile_shift;
    R.damage_count = 0u;
    R.damage_shader_active = 0;
    if (!S.phase_damage || S.interlock || !s_backend_mode || R.work->wireframe || R.work->depth_view ||
        !previous->geometry || !current->geometry || !previous->geometry->primitives || !current->geometry->primitives ||
        previous->geometry->gpu_setup || current->geometry->gpu_setup || !current->count ||
        previous->count != current->count) return 0;
    const GlNativeGpuCommand *seed = &current->commands[0], *old_seed = &previous->commands[0];
    if (seed->kind != NATIVE_GPU_SPAN || !nvk_phase_command_equal(seed, old_seed, 0) || (seed->mask & 2u) ||
        seed->x != R.work->phase_crop_x || seed->y != R.work->phase_crop_y ||
        seed->w != R.work->scanout_width || seed->h != R.work->scanout_height ||
        seed->plane < GL_NATIVE_GPU_PHASE_BASE || old_seed->plane < GL_NATIVE_GPU_PHASE_BASE ||
        seed->plane >= GL_NATIVE_GPU_PLANES || old_seed->plane >= GL_NATIVE_GPU_PLANES) return 0;
    const NvkPlane *plane = &R.planes[seed->plane], *old = &R.planes[old_seed->plane];
    if (!plane->color.image || !old->color.image || plane->width != old->width || plane->height != old->height ||
        plane->has_key != old->has_key) return 0;
    const uint32_t scale = native_gpu_plane_scale(R.work, seed->plane);
    const int32_t clip[4] = { seed->x * (int)scale, seed->y * (int)scale,
        (seed->x + seed->w) * (int)scale, (seed->y + seed->h) * (int)scale };
    if (clip[0] < 0 || clip[1] < 0 || clip[2] > (int)plane->width || clip[3] > (int)plane->height) return 0;
    const uint32_t columns = (plane->width + tile - 1u) / tile, rows = (plane->height + tile - 1u) / tile;
    if (!columns || !rows || (uint64_t)columns * rows > 65536u) return 0;
    uint8_t *tiles = calloc((size_t)columns * rows, 1u);
    if (!tiles) return 0;
    int valid = 1;
    for (uint32_t c = 0u; c < current->count && valid; ++c) {
        const GlNativeGpuCommand *a = &previous->commands[c], *b = &current->commands[c];
        if (a->kind != b->kind || a->plane != old_seed->plane || b->plane != seed->plane) { valid = 0; break; }
        if (b->kind == NATIVE_GPU_TEMPLATE) {
            if (a->data != b->data || !nvk_phase_command_equal(a, b, 0)) valid = 0;
        } else if (b->kind == NATIVE_GPU_DRAW) {
            if (a->source || b->source || a->mesh_count != b->mesh_count ||
                a->draw.topology != b->draw.topology) { valid = 0; break; }
            const int lines = b->draw.topology == GPU_RENDER_SEMANTIC_LINES;
            const uint32_t count = lines ? b->draw.line_count : native_gpu_command_triangles(b);
            const uint32_t old_count = lines ? a->draw.line_count : native_gpu_command_triangles(a);
            if (count != old_count || previous->geometry->offsets[c].primitive + count > previous->geometry->offsets[c+1u].primitive ||
                current->geometry->offsets[c].primitive + count > current->geometry->offsets[c+1u].primitive) { valid = 0; break; }
            const int shared = nvk_phase_command_equal(a, b, 1);
            if (b->mesh_count && ((uint64_t)a->data + (uint64_t)count * sizeof(GlNativeGpuMeshTriangle) > previous->bytes ||
                (uint64_t)b->data + (uint64_t)count * sizeof(GlNativeGpuMeshTriangle) > current->bytes)) { valid = 0; break; }
            for (uint32_t t = 0u; t < count; ++t) {
                if (shared && (!b->mesh_count || !memcmp(previous->data + a->data + (size_t)t * sizeof(GlNativeGpuMeshTriangle),
                    current->data + b->data + (size_t)t * sizeof(GlNativeGpuMeshTriangle), sizeof(GlNativeGpuMeshTriangle)))) continue;
                nvk_damage_primitive(tiles, columns, clip, scale, &previous->geometry->primitives[previous->geometry->offsets[c].primitive + t]);
                nvk_damage_primitive(tiles, columns, clip, scale, &current->geometry->primitives[current->geometry->offsets[c].primitive + t]);
            }
        } else if (b->kind == NATIVE_GPU_SPAN || b->kind == NATIVE_GPU_SPANS) {
            if ((a->source != UINT32_MAX && a->source < GL_NATIVE_GPU_BASE_SOURCE) ||
                (b->source != UINT32_MAX && b->source < GL_NATIVE_GPU_BASE_SOURCE)) { valid = 0; break; }
            int same = nvk_phase_command_equal(a, b, 0);
            if (b->kind == NATIVE_GPU_SPANS) {
                const uint64_t bytes = (uint64_t)b->mesh_count * sizeof(GlNativeGpuSpanRect);
                if (a->mesh_count != b->mesh_count || (uint64_t)a->data + bytes > previous->bytes ||
                    (uint64_t)b->data + bytes > current->bytes) { valid = 0; break; }
                same = same && !memcmp(previous->data + a->data, current->data + b->data, (size_t)bytes);
            }
            if (!same) {
                nvk_damage_mark(tiles, columns, clip, a->x * (int)scale, a->y * (int)scale,
                    (a->x + a->w) * (int)scale, (a->y + a->h) * (int)scale);
                nvk_damage_mark(tiles, columns, clip, b->x * (int)scale, b->y * (int)scale,
                    (b->x + b->w) * (int)scale, (b->y + b->h) * (int)scale);
            }
        } else valid = 0;
    }
    uint64_t area = 0u;
    /* Bound replay fragmentation: a few non-overlapping horizontal bands
     * trade some conservative overdraw for many fewer scissor/draw changes.
     * The complete journal is replayed inside each expanded rectangle. */
    const char *bands_option = getenv("PSX_NATIVE_VK_DAMAGE_BANDS");
    unsigned bands = bands_option ? (unsigned)strtoul(bands_option, NULL, 10) : 0u;
    if (bands > 16u) bands = 4u;
    if (bands && valid) {
        const uint32_t first_row = (uint32_t)clip[1] / tile;
        const uint32_t last_row = ((uint32_t)clip[3] + tile - 1u) / tile;
        for (uint32_t band = 0u; band < bands; ++band) {
            const uint32_t begin = first_row + (last_row - first_row) * band / bands;
            const uint32_t end = first_row + (last_row - first_row) * (band + 1u) / bands;
            uint32_t left = columns, right = 0u, top = end, bottom = begin;
            for (uint32_t y = begin; y < end; ++y) for (uint32_t x = 0u; x < columns; ++x)
                if (tiles[y * columns + x]) {
                    if (x < left) left = x; if (x + 1u > right) right = x + 1u;
                    if (y < top) top = y; if (y + 1u > bottom) bottom = y + 1u;
                }
            if (left >= right || top >= bottom) continue;
            int x0 = (int)left * (int)tile, x1 = (int)right * (int)tile, y0 = (int)top * (int)tile, y1 = (int)bottom * (int)tile;
            if (x0 < clip[0]) x0 = clip[0]; if (y0 < clip[1]) y0 = clip[1];
            if (x1 > clip[2]) x1 = clip[2]; if (y1 > clip[3]) y1 = clip[3];
            int32_t *rect = R.damage_rects[R.damage_count++];
            rect[0] = x0; rect[1] = y0; rect[2] = x1 - x0; rect[3] = y1 - y0;
            area += (uint64_t)(x1 - x0) * (uint32_t)(y1 - y0);
        }
    }
    for (uint32_t y = 0u; !bands && y < rows && valid; ++y) for (uint32_t x = 0u; x < columns;) {
        if (!tiles[y * columns + x]) { ++x; continue; }
        const uint32_t begin = x++;
        while (x < columns && tiles[y * columns + x]) ++x;
        int left = (int)begin * (int)tile, right = (int)x * (int)tile, top = (int)y * (int)tile, bottom = top + (int)tile;
        if (left < clip[0]) left = clip[0]; if (right > clip[2]) right = clip[2];
        if (top < clip[1]) top = clip[1]; if (bottom > clip[3]) bottom = clip[3];
        if (left >= right || top >= bottom) continue;
        area += (uint64_t)(right - left) * (bottom - top);
        uint32_t r;
        for (r = 0u; r < R.damage_count; ++r) {
            int32_t *rect = R.damage_rects[r];
            if (rect[0] == left && rect[2] == right - left && rect[1] + rect[3] == top) {
                rect[3] += bottom - top; break;
            }
        }
        if (r == R.damage_count) {
            if (r == sizeof(R.damage_rects) / sizeof(R.damage_rects[0])) { valid = 0; break; }
            int32_t *rect = R.damage_rects[R.damage_count++];
            rect[0] = left; rect[1] = top; rect[2] = right - left; rect[3] = bottom - top;
        }
    }
    const uint64_t full = (uint64_t)seed->w * seed->h * scale * scale;
    if (!valid || area * 5u >= full * 4u) { free(tiles); R.damage_count = 0u; return 0; }
    if (S.shader_damage && S.core_batch && (!S.capture_enabled || S.hd_batch) && !bands) {
        const uint32_t words = 5u + (columns * rows + 31u) / 32u;
        uint32_t chunk; VkDeviceSize offset;
        uint32_t mask[5u + 2048u];
        void *upload = nvk_arena(words * sizeof(*mask), 16u, &chunk, &offset);
        if (upload) {
            memset(mask, 0, words * sizeof(*mask));
            mask[0] = columns | (S.damage_tile_shift << 16u); memcpy(mask + 1u, clip, sizeof(clip));
            for (uint32_t i = 0u; i < columns * rows; ++i)
                if (tiles[i]) mask[5u + i / 32u] |= 1u << (i % 32u);
            memcpy(upload, mask, words * sizeof(*mask));
            R.damage_buffer = S.chunks[chunk].buf.buffer;
            R.damage_word_offset = (uint32_t)(offset / sizeof(*mask));
            R.damage_shader_active = 1;
        }
    }
    free(tiles);
    const char *copy_elision = getenv("PSX_NATIVE_VK_PHASE_COPY_ELISION");
    const char *reuse_plane = getenv("PSX_NATIVE_VK_PHASE_REUSE");
    if (R.work->scanout && reuse_plane && reuse_plane[0] == '1') {
        /* Save the completed immutable image before recycling its working
         * surface. The validated recipe only reads its own destination and
         * immutable base textures. Later phases therefore need the previous
         * pixels, not a second allocation containing a copy of those pixels.
         * Swapping ownership preserves depth, hazard state and single-owner
         * cleanup; no two plane slots alias the same Vulkan objects. */
        const uint32_t image = old_seed->plane - GL_NATIVE_GPU_PHASE_BASE + 1u;
        if (!R.scanned_images[image] && !nvk_scanout_image(image)) {
            R.damage_count = 0u;
            R.damage_shader_active = 0;
            return -1;
        }
        NvkPlane spare = R.planes[seed->plane];
        R.planes[seed->plane] = R.planes[old_seed->plane];
        R.planes[old_seed->plane] = spare;
    } else if (copy_elision && copy_elision[0] == '1') {
        /* Only this crop is scanned out. Every damaged pixel is initialized
         * by the first unconditional SPAN before any local depth read.
         * Undamaged depth is neither sampled by scanout nor exported by this
         * pixel-local recipe, so carrying it from the previous phase is
         * unnecessary. A later full replay initializes the whole crop. */
        NvkPlane *target = &R.planes[seed->plane];
        nvk_pass_end();
        VkImageCopy region = {0};
        region.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        region.dstSubresource = region.srcSubresource;
        region.srcOffset = region.dstOffset = (VkOffset3D){ clip[0], clip[1], 0 };
        region.extent = (VkExtent3D){ (uint32_t)(clip[2] - clip[0]), (uint32_t)(clip[3] - clip[1]), 1u };
        vkCmdCopyImage(R.cmd, old->color.image, VK_IMAGE_LAYOUT_GENERAL,
            target->color.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
        nvk_barrier_all();
        target->dirty_all = 1;
        /* Retain conservative CPU bounds until the seed initializes damage. */
        free(target->depth_max);
        target->depth_max = NULL;
        target->depth_zero = 0;
    } else {
        nvk_plane_copy(old, &R.planes[seed->plane], 0);
        if (old->has_key) nvk_plane_copy(old, &R.planes[seed->plane], 1);
    }
    R.damage_plane = seed->plane;
    const unsigned image = seed->plane - GL_NATIVE_GPU_PHASE_BASE + 1u;
    if (R.work->scanout && image < NVK_IMAGES && scale == R.work->scale) {
        NvkPlane *output = &S.images[image];
        int32_t bounds[4] = { clip[2], clip[3], clip[0], clip[1] };
        for (uint32_t i = 0u; i < R.damage_count; ++i) {
            const int32_t *r = R.damage_rects[i];
            if (bounds[0] > r[0]) bounds[0] = r[0];
            if (bounds[1] > r[1]) bounds[1] = r[1];
            if (bounds[2] < r[0] + r[2]) bounds[2] = r[0] + r[2];
            if (bounds[3] < r[1] + r[3]) bounds[3] = r[1] + r[3];
        }
        if (R.damage_count) {
            P.damage_bounds_pixels += (uint64_t)(bounds[2] - bounds[0]) * (bounds[3] - bounds[1]);
            for (unsigned i = 0u; i < 4u; ++i) bounds[i] -= clip[i & 1u];
        } else memset(bounds, 0, sizeof(bounds));
        memcpy(output->damage_bounds, bounds, sizeof(bounds));
        output->damage_valid = 1;
        output->damage_regions_side = S.damage_regions;
        if (S.damage_regions) {
            memset(output->damage_regions, 0, sizeof(output->damage_regions));
            const unsigned side = (unsigned)S.damage_regions;
            for (unsigned q = 0u; q < side * side; ++q) {
                const int qx0 = clip[0] + (clip[2] - clip[0]) * (int)(q % side) / (int)side;
                const int qx1 = clip[0] + (clip[2] - clip[0]) * (int)(q % side + 1u) / (int)side;
                const int qy0 = clip[1] + (clip[3] - clip[1]) * (int)(q / side) / (int)side;
                const int qy1 = clip[1] + (clip[3] - clip[1]) * (int)(q / side + 1u) / (int)side;
                int32_t b[4] = { qx1, qy1, qx0, qy0 };
                for (uint32_t i = 0u; i < R.damage_count; ++i) {
                    const int32_t *r = R.damage_rects[i];
                    const int x0 = r[0] > qx0 ? r[0] : qx0, y0 = r[1] > qy0 ? r[1] : qy0;
                    const int x1 = r[0] + r[2] < qx1 ? r[0] + r[2] : qx1;
                    const int y1 = r[1] + r[3] < qy1 ? r[1] + r[3] : qy1;
                    if (x0 >= x1 || y0 >= y1) continue;
                    if (b[0] > x0) b[0] = x0; if (b[1] > y0) b[1] = y0;
                    if (b[2] < x1) b[2] = x1; if (b[3] < y1) b[3] = y1;
                }
                if (b[0] < b[2] && b[1] < b[3]) {
                    for (unsigned j = 0u; j < 4u; ++j) b[j] -= clip[j & 1u];
                    memcpy(output->damage_regions[q], b, sizeof(b));
                }
            }
        }
    }
    ++P.damage_phases; P.damage_pixels += area; P.damage_full_pixels += full;
    return 1;
}

/* Executes the work. On success the images' readback is mapped in R.readback; with out_fence the
 * submission is left running and its fence returned, else it is waited for. */
static int nvk_execute(const GlNativeGpuWork *work, VkFence *out_fence)
{
    const int phase_retry = out_fence && s_retry_phases;
    if (!phase_retry) ++S.work_sequence;
    if (s_backend_mode && work->fresh && !phase_retry) nvk_begin_layout();
    /* The base journal stops changing at phase_stream_ready.
     * Only published chunk descriptors/dimensions and the final seal are read
     * concurrently; take those under the same mutex as publication. */
    uint32_t base_count;
    const GlNativeGpuCommand *base_commands;
    const uint8_t *base_data;
    uint16_t widths[GL_NATIVE_GPU_PLANES], heights[GL_NATIVE_GPU_PLANES];
    int streaming = 0;
    if (out_fence) SDL_LockMutex(s_native_state_mutex);
    base_count = work->published_count;
    base_commands = work->published_commands; base_data = work->published_data;
    memcpy(widths, work->published_widths, sizeof(widths));
    memcpy(heights, work->published_heights, sizeof(heights));
    streaming = out_fence && !work->sealed;
    if (out_fence) SDL_UnlockMutex(s_native_state_mutex);
    P.streamed_works += streaming;
    memset(&R.c, 0, sizeof(R.c));
    R.work = work;
    R.capture_active = R.capture_used = 0;
    R.capture_eligible = -1;
    if (work->render_width || !out_fence || phase_retry || !nvk_capture_reserve(work)) R.capture_eligible = 0;
    R.damage_active = 0;
    R.damage_region = -1;
    R.damage_shader_active = 0;
    R.texture_filter = gr_texture_filter(); R.sprite_filter = s_sprite_filter;
    R.anisotropy = gl_renderer_anisotropy(); R.filter_strength = gl_renderer_scene_filter_strength();
    R.fail_stage = 0u; R.fail_command = 0u; R.fail_kind = 0u; R.fail_code = 0;
    R.pass_plane = NULL;
    memset(&R.b, 0, sizeof(R.b));
    memset(R.reset_word_rows, 0, sizeof(R.reset_word_rows));
    R.planes = S.work_planes;
    R.snapshots = S.snapshots;
    R.recipe = S.dummy_view;
    memset(R.readback_used, 0, sizeof(R.readback_used));
    R.hash_groups = 0u;
    memset(R.scanned_images, 0, sizeof(R.scanned_images));
    for (unsigned image = 0u; image < NVK_IMAGES; ++image) S.images[image].damage_valid = 0;
    R.scheduled_run = 0;
    R.batchable = 0;
    nvk_template_plan_free();
    R.plan_attempted = 0;
    TC.active = 0;
    IB.draw_count = 0u;
    CB.triangles = 0u;
    memset(&R.key, 0, sizeof(R.key));
    s_capture_batch_count = 0u;
    R.key.src_sampler = S.sampler_clamp;
    R.key.hd_sampler = S.sampler_clamp;
    S.chunk_cur = 0u;
    for (uint32_t i = 0u; i < S.chunk_count; ++i) S.chunks[i].cursor = 0u;
    S.decoded_work_count = 0u; ++S.decoded_generation;
    memset(S.decoded_work_hints, 0, sizeof(S.decoded_work_hints));
    if (!S.persistent_decode) for (uint32_t i = 0u; i < S.decoded_count; ++i) S.decoded[i].valid = 0;
    int ok = 1;
    R.cmd = VK_NULL_HANDLE;
    R.cmd_used = 0u; R.since_submit = 0u;
    if (!nvk_cmd_begin()) {
        nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PROGRAM, 0u, 0u);
        return 0;
    }
    R.recording = 1;
    uint64_t t_mark = SDL_GetTicksNS();
    uint64_t t_next;
#define NVK_LAP(phase) (t_next = SDL_GetTicksNS(), nvk_prof_cpu((phase), t_next - t_mark), t_mark = t_next)
    P.capture_queries = P.detail_queries = 0u;
    if (P.pool) { vkCmdResetQueryPool(R.cmd, P.pool, 0u, NVK_TS + (P.capture_profile ? NVK_CAPTURE_TS + NVK_DETAIL_TS : 0u)); P.stamped = 1; }
    nvk_prof_stamp(0u);
    nvk_barrier_all();   /* orders this work after every earlier submission (presenter reads, earlier works) */
    /* A previous execution that was never committed is dropped. */
    S.pending_work = NULL;
    NVK_LAP(0u);
    /* Planes (GL: the first slice of the work). */
    for (unsigned i = 0; i < GL_NATIVE_GPU_PLANES && ok; ++i) if (widths[i] && !R.planes[i].color.image) {
        R.planes[i] = S.spare[i];
        memset(&S.spare[i], 0, sizeof(S.spare[i]));
        const uint32_t plane_scale = native_gpu_plane_scale(work, i);
        ok = nvk_work_plane_size(&R.planes[i], i, widths[i] * plane_scale, heights[i] * plane_scale);
        if (ok) R.planes[i].dirty_all = 1;
        if (ok && i && work->depth_test) ok = nvk_plane_depth(&R.planes[i]);
        if (!ok) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PLANE, 0u, i);
        if (ok && !work->fresh && i < GL_NATIVE_GPU_PHASE_BASE && S.committed[i].color.image) {
            if (S.committed[i].width != R.planes[i].width || S.committed[i].height != R.planes[i].height) {
                nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PLANE, 0u, 0x100u + i);
                ok = 0;
                break;
            }
            nvk_plane_copy(&S.committed[i], &R.planes[i], 0);
            if (S.committed[i].has_key) {
                ok = nvk_plane_depth(&R.planes[i]);
                if (ok) nvk_plane_copy(&S.committed[i], &R.planes[i], 1);
            }
        }
    }
    /* The words image starts as the work's initial words. */
    if (ok && !phase_retry) {
        ok = nvk_sync_initial_words(base_data + work->initial_words_offset);
        if (!ok) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PHASE_UPLOAD, 0u, 0u);
    }
    if (streaming && ok) {
        nvk_pass_end();
        nvk_prof_stamp(1u);
        ok = nvk_submit_partial(VK_NULL_HANDLE);
        NVK_LAP(1u);
        const GlNativeGpuGeometry *geometry = work->geometry && base_count <= work->geometry->command_count
            ? work->geometry : NULL;
        nvk_prof_stamp(4u);
        if (ok && base_count) ok = nvk_slice(base_commands, base_data, 0u, base_count, geometry);
        nvk_pass_end();
        nvk_prof_stamp(5u);
        if (ok) ok = nvk_submit_partial(VK_NULL_HANDLE);
        SDL_LockMutex(s_native_state_mutex);
        /* Keep base execution overlapped with phase preparation, but optionally
         * wait for the complete immutable recipe before recording any phases.
         * Otherwise early chunks cannot use capture/damage proofs and may cost
         * more GPU time than the preparation overlap saves. */
        while (ok && (S.phase_seal || !work->published_chunk_count) && !work->sealed && !work->cancel_requested &&
               SDL_AtomicGet(&s_native_gpu_run))
            SDL_CondWaitTimeout(s_native_gpu_condition, s_native_state_mutex, 2);
        const int cancelled = work->cancel_requested || !SDL_AtomicGet(&s_native_gpu_run);
        SDL_UnlockMutex(s_native_state_mutex);
        if (cancelled) return 2;
    }
    /* Phase texture snapshots and template vertices serve the phase slices. */
    const GlNativePhaseTextures *textures = ok ? work->phase_textures : NULL;
    if (textures && (uint64_t)textures->count * VRAM_W * VRAM_H > vk_core_caps()->max_texel_buffer_elements) {
        nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PHASE_UPLOAD, 0u, NATIVE_VK_REFUSE_TEXEL_LIMIT);
        ok = 0;
    }
    if (ok && textures && textures->count) {
        const VkDeviceSize bytes = (VkDeviceSize)NVK_WORDS_BYTES * textures->count;
        /* The buffer and its view are kept between works while the snapshot count stays within a factor of four. */
        if (!R.recipe_buffer.buffer || R.recipe_buffer.size < bytes || R.recipe_buffer.size > 4u * bytes) {
            if (S.recipe_view) vkDestroyBufferView(vk_core_device(), S.recipe_view, NULL);
            S.recipe_view = VK_NULL_HANDLE;
            vk_core_buffer_destroy(&R.recipe_buffer);
            memset(&R.recipe_buffer, 0, sizeof(R.recipe_buffer));
        }
        if (!R.recipe_buffer.buffer)
            ok = vk_core_buffer_create(&R.recipe_buffer, bytes,
                                       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_CORE_MEMORY_DEVICE,
                                        "native phase textures");
        if (ok && !S.recipe_view) {
            VkBufferViewCreateInfo bvi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
            bvi.buffer = R.recipe_buffer.buffer; bvi.format = VK_FORMAT_R16_UINT; bvi.range = VK_WHOLE_SIZE;
            ok = nvk_ok(vkCreateBufferView(vk_core_device(), &bvi, NULL, &S.recipe_view), "vkCreateBufferView");
        }
        if (ok) {
            /* Each interpolated phase samples these snapshots per fragment.
             * Upload once to device-local storage instead of repeatedly
             * fetching a host-visible allocation over the PCIe link. */
            uint32_t chunk;
            VkDeviceSize offset;
            uint8_t *stage = nvk_arena(bytes, 16u, &chunk, &offset);
            if (!stage) ok = 0;
            else {
                memcpy(stage, textures->words, (size_t)bytes);
                const VkBufferCopy copy = { offset, 0u, bytes };
                nvk_pass_end();
                vkCmdCopyBuffer(R.cmd, S.chunks[chunk].buf.buffer, R.recipe_buffer.buffer, 1u, &copy);
                nvk_barrier_all();
                R.recipe = S.recipe_view;
            }
        }
        if (!ok) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PHASE_UPLOAD, 0u, 1u);
    }
    const GlNativePhaseTemplates *cache = ok ? work->phase_templates : NULL;
    R.template_vsrc = (NvkVertexSource){0};
    if (ok && cache && cache->geometry) {
        const uint32_t count = cache->geometry->offsets[cache->count].vertex;
        R.template_vsrc.vertices = cache->geometry->vertices;
        R.template_vsrc.count = count;
        if (count && !S.lazy_vertices && !nvk_upload_vertices(cache->geometry->vertices, count, &R.template_vsrc.buf, &R.template_vsrc.off)) {
            nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_PHASE_UPLOAD, 0u, 2u);
            ok = 0;
        }
    }
    nvk_pass_end();
    if (!streaming) nvk_prof_stamp(1u);
    if (ok && !nvk_submit_partial(VK_NULL_HANDLE)) ok = 0;
    if (!streaming) NVK_LAP(1u);
    /* The main journal, then each phase chunk, in the order the GL executor services them. */
    if (!streaming) nvk_prof_stamp(4u);
    if (ok && !streaming && !phase_retry) {
        const GlNativeGpuGeometry *geometry = work->geometry && base_count <= work->geometry->command_count
            ? work->geometry : NULL;
        if (base_count)
            ok = nvk_slice(base_commands, base_data, 0u, base_count, geometry);
    }
    if (!streaming) { nvk_pass_end(); nvk_prof_stamp(5u); }
    if ((streaming || S.pace_limit) && ok) ok = nvk_submit_partial(VK_NULL_HANDLE);
    nvk_prof_stamp(6u);
    for (uint32_t c = 0u; ok; ++c) {
        GlNativeGpuChunk chunk;
        int sealed = 1;
        uint32_t available;
        if (out_fence) SDL_LockMutex(s_native_state_mutex);
        if (streaming) {
            while (c >= work->published_chunk_count && !work->sealed && !work->cancel_requested &&
                   SDL_AtomicGet(&s_native_gpu_run))
                SDL_CondWaitTimeout(s_native_gpu_condition, s_native_state_mutex, 2);
            if (work->cancel_requested || !SDL_AtomicGet(&s_native_gpu_run)) {
                SDL_UnlockMutex(s_native_state_mutex);
                return 2; /* caller drains submissions before releasing storage */
            }
        }
        available = work->published_chunk_count;
        sealed = work->sealed;
        if (c < available) {
            chunk = work->phase_chunks[c];
            memcpy(widths, work->published_widths, sizeof(widths));
            memcpy(heights, work->published_heights, sizeof(heights));
        }
        if (out_fence) SDL_UnlockMutex(s_native_state_mutex);
        if (c >= available) break;
        if (sealed && R.capture_eligible < 0) R.capture_eligible = nvk_capture_work_eligible(work);
        for (unsigned i = GL_NATIVE_GPU_PHASE_BASE; i < GL_NATIVE_GPU_PLANES && ok; ++i)
            if (widths[i] && !R.planes[i].color.image) {
                R.planes[i] = S.spare[i];
                memset(&S.spare[i], 0, sizeof(S.spare[i]));
                const uint32_t scale = native_gpu_plane_scale(work, i);
                ok = nvk_work_plane_size(&R.planes[i], i, widths[i] * scale, heights[i] * scale);
                if (ok) R.planes[i].dirty_all = 1;
                if (ok && work->depth_test) ok = nvk_plane_depth(&R.planes[i]);
            }
        int damaged = 0;
        if (ok && sealed && c && c < work->phase_count) {
            damaged = nvk_phase_damage(&work->phase_chunks[c - 1u], &chunk);
            if (damaged < 0) { ok = 0; break; }
        }
        R.damage_active = damaged;
        if (ok && chunk.count && (!damaged || R.damage_count))
            ok = nvk_slice(chunk.commands, chunk.data, 0u, chunk.count, chunk.geometry);
        nvk_pass_end();
        R.damage_active = 0;
        R.damage_shader_active = 0;
        if ((streaming || S.pace_limit) && ok) {
            P.streamed_chunks += !sealed;
            ok = nvk_submit_partial(VK_NULL_HANDLE);
        }
    }
    nvk_pass_end();
    nvk_prof_stamp(7u);
    nvk_prof_stamp(2u);
    NVK_LAP(2u);
    if (ok && work->scanout) {
        for (unsigned image = 0; image <= work->phase_count && ok; ++image)
            if (!R.scanned_images[image]) ok = nvk_scanout_image(image);
        if (ok) P.rendered_phases += work->phase_count;
        if (ok) ok = nvk_hash_record(&S.images[0]);
        for (unsigned image = 0; image <= work->phase_count && ok; ++image)
            if (nvk_wants_readback(image)) ok = nvk_readback_record(image);
        if (ok) {
            if (R.capture_used) {
                const VkBufferCopy overflow = { 0u, 0u, sizeof(uint32_t) };
                vkCmdCopyBuffer(R.cmd, S.capture_meta.buffer, S.capture_readback.buffer, 1u, &overflow);
            }
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        } else if (!R.fail_stage) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_SCANOUT, 0u, 2u);
    }
    if (ok && work->scanout && S.publish_barriers) {
        /* Publish endpoint writes before signalling their compile fence.
         * A later presenter can read them without stalling unrelated work
         * recorded since this endpoint became ready. The execution-start
         * barrier still protects recycled endpoints from prior readers. */
        VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0u, 1u, &barrier, 0u, NULL, 0u, NULL);
    }
    for (unsigned i = 0u; i < GL_NATIVE_GPU_PLANES; ++i) if (R.snapshots[i].color.image) {
        nvk_plane_free(&S.spare_snapshots[i]);
        S.spare_snapshots[i] = R.snapshots[i];
        memset(&R.snapshots[i], 0, sizeof(R.snapshots[i]));
    }
    nvk_prof_stamp(3u);
    NVK_LAP(3u);
    VkFence fence = VK_NULL_HANDLE;
    if (ok && !S.fence) {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!nvk_ok(vkCreateFence(vk_core_device(), &fi, NULL, &S.fence), "vkCreateFence")) S.fence = VK_NULL_HANDLE;
    }
    fence = S.fence;
    const int recorded = ok && fence && nvk_ok(vkResetFences(vk_core_device(), 1u, &fence), "vkResetFences");
    R.recording = 0;
    if (recorded) {
        ok = nvk_submit_partial(fence);
        if (!ok) nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_FENCE, 0u, 0u);
        if (ok && out_fence) {
            *out_fence = fence;
        } else if (ok) {
            const uint64_t started = SDL_GetTicksNS();
            XgRenderFenceStatus status;
            while ((status = native_vk_sync_poll(fence)) == XG_RENDER_FENCE_PENDING) {
                if (SDL_GetTicksNS() - started > 60000000000ull) break;
                SDL_Delay(1);
            }
            if (status != XG_RENDER_FENCE_READY) {
                nvk_fail(GL_RENDERER_NATIVE_GPU_FAILURE_FENCE_WAIT, 0u, (uint32_t)status);
                ok = 0;
                (void)vk_core_device_wait_idle();
            }
        }
    } else ok = 0;
    if (!ok) {
        (void)nvk_submit_thread_sync();
        S.pending_count = S.pending_head = 0u;
        S.upload_valid = 0;
        for (uint32_t i = 0u; i < S.decoded_count; ++i) S.decoded[i].valid = 0;
        (void)vk_core_device_wait_idle();
    }
    NVK_LAP(4u);
#undef NVK_LAP
    ++P.works;
    return ok;
}

/* Everything an execution held that must outlive its commands. */
static void nvk_release(void)
{
    nvk_schedule_cache_clear();
    nvk_template_plan_free();
    for (uint32_t i = 0u; i < S.garbage_count; ++i) nvk_plane_destroy(&S.garbage[i]);
    S.garbage_count = 0u;
    nvk_hd_free_garbage();
    S.seed_count = 0u;
    R.recipe = VK_NULL_HANDLE;
    for (uint32_t i = 0u; i < S.pool_count && i <= S.pool_cur; ++i) (void)vkResetDescriptorPool(vk_core_device(), S.pools[i], 0);
    S.pool_cur = 0u;
    S.set0_count = 0u;
    for (uint32_t i = 0u; i < S.chunk_count; ++i) {
        S.chunks[i].native_set = S.chunks[i].scan_set = VK_NULL_HANDLE;
        S.chunks[i].cursor = 0u;
    }
    S.chunk_cur = 0u;
}

/* ===== shadow mode ============================================================================================================================ */
typedef struct NvkMismatch {
    uint64_t guest_vblank, source_sequence, guest_cycle;
    uint64_t run;
    uint32_t image, width, height;
    uint64_t gl_digest, vk_digest, differing_pixels;
    uint32_t first_x, first_y, gl_pixel, vk_pixel;
} NvkMismatch;

static const char *const s_refusal_names[NATIVE_VK_REFUSE_COUNT] = {
    "gl_interlock_tier", "canonical_gpu", "depth_view", "wireframe", "debug_mipmaps",
    "gpu_setup_compute", "texel_buffer_limit", "unknown_command"
};

static struct {
    int enabled_checked, enabled;
    uint64_t works, executed, compared, identical, mismatching, refused[NATIVE_VK_REFUSE_COUNT];
    uint64_t skipped_stale, vk_failed, counter_mismatches, scanout_works;
    uint32_t last_fail_stage, last_fail_command, last_fail_kind;
    int32_t last_fail_code;
    NvkMismatch ring[NVK_MISMATCH_RING];
    uint32_t ring_count;
    struct { uint8_t *pixels; uint32_t width, height; int valid; size_t capacity; } gl[NVK_IMAGES];
    uint8_t *dump_gl, *dump_vk;
    uint32_t dump_width, dump_height, dump_image;
    uint64_t dump_run;
    char counters_text[256];
} H;

int native_vk_shadow_enabled(void)
{
    if (!H.enabled_checked) {
        const char *env = getenv("PSX_NATIVE_VK_SHADOW");
        H.enabled = env && env[0] == '1';
        H.enabled_checked = 1;
    }
    return H.enabled && !(S.tried && !S.ready);
}

int native_vk_shadow_init(void)
{
    if (!H.enabled_checked) (void)native_vk_shadow_enabled();
    if (!H.enabled || S.tried) return S.ready;
    S.tried = 1;
    S.mutex = SDL_CreateMutex();
    if (!S.mutex) { nvk_note("mutex creation failed"); return 0; }
    if (!vk_core_ready()) {
        VkCoreDeviceRequest request;
        memset(&request, 0, sizeof(request));
        if (!vk_core_init(&request)) { snprintf(S.reason, sizeof(S.reason), "vk_core: %s", vk_core_failure()); return 0; }
    }
    S.ready = nvk_init();
    if (!S.ready && !S.reason[0]) nvk_note("executor initialisation failed");
    return S.ready;
}

void native_vk_shadow_gl_image(unsigned index, const void *pixels, uint32_t width, uint32_t height)
{
    if (!S.ready || index >= NVK_IMAGES) return;
    const size_t bytes = (size_t)width * height * 4u;
    SDL_LockMutex(S.mutex);
    if (H.gl[index].capacity < bytes) {
        uint8_t *grown = realloc(H.gl[index].pixels, bytes);
        if (!grown) { H.gl[index].valid = 0; SDL_UnlockMutex(S.mutex); return; }
        H.gl[index].pixels = grown; H.gl[index].capacity = bytes;
    }
    memcpy(H.gl[index].pixels, pixels, bytes);
    H.gl[index].width = width; H.gl[index].height = height; H.gl[index].valid = 1;
    SDL_UnlockMutex(S.mutex);
}

/* The first refusal that applies to the work, or -1. Covers the whole journal,
 * including the template cache the phase chunks replay. */
static int nvk_refusal(const GlNativeGpuWork *work, int gl_interlock)
{
    if ((gl_interlock != 0) != (S.interlock != 0)) return NATIVE_VK_REFUSE_GL_TIER;   /* the tiers must agree */
    if (work->canonical_gpu) return NATIVE_VK_REFUSE_CANONICAL_GPU;
    if (gl_renderer_debug_mipmaps()) return NATIVE_VK_REFUSE_MIPMAPS;
    if (!S.pipe_setup && (work->gpu_setup || (work->geometry && work->geometry->gpu_setup) ||
        (work->phase_templates && work->phase_templates->geometry && work->phase_templates->geometry->gpu_setup)))
        return NATIVE_VK_REFUSE_GPU_SETUP;
    if (work->phase_textures && work->phase_textures->count &&
        (uint64_t)work->phase_textures->count * VRAM_W * VRAM_H > vk_core_caps()->max_texel_buffer_elements)
        return NATIVE_VK_REFUSE_TEXEL_LIMIT;
    for (unsigned pass = 0u; pass < 3u; ++pass) {
        const GlNativeGpuCommand *commands = work->published_commands;
        uint32_t count = work->published_count, chunks = 1u;
        if (pass == 1u) { commands = work->phase_templates ? work->phase_templates->commands : NULL;
                          count = work->phase_templates ? work->phase_templates->count : 0u; }
        if (pass == 2u) { commands = NULL; count = 0u; chunks = work->published_chunk_count; }
        for (uint32_t c = 0u; c < (pass == 2u ? chunks : 1u); ++c) {
            if (pass == 2u) { commands = work->phase_chunks[c].commands; count = work->phase_chunks[c].count; }
            for (uint32_t i = 0u; i < count; ++i) {
                const GlNativeGpuCommand *command = &commands[i];
                switch (command->kind) {
                case NATIVE_GPU_DRAW: case NATIVE_GPU_SEED: case NATIVE_GPU_WORDS: case NATIVE_GPU_SNAPSHOT: case NATIVE_GPU_SPAN:
                case NATIVE_GPU_WORDS_RESET: case NATIVE_GPU_BASE: case NATIVE_GPU_TEMPLATE: case NATIVE_GPU_SPANS:
                    break;
                case NATIVE_GPU_CANONICAL_READBACK: case NATIVE_GPU_PROVENANCE_CLEAR:
                    return NATIVE_VK_REFUSE_CANONICAL_GPU;
                default:
                    return NATIVE_VK_REFUSE_COMMAND;
                }
            }
        }
    }
    return -1;
}

static void nvk_compare_image(const GlNativeGpuWork *work, unsigned image, const uint8_t *vk_pixels)
{
    if (!H.gl[image].valid) return;
    const uint32_t w = S.images[image].width, h = S.images[image].height;
    const size_t bytes = (size_t)w * h * 4u;
    const uint64_t vk_digest = native_gpu_image_digest(vk_pixels, bytes, NULL);
    const uint64_t gl_digest = work->image_digests[image];
    ++H.compared;
    if (vk_digest == gl_digest) { ++H.identical; return; }
    ++H.mismatching;
    NvkMismatch *m = &H.ring[H.ring_count % NVK_MISMATCH_RING];
    ++H.ring_count;
    memset(m, 0, sizeof(*m));
    m->guest_vblank = work->identity.guest_vblank_sequence;
    m->source_sequence = work->identity.source_sequence;
    m->guest_cycle = work->identity.guest_cycle;
    m->run = H.executed;
    m->image = image; m->width = w; m->height = h;
    m->gl_digest = gl_digest; m->vk_digest = vk_digest;
    const uint8_t *gl = H.gl[image].pixels;
    if (H.gl[image].width == w && H.gl[image].height == h) {
        int first = 1;
        for (size_t p = 0u; p < (size_t)w * h; ++p) {
            uint32_t a, b;
            memcpy(&a, gl + p * 4u, 4u); memcpy(&b, vk_pixels + p * 4u, 4u);
            if (a == b) continue;
            ++m->differing_pixels;
            if (first) { m->first_x = (uint32_t)(p % w); m->first_y = (uint32_t)(p / w); m->gl_pixel = a; m->vk_pixel = b; first = 0; }
        }
        /* The dump keeps the FIRST mismatch: later ones inherit it through the committed planes. */
        if (!H.dump_gl) {
            H.dump_gl = malloc(bytes); H.dump_vk = malloc(bytes);
            if (H.dump_gl && H.dump_vk) {
                memcpy(H.dump_gl, gl, bytes); memcpy(H.dump_vk, vk_pixels, bytes);
                H.dump_width = w; H.dump_height = h; H.dump_image = image; H.dump_run = H.executed;
            } else { free(H.dump_gl); free(H.dump_vk); H.dump_gl = H.dump_vk = NULL; }
        }
    } else m->differing_pixels = UINT64_MAX; /* the GL image has another size */
}

void native_vk_shadow_run(const GlNativeGpuWork *work, int gl_interlock)
{
    if (!S.ready || !work) return;
    SDL_LockMutex(S.mutex);
    ++H.works;
    const int refusal = nvk_refusal(work, gl_interlock);
    if (refusal >= 0) { ++H.refused[refusal]; goto done; }
    if (!work->fresh && !S.committed_valid) { ++H.skipped_stale; goto done; }
    ++H.executed;
    H.scanout_works += work->scanout != 0;
    const int ok = nvk_execute(work, NULL);
    if (!ok) {
        ++H.vk_failed;
        H.last_fail_stage = R.fail_stage; H.last_fail_command = R.fail_command;
        H.last_fail_kind = R.fail_kind; H.last_fail_code = R.fail_code;
    } else {
        for (unsigned image = 0; work->scanout && image <= work->phase_count; ++image) {
            vk_core_buffer_invalidate(&R.readback[image], 0, VK_WHOLE_SIZE);
            nvk_compare_image(work, image, R.readback[image].mapped);
        }
        const NvkCounters *c = &R.c;
        if (c->destination_barriers != work->destination_barriers || c->skipped_barriers != work->skipped_barriers ||
            c->scheduled_layers != work->scheduled_layers || c->geometry_draws != work->geometry_draws ||
            c->transfer_draws != work->transfer_draws || c->word_uploads != work->word_uploads ||
            c->snapshot_commands != work->snapshot_commands || c->depth_tested_triangles != work->depth_tested_triangles) {
            ++H.counter_mismatches;
            snprintf(H.counters_text, sizeof(H.counters_text),
                "vblank %llu barriers %llu/%llu skipped %llu/%llu layers %llu/%llu draws %llu/%llu transfers %llu/%llu words %llu/%llu snapshots %llu/%llu depth %llu/%llu (vk/gl)",
                (unsigned long long)work->identity.guest_vblank_sequence,
                (unsigned long long)c->destination_barriers, (unsigned long long)work->destination_barriers,
                (unsigned long long)c->skipped_barriers, (unsigned long long)work->skipped_barriers,
                (unsigned long long)c->scheduled_layers, (unsigned long long)work->scheduled_layers,
                (unsigned long long)c->geometry_draws, (unsigned long long)work->geometry_draws,
                (unsigned long long)c->transfer_draws, (unsigned long long)work->transfer_draws,
                (unsigned long long)c->word_uploads, (unsigned long long)work->word_uploads,
                (unsigned long long)c->snapshot_commands, (unsigned long long)work->snapshot_commands,
                (unsigned long long)c->depth_tested_triangles, (unsigned long long)work->depth_tested_triangles);
        }
        S.pending_work = work;
        S.pending_identity = work->identity;
    }
    nvk_release();
done:
    for (unsigned i = 0u; i < NVK_IMAGES; ++i) H.gl[i].valid = 0;
    SDL_UnlockMutex(S.mutex);
}

void native_vk_shadow_apply(const GlNativeGpuWork *work)
{
    if (!S.ready) return;
    SDL_LockMutex(S.mutex);
    if (S.pending_work == work && !memcmp(&S.pending_identity, &work->identity, sizeof(work->identity))) {
        /* The work's planes become the committed ones; the retired ones are its spares. */
        for (unsigned i = 0u; i < GL_NATIVE_GPU_PHASE_BASE; ++i) {
            NvkPlane old = S.committed[i];
            S.committed[i] = S.work_planes[i];
            S.work_planes[i] = old;
        }
        nvk_planes_to_spare();
        S.committed_valid = 1;
    } else {
        /* A work the executor did not run was committed on GL: the shadow cannot follow. */
        S.committed_valid = 0;
    }
    S.pending_work = NULL;
    SDL_UnlockMutex(S.mutex);
}

void native_vk_shadow_shutdown(void)
{
    if (!S.tried) return;
    if (S.mutex) SDL_LockMutex(S.mutex);
    nvk_teardown();
    for (unsigned i = 0u; i < NVK_IMAGES; ++i) { free(H.gl[i].pixels); H.gl[i].pixels = NULL; H.gl[i].capacity = 0u; H.gl[i].valid = 0; }
    if (S.mutex) SDL_UnlockMutex(S.mutex);
}

/* ---- debug commands ---------------------------------------------------------------------------------------------------------------------- */
int native_vk_shadow_json(char *out, int capacity)
{
    if (!H.enabled_checked) (void)native_vk_shadow_enabled();
    if (!H.enabled) return snprintf(out, (size_t)capacity, "{\"enabled\":0,\"reason\":\"PSX_NATIVE_VK_SHADOW is not 1\"}");
    if (!S.tried) return snprintf(out, (size_t)capacity, "{\"enabled\":1,\"ready\":0,\"reason\":\"not initialised\"}");
    if (!S.ready) return snprintf(out, (size_t)capacity, "{\"enabled\":1,\"ready\":0,\"reason\":\"%s\"}", S.reason);
    SDL_LockMutex(S.mutex);
    int n = snprintf(out, (size_t)capacity,
        "{\"enabled\":1,\"ready\":1,\"works\":%llu,\"executed\":%llu,\"scanout_works\":%llu,\"compared_images\":%llu,"
        "\"identical\":%llu,\"mismatching\":%llu,\"skipped_stale_base\":%llu,\"vk_failed\":%llu,"
        "\"counter_mismatches\":%llu,\"last_counter_mismatch\":\"%s\","
        "\"last_failure\":{\"stage\":%u,\"command\":%u,\"kind\":%u,\"vk_result\":%d},\"refused\":{",
        (unsigned long long)H.works, (unsigned long long)H.executed, (unsigned long long)H.scanout_works,
        (unsigned long long)H.compared, (unsigned long long)H.identical, (unsigned long long)H.mismatching,
        (unsigned long long)H.skipped_stale, (unsigned long long)H.vk_failed, (unsigned long long)H.counter_mismatches,
        H.counters_text, H.last_fail_stage, H.last_fail_command, H.last_fail_kind, H.last_fail_code);
    for (unsigned i = 0u; i < NATIVE_VK_REFUSE_COUNT && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%llu", i ? "," : "", s_refusal_names[i],
                      (unsigned long long)H.refused[i]);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "},\"mismatches\":[");
    const uint32_t shown = H.ring_count < NVK_MISMATCH_RING ? H.ring_count : NVK_MISMATCH_RING;
    for (uint32_t k = 0u; k < shown && n < capacity; ++k) {
        const NvkMismatch *m = &H.ring[(H.ring_count - shown + k) % NVK_MISMATCH_RING];
        n += snprintf(out + n, (size_t)(capacity - n),
            "%s{\"guest_vblank\":%llu,\"source_sequence\":%llu,\"guest_cycle\":%llu,\"run\":%llu,\"image\":%u,"
            "\"width\":%u,\"height\":%u,\"gl_digest\":\"%016llx\",\"vk_digest\":\"%016llx\","
            "\"differing_pixels\":%lld,\"first_x\":%u,\"first_y\":%u,\"gl_pixel\":\"%08x\",\"vk_pixel\":\"%08x\"}",
            k ? "," : "", (unsigned long long)m->guest_vblank, (unsigned long long)m->source_sequence,
            (unsigned long long)m->guest_cycle, (unsigned long long)m->run, m->image, m->width, m->height,
            (unsigned long long)m->gl_digest, (unsigned long long)m->vk_digest,
            m->differing_pixels == UINT64_MAX ? -1ll : (long long)m->differing_pixels,
            m->first_x, m->first_y, m->gl_pixel, m->vk_pixel);
    }
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "]}");
    SDL_UnlockMutex(S.mutex);
    return n;
}

int native_vk_shadow_dump(const char *path, char *err, size_t err_size)
{
    if (!S.ready) { snprintf(err, err_size, "shadow is not running"); return 0; }
    int ok = 0;
    SDL_LockMutex(S.mutex);
    if (!H.dump_gl || !H.dump_vk) snprintf(err, err_size, "no mismatch recorded");
    else {
        char name[1024];
        const size_t bytes = (size_t)H.dump_width * H.dump_height * 4u;
        const char *suffix[3] = { ".gl.rgba", ".vk.rgba", ".json" };
        FILE *f[3] = {0};
        for (unsigned i = 0u; i < 3u; ++i) {
            snprintf(name, sizeof(name), "%s%s", path, suffix[i]);
            f[i] = fopen(name, "wb");
        }
        if (!f[0] || !f[1] || !f[2]) snprintf(err, err_size, "cannot open %s*", path);
        else {
            ok = fwrite(H.dump_gl, 1u, bytes, f[0]) == bytes && fwrite(H.dump_vk, 1u, bytes, f[1]) == bytes;
            fprintf(f[2], "{\"width\":%u,\"height\":%u,\"image\":%u,\"run\":%llu,\"format\":\"rgba8\"}\n", H.dump_width, H.dump_height, H.dump_image,
                    (unsigned long long)H.dump_run);
            if (!ok) snprintf(err, err_size, "write failed");
        }
        for (unsigned i = 0u; i < 3u; ++i) if (f[i]) fclose(f[i]);
    }
    SDL_UnlockMutex(S.mutex);
    return ok;
}

/* ===== the Native backend (no OpenGL) ========================================================================================== */
/* The executor overlaps the immutable base and published phase chunks with CPU preparation,
 * and waits for the seal before scanout. The GPU thread keeps the
 * state machine, the streaming contract and the notify of the OpenGL GPU thread (native_gpu_service):
 * QUEUED/DISPATCHING -> (WAIT_INPUT while the journal is still being published) -> SUBMITTED -> READY,
 * PUBLISHED frees the work, CANCELLED releases what it started. */
static struct {
    SDL_Thread *thread;
    VkFence fence;
    int in_flight;               /* R / S.work_planes belong to s_native_gpu_work */
    uint64_t works, failed_works, refusals[NATIVE_VK_REFUSE_COUNT];
    uint32_t last_fail_stage, last_fail_command, last_fail_kind;
    int32_t last_fail_code;
    char reason[160];
} B;

/* Waits for the device, then drops what an unfinished execution holds. */
static void nvk_backend_abandon(void)
{
    (void)nvk_submit_thread_sync();
    S.pending_count = S.pending_head = 0u;
    for (uint32_t i = 0u; i < S.decoded_count; ++i) S.decoded[i].valid = 0;
    S.upload_valid = 0;
    (void)vk_core_device_wait_idle();
    R.recording = 0;
    B.fence = VK_NULL_HANDLE;
    nvk_release();
    nvk_planes_to_spare();
    B.in_flight = 0;
}

static int nvk_backend_submit(GlNativeGpuWork *work, uint64_t started, uint64_t cpu_started)
{
    s_retry_phases = 0;
    SDL_LockMutex(s_native_state_mutex);
    int ok = !work->sealed || !work->render_failed;
    int refusal = -1;
    if (ok && work->sealed && (refusal = nvk_refusal(work, S.interlock)) >= 0) {
        ++B.refusals[refusal];
        native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_COMMAND, 0u, 0x1000u + (uint32_t)refusal, 0u);
        ok = 0;
    }
    SDL_UnlockMutex(s_native_state_mutex);
    work->timing.dispatch_begin_ns = started;
    if (ok) {
        B.in_flight = 1;
        ok = nvk_execute(work, &B.fence);
        if (ok == 2) {
            nvk_backend_abandon();
            SDL_LockMutex(s_native_state_mutex);
            work->state = 0;
            s_native_compiler_diag.gpu.cancelled++;
            SDL_UnlockMutex(s_native_state_mutex);
            return 1;
        }
        if (ok && work->render_failed) ok = 0;
        if (!ok) {
            native_gpu_fail_note(work, R.fail_stage ? R.fail_stage : GL_RENDERER_NATIVE_GPU_FAILURE_COMMAND,
                                 R.fail_command, R.fail_kind, (unsigned)R.fail_code);
            B.last_fail_stage = R.fail_stage; B.last_fail_command = R.fail_command;
            B.last_fail_kind = R.fail_kind; B.last_fail_code = R.fail_code;
            nvk_backend_abandon();
        }
    }
    if (!ok) {
        ++B.failed_works;
        SDL_LockMutex(s_native_state_mutex);
        work->failed = 1;
        work->state = NATIVE_GPU_READY;
        SDL_UnlockMutex(s_native_state_mutex);
        return -1;
    }
    ++B.works;
    for (unsigned image = 0u; work->scanout && image <= work->phase_count; ++image) {
        work->image_widths[image] = S.images[image].width;
        work->image_heights[image] = S.images[image].height;
    }
    work->destination_barriers = R.c.destination_barriers;
    work->skipped_barriers = R.c.skipped_barriers;
    work->scheduled_layers = R.c.scheduled_layers;
    work->geometry_draws = R.c.geometry_draws;
    work->transfer_draws = R.c.transfer_draws;
    work->word_uploads = R.c.word_uploads;
    work->snapshot_commands = R.c.snapshot_commands;
    work->depth_tested_triangles = R.c.depth_tested_triangles;
    work->submitted_ns = SDL_GetTicksNS();
    work->timing.dispatch_end_ns = work->submitted_ns;
    work->timing.service_ns += work->submitted_ns - started;
    work->timing.service_cpu_ns += native_thread_cpu_ns() - cpu_started;
    SDL_LockMutex(s_native_state_mutex);
    s_native_compiler_diag.gpu.submitted++;
    s_native_compiler_diag.gpu.commands += work->count + work->phase_command_count;
    s_native_compiler_diag.gpu.captured_bytes += work->bytes + work->phase_bytes;
    s_native_compiler_diag.gpu.geometry_draws += work->geometry_draws;
    s_native_compiler_diag.gpu.transfer_draws += work->transfer_draws;
    s_native_compiler_diag.gpu.word_uploads += work->word_uploads;
    s_native_compiler_diag.gpu.snapshot_commands += work->snapshot_commands;
    s_native_compiler_diag.gpu.destination_barriers += work->destination_barriers;
    s_native_compiler_diag.gpu.skipped_barriers += work->skipped_barriers;
    s_native_compiler_diag.gpu.scheduled_layers += work->scheduled_layers;
    s_native_compiler_diag.gpu.depth_tested_triangles += work->depth_tested_triangles;
    work->state = NATIVE_GPU_SUBMITTED;
    SDL_UnlockMutex(s_native_state_mutex);
    return 1;
}

static int nvk_backend_complete(GlNativeGpuWork *work, uint64_t started, uint64_t cpu_started)
{
    const XgRenderFenceStatus status = native_vk_sync_poll(B.fence);
    SDL_LockMutex(s_native_state_mutex);
    s_native_compiler_diag.gpu.fence_polls++;
    s_native_compiler_diag.gpu.fence_pending += status == XG_RENDER_FENCE_PENDING;
    SDL_UnlockMutex(s_native_state_mutex);
    if (status == XG_RENDER_FENCE_PENDING) return 0;
    if (status == XG_RENDER_FENCE_READY && R.capture_used && !s_retry_phases) {
        vk_core_buffer_invalidate(&S.capture_readback, 0u, VK_WHOLE_SIZE);
        if (*(const uint32_t *)S.capture_readback.mapped) {
            P.capture_retry_bits |= *(const uint32_t *)S.capture_readback.mapped;
            ++P.capture_retries;
            S.capture_failed_scene = work->identity.scene_generation;
            nvk_release();
            s_retry_phases = 1;
            if (nvk_execute(work, &B.fence)) {
                work->submitted_ns = SDL_GetTicksNS();
                return 0;
            }
            native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_COMMAND, work->count, 0u, (unsigned)R.fail_code);
            work->failed = 1;
            nvk_backend_abandon();
            SDL_LockMutex(s_native_state_mutex);
            work->state = NATIVE_GPU_READY; ++B.failed_works;
            SDL_UnlockMutex(s_native_state_mutex);
            return -1;
        }
    }
    int hashes_ok = 1;
    size_t mapped_bytes = 0u;
    const uint64_t complete_started = SDL_GetTicksNS();
    if (status == XG_RENDER_FENCE_READY && P.submit_pool && P.submit_queries) {
        uint64_t times[NVK_SUBMIT_TS];
        if (vkGetQueryPoolResults(vk_core_device(), P.submit_pool, 0u, P.submit_queries,
            sizeof(times), times, sizeof(times[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (uint32_t q = 0u; q < P.submit_queries; q += 2u) {
                const uint64_t ns = (uint64_t)((times[q+1u] - times[q]) * (double)vk_core_caps()->timestamp_period_ns);
                ++P.timed_submits;
                P.submit_gpu_ns += ns;
                if (ns > P.submit_gpu_max_ns) P.submit_gpu_max_ns = ns;
            }
        }
    }
    if (status == XG_RENDER_FENCE_READY && P.pool && P.stamped) {
        uint64_t detail_ts[NVK_DETAIL_TS];
        if (P.detail_queries && vkGetQueryPoolResults(vk_core_device(), P.pool, NVK_TS + NVK_CAPTURE_TS,
            P.detail_queries, sizeof(detail_ts), detail_ts, sizeof(detail_ts[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (uint32_t q = 0u; q < P.detail_queries; q += 2u) {
                const uint32_t kind = P.detail_kind[q / 2u];
                ++P.detail_runs[kind];
                P.detail_ns[kind] += (uint64_t)((double)(detail_ts[q+1u]-detail_ts[q]) * vk_core_caps()->timestamp_period_ns);
            }
        }
        uint64_t capture_ts[NVK_CAPTURE_TS];
        if (P.capture_queries && vkGetQueryPoolResults(vk_core_device(), P.pool, NVK_TS, P.capture_queries,
            sizeof(capture_ts), capture_ts, sizeof(capture_ts[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            P.capture_gpu_runs += P.capture_queries / 4u;
            for (uint32_t q = 0u; q < P.capture_queries; q += 4u)
                for (uint32_t i = 0u; i < 3u; ++i)
                    P.capture_gpu_ns[i] += (uint64_t)((double)(capture_ts[q+i+1u]-capture_ts[q+i]) * vk_core_caps()->timestamp_period_ns);
        }
        uint64_t ts[NVK_TS];
        if (vkGetQueryPoolResults(vk_core_device(), P.pool, 0u, NVK_TS, sizeof(ts), ts, sizeof(ts[0]),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            ++P.gpu_works;
            P.base_gpu_ns += (uint64_t)((double)(ts[5]-ts[4]) * vk_core_caps()->timestamp_period_ns);
            P.phases_gpu_ns += (uint64_t)((double)(ts[7]-ts[6]) * vk_core_caps()->timestamp_period_ns);
            for (unsigned i = 0u; i < NVK_GPU_PHASES; ++i) {
                const uint64_t ns = (uint64_t)((double)(ts[i + 1u] - ts[i]) * vk_core_caps()->timestamp_period_ns);
                P.gpu_ns[i] += ns;
                if (ns > P.gpu_max[i]) P.gpu_max[i] = ns;
            }
        }
    }
    if (status == XG_RENDER_FENCE_READY) {
        const uint64_t latency = SDL_GetTicksNS() - work->submitted_ns;
        SDL_LockMutex(s_native_state_mutex);
        s_native_compiler_diag.gpu.fence_latency_ns += latency;
        if (latency > s_native_compiler_diag.gpu.fence_latency_max_ns) s_native_compiler_diag.gpu.fence_latency_max_ns = latency;
        SDL_UnlockMutex(s_native_state_mutex);
        uint64_t gpu_visible = 0u;
        if (R.hash_groups && work->scanout) {
            const uint64_t hash_started = native_thread_cpu_ns();
            work->timing.hash_begin_ns = SDL_GetTicksNS();
            vk_core_buffer_invalidate(&S.hash_results, 0, (VkDeviceSize)R.hash_groups * 16u);
            const uint32_t (*groups)[4] = S.hash_results.mapped;
            uint64_t digest = native_image_mix64((uint64_t)S.images[0].width * S.images[0].height);
            for (uint32_t g = 0u; g < R.hash_groups; ++g) {
                digest ^= (uint64_t)groups[g][0] | ((uint64_t)groups[g][1] << 32u);
                gpu_visible += groups[g][2];
            }
            work->image_digests[0] = digest;
            work->visible_pixels = gpu_visible;
            mapped_bytes += (size_t)R.hash_groups * 16u;
            work->timing.hash_cpu_ns += native_thread_cpu_ns() - hash_started;
            work->timing.hash_end_ns = SDL_GetTicksNS();
        }
        for (unsigned i = 0u; i <= work->phase_count && work->scanout; ++i) {
            const uint64_t copy_started = SDL_GetTicksNS();
            if (!R.readback_used[i]) { if (i || !R.hash_groups) work->image_digests[i] = 0u; continue; }
            const size_t bytes = (size_t)S.images[i].width * S.images[i].height * 4u;
            vk_core_buffer_invalidate(&R.readback[i], 0, VK_WHOLE_SIZE);
            const uint8_t *pixels = R.readback[i].mapped;
            mapped_bytes += bytes;
            if (s_native_motion_probe.target_vblank) {
                if (s_native_gpu_readback_capacity[i] >= bytes) {
                    work->readback_pixels[i] = s_native_gpu_readback_spare[i];
                    work->readback_capacity[i] = s_native_gpu_readback_capacity[i];
                    s_native_gpu_readback_spare[i] = NULL; s_native_gpu_readback_capacity[i] = 0;
                } else {
                    work->readback_pixels[i] = malloc(bytes);
                    work->readback_capacity[i] = bytes;
                }
                if (work->readback_pixels[i]) memcpy(work->readback_pixels[i], pixels, bytes);
                else hashes_ok = 0;
            }
            const uint64_t hash_started = native_thread_cpu_ns();
            if (!i) work->timing.hash_begin_ns = SDL_GetTicksNS();
            uint64_t visible = 0u;
            work->image_digests[i] = native_gpu_image_digest(pixels, bytes, !i ? &visible : NULL);
            if (!i) {
                if (!R.hash_groups) work->visible_pixels = visible;
                if (gl_renderer_native_guest_reference_enabled()) {
                    const uint32_t *rgba = (const uint32_t *)(const void *)pixels;
                    work->reference_pixels = malloc((size_t)work->scanout_width * work->scanout_height * sizeof(uint32_t));
                    if (!work->reference_pixels) hashes_ok = 0;
                    else for (uint32_t y = 0; y < work->scanout_height; ++y)
                        for (uint32_t x = 0; x < work->scanout_width; ++x)
                            work->reference_pixels[(size_t)y * work->scanout_width + x] =
                                rgba[(size_t)native_resolution_edge(y, work->scanout_height, S.images[0].height) * S.images[0].width +
                                    native_resolution_edge(x, work->scanout_width, S.images[0].width)];
                }
            }
            work->timing.hash_cpu_ns += native_thread_cpu_ns() - hash_started;
            work->timing.hash_end_ns = SDL_GetTicksNS();
            work->timing.copy_ns += SDL_GetTicksNS() - copy_started;
        }
    }
    B.fence = VK_NULL_HANDLE;
    nvk_release();
    B.in_flight = 0;
    nvk_prof_cpu(5u, SDL_GetTicksNS() - complete_started);
    SDL_LockMutex(s_native_state_mutex);
    if (status != XG_RENDER_FENCE_READY) {
        native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_FENCE_WAIT, work->count, 0u, 0u);
        if (!work->fail_gl_error) work->fail_gl_error = (uint32_t)status;
        (void)vk_core_device_wait_idle();
    } else if (!hashes_ok) native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_READBACK, work->count, 0u, 0u);
    work->failed |= !hashes_ok || status != XG_RENDER_FENCE_READY;
    if (!work->failed) {
        s_native_compiler_diag.gpu.completed++;
        if (work->scanout) s_native_compiler_diag.gpu.readback_bytes += mapped_bytes;
    } else ++B.failed_works;
    work->timing.ready_ns = SDL_GetTicksNS();
    work->timing.service_ns += work->timing.ready_ns - started;
    work->timing.service_cpu_ns += native_thread_cpu_ns() - cpu_started;
    work->state = NATIVE_GPU_READY;
    SDL_UnlockMutex(s_native_state_mutex);
    return work->failed ? -1 : 1;
}

/* native_gpu_service for the Vulkan executor. */
static int nvk_backend_service(void)
{
    SDL_LockMutex(s_native_state_mutex);
    GlNativeGpuWork *work = s_native_gpu_work;
    if (!work || !work->state || work->state == NATIVE_GPU_READY) { SDL_UnlockMutex(s_native_state_mutex); return 0; }
    if (work->cancel_requested) work->state = NATIVE_GPU_CANCELLED;
    const int state = work->state;
    const char *stream = getenv("PSX_NATIVE_VK_PHASE_STREAM");
    if ((state == NATIVE_GPU_QUEUED || state == NATIVE_GPU_DISPATCHING) && !work->sealed &&
        (!work->phase_stream_ready || (stream && stream[0] == '0'))) {
        /* The journal is still being published: publication wakes the thread again. */
        work->state = NATIVE_GPU_WAIT_INPUT;
        SDL_UnlockMutex(s_native_state_mutex);
        return 0;
    }
    if (state == NATIVE_GPU_QUEUED) work->state = NATIVE_GPU_DISPATCHING;
    SDL_UnlockMutex(s_native_state_mutex);
    const uint64_t started = SDL_GetTicksNS();
    const uint64_t cpu_started = native_thread_cpu_ns();
    int result = 0;
    if (state == NATIVE_GPU_PUBLISHED) {
        SDL_LockMutex(s_native_state_mutex);
        s_native_gpu_work = NULL;
        SDL_UnlockMutex(s_native_state_mutex);
        native_work_free(work);
        result = 1;
    } else if (state == NATIVE_GPU_CANCELLED) {
        if (B.in_flight) nvk_backend_abandon();
        SDL_LockMutex(s_native_state_mutex);
        work->state = 0;
        s_native_compiler_diag.gpu.cancelled++;
        SDL_UnlockMutex(s_native_state_mutex);
        result = 1;
    } else if (state == NATIVE_GPU_SUBMITTED) {
        result = nvk_backend_complete(work, started, cpu_started);
    } else {
        result = nvk_backend_submit(work, started, cpu_started);
    }
    const uint64_t elapsed = SDL_GetTicksNS() - started;
    SDL_LockMutex(s_native_state_mutex);
    s_native_compiler_diag.gpu.service_ns += elapsed;
    if (state == NATIVE_GPU_QUEUED || state == NATIVE_GPU_DISPATCHING) s_native_compiler_diag.gpu.submit_ns += elapsed;
    if (state == NATIVE_GPU_SUBMITTED) s_native_compiler_diag.gpu.finish_ns += elapsed;
    if (elapsed > s_native_compiler_diag.gpu.service_max_ns) s_native_compiler_diag.gpu.service_max_ns = elapsed;
    SDL_UnlockMutex(s_native_state_mutex);
    return result;
}

static int nvk_backend_thread(void *unused)
{
    (void)unused;
    SDL_AtomicSet(&s_native_gpu_status, 1);
    while (SDL_AtomicGet(&s_native_gpu_run)) {
        SDL_LockMutex(s_native_state_mutex);
        const int state = s_native_gpu_work ? s_native_gpu_work->state : 0;
        if (!state || state == NATIVE_GPU_READY || state == NATIVE_GPU_WAIT_INPUT) {
            SDL_CondWaitTimeout(s_native_gpu_condition, s_native_state_mutex, 2);
            SDL_UnlockMutex(s_native_state_mutex);
            continue;
        }
        SDL_UnlockMutex(s_native_state_mutex);
        const int result = nvk_backend_service();
        SDL_LockMutex(s_native_state_mutex);
        if (result && s_native_gpu_notify) s_native_gpu_notify(s_native_gpu_notify_data);
        SDL_UnlockMutex(s_native_state_mutex);
        if (result < 0) {
            SDL_AtomicSet(&s_native_gpu_status, -1);
            while (SDL_AtomicGet(&s_native_gpu_run)) SDL_Delay(1);
            break;
        }
        if (!result) {
#if defined(PSX_SDL3)
            if (state == NATIVE_GPU_SUBMITTED) SDL_DelayNS(100000u);
            else SDL_Delay(1);
#else
            SDL_Delay(1);
#endif
        }
    }
    if (B.in_flight) nvk_backend_abandon();
    else (void)vk_core_device_wait_idle();
    return 0;
}

/* ---- the NativeGpuBackend table ------------------------------------------------------------------------------------------------- */
static int nvk_be_plane_resident(unsigned plane)
{
    return plane < GL_NATIVE_GPU_PHASE_BASE && S.committed[plane].color.image != VK_NULL_HANDLE;
}

static void nvk_be_plane_extent(const GlNativeGpuWork *work, unsigned plane, uint32_t *width, uint32_t *height)
{
    (void)work;
    const NvkPlane *p = plane < GL_NATIVE_GPU_PLANES ? &S.work_planes[plane] : NULL;
    *width = p && p->color.image ? p->width : 0u;
    *height = p && p->color.image ? p->height : 0u;
}

/* Publication exchanges the work's planes with the committed ones. */
static void nvk_be_apply_planes(GlNativeGpuWork *work)
{
    (void)work;
    for (uint32_t i = 0u; i < GL_NATIVE_GPU_PHASE_BASE; ++i) {
        NvkPlane old = S.committed[i];
        S.committed[i] = S.work_planes[i];
        S.work_planes[i] = old;
    }
    nvk_planes_to_spare();
}

static NvkPlane *nvk_image_ptr(uint64_t handle) { return (NvkPlane *)(uintptr_t)handle; }

/* Swaps the endpoint image `slot` with the work's scan image: the endpoint owns the new one
 * and the work's next scan image is the endpoint's retired one. */
static void nvk_adopt_one(NativeImageSlot *slot, NvkPlane *scan)
{
    NvkPlane *fresh = malloc(sizeof(*fresh));
    if (!fresh) { R.fail_code = (int32_t)VK_ERROR_OUT_OF_HOST_MEMORY; abort(); }
    *fresh = *scan;
    NvkPlane *old = nvk_image_ptr(slot->color);
    if (old) { *scan = *old; free(old); }
    else memset(scan, 0, sizeof(*scan));
    slot->color = slot->mask = (uint64_t)(uintptr_t)fresh;
    slot->width = fresh->width; slot->height = fresh->height;
    slot->mask_valid = 1;
}

static void nvk_be_adopt_images(GlNativeEndpointSlot *endpoint, GlNativeGpuWork *work, uint32_t phase_count)
{
    for (uint32_t i = 0u; i <= phase_count; ++i) {
        if (!i) {
            nvk_adopt_one(&endpoint->image, &S.images[0]);
            endpoint->mask_valid = 1;
            endpoint->upload_state = GL_NATIVE_ENDPOINT_UPLOAD_READY;
        } else {
            NativeImageSlot *phase = &endpoint->phases[i - 1u];
            nvk_adopt_one(phase, &S.images[i]);
            phase->digest = work->image_digests[i];
            phase->upload_state = GL_NATIVE_ENDPOINT_UPLOAD_READY;
        }
    }
    nvk_discard_retired_layouts_locked();
    for (uint32_t i = 0u; i < GL_NATIVE_GPU_PLANES; ++i) {
        if (nvk_cached_plane_expired(&S.spare[i]))
            nvk_discard_cached_plane(&S.spare[i]);
        if (nvk_cached_plane_expired(&S.spare_snapshots[i]))
            nvk_discard_cached_plane(&S.spare_snapshots[i]);
    }
    /* adopt_one can return a retired image from an older layout to the
     * executor. It must not keep that old cache alive indefinitely. */
    for (uint32_t i = 0u; i < NVK_IMAGES; ++i)
        if (nvk_cached_plane_expired(&S.images[i]))
            nvk_discard_cached_plane(&S.images[i]);
}

static XgRenderFenceStatus nvk_be_sync_poll(void *sync) { return native_vk_sync_poll(sync); }

static int nvk_be_sync_wait_delete(void *sync)
{
    VkFence fence = (VkFence)sync;
    if (!vk_core_check(vkWaitForFences(vk_core_device(), 1u, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return 0;
    vkDestroyFence(vk_core_device(), fence, NULL);
    return 1;
}

/* A CPU-built endpoint image (staged pixels): one sampled RGBA8 image, no mask. */
static int nvk_be_upload_staged(NativeImageSlot *image, int width, int height, const uint32_t *pixels)
{
    if (!image || !pixels || width <= 0 || height <= 0) return 0;
    NvkPlane *p = nvk_image_ptr(image->color);
    if (p && (p->width != (uint32_t)width || p->height != (uint32_t)height || p->framebuffer)) {
        nvk_plane_destroy(p); free(p); p = NULL; image->color = image->mask = 0u;
    }
    if (!p) {
        p = calloc(1u, sizeof(*p));
        if (!p) return 0;
        if (!vk_core_image_create(&p->color, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)width, (uint32_t)height, 1u,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "native staged endpoint")) { free(p); return 0; }
        p->width = (uint32_t)width; p->height = (uint32_t)height;
        image->color = (uint64_t)(uintptr_t)p;
        image->mask = 0u;
    }
    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4u;
    VkCoreBuffer stage;
    if (!vk_core_buffer_create(&stage, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_CORE_MEMORY_UPLOAD, "native staged pixels"))
        return 0;
    memcpy(stage.mapped, pixels, (size_t)bytes);
    vk_core_buffer_flush(&stage, 0, bytes);
    VkCommandBuffer cmd = vk_core_oneshot_begin();
    int ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        vk_core_image_barrier(cmd, p->color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        region.imageExtent = (VkExtent3D){ (uint32_t)width, (uint32_t)height, 1u };
        vkCmdCopyBufferToImage(cmd, stage.buffer, p->color.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
        if (S.publish_barriers) {
            VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0u, 1u, &barrier, 0u, NULL, 0u, NULL);
        }
        ok = vk_core_oneshot_end_wait(cmd);
    }
    vk_core_buffer_destroy(&stage);
    return ok;
}

static void nvk_be_image_destroy(NativeImageSlot *image)
{
    NvkPlane *p = nvk_image_ptr(image->color);
    if (p) { nvk_plane_destroy(p); free(p); }
    image->color = image->mask = image->target = 0u;
}

static const NativeGpuBackend s_native_vk_backend = {
    .plane_resident = nvk_be_plane_resident,
    .plane_extent = nvk_be_plane_extent,
    .apply_planes = nvk_be_apply_planes,
    .adopt_images = nvk_be_adopt_images,
    .sync_poll = nvk_be_sync_poll,
    .sync_wait_delete = nvk_be_sync_wait_delete,
    .upload_staged = nvk_be_upload_staged,
    .image_destroy = nvk_be_image_destroy,
};

int native_vk_endpoint_views(uint64_t color_handle, VkImageView *color, VkImageView *mask)
{
    const NvkPlane *p = nvk_image_ptr(color_handle);
    if (!p || !p->color.image) return 0;
    *color = p->color.view;
    *mask = p->alpha_view;
    return 1;
}

int native_vk_endpoint_damage(uint64_t color_handle, int32_t bounds[4])
{
    const NvkPlane *p = nvk_image_ptr(color_handle);
    if (!p || !p->color.image || !p->damage_valid) return 0;
    memcpy(bounds, p->damage_bounds, sizeof(p->damage_bounds));
    return 1;
}

int native_vk_endpoint_damage_regions(uint64_t color_handle, int32_t regions[NATIVE_VK_DAMAGE_REGIONS][4])
{
    const NvkPlane *p = nvk_image_ptr(color_handle);
    if (!p || !p->color.image || !p->damage_valid || !p->damage_regions_side) return 0;
    memcpy(regions, p->damage_regions, sizeof(p->damage_regions));
    return p->damage_regions_side;
}

int native_vk_backend_start(char *reason, size_t reason_size)
{
    memset(&B, 0, sizeof(B));
    if (getenv("PSX_NATIVE_GPU_CANONICAL")) {
        snprintf(reason, reason_size, "PSX_NATIVE_GPU_CANONICAL (GPU canonical readback) is not implemented on Vulkan");
        return 0;
    }
    if (!vk_core_ready()) { snprintf(reason, reason_size, "vk_core is not initialised"); return 0; }
    if (S.tried) { snprintf(reason, reason_size, "the Vulkan executor already ran (shadow mode)"); return 0; }
    S.tried = 1;
    s_backend_mode = 1;
    if (!nvk_init()) {
        snprintf(reason, reason_size, "executor initialisation failed: %s", S.reason[0] ? S.reason : vk_core_failure());
        nvk_teardown();
        s_backend_mode = 0;
        return 0;
    }
    S.ready = 1;
    g_native_backend = &s_native_vk_backend;
    SDL_AtomicSet(&s_native_gpu_run, 1);
    SDL_AtomicSet(&s_native_gpu_status, 0);
    s_native_gpu_thread = SDL_CreateThread(nvk_backend_thread, "native-gpu-vk", NULL);
    B.thread = s_native_gpu_thread;
    if (!B.thread) {
        snprintf(reason, reason_size, "GPU thread creation failed");
        nvk_teardown();
        g_native_backend = NULL;
        s_backend_mode = 0;
        return 0;
    }
    while (!SDL_AtomicGet(&s_native_gpu_status)) SDL_Delay(1);
    return 1;
}

void native_vk_backend_join(void)
{
    if (B.thread) {
        if (s_native_state_mutex) {
            SDL_LockMutex(s_native_state_mutex);
            s_native_gpu_notify = NULL; s_native_gpu_notify_data = NULL;
            SDL_AtomicSet(&s_native_gpu_run, 0);
            SDL_CondBroadcast(s_native_gpu_condition);
            SDL_UnlockMutex(s_native_state_mutex);
        }
        SDL_WaitThread(B.thread, NULL);
        B.thread = NULL;
        s_native_gpu_thread = NULL;
    }
}

void native_vk_backend_stop(void)
{
    native_vk_backend_join();
    if (!S.tried) return;
    for (uint32_t i = 0u; i < GL_NATIVE_ENDPOINT_CAPACITY; ++i) {
        nvk_be_image_destroy(&s_native_endpoints[i].image);
        for (uint32_t p = 0u; p < GL_NATIVE_MOTION_PHASE_CAPACITY; ++p) nvk_be_image_destroy(&s_native_endpoints[i].phases[p]);
    }
    nvk_teardown();
    s_backend_mode = 0;
    g_native_backend = NULL;
}

int native_vk_backend_json(char *out, int capacity)
{
    int n = snprintf(out, (size_t)capacity,
        "{\"running\":%d,\"tier\":\"%s\",\"works\":%llu,\"failed_works\":%llu,\"last_fail\":{\"stage\":%u,\"command\":%u,\"kind\":%u,\"code\":%d},"
        "\"hd_resident\":%llu,\"hd_bytes\":%llu,\"hd_failed\":%llu,\"material_pipelines\":%u,"
        "\"layout_cache_resets\":%llu,\"layout_cache_evictions\":%llu,\"layout_cache_evicted_bytes\":%llu,"
        "\"streamed_works\":%llu,\"streamed_chunks\":%llu,\"cleared_spans\":%llu,\"cleared_rects\":%llu,\"damage_bounds_pixels\":%llu,"
        "\"capture_pixels\":%llu,\"capture_full_pixels\":%llu,"
        "\"timed_submits\":%llu,\"submit_gpu_ns\":%llu,\"submit_gpu_max_ns\":%llu,\"submit_timing_overflow\":%llu,"
        "\"rendered_phases\":%llu,\"schedule_cache_hits\":%llu,\"core_batches\":%llu,\"core_batch_triangles\":%llu,\"draw_calls\":%llu,\"submissions\":%llu,"
        "\"raw_vertex_upload_bytes\":%llu,\"submit_worker\":%d,\"queue_ahead\":%u,\"fragment_node_bytes\":%u,"
        "\"narrow_submits\":%llu,\"wide_submits\":%llu,"
        "\"decoded_pages\":%llu,\"decoded_reuses\":%llu,\"decoded_hint_hits\":%llu,\"decoded_hint_misses\":%llu,\"decoded_negative_hits\":%llu,\"decoded_work_full\":%llu,\"decoded_slots_full\":%llu,\"aniso_single_tap_triangles\":%llu,\"damage_phases\":%llu,\"damage_pixels\":%llu,\"damage_full_pixels\":%llu,"
        "\"descriptor_hint_hits\":%llu,\"descriptor_hint_misses\":%llu,"
        "\"depth_reads_elided\":%llu,\"capture_runs\":%llu,\"capture_triangles\":%llu,\"capture_retries\":%llu,"
        "\"decoded_arrays\":%d,\"damage_tile\":%u,\"hd_batch\":%d,\"linked_capture\":%d,\"capture_retry_bits\":%u,\"hardware_aniso\":%d,\"refusals\":{",
        B.thread != NULL, S.interlock ? "interlock" : "core", (unsigned long long)B.works, (unsigned long long)B.failed_works, B.last_fail_stage,
        B.last_fail_command, B.last_fail_kind, B.last_fail_code, (unsigned long long)s_hd_cache.resident,
        (unsigned long long)s_hd_cache.bytes, (unsigned long long)s_hd_cache.failed, S.material_count,
        (unsigned long long)P.layout_cache_resets, (unsigned long long)P.layout_cache_evictions,
        (unsigned long long)P.layout_cache_evicted_bytes,
        (unsigned long long)P.streamed_works, (unsigned long long)P.streamed_chunks,
        (unsigned long long)P.cleared_spans, (unsigned long long)P.cleared_rects,
        (unsigned long long)P.damage_bounds_pixels,
        (unsigned long long)P.capture_pixels, (unsigned long long)P.capture_full_pixels,
        (unsigned long long)P.timed_submits, (unsigned long long)P.submit_gpu_ns,
        (unsigned long long)P.submit_gpu_max_ns, (unsigned long long)P.submit_timing_overflow,
        (unsigned long long)P.rendered_phases,
        (unsigned long long)s_template_schedule_hits, (unsigned long long)P.core_batches, (unsigned long long)P.core_batch_triangles,
        (unsigned long long)P.draw_calls, (unsigned long long)P.submissions,
        (unsigned long long)P.raw_vertex_upload_bytes, SQ.thread != NULL, S.pace_limit, S.capture_enabled ? 16u : 0u,
        (unsigned long long)P.narrow_submits, (unsigned long long)P.wide_submits,
        (unsigned long long)P.decoded_pages, (unsigned long long)P.decoded_reuses,
        (unsigned long long)P.decoded_hint_hits, (unsigned long long)P.decoded_hint_misses,
        (unsigned long long)P.decoded_negative_hits,
        (unsigned long long)P.decoded_work_full, (unsigned long long)P.decoded_slots_full,
        (unsigned long long)P.aniso_single_tap_triangles, (unsigned long long)P.damage_phases,
        (unsigned long long)P.damage_pixels, (unsigned long long)P.damage_full_pixels,
        (unsigned long long)P.descriptor_hint_hits, (unsigned long long)P.descriptor_hint_misses,
        (unsigned long long)P.depth_reads_elided,
        (unsigned long long)P.capture_runs, (unsigned long long)P.capture_triangles, (unsigned long long)P.capture_retries, S.decode_arrays, 1u << S.damage_tile_shift, S.hd_batch, S.linked_capture, P.capture_retry_bits, S.hardware_aniso);
    for (unsigned i = 0u; i < NATIVE_VK_REFUSE_COUNT && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%llu", i ? "," : "", s_refusal_names[i],
                      (unsigned long long)B.refusals[i]);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n),
        "},\"batch_counts\":{\"capture_draws\":%llu,\"capture_triangles\":%llu,"
        "\"hd_draws\":%llu,\"hd_triangles\":%llu,\"guest_draws\":%llu,\"guest_triangles\":%llu",
        (unsigned long long)P.batch_draws[0], (unsigned long long)P.batch_triangles[0],
        (unsigned long long)P.batch_draws[1], (unsigned long long)P.batch_triangles[1],
        (unsigned long long)P.batch_draws[2], (unsigned long long)P.batch_triangles[2]);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n),
        "},\"capture_gpu\":{\"runs\":%llu,\"reset_ns\":%llu,\"draw_ns\":%llu,\"resolve_ns\":%llu,\"base_ns\":%llu,\"phases_ns\":%llu",
        (unsigned long long)P.capture_gpu_runs, (unsigned long long)P.capture_gpu_ns[0],
        (unsigned long long)P.capture_gpu_ns[1], (unsigned long long)P.capture_gpu_ns[2],
        (unsigned long long)P.base_gpu_ns, (unsigned long long)P.phases_gpu_ns);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "},\"profile\":{\"works\":%llu,\"gpu_works\":%llu,\"cpu_us_avg\":{",
                                    (unsigned long long)P.works, (unsigned long long)P.gpu_works);
    for (unsigned i = 0u; i < NVK_CPU_PHASES && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%.1f", i ? "," : "", s_cpu_phase_names[i],
                      P.works ? (double)P.cpu_ns[i] / 1e3 / (double)P.works : 0.0);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "},\"cpu_us_max\":{");
    for (unsigned i = 0u; i < NVK_CPU_PHASES && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%.1f", i ? "," : "", s_cpu_phase_names[i], (double)P.cpu_max[i] / 1e3);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "},\"gpu_us_avg\":{");
    for (unsigned i = 0u; i < NVK_GPU_PHASES && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%.1f", i ? "," : "", s_gpu_phase_names[i],
                      P.gpu_works ? (double)P.gpu_ns[i] / 1e3 / (double)P.gpu_works : 0.0);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "},\"gpu_us_max\":{");
    for (unsigned i = 0u; i < NVK_GPU_PHASES && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%.1f", i ? "," : "", s_gpu_phase_names[i], (double)P.gpu_max[i] / 1e3);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "},\"detail_gpu_ns\":{");
    const char *detail_names[4] = { "plane_copy", "schedule_guest", "scanout", "schedule_hd" };
    for (unsigned i = 0u; i < 4u && n < capacity; ++i)
        n += snprintf(out + n, (size_t)(capacity - n), "%s\"%s\":%llu", i ? "," : "", detail_names[i],
            (unsigned long long)P.detail_ns[i]);
    if (n < capacity) n += snprintf(out + n, (size_t)(capacity - n), "}}}");
    return n;
}

#endif /* PSX_HAVE_VULKAN */
