#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "gpu.h"
#include "gpu_render.h"
#include "gpu_semantic_workload.h"
#include "gpu_sw_renderer.h"
#include "gpu_gl_renderer.h"
#include "native_vk_present.h"
#include "gpu_vram_region_set.h"
#include "psx_render_nclip.h"
#include "xg_render_motion.h"
#include "xg_render_native_mesh.h"
#include "xg_render_native_work.h"
#include "xg_render_semantic_presentation.h"
#include "latency_ring.h"
#include "debug_overlay.h"
#include "runtime_menu.h"
#include "wayland_presentation.h"

#include <limits.h>
#include <float.h>
#include "mod_texture_banks.h"
#include "hd_texture_runtime.h"
#include "frame_interpolation.h"
#include "host_osd.h"
#include "host_time.h"
#include "host_cpu.h"
#include "host_parallel.h"
#include "frame_pacing.h"
#include "psx_rewind.h"

#include "psx_sdl.h"
#include "native_phase_cadence.h"
#include "native_gpu_hazard.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "png_write.h"   /* png_write_rgb — present_shot readback */
#include "native_work_internal.h"
#include "native_raster_layout.h"
#include "native_resolution.h"
#include "gpu_uv.h"

/* Adaptive phase budget: how many motion phases each batch may author.
 * The tick-rate need (interval x hz) assumes generation is free; when the
 * worker cannot render that many phases before the tick deadline, batches
 * publish at the deadline and every tick selects due-whole (phased 0/s at
 * 240 Hz). The budget tracks what fits: partial publish snaps it to the
 * rendered prefix, expiry steps it down, sustained early completion probes
 * upward. Worker-thread only (motion_prepare + denominator site below). */
unsigned int s_native_phase_budget = GL_NATIVE_MOTION_PHASE_CAPACITY;
/* CPU cost per recipe draw: fixed preparation, phase projection, phase replay.
 * Queue/scheduler delays reduce only the current batch's available time. */
uint64_t s_native_phase_cost_per_draw[3];

SDL_mutex *s_native_state_mutex;
GlNativeEndpointSlot s_native_endpoints[GL_NATIVE_ENDPOINT_CAPACITY];
GlNativeFenceSlot s_native_fences[GL_NATIVE_FENCE_CAPACITY];
uint64_t s_native_backend_generation;
uint64_t s_native_temporal_generation;
/* Worker-owned VRAM. Presentation epochs do not reset device memory: the
 * source must drain on scene/artifact changes and upload a full reset/restore
 * baseline. Neither the compatibility renderer nor the presenter owns this. */
uint32_t *s_native_native_vram;
uint16_t *s_native_native_words;
/* The previous committed canvas is a private staging spare. Per-block versions
 * let the next transaction synchronize only changed VRAM, while the current
 * committed pair remains untouched until publication (including on failure). */
static uint32_t *s_native_spare_vram;
static uint16_t *s_native_spare_words;
static uint64_t s_native_spare_versions[VRAM_H][GPU_VRAM_REGION_WORDS_PER_ROW];
void native_canvas_spare_clear(void) {
    free(s_native_spare_vram); s_native_spare_vram = NULL;
    free(s_native_spare_words); s_native_spare_words = NULL;
}
XgPresentationIdentity s_native_native_vram_identity;
static inline GlNativeTextureState native_texture_state_semantic(const GpuRenderMaterial *m) {
    return (GlNativeTextureState){m->tpage, m->texture_page_x, m->texture_page_y, m->clut_x, m->clut_y,
        (uint8_t)m->texture_depth, (uint8_t)m->shading, m->textured, m->raw_texture,
        m->semi_transparent, (uint8_t)m->blend_mode};
}
static inline int native_recipe_pose_equal(const GlNativeRecipeMeshPose *a, const GlNativeRecipeMeshPose *b) {
    return a->scene_id == b->scene_id && a->motion_index == b->motion_index && a->part == b->part &&
        a->producer_id == b->producer_id && a->identity_valid == b->identity_valid;
}
static void native_recipe_mesh_release(GlNativeRecipeMesh *mesh) {
    if (mesh && !--mesh->references) { free(mesh->textures); free(mesh->poses); free(mesh); }
}
static void native_wave_release(GlNativeWaveWarp *warp) {
    if (warp && !--warp->references) free(warp);
}
static int native_wave_geometry_equal(const GlNativeWaveWarp *a, const GlNativeWaveWarp *b) {
    if (a == b) return 1;
    return a && b && a->lifted == b->lifted && !memcmp(&a->lift, &b->lift, sizeof(a->lift)) &&
        a->view_width == b->view_width && a->offset == b->offset && a->origin_y == b->origin_y &&
        !memcmp(&a->current, &b->current, sizeof(a->current));
}
GlNativeMotionHistory s_native_motion_history;
int s_native_recipe_audit_enabled; /* Latched before starting the Native worker. */
int s_native_phase_hashes_enabled;
int s_native_record_audit_enabled;
int s_native_depth_stats_enabled;
int s_native_phase_stats_enabled;
static GlNativePhaseProducer s_native_phase_producers[65536];
uint32_t s_native_phase_producer_count;
volatile uint64_t s_native_coverage_acquires, s_native_coverage_releases, s_native_coverage_release_errors;
volatile uint64_t s_native_coverage_publications;
static volatile GlNativeVertexDiagnostics s_native_vertex_events[64];
static volatile uint64_t s_native_vertex_event_total;
static volatile uint64_t s_native_vertex_phase_total;
static volatile GlNativeMotionRejectEvent s_native_motion_reject_events[GL_RENDERER_NATIVE_MOTION_REJECT_CAPACITY];
static volatile uint64_t s_native_motion_reject_total;
GlNativeViewState s_native_views = {.active = -1};

static void native_views_copy(GlNativeViewState *destination, const GlNativeViewState *source) {
    /* New targets/domains are initialized in full when appended. Unused slots
     * need not cross the staging/publication boundary on every frame. Copy all
     * metadata, including word versions, and retain the existing ownership rules. */
    memcpy(destination, source, offsetof(GlNativeViewState, targets));
    memcpy(destination->targets, source->targets, source->count * sizeof(source->targets[0]));
    memcpy(destination->domains, source->domains, source->domain_count * sizeof(source->domains[0]));
}

/* The depth plane of a VIEW domain lives right after its pixels in the same
 * allocation, with the same (physical VRAM row, x) indexing. */
static size_t native_view_domain_bytes(const GlNativeViewState *views) {
    return (size_t)views->width * VRAM_H * (sizeof(uint32_t) + (views->depth_test ? sizeof(uint32_t) : 0u));
}

static uint32_t *native_view_domain_depth(const GlNativeViewState *views, const GlNativeViewDomain *domain) {
    return views->depth_test && domain->pixels
        ? domain->pixels + (size_t)views->width * VRAM_H : NULL;
}

/* Depth rows aliasing target->pixels (same base row), or NULL. */
static uint32_t *native_view_target_depth(const GlNativeViewState *views, const GlNativeViewTarget *target) {
    if (!views->depth_test || !target->pixels) return NULL;
    for (uint32_t i = 0u; i < views->domain_count; ++i) {
        const GlNativeViewDomain *domain = &views->domains[i];
        if (domain->x == target->x && domain->width == target->width && domain->pixels)
            return native_view_domain_depth(views, domain) + (target->pixels - domain->pixels);
    }
    return NULL;
}

/* Every non-draw write (fill/copy/upload/seed/margin/wave) leaves a pixel that
 * no certified surface owns: its depth returns to far, in lockstep with the
 * GPU span/seed that writes zero into the plane's depth attachment. */
static void native_depth_clear_rows(uint32_t *depth, size_t pitch, int x, int y, int width, int height) {
    if (!depth || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row)
        memset(depth + (size_t)(y + row) * pitch + x, 0, (size_t)width * sizeof(*depth));
}

/* A triangle's complete material: the command's, with a mesh triangle's own
 * texture state applied in scratch. */
static inline const XgRenderIrMaterialState *native_gpu_command_material(
        const GlNativeGpuCommand *command, const uint8_t *data, uint32_t triangle,
        XgRenderIrMaterialState *scratch) {
    if (!command->mesh_count) return &command->draw.primitive.material;
    *scratch = command->draw.primitive.material;
    native_texture_state_apply(scratch,
        &((const GlNativeGpuMeshTriangle *)(const void *)(data + command->data))[triangle].texture);
    return scratch;
}
SDL_atomic_t s_native_gpu_setup_ready;
static int native_gpu_setup_enabled(uint64_t triangles) {
    const char *option=getenv("PSX_NATIVE_GPU_SETUP");
    if (!SDL_AtomicGet(&s_native_gpu_setup_ready) || (option && option[0]=='0')) return 0;
    /* Small batches cost less to prepare than to pack/upload/dispatch compute,
     * including on a single E-core. Keep large meshes on the GPU. Count faces,
     * not commands: one mesh command can contain thousands of triangles.
     * An explicit nonzero override retains the forced-GPU diagnostic path. */
    return triangles > 128u || (option && option[0]);
}
struct GlNativeGpuWork;
uint32_t s_native_gpu_snapshot_capacity;
static GlNativeWorkTiming s_native_work_timing[8192];
uint64_t s_native_work_timing_count;
uint64_t s_native_timing_origin;
uint64_t native_thread_cpu_ns(void) {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec t;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0)
        return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
#endif
    return 0;
}
void native_work_timing_dump(void) {
    const char *path=getenv("PSX_NATIVE_TIMING_OUT");
    if(!path||!path[0])return;
    FILE *f=fopen(path,"w");
    if(!f)return;
    fputs("epoch,sequence,vblank,gpu,scale,width,height,commands,phases,digest,begin_ns,compile_cpu_ns,queued_ns,dispatch_begin_ns,dispatch_end_ns,ready_ns,hash_begin_ns,hash_end_ns,hash_cpu_ns,end_ns,service_ns,service_cpu_ns,copy_ns,deadline_ns,previous_vblank,previous_cycle,temporal_status,fresh,operations,native_cpu_ns,phase_cpu_ns,phase_begin_ns,reference_digest,phase_project_cpu_ns,phase_capture_cpu_ns,phase_geometry_cpu_ns,phase_digests,service_prepare_cpu_ns,service_draw_cpu_ns,service_scanout_cpu_ns,canonical_read_cpu_ns,image_alloc_cpu_ns,gpu_setup_triangles\n",f);
    const uint64_t first = s_native_work_timing_count > 8192u ? s_native_work_timing_count - 8192u : 0u;
    for(uint64_t i=first;i<s_native_work_timing_count;++i) {
        GlNativeWorkTiming absolute=s_native_work_timing[i % 8192u];
        uint64_t *clocks[]={&absolute.begin_ns,&absolute.queued_ns,&absolute.dispatch_begin_ns,
            &absolute.dispatch_end_ns,&absolute.ready_ns,&absolute.hash_begin_ns,&absolute.hash_end_ns,&absolute.end_ns,
            &absolute.deadline_ns,&absolute.phase_begin_ns};
        for(unsigned j=0;j<sizeof(clocks)/sizeof(clocks[0]);++j)if(*clocks[j])*clocks[j]+=s_native_timing_origin;
        const GlNativeWorkTiming *t=&absolute;
        fprintf(f,"%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%016llx,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%llu,%llu,%llu,%016llx,%llu,%llu,%llu,",
            (unsigned long long)t->identity.presentation_epoch,(unsigned long long)t->identity.source_sequence,
            (unsigned long long)t->identity.guest_vblank_sequence,t->gpu,t->scale,t->width,t->height,t->commands,t->phases,
            (unsigned long long)t->digest,(unsigned long long)t->begin_ns,(unsigned long long)t->compile_cpu_ns,
            (unsigned long long)t->queued_ns,(unsigned long long)t->dispatch_begin_ns,(unsigned long long)t->dispatch_end_ns,
            (unsigned long long)t->ready_ns,(unsigned long long)t->hash_begin_ns,(unsigned long long)t->hash_end_ns,
            (unsigned long long)t->hash_cpu_ns,(unsigned long long)t->end_ns,(unsigned long long)t->service_ns,
            (unsigned long long)t->service_cpu_ns,(unsigned long long)t->copy_ns,
            (unsigned long long)t->deadline_ns,(unsigned long long)t->previous_vblank,
            (unsigned long long)t->previous_cycle,t->temporal_status,t->fresh,t->operations,
            (unsigned long long)t->native_cpu_ns,(unsigned long long)t->phase_cpu_ns,
            (unsigned long long)t->phase_begin_ns,(unsigned long long)t->reference_digest,
            (unsigned long long)t->phase_project_cpu_ns,(unsigned long long)t->phase_capture_cpu_ns,
            (unsigned long long)t->phase_geometry_cpu_ns);
        if (s_native_phase_hashes_enabled)
            for (uint32_t phase = 0u; phase < t->phases; ++phase)
                fprintf(f, "%s%016llx", phase ? ":" : "", (unsigned long long)t->phase_digests[phase]);
        fprintf(f,",%llu,%llu,%llu,%llu,%llu,%u\n",(unsigned long long)t->service_prepare_cpu_ns,
            (unsigned long long)t->service_draw_cpu_ns,(unsigned long long)t->service_scanout_cpu_ns,
            (unsigned long long)t->canonical_read_cpu_ns,(unsigned long long)t->image_alloc_cpu_ns,t->gpu_setup_triangles);
    }
    fclose(f);
    path=getenv("PSX_NATIVE_PHASE_OUT");
    if(!path||!path[0])return;
    f=fopen(path,"w");
    if(!f)return;
    fputs("epoch,sequence,vblank,phase,producer,draws,moved_draws,moved_vertices\n",f);
    for(uint32_t i=0u;i<s_native_phase_producer_count;++i) {
        const GlNativePhaseProducer *p=&s_native_phase_producers[i];
        fprintf(f,"%llu,%llu,%llu,%u,%u,%u,%u,%u\n",
            (unsigned long long)p->identity.presentation_epoch,(unsigned long long)p->identity.source_sequence,
            (unsigned long long)p->identity.guest_vblank_sequence,p->phase,p->producer,p->draws,p->moved_draws,p->moved_vertices);
    }
    fclose(f);
}
/* Compiler-owner hints only. Each transaction still owns its own allocations;
 * a larger frame keeps the existing append/retirement growth path. */
static uint32_t s_native_command_reserve, s_native_data_reserve;
static uint32_t s_native_previous_commands, s_native_previous_bytes;
static uint32_t native_gpu_reserve_hint(uint32_t required) {
    uint32_t capacity = 16u;
    while (capacity < required)
        capacity = capacity > UINT32_MAX / 2u ? UINT32_MAX : capacity * 2u;
    return capacity;
}
void native_gpu_buffers_free(GlNativeGpuWork *work) {
    for (uint32_t i = 0u; i < work->chunk_count; ++i) {
        free(work->phase_chunks[i].commands);
        free(work->phase_chunks[i].data);
        native_gpu_geometry_free(work->phase_chunks[i].geometry);
    }
    work->chunk_count = work->published_chunk_count = 0u;
    free(work->commands); work->commands = NULL;
    free(work->data); work->data = NULL;
    free(work->canonical_pixels); work->canonical_pixels = NULL;
    free(work->canonical_provenance); work->canonical_provenance = NULL;
    if (work->template_parameters) {
        free(work->template_parameters->commands);
        free(work->template_parameters->visible);
        free(work->template_parameters);
        work->template_parameters = NULL;
    }
    native_gpu_geometry_free(work->geometry); work->geometry = NULL;
    if (work->owns_phase_textures) {
        free(work->phase_textures->words);
        free(work->phase_textures->layers);
        free((void *)work->phase_textures);
    }
    work->phase_textures = NULL;
    work->owns_phase_textures = 0;
    if (work->owns_phase_templates) {
        free(work->phase_templates->commands);
        free(work->phase_templates->data);
        free(work->phase_templates->record_commands);
        free(work->phase_templates->phase_vertices_needed);
        native_gpu_geometry_free(work->phase_templates->geometry);
        free((void *)work->phase_templates);
    }
    work->phase_templates = NULL;
    work->owns_phase_templates = 0;
    xg_render_retired_buffers_free(&work->retired_buffers);
}
GlNativeMotionProbe s_native_motion_probe;
GlNativeGpuWork *s_native_gpu_work;
uint8_t *s_native_gpu_readback_spare[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
size_t s_native_gpu_readback_capacity[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
uint32_t s_native_gpu_scale;
static GlNativeRasterLayout s_native_raster_layout = {.scale_x = 1.f, .scale_y = 1.f};
int s_native_gpu_canonical;
static GlNativeGpuCommand *native_gpu_command(GlNativeGpuWork *, GlNativeGpuOp);
static int native_gpu_data(GlNativeGpuWork *, const void *, size_t, uint32_t *);
static int native_gpu_seed(GlNativeGpuWork *, uint32_t, uint32_t, uint32_t, const uint32_t *, uint32_t *);
static int native_gpu_span(GlNativeGpuWork *, uint32_t, int, int, int, int, uint32_t, float, float, float, float, uint32_t, uint32_t);
static int native_gpu_pixel(GlNativeGpuWork *, uint32_t, int, int, uint32_t, uint32_t);
static volatile GlNativeRecipeDropEvent s_native_recipe_drop_events[32];

static volatile uint64_t s_native_recipe_drop_total;
static GlNativeRecipeDropEvent s_native_recipe_texture_read;
uint64_t s_native_pending_present_sequence;
int s_native_pending_present_valid;
GlRendererNativeCompilerDiagnostics s_native_compiler_diag;
GlRendererNativePipelineDiagnostics s_native_pipeline_diag;
static GlNativePendingGuestReference s_native_pending_guest_reference;
static GlNativeGuestReferenceSlot
    s_native_guest_reference_ring[GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY];
static uint64_t s_native_guest_reference_total;
static GlRendererNativeGuestReferenceEvent s_native_guest_reference_failures[
    GL_NATIVE_GUEST_REFERENCE_FAILURE_CAPACITY];
static uint64_t s_native_guest_reference_failure_sequences[
    GL_NATIVE_GUEST_REFERENCE_FAILURE_CAPACITY];
static uint64_t s_native_guest_reference_failure_total;
static int s_native_guest_reference_enabled = -1;
static GlNativeGpuPending s_native_gpu_pending;
static GlNativePrefetch s_native_prefetch[GL_NATIVE_PREFETCH_CAPACITY];
static uint32_t s_native_prefetch_count;

/* Logical scanout still feeds reference/history checks with GPU presentation.
 * Resolve row wrapping outside the flat loops so their conversions vectorize. */
static void native_scanout_row_15(uint32_t *output, const uint16_t *row,
        uint32_t x, uint32_t width) {
    const uint32_t first = width < VRAM_W - x ? width : VRAM_W - x;
    const uint16_t *input = row + x;
    for (uint32_t i = 0u; i < first; ++i)
        output[i] = conv_1555_to_rgba8(input[i]) | UINT32_C(0xff000000);
    output += first;
    for (uint32_t i = 0u; i < width - first; ++i)
        output[i] = conv_1555_to_rgba8(row[i]) | UINT32_C(0xff000000);
}

static void native_scanout_row_24(uint32_t *output, const uint16_t *row,
        uint32_t x, uint32_t width) {
    while (width >= 2u) {
        /* Two RGB pixels occupy three words, starting at an even byte address.
         * Only a pair straddling the VRAM edge needs individual wrapped reads. */
        if (x > VRAM_W - 3u) {
            const uint32_t a = row[x], b = row[(x + 1u) & (VRAM_W - 1u)];
            const uint32_t c = row[(x + 2u) & (VRAM_W - 1u)];
            output[0] = UINT32_C(0xff000000) | a | ((b & 255u) << 16u);
            output[1] = UINT32_C(0xff000000) | (b >> 8u) | (c << 8u);
            output += 2u; width -= 2u; x = (x + 3u) & (VRAM_W - 1u);
            continue;
        }
        const uint32_t available = (VRAM_W - x) / 3u;
        const uint32_t pairs = width / 2u < available ? width / 2u : available;
        const uint16_t *input = row + x;
        for (uint32_t i = 0u; i < pairs; ++i) {
            const uint32_t a = input[i * 3u], b = input[i * 3u + 1u], c = input[i * 3u + 2u];
            output[i * 2u] = UINT32_C(0xff000000) | a | ((b & 255u) << 16u);
            output[i * 2u + 1u] = UINT32_C(0xff000000) | (b >> 8u) | (c << 8u);
        }
        output += pairs * 2u; width -= pairs * 2u; x = (x + pairs * 3u) & (VRAM_W - 1u);
    }
    if (width) {
        const uint32_t a = row[x], b = row[(x + 1u) & (VRAM_W - 1u)];
        output[0] = UINT32_C(0xff000000) | a | ((b & 255u) << 16u);
    }
}

uint32_t native_next_generation(uint32_t generation) {
    generation++;
    return generation == 0u ? 1u : generation;
}

static int native_identity_equal(const XgPresentationIdentity *a,
                             const XgPresentationIdentity *b) {
    return a && b &&
        a->presentation_epoch == b->presentation_epoch &&
        a->source_sequence == b->source_sequence &&
        a->guest_vblank_sequence == b->guest_vblank_sequence &&
        a->guest_cycle == b->guest_cycle &&
        a->scene_generation == b->scene_generation;
}

void native_guest_reference_reset_locked(void) {
    free(s_native_pending_guest_reference.pixels);
    memset(&s_native_pending_guest_reference, 0,
           sizeof(s_native_pending_guest_reference));
    for (uint32_t index = 0u;
         index < GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY; ++index) {
        free(s_native_guest_reference_ring[index].pixels);
        memset(&s_native_guest_reference_ring[index], 0,
               sizeof(s_native_guest_reference_ring[index]));
    }
    s_native_guest_reference_total = 0u;
    memset(s_native_guest_reference_failures, 0,
           sizeof(s_native_guest_reference_failures));
    memset(s_native_guest_reference_failure_sequences, 0,
           sizeof(s_native_guest_reference_failure_sequences));
    s_native_guest_reference_failure_total = 0u;
}

int gl_renderer_native_guest_reference_enabled(void) {
    if (s_native_guest_reference_enabled < 0) {
        const char *value = getenv("PSX_NATIVE_VISUAL_TRUTH");
        s_native_guest_reference_enabled =
            value != NULL && value[0] == '1' ? 1 : 0;
    }
    return s_native_guest_reference_enabled;
}

int gl_renderer_native_capture_guest_reference(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle) {
    GpuDisplayInfo display = {0};
    GlNativePendingGuestReference captured = {0};
    const uint16_t *guest_vram;
    size_t pixel_count;

    if (!gl_renderer_native_guest_reference_enabled() || !s_native_active ||
        !s_native_state_mutex)
        return 0;
    gpu_get_display_info(&display);
    if (display.disabled || display.width == 0u || display.height == 0u) {
        SDL_LockMutex(s_native_state_mutex);
        free(s_native_pending_guest_reference.pixels);
        memset(&s_native_pending_guest_reference, 0,
               sizeof(s_native_pending_guest_reference));
        SDL_UnlockMutex(s_native_state_mutex);
        return 0;
    }
    if (display.width > 640u || display.height > 512u ||
        display.width > SIZE_MAX / display.height) {
        SDL_LockMutex(s_native_state_mutex);
        s_native_pipeline_diag.guest_reference_capture_attempts++;
        s_native_pipeline_diag.guest_reference_capture_failures++;
        free(s_native_pending_guest_reference.pixels);
        memset(&s_native_pending_guest_reference, 0,
               sizeof(s_native_pending_guest_reference));
        SDL_UnlockMutex(s_native_state_mutex);
        return 0;
    }
    pixel_count = (size_t)display.width * display.height;
    captured.pixels = (uint32_t *)malloc(pixel_count * sizeof(*captured.pixels));
    guest_vram = gpu_get_vram();
    if (!captured.pixels || !guest_vram) {
        free(captured.pixels);
        SDL_LockMutex(s_native_state_mutex);
        s_native_pipeline_diag.guest_reference_capture_attempts++;
        s_native_pipeline_diag.guest_reference_capture_failures++;
        free(s_native_pending_guest_reference.pixels);
        memset(&s_native_pending_guest_reference, 0,
               sizeof(s_native_pending_guest_reference));
        SDL_UnlockMutex(s_native_state_mutex);
        return 0;
    }
    for (uint32_t y = 0u; y < display.height; ++y) {
        const uint32_t vram_y = (display.display_y + y) & 511u;
        for (uint32_t x = 0u; x < display.width; ++x) {
            uint32_t pixel;
            if (display.depth24) {
                const uint32_t byte_x =
                    ((display.display_x & 1023u) * 2u) + x * 3u;
                uint8_t rgb[3] = {0u, 0u, 0u};
                if (byte_x + 2u < 2048u) {
                    for (uint32_t channel = 0u; channel < 3u; ++channel) {
                        const uint32_t offset = byte_x + channel;
                        const uint16_t word = guest_vram[
                            (size_t)vram_y * 1024u + (offset >> 1u)];
                        rgb[channel] = (offset & 1u)
                            ? (uint8_t)(word >> 8u) : (uint8_t)word;
                    }
                }
                pixel = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8u |
                    (uint32_t)rgb[2] << 16u;
            } else {
                const uint32_t vram_x = (display.display_x + x) & 1023u;
                pixel = conv_1555_to_rgba8(
                    guest_vram[(size_t)vram_y * 1024u + vram_x]);
            }
            captured.pixels[(size_t)y * display.width + x] = pixel;
        }
    }
    captured.guest_vblank_sequence = guest_vblank_sequence;
    captured.guest_cycle = guest_cycle;
    captured.pixel_digest = pres_hash_bytes(
        (const uint8_t *)captured.pixels,
        pixel_count * sizeof(*captured.pixels));
    captured.width = display.width;
    captured.height = display.height;
    captured.depth24 = display.depth24 ? 1u : 0u;
    captured.valid = 1u;

    SDL_LockMutex(s_native_state_mutex);
    s_native_pipeline_diag.guest_reference_capture_attempts++;
    s_native_pipeline_diag.guest_reference_captures++;
    free(s_native_pending_guest_reference.pixels);
    s_native_pending_guest_reference = captured;
    SDL_UnlockMutex(s_native_state_mutex);
    return 1;
}

static void native_guest_reference_bind_locked(
        const XgRenderSourceCommitHeader *header) {
    const uint64_t sequence = s_native_guest_reference_total++;
    GlNativeGuestReferenceSlot *slot = &s_native_guest_reference_ring[
        sequence % GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY];
    const int matches_pending = header != NULL &&
        s_native_pending_guest_reference.valid &&
        s_native_pending_guest_reference.guest_vblank_sequence ==
            header->identity.guest_vblank_sequence &&
        s_native_pending_guest_reference.guest_cycle == header->identity.guest_cycle;

    if (slot->pixels != NULL) {
        s_native_pipeline_diag.guest_reference_dropped_sources++;
        free(slot->pixels);
    }
    memset(slot, 0, sizeof(*slot));
    slot->sequence = sequence;
    if (header != NULL) {
        slot->event.identity = header->identity;
        slot->event.identity_valid = 1u;
    }
    if (matches_pending) {
        slot->pixels = s_native_pending_guest_reference.pixels;
        slot->event.reference_pixel_digest =
            s_native_pending_guest_reference.pixel_digest;
        slot->event.reference_width = s_native_pending_guest_reference.width;
        slot->event.reference_height = s_native_pending_guest_reference.height;
        slot->event.reference_depth24 =
            s_native_pending_guest_reference.depth24;
        slot->event.reference_valid = 1u;
        s_native_pending_guest_reference.pixels = NULL;
        s_native_pipeline_diag.guest_reference_bound_sources++;
    } else {
        slot->event.mismatch_mask =
            GL_NATIVE_GUEST_REFERENCE_MISMATCH_MISSING;
        s_native_pipeline_diag.guest_reference_missing_sources++;
    }
    free(s_native_pending_guest_reference.pixels);
    memset(&s_native_pending_guest_reference, 0,
           sizeof(s_native_pending_guest_reference));
}

static void native_guest_reference_compare(
        const GlNativeCompileAudit *audit,
        const uint32_t *endpoint_pixels, uint64_t endpoint_digest) {
    const XgRenderSourceCommitHeader *header = audit ? &audit->header : NULL;
    GlNativeGuestReferenceSlot *slot = NULL;
    GlRendererNativeGuestReferenceEvent result;
    uint32_t *reference_pixels = NULL;
    uint64_t sequence = 0u;

    if (!header || !endpoint_pixels || !s_native_state_mutex ||
        !gl_renderer_native_guest_reference_enabled())
        return;
    SDL_LockMutex(s_native_state_mutex);
    for (uint64_t cursor = s_native_guest_reference_total;
         cursor != 0u && s_native_guest_reference_total - cursor <
             GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY;) {
        GlNativeGuestReferenceSlot *candidate;
        cursor--;
        candidate = &s_native_guest_reference_ring[
            cursor % GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY];
        if (candidate->sequence == cursor &&
            candidate->event.identity_valid &&
            native_identity_equal(&candidate->event.identity, &header->identity)) {
            slot = candidate;
            sequence = cursor;
            result = candidate->event;
            reference_pixels = candidate->pixels;
            candidate->pixels = NULL;
            break;
        }
    }
    SDL_UnlockMutex(s_native_state_mutex);
    if (!slot) return;

    result.endpoint_valid = 1u;
    result.semantic_digest = header->digest;
    result.record_audit_digest = audit->record_digest;
    result.endpoint_pixel_digest = endpoint_digest;
    result.rendered_draw_pixels = audit->rendered_draw_pixels;
    result.endpoint_visible_pixels = audit->endpoint_visible_pixels;
    result.scene_module = (uint32_t)header->scene.module;
    result.authored_scene_id = header->scene.authored_scene_id;
    result.authored_submode = header->scene.authored_submode;
    result.source_interval_vblanks = header->source_interval_vblanks;
    result.source_display_x = header->display.display_x;
    result.source_display_y = header->display.display_y;
    result.endpoint_width = audit->endpoint_storage_width;
    result.endpoint_height = audit->endpoint_storage_height;
    result.endpoint_depth24 = header->display.depth24 ? 1u : 0u;
    result.consumed_records[0] = audit->consumed_passes;
    result.consumed_records[1] = audit->consumed_draws;
    result.consumed_records[2] = audit->consumed_resources;
    result.consumed_records[3] = audit->consumed_surface_edges;
    result.consumed_records[4] = audit->consumed_ui_nodes;
    result.consumed_records[5] = audit->consumed_ui_glyph_runs;
    result.rendered_records[0] = audit->rendered_passes;
    result.rendered_records[1] = audit->rendered_draws;
    result.rendered_records[2] = audit->rendered_surface_edges;
    result.rendered_records[3] = audit->rendered_ui_nodes;
    result.rendered_records[4] = audit->rendered_ui_glyphs;
    result.pass_operations[0] = audit->pass_loads;
    result.pass_operations[1] = audit->pass_clears;
    result.pass_operations[2] = audit->pass_discards;
    result.pass_operations[3] = audit->pass_stores;
    if (!result.reference_valid || !reference_pixels) {
        result.mismatch_mask |= GL_NATIVE_GUEST_REFERENCE_MISMATCH_MISSING;
    } else {
        if (result.reference_width != result.endpoint_width)
            result.mismatch_mask |= GL_NATIVE_GUEST_REFERENCE_MISMATCH_WIDTH;
        if (result.reference_height != result.endpoint_height)
            result.mismatch_mask |= GL_NATIVE_GUEST_REFERENCE_MISMATCH_HEIGHT;
        if (result.reference_depth24 != result.endpoint_depth24)
            result.mismatch_mask |= GL_NATIVE_GUEST_REFERENCE_MISMATCH_DEPTH;
        if (result.mismatch_mask == 0u &&
            result.reference_pixel_digest != endpoint_digest) {
            uint32_t min_x = UINT32_MAX, min_y = UINT32_MAX;
            uint32_t max_x = 0u, max_y = 0u;
            const size_t pixel_count =
                (size_t)result.reference_width * result.reference_height;
            for (size_t index = 0u; index < pixel_count; ++index) {
                uint32_t x, y;
                if (reference_pixels[index] == endpoint_pixels[index]) continue;
                x = (uint32_t)(index % result.reference_width);
                y = (uint32_t)(index / result.reference_width);
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (result.sample_count <
                        GL_NATIVE_GUEST_REFERENCE_SAMPLE_CAPACITY) {
                    const uint8_t sample = result.sample_count++;
                    result.sample_x[sample] = (uint16_t)x;
                    result.sample_y[sample] = (uint16_t)y;
                    result.reference_samples[sample] = reference_pixels[index];
                    result.endpoint_samples[sample] = endpoint_pixels[index];
                }
                result.mismatch_pixel_count++;
            }
            if (result.mismatch_pixel_count != 0u) {
                result.mismatch_mask |=
                    GL_NATIVE_GUEST_REFERENCE_MISMATCH_PIXELS;
                result.mismatch_bounds[0] = min_x;
                result.mismatch_bounds[1] = min_y;
                result.mismatch_bounds[2] = max_x;
                result.mismatch_bounds[3] = max_y;
            }
        }
    }
    free(reference_pixels);
    result.comparison_valid = 1u;
    result.matches_endpoint = result.mismatch_mask == 0u ? 1u : 0u;

    SDL_LockMutex(s_native_state_mutex);
    slot = &s_native_guest_reference_ring[
        sequence % GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY];
    if (slot->sequence == sequence && slot->event.identity_valid &&
        native_identity_equal(&slot->event.identity, &header->identity)) {
        slot->event = result;
        s_native_pipeline_diag.guest_reference_comparisons++;
        if (result.matches_endpoint)
            s_native_pipeline_diag.guest_reference_matches++;
        else {
            const uint64_t failure_index =
                s_native_guest_reference_failure_total++;
            s_native_pipeline_diag.guest_reference_mismatches++;
            if (failure_index < GL_NATIVE_GUEST_REFERENCE_FAILURE_CAPACITY) {
                s_native_guest_reference_failures[failure_index] = result;
                s_native_guest_reference_failure_sequences[failure_index] =
                    sequence;
            }
        }
        s_native_pipeline_diag.last_guest_reference_sequence = sequence;
        s_native_pipeline_diag.last_guest_reference_digest =
            result.reference_pixel_digest;
        s_native_pipeline_diag.last_guest_reference_endpoint_digest =
            result.endpoint_pixel_digest;
        s_native_pipeline_diag.last_guest_reference_mismatch_pixels =
            result.mismatch_pixel_count;
        s_native_pipeline_diag.last_guest_reference_identity = result.identity;
        s_native_pipeline_diag.last_guest_reference_mismatch_mask =
            result.mismatch_mask;
        s_native_pipeline_diag.last_guest_reference_comparison_valid = 1;
        s_native_pipeline_diag.last_guest_reference_match =
            result.matches_endpoint ? 1 : 0;
    }
    SDL_UnlockMutex(s_native_state_mutex);
}

uint64_t gl_renderer_native_guest_reference_total(void) {
    uint64_t total = 0u;
    if (!s_native_state_mutex) return 0u;
    SDL_LockMutex(s_native_state_mutex);
    total = s_native_guest_reference_total;
    SDL_UnlockMutex(s_native_state_mutex);
    return total;
}

int gl_renderer_native_guest_reference_get(
        uint64_t sequence, GlRendererNativeGuestReferenceEvent *out_event) {
    GlNativeGuestReferenceSlot *slot;
    int found = 0;
    if (!out_event || !s_native_state_mutex) return 0;
    SDL_LockMutex(s_native_state_mutex);
    slot = &s_native_guest_reference_ring[
        sequence % GL_NATIVE_GUEST_REFERENCE_RING_CAPACITY];
    if (sequence < s_native_guest_reference_total && slot->sequence == sequence) {
        *out_event = slot->event;
        found = 1;
    }
    SDL_UnlockMutex(s_native_state_mutex);
    return found;
}

uint64_t gl_renderer_native_guest_reference_failure_total(void) {
    uint64_t total = 0u;
    if (!s_native_state_mutex) return 0u;
    SDL_LockMutex(s_native_state_mutex);
    total = s_native_guest_reference_failure_total;
    SDL_UnlockMutex(s_native_state_mutex);
    return total;
}

int gl_renderer_native_guest_reference_failure_get(
        uint32_t index, uint64_t *out_sequence,
        GlRendererNativeGuestReferenceEvent *out_event) {
    int found = 0;
    if (!out_sequence || !out_event || !s_native_state_mutex) return 0;
    SDL_LockMutex(s_native_state_mutex);
    if (index < GL_NATIVE_GUEST_REFERENCE_FAILURE_CAPACITY &&
        index < s_native_guest_reference_failure_total) {
        *out_sequence = s_native_guest_reference_failure_sequences[index];
        *out_event = s_native_guest_reference_failures[index];
        found = 1;
    }
    SDL_UnlockMutex(s_native_state_mutex);
    return found;
}

static int native_header_equal(const XgRenderSourceCommitHeader *a,
                           const XgRenderSourceCommitHeader *b) {
    return a && b && native_identity_equal(&a->identity, &b->identity) &&
        a->digest == b->digest && a->pass_count == b->pass_count &&
        a->draw_count == b->draw_count &&
        a->resource_count == b->resource_count &&
        a->surface_edge_count == b->surface_edge_count &&
        a->ui_node_count == b->ui_node_count &&
        a->ui_glyph_run_count == b->ui_glyph_run_count &&
        a->ui_glyph_placement_count == b->ui_glyph_placement_count &&
        a->native_operation_count == b->native_operation_count &&
        a->motion_resource_count == b->motion_resource_count &&
        a->temporal_coverage_count == b->temporal_coverage_count &&
        a->temporal_publication_count == b->temporal_publication_count &&
        a->native_work == b->native_work &&
        a->display_boundary == b->display_boundary &&
        a->source_interval_vblanks == b->source_interval_vblanks &&
        a->state == b->state && a->discontinuity == b->discontinuity &&
        a->temporally_eligible == b->temporally_eligible &&
        a->scene.disc_id == b->scene.disc_id &&
        a->scene.executable_identity == b->scene.executable_identity &&
        a->scene.primary_overlay_identity ==
            b->scene.primary_overlay_identity &&
        a->scene.companion_set_identity == b->scene.companion_set_identity &&
        a->scene.authored_scene_id == b->scene.authored_scene_id &&
        a->scene.authored_submode == b->scene.authored_submode &&
        a->scene.module == b->scene.module &&
        a->display.width == b->display.width &&
        a->display.height == b->display.height &&
        a->display.display_x == b->display.display_x &&
        a->display.display_y == b->display.display_y &&
        a->display.aspect_num == b->display.aspect_num &&
        a->display.aspect_den == b->display.aspect_den &&
        a->display.depth24 == b->display.depth24 &&
        a->display.interlaced == b->display.interlaced &&
        a->display.disabled == b->display.disabled &&
        a->display.native_width == b->display.native_width &&
        a->display.native_height == b->display.native_height &&
        a->display.native_offset_x == b->display.native_offset_x &&
        a->display.temporal_hz == b->display.temporal_hz;
}

uint32_t native_endpoint_mismatch_mask(
        const XgRenderCompiledEndpoint *compiled,
        const GlNativeEndpointSlot *endpoint, uint32_t generation) {
    uint32_t mismatch = 0u;

    if (!compiled || !endpoint || !endpoint->in_use || endpoint->release_pending ||
        endpoint->generation != generation) {
        return XG_RENDER_ENDPOINT_MISMATCH_HANDLE;
    }
    if (compiled->opaque_handle == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_HANDLE;
    if (compiled->identity.presentation_epoch !=
            endpoint->identity.presentation_epoch)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_PRESENTATION_EPOCH;
    if (compiled->identity.source_sequence !=
            endpoint->identity.source_sequence)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_SOURCE_SEQUENCE;
    if (compiled->identity.guest_vblank_sequence !=
            endpoint->identity.guest_vblank_sequence)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_GUEST_VBLANK;
    if (compiled->identity.guest_cycle != endpoint->identity.guest_cycle)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_GUEST_CYCLE;
    if (compiled->identity.scene_generation !=
            endpoint->identity.scene_generation)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_SCENE_GENERATION;
    if (compiled->digest != endpoint->digest)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_DIGEST;
    if (compiled->backend_generation != endpoint->backend_generation)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_BACKEND_GENERATION;
    if (compiled->temporal_phase_count != endpoint->metadata.temporal_phase_count ||
        compiled->temporal_interval_ns != endpoint->metadata.temporal_interval_ns ||
        compiled->pixel_digest != endpoint->pixel_digest ||
        compiled->temporal_previous_pixel_digest != endpoint->metadata.temporal_previous_pixel_digest)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_CALLBACK;
    if (compiled->width != endpoint->metadata.width || compiled->width == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_WIDTH;
    if (compiled->height != endpoint->metadata.height ||
        compiled->height == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_HEIGHT;
    if (compiled->format != endpoint->metadata.format ||
        compiled->format != GL_RENDERER_NATIVE_ENDPOINT_FORMAT_RGBA8)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_FORMAT;
    return mismatch;
}

static uint64_t native_audit_bytes(uint64_t digest, const void *data, size_t size) {
    /* This hash is exported as diagnostic evidence only. Source identity,
     * resource generations and actual image digests have their own checks. */
    if (!s_native_record_audit_enabled) return 0u;
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i = 0u;
    /* The unused portion of fixed-capacity draw records is zero. Preserve the
     * bytewise FNV digest, folding eight zero multiplications into one. */
    for (; size - i >= sizeof(uint64_t); i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, bytes + i, sizeof(word));
        if (word == 0u) digest *= UINT64_C(0x1efac7090aef4a21);
        else for (size_t byte = 0u; byte < sizeof(word); ++byte) {
            digest ^= bytes[i + byte];
            digest *= UINT64_C(1099511628211);
        }
    }
    for (; i < size; ++i) {
        digest ^= bytes[i];
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

static void native_audit_block(GlNativeCompileAudit *audit, uint32_t blocker,
                           uint32_t record_index, uint64_t resource_id,
                           uint64_t resource_generation) {
    if (!audit || audit->blocker != GL_RENDERER_NATIVE_BLOCKER_NONE) return;
    audit->blocker = blocker;
    audit->blocker_record_index = record_index;
    audit->blocker_resource_id = resource_id;
    audit->blocker_resource_generation = resource_generation;
}

static void native_publish_compile_audit(const GlNativeCompileAudit *audit,
                                     XgRenderCompileResult result) {
    if (!audit || !s_native_state_mutex) return;
    SDL_LockMutex(s_native_state_mutex);
    if (result == XG_RENDER_COMPILE_ENDPOINT_READY) {
        s_native_compiler_diag.compiled_endpoints++;
        if (audit->header.native_work) {
            s_native_compiler_diag.temporal_status_counts[audit->temporal_status]++;
            s_native_compiler_diag.last_temporal_identity = audit->header.identity;
            s_native_compiler_diag.last_temporal_status = audit->temporal_status;
            s_native_compiler_diag.last_temporal_phase_count = audit->temporal_phase_count;
            s_native_compiler_diag.last_temporal_interval_vblanks = audit->temporal_interval_vblanks;
            s_native_compiler_diag.last_temporal_interval_ns = audit->temporal_interval_ns;
            s_native_compiler_diag.motion_evaluations += audit->motion_evaluations;
            s_native_compiler_diag.motion_projected_draws += audit->motion_projected_draws;
            s_native_compiler_diag.phase_gen_ns_total += audit->phase_gen_ns;
            s_native_compiler_diag.phase_gen_batches += audit->phase_gen_count > 0u ? 1u : 0u;
            s_native_compiler_diag.phase_gen_phases += audit->phase_gen_count;
            s_native_compiler_diag.recipe_canonical_match = audit->recipe_canonical_match;
            s_native_compiler_diag.recipe_view_match = audit->recipe_view_match;
            s_native_compiler_diag.recipe_validation_performed = audit->recipe_validation_performed;
        }
    } else if (result == XG_RENDER_COMPILE_FAILED) {
        s_native_compiler_diag.compile_failures++;
    } else if (result == XG_RENDER_COMPILE_WOULD_BLOCK) {
        s_native_compiler_diag.compile_would_block++;
    }
    if (audit->header.native_work &&
        (result == XG_RENDER_COMPILE_ENDPOINT_READY ||
         result == XG_RENDER_COMPILE_APPLIED)) {
        s_native_compiler_diag.applied_native_work++;
        s_native_compiler_diag.view_logical_draws += audit->rendered_view_draws;
        s_native_compiler_diag.view_physical_raster_passes += audit->view_raster_passes;
        for (unsigned i = 0u; i < 3u; ++i)
            s_native_compiler_diag.depth_draws[i] += audit->depth_draws[i];
        s_native_compiler_diag.depth_tested_fragments += audit->depth_tested_fragments;
        s_native_compiler_diag.depth_rejected_fragments += audit->depth_rejected_fragments;
        s_native_compiler_diag.depth_written_fragments += audit->depth_written_fragments;
        s_native_compiler_diag.depth_reset_fragments += audit->depth_reset_fragments;
        if (audit->depth_key_min && audit->depth_key_max >= audit->depth_key_min) {
            const int lo = (int)(log2((double)native_depth_decode(audit->depth_key_min)) * 256.0);
            const int hi = (int)(log2((double)native_depth_decode(audit->depth_key_max)) * 256.0);
            if (!s_native_depth_view_log_max) {
                s_native_depth_view_log_min = lo; s_native_depth_view_log_max = hi;
            } else {
                s_native_depth_view_log_min += (lo - s_native_depth_view_log_min) / 4;
                s_native_depth_view_log_max += (hi - s_native_depth_view_log_max) / 4;
            }
        }
    }
    if (audit->header.native_work)
        s_native_compiler_diag.native_depth_test = audit->header.display.native_depth_test;
    s_native_compiler_diag.consumed_native_operations =
        audit->consumed_native_operations;
    s_native_compiler_diag.consumed_motion_resources = audit->consumed_motion_resources;
    if (result == XG_RENDER_COMPILE_APPLIED && audit->header.display_boundary) {
        s_native_compiler_diag.duplicate_endpoints++;
        s_native_compiler_diag.temporal_status_counts[GL_RENDERER_NATIVE_TEMPORAL_DUPLICATE]++;
    }
    s_native_compiler_diag.rendered_view_draws = audit->rendered_view_draws;
    s_native_compiler_diag.marked_view_draws = audit->marked_view_draws;
    s_native_compiler_diag.view_copies = audit->view_copies;
    s_native_compiler_diag.view_wave_rows = audit->view_wave_rows;
    s_native_compiler_diag.view_wave_incomplete = audit->view_wave_incomplete;
    s_native_compiler_diag.last_storage_width = audit->endpoint_storage_width;
    s_native_compiler_diag.last_storage_height = audit->endpoint_storage_height;
    s_native_compiler_diag.native_vram_identity = s_native_native_vram_identity;
    s_native_compiler_diag.view_target_count = s_native_views.count;
    s_native_compiler_diag.view_domain_count = s_native_views.domain_count;
    s_native_compiler_diag.active_view_target = s_native_views.active;
    s_native_compiler_diag.last_commit_digest = audit->header.digest;
    s_native_compiler_diag.last_record_audit_digest = audit->record_digest;
    s_native_compiler_diag.blocker_resource_id = audit->blocker_resource_id;
    s_native_compiler_diag.blocker_resource_generation =
        audit->blocker_resource_generation;
    s_native_compiler_diag.rendered_draw_pixels = audit->rendered_draw_pixels;
    s_native_compiler_diag.endpoint_visible_pixels = audit->endpoint_visible_pixels;
    s_native_compiler_diag.last_endpoint_pixel_digest =
        audit->endpoint_pixel_digest;
    s_native_compiler_diag.last_compile_identity = audit->header.identity;
    s_native_compiler_diag.last_endpoint_pixel_digest_valid =
        result == XG_RENDER_COMPILE_ENDPOINT_READY;
    s_native_compiler_diag.last_blocker = audit->blocker;
    s_native_compiler_diag.blocker_record_index = audit->blocker_record_index;
    s_native_compiler_diag.consumed_passes = audit->consumed_passes;
    s_native_compiler_diag.consumed_draws = audit->consumed_draws;
    s_native_compiler_diag.consumed_resources = audit->consumed_resources;
    s_native_compiler_diag.consumed_surface_edges =
        audit->consumed_surface_edges;
    s_native_compiler_diag.consumed_ui_nodes = audit->consumed_ui_nodes;
    s_native_compiler_diag.consumed_ui_glyph_runs =
        audit->consumed_ui_glyph_runs;
    s_native_compiler_diag.consumed_ui_glyph_placements =
        audit->consumed_ui_glyph_placements;
    s_native_compiler_diag.rendered_passes = audit->rendered_passes;
    s_native_compiler_diag.rendered_draws = audit->rendered_draws;
    s_native_compiler_diag.rendered_surface_edges =
        audit->rendered_surface_edges;
    s_native_compiler_diag.rendered_ui_nodes = audit->rendered_ui_nodes;
    s_native_compiler_diag.rendered_ui_glyphs = audit->rendered_ui_glyphs;
    s_native_compiler_diag.pass_loads = audit->pass_loads;
    s_native_compiler_diag.pass_clears = audit->pass_clears;
    s_native_compiler_diag.pass_discards = audit->pass_discards;
    s_native_compiler_diag.pass_stores = audit->pass_stores;
    s_native_compiler_diag.all_records_consumed = audit->all_records_consumed;
    s_native_compiler_diag.last_endpoint_was_movie = audit->endpoint_was_movie;
    s_native_compiler_diag.last_endpoint_was_depth24 =
        audit->endpoint_was_depth24;
    s_native_compiler_diag.last_endpoint_was_discrete =
        audit->endpoint_was_discrete;
    s_native_compiler_diag.last_endpoint_display_x =
        audit->header.display.display_x;
    s_native_compiler_diag.last_endpoint_display_y =
        audit->header.display.display_y;
    s_native_compiler_diag.last_endpoint_width = audit->header.display.width;
    s_native_compiler_diag.last_endpoint_height = audit->header.display.height;
    s_native_compiler_diag.last_endpoint_first_edge_kind =
        audit->consumed_surface_edges != 0u
            ? (uint32_t)audit->edges[0].kind : UINT32_MAX;
    s_native_pipeline_diag.last_identity = audit->header.identity;
    s_native_pipeline_diag.last_commit_digest = audit->header.digest;
    s_native_pipeline_diag.last_record_audit_digest = audit->record_digest;
    s_native_pipeline_diag.last_endpoint_pixel_digest =
        audit->endpoint_pixel_digest;
    SDL_UnlockMutex(s_native_state_mutex);
}

void native_endpoint_maybe_recycle_locked(uint32_t slot,
                                              uint32_t generation) {
    GlNativeEndpointSlot *endpoint;
    if (slot >= GL_NATIVE_ENDPOINT_CAPACITY) return;
    endpoint = &s_native_endpoints[slot];
    if (!endpoint->in_use || endpoint->generation != generation ||
        !endpoint->release_pending || endpoint->fence_refs != 0u ||
        endpoint->presenter_busy)
        return;
    free(endpoint->staged_pixels);
    endpoint->staged_pixels = NULL;
    for (uint32_t i = 0u; i < GL_NATIVE_MOTION_PHASE_CAPACITY; ++i) {
        free(endpoint->phases[i].pixels);
        endpoint->phases[i].pixels = NULL;
        endpoint->phases[i].digest = 0u;
        endpoint->phases[i].upload_state = GL_NATIVE_ENDPOINT_UPLOAD_NONE;
    }
    endpoint->in_use = 0;
    endpoint->release_pending = 0;
    endpoint->presenter_busy = 0;
    endpoint->upload_state = GL_NATIVE_ENDPOINT_UPLOAD_NONE;
    memset(&endpoint->identity, 0, sizeof(endpoint->identity));
    memset(&endpoint->metadata, 0, sizeof(endpoint->metadata));
    endpoint->digest = 0u;
    endpoint->record_audit_digest = 0u;
    endpoint->pixel_digest = 0u;
    endpoint->backend_generation = 0u;
}

int gl_renderer_native_endpoint_metadata(
        uint64_t opaque_handle,
        GlRendererNativeEndpointMetadata *out_metadata) {
    uint32_t slot, generation;
    int ok = 0;
    if (!out_metadata || !s_native_state_mutex ||
        !native_unpack_handle(opaque_handle, GL_NATIVE_ENDPOINT_CAPACITY,
                          &slot, &generation))
        return 0;
    SDL_LockMutex(s_native_state_mutex);
    if (s_native_endpoints[slot].in_use &&
        s_native_endpoints[slot].generation == generation) {
        *out_metadata = s_native_endpoints[slot].metadata;
        ok = 1;
    }
    SDL_UnlockMutex(s_native_state_mutex);
    return ok;
}

void gl_renderer_native_compiler_diagnostics(
        GlRendererNativeCompilerDiagnostics *out_diagnostics) {
    if (!out_diagnostics) return;
    memset(out_diagnostics, 0, sizeof(*out_diagnostics));
    if (!s_native_state_mutex) return;
    SDL_LockMutex(s_native_state_mutex);
    native_publish_legacy_owner_locked();
    *out_diagnostics = s_native_compiler_diag;
    out_diagnostics->gpu.pending_state = s_native_gpu_work ? (uint32_t)s_native_gpu_work->state : 0u;
    SDL_UnlockMutex(s_native_state_mutex);
}

void gl_renderer_native_pipeline_diagnostics(
        GlRendererNativePipelineDiagnostics *out_diagnostics) {
    if (!out_diagnostics) return;
    memset(out_diagnostics, 0, sizeof(*out_diagnostics));
    if (!s_native_state_mutex) return;
    SDL_LockMutex(s_native_state_mutex);
    *out_diagnostics = s_native_pipeline_diag;
    out_diagnostics->pending_present = s_native_pending_present_valid;
    SDL_UnlockMutex(s_native_state_mutex);
}

const char *gl_renderer_native_present_blocker_name(uint32_t blocker) {
    switch ((GlRendererNativePresentBlocker)blocker) {
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_NONE: return "none";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_INVALID_ARGUMENT:
        return "invalid presenter argument";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_ENDPOINT_MISMATCH:
        return "compiled endpoint no longer matches retained endpoint";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_STAGING_MISSING:
        return "endpoint staging pixels missing";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_CONTEXT:
        return "presenter GL context unavailable";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_UPLOAD:
        return "endpoint texture upload failed";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_DRAWABLE:
        return "window drawable has invalid dimensions";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW:
        return "presenter draw or GL completion failed";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_FENCE:
        return "presenter completion fence unavailable";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_SWAP_CONTEXT:
        return "swap GL context unavailable";
    case GL_RENDERER_NATIVE_PRESENT_BLOCKER_SWAP_REJECTED:
        return "swap ownership or SDL swap failed";
    }
    return "unknown Native presenter blocker";
}

const char *gl_renderer_native_temporal_status_name(uint32_t status) {
    static const char *const names[GL_RENDERER_NATIVE_TEMPORAL_STATUS_COUNT] = {
        "not_applicable", "source_state_required", "discrete_display",
        "no_recipe", "replay_failed", "replay_mismatch",
        "ready", "duplicate", "no_history", "binding_incomplete", "pose_incompatible",
        "clip_required", "projection_invalid", "allocation", "static_pose",
        "smoothing_disabled", "source_cadence",
        "deadline_expired", "whole_only",
    };
    return status < GL_RENDERER_NATIVE_TEMPORAL_STATUS_COUNT ? names[status] : "unknown";
}

const char *gl_renderer_native_compile_blocker_name(uint32_t blocker) {
    switch ((GlRendererNativeCompileBlocker)blocker) {
    case GL_RENDERER_NATIVE_BLOCKER_NONE: return "none";
    case GL_RENDERER_NATIVE_BLOCKER_HEADER_COPY: return "SourceCommit header copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_HEADER_MISMATCH: return "sealed SourceCommit header mismatch";
    case GL_RENDERER_NATIVE_BLOCKER_INVALID_DISPLAY: return "invalid semantic display descriptor";
    case GL_RENDERER_NATIVE_BLOCKER_PASS_COPY: return "SourceCommit pass copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_DRAW_COPY: return "SourceCommit draw copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_RESOURCE_COPY: return "SourceCommit resource copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_RESOURCE_VIEW: return "retained immutable resource view unavailable";
    case GL_RENDERER_NATIVE_BLOCKER_RESOURCE_DIGEST: return "retained resource digest mismatch";
    case GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_COPY: return "SourceCommit surface-edge copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_UI_NODE_COPY: return "SourceCommit UI-node copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_UI_GLYPH_RUN_COPY: return "SourceCommit UI-glyph-run copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_UI_GLYPH_PLACEMENT_COPY: return "SourceCommit UI-glyph-placement copy failed";
    case GL_RENDERER_NATIVE_BLOCKER_TARGET_SURFACE_DESCRIPTOR:
        return "target resource ref/view lacks immutable width, height, row pitch and pixel format";
    case GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR:
        return "texture resource ref/view lacks immutable width, height, row pitch, pixel format and VRAM origin";
    case GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR:
        return "UI resource ref/view lacks immutable atlas/image layout and pixel format";
    case GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_DESCRIPTOR:
        return "surface edge lacks destination rectangle and immutable source/target layouts";
    case GL_RENDERER_NATIVE_BLOCKER_MOVIE_LAYOUT:
        return "Movie resource bytes do not match the full-frame edge/display depth layout";
    case GL_RENDERER_NATIVE_BLOCKER_ENDPOINT_CAPACITY: return "bounded endpoint slots exhausted";
    /* Enum names are service ABI; descriptions reflect the CPU-only worker. */
    case GL_RENDERER_NATIVE_BLOCKER_WORKER_CONTEXT: return "main-thread presenter GL context unavailable";
    case GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE: return "software staging or main-thread endpoint upload failed";
    case GL_RENDERER_NATIVE_BLOCKER_FENCE_CAPACITY: return "bounded fence slots exhausted";
    case GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD:
        return "semantic record is invalid or references absent immutable data";
    case GL_RENDERER_NATIVE_BLOCKER_PASS_DEPENDENCY:
        return "semantic pass dependency is absent or not yet complete";
    case GL_RENDERER_NATIVE_BLOCKER_UNSUPPORTED_UI_NODE:
        return "UI node requires semantics not present in its immutable record";
    case GL_RENDERER_NATIVE_BLOCKER_NO_STORED_DISPLAY_SURFACE:
        return "no stored pass target contains the semantic display rectangle";
    case GL_RENDERER_NATIVE_BLOCKER_SURFACE_HISTORY:
        return "surface history is stale, discarded or incompatible with the resource";
    case GL_RENDERER_NATIVE_BLOCKER_NATIVE_OPERATION:
        return "native VRAM operation is invalid, unavailable or out of order";
    }
    return "unknown Native semantic compiler blocker";
}

int gl_renderer_native_capture_source(
        XgRenderSourceCommitHandle commit,
        const XgRenderSourceCommitHeader *sealed_header) {
    XgRenderSourceCommitHeader copied;
    int accepted = 0;

    if (!s_native_active || !s_native_state_mutex || !sealed_header ||
        sealed_header->state != XG_RENDER_SOURCE_SEALED)
        goto finished;
    if (xg_render_source_commit_header_copy(commit, &copied) !=
            XG_RENDER_SOURCE_COMMIT_OK ||
        !native_header_equal(&copied, sealed_header))
        goto finished;
    accepted = 1;

finished:
    if (s_native_state_mutex) {
        SDL_LockMutex(s_native_state_mutex);
        s_native_pipeline_diag.capture_attempts++;
        if (accepted) {
            s_native_compiler_diag.sealed_captures++;
            s_native_compiler_diag.last_capture_identity = copied.identity;
            s_native_pipeline_diag.capture_successes++;
            s_native_pipeline_diag.last_identity = copied.identity;
            s_native_pipeline_diag.last_commit_digest = copied.digest;
            s_native_pipeline_diag.last_present_blocker =
                GL_RENDERER_NATIVE_PRESENT_BLOCKER_NONE;
            if (gl_renderer_native_guest_reference_enabled() &&
                (!copied.native_work || copied.display_boundary))
                native_guest_reference_bind_locked(&copied);
        } else {
            s_native_compiler_diag.sealed_capture_rejections++;
            s_native_compiler_diag.last_blocker =
                GL_RENDERER_NATIVE_BLOCKER_HEADER_MISMATCH;
            s_native_pipeline_diag.capture_failures++;
        }
        SDL_UnlockMutex(s_native_state_mutex);
    }
    return accepted;
}

static GlNativeResourceRecord *native_audit_resource(GlNativeCompileAudit *audit,
                                             uint64_t resource_id,
                                             uint64_t generation) {
    for (uint32_t i = 0u; i < audit->consumed_resources; ++i) {
        GlNativeResourceRecord *resource = &audit->resources[i];
        if (resource->reference.resource_id == resource_id &&
            resource->reference.generation == generation)
            return resource;
    }
    return NULL;
}

static int native_consume_source_commit(XgRenderSourceCommitHandle commit,
                                    GlNativeCompileAudit *audit) {
    XgRenderSourceCommitResult result;

    memset(audit, 0, offsetof(GlNativeCompileAudit, passes));
    audit->record_digest = s_native_record_audit_enabled ? UINT64_C(1469598103934665603) : 0u;
    result = xg_render_source_commit_header_copy(commit, &audit->header);
    if (result != XG_RENDER_SOURCE_COMMIT_OK) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_HEADER_COPY, 0u, 0u, 0u);
        return 0;
    }
    audit->endpoint_storage_width = audit->header.display.width;
    audit->endpoint_storage_height = audit->header.display.height;
    audit->record_digest = native_audit_bytes(
        audit->record_digest, &audit->header.identity,
        sizeof(audit->header.identity));
    audit->record_digest = native_audit_bytes(
        audit->record_digest, &audit->header.digest,
        sizeof(audit->header.digest));
    if (audit->header.pass_count > XG_RENDER_SCENE_PASS_CAPACITY ||
        audit->header.native_operation_count > XG_RENDER_NATIVE_OPERATION_CAPACITY ||
        audit->header.motion_resource_count > XG_RENDER_SCENE_RESOURCE_CAPACITY ||
        audit->header.temporal_coverage_count > XG_RENDER_SCENE_RESOURCE_CAPACITY ||
        audit->header.temporal_publication_count > XG_RENDER_SCENE_RESOURCE_CAPACITY ||
        audit->header.draw_count > XG_RENDER_SCENE_DRAW_CAPACITY ||
        audit->header.resource_count > XG_RENDER_SCENE_RESOURCE_CAPACITY ||
        audit->header.surface_edge_count >
            XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY ||
        audit->header.ui_node_count > XG_RENDER_SCENE_UI_NODE_CAPACITY ||
        audit->header.ui_glyph_run_count >
            XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY ||
        audit->header.ui_glyph_placement_count >
            XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                       0u, 0u, 0u);
        return 0;
    }
    if ((audit->header.native_work &&
         (audit->header.pass_count || audit->header.draw_count ||
          audit->header.surface_edge_count || audit->header.ui_node_count ||
          audit->header.ui_glyph_run_count || audit->header.ui_glyph_placement_count)) ||
        (!audit->header.native_work && (audit->header.native_operation_count || audit->header.motion_resource_count ||
                                      audit->header.temporal_coverage_count || audit->header.temporal_publication_count))) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_NATIVE_OPERATION, 0u, 0u, 0u);
        return 0;
    }

    for (uint32_t i = 0u; i < audit->header.motion_resource_count; ++i) {
        XgRenderMotionRef ref;
        const XgRenderMotionPose *pose;
        if (xg_render_source_commit_motion_resource_copy(commit, i, &ref) != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_COPY, i, 0u, 0u);
            return 0;
        }
        if (!xg_render_motion_view(ref, &pose) ||
            pose->presentation_epoch != audit->header.identity.presentation_epoch ||
            pose->scene_generation != audit->header.identity.scene_generation) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_VIEW, i, ref.handle.resource_id, ref.handle.generation);
            return 0;
        }
        audit->motion_resources[audit->consumed_motion_resources++] = ref;
        audit->record_digest = native_audit_bytes(audit->record_digest, &ref, sizeof(ref));
    }
    for (uint32_t i = 0u; i < audit->header.temporal_coverage_count; ++i) {
        XgSemanticResourceRef ref;
        XgRenderTemporalCoverageView view;
        if (xg_render_source_commit_temporal_coverage_copy(commit, i, &ref) != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_COPY, i, 0u, 0u); return 0;
        }
        if (!xg_render_temporal_coverage_view(ref, &view) ||
            view.header->identity.presentation_epoch != audit->header.identity.presentation_epoch ||
            view.header->identity.scene_generation != audit->header.identity.scene_generation) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_VIEW, i, ref.resource_id, ref.generation); return 0;
        }
        audit->temporal_coverages[audit->consumed_temporal_coverages++] = ref;
        audit->record_digest = native_audit_bytes(audit->record_digest, &ref, sizeof(ref));
    }
    for (uint32_t i = 0u; i < audit->header.temporal_publication_count; ++i) {
        XgRenderTemporalPublication publication;
        if (xg_render_source_commit_temporal_publication_copy(commit, i, &publication) != XG_RENDER_SOURCE_COMMIT_OK ||
            publication.before_operation > audit->header.native_operation_count ||
            (i && publication.before_operation < audit->temporal_publications[i - 1u].before_operation)) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_COPY, i, 0u, 0u); return 0;
        }
        int retained = 0;
        for (uint32_t j = 0u; j < audit->consumed_temporal_coverages; ++j)
            retained |= !memcmp(&publication.coverage, &audit->temporal_coverages[j], sizeof(publication.coverage));
        if (!retained) { native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD, i, 0u, 0u); return 0; }
        audit->temporal_publications[audit->consumed_temporal_publications++] = publication;
        audit->record_digest = native_audit_bytes(audit->record_digest, &publication, sizeof(publication));
    }
    for (uint32_t i = 0u; i < audit->header.pass_count; ++i) {
        XgSemanticPassRecord pass;
        result = xg_render_source_commit_pass_copy(commit, i, &pass);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_PASS_COPY, i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &pass, sizeof(pass));
        if (audit->consumed_passes < XG_RENDER_SCENE_PASS_CAPACITY)
            audit->passes[audit->consumed_passes] = pass;
        audit->consumed_passes++;
    }
    for (uint32_t i = 0u; i < audit->header.draw_count; ++i) {
        XgSemanticDrawRecord draw;
        result = xg_render_source_commit_draw_copy(commit, i, &draw);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_DRAW_COPY, i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &draw, sizeof(draw));
        audit->saw_textured_draw |= draw.primitive.material.textured ? 1 : 0;
        if (audit->consumed_draws < XG_RENDER_SCENE_DRAW_CAPACITY)
            audit->draws[audit->consumed_draws] = draw;
        audit->consumed_draws++;
    }
    for (uint32_t i = 0u; i < audit->header.resource_count; ++i) {
        GlNativeResourceRecord resource;
        memset(&resource, 0, sizeof(resource));
        result = xg_render_source_commit_resource_copy(
            commit, i, &resource.reference);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_COPY,
                           i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &resource.reference,
            sizeof(resource.reference));
        if (xg_render_resource_view((XgRenderResourceHandle){
                resource.reference.resource_id,
                resource.reference.generation }, &resource.view) !=
                XG_RENDER_RESOURCE_OK || resource.view.bytes == NULL ||
            resource.view.byte_count == 0u) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_VIEW, i,
                           resource.reference.resource_id,
                           resource.reference.generation);
        } else if (resource.view.content_digest !=
                       resource.reference.content_digest) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_DIGEST, i,
                           resource.reference.resource_id,
                           resource.reference.generation);
        } else {
            resource.view_valid = 1;
        }
        if (audit->consumed_resources < XG_RENDER_SCENE_RESOURCE_CAPACITY)
            audit->resources[audit->consumed_resources] = resource;
        audit->consumed_resources++;
    }
    for (uint32_t i = 0u; i < audit->header.surface_edge_count; ++i) {
        XgSemanticSurfaceEdge edge;
        result = xg_render_source_commit_surface_edge_copy(commit, i, &edge);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_COPY,
                           i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &edge, sizeof(edge));
        if (audit->consumed_surface_edges <
                XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY)
            audit->edges[audit->consumed_surface_edges] = edge;
        audit->consumed_surface_edges++;
    }
    for (uint32_t i = 0u; i < audit->header.ui_node_count; ++i) {
        XgSemanticUiNodeRecord node;
        result = xg_render_source_commit_ui_node_copy(commit, i, &node);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_UI_NODE_COPY,
                           i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &node, sizeof(node));
        if (audit->consumed_ui_nodes < XG_RENDER_SCENE_UI_NODE_CAPACITY)
            audit->ui_nodes[audit->consumed_ui_nodes] = node;
        audit->consumed_ui_nodes++;
    }
    for (uint32_t i = 0u; i < audit->header.ui_glyph_run_count; ++i) {
        XgSemanticUiGlyphRunRecord run;
        result = xg_render_source_commit_ui_glyph_run_copy(commit, i, &run);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_UI_GLYPH_RUN_COPY,
                           i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &run, sizeof(run));
        if (audit->consumed_ui_glyph_runs <
                XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY)
            audit->ui_runs[audit->consumed_ui_glyph_runs] = run;
        audit->consumed_ui_glyph_runs++;
    }
    for (uint32_t i = 0u; i < audit->header.ui_glyph_placement_count; ++i) {
        XgSemanticUiGlyphPlacementRecord placement;
        result = xg_render_source_commit_ui_glyph_placement_copy(
            commit, i, &placement);
        if (result != XG_RENDER_SOURCE_COMMIT_OK) {
            native_audit_block(
                audit, GL_RENDERER_NATIVE_BLOCKER_UI_GLYPH_PLACEMENT_COPY,
                i, 0u, 0u);
            continue;
        }
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &placement, sizeof(placement));
        if (audit->consumed_ui_glyph_placements <
                XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY)
            audit->ui_placements[audit->consumed_ui_glyph_placements] =
                placement;
        audit->consumed_ui_glyph_placements++;
    }
    audit->all_records_consumed =
        audit->consumed_passes == audit->header.pass_count &&
        audit->consumed_draws == audit->header.draw_count &&
        audit->consumed_resources == audit->header.resource_count &&
        audit->consumed_motion_resources == audit->header.motion_resource_count &&
        audit->consumed_temporal_coverages == audit->header.temporal_coverage_count &&
        audit->consumed_temporal_publications == audit->header.temporal_publication_count &&
        audit->consumed_surface_edges == audit->header.surface_edge_count &&
        audit->consumed_ui_nodes == audit->header.ui_node_count &&
        audit->consumed_ui_glyph_runs == audit->header.ui_glyph_run_count &&
        audit->consumed_ui_glyph_placements ==
            audit->header.ui_glyph_placement_count;
    return audit->all_records_consumed;
}

typedef struct GlNativeCpuSurface {
    GlNativeResourceRecord *resource;
    uint32_t *pixels;
    /* Writable only on the transaction-private Native canvas, never VIEW or
     * replay targets. Kept in lockstep with pixels at actual fragment writes. */
    uint16_t *words;
    /* Optional Native depth plane (FP32 reciprocal Z bits, 0 = far), indexed like
     * pixels. Present only on VIEW/replay presentation targets with the option. */
    uint32_t *depth;
    uint32_t width;
    uint32_t height;
    int valid;
    int stored;
} GlNativeCpuSurface;

typedef struct GlNativePersistentSurface {
    uint64_t resource_id;
    uint64_t generation;
    uint64_t seed_digest;
    uint64_t use_sequence;
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    int valid;
} GlNativePersistentSurface;

static GlNativePersistentSurface
    s_native_persistent_surfaces[XG_RENDER_SCENE_RESOURCE_CAPACITY];
/* Worker-owned history, published only with a successfully compiled endpoint.
 * This is carry state, not a replacement for skipped source operations. */
static XgPresentationIdentity s_native_persistent_identity;

static int native_persistent_scope_matches(const XgPresentationIdentity *identity) {
    return s_native_persistent_identity.source_sequence != 0u &&
        identity->presentation_epoch ==
            s_native_persistent_identity.presentation_epoch &&
        identity->scene_generation == s_native_persistent_identity.scene_generation;
}

static GlNativePersistentSurface *native_persistent_surface_find(
    uint64_t resource_id, uint64_t generation);

typedef struct GlNativeCpuCompiler {
    GlNativeCompileAudit *audit;
    GlNativeCpuSurface *native_vram;
    const uint16_t *native_words; /* Live Native canvas or immutable recipe texels. */
    GlNativeResourceRecord *native_texture;
    GlNativeResourceRecord *native_clut;
    int view_pass;
    GpuVramRegionSet *raster_words;
    uint8_t dither_x, dither_y;
    bool dithering_disabled;
    GlNativeCpuSurface surfaces[XG_RENDER_SCENE_RESOURCE_CAPACITY];
    GlNativeCpuSurface *last_display_surface;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    GlNativeGpuWork *gpu;
    uint32_t gpu_plane;
    int gpu_only;
    int canonical_dirty;
    int canonical_left, canonical_top, canonical_right, canonical_bottom;
    /* Row band of a parallel VIEW reference raster (raster Y, inclusive). */
    int banded, band_top, band_bottom;
    struct GlNativeViewWorker *view_worker;
    GlNativeViewState *native_views;
    /* Mesh mode: the template draw carries no triangles; native_view_raster
     * emits these (an XgRenderIrTriangle at mesh_base + i * mesh_stride,
     * with the GlNativeTextureState at mesh_textures + i * mesh_stride, or
     * the template's when mesh_textures is NULL), GPU only, as one mesh
     * command. */
    const uint8_t *mesh_base;
    const uint8_t *mesh_textures;
    /* Optional uint32_t flags at the same stride: PHASE_ONLY triangles are
     * left out of this (endpoint) draw. */
    const uint8_t *mesh_flags;
    size_t mesh_stride;
    uint32_t mesh_count;
    uint32_t texture_snapshot;
    /* Optional: the mesh is this caller's scratch, relocated in place. */
    GlNativeGpuMeshTriangle *mesh_writable;
} GlNativeCpuCompiler;
static int native_gpu_canonical_sync(GlNativeCpuCompiler *);
static int native_gpu_draw(GlNativeCpuCompiler *, const XgSemanticDrawRecord *, int);
static int native_gpu_draw_mesh(GlNativeCpuCompiler *, const XgSemanticDrawRecord *, int,
                                const GlNativeGpuMeshTriangle *, uint32_t);

/* Per-thread scratch triangles for mesh relocation (compile owner and phase
 * workers each have their own). */
static GlNativeGpuMeshTriangle *native_mesh_buffer(unsigned which, uint32_t count) {
    static _Thread_local GlNativeGpuMeshTriangle *buffers[2];
    static _Thread_local uint32_t capacities[2];
    if (which > 1u) return NULL;
    if (count > capacities[which]) {
        uint32_t capacity = capacities[which] ? capacities[which] : 1024u;
        while (capacity < count) capacity *= 2u;
        GlNativeGpuMeshTriangle *grown = realloc(buffers[which], (size_t)capacity * sizeof(*grown));
        if (!grown) return NULL;
        buffers[which] = grown;
        capacities[which] = capacity;
    }
    return buffers[which];
}
static int native_transition_texture(const GlNativeViewState *, const XgSemanticDrawRecord *);
static int native_view_worker_drain(GlNativeCpuCompiler *, int, int, int, int);

typedef enum GlNativeOrderedKind {
    GL_NATIVE_ORDERED_EDGE = 0,
    GL_NATIVE_ORDERED_DRAW,
    GL_NATIVE_ORDERED_UI_NODE,
    GL_NATIVE_ORDERED_UI_RUN,
} GlNativeOrderedKind;

typedef struct GlNativeOrderedRecord {
    XgSemanticOrderKey order;
    uint32_t index;
    GlNativeOrderedKind kind;
} GlNativeOrderedRecord;

static size_t native_descriptor_min_row_bytes(
        const XgRenderResourceDescriptor *descriptor) {
    switch (descriptor->pixel_format) {
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8:
        return (size_t)descriptor->width * 4u;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24:
        return (size_t)descriptor->width * 3u;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555:
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555:
        return (size_t)descriptor->width * 2u;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4:
        return ((size_t)descriptor->width + 1u) / 2u;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8:
        return descriptor->width;
    default:
        return 0u;
    }
}

static int native_pixel_descriptor_valid(const GlNativeResourceRecord *resource,
                                     int require_vram) {
    const XgRenderResourceDescriptor *descriptor;
    size_t minimum_row;
    if (!resource || !resource->view_valid) return 0;
    descriptor = &resource->view.descriptor;
    minimum_row = native_descriptor_min_row_bytes(descriptor);
    if (!xg_render_resource_descriptor_validate(descriptor) ||
        descriptor->version != XG_RENDER_RESOURCE_DESCRIPTOR_VERSION ||
        minimum_row == 0u || descriptor->row_pitch < minimum_row ||
        descriptor->height > resource->view.byte_count /
                                  descriptor->row_pitch)
        return 0;
    if (require_vram &&
        (descriptor->flags &
         XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) == 0u)
        return 0;
    return 1;
}

static uint16_t native_read_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8u);
}

static uint16_t native_rgba_to_1555(uint32_t color) {
    return (uint16_t)(((color & 0xffu) >> 3u) |
        ((((color >> 8u) & 0xffu) >> 3u) << 5u) |
        ((((color >> 16u) & 0xffu) >> 3u) << 10u) |
        (((color >> 24u) != 0u) ? UINT16_C(0x8000) : 0u));
}

static int native_resource_local_xy(const XgRenderResourceDescriptor *descriptor,
                                int x, int y, int *local_x, int *local_y) {
    if ((descriptor->flags &
         XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u) {
        x -= (int)descriptor->vram_x;
        y -= (int)descriptor->vram_y;
        if (x < 0 || y < 0 || (uint32_t)x >= descriptor->vram_width ||
            (uint32_t)y >= descriptor->vram_height)
            return 0;
    }
    if (x < 0 || y < 0 || (uint32_t)x >= descriptor->width ||
        (uint32_t)y >= descriptor->height)
        return 0;
    *local_x = x;
    *local_y = y;
    return 1;
}

static GlNativeCpuSurface *native_cpu_surface_for(GlNativeCpuCompiler *compiler,
                                         GlNativeResourceRecord *resource) {
    GlNativeResourceRecord *base;
    if (!compiler || !resource) return NULL;
    if (compiler->native_vram &&
        (resource == compiler->native_texture || resource == compiler->native_clut))
        return compiler->native_vram;
    base = compiler->audit->resources;
    if (resource < base ||
        resource >= base + compiler->audit->consumed_resources)
        return NULL;
    return &compiler->surfaces[resource - base];
}

static int native_cpu_surface_load(GlNativeCpuCompiler *compiler,
                               GlNativeResourceRecord *resource,
                               uint32_t blocker, uint32_t record_index,
                               int preserve_history,
                               GlNativeCpuSurface **out_surface) {
    const XgRenderResourceDescriptor *descriptor;
    GlNativeCpuSurface *surface;
    const GlNativePersistentSurface *persistent = NULL;
    const XgPresentationIdentity *identity = &compiler->audit->header.identity;
    const uint8_t *bytes;
    size_t pixel_count;
    if (!native_pixel_descriptor_valid(resource, 0)) {
        native_audit_block(compiler->audit, blocker, record_index,
                       resource ? resource->reference.resource_id : 0u,
                       resource ? resource->reference.generation : 0u);
        return 0;
    }
    descriptor = &resource->view.descriptor;
    if (descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8 &&
        descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24 &&
        descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 &&
        descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555) {
        native_audit_block(compiler->audit, blocker, record_index,
                       resource->reference.resource_id,
                       resource->reference.generation);
        return 0;
    }
    surface = native_cpu_surface_for(compiler, resource);
    if (!surface) return 0;
    if (surface->valid && surface->pixels) {
        *out_surface = surface;
        return 1;
    }
    if (preserve_history) {
        if (native_persistent_scope_matches(identity))
            persistent = native_persistent_surface_find(
                resource->reference.resource_id, resource->reference.generation);
        if (surface->pixels || (persistent &&
            (identity->source_sequence <=
                 s_native_persistent_identity.source_sequence ||
             persistent->seed_digest != resource->reference.content_digest ||
             persistent->width != descriptor->width ||
             persistent->height != descriptor->height))) {
            native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_SURFACE_HISTORY,
                           record_index, resource->reference.resource_id,
                           resource->reference.generation);
            return 0;
        }
    }
    if (!surface->pixels) {
        if (descriptor->width > SIZE_MAX / descriptor->height ||
            (pixel_count = (size_t)descriptor->width * descriptor->height) >
                SIZE_MAX / sizeof(*surface->pixels)) {
            native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
                           record_index, resource->reference.resource_id,
                           resource->reference.generation);
            return 0;
        }
        surface->pixels = (uint32_t *)malloc(
            pixel_count * sizeof(*surface->pixels));
        if (!surface->pixels) {
            native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
                           record_index, resource->reference.resource_id,
                           resource->reference.generation);
            return 0;
        }
        surface->resource = resource;
        surface->width = descriptor->width;
        surface->height = descriptor->height;
    }
    if (persistent) {
        memcpy(surface->pixels, persistent->pixels,
               (size_t)surface->width * surface->height *
                   sizeof(*surface->pixels));
        surface->valid = 1;
        *out_surface = surface;
        return 1;
    }
    bytes = (const uint8_t *)resource->view.bytes;
    for (uint32_t y = 0u; y < descriptor->height; ++y) {
        const uint8_t *row = bytes + (size_t)y * descriptor->row_pitch;
        uint32_t *target = surface->pixels + (size_t)y * descriptor->width;
        for (uint32_t x = 0u; x < descriptor->width; ++x) {
            if (descriptor->pixel_format ==
                    XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8) {
                target[x] = (uint32_t)row[x * 4u] |
                    (uint32_t)row[x * 4u + 1u] << 8u |
                    (uint32_t)row[x * 4u + 2u] << 16u |
                    (uint32_t)row[x * 4u + 3u] << 24u;
            } else if (descriptor->pixel_format ==
                       XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24) {
                target[x] = (uint32_t)row[x * 3u] |
                    (uint32_t)row[x * 3u + 1u] << 8u |
                    (uint32_t)row[x * 3u + 2u] << 16u |
                    UINT32_C(0xff000000);
            } else {
                target[x] = conv_1555_to_rgba8(
                    native_read_u16(row + x * 2u));
            }
        }
    }
    surface->valid = 1;
    *out_surface = surface;
    return 1;
}

static void native_cpu_compiler_destroy(GlNativeCpuCompiler *compiler) {
    if (!compiler) return;
    for (uint32_t i = 0u; i < XG_RENDER_SCENE_RESOURCE_CAPACITY; ++i)
        free(compiler->surfaces[i].pixels);
    memset(compiler, 0, sizeof(*compiler));
}

void native_persistent_surfaces_clear(void) {
    for (uint32_t i = 0u; i < XG_RENDER_SCENE_RESOURCE_CAPACITY; ++i)
        free(s_native_persistent_surfaces[i].pixels);
    memset(s_native_persistent_surfaces, 0, sizeof(s_native_persistent_surfaces));
    memset(&s_native_persistent_identity, 0, sizeof(s_native_persistent_identity));
}

static GlNativePersistentSurface *native_persistent_surface_find(
        uint64_t resource_id, uint64_t generation) {
    for (uint32_t i = 0u; i < XG_RENDER_SCENE_RESOURCE_CAPACITY; ++i) {
        GlNativePersistentSurface *surface = &s_native_persistent_surfaces[i];
        if (surface->valid && surface->resource_id == resource_id &&
            surface->generation == generation)
            return surface;
    }
    return NULL;
}

/* Called under the endpoint publication lock, after all fallible work. Each
 * stored surface already owns its allocation. There are at most as many stores
 * as cache slots; increasing source sequences prevent evicting a new store. */
static void native_cpu_surface_commit_stores(GlNativeCpuCompiler *compiler) {
    const XgPresentationIdentity *identity = &compiler->audit->header.identity;
    if (!native_persistent_scope_matches(identity))
        native_persistent_surfaces_clear();
    for (uint32_t i = 0u; i < XG_RENDER_SCENE_RESOURCE_CAPACITY; ++i) {
        GlNativeCpuSurface *surface = &compiler->surfaces[i];
        GlNativePersistentSurface *persistent;
        if (!surface->stored) continue;
        persistent = native_persistent_surface_find(
            surface->resource->reference.resource_id,
            surface->resource->reference.generation);
        if (!persistent) {
            persistent = &s_native_persistent_surfaces[0];
            for (uint32_t j = 0u; j < XG_RENDER_SCENE_RESOURCE_CAPACITY; ++j) {
                GlNativePersistentSurface *candidate = &s_native_persistent_surfaces[j];
                if (!candidate->valid) {
                    persistent = candidate;
                    break;
                }
                if (candidate->use_sequence < persistent->use_sequence)
                    persistent = candidate;
            }
        }
        free(persistent->pixels);
        *persistent = (GlNativePersistentSurface){
            .resource_id = surface->resource->reference.resource_id,
            .generation = surface->resource->reference.generation,
            .seed_digest = surface->resource->reference.content_digest,
            .use_sequence = identity->source_sequence,
            .pixels = surface->pixels,
            .width = surface->width,
            .height = surface->height,
            .valid = 1,
        };
        surface->pixels = NULL;
    }
    s_native_persistent_identity = *identity;
}

static int native_surface_pixel(const GlNativeCpuSurface *surface, int x, int y,
                            uint32_t **out_pixel) {
    const XgRenderResourceDescriptor *descriptor;
    int local_x, local_y;
    if (!surface || !surface->valid || !surface->pixels || !surface->resource)
        return 0;
    descriptor = &surface->resource->view.descriptor;
    if (!native_resource_local_xy(descriptor, x, y, &local_x, &local_y)) return 0;
    *out_pixel = surface->pixels + (size_t)local_y * surface->width + local_x;
    return 1;
}

static int native_pixel_in_viewport(const GlNativeCpuCompiler *compiler,
                                const GlNativeCpuSurface *surface,
                                const uint32_t *pixel) {
    const ptrdiff_t offset = pixel - surface->pixels;
    uint32_t x;
    uint32_t y;

    if (offset < 0 || (size_t)offset >=
            (size_t)surface->width * surface->height)
        return 0;
    x = (uint32_t)((size_t)offset % surface->width);
    y = (uint32_t)((size_t)offset / surface->width);
    return x >= compiler->viewport_x && y >= compiler->viewport_y &&
        x - compiler->viewport_x < compiler->viewport_width &&
        y - compiler->viewport_y < compiler->viewport_height;
}

static int native_surface_local_pixel(const GlNativeCpuSurface *surface, int x, int y,
                                  uint32_t **out_pixel) {
    if (!surface || !surface->valid || !surface->pixels || x < 0 || y < 0 ||
        (uint32_t)x >= surface->width || (uint32_t)y >= surface->height)
        return 0;
    *out_pixel = surface->pixels + (size_t)y * surface->width + x;
    return 1;
}

static int native_resource_word(GlNativeCpuCompiler *compiler,
                            GlNativeResourceRecord *resource, int x, int y,
                            uint16_t *out_word) {
    const XgRenderResourceDescriptor *descriptor;
    GlNativeCpuSurface *surface;
    int local_x, local_y;
    if (compiler->native_vram &&
        (resource == compiler->native_texture || resource == compiler->native_clut)) {
        *out_word = compiler->native_words[
            (size_t)(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))];
        return 1;
    }
    if (!native_pixel_descriptor_valid(resource, 0)) return 0;
    descriptor = &resource->view.descriptor;
    if (!native_resource_local_xy(descriptor, x, y, &local_x, &local_y)) return 0;
    surface = native_cpu_surface_for(compiler, resource);
    if ((resource->view.kind == XG_RENDER_RESOURCE_FRAMEBUFFER ||
         resource->view.kind == XG_RENDER_RESOURCE_GENERATED_SURFACE) &&
        !native_cpu_surface_load(compiler, resource,
            GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR, 0u, 1, &surface))
        return 0;
    if (surface && surface->valid && surface->pixels) {
        *out_word = native_rgba_to_1555(
            surface->pixels[(size_t)local_y * surface->width + local_x]);
        return 1;
    }
    if (descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 &&
        descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555)
        return 0;
    *out_word = native_read_u16((const uint8_t *)resource->view.bytes +
        (size_t)local_y * descriptor->row_pitch + (size_t)local_x * 2u);
    return 1;
}

static int native_resource_word_local(GlNativeCpuCompiler *compiler,
                                  GlNativeResourceRecord *resource, int x, int y,
                                  uint16_t *out_word) {
    const XgRenderResourceDescriptor *descriptor;
    GlNativeCpuSurface *surface;
    if (compiler->native_vram &&
        (resource == compiler->native_texture || resource == compiler->native_clut)) {
        *out_word = compiler->native_words[
            (size_t)(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))];
        return 1;
    }
    if (!native_pixel_descriptor_valid(resource, 0) || x < 0 || y < 0)
        return 0;
    descriptor = &resource->view.descriptor;
    if ((uint32_t)x >= descriptor->width ||
        (uint32_t)y >= descriptor->height)
        return 0;
    surface = native_cpu_surface_for(compiler, resource);
    if ((resource->view.kind == XG_RENDER_RESOURCE_FRAMEBUFFER ||
         resource->view.kind == XG_RENDER_RESOURCE_GENERATED_SURFACE) &&
        !native_cpu_surface_load(compiler, resource,
            GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR, 0u, 1, &surface))
        return 0;
    if (surface && surface->valid && surface->pixels) {
        *out_word = native_rgba_to_1555(
            surface->pixels[(size_t)y * surface->width + x]);
        return 1;
    }
    if (descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 &&
        descriptor->pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555)
        return 0;
    *out_word = native_read_u16((const uint8_t *)resource->view.bytes +
        (size_t)y * descriptor->row_pitch + (size_t)x * 2u);
    return 1;
}

int native_texture_window_coord(int coordinate, uint8_t mask,
                                   uint8_t offset) {
    coordinate &= 255;
    return (coordinate & ~((int)mask * 8)) |
           (((int)offset & (int)mask) * 8);
}

static int native_wrap_coord(int coordinate, uint32_t size,
                         XgRenderResourceWrap wrap) {
    int period;
    if (size == 0u || size > INT_MAX) return -1;
    if (wrap == XG_RENDER_RESOURCE_WRAP_CLAMP)
        return coordinate < 0 ? 0 :
            coordinate >= (int)size ? (int)size - 1 : coordinate;
    if (wrap == XG_RENDER_RESOURCE_WRAP_REPEAT) {
        coordinate %= (int)size;
        return coordinate < 0 ? coordinate + (int)size : coordinate;
    }
    if (wrap != XG_RENDER_RESOURCE_WRAP_MIRROR || size > INT_MAX / 2u)
        return -1;
    period = (int)size * 2;
    coordinate %= period;
    if (coordinate < 0) coordinate += period;
    return coordinate < (int)size ? coordinate : period - 1 - coordinate;
}

static int native_sample_draw_texel(GlNativeCpuCompiler *compiler,
                                const XgSemanticDrawRecord *draw,
                                GlNativeResourceRecord *texture,
                                GlNativeResourceRecord *clut,
                                int u, int v, uint16_t *out_texel) {
    const XgRenderIrMaterialState *material = &draw->primitive.material;
    const XgRenderResourceDescriptor *descriptor;
    int page_x, page_y, index = 0;
    uint16_t packed;
    if (!texture ||
        (texture != compiler->native_texture && !native_pixel_descriptor_valid(texture, 0)))
        return 0;
    descriptor = &texture->view.descriptor;
    u = native_texture_window_coord(u, material->texture_window_mask_x,
                                material->texture_window_offset_x);
    v = native_texture_window_coord(v, material->texture_window_mask_y,
                                material->texture_window_offset_y);
    if (descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8 ||
        descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24) {
        GlNativeCpuSurface *surface = NULL;
        uint32_t *pixel;
        int sample_x = u;
        int sample_y = v;
        if (!native_cpu_surface_load(compiler, texture,
                GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR, 0u, 1, &surface))
            return 0;
        if (sample_x < 0 || sample_y < 0) return 0;
        if ((descriptor->flags &
             XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u) {
            sample_x += (int)material->texture_page_x * 64;
            sample_y += (int)material->texture_page_y * 256;
            if (!native_surface_pixel(surface, sample_x, sample_y, &pixel))
                return 0;
        } else {
            sample_x = native_wrap_coord(sample_x, descriptor->width,
                                     descriptor->wrap_u);
            sample_y = native_wrap_coord(sample_y, descriptor->height,
                                     descriptor->wrap_v);
            if (!native_surface_local_pixel(surface, sample_x, sample_y, &pixel))
                return 0;
        }
        if ((*pixel >> 24u) == 0u) {
            *out_texel = 0u;
        } else {
            *out_texel = native_rgba_to_1555(*pixel) | UINT16_C(0x8000);
        }
        return 1;
    }
    if (descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 ||
        descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8) {
        int local_x, local_y;
        const uint8_t *row;
        local_x = native_wrap_coord(u, descriptor->width, descriptor->wrap_u);
        local_y = native_wrap_coord(v, descriptor->height, descriptor->wrap_v);
        if (local_x < 0 || local_y < 0)
            return 0;
        row = (const uint8_t *)texture->view.bytes +
            (size_t)local_y * descriptor->row_pitch;
        index = descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4
            ? (row[(uint32_t)local_x >> 1u] >> ((local_x & 1) * 4)) & 15
            : row[local_x];
    } else if (descriptor->pixel_format ==
               XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555) {
        page_x = (int)material->texture_page_x * 64;
        page_y = (int)material->texture_page_y * 256;
        if (material->texture_depth == XG_RENDER_IR_TEXTURE_15_BIT)
            return native_resource_word(compiler, texture, page_x + u,
                                    page_y + v, out_texel);
        if (!native_resource_word(compiler, texture,
                page_x + (material->texture_depth ==
                              XG_RENDER_IR_TEXTURE_4_BIT ? u / 4 : u / 2),
                page_y + v, &packed))
            return 0;
        index = material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT
            ? (packed >> ((u & 3) * 4)) & 15
            : (packed >> ((u & 1) * 8)) & 255;
    } else {
        return 0;
    }
    if (!clut ||
        (clut != compiler->native_clut && !native_pixel_descriptor_valid(clut, 0)))
        return 0;
    if ((clut->view.descriptor.flags &
         XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u)
        return native_resource_word(compiler, clut,
                                (int)material->clut_x + index,
                                material->clut_y, out_texel);
    return native_resource_word(compiler, clut, index, 0, out_texel);
}

static int native_fixed_floor(int32_t value) {
    int quotient = value / INT32_C(65536);
    if (value < 0 && (value & INT32_C(0xffff)) != 0) quotient--;
    return quotient;
}

static int native_clamp5(int value) {
    return value < 0 ? 0 : value > 31 ? 31 : value;
}

static inline uint16_t native_psx_fragment_word(uint32_t destination, int r5, int g5, int b5,
                                int mask, int blend, int blend_mode) {
    int dr = (int)(destination & 0xffu) >> 3u;
    int dg = (int)((destination >> 8u) & 0xffu) >> 3u;
    int db = (int)((destination >> 16u) & 0xffu) >> 3u;
    if (blend) {
        switch (blend_mode) {
        case XG_RENDER_IR_BLEND_AVERAGE:
            r5 = (dr + r5) / 2; g5 = (dg + g5) / 2; b5 = (db + b5) / 2;
            break;
        case XG_RENDER_IR_BLEND_ADD:
            r5 += dr; g5 += dg; b5 += db;
            break;
        case XG_RENDER_IR_BLEND_SUBTRACT:
            r5 = dr - r5; g5 = dg - g5; b5 = db - b5;
            break;
        case XG_RENDER_IR_BLEND_ADD_QUARTER:
            r5 = dr + r5 / 4; g5 = dg + g5 / 4; b5 = db + b5 / 4;
            break;
        }
    }
    r5 = native_clamp5(r5); g5 = native_clamp5(g5); b5 = native_clamp5(b5);
    return (uint16_t)(r5 | (g5 << 5u) | (b5 << 10u) | (mask ? UINT16_C(0x8000) : 0u));
}

static uint32_t native_psx_fragment(uint32_t destination, int r5, int g5, int b5,
                                int mask, int blend, int blend_mode) {
    return conv_1555_to_rgba8(native_psx_fragment_word(destination, r5, g5, b5, mask, blend, blend_mode));
}

/* Keep the transport conversion local: linking submission would pull producer
 * and authentication state into the otherwise independent render worker. */
static void native_materialize_triangle(const GpuRenderSemanticTriangle *source_triangle,
                                        XgRenderIrTriangle *triangle) {
    triangle->split_index = source_triangle->split_index;
    triangle->split_count = source_triangle->split_count;
    for (uint32_t vi = 0u; vi < 3u; ++vi) {
        XgRenderIrVertex *target = &triangle->vertices[vi];
        const GpuRenderSemanticVertex *source = &source_triangle->vertices[vi];
#define NATIVE_COPY_VERTEX(field) target->field = source->field
        NATIVE_COPY_VERTEX(x); NATIVE_COPY_VERTEX(y); NATIVE_COPY_VERTEX(u); NATIVE_COPY_VERTEX(v);
        NATIVE_COPY_VERTEX(r); NATIVE_COPY_VERTEX(g); NATIVE_COPY_VERTEX(b);
        NATIVE_COPY_VERTEX(native_view_x); NATIVE_COPY_VERTEX(native_view_y);
        NATIVE_COPY_VERTEX(native_view_position); NATIVE_COPY_VERTEX(native_view_depth);
        NATIVE_COPY_VERTEX(projective_view_x); NATIVE_COPY_VERTEX(projective_view_y);
        NATIVE_COPY_VERTEX(projective_view_z);
        NATIVE_COPY_VERTEX(projective_offset_x); NATIVE_COPY_VERTEX(projective_offset_y);
        NATIVE_COPY_VERTEX(projective_native_offset_x); NATIVE_COPY_VERTEX(projective_native_offset_y);
        NATIVE_COPY_VERTEX(projective_distance); NATIVE_COPY_VERTEX(projective_position);
        NATIVE_COPY_VERTEX(temporal_depth); NATIVE_COPY_VERTEX(temporal_depth_valid);
        NATIVE_COPY_VERTEX(interpolation_group_id); NATIVE_COPY_VERTEX(interpolation_vertex_id);
        NATIVE_COPY_VERTEX(interpolation_vertex_identity_valid);
#undef NATIVE_COPY_VERTEX
    }
}

static int native_materialize_native_draw(const GpuRenderSemantic *semantic,
                                      XgSemanticDrawRecord *draw) {
    if (semantic->triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY ||
        semantic->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY)
        return 0;
    memset(draw, 0, sizeof(*draw));
    draw->topology = semantic->topology;
    draw->line_count = semantic->line_count;
    memcpy(draw->lines, semantic->lines,
           semantic->line_count * sizeof(draw->lines[0]));
    draw->screen_space_2d = semantic->screen_space_2d;
    draw->aa_exempt = semantic->aa_exempt;
    draw->sprite_texture = semantic->sprite_texture;
    draw->native_view_effect = semantic->native_view_effect;
    draw->native_view_effect_index = semantic->native_view_effect_index;
    XgRenderIrNativePrimitive *primitive = &draw->primitive;
    primitive->depth_policy = semantic->depth_policy;
    primitive->depth_bias = semantic->depth_bias;
    primitive->aa_exempt = semantic->aa_exempt;
    primitive->sprite_texture = semantic->sprite_texture;
#define NATIVE_COPY_MATERIAL(field) primitive->material.field = semantic->material.field
    NATIVE_COPY_MATERIAL(tpage);
    NATIVE_COPY_MATERIAL(texture_page_x); NATIVE_COPY_MATERIAL(texture_page_y);
    NATIVE_COPY_MATERIAL(clut_x); NATIVE_COPY_MATERIAL(clut_y);
    NATIVE_COPY_MATERIAL(draw_area_left); NATIVE_COPY_MATERIAL(draw_area_top);
    NATIVE_COPY_MATERIAL(draw_area_right); NATIVE_COPY_MATERIAL(draw_area_bottom);
    NATIVE_COPY_MATERIAL(draw_offset_x); NATIVE_COPY_MATERIAL(draw_offset_y);
    NATIVE_COPY_MATERIAL(texture_window_mask_x); NATIVE_COPY_MATERIAL(texture_window_mask_y);
    NATIVE_COPY_MATERIAL(texture_window_offset_x); NATIVE_COPY_MATERIAL(texture_window_offset_y);
    NATIVE_COPY_MATERIAL(textured); NATIVE_COPY_MATERIAL(raw_texture);
    NATIVE_COPY_MATERIAL(semi_transparent); NATIVE_COPY_MATERIAL(dither);
    NATIVE_COPY_MATERIAL(mask_set); NATIVE_COPY_MATERIAL(mask_check);
#undef NATIVE_COPY_MATERIAL
    primitive->material.texture_depth = (XgRenderIrTextureDepth)semantic->material.texture_depth;
    primitive->material.shading = (XgRenderIrShading)semantic->material.shading;
    primitive->material.blend_mode = (XgRenderIrBlendMode)semantic->material.blend_mode;
    primitive->triangle_count = semantic->triangle_count;
    for (uint32_t ti = 0u; ti < semantic->triangle_count; ++ti)
        native_materialize_triangle(&semantic->triangles[ti], &primitive->triangles[ti]);
    return 1;
}

void native_attribute_planes(const XgRenderIrTriangle *triangle,
                                const double x[3], const double y[3],
                                uint32_t scale, int gouraud,
                                GlNativeAttributePlanes *out) {
    const int continuous = scale == 0u;
    if (continuous) scale = 1u;
    double value[5][3];
    const double area = (x[1]-x[0])*(y[2]-y[0]) - (x[2]-x[0])*(y[1]-y[0]);
    /* Beetle DrawTriangle's core selection, before its Y sort, including its
     * asymmetric X tie rules. Never sort XY independently of UV. */
    const unsigned core = x[1] <= x[0] ? (x[2] <= x[1] ? 2u : 1u) :
                          x[2] < x[0] ? 2u : 0u;
    memset(out, 0, sizeof(*out));
    out->x = x[core]; out->y = y[core]; out->integral = !continuous;
    {
        /* The PS1 never samples a mapping's exclusive-edge texel (gpu_uv.h).
         * The seed bias above lands on it exactly only for unit uv slopes; a
         * scaled mirrored 2D card (Field actor halves) otherwise reaches the
         * never-sampled column at its edge and paints its transparent texel
         * as a seam. Clamp to the sampled range like the canonical path. */
        float fx[3], fy[3];
        int us[3], vs[3];
        for (unsigned i = 0u; i < 3u; ++i) {
            fx[i] = (float)x[i]; fy[i] = (float)y[i];
            us[i] = (int)floor(triangle->vertices[i].u / 65536.0);
            vs[i] = (int)floor(triangle->vertices[i].v / 65536.0);
        }
        psx_uv_tri_limits_f32(fx, fy, us, vs, out->limits);
    }
    for (unsigned i = 0u; i < 3u; ++i) {
        const XgRenderIrVertex *v = &triangle->vertices[i];
        const XgRenderIrVertex *c = &triangle->vertices[gouraud ? i : 0u];
        value[0][i] = v->u / 65536.0; value[1][i] = v->v / 65536.0;
        value[2][i] = c->r; value[3][i] = c->g; value[4][i] = c->b;
        if (x[i] != floor(x[i]) || y[i] != floor(y[i]) ||
            v->u % 65536 || v->v % 65536) out->integral = 0;
    }
    if (area == 0.0) return;
    for (unsigned a = 0u; a < 5u; ++a) {
        if (a >= 2u && !gouraud) {
            /* Flat colour has no gradients, including the texture-footprint
             * caller which only consumes UV. Keep the exact Q12 seed. */
            const int64_t seed = (int64_t)value[a][0] * 4096 + 2048;
            out->plane[a][0] = out->integral ? seed / 4096.0 : value[a][0] + 0.5;
            out->dda[a][0] = out->integral ? (uint32_t)seed & UINT32_C(0xfffff) : 0u;
            continue;
        }
        const double nx = (value[a][1]-value[a][0])*(y[2]-y[0]) -
                          (value[a][2]-value[a][0])*(y[1]-y[0]);
        const double ny = (x[1]-x[0])*(value[a][2]-value[a][0]) -
                          (x[2]-x[0])*(value[a][1]-value[a][0]);
        const double dx = nx / area, dy = ny / area;
        /* Native UV seed is 1/2 texel. Upscaled affine sampling follows
         * Beetle's 1/(2*S) seed and decreasing-axis 1-1/S compensation.
         * This is raster-local; PsyQ's authored reversal remains untouched. */
        const int mirror = a < 2u && ((dx < 0.0 && dy == 0.0) ||
                                      (dy < 0.0 && dx == 0.0));
        const double bias = a < 2u ? 0.5 / scale +
            (mirror ? 1.0 - 1.0 / scale : 0.0) : 0.5;
        out->plane[a][0] = value[a][core] + bias;
        out->plane[a][1] = dx; out->plane[a][2] = dy;
        if (out->integral) {
            /* Integer products are exact here (Q16 inputs have integer parts
             * in signed 16-bit range). Division must truncate toward zero,
             * including negative slopes, before accumulator padding. Keeping
             * only 20 bits is equivalent to Beetle's uint32 Q24 wrap >> 12. */
            const int64_t denominator = (int64_t)area * scale;
            const int64_t sx = (int64_t)nx * 4096 / denominator;
            const int64_t sy = (int64_t)ny * 4096 / denominator;
            const int64_t seed = (int64_t)value[a][core] * 4096 +
                (a < 2u ? 2048 / scale + (mirror ? 4096 - 4096 / scale : 0) : 2048);
            out->dda[a][0] = (uint32_t)seed & UINT32_C(0xfffff);
            out->dda[a][1] = (uint32_t)sx & UINT32_C(0xfffff);
            out->dda[a][2] = (uint32_t)sy & UINT32_C(0xfffff);
            /* Unwrapped counterpart for dependency extrema. Do not infer a
             * signed gradient from the modulo-256 shader accumulator. */
            out->plane[a][0] = seed / 4096.0;
            out->plane[a][1] = sx * (double)scale / 4096.0;
            out->plane[a][2] = sy * (double)scale / 4096.0;
        }
    }
}

/* Perspective payload contract for one CPU-rasterized triangle: every vertex
 * must carry a projective position with a positive view z (the divisor used by
 * q = 1/z). A partially projected triangle keeps the faithful affine path
 * instead of blocking the frame, exactly like the GPU vertex weights. */
static int native_triangle_all_projective(const XgRenderIrTriangle *triangle) {
    unsigned vertex;
    if (triangle == NULL) return 0;
    for (vertex = 0u; vertex < 3u; ++vertex) {
        const XgRenderIrVertex *current = &triangle->vertices[vertex];
        if (!current->projective_position || current->projective_view_z <= 0)
            return 0;
    }
    return 1;
}

/* ---- Native depth test (host option; see GpuRenderDepthPolicy) ----------
 * The PS1 has no depth buffer: primitives are sorted into OT buckets and the
 * game relies on that order. The optional depth plane only arbitrates between
 * producer-classified 3D surfaces. Invariant: a plane texel holds the key of
 * the certified opaque surface visible there, or 0 ("far") when anything else
 * (a transfer, seed, clear, uncertified or merely tested opaque fragment)
 * wrote it. Blended fragments leave it unchanged. So OT order decides wherever
 * two certified surfaces do not meet directly.
 *
 * Key: FP32 D = 4096 / z_q12 (= 1 / view Z), affine in screen space.
 * The R32UI planes preserve the float bits, not a quantized integer depth.
 * Each triangle has an FP32 seed at the integer raster origin and logical
 * gradients; GPU upload scales the gradients once per draw. Two fused
 * multiply-adds evaluate the plane. Ties within 2^-10 resolve to the later
 * draw (OT order). Guest VRAM (plane 0 / native_vram) never has depth. */

/* Q12 unfloored view Z as projected. z <= H/2 is where the GTE quotient
 * saturates, so the vertex was placed as if at H/2: use that depth rather than
 * dropping the triangle (the near-plane "clip" in depth space). z > 0xffff is
 * the far saturation. Behind the camera (z <= 0) has no depth. */
static int32_t native_depth_q12(double z, uint32_t distance) {
    if (!isfinite(z) || z <= 0.0) return 0;
    const double near_depth = distance * (65536.0 / 131071.0);
    if (z < near_depth) z = near_depth;
    if (z > 65535.0) z = 65535.0;
    const double q12 = z * 4096.0;
    return q12 >= 1.0 ? (int32_t)llround(q12) : 0;
}

/* Unfloored producer depth first; the integer GTE MAC3 only as a fallback. */
int32_t native_vertex_depth_q12(const XgRenderIrVertex *vertex) {
    if (vertex->native_view_depth > 0) return vertex->native_view_depth;
    return vertex->projective_position && vertex->projective_view_z > 0 &&
        vertex->projective_view_z <= 0xffff ? vertex->projective_view_z * 4096 : 0;
}

/* Reciprocal depth through the exact raster positions used by coverage.
 * Prepare in double, then round the seed and gradients once to FP32. */
static int native_depth_plane(const XgRenderIrTriangle *triangle,
                              const double x[3], const double y[3], GlNativeDepthPlane *out) {
    double d[3];
    for (unsigned i = 0u; i < 3u; ++i) {
        const int32_t z = native_vertex_depth_q12(&triangle->vertices[i]);
        if (z <= 0) return 0;
        d[i] = 4096.0 / z;
    }
    const double area = (x[1]-x[0])*(y[2]-y[0]) - (x[2]-x[0])*(y[1]-y[0]);
    if (area == 0.0 || !isfinite(area)) return 0;
    if (!isfinite(x[0]) || !isfinite(y[0]) || fabs(x[0]) > 1048576.0 || fabs(y[0]) > 1048576.0) return 0;
    const double ox = floor(x[0]), oy = floor(y[0]);
    const double dx = ((d[1]-d[0])*(y[2]-y[0]) - (d[2]-d[0])*(y[1]-y[0])) / area;
    const double dy = ((x[1]-x[0])*(d[2]-d[0]) - (x[2]-x[0])*(d[1]-d[0])) / area;
    const float seed = (float)(d[0] + dx * (ox - x[0]) + dy * (oy - y[0]));
    const float a = (float)dx, b = (float)dy;
    if (isfinite(seed) && isfinite(a) && isfinite(b)) {
        *out = (GlNativeDepthPlane){seed, a, b, (int32_t)ox, (int32_t)oy};
        return 1;
    }
    /* Only non-representable planes need a conservative flat fallback now;
     * finite steep triangles no longer hit the old fixed-point slope limit. */
    *out = (GlNativeDepthPlane){(float)fmin(fmin(d[0], d[1]), d[2]), 0.0f, 0.0f, (int32_t)ox, (int32_t)oy};
    return 1;
}

static inline uint32_t native_depth_at(const GlNativeDepthPlane *plane, int x, int y) {
    return native_depth_evaluate(plane->n0, plane->a, plane->b, (float)(x - plane->ox), (float)(y - plane->oy));
}

static inline int native_depth_pass(uint32_t key, int bias, uint32_t stored) {
    return native_depth_biased(key, (unsigned)bias) >= native_depth_threshold(stored);
}

/* Per-triangle mode from the draw's stamp and its FINAL material. */
GlNativeDepthMode native_depth_mode(const XgSemanticDrawRecord *draw, const XgRenderIrTriangle *triangle,
                                           const double x[3], const double y[3]) {
    GlNativeDepthMode mode = {0, GL_NATIVE_DEPTH_RESET, 0, 0, GL_NATIVE_DEPTH_SHOW_NONE, {0}};
    unsigned policy = draw->primitive.depth_policy;
    mode.semi = draw->primitive.material.semi_transparent ? 1 : 0;
    if (draw->topology != GPU_RENDER_SEMANTIC_TRIANGLES || draw->screen_space_2d || draw->native_view_effect ||
        policy > GPU_RENDER_DEPTH_TEST_WRITE)
        policy = GPU_RENDER_DEPTH_NONE;
    if (policy == GPU_RENDER_DEPTH_NONE) return mode;
    if (!triangle || !native_depth_plane(triangle, x, y, &mode.plane)) {
        mode.show = GL_NATIVE_DEPTH_SHOW_FALLBACK;
        return mode;
    }
    mode.test = 1;
    mode.bias = draw->primitive.depth_bias <= 16u ? draw->primitive.depth_bias : 0;
    if (policy == GPU_RENDER_DEPTH_TEST_WRITE) {
        mode.write = GL_NATIVE_DEPTH_WRITE;
        mode.show = GL_NATIVE_DEPTH_SHOW_WRITE;
    } else mode.show = GL_NATIVE_DEPTH_SHOW_TEST;
    return mode;
}

/* CPU reference for the extra Native 3D texture taps. Nearest coverage/STP is
 * decided by the caller; these samples contribute only RGB, after the CLUT. */
static int native_filter_tap(GlNativeCpuCompiler *compiler,
        const XgSemanticDrawRecord *draw, GlNativeResourceRecord *texture,
        GlNativeResourceRecord *clut, const uint16_t *native_words,
        const GlNativeAttributePlanes *attributes, int u, int v,
        uint16_t *out) {
    const XgRenderIrMaterialState *m = &draw->primitive.material;
    u &= 255; v &= 255;
    if (!(m->texture_window_mask_x | m->texture_window_mask_y)) {
        if (u < attributes->limits[0]) u = attributes->limits[0];
        if (u > attributes->limits[2]) u = attributes->limits[2];
        if (v < attributes->limits[1]) v = attributes->limits[1];
        if (v > attributes->limits[3]) v = attributes->limits[3];
    }
    if (native_words) {
        *out = native_native_texel(native_words, m, u, v);
        return 1;
    }
    return native_sample_draw_texel(compiler, draw, texture, clut, u, v, out);
}

static int native_anisotropic_tap(GlNativeCpuCompiler *compiler,
        const XgSemanticDrawRecord *draw, GlNativeResourceRecord *texture,
        GlNativeResourceRecord *clut, const uint16_t *native_words,
        const GlNativeAttributePlanes *attributes, double u, double v,
        double color[3], int *valid) {
    const double fu = u - floor(u) - 0.5, fv = v - floor(v) - 0.5;
    const int du = fu < 0.0 ? -1 : 1, dv = fv < 0.0 ? -1 : 1;
    const double x = fabs(fu), y = fabs(fv);
    const double weights[4] = {
        (1.0 - x) * (1.0 - y), x * (1.0 - y),
        (1.0 - x) * y, x * y,
    };
    uint16_t taps[4];
    double total = 0.0;
    color[0] = color[1] = color[2] = 0.0;
    *valid = 0;
    for (int i = 0; i < 4; ++i) {
        if (!native_filter_tap(compiler, draw, texture, clut, native_words,
                attributes, (int)floor(u) + ((i & 1) ? du : 0),
                (int)floor(v) + ((i & 2) ? dv : 0), &taps[i]))
            return 0;
        if (taps[i]) total += weights[i];
    }
    if (total == 0.0) return 1;
    for (int i = 0; i < 4; ++i) if (taps[i])
        for (int channel = 0; channel < 3; ++channel)
            color[channel] += ((taps[i] >> (channel * 5)) & 31) * weights[i] / total;
    *valid = 1;
    return 1;
}

/* Singular-value footprint shared with the GL shader: physical-pixel texel
 * gradients, major-axis span, and at most N samples, only for minification. */
static int native_anisotropic_span(double ux, double vx, double uy, double vy,
        int maximum, double *span_u, double *span_v) {
    const double xx = ux*ux + vx*vx, yy = uy*uy + vy*vy;
    const double xy = ux*uy + vx*vy;
    const double major = 0.5 * (xx + yy + sqrt((xx-yy)*(xx-yy) + 4.0*xy*xy));
    const double minor = fmax(xx + yy - major, 1.0);
    int count = (int)fmin(16.0, ceil(sqrt(major / minor)));
    if (count > maximum) count = maximum;
    if (count < 2) return 1;
    double dx = 1.0, dy = 0.0;
    if (fabs(xy) > 0.000001) {
        dx = xy; dy = major - xx;
        const double norm = hypot(dx, dy);
        dx /= norm; dy /= norm;
    } else if (yy > xx) { dx = 0.0; dy = 1.0; }
    *span_u = ux*dx + uy*dy;
    *span_v = vx*dx + vy*dy;
    return count;
}

static inline void native_attribute_step(uint32_t q[5], const GlNativeAttributePlanes *attributes) {
    q[0] += attributes->dda[0][1];
    q[1] += attributes->dda[1][1];
    q[2] += attributes->dda[2][1];
    q[3] += attributes->dda[3][1];
    q[4] += attributes->dda[4][1];
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline int native_render_draw_impl(GlNativeCpuCompiler *compiler,
                          GlNativeCpuSurface *target,
                          const XgSemanticDrawRecord *draw,
                          uint32_t record_index, int canonical) {
    static const int dither[16] = {
        -4, 0, -3, 1, 2, -2, 3, -1,
        -3, 1, -4, 0, 3, -1, 2, -2,
    };
    const XgRenderIrNativePrimitive *primitive = &draw->primitive;
    /* Immutable draw state cannot alias the writable VRAM words. Keeping a
     * local value lets the sampler hoist page/window/CLUT constants across
     * fragment writes, including the uint16_t fields of the material. */
    const XgRenderIrMaterialState material_value = primitive->material;
    const XgRenderIrMaterialState *material = &material_value;
    GlNativeResourceRecord *texture_resource = NULL;
    GlNativeResourceRecord *clut_resource = NULL;
    uint16_t encoded_depth = (uint16_t)((material->tpage >> 7u) & 3u);
    const int lines = draw->topology == GPU_RENDER_SEMANTIC_LINES;
    if (!target || !target->valid ||
        (draw->topology != GPU_RENDER_SEMANTIC_TRIANGLES && !lines) ||
        (lines && (primitive->triangle_count != 0u || draw->line_count == 0u ||
                   draw->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY ||
                   material->textured || material->raw_texture || draw->screen_space_2d ||
                   draw->native_view_effect)) ||
        (!lines && (primitive->triangle_count == 0u ||
                    primitive->triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY ||
                    draw->line_count != 0u)) ||
        draw->screen_space_2d > GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE ||
        draw->native_view_effect > GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID ||
        (!draw->native_view_effect && draw->native_view_effect_index != 0u) ||
        (draw->native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
         draw->native_view_effect_index >= 340u) ||
        material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4u) & 1u) ||
        material->blend_mode !=
            (XgRenderIrBlendMode)((material->tpage >> 5u) & 3u) ||
        material->texture_depth != (XgRenderIrTextureDepth)encoded_depth ||
        material->clut_x > 1023u || (material->clut_x & 15u) != 0u ||
        material->clut_y > 511u ||
        material->draw_area_left > 1023u || material->draw_area_top > 1023u ||
        material->draw_area_right > 1023u ||
        material->draw_area_bottom > 1023u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        material->shading < XG_RENDER_IR_SHADING_FLAT ||
        material->shading > XG_RENDER_IR_SHADING_GOURAUD ||
        material->blend_mode > XG_RENDER_IR_BLEND_ADD_QUARTER ||
        (material->raw_texture && !material->textured)) {
        native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                       record_index, 0u, 0u);
        return 0;
    }
    if (material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom) {
        compiler->audit->rendered_draws++;
        return 1;
    }
    if (compiler->gpu && (lines || compiler->gpu_only)) {
        const int origin = target->resource &&
            (target->resource->view.descriptor.flags & XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION)
            ? target->resource->view.descriptor.vram_y : 0;
        if (!native_gpu_draw(compiler, draw,
            compiler->gpu_plane >= GL_NATIVE_GPU_PHASE_BASE ? 0 : origin)) return 0;
        if (compiler->gpu_only) return 1;
    }
    if (lines) {
        if (target->words && !native_view_worker_drain(compiler, 0, 0, VRAM_W - 1, VRAM_H - 1)) return 0;
        for (uint32_t li = 0u; li < draw->line_count; ++li) {
            const GpuRenderSemanticVertex *a = &draw->lines[li].vertices[0];
            const GpuRenderSemanticVertex *b = &draw->lines[li].vertices[1];
            int x = native_fixed_floor(!compiler->native_vram && a->native_view_position
                ? a->native_view_x : a->x) + material->draw_offset_x;
            int y = native_fixed_floor(!compiler->native_vram && a->native_view_position
                ? a->native_view_y : a->y) + material->draw_offset_y;
            const int end_x = native_fixed_floor(!compiler->native_vram && b->native_view_position
                ? b->native_view_x : b->x) + material->draw_offset_x;
            const int end_y = native_fixed_floor(!compiler->native_vram && b->native_view_position
                ? b->native_view_y : b->y) + material->draw_offset_y;
            const int dx = abs(end_x - x), dy = abs(end_y - y);
            const int step_x = x < end_x ? 1 : -1;
            const int step_y = y < end_y ? 1 : -1;
            const int steps = dx > dy ? dx : dy;
            int error = dx - dy;
            if (compiler->native_vram && !compiler->view_pass &&
                (dx >= VRAM_W || dy >= VRAM_H)) continue;
            for (int step = 0;; ++step) {
                uint32_t *destination;
                if (x >= material->draw_area_left && x <= material->draw_area_right &&
                    y >= material->draw_area_top && y <= material->draw_area_bottom &&
                    (!compiler->banded ||
                     (y >= compiler->band_top && y <= compiler->band_bottom)) &&
                    native_surface_pixel(target, x, y, &destination) &&
                    native_pixel_in_viewport(compiler, target, destination) &&
                    (!material->mask_check || (*destination >> 24u) == 0u)) {
                    int color[3] = {a->r, a->g, a->b};
                    const int end_color[3] = {b->r, b->g, b->b};
                    const int phase = material->dither && !compiler->dithering_disabled ? dither[
                        (((y + compiler->dither_y) & 3) << 2) | ((x + compiler->dither_x) & 3)] : 0;
                    for (int channel = 0; channel < 3; ++channel) {
                        if (steps && material->shading == XG_RENDER_IR_SHADING_GOURAUD)
                            color[channel] += (end_color[channel] - color[channel]) * step / steps;
                        color[channel] = native_clamp5((color[channel] + phase) >> 3);
                    }
                    const uint16_t word = native_psx_fragment_word(*destination,
                        color[0], color[1], color[2], material->mask_set,
                        material->semi_transparent, material->blend_mode);
                    *destination = conv_1555_to_rgba8(word);
                    if (target->depth && !material->semi_transparent)
                        target->depth[destination - target->pixels] = 0u;
                    if (target->words) {
                        const size_t index = destination - target->pixels;
                        target->words[index] = word;
                        if (compiler->native_views) {
                            compiler->native_views->word_versions[index / VRAM_W][(index % VRAM_W) / 64u] = compiler->native_views->write_serial;
                            compiler->native_views->word_page_versions[index / (VRAM_W * 256u)][(index % VRAM_W) / 64u] = compiler->native_views->write_serial;
                        }
                        if (compiler->gpu) {
                            compiler->gpu->dirty_word_rows[index / VRAM_W] |= 1u << ((index % VRAM_W) / 64u);
                            compiler->gpu->dirty_texture_pages[index / (VRAM_W * 256u)] |= 1u << ((index % VRAM_W) / 64u);
                        }
                    }
                    if (compiler->raster_words && !compiler->view_pass)
                        gpu_vram_region_mark_rect(compiler->raster_words, x, y, x, y);
                    compiler->audit->rendered_draw_pixels++;
                }
                if (x == end_x && y == end_y) break;
                const int twice_error = error * 2;
                if (twice_error >= -dy) { error -= dy; x += step_x; }
                if (twice_error <= dx) { error += dx; y += step_y; }
            }
        }
        compiler->audit->rendered_draws++;
        return 1;
    }
    const GlNativeViewState *transition_views = compiler->view_pass ? compiler->native_views : NULL;
    const int transition = !canonical && transition_views ? native_transition_texture(transition_views, draw) : -1;
    if (material->textured) {
        texture_resource = compiler->native_texture ? compiler->native_texture : native_audit_resource(
            compiler->audit, draw->texture_resource_id,
            draw->texture_generation);
        if (!texture_resource ||
            !native_pixel_descriptor_valid(texture_resource, 0) ||
            (texture_resource->view.kind != XG_RENDER_RESOURCE_TEXTURE &&
             texture_resource->view.kind !=
                  XG_RENDER_RESOURCE_GENERATED_SURFACE &&
              texture_resource->view.kind != XG_RENDER_RESOURCE_FRAMEBUFFER &&
              texture_resource->view.kind != XG_RENDER_RESOURCE_MOVIE_FRAME) ||
            (texture_resource->view.descriptor.pixel_format ==
                 XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 &&
             material->texture_depth != XG_RENDER_IR_TEXTURE_4_BIT) ||
            (texture_resource->view.descriptor.pixel_format ==
                 XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8 &&
             material->texture_depth != XG_RENDER_IR_TEXTURE_8_BIT)) {
            native_audit_block(compiler->audit,
                GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR, record_index,
                draw->texture_resource_id, draw->texture_generation);
            return 0;
        }
        if (material->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT) {
            clut_resource = compiler->native_clut ? compiler->native_clut : native_audit_resource(
                compiler->audit, draw->clut_resource_id,
                draw->clut_generation);
            if (!clut_resource ||
                !native_pixel_descriptor_valid(clut_resource, 0) ||
                clut_resource->view.kind != XG_RENDER_RESOURCE_CLUT ||
                clut_resource->view.descriptor.pixel_format !=
                    XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555) {
                native_audit_block(compiler->audit,
                    GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR, record_index,
                    draw->clut_resource_id, draw->clut_generation);
                return 0;
            }
        }
    }
    /* These handles were validated once above. Do not repeat generic resource
     * dispatch or RGBA conversion for each Native texture and palette access. */
    const uint16_t *native_words = canonical ? target->words : texture_resource == compiler->native_texture &&
        (material->texture_depth == XG_RENDER_IR_TEXTURE_15_BIT || clut_resource == compiler->native_clut)
        ? compiler->native_words : NULL;
    const XgRenderResourceDescriptor *target_descriptor = target->resource ? &target->resource->view.descriptor : NULL;
    const int native_target = canonical || (compiler->native_vram && target->pixels && target_descriptor &&
        !compiler->viewport_x && !compiler->viewport_y && target->width <= VRAM_W && target->height <= VRAM_H &&
        target_descriptor->width == target->width && target_descriptor->height == target->height &&
        (!(target_descriptor->flags & XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) ||
         (!target_descriptor->vram_x &&
          target_descriptor->vram_width == target->width && target_descriptor->vram_height == target->height)));
    const int native_origin_y = !canonical && native_target &&
        (target_descriptor->flags & XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) ? target_descriptor->vram_y : 0;
    const int canonical_rows = canonical || (native_target && target == compiler->native_vram &&
        target->words && target->width == VRAM_W && native_origin_y == 0);
    for (uint32_t ti = 0u; ti < primitive->triangle_count; ++ti) {
        const XgRenderIrTriangle *triangle = &primitive->triangles[ti];
        const int flat = material->shading == XG_RENDER_IR_SHADING_FLAT;
        const int no_dither = !material->dither || compiler->dithering_disabled;
        const int direct_texel = material->raw_texture || (flat && no_dither &&
            triangle->vertices[0].r == 128u && triangle->vertices[0].g == 128u &&
            triangle->vertices[0].b == 128u);
        const int filter_strength = draw->sprite_texture ? 100 :
            gl_renderer_scene_filter_strength();
        const int filter_draw = !canonical && material->textured && target != compiler->native_vram &&
            filter_strength > 0 && (draw->sprite_texture ? s_sprite_filter : s_tex_filter);
        const int anisotropy = !canonical && material->textured && target != compiler->native_vram &&
            !draw->sprite_texture &&
            draw->screen_space_2d == GPU_RENDER_SCREEN_SPACE_2D_NONE ?
            gl_renderer_anisotropy() : 0;
        const int constant_fragment = !material->textured && flat && no_dither &&
            !material->semi_transparent;
        const uint16_t constant_word = (uint16_t)((triangle->vertices[0].r >> 3u) |
            ((triangle->vertices[0].g >> 3u) << 5u) |
            ((triangle->vertices[0].b >> 3u) << 10u) |
            (material->mask_set ? UINT16_C(0x8000) : 0u));
        double px[3], py[3], area;
        double edges[3][3];
        int integral_edges = 1;
        int top_left[3];
        /* The canonical VRAM raster is the guest-visible surface and keeps the
         * faithful affine sampling. Every presentation surface (native view,
         * recipe target, compatibility pass) takes the perspective payload when
         * the triangle carries it, matching the GPU shader path. */
        int perspective = !canonical && (compiler->native_vram == NULL ||
                           target != compiler->native_vram) &&
            material->textured &&
            native_triangle_all_projective(triangle);
        int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
        if (triangle->split_index != ti ||
            triangle->split_count != primitive->triangle_count) {
            native_audit_block(compiler->audit,
                           GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           record_index, 0u, 0u);
            return 0;
        }
        for (int vi = 0; vi < 3; ++vi) {
            const XgRenderIrVertex *vertex = &triangle->vertices[vi];
            int32_t position_x = !compiler->native_vram && vertex->native_view_position
                ? vertex->native_view_x : vertex->x;
            int32_t position_y = !compiler->native_vram && vertex->native_view_position
                ? vertex->native_view_y : vertex->y;
            integral_edges &= (((uint32_t)position_x | (uint32_t)position_y) & UINT32_C(0xffff)) == 0u;
            int ix, iy;
            if (!compiler->native_vram &&
                ((vertex->projective_position ? 1 : 0) != perspective ||
                 (perspective && vertex->projective_view_z <= 0))) {
                native_audit_block(compiler->audit,
                               GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                               record_index, 0u, 0u);
                return 0;
            }
            px[vi] = (double)position_x / 65536.0 + material->draw_offset_x;
            py[vi] = (double)position_y / 65536.0 + material->draw_offset_y;
            ix = native_fixed_floor(position_x) + material->draw_offset_x;
            iy = native_fixed_floor(position_y) + material->draw_offset_y;
            if (ix < min_x) min_x = ix;
            if (ix > max_x) max_x = ix;
            if (iy < min_y) min_y = iy;
            if (iy > max_y) max_y = iy;
        }
        if (min_x < material->draw_area_left) min_x = material->draw_area_left;
        if (min_y < material->draw_area_top) min_y = material->draw_area_top;
        if (max_x > material->draw_area_right) max_x = material->draw_area_right;
        if (max_y > material->draw_area_bottom) max_y = material->draw_area_bottom;
        if (native_target) {
            /* Same accepted integer pixel domain as surface_pixel + viewport,
             * with unchanged edge/UV equations. No per-fragment descriptor
             * lookup or pointer-to-XY division is needed on these owned planes. */
            const uint32_t width = target->width < compiler->viewport_width ? target->width : compiler->viewport_width;
            const uint32_t height = target->height < compiler->viewport_height ? target->height : compiler->viewport_height;
            if (min_x < 0) min_x = 0;
            if (min_y < native_origin_y) min_y = native_origin_y;
            if (max_x >= (int)width) max_x = (int)width - 1;
            if (max_y >= native_origin_y + (int)height) max_y = native_origin_y + (int)height - 1;
        }
        if (compiler->banded) {
            /* Disjoint rows of one ordered raster: edge/UV equations and
             * absolute dither coordinates are unchanged, so every pixel gets
             * exactly the fragments of the single-thread reference. */
            if (min_y < compiler->band_top) min_y = compiler->band_top;
            if (max_y > compiler->band_bottom) max_y = compiler->band_bottom;
            if (min_y > max_y) continue;
        }
        /* No canonical pixel survives clipping. VIEW was submitted separately;
         * avoid attribute setup and a redundant, fully clipped GPU command. */
        if ((min_x > max_x || min_y > max_y) &&
            (!compiler->gpu || (!compiler->view_pass && target == compiler->native_vram)))
            continue;
        area = (px[1] - px[0]) * (py[2] - py[0]) -
               (py[1] - py[0]) * (px[2] - px[0]);
        if (area == 0.0) continue;
        if (target->words && !native_view_worker_drain(compiler, min_x, min_y, max_x, max_y)) return 0;
        GlNativeAttributePlanes attributes;
        /* Constant untextured fragments go directly to native_write_fragment.
         * Their CPU raster consumes no UV or colour interpolation planes. */
        if (!constant_fragment)
            native_attribute_planes(triangle, px, py, 1u,
                material->shading == XG_RENDER_IR_SHADING_GOURAUD, &attributes);
        const GlNativeDepthMode depth_mode = !canonical && target->depth
            ? native_depth_mode(draw, triangle, px, py) : (GlNativeDepthMode){0};
        const double orientation = area < 0.0 ? -1.0 : 1.0;
        area *= orientation;
        for (int vi = 0; vi < 3; ++vi) {
            const int a = (vi + 1) % 3;
            const int b = (vi + 2) % 3;
            edges[vi][0] = (py[a] - py[b]) * orientation;
            edges[vi][1] = (px[b] - px[a]) * orientation;
            edges[vi][2] = (px[a] * py[b] - py[a] * px[b]) * orientation;
            /* Y grows downwards. Reversing a shared edge reverses ownership. */
            top_left[vi] = edges[vi][0] > 0.0 ||
                (edges[vi][0] == 0.0 && edges[vi][1] > 0.0);
        }
        if (compiler->gpu) {
            /* A later triangle may read words written by the earlier one.
             * Snapshot its actual texture dependencies at this FIFO boundary. */
            XgSemanticDrawRecord captured = *draw;
            captured.primitive.triangle_count = 1u;
            captured.primitive.triangles[0] = *triangle;
            captured.primitive.triangles[0].split_index = 0u;
            captured.primitive.triangles[0].split_count = 1u;
            if (!native_gpu_draw(compiler, &captured, native_origin_y)) return 0;
        }
        /* Skip the exterior of wide triangle bounding boxes. The bounds are
         * deliberately expanded by a pixel; the original edge equations below
         * still decide coverage, including fractional VIEW and top-left ties.
         * Q16 coordinates make nonzero edge slopes at least 1/65536, so double
         * rounding in these intersections is far below the one-pixel guard. */
        double edge_inverse[3];
        const int trim_rows = canonical || max_x - min_x >= 16;
        if (trim_rows)
            for (int e = 0; e < 3; ++e)
                edge_inverse[e] = edges[e][0] ? 1.0 / edges[e][0] : 0.0;
        const unsigned attribute_begin = material->textured && !perspective ? 0u : 2u;
        const unsigned attribute_end = !material->raw_texture &&
            material->shading == XG_RENDER_IR_SHADING_GOURAUD ? 5u : 2u;
        double perspective_reciprocal[3] = {0};
        if (perspective)
            for (unsigned v = 0u; v < 3u; ++v)
                perspective_reciprocal[v] = 1.0 / triangle->vertices[v].projective_view_z;
        for (int y = min_y; y <= max_y; ++y) {
            uint16_t written_blocks = 0u;
            int row_min = min_x, row_max = max_x;
            if (trim_rows) {
                const double sy = y + 1.0 / 64.0;
                for (int e = 0; e < 3; ++e) {
                    const double boundary = -(edges[e][1] * sy + edges[e][2]) *
                        edge_inverse[e] - 1.0 / 64.0;
                    if (edges[e][0] > 0.0 && boundary > row_min) {
                        if (boundary > max_x + 1.0) { row_min = max_x + 1; break; }
                        row_min = (int)floor(boundary);
                    } else if (edges[e][0] < 0.0 && boundary < row_max) {
                        if (boundary < min_x - 1.0) { row_max = min_x - 1; break; }
                        row_max = (int)ceil(boundary);
                    }
                }
            }
            /* Integer vertices produce edge values in exact multiples of
             * 1/64, well within double's integer precision for Q16 positions.
             * Advance those edges by addition; fractional VIEW geometry keeps
             * its original evaluation, avoiding accumulated rounding changes. */
            double row_edges[3];
            for (int e = 0; e < 3; ++e)
                row_edges[e] = edges[e][0] * (row_min + 1.0 / 64.0) +
                    edges[e][1] * (y + 1.0 / 64.0) + edges[e][2];
            if (canonical) {
                /* Coverage is an intersection of three linear half-spaces.
                 * Integer-vertex edges are exact in double here. Refine the
                 * conservative row bounds with the original top-left rule;
                 * every pixel between the resulting endpoints is then inside.
                 * This removes three edge tests from every covered fragment. */
                for (int e = 0; e < 3 && row_min <= row_max; ++e) {
                    if (edges[e][0] == 0.0 &&
                        (row_edges[e] < 0.0 || (row_edges[e] == 0.0 && !top_left[e]))) {
                        row_min = row_max + 1;
                        break;
                    }
                    if (edges[e][0] > 0.0) {
                        double edge = edges[e][0] * (row_min + 1.0 / 64.0) +
                            edges[e][1] * (y + 1.0 / 64.0) + edges[e][2];
                        while (row_min <= row_max && (edge < 0.0 || (edge == 0.0 && !top_left[e]))) {
                            ++row_min; edge += edges[e][0];
                        }
                    } else if (edges[e][0] < 0.0) {
                        double edge = edges[e][0] * (row_max + 1.0 / 64.0) +
                            edges[e][1] * (y + 1.0 / 64.0) + edges[e][2];
                        while (row_min <= row_max && (edge < 0.0 || (edge == 0.0 && !top_left[e]))) {
                            --row_max; edge -= edges[e][0];
                        }
                    }
                }
            }
            uint32_t row_attributes[5];
            if (canonical && !constant_fragment)
                for (unsigned a = 0u; a < 5u; ++a)
                    row_attributes[a] = attributes.dda[a][0] +
                        attributes.dda[a][1] * (uint32_t)(row_min - (int)attributes.x) +
                        attributes.dda[a][2] * (uint32_t)(y - (int)attributes.y);
            for (int x = row_min; x <= row_max; ++x,
                    row_edges[0] += edges[0][0], row_edges[1] += edges[1][0],
                    row_edges[2] += edges[2][0],
                    (canonical && !constant_fragment ?
                        native_attribute_step(row_attributes, &attributes) : (void)0)) {
                const double sx = x + 1.0 / 64.0;
                const double sy = y + 1.0 / 64.0;
                const double e0 = (canonical || integral_edges) ? row_edges[0] :
                    edges[0][0] * sx + edges[0][1] * sy + edges[0][2];
                const double e1 = (canonical || integral_edges) ? row_edges[1] :
                    edges[1][0] * sx + edges[1][1] * sy + edges[1][2];
                const double e2 = (canonical || integral_edges) ? row_edges[2] :
                    edges[2][0] * sx + edges[2][1] * sy + edges[2][2];
                uint32_t *destination;
                uint16_t word;
                int color8[3], color5[3], mask, blend;
                if (!canonical && (e0 < 0.0 || (e0 == 0.0 && !top_left[0]) ||
                    e1 < 0.0 || (e1 == 0.0 && !top_left[1]) ||
                    e2 < 0.0 || (e2 == 0.0 && !top_left[2])))
                    continue;
                if (native_target) destination = target->pixels + (size_t)(y - native_origin_y) * target->width + x;
                else if (!native_surface_pixel(target, x, y, &destination) ||
                    !native_pixel_in_viewport(compiler, target, destination)) continue;
                if (material->mask_check && (*destination >> 24u) != 0u)
                    continue;
                uint32_t *const depth_cell = !canonical && target->depth ? target->depth + (destination - target->pixels) : NULL;
                uint32_t fragment_depth = 0u;
                int blended = 0; /* this fragment actually blends: it keeps the stored key */
                if (depth_cell && depth_mode.test) {
                    fragment_depth = native_depth_at(&depth_mode.plane, x, y);
                    compiler->audit->depth_tested_fragments++;
                    if (!native_depth_pass(fragment_depth, depth_mode.bias, *depth_cell)) {
                        compiler->audit->depth_rejected_fragments++;
                        continue;
                    }
                }
                if (constant_fragment) {
                    word = constant_word;
                    goto native_write_fragment;
                }
                double sampled[5];
                double continuous_u = 0.0, continuous_v = 0.0;
                /* Flat RGB is constant, raw textures ignore RGB, and perspective
                 * UV replaces the affine result. Keep the remaining planes exact. */
                color8[0] = triangle->vertices[0].r;
                color8[1] = triangle->vertices[0].g;
                color8[2] = triangle->vertices[0].b;
                if (canonical) {
                    /* Unsigned Q12 addition is exactly the original multiply
                     * modulo 2^32. Step even across uncovered/transparent
                     * pixels, so the sample remains tied to its absolute X. */
                    if (material->textured) {
                        sampled[0] = (row_attributes[0] >> 12u) & 255u;
                        sampled[1] = (row_attributes[1] >> 12u) & 255u;
                    }
                    if (!material->raw_texture && material->shading == XG_RENDER_IR_SHADING_GOURAUD) {
                        color8[0] = (row_attributes[2] >> 12u) & 255u;
                        color8[1] = (row_attributes[3] >> 12u) & 255u;
                        color8[2] = (row_attributes[4] >> 12u) & 255u;
                    }
                } else for (unsigned a = attribute_begin; a < attribute_end; ++a) {
                    if (canonical || attributes.integral) {
                        const uint32_t q = attributes.dda[a][0] +
                            attributes.dda[a][1] * (uint32_t)(x - (int)attributes.x) +
                            attributes.dda[a][2] * (uint32_t)(y - (int)attributes.y);
                        const int value = (q & UINT32_C(0xfffff)) >> 12u;
                        if (a < 2u) {
                            sampled[a] = value;
                            if (filter_draw || anisotropy > 1) {
                                const double phase = (q & UINT32_C(0xfffff)) / 4096.0;
                                if (a == 0u) continuous_u = phase;
                                else continuous_v = phase;
                            }
                        }
                        else color8[a - 2u] = value;
                    } else {
                        /* Fractional VIEW geometry uses its actual plane, not
                         * an invented PS1 fractional-coordinate instruction. */
                        const double phase = attributes.plane[a][0] +
                            attributes.plane[a][1] * (x - attributes.x) +
                            attributes.plane[a][2] * (y - attributes.y);
                        sampled[a] = floor(phase);
                        if (a < 2u && (filter_draw || anisotropy > 1)) {
                            if (a == 0u) continuous_u = phase;
                            else continuous_v = phase;
                        }
                        if (a >= 2u)
                            color8[a - 2u] = (int)fmax(0.0, fmin(255.0, sampled[a]));
                    }
                }
                mask = material->mask_set ? 1 : 0;
                blend = material->semi_transparent ? 1 : 0;
                if (material->textured) {
                    uint16_t texel;
                    if (perspective) {
                        const double w0 = (edges[0][0]*x + edges[0][1]*y + edges[0][2]) / area;
                        const double w1 = (edges[1][0]*x + edges[1][1]*y + edges[1][2]) / area;
                        const double w2 = (edges[2][0]*x + edges[2][1]*y + edges[2][2]) / area;
                        const double q0 = perspective_reciprocal[0];
                        const double q1 = perspective_reciprocal[1];
                        const double q2 = perspective_reciprocal[2];
                        double denominator = w0 * q0 + w1 * q1 + w2 * q2;
                        if (denominator <= 0.0) continue;
                        const double u = (w0 * triangle->vertices[0].u * q0 +
                             w1 * triangle->vertices[1].u * q1 +
                             w2 * triangle->vertices[2].u * q2) / denominator;
                        const double v = (w0 * triangle->vertices[0].v * q0 +
                             w1 * triangle->vertices[1].v * q1 +
                             w2 * triangle->vertices[2].v * q2) / denominator;
                        sampled[0] = floor(u / 65536.0 + 0.5);
                        sampled[1] = floor(v / 65536.0 + 0.5);
                        continuous_u = u / 65536.0;
                        continuous_v = v / 65536.0;
                    }
                    /* floor() produced integral texcoords. For values fitting
                     * int, the sampler's low-eight-bit wrap is exactly fmod(256),
                     * including negative coordinates, without a floating divide. */
                    int sample_u = canonical || (attributes.integral && !perspective) ||
                        (sampled[0] >= INT_MIN && sampled[0] <= INT_MAX)
                        ? (int)sampled[0] : (int)fmod(sampled[0], 256.0);
                    int sample_v = canonical || (attributes.integral && !perspective) ||
                        (sampled[1] >= INT_MIN && sampled[1] <= INT_MAX)
                        ? (int)sampled[1] : (int)fmod(sampled[1], 256.0);
                    if ((material->texture_window_mask_x |
                         material->texture_window_mask_y) == 0u) {
                        int su = sample_u & 255, sv = sample_v & 255;
                        su = su < attributes.limits[0] ? attributes.limits[0] :
                             su > attributes.limits[2] ? attributes.limits[2] : su;
                        sv = sv < attributes.limits[1] ? attributes.limits[1] :
                             sv > attributes.limits[3] ? attributes.limits[3] : sv;
                        sample_u = su; sample_v = sv;
                    }
                    if (native_words) {
                        texel = native_native_texel(native_words, material, sample_u, sample_v);
                    } else if (!native_sample_draw_texel(compiler, draw,
                            texture_resource, clut_resource,
                            sample_u, sample_v, &texel)) {
                        native_audit_block(compiler->audit,
                            GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR,
                            record_index, draw->texture_resource_id,
                            draw->texture_generation);
                        return 0;
                    }
                    /* The CPU VIEW endpoint is also used at render scale 1.
                     * Its screenshot must sample the same full-width domain
                     * as the GPU, rather than the canonical texture crop. */
                    if (transition >= 0) {
                        const GlNativeViewState *views = transition_views;
                        const GlNativeViewTarget *source = &views->targets[transition];
                        int sx = (int)floor((continuous_u + 0.5 - 1.0 / 64.0 +
                            material->texture_page_x * 64 - source->x) * views->width / source->width);
                        int sy = (int)floor(continuous_v + 0.5 - 1.0 / 64.0);
                        sx = sx < 0 ? 0 : sx >= (int)views->width ? (int)views->width - 1 : sx;
                        sy = sy < 0 ? 0 : sy >= source->height ? source->height - 1 : sy;
                        if (source->pixels) texel = (texel & UINT16_C(0x8000)) |
                            (native_rgba_to_1555(source->pixels[(size_t)sy * views->width + sx]) & UINT16_C(0x7fff));
                    }
                    if (texel == 0u) continue;
                    /* Nearest sampling has integral RGB555 channels. Preserve
                     * the two original truncations (modulation /16, then /8
                     * after dithering), without constructing floating-point
                     * filter taps for every canonical fragment. */
                    if (!filter_draw && anisotropy <= 1) {
                        if (direct_texel) {
                            word = texel | (material->mask_set ? UINT16_C(0x8000) : 0u);
                            if (material->semi_transparent && (texel & UINT16_C(0x8000))) {
                                word = native_psx_fragment_word(*destination,
                                    texel & 31u, (texel >> 5u) & 31u, (texel >> 10u) & 31u,
                                    1, 1, material->blend_mode);
                                blended = 1;
                            }
                            goto native_write_fragment;
                        }
                        color5[0] = ((texel & 31u) * (unsigned)color8[0]) >> 4u;
                        color5[1] = (((texel >> 5u) & 31u) * (unsigned)color8[1]) >> 4u;
                        color5[2] = (((texel >> 10u) & 31u) * (unsigned)color8[2]) >> 4u;
                        mask |= (texel >> 15u) & 1u;
                        blend &= (texel >> 15u) & 1u;
                        goto native_quantize_fragment;
                    }
                    double filtered5[3] = {
                        texel & 31u, (texel >> 5u) & 31u, (texel >> 10u) & 31u,
                    };
                    int filter_pixel = transition < 0 && filter_draw;
                    const double shifted_u = continuous_u +
                        (perspective ? 0.5 - 1.0 / 64.0 : 0.0);
                    const double shifted_v = continuous_v +
                        (perspective ? 0.5 - 1.0 / 64.0 : 0.0);
                    if (transition < 0 && filter_draw) {
                        filtered5[0] = filtered5[1] = filtered5[2] = 0.0;
                        /* Nearest is authoritative for coverage/STP. Clamp the
                         * other three taps to this primitive's sampled texels
                         * before applying the PSX texture window/CLUT. */
                        double fx = shifted_u - floor(shifted_u) - 0.5;
                        double fy = shifted_v - floor(shifted_v) - 0.5;
                        const int du = fx < 0.0 ? -1 : 1;
                        const int dv = fy < 0.0 ? -1 : 1;
                        fx = fabs(fx); fy = fabs(fy);
                        const double weights[4] = {
                            (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
                            (1.0 - fx) * fy, fx * fy,
                        };
                        uint16_t taps[4] = {texel, 0, 0, 0};
                        double total = weights[0];
                        for (int tap = 1; tap < 4; ++tap) {
                            int tu = ((sample_u & 255) + ((tap & 1) ? du : 0)) & 255;
                            int tv = ((sample_v & 255) + ((tap & 2) ? dv : 0)) & 255;
                            if (!(material->texture_window_mask_x |
                                  material->texture_window_mask_y)) {
                                if (tu < attributes.limits[0]) tu = attributes.limits[0];
                                if (tu > attributes.limits[2]) tu = attributes.limits[2];
                                if (tv < attributes.limits[1]) tv = attributes.limits[1];
                                if (tv > attributes.limits[3]) tv = attributes.limits[3];
                            }
                            if (native_words) taps[tap] = native_native_texel(
                                native_words, material, tu, tv);
                            else if (!native_sample_draw_texel(compiler, draw,
                                     texture_resource, clut_resource, tu, tv,
                                     &taps[tap])) {
                                native_audit_block(compiler->audit,
                                    GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR,
                                    record_index, draw->texture_resource_id,
                                    draw->texture_generation);
                                return 0;
                            }
                            if (taps[tap]) total += weights[tap];
                        }
                        for (int tap = 0; tap < 4; ++tap) if (taps[tap])
                            for (int channel = 0; channel < 3; ++channel)
                                filtered5[channel] += ((taps[tap] >> (channel * 5)) & 31) *
                                    weights[tap] / total;
                    }
                    if (transition < 0 && anisotropy > 1) {
                        double ux = attributes.plane[0][1], vx = attributes.plane[1][1];
                        double uy = attributes.plane[0][2], vy = attributes.plane[1][2];
                        if (perspective) {
                            double w[3], reciprocal[3], denominator = 0.0;
                            for (int i = 0; i < 3; ++i) {
                                w[i] = (edges[i][0]*x + edges[i][1]*y + edges[i][2]) / area;
                                reciprocal[i] = perspective_reciprocal[i];
                                denominator += w[i] * reciprocal[i];
                            }
                            if (denominator > 0.0) {
                                double den_dx = 0.0, den_dy = 0.0;
                                ux = vx = uy = vy = 0.0;
                                for (int i = 0; i < 3; ++i) {
                                    const double sx = edges[i][0] * reciprocal[i] / area;
                                    const double sy = edges[i][1] * reciprocal[i] / area;
                                    const double u = triangle->vertices[i].u / 65536.0;
                                    const double v = triangle->vertices[i].v / 65536.0;
                                    den_dx += sx; den_dy += sy;
                                    ux += sx*u; vx += sx*v;
                                    uy += sy*u; vy += sy*v;
                                }
                                ux = (ux - continuous_u * den_dx) / denominator;
                                vx = (vx - continuous_v * den_dx) / denominator;
                                uy = (uy - continuous_u * den_dy) / denominator;
                                vy = (vy - continuous_v * den_dy) / denominator;
                            }
                        }
                        double span_u = 0.0, span_v = 0.0;
                        const int samples = native_anisotropic_span(ux, vx, uy, vy,
                            anisotropy, &span_u, &span_v);
                        if (samples > 1) {
                            filter_pixel = 1;
                            double sum[3] = {0.0, 0.0, 0.0};
                            int valid = 0;
                            for (int i = 0; i < samples; ++i) {
                                const double offset = ((i + 0.5) / samples) - 0.5;
                                double color[3] = {0.0, 0.0, 0.0};
                                int sampled_valid = 0;
                                int fetched = 1;
                                if (filter_draw) {
                                    fetched = native_anisotropic_tap(compiler, draw,
                                        texture_resource, clut_resource, native_words,
                                        &attributes, shifted_u + offset*span_u,
                                        shifted_v + offset*span_v, color, &sampled_valid);
                                } else {
                                    uint16_t tap;
                                    fetched = native_filter_tap(compiler, draw,
                                        texture_resource, clut_resource, native_words,
                                        &attributes, (int)floor(shifted_u + offset*span_u),
                                        (int)floor(shifted_v + offset*span_v), &tap);
                                    if (fetched && tap) {
                                        for (int channel = 0; channel < 3; ++channel)
                                            color[channel] = (tap >> (channel * 5)) & 31;
                                        sampled_valid = 1;
                                    }
                                }
                                if (!fetched) {
                                    native_audit_block(compiler->audit,
                                        GL_RENDERER_NATIVE_BLOCKER_TEXTURE_DESCRIPTOR,
                                        record_index, draw->texture_resource_id,
                                        draw->texture_generation);
                                    return 0;
                                }
                                if (!sampled_valid) continue;
                                for (int channel = 0; channel < 3; ++channel)
                                    sum[channel] += color[channel];
                                ++valid;
                            }
                            if (valid) for (int channel = 0; channel < 3; ++channel)
                                filtered5[channel] = sum[channel] / valid;
                        }
                    }
                    /* Raw texture and undithered neutral modulation preserve
                     * RGB555 exactly. Keep the texel's mask/blend bit, without
                     * unpacking/modulating/clamping/repacking opaque pixels. */
                    if (direct_texel && !filter_pixel) {
                        word = texel | (material->mask_set ? UINT16_C(0x8000) : 0u);
                        if (material->semi_transparent && (texel & UINT16_C(0x8000))) {
                            word = native_psx_fragment_word(*destination,
                                texel & 31u, (texel >> 5u) & 31u, (texel >> 10u) & 31u,
                                1, 1, material->blend_mode);
                            blended = 1;
                        }
                        goto native_write_fragment;
                    }
                    for (int channel = 0; channel < 3; ++channel) {
                        const double nearest5 = (double)((texel >> (channel * 5)) & 31);
                        const double tex5 = !filter_pixel ? nearest5 :
                            !filter_draw || filter_strength == 100 ? filtered5[channel] :
                            nearest5 + (filtered5[channel] - nearest5) * filter_strength / 100.0;
                        color5[channel] = material->raw_texture
                            ? (int)tex5
                            : (int)(tex5 * color8[channel] / 16.0);
                    }
                    mask |= (texel >> 15u) & 1u;
                    blend &= (texel >> 15u) & 1u;
                } else {
                    for (int channel = 0; channel < 3; ++channel)
                        color5[channel] = color8[channel];
                }
native_quantize_fragment:
                if (!material->textured || !material->raw_texture) {
                    int phase = material->dither && !compiler->dithering_disabled ? dither[
                        (((y + compiler->dither_y) & 3) << 2) | ((x + compiler->dither_x) & 3)] : 0;
                    for (int channel = 0; channel < 3; ++channel)
                        color5[channel] = native_clamp5(
                            (color5[channel] + phase) >> 3u);
                }
                /* RGB is already clamped above (or comes from a raw RGB555
                 * texel). Opaque fragments need neither a destination read nor
                 * the blend dispatch and its second set of clamps. */
                word = blend ? native_psx_fragment_word(*destination,
                    color5[0], color5[1], color5[2], mask, blend,
                    material->blend_mode) : (uint16_t)(color5[0] |
                        (color5[1] << 5u) | (color5[2] << 10u) | (mask << 15u));
                blended = blend;
native_write_fragment:
                *destination = conv_1555_to_rgba8(word);
                /* Per texel: blended fragments keep the key; opaque texels of a
                 * semi-transparent material write like any opaque fragment. */
                if (depth_cell && !blended) {
                    if (depth_mode.write == GL_NATIVE_DEPTH_WRITE) {
                        *depth_cell = fragment_depth;
                        compiler->audit->depth_written_fragments++;
                        if (!compiler->audit->depth_key_min || fragment_depth < compiler->audit->depth_key_min)
                            compiler->audit->depth_key_min = fragment_depth;
                        if (fragment_depth > compiler->audit->depth_key_max)
                            compiler->audit->depth_key_max = fragment_depth;
                    } else if (*depth_cell != 0u) {
                        *depth_cell = 0u;
                        compiler->audit->depth_reset_fragments++;
                    }
                }
                if (target->words) {
                    const size_t index = destination - target->pixels;
                    target->words[index] = word;
                    if (canonical_rows) written_blocks |= (uint16_t)(1u << (x >> 6));
                    else {
                        if (compiler->native_views) {
                            compiler->native_views->word_versions[index / VRAM_W][(index % VRAM_W) / 64u] = compiler->native_views->write_serial;
                            compiler->native_views->word_page_versions[index / (VRAM_W * 256u)][(index % VRAM_W) / 64u] = compiler->native_views->write_serial;
                        }
                        if (compiler->gpu) {
                            compiler->gpu->dirty_word_rows[index / VRAM_W] |= 1u << ((index % VRAM_W) / 64u);
                            compiler->gpu->dirty_texture_pages[index / (VRAM_W * 256u)] |= 1u << ((index % VRAM_W) / 64u);
                        }
                    }
                }
                if (compiler->raster_words && !compiler->view_pass) {
                    if (canonical_rows) {
                        uint64_t *bits = &compiler->raster_words->rows[y][x >> 6];
                        if (!*bits) compiler->raster_words->nonzero_words++;
                        *bits |= UINT64_C(1) << (x & 63);
                    } else gpu_vram_region_mark_rect(compiler->raster_words, x, y, x, y);
                }
                compiler->audit->rendered_draw_pixels++;
            }
            /* Readers observe versions at draw boundaries, not between texels
             * of this row. Publish each actually written block only once. */
            if (written_blocks) {
                if (compiler->gpu) {
                    compiler->gpu->dirty_word_rows[y] |= written_blocks;
                    compiler->gpu->dirty_texture_pages[y >> 8] |= written_blocks;
                }
                if (compiler->native_views)
                    for (unsigned b = 0u; b < GPU_VRAM_REGION_WORDS_PER_ROW; ++b)
                        if (written_blocks & (1u << b)) {
                            compiler->native_views->word_versions[y][b] = compiler->native_views->write_serial;
                            compiler->native_views->word_page_versions[y >> 8][b] = compiler->native_views->write_serial;
                        }
            }
        }
    }
    compiler->audit->rendered_draws++;
    return 1;
}

/* Specialize the same raster equations for the common canonical integer
 * surface. The constant argument lets the compiler remove VIEW/depth/filter
 * branches from its pixel loop without maintaining a second raster algorithm. */
static int native_render_draw(GlNativeCpuCompiler *compiler,
        GlNativeCpuSurface *target, const XgSemanticDrawRecord *draw,
        uint32_t record_index) {
    if (compiler->gpu && compiler->gpu->canonical_gpu && target == compiler->native_vram) {
        /* Host enhancement packets belong to VIEW, never to guest VRAM. */
        if (compiler->gpu_only) return 1;
        if (draw->topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
            (!draw->primitive.triangle_count || draw->primitive.triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY)) {
            native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                record_index, 0u, 0u);
            return 0;
        }
        const unsigned count = draw->topology == GPU_RENDER_SEMANTIC_TRIANGLES
            ? draw->primitive.triangle_count : 1u;
        for (unsigned t = 0u; t < count; ++t) {
            XgSemanticDrawRecord part = *draw;
            if (draw->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
                part.primitive.triangle_count = 1u;
                part.primitive.triangles[0] = draw->primitive.triangles[t];
                part.primitive.triangles[0].split_index = 0u;
                part.primitive.triangles[0].split_count = 1u;
            }
            compiler->gpu_only = 1;
            const int ok = native_render_draw_impl(compiler, target, &part, record_index, 0);
            compiler->gpu_only = 0;
            if (!ok) return 0;
            const XgRenderIrMaterialState *m = &part.primitive.material;
            int left = VRAM_W, top = VRAM_H, right = -1, bottom = -1;
            const unsigned vertices = part.topology == GPU_RENDER_SEMANTIC_TRIANGLES ? 3u : part.line_count * 2u;
            for (unsigned v = 0u; v < vertices; ++v) {
                const int32_t x = part.topology == GPU_RENDER_SEMANTIC_TRIANGLES
                    ? part.primitive.triangles[0].vertices[v].x : part.lines[v / 2u].vertices[v % 2u].x;
                const int32_t y = part.topology == GPU_RENDER_SEMANTIC_TRIANGLES
                    ? part.primitive.triangles[0].vertices[v].y : part.lines[v / 2u].vertices[v % 2u].y;
                const int px = native_fixed_floor(x) + m->draw_offset_x;
                const int py = native_fixed_floor(y) + m->draw_offset_y;
                if (px < left) left = px; if (px > right) right = px;
                if (py < top) top = py; if (py > bottom) bottom = py;
            }
            if (left < m->draw_area_left) left = m->draw_area_left;
            if (top < m->draw_area_top) top = m->draw_area_top;
            if (right > m->draw_area_right) right = m->draw_area_right;
            if (bottom > m->draw_area_bottom) bottom = m->draw_area_bottom;
            if (left < 0) left = 0; if (top < 0) top = 0;
            if (right >= VRAM_W) right = VRAM_W - 1;
            if (bottom >= VRAM_H) bottom = VRAM_H - 1;
            if (left <= right && top <= bottom) {
                if (!compiler->canonical_dirty) {
                    compiler->canonical_left = left; compiler->canonical_top = top;
                    compiler->canonical_right = right; compiler->canonical_bottom = bottom;
                } else {
                    if (left < compiler->canonical_left) compiler->canonical_left = left;
                    if (top < compiler->canonical_top) compiler->canonical_top = top;
                    if (right > compiler->canonical_right) compiler->canonical_right = right;
                    if (bottom > compiler->canonical_bottom) compiler->canonical_bottom = bottom;
                }
                compiler->canonical_dirty = 1;
            }
        }
        compiler->audit->rendered_draws++;
        return 1;
    }
    int canonical = target && compiler->native_vram == target &&
        compiler->native_words == target->words && target->words && target->pixels &&
        compiler->native_texture && compiler->native_clut && !compiler->view_pass &&
        !target->depth && target->width == VRAM_W && target->height == VRAM_H &&
        !compiler->viewport_x && !compiler->viewport_y &&
        compiler->viewport_width == VRAM_W && compiler->viewport_height == VRAM_H &&
        target->resource && target->resource->view.descriptor.width == VRAM_W &&
        target->resource->view.descriptor.height == VRAM_H &&
        (!(target->resource->view.descriptor.flags & XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) ||
         (!target->resource->view.descriptor.vram_x && !target->resource->view.descriptor.vram_y &&
          target->resource->view.descriptor.vram_width == VRAM_W &&
          target->resource->view.descriptor.vram_height == VRAM_H)) &&
        draw->topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
        draw->primitive.triangle_count <= XG_RENDER_IR_TRIANGLE_CAPACITY;
    for (uint32_t t = 0u; canonical && t < draw->primitive.triangle_count; ++t)
        for (unsigned v = 0u; v < 3u; ++v) {
            const XgRenderIrVertex *vertex = &draw->primitive.triangles[t].vertices[v];
            if (((uint32_t)vertex->x | (uint32_t)vertex->y |
                 (uint32_t)vertex->u | (uint32_t)vertex->v) & UINT32_C(0xffff)) canonical = 0;
        }
    if (canonical) return native_render_draw_impl(compiler, target, draw, record_index, 1);
    return native_render_draw_impl(compiler, target, draw, record_index, 0);
}

/* CPU VIEW is independent storage, but samples canonical raw words. The single
 * reader preserves the result of draw order (opaque runs may cull hidden
 * fragments); canonical writes crossing its conservative read set, all
 * transfers/target changes, and transaction retirement must join it. */
#define GL_NATIVE_VIEW_JOB_CAPACITY 4096u
#define GL_NATIVE_VIEW_PUBLISH_JOBS 32u
typedef struct GlNativeViewJob {
    XgSemanticDrawRecord draw;
    GlNativeResourceRecord resource;
    GlNativeCpuSurface surface;
    uint32_t record_index;
    uint8_t dither_x, dither_y;
} GlNativeViewJob;
/* The VIEW reference raster uses the available CPUs. Each band owns
 * disjoint raster rows and replays every job in submission order, so the
 * per-pixel draw order (and the result) is identical to one reader. */
struct GlNativeViewWorker;
typedef struct GlNativeViewBand {
    struct GlNativeViewWorker *owner;
    GlNativeCpuCompiler compiler;
    GlNativeCompileAudit audit;
    SDL_Thread *thread;
    uint32_t index, done;
    int failed;
} GlNativeViewBand;
typedef struct GlNativeViewWorker {
    SDL_mutex *mutex;
    SDL_cond *condition;
    GlNativeViewJob *jobs;
    uint16_t reads[VRAM_H];
    uint16_t read_pages[2], texture_read_pages[2];
    uint32_t count, published, band_count;
    int stop;
    GlNativeViewBand *bands;
} GlNativeViewWorker;
static GlNativeViewWorker *s_native_view_worker;

static int native_view_worker_idle(const GlNativeViewWorker *worker) {
    for (uint32_t i = 0u; i < worker->band_count; ++i)
        if (worker->bands[i].done != worker->count) return 0;
    return 1;
}

static int native_view_worker_main(void *data) {
    GlNativeViewBand *band = data;
    GlNativeViewWorker *worker = band->owner;
    SDL_LockMutex(worker->mutex);
    for (;;) {
        while (band->done == worker->published && !worker->stop)
            SDL_CondWait(worker->condition, worker->mutex);
        if (band->done == worker->published && worker->stop) break;
        const uint32_t begin = band->done, end = worker->published;
        const int bands = (int)worker->band_count;
        SDL_UnlockMutex(worker->mutex);
        /* Published jobs never change until finish has joined this reader.
         * Drain a captured prefix without a mutex round trip per triangle. */
        for (uint32_t i = begin; i < end; ++i) {
            GlNativeViewJob *job = &worker->jobs[i];
            const int origin = (job->resource.view.descriptor.flags &
                                XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION)
                ? (int)job->resource.view.descriptor.vram_y : 0;
            const int height = (int)job->surface.height;
            const int index = (int)band->index;
            band->compiler.viewport_width = job->surface.width;
            band->compiler.viewport_height = job->surface.height;
            band->compiler.dither_x = job->dither_x; band->compiler.dither_y = job->dither_y;
            band->compiler.banded = bands > 1;
            /* Outer bands are open-ended: a band split can move work between
             * threads but can never leave a row uncovered. */
            band->compiler.band_top = index == 0 ? INT_MIN : origin + height * index / bands;
            band->compiler.band_bottom = index == bands - 1 ? INT_MAX
                : origin + height * (index + 1) / bands - 1;
            /* The owner already validated/captured the draw. Keep band zero's
             * full record validation, but don't repeat it on bands outside the
             * scissor. Their pixels cannot be touched by this job. */
            if (index != 0 && job->draw.topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
                ((int)job->draw.primitive.material.draw_area_bottom < band->compiler.band_top ||
                 (int)job->draw.primitive.material.draw_area_top > band->compiler.band_bottom))
                continue;
            const int ok = band->failed || native_render_draw(&band->compiler, &job->surface, &job->draw, job->record_index);
            band->failed |= !ok;
        }
        SDL_LockMutex(worker->mutex);
        band->done = end;
        SDL_CondBroadcast(worker->condition);
    }
    SDL_UnlockMutex(worker->mutex);
    return 1;
}

static int native_view_worker_failed(const GlNativeViewWorker *worker,
                                     const GlNativeCompileAudit **out_audit) {
    for (uint32_t i = 0u; i < worker->band_count; ++i)
        if (worker->bands[i].failed) {
            if (out_audit) *out_audit = &worker->bands[i].audit;
            return 1;
        }
    return 0;
}

/* The producer owns count and unpublished jobs. Readers see only a complete,
 * immutable prefix under the mutex; dependencies publish even a short tail
 * before joining. Waking every reader for each tiny primitive costs more
 * than its reference raster. */
static void native_view_worker_publish(GlNativeViewWorker *worker) {
    if (worker->published == worker->count) return;
    worker->published = worker->count;
    SDL_CondBroadcast(worker->condition);
}

static int native_view_worker_drain(GlNativeCpuCompiler *compiler, int left, int top, int right, int bottom) {
    GlNativeViewWorker *worker = compiler->view_worker;
    if (!worker || left > right || top > bottom) return 1;
    if (left < 0) left = 0; if (right >= VRAM_W) right = VRAM_W - 1;
    if (top < 0) top = 0; if (bottom >= VRAM_H) bottom = VRAM_H - 1;
    if (left > right || top > bottom) return 1;
    const uint16_t mask = (uint16_t)(((UINT32_C(1) << (right / 64 - left / 64 + 1)) - 1u) << (left / 64));
    const int all = left == 0 && right == VRAM_W - 1 && top == 0 && bottom == VRAM_H - 1;
    if (!all) {
        const uint16_t pages = worker->read_pages[top / 256] | worker->read_pages[bottom / 256];
        if (!(pages & mask)) return 1;
    }
    uint16_t reads = 0u;
    for (int y = top; y <= bottom; ++y) reads |= worker->reads[y];
    if (!(reads & mask) && !all) return 1;
    SDL_LockMutex(worker->mutex);
    native_view_worker_publish(worker);
    while (!native_view_worker_idle(worker)) SDL_CondWait(worker->condition, worker->mutex);
    const int ok = !native_view_worker_failed(worker, NULL);
    SDL_UnlockMutex(worker->mutex);
    memset(worker->reads, 0, sizeof(worker->reads));
    memset(worker->read_pages, 0, sizeof(worker->read_pages));
    memset(worker->texture_read_pages, 0, sizeof(worker->texture_read_pages));
    return ok;
}

static int native_view_worker_finish(GlNativeCpuCompiler *compiler) {
    GlNativeViewWorker *worker = compiler->view_worker;
    const GlNativeCompileAudit *failed = NULL;
    if (!worker) return 1;
    SDL_LockMutex(worker->mutex);
    native_view_worker_publish(worker);
    while (!native_view_worker_idle(worker)) SDL_CondWait(worker->condition, worker->mutex);
    const int ok = !native_view_worker_failed(worker, &failed);
    if (!ok) native_audit_block(compiler->audit, failed->blocker, failed->blocker_record_index,
        failed->blocker_resource_id, failed->blocker_resource_generation);
    /* Band audits restart at every bind; fold their depth counters once here. */
    for (uint32_t i = 0u; i < worker->band_count; ++i) {
        GlNativeCompileAudit *band = &worker->bands[i].audit;
        compiler->audit->depth_tested_fragments += band->depth_tested_fragments;
        compiler->audit->depth_rejected_fragments += band->depth_rejected_fragments;
        compiler->audit->depth_written_fragments += band->depth_written_fragments;
        compiler->audit->depth_reset_fragments += band->depth_reset_fragments;
        if (band->depth_key_min && (!compiler->audit->depth_key_min ||
                band->depth_key_min < compiler->audit->depth_key_min))
            compiler->audit->depth_key_min = band->depth_key_min;
        if (band->depth_key_max > compiler->audit->depth_key_max)
            compiler->audit->depth_key_max = band->depth_key_max;
        band->depth_tested_fragments = band->depth_rejected_fragments = 0u;
        band->depth_written_fragments = band->depth_reset_fragments = 0u;
        band->depth_key_min = band->depth_key_max = 0u;
    }
    SDL_UnlockMutex(worker->mutex);
    compiler->view_worker = NULL;
    return ok;
}

void native_view_worker_shutdown(void) {
    GlNativeViewWorker *worker = s_native_view_worker;
    if (!worker) return;
    SDL_LockMutex(worker->mutex); worker->stop = 1;
    SDL_CondBroadcast(worker->condition); SDL_UnlockMutex(worker->mutex);
    for (uint32_t i = 0u; i < worker->band_count; ++i)
        SDL_WaitThread(worker->bands[i].thread, NULL);
    SDL_DestroyCond(worker->condition); SDL_DestroyMutex(worker->mutex);
    free(worker->bands); free(worker->jobs); free(worker); s_native_view_worker = NULL;
}

static void native_view_worker_bind(GlNativeViewWorker *worker, const GlNativeCpuCompiler *compiler) {
    for (uint32_t i = 0u; i < worker->band_count; ++i) {
        GlNativeViewBand *band = &worker->bands[i];
        band->compiler = *compiler;
        band->compiler.audit = &band->audit; band->compiler.gpu = NULL;
        band->compiler.view_worker = NULL; band->compiler.raster_words = NULL;
        band->compiler.banded = 0;
        band->done = 0u; band->failed = 0;
        band->audit.blocker = 0u; band->audit.rendered_draws = 0u;
        band->audit.rendered_draw_pixels = 0u;
        band->audit.depth_tested_fragments = band->audit.depth_rejected_fragments = 0u;
        band->audit.depth_written_fragments = band->audit.depth_reset_fragments = 0u;
        band->audit.depth_key_min = band->audit.depth_key_max = 0u;
    }
    worker->count = 0u;
    worker->published = 0u;
    memset(worker->reads, 0, sizeof(worker->reads));
    memset(worker->read_pages, 0, sizeof(worker->read_pages));
    memset(worker->texture_read_pages, 0, sizeof(worker->texture_read_pages));
}

static int native_view_worker_draw(GlNativeCpuCompiler *compiler, GlNativeCpuSurface *surface,
                               const XgSemanticDrawRecord *draw, uint32_t record_index) {
    if (!compiler->view_worker) {
        GlNativeViewWorker *worker = s_native_view_worker;
        if (worker) {
            /* The preceding transaction joined every job before freeing its
             * input storage. Rebind only while the readers are quiescent. */
            SDL_LockMutex(worker->mutex);
            native_view_worker_bind(worker, compiler);
            SDL_UnlockMutex(worker->mutex);
        } else {
            const uint32_t wanted = psx_host_worker_count((unsigned)SDL_GetCPUCount(), surface->height);
            if (!wanted) return native_render_draw(compiler, surface, draw, record_index);
            worker = calloc(1u, sizeof(*worker));
            if (!worker) return native_render_draw(compiler, surface, draw, record_index);
            worker->jobs = malloc(GL_NATIVE_VIEW_JOB_CAPACITY * sizeof(*worker->jobs));
            worker->bands = calloc(wanted, sizeof(*worker->bands));
            worker->mutex = SDL_CreateMutex(); worker->condition = SDL_CreateCond();
            if (worker->jobs && worker->bands && worker->mutex && worker->condition) {
                /* Bands start idle (done == count == 0), so the band count
                 * is final before any job is published. Stop at the first
                 * failed thread: bands must be a contiguous 0..n-1 range. */
                SDL_LockMutex(worker->mutex);
                for (uint32_t i = 0u; i < wanted; ++i) {
                    worker->bands[i].owner = worker;
                    worker->bands[i].index = i;
                    worker->bands[i].thread = SDL_CreateThread(
                        native_view_worker_main, "native-reference", &worker->bands[i]);
                    if (!worker->bands[i].thread) break;
                    worker->band_count++;
                }
                native_view_worker_bind(worker, compiler);
                SDL_UnlockMutex(worker->mutex);
            }
            if (!worker->band_count) {
                if (worker->condition) SDL_DestroyCond(worker->condition);
                if (worker->mutex) SDL_DestroyMutex(worker->mutex);
                free(worker->bands); free(worker->jobs); free(worker);
                return native_render_draw(compiler, surface, draw, record_index);
            }
            s_native_view_worker = worker;
        }
        compiler->view_worker = worker;
    }
    GlNativeViewWorker *worker = compiler->view_worker;
    if (worker->count == GL_NATIVE_VIEW_JOB_CAPACITY) {
        if (!native_view_worker_finish(compiler)) return 0;
        return native_view_worker_draw(compiler, surface, draw, record_index);
    }
    /* Journal capture remains on its one compiler owner, before canonical writes.
     * Only the independent CPU raster is delegated. Failed validation still rolls
     * back the complete private transaction before ACK/publication. */
    compiler->gpu_only = 1;
    const int captured = native_render_draw(compiler, surface, draw, record_index);
    compiler->gpu_only = 0;
    if (!captured) return 0;
    const XgRenderIrMaterialState *m = &draw->primitive.material;
    if (m->textured) {
        const unsigned blocks = 1u << m->texture_depth;
        uint32_t mask = ((1u << blocks) - 1u) << m->texture_page_x;
        mask = (mask | (mask >> 16u)) & 65535u;
        const uint16_t added = (uint16_t)mask & ~worker->texture_read_pages[m->texture_page_y];
        if (added) {
            for (unsigned y = m->texture_page_y * 256u; y < (m->texture_page_y + 1u) * 256u; ++y)
                worker->reads[y] |= added;
            worker->texture_read_pages[m->texture_page_y] |= added;
        }
        worker->read_pages[m->texture_page_y] |= (uint16_t)mask;
        if (m->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT) {
            const unsigned entries = m->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT ? 16u : 256u;
            const unsigned blocks = ((m->clut_x & 63u) + entries + 63u) / 64u;
            mask = ((1u << blocks) - 1u) << (m->clut_x / 64u);
            const uint16_t clut_mask = (uint16_t)(mask | (mask >> 16u));
            worker->reads[m->clut_y] |= clut_mask;
            worker->read_pages[m->clut_y / 256u] |= clut_mask;
        }
    }
    GlNativeViewJob *job = &worker->jobs[worker->count];
    job->draw = *draw; job->resource = *surface->resource; job->surface = *surface;
    job->surface.resource = &job->resource; job->record_index = record_index;
    job->dither_x = compiler->dither_x; job->dither_y = compiler->dither_y;
    worker->count++;
    if (worker->count % GL_NATIVE_VIEW_PUBLISH_JOBS == 0u) {
        SDL_LockMutex(worker->mutex);
        native_view_worker_publish(worker);
        SDL_UnlockMutex(worker->mutex);
    }
    return 1;
}

static void native_recipe_pixels_release(GlNativeRecipePixels *pixels) {
    if (pixels && --pixels->references == 0u) free(pixels);
}

static int native_coverage_acquire_ref(XgSemanticResourceRef ref) {
    if (xg_render_resource_acquire_snapshot((XgRenderResourceHandle){ref.resource_id, ref.generation},
            ref.content_digest) != XG_RENDER_RESOURCE_OK) return 0;
    s_native_coverage_acquires++; return 1;
}

static void native_coverage_release_ref(XgSemanticResourceRef ref) {
    if (!ref.resource_id) return;
    if (xg_render_resource_release((XgRenderResourceHandle){ref.resource_id, ref.generation}) == XG_RENDER_RESOURCE_OK)
        s_native_coverage_releases++;
    else s_native_coverage_release_errors++;
}

static void native_coverage_release(GlNativeCoverageState *state) {
    if (!state || --state->references) return;
    for (uint32_t i = 0u; i < state->count; ++i) {
        const XgSemanticResourceRef refs[2] = {state->scopes[i].current.reference, state->scopes[i].previous.reference};
        for (unsigned j = 0u; j < 2u; ++j)
            native_coverage_release_ref(refs[j]);
    }
    free(state->scopes);
    free(state);
}

static int native_coverage_publish(GlNativeViewState *views, XgSemanticResourceRef ref) {
    XgRenderTemporalCoverageView view;
    if (!xg_render_temporal_coverage_view(ref, &view)) return 0;
    GlNativeCoverageState *state = views->publication;
    uint32_t index = 0u;
    while (state && index < state->count && state->scopes[index].current.view.header->producer_scope != view.header->producer_scope) ++index;
    if (state && index < state->count && !memcmp(&ref, &state->scopes[index].current.reference, sizeof(ref))) return 1;
    if (index == UINT32_MAX) return 0;
    if (!state || state->references != 1u) {
        GlNativeCoverageState *copy = calloc(1u, sizeof(*copy));
        if (!copy) return 0;
        copy->references = 1u;
        if (state && state->count) {
            copy->scopes = xg_render_array_reserve(NULL, sizeof(*copy->scopes),
                &copy->capacity, state->count, UINT32_MAX);
            if (!copy->scopes) { free(copy); return 0; }
        }
        for (uint32_t i = 0u; state && i < state->count; ++i) {
            const GlNativeRecipeCoverage sources[2] = {state->scopes[i].current, state->scopes[i].previous};
            GlNativeRecipeCoverage *destinations[2] = {&copy->scopes[i].current, &copy->scopes[i].previous};
            copy->count = i + 1u;
            for (unsigned j = 0u; j < 2u; ++j) {
                const XgSemanticResourceRef source = sources[j].reference;
                if (!source.resource_id) continue;
                if (!native_coverage_acquire_ref(source)) { native_coverage_release(copy); return 0; }
                *destinations[j] = sources[j];
            }
        }
        native_coverage_release(state); views->publication = state = copy;
    }
    GlNativeCoverageScope *grown = xg_render_array_reserve(state->scopes,
        sizeof(*grown), &state->capacity, index + 1u, UINT32_MAX);
    if (!grown) return 0;
    state->scopes = grown;
    if (!native_coverage_acquire_ref(ref)) return 0;
    GlNativeCoverageScope *scope = &state->scopes[index];
    if (index == state->count) state->count++;
    native_coverage_release_ref(scope->previous.reference);
    scope->previous = scope->current;
    scope->current = (GlNativeRecipeCoverage){ref, view, scope->previous.reference};
    return 1;
}

void native_recipe_release(GlNativeRecipe *recipe) {
    if (!recipe || --recipe->references) return;
    native_coverage_release(recipe->publication);
    for (uint32_t i = 0u; i < recipe->motion_count; ++i)
        (void)xg_render_resource_release(recipe->motions[i].handle);
    for (uint32_t i = 0u; i < recipe->coverage_count; ++i)
        native_coverage_release_ref(recipe->coverages[i].reference);
    for (uint32_t i = 0u; i < recipe->count; ++i) {
        native_recipe_pixels_release(recipe->draws[i].textures);
        native_recipe_mesh_release(recipe->draws[i].mesh);
    }
    native_recipe_pixels_release(recipe->textures);
    native_wave_release(recipe->wave);
    free(recipe->draws);
    free(recipe->motions);
    free(recipe->coverages);
    free(recipe);
}

static void native_recipe_drop_at(GlNativeViewTarget *target, uint32_t line, const char *caller) {
    if (target->recipe) {
        const uint32_t count = target->recipe->count;
        target->recipe_draws = count >= target->recipe_draws ? count
            : target->recipe_draws - (target->recipe_draws - count) / 4u;
        volatile GlNativeRecipeDropEvent *event = &s_native_recipe_drop_events[s_native_recipe_drop_total % 32u];
        *event = s_native_recipe_texture_read;
        event->caller = caller; event->line = line;
        event->x = target->x; event->y = target->y;
        event->width = target->width; event->height = target->height;
        event->draws = target->recipe->count; event->motions = target->recipe->motion_count;
        event->textures = target->recipe->texture_count;
        s_native_recipe_drop_total++;
    }
    native_recipe_release(target->recipe);
    target->recipe = NULL;
}
#define native_recipe_drop(target) native_recipe_drop_at((target), __LINE__, __func__)

/* Recipes exist solely to replay intermediate temporal frames. Endpoint
 * draws, framebuffer copies and the wave warp execute on their own GPU planes
 * without them. Keep capture available to explicit diagnostics. */
static int native_recipe_tracking_hz(uint16_t temporal_hz) {
    return temporal_hz || s_native_recipe_audit_enabled ||
        s_native_motion_probe.target_vblank;
}

static int native_recipe_tracking(const GlNativeViewState *views) {
    return native_recipe_tracking_hz(views->temporal_hz);
}

static int native_recipe_private(GlNativeViewTarget *target) {
    GlNativeRecipe *recipe = target->recipe;
    if (!recipe) return 0;
    if (recipe->references == 1u) return 1;
    GlNativeRecipe *copy = malloc(sizeof(*copy));
    if (!copy) {
        s_native_recipe_texture_read.texture_reason = "recipe_allocation";
        native_recipe_drop(target); return 0;
    }
    *copy = *recipe;
    copy->draws = NULL; copy->draw_capacity = 0u;
    copy->motions = NULL; copy->motion_capacity = 0u;
    copy->coverages = NULL; copy->coverage_capacity = 0u;
    if (recipe->count) copy->draws = xg_render_array_reserve_uninitialized(NULL, sizeof(*copy->draws),
        &copy->draw_capacity, recipe->count, UINT32_MAX);
    if (recipe->motion_count) copy->motions = xg_render_array_reserve(NULL, sizeof(*copy->motions),
        &copy->motion_capacity, recipe->motion_count, UINT32_MAX);
    if (recipe->coverage_count) copy->coverages = xg_render_array_reserve(NULL, sizeof(*copy->coverages),
        &copy->coverage_capacity, recipe->coverage_count, UINT32_MAX);
    if ((recipe->count && !copy->draws) || (recipe->motion_count && !copy->motions) ||
        (recipe->coverage_count && !copy->coverages)) {
        free(copy->draws); free(copy->motions); free(copy->coverages); free(copy);
        s_native_recipe_texture_read.texture_reason = "recipe_allocation";
        native_recipe_drop(target); return 0;
    }
    if (recipe->count) memcpy(copy->draws, recipe->draws, recipe->count * sizeof(*copy->draws));
    if (recipe->motion_count) memcpy(copy->motions, recipe->motions, recipe->motion_count * sizeof(*copy->motions));
    if (recipe->coverage_count) memcpy(copy->coverages, recipe->coverages, recipe->coverage_count * sizeof(*copy->coverages));
    copy->references = 1u;
    if (copy->publication) copy->publication->references++;
    if (copy->textures) copy->textures->references++;
    if (copy->wave) copy->wave->references++;
    for (uint32_t i = 0u; i < copy->count; ++i) {
        if (copy->draws[i].textures) copy->draws[i].textures->references++;
        if (copy->draws[i].mesh) copy->draws[i].mesh->references++;
    }
    const uint32_t motion_count = copy->motion_count;
    copy->motion_count = 0u;
    const uint32_t coverage_count = copy->coverage_count;
    copy->coverage_count = 0u;
    for (uint32_t i = 0u; i < motion_count; ++i) {
        if (xg_render_resource_acquire_snapshot(copy->motions[i].handle, copy->motions[i].digest) != XG_RENDER_RESOURCE_OK) {
            s_native_recipe_texture_read.texture_reason = "motion_retain";
            native_recipe_release(copy); native_recipe_drop(target); return 0;
        }
        copy->motion_count++;
    }
    for (uint32_t i = 0u; i < coverage_count; ++i) {
        const XgSemanticResourceRef ref = copy->coverages[i].reference;
        if (!native_coverage_acquire_ref(ref)) {
            native_recipe_release(copy); native_recipe_drop(target); return 0;
        }
        copy->coverage_count++;
    }
    native_recipe_release(recipe);
    target->recipe = copy;
    return 1;
}

void native_views_discard(GlNativeViewState *views, int all) {
    native_coverage_release(views->publication);
    for (uint32_t i = 0u; i < views->domain_count; ++i)
        if (all || (views->owned & (UINT64_C(1) << i))) free(views->domains[i].pixels);
    for (uint32_t i = 0u; i < views->count; ++i)
        native_recipe_release(views->targets[i].recipe);
    memset(views, 0, sizeof(*views));
    views->active = -1;
}

static int native_view_eligible(const GlNativeViewState *views, const GlNativeViewTarget *target) {
    return views->width != 0u &&
        target->width == views->width - 2u * views->offset;
}

/* Retail framebuffer captures use the active short-display height (216),
 * Field's 224 lines, or a 240-line texture including bottom padding. */
static int native_transition_height(uint32_t height) {
    return height == 216u || height == 224u || height == 240u;
}

static int native_transition_texture(const GlNativeViewState *views,
                                     const XgSemanticDrawRecord *draw) {
    const XgRenderIrMaterialState *m = &draw->primitive.material;
    if (draw->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        draw->native_view_effect || !m->textured ||
        m->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT ||
        m->texture_page_y != 1u || m->texture_page_x < 11u ||
        m->texture_window_mask_x || m->texture_window_mask_y)
        return -1;
    for (uint32_t i = 0u; i < views->count; ++i) {
        const GlNativeViewTarget *source = &views->targets[i];
        if (source->transition_snapshot != 1 || !native_view_eligible(views, source)) continue;
        /* World Map's FT4 mesh reaches row 239 even in the 216-row display.
         * Its off-screen vertices are safe when V equals raster Y throughout
         * the triangle: the scissor proves every sampled row is captured. */
        int clipped_v = m->draw_area_top <= m->draw_area_bottom &&
            m->draw_area_bottom - m->draw_area_top < source->height;
        for (uint32_t t = 0u; t < draw->primitive.triangle_count && clipped_v; ++t)
            for (uint32_t v = 0u; v < 3u; ++v) {
                const XgRenderIrVertex *vertex = &draw->primitive.triangles[t].vertices[v];
                if (vertex->native_view_position || vertex->projective_position ||
                    vertex->v != (int64_t)vertex->y +
                        (int64_t)(m->draw_offset_y - m->draw_area_top) * 65536)
                    clipped_v = 0;
            }
        int inside = draw->primitive.triangle_count != 0u;
        for (uint32_t t = 0u; t < draw->primitive.triangle_count && inside; ++t)
            for (uint32_t v = 0u; v < 3u; ++v) {
                const XgRenderIrVertex *vertex = &draw->primitive.triangles[t].vertices[v];
                const int64_t u = vertex->u +
                    (int64_t)(m->texture_page_x * 64 - source->x) * 65536;
                if (u < 0 || u > (int64_t)source->width * 65536 ||
                    (!clipped_v && (vertex->v < 0 ||
                        vertex->v > (int32_t)source->height * 65536)))
                    inside = 0;
            }
        if (inside) return (int)i;
    }
    return -1;
}

static int native_transition_overwritten(const GlNativeViewTarget *target,
                                        const XgSemanticDrawRecord *draw) {
    const XgRenderIrMaterialState *m = &draw->primitive.material;
    if (!target->transition_snapshot ||
        m->draw_area_left >= target->x + target->width || m->draw_area_right < target->x ||
        m->draw_area_top >= target->y + target->height || m->draw_area_bottom < target->y)
        return 0;
    for (uint32_t t = 0; t < draw->primitive.triangle_count; ++t) {
        int64_t left = INT64_MAX, top = INT64_MAX, right = INT64_MIN, bottom = INT64_MIN;
        for (uint32_t v = 0; v < 3; ++v) {
            const XgRenderIrVertex *vertex = &draw->primitive.triangles[t].vertices[v];
            const int64_t x = vertex->x + (int64_t)m->draw_offset_x * 65536;
            const int64_t y = vertex->y + (int64_t)m->draw_offset_y * 65536;
            if (x < left) left = x; if (x > right) right = x;
            if (y < top) top = y; if (y > bottom) bottom = y;
        }
        if (left < (int64_t)(target->x + target->width) * 65536 &&
            right >= (int64_t)target->x * 65536 &&
            top < (int64_t)(target->y + target->height) * 65536 &&
            bottom >= (int64_t)target->y * 65536) return 1;
    }
    return 0;
}

static uint32_t native_view_domain(GlNativeViewState *views, const GlNativeViewTarget *target) {
    for (uint32_t i = 0u; i < views->domain_count; ++i)
        if (views->domains[i].x == target->x && views->domains[i].width == target->width) return i;
    if (views->domain_count == GL_NATIVE_VIEW_TARGET_CAPACITY) return UINT32_MAX;
    const uint32_t index = views->domain_count++;
    views->domains[index] = (GlNativeViewDomain){.x = target->x, .width = target->width};
    return index;
}

static int native_view_domain_private(GlNativeViewState *views, uint32_t index, uint32_t **copy_snapshot) {
    GlNativeViewDomain *domain = &views->domains[index];
    const uint64_t bit = UINT64_C(1) << index;
    if ((views->owned & bit) && !copy_snapshot) return 1;
    const size_t bytes = native_view_domain_bytes(views);
    uint32_t *pixels = domain->pixels ? malloc(bytes) : calloc(1u, bytes);
    if (!pixels) return 0;
    if (domain->pixels) memcpy(pixels, domain->pixels, bytes);
    if ((views->owned & bit) && copy_snapshot) *copy_snapshot = domain->pixels;
    domain->pixels = pixels;
    views->owned |= bit;
    for (uint32_t i = 0u; i < views->count; ++i) {
        GlNativeViewTarget *alias = &views->targets[i];
        if (alias->pixels && alias->x == domain->x && alias->width == domain->width)
            alias->pixels = pixels + (size_t)alias->y * views->width;
    }
    return 1;
}

static int native_view_private(GlNativeViewState *views, uint32_t index,
                           const uint32_t *canonical) {
    GlNativeViewTarget *target = &views->targets[index];
    if (!native_view_eligible(views, target)) return 1;
    const uint32_t slot = native_view_domain(views, target);
    if (slot == UINT32_MAX || !native_view_domain_private(views, slot, NULL)) return 0;
    if (target->pixels) return 1;
    GlNativeViewDomain *domain = &views->domains[slot];
    const int gpu_new = views->gpu && !views->gpu->widths[slot + 1u];
    /* Previously declared rows are already the same memory, regardless of
     * which vertical alias was active. Seed ONLY genuinely new device rows. */
    for (uint32_t y = target->y; y < target->y + target->height; ++y) {
        if (domain->initialized[y]) continue;
        memcpy(domain->pixels + (size_t)y * views->width + views->offset,
            canonical + (size_t)y * VRAM_W + target->x, (size_t)target->width * sizeof(uint32_t));
        native_depth_clear_rows(native_view_domain_depth(views, domain), views->width,
            views->offset, (int)y, target->width, 1);
        if (views->gpu && !gpu_new) {
            GlNativeGpuCommand *seed = native_gpu_command(views->gpu, NATIVE_GPU_SEED);
            if (!seed) return 0;
            seed->plane = slot + 1u; seed->x = views->offset; seed->y = y;
            seed->w = target->width; seed->h = 1;
            if (!native_gpu_data(views->gpu, canonical + (size_t)y * VRAM_W + target->x,
                (size_t)target->width * sizeof(uint32_t), &seed->data)) return 0;
        }
        domain->initialized[y] = 1u;
    }
    if (!native_gpu_seed(views->gpu, slot + 1u, views->width, VRAM_H, domain->pixels,
            native_view_domain_depth(views, domain))) return 0;
    target->pixels = domain->pixels + (size_t)target->y * views->width;
    return 1;
}

static int native_view_target(GlNativeViewState *views, uint16_t x, uint16_t y,
                          uint16_t width, uint16_t height) {
    if (!width || !height || x >= VRAM_W || y >= VRAM_H ||
        width > VRAM_W - x || height > VRAM_H - y) return -1;
    int containing = -1;
    for (uint32_t i = 0u; i < views->count; ++i) {
        const GlNativeViewTarget *target = &views->targets[i];
        if (native_view_eligible(views, target) && width < target->width &&
            x >= target->x && y >= target->y &&
            x + width <= target->x + target->width && y + height <= target->y + target->height &&
            (containing < 0 || target->declaration > views->targets[containing].declaration))
            containing = (int)i;
    }
    /* Only narrower SDK clips reuse a containing projection. Full-width pages
     * own their exact vertical extent, even inside an older framebuffer/atlas;
     * the DRAW still carries its material scissor. */
    if (containing >= 0) return containing;
    for (uint32_t i = 0u; i < views->count; ++i) {
        const GlNativeViewTarget *target = &views->targets[i];
        if (target->x == x && target->y == y && target->width == width &&
            target->height == height) return (int)i;
    }
    if (views->count == GL_NATIVE_VIEW_TARGET_CAPACITY) return -1;
    const uint32_t index = views->count++;
    views->targets[index] = (GlNativeViewTarget){.x = x, .y = y,
        .width = width, .height = height,
        .declaration = ++views->declaration_sequence};
    return (int)index;
}

/* Resolve aliases using explicit target state. A full VIEW row must match its
 * horizontal projection domain, not a wider active VRAM/scissor rectangle. */
static int native_view_cover(const GlNativeViewState *views, uint32_t count,
                         int x, int y, int w, int h) {
    int found = -1;
    const int view_row = views->width && w == (int)views->width - 2 * views->offset;
    if (views->active >= 0 && (uint32_t)views->active < count) {
        const GlNativeViewTarget *target = &views->targets[views->active];
        if ((!view_row || (x == target->x && w == target->width)) &&
            x >= target->x && y >= target->y &&
            x + w <= target->x + target->width &&
            y + h <= target->y + target->height) return views->active;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const GlNativeViewTarget *target = &views->targets[i];
        if (view_row && (x != target->x || w != target->width)) continue;
        if (x < target->x || y < target->y ||
            x + w > target->x + target->width ||
            y + h > target->y + target->height) continue;
        /* An old exact-size alias can outlive a scene change. In battle the
         * 216-line scanout crops a freshly declared 224-line framebuffer;
         * selecting an older 216-line target loses its temporal recipe every
         * other page flip. Resolve by declaration age, as for scanout rows,
         * instead of letting that stale exact extent override the owner. */
        if (found < 0 || target->declaration > views->targets[found].declaration)
            found = (int)i;
    }
    return found;
}

static int64_t native_div_floor(int64_t numerator, int64_t denominator) {
    const int64_t quotient = numerator / denominator;
    return quotient - (numerator < 0 && numerator % denominator != 0);
}

static uint32_t native_recipe_vertex_count(const GpuRenderSemantic *draw) {
    return draw->topology == GPU_RENDER_SEMANTIC_LINES
        ? draw->line_count * 2u : draw->triangle_count * 3u;
}

static GpuRenderSemanticVertex *native_recipe_vertex(GpuRenderSemantic *draw, uint32_t index) {
    return draw->topology == GPU_RENDER_SEMANTIC_LINES
        ? &draw->lines[index / 2u].vertices[index % 2u]
        : &draw->triangles[index / 3u].vertices[index % 3u];
}

static int native_recipe_region(const GpuVramRegionSet *set, int x, int y, int w, int h) {
    GpuVramRect rectangles[4];
    const int count = gpu_vram_split_transfer(x, y, w, h, rectangles);
    for (int i = 0; i < count; ++i)
        if (gpu_vram_region_intersects(set, rectangles[i].x, rectangles[i].y,
            rectangles[i].x + rectangles[i].w - 1, rectangles[i].y + rectangles[i].h - 1)) return 1;
    return 0;
}

static int native_recipe_feedback_region(GlNativeRecipe *recipe, int x, int y, int w, int h) {
    for (uint32_t i = 0u; i < recipe->feedback_cache_count; ++i)
        if (recipe->feedback_cache[i].x == x && recipe->feedback_cache[i].y == y &&
            recipe->feedback_cache[i].w == w && recipe->feedback_cache[i].h == h)
            return recipe->feedback_cache[i].result;
    const int result = native_recipe_region(&recipe->feedback, x, y, w, h);
    const uint32_t slot = recipe->feedback_cache_next++ % 4u;
    recipe->feedback_cache[slot].x = (int16_t)x; recipe->feedback_cache[slot].y = (int16_t)y;
    recipe->feedback_cache[slot].w = (int16_t)w; recipe->feedback_cache[slot].h = (int16_t)h;
    recipe->feedback_cache[slot].result = (uint8_t)result;
    if (recipe->feedback_cache_count < 4u) recipe->feedback_cache_count++;
    return result;
}

static int native_recipe_word(GlNativeRecipe *recipe,
                            const GlNativeViewTarget *target, const uint16_t *words,
                            int x, int y, int *changed) {
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    const int own_target = x >= target->x && x < target->x + target->width &&
        y >= target->y && y < target->y + target->height;
    if (own_target || gpu_vram_region_intersects(&recipe->feedback, x, y, x, y)) {
        s_native_recipe_texture_read.texture_reason = own_target ? "target_feedback" : "target_feedback_alias";
        s_native_recipe_texture_read.texture_x = x; s_native_recipe_texture_read.texture_y = y;
        return 0;
    }
    if (!recipe->textures || recipe->textures->words[(size_t)y * VRAM_W + x] != words[(size_t)y * VRAM_W + x])
        *changed = 1;
    return 1;
}

static void native_recipe_read_span(GlNativeRecipeReads *reads, int x, int y, unsigned count) {
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    /* Palettes are contiguous words, including the horizontal VRAM wrap.
     * Record each 64-word mask once instead of revisiting it per entry. */
    while (count) {
        const unsigned first = (unsigned)x & 63u;
        const unsigned length = count < 64u - first ? count : 64u - first;
        uint64_t *word = &reads->words.rows[y][x >> 6];
        if (!*word)
            reads->indices[reads->words.nonzero_words++] = (uint16_t)(y * GPU_VRAM_REGION_WORDS_PER_ROW + (x >> 6));
        *word |= gpu_vram_region_word_mask((int)first, (int)(first + length - 1u));
        x = (x + (int)length) & (VRAM_W - 1);
        count -= length;
    }
}

static unsigned native_attribute_clip(double polygon[12][2], unsigned count,
                                   unsigned axis, double boundary, int upper) {
    double result[12][2];
    unsigned used = 0u;
    for (unsigned i = 0u; i < count; ++i) {
        const double *a = polygon[i], *b = polygon[(i + 1u) % count];
        const double da = upper ? boundary - a[axis] : a[axis] - boundary;
        const double db = upper ? boundary - b[axis] : b[axis] - boundary;
        if (da >= 0.0) { result[used][0] = a[0]; result[used++][1] = a[1]; }
        if ((da < 0.0) != (db < 0.0)) {
            const double t = da / (da - db);
            result[used][axis] = boundary;
            result[used++][1u-axis] = a[1u-axis] + t * (b[1u-axis] - a[1u-axis]);
        }
    }
    /* A triangle clipped by four half-planes has at most seven vertices. */
    memcpy(polygon, result, used * sizeof(result[0]));
    return used;
}

static unsigned native_first_bit(uint64_t bits) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(bits);
#else
    unsigned first = 0u;
    if (!(bits & UINT64_C(0xffffffff))) { bits >>= 32u; first += 32u; }
    if (!(bits & UINT64_C(0xffff))) { bits >>= 16u; first += 16u; }
    if (!(bits & UINT64_C(0xff))) { bits >>= 8u; first += 8u; }
    if (!(bits & UINT64_C(0xf))) { bits >>= 4u; first += 4u; }
    if (!(bits & UINT64_C(0x3))) { bits >>= 2u; first += 2u; }
    return first + !(bits & 1u);
#endif
}

static void native_recipe_reads_clear(GlNativeRecipeReads *reads) {
    /* Call-site scratch belongs to the serialized compiler worker. Every bit
     * set below is indexed, so clear precisely the previous nonzero words. */
    for(uint32_t i=0;i<reads->words.nonzero_words;++i) {
        const unsigned index=reads->indices[i];
        reads->words.rows[index/GPU_VRAM_REGION_WORDS_PER_ROW][index%GPU_VRAM_REGION_WORDS_PER_ROW]=0;
    }
    reads->words.nonzero_words=0;
}

/* Mark a VRAM word rectangle (wrapping in X at VRAM_W) and index its words. */
static void native_recipe_reads_mark(GlNativeRecipeReads *reads, int x, int y, int width, int height) {
    for (int part = 0; part < 2 && width > 0; ++part) {
        const int x0 = x & (VRAM_W - 1), span = width < VRAM_W - x0 ? width : VRAM_W - x0;
        for (int row = y; row < y + height && row < VRAM_H; ++row)
            for (int word = x0 >> 6; word <= (x0 + span - 1) >> 6; ++word) {
                const int first = word == x0 >> 6 ? x0 & 63 : 0;
                const int last = word == (x0 + span - 1) >> 6 ? (x0 + span - 1) & 63 : 63;
                uint64_t *bits = &reads->words.rows[row][word];
                if (!*bits)
                    reads->indices[reads->words.nonzero_words++] =
                        (uint16_t)(row * GPU_VRAM_REGION_WORDS_PER_ROW + word);
                *bits |= gpu_vram_region_word_mask(first, last);
            }
        x = 0; width -= span;
    }
}

/* A mesh's reads: its whole texture page and CLUT row, a superset of the
 * texels any of its triangles samples under that material. */
static void native_recipe_mesh_reads(const GpuRenderSemantic *draw, GlNativeRecipeReads *reads) {
    const GpuRenderMaterial *m = &draw->material;
    native_recipe_reads_clear(reads);
    if (!m->textured) return;
    native_recipe_reads_mark(reads, m->texture_page_x * 64, m->texture_page_y * 256,
                             64 << m->texture_depth, 256);
    if (m->texture_depth != GPU_RENDER_TEXTURE_15_BIT)
        native_recipe_reads_mark(reads, m->clut_x, m->clut_y,
                                 m->texture_depth == GPU_RENDER_TEXTURE_4_BIT ? 16 : 256, 1);
}

static void native_recipe_texture_reads_uncached(const GpuRenderSemantic *draw, GlNativeRecipeReads *reads,
                                    uint32_t render_scale, int origin_y,
                                    const GlNativeRecipe *view) {
    const GpuRenderMaterial *m = &draw->material;
    native_recipe_reads_clear(reads);
    if (!m->textured || draw->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        m->draw_area_left > m->draw_area_right || m->draw_area_top > m->draw_area_bottom) return;
    if (!render_scale) render_scale = 1u;
    const int shift = m->texture_depth == GPU_RENDER_TEXTURE_4_BIT ? 2 :
        m->texture_depth == GPU_RENDER_TEXTURE_8_BIT ? 1 : 0;
    /* Recipes may consume both canonical and extended VIEW geometry. These
     * transforms are local copies of the native_view_raster coordinate contract;
     * no source positions or layout flags are changed by dependency analysis. */
    for (unsigned variant = 0u; variant < (view && view->view_width ? 2u : 1u); ++variant) {
        int left = m->draw_area_left, right = m->draw_area_right;
        int top = (int)m->draw_area_top - origin_y, bottom = (int)m->draw_area_bottom - origin_y;
        int64_t translation = variant ? (int64_t)view->offset * 65536 : 0;
        const int mode = variant ? draw->screen_space_2d : GPU_RENDER_SCREEN_SPACE_2D_NONE;
        if (mode == GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE) {
            int64_t lo = INT64_MAX, hi = INT64_MIN;
            for (unsigned t = 0u; t < draw->triangle_count; ++t)
                for (unsigned v = 0u; v < 3u; ++v) {
                    const int64_t x = draw->triangles[t].vertices[v].x + (int64_t)m->draw_offset_x * 65536;
                    if (x < lo) lo = x; if (x > hi) hi = x;
                }
            if (lo <= hi) {
                const int64_t center = lo + (hi-lo)/2, middle = (int64_t)view->width * 32768;
                const int64_t anchor = center < middle ? lo : center > middle ? hi : center;
                translation = native_div_floor(anchor * view->view_width, view->width) - anchor;
            }
        }
        if (variant) {
            if (mode == GPU_RENDER_SCREEN_SPACE_2D_STRETCH) {
                left = (int)native_div_floor((int64_t)left * view->view_width, view->width);
                right = (int)native_div_floor((int64_t)(right+1) * view->view_width, view->width)-1;
            } else {
                left = left == 0 ? 0 : (int)native_div_floor((int64_t)left*65536+translation,65536);
                right = right == view->width-1 ? view->view_width-1 :
                    (int)native_div_floor((int64_t)(right+1)*65536+translation,65536)-1;
            }
            if (right >= view->view_width) right = view->view_width-1;
        }
        if (left < 0) left = 0; if (top < 0) top = 0;
        if (left > right || top > bottom) continue;
        for (unsigned t = 0u; t < draw->triangle_count; ++t) {
            XgRenderIrTriangle triangle = {0};
            double x[3], y[3];
            for (unsigned v = 0u; v < 3u; ++v) {
                const GpuRenderSemanticVertex *s = &draw->triangles[t].vertices[v];
                int64_t px = (variant && s->native_view_position ? s->native_view_x : s->x) +
                    (int64_t)m->draw_offset_x * 65536;
                if (variant && !s->native_view_position) px = mode == GPU_RENDER_SCREEN_SPACE_2D_STRETCH
                    ? native_div_floor(px * view->view_width, view->width) : px + translation;
                x[v] = px / 65536.0;
                y[v] = (variant && s->native_view_position ? s->native_view_y : s->y) / 65536.0 +
                    m->draw_offset_y - origin_y;
                triangle.vertices[v].u = s->u; triangle.vertices[v].v = s->v;
            }
            if ((x[1]-x[0])*(y[2]-y[0]) == (x[2]-x[0])*(y[1]-y[0])) continue;
            /* CPU 1x and the actual GPU scale only, not a union of hypothetical
             * scale settings which could introduce false framebuffer feedback. */
            for (unsigned pass = 0u; pass < (render_scale == 1u ? 1u : 2u); ++pass) {
                const uint32_t scale = pass ? render_scale : 1u;
                GlNativeAttributePlanes attributes;
                double polygon[12][2];
                double position_error[2] = {0.0,0.0};
                double minimum[2] = {DBL_MAX,DBL_MAX}, maximum[2] = {-DBL_MAX,-DBL_MAX};
                unsigned count = 3u;
                native_attribute_planes(&triangle, x, y, scale, 0, &attributes);
                for (unsigned v = 0u; v < 3u; ++v) {
                    polygon[v][0] = x[v] - 1.0/64.0;
                    polygon[v][1] = y[v] - 1.0/64.0;
                    for (unsigned a = 0u; a < 2u; ++a) {
                        if (polygon[v][a] < minimum[a]) minimum[a] = polygon[v][a];
                        if (polygon[v][a] > maximum[a]) maximum[a] = polygon[v][a];
                    }
                    /* GPU position conversion/projection is float even when
                     * its attribute accumulator is integer. Bound that small
                     * coverage displacement separately from plane rounding. */
                    position_error[0] = fmax(position_error[0],8.0*FLT_EPSILON*
                        (fabs(x[v])+abs(m->draw_offset_x)+abs(left)+abs(right)+1.0));
                    position_error[1] = fmax(position_error[1],8.0*FLT_EPSILON*
                        (fabs(y[v])+abs(m->draw_offset_y)+abs(origin_y)+abs(top)+abs(bottom)+1.0));
                }
                /* Coverage is sampled at attribute_position + 1/64. Clip that
                 * shifted triangle to the closed physical sample-index domain,
                 * not to its screen AABB. Affine extrema occur at clip vertices. */
                const double sample_left = fmax(left,ceil((minimum[0]-position_error[0])*scale)/scale);
                const double sample_right = fmin(right+1.0-1.0/scale,floor((maximum[0]+position_error[0])*scale)/scale);
                const double sample_top = fmax(top,ceil((minimum[1]-position_error[1])*scale)/scale);
                const double sample_bottom = fmin(bottom+1.0-1.0/scale,floor((maximum[1]+position_error[1])*scale)/scale);
                if (sample_left > sample_right || sample_top > sample_bottom) continue;
                count = native_attribute_clip(polygon,count,0u,sample_left-position_error[0],0);
                count = native_attribute_clip(polygon,count,0u,sample_right+position_error[0],1);
                count = native_attribute_clip(polygon,count,1u,sample_top-position_error[1],0);
                count = native_attribute_clip(polygon,count,1u,sample_bottom+position_error[1],1);
                if (!count) continue;
                int start[2], length[2];
                for (unsigned a = 0u; a < 2u; ++a) {
                    double lo = DBL_MAX, hi = -DBL_MAX;
                    for (unsigned v = 0u; v < count; ++v) {
                        const double dx = polygon[v][0]-attributes.x, dy = polygon[v][1]-attributes.y;
                        const double *p = attributes.plane[a];
                        const double value = p[0]+p[1]*dx+p[2]*dy;
                        const double magnitude = fabs(p[0])+fabs(p[1])*(fabs(polygon[v][0])+fabs(attributes.x)+1.0)+
                            fabs(p[2])*(fabs(polygon[v][1])+fabs(attributes.y)+1.0)+1.0;
                        double error = 64.0 * DBL_EPSILON * magnitude +
                            fabs(p[1])*position_error[0]+fabs(p[2])*position_error[1];
                        double lower = value-error, upper = value+error;
                        if (!attributes.integral) {
                            /* Include the shader's float uniforms and arithmetic
                             * as well as CPU double evaluation. Error depends on
                             * the actual plane, never an assumed +/-1 texel. */
                            const double fv = (float)p[0]+(double)(float)p[1]*(polygon[v][0]-(float)attributes.x)+
                                (double)(float)p[2]*(polygon[v][1]-(float)attributes.y);
                            error += 16.0 * FLT_EPSILON * magnitude;
                            lower = fmin(lower,fv-error); upper = fmax(upper,fv+error);
                        }
                        if (lower < lo) lo = lower; if (upper > hi) hi = upper;
                    }
                    /* Integral DDA samples lie on the Q12 lattice even when
                     * the clipping intersections do not. Snap outward bounds
                     * inward to that lattice before taking the texel floor. */
                    if (attributes.integral) { lo = ceil(lo*4096.0)/4096.0; hi = floor(hi*4096.0)/4096.0; }
                    lo = floor(lo); hi = floor(hi);
                    if (!isfinite(lo) || !isfinite(hi) || hi-lo >= 255.0) { start[a]=0; length[a]=256; }
                    else { start[a]=(int)fmod(lo,256.0); length[a]=hi < lo ? 0 : (int)(hi-lo)+1; }
                }
                uint64_t columns[GPU_VRAM_REGION_WORDS_PER_ROW] = {0};
                uint8_t column_indices[GPU_VRAM_REGION_WORDS_PER_ROW];
                unsigned column_count = 0u;
                for (int u = 0; u < length[0]; ++u) {
                    const int tx = (m->texture_page_x*64 + (native_texture_window_coord(start[0]+u,
                        m->texture_window_mask_x,m->texture_window_offset_x) >> shift)) & (VRAM_W-1);
                    if (!columns[tx>>6]) column_indices[column_count++]=(uint8_t)(tx>>6);
                    columns[tx>>6] |= UINT64_C(1) << (tx&63);
                }
                for (int v = 0; v < length[1]; ++v) {
                    const int ty = (m->texture_page_y*256 + native_texture_window_coord(start[1]+v,
                        m->texture_window_mask_y,m->texture_window_offset_y)) & (VRAM_H-1);
                    for (unsigned c = 0u; c < column_count; ++c) {
                        const unsigned column = column_indices[c];
                        uint64_t *word = &reads->words.rows[ty][column];
                        if (!*word) reads->indices[reads->words.nonzero_words++] =
                            (uint16_t)(ty*GPU_VRAM_REGION_WORDS_PER_ROW+column);
                        *word |= columns[column];
                    }
                }
            }
        }
    }
    if (shift && reads->words.nonzero_words) {
        const int entries = shift == 2 ? 16 : 256;
        native_recipe_read_span(reads, m->clut_x, m->clut_y, (unsigned)entries);
    }
}

typedef struct GlNativeTextureReadKey {
    int32_t material[18];
    uint32_t scale, topology, triangle_count, screen_space, width, view_width, offset;
    int32_t origin_y;
    int32_t vertices[6][7];
} GlNativeTextureReadKey;
typedef struct GlNativeTextureReadWord {
    uint64_t bits;
    uint16_t index;
} GlNativeTextureReadWord;
typedef struct GlNativeTextureReadCache {
    GlNativeTextureReadKey key;
    GlNativeTextureReadWord *words;
    uint64_t hash;
    uint32_t count, capacity;
    int valid;
} GlNativeTextureReadCache;
static GlNativeTextureReadCache s_native_texture_read_cache[2048];

void native_recipe_reads_cache_clear(void) {
    for (unsigned i = 0u; i < 2048u; ++i) free(s_native_texture_read_cache[i].words);
    memset(s_native_texture_read_cache, 0, sizeof(s_native_texture_read_cache));
}

static void native_recipe_texture_reads(const GpuRenderSemantic *draw, GlNativeRecipeReads *reads,
                                    uint32_t render_scale, int origin_y,
                                    const GlNativeRecipe *view) {
    const GpuRenderMaterial *m = &draw->material;
    if (!m->textured || draw->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        m->draw_area_left > m->draw_area_right || m->draw_area_top > m->draw_area_bottom) {
        native_recipe_reads_clear(reads); return;
    }
    /* This footprint depends on coordinates/material/layout, never on current
     * VRAM contents. Keep the exact clipped result for repeated static geometry;
     * version, feedback and texel comparisons still run against live snapshots. */
    GlNativeTextureReadKey key = {
        .material = {m->textured, m->texture_depth, m->texture_page_x, m->texture_page_y,
            m->clut_x, m->clut_y, m->texture_window_mask_x, m->texture_window_mask_y,
            m->texture_window_offset_x, m->texture_window_offset_y,
            m->draw_area_left, m->draw_area_right, m->draw_area_top, m->draw_area_bottom,
            m->draw_offset_x, m->draw_offset_y},
        .scale = render_scale ? render_scale : 1u, .origin_y = origin_y,
        .topology = draw->topology, .triangle_count = draw->triangle_count,
        .screen_space = draw->screen_space_2d,
        .width = view && view->view_width ? view->width : 0u,
        .view_width = view ? view->view_width : 0u,
        .offset = view && view->view_width ? view->offset : 0u,
    };
    for (uint32_t t = 0u; t < draw->triangle_count; ++t)
        for (unsigned v = 0u; v < 3u; ++v) {
            const GpuRenderSemanticVertex *p = &draw->triangles[t].vertices[v];
            int32_t *value = key.vertices[t * 3u + v];
            value[0] = p->x; value[1] = p->y; value[2] = p->u; value[3] = p->v;
            value[4] = p->native_view_x; value[5] = p->native_view_y;
            value[6] = p->native_view_position;
        }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0u; i < sizeof(key); i += sizeof(uint32_t)) {
        uint32_t word; memcpy(&word, (const uint8_t *)&key + i, sizeof(word));
        hash = (hash ^ word) * UINT64_C(1099511628211);
    }
    GlNativeTextureReadCache *cached = &s_native_texture_read_cache[(hash ^ (hash >> 32u)) & 2047u];
    if (cached->valid && cached->hash == hash && !memcmp(&cached->key, &key, sizeof(key))) {
        native_recipe_reads_clear(reads);
        for (uint32_t i = 0u; i < cached->count; ++i) {
            const unsigned index = cached->words[i].index;
            reads->indices[i] = (uint16_t)index;
            reads->words.rows[index / GPU_VRAM_REGION_WORDS_PER_ROW][index % GPU_VRAM_REGION_WORDS_PER_ROW] =
                cached->words[i].bits;
        }
        reads->words.nonzero_words = cached->count;
        return;
    }
    native_recipe_texture_reads_uncached(draw, reads, render_scale, origin_y, view);
    const uint32_t count = reads->words.nonzero_words;
    if (count > cached->capacity) {
        GlNativeTextureReadWord *words = realloc(cached->words, (size_t)count * sizeof(*words));
        if (!words) { cached->valid = 0; return; }
        cached->words = words; cached->capacity = count;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const unsigned index = reads->indices[i];
        cached->words[i].index = (uint16_t)index;
        cached->words[i].bits = reads->words.rows[index / GPU_VRAM_REGION_WORDS_PER_ROW][index % GPU_VRAM_REGION_WORDS_PER_ROW];
    }
    cached->key = key; cached->hash = hash; cached->count = count; cached->valid = 1;
}

static void native_gpu_publish_prefix(GlNativeGpuWork *work,int seal) {
    if (work && work->deferred) return;
    if(!work||(!seal&&work->count-work->published_count<512u&&
        work->chunk_count==work->published_chunk_count))return;
    SDL_LockMutex(s_native_state_mutex);
    work->published_count=work->count;
    work->published_chunk_count=work->chunk_count;
    work->published_commands=work->commands;
    work->published_data=work->data;
    memcpy(work->published_widths,work->widths,sizeof(work->widths));
    memcpy(work->published_heights,work->heights,sizeof(work->heights));
    if (seal == -2) work->phase_stream_ready = 1;
    if(!work->state) {
        work->state=NATIVE_GPU_QUEUED;s_native_gpu_work=work;
        work->timing.queued_ns=SDL_GetTicksNS();
    } else if(work->state==NATIVE_GPU_WAIT_INPUT)work->state=NATIVE_GPU_DISPATCHING;
    work->sealed=seal > 0;
    SDL_CondBroadcast(s_native_gpu_condition);
    SDL_UnlockMutex(s_native_state_mutex);
}

static void native_gpu_discard_phases(GlNativeGpuWork *work, uint32_t checkpoint) {
    if (!work) return;
    work->phase_count = 0u;
    if (work->published_count > checkpoint || work->published_chunk_count) {
        /* A completed phase can already be executing on GL. Its immutable
         * prefix and plane dimensions must survive even an optional audit
         * failure; whole-only scanout ignores the discarded phase images. */
        work->count = work->published_count;
    } else {
        work->count = checkpoint;
        for (uint32_t i = GL_NATIVE_GPU_PHASE_BASE; i < GL_NATIVE_GPU_PLANES; ++i)
            work->widths[i] = work->heights[i] = 0u;
    }
}

static GlNativeGpuCommand *native_gpu_command(GlNativeGpuWork *work, GlNativeGpuOp kind) {
    if (!work || work->failed) return NULL;
    if (work->count == work->capacity) {
        void *commands = work->count == UINT32_MAX ? NULL : work->deferred
            ? xg_render_array_reserve_uninitialized(work->commands, sizeof(*work->commands),
                &work->capacity, work->count + 1u, UINT32_MAX)
            : xg_render_append_reserve(work->commands, sizeof(*work->commands), &work->capacity,
                work->count, work->count + 1u, UINT32_MAX, &work->retired_buffers);
        if (!commands) { native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_JOURNAL, work->count, kind, 0u); work->failed = 1; return NULL; }
        work->commands = commands;
    }
    GlNativeGpuCommand *command = &work->commands[work->count++];
    /* DRAW replaces the entire draw record below; other opcodes never read it. */
    memset(command, 0, offsetof(GlNativeGpuCommand, draw)); command->kind = kind;
    return command;
}

static int native_gpu_data(GlNativeGpuWork *work, const void *data, size_t bytes, uint32_t *offset) {
    if (work->failed || bytes > UINT32_MAX - work->bytes) {
        native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_JOURNAL, work->count, 0u, 0u);
        work->failed = 1; return 0;
    }
    if (work->bytes + bytes > work->data_capacity) {
        /* Private journals have no GL reader yet. Reallocating them avoids
         * retaining and copying every intermediate growth buffer. Published
         * journals keep their immutable prefixes until their fence retires. */
        void *memory = work->deferred
            ? xg_render_array_reserve_uninitialized(work->data, 1u, &work->data_capacity,
                work->bytes + (uint32_t)bytes, UINT32_MAX)
            : xg_render_append_reserve(work->data, 1u, &work->data_capacity,
                work->bytes, work->bytes + (uint32_t)bytes, UINT32_MAX, &work->retired_buffers);
        if (!memory) { native_gpu_fail_note(work, GL_RENDERER_NATIVE_GPU_FAILURE_JOURNAL, work->count, 0u, 0u); work->failed = 1; return 0; }
        work->data = memory;
    }
    *offset = work->bytes;
    memcpy(work->data + work->bytes, data, bytes); work->bytes += (uint32_t)bytes;
    return 1;
}

/* Read back completed GPU raster work only at a CPU dependency boundary.
 * The compile owner sleeps while the GL owner executes the published prefix. */
static int native_gpu_canonical_sync(GlNativeCpuCompiler *compiler) {
    if (!compiler->canonical_dirty) return 1;
    GlNativeGpuWork *work = compiler->gpu;
    if (!work || !work->canonical_gpu || work->deferred) return 0;
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_CANONICAL_READBACK);
    if (!command) return 0;
    command->data = ++work->canonical_requested;
    command->x = compiler->canonical_left & ~63;
    command->y = compiler->canonical_top;
    command->w = (compiler->canonical_right | 63) - command->x + 1;
    command->h = compiler->canonical_bottom - command->y + 1;
    native_gpu_publish_prefix(work, -1);
    SDL_LockMutex(s_native_state_mutex);
    while (work->canonical_completed < work->canonical_requested &&
           !work->render_failed && !work->failed && !work->cancel_requested &&
           SDL_AtomicGet(&s_native_gpu_run) && SDL_AtomicGet(&s_native_gpu_status) > 0)
        SDL_CondWaitTimeout(s_native_gpu_condition, s_native_state_mutex, 10);
    const int ok = work->canonical_completed == work->canonical_requested &&
        !work->render_failed && !work->failed && !work->cancel_requested;
    SDL_UnlockMutex(s_native_state_mutex);
    if (!ok) return 0;
    GlNativeViewState *views = compiler->native_views;
    const unsigned first = (unsigned)compiler->canonical_left / 64u;
    const unsigned last = (unsigned)compiler->canonical_right / 64u;
    const uint16_t mask = (uint16_t)(((1u << (last - first + 1u)) - 1u) << first);
    for (int y = compiler->canonical_top; y <= compiler->canonical_bottom; ++y) {
        for (unsigned b = first; b <= last; ++b) {
            const size_t offset = (size_t)y * VRAM_W + b * 64u;
            for (unsigned x = 0u; x < 64u; ++x) {
                /* GPU alpha carries both the guest mask and AA exemption.
                 * Only its high bit is the guest mask. */
                const uint32_t pixel = work->canonical_pixels[offset + x];
                const uint16_t word = native_rgba_to_1555(pixel & UINT32_C(0x80ffffff));
                compiler->native_vram->words[offset + x] = word;
                compiler->native_vram->pixels[offset + x] = conv_1555_to_rgba8(word);
            }
            uint64_t written = 0u;
            for (unsigned x = 0u; x < 64u; ++x)
                if (work->canonical_provenance[offset + x]) written |= UINT64_C(1) << x;
            uint64_t *previous = &views->raster_words.rows[y][b];
            views->raster_words.nonzero_words += (written != 0u) - (*previous != 0u);
            *previous = written;
            views->word_versions[y][b] = views->write_serial;
            views->word_page_versions[y >> 8][b] = views->write_serial;
        }
        work->dirty_word_rows[y] |= mask;
        work->dirty_texture_pages[y >> 8] |= mask;
    }
    compiler->canonical_dirty = 0;
    return 1;
}

/* GPU raster writes stay resident until a CPU consumer actually samples
 * them. Recipe textures usually live outside the framebuffer: those captures
 * can reuse the exact CPU words without a draw-by-draw GPU readback. */
static int native_gpu_canonical_texture_sync(GlNativeCpuCompiler *compiler,
                                            const GpuRenderSemantic *draw) {
    if (!compiler->canonical_dirty || !draw->material.textured) return 1;
    const GpuRenderMaterial *m = &draw->material;
    GpuVramRect rectangles[4];
    for (unsigned region = 0u; region < (m->texture_depth == GPU_RENDER_TEXTURE_15_BIT ? 1u : 2u); ++region) {
        const int count = gpu_vram_split_transfer(region ? m->clut_x : m->texture_page_x * 64,
            region ? m->clut_y : m->texture_page_y * 256,
            region ? (m->texture_depth == GPU_RENDER_TEXTURE_4_BIT ? 16 : 256) : 64 << m->texture_depth,
            region ? 1 : 256, rectangles);
        for (int i = 0; i < count; ++i) {
            const GpuVramRect *r = &rectangles[i];
            if (r->x <= compiler->canonical_right && compiler->canonical_left < r->x + r->w &&
                r->y <= compiler->canonical_bottom && compiler->canonical_top < r->y + r->h)
                return native_gpu_canonical_sync(compiler);
        }
    }
    return 1;
}

static int native_gpu_preserves_scanout(const GlNativeGpuWork *work) {
    GpuVramRect scanout[4];
    const int regions=gpu_vram_split_transfer(work->scanout_x,work->scanout_y,
        work->scanout_canonical_width,work->scanout_height,scanout);
    for (uint32_t i=0u;i<work->count;++i)
        switch(work->commands[i].kind) {
        case NATIVE_GPU_BASE: case NATIVE_GPU_SNAPSHOT:
        case NATIVE_GPU_WORDS: case NATIVE_GPU_WORDS_RESET:
        case NATIVE_GPU_CANONICAL_READBACK: case NATIVE_GPU_PROVENANCE_CLEAR:
            break;
        case NATIVE_GPU_SEED: {
            const GlNativeGpuCommand *c=&work->commands[i];
            /* Field uploads a 128-word CLUT at y=251 on intervening vblanks.
             * It changes future texture samples, not already rasterized VIEW
             * pixels or the current canonical scanout. No DRAW/copy into a
             * VIEW plane is allowed anywhere in this journal. */
            if(c->plane)return 0;
            for(int r=0;r<regions;++r)
                if(c->x<scanout[r].x+scanout[r].w && scanout[r].x<c->x+c->w &&
                   c->y<scanout[r].y+scanout[r].h && scanout[r].y<c->y+c->h)return 0;
            break;
        }
        default: return 0;
        }
    return 1;
}

static int native_gpu_seed(GlNativeGpuWork *work, uint32_t plane, uint32_t width, uint32_t height,
                       const uint32_t *pixels, uint32_t *depth) {
    if (!work) return 1;
    if (plane >= GL_NATIVE_GPU_PLANES || !width || !height) return 0;
    if (work->widths[plane]) return 1;
    work->widths[plane] = (uint16_t)width; work->heights[plane] = (uint16_t)height;
    if (!work->fresh && plane < GL_NATIVE_GPU_PHASE_BASE && work->base_planes[plane]) return 1;
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_SEED);
    if (!command) return 0;
    /* A whole-plane seed writes far depth on the GPU; keep the CPU plane equal. */
    if (depth) memset(depth, 0, (size_t)width * height * sizeof(*depth));
    command->plane = plane; command->w = width; command->h = height;
    return native_gpu_data(work, pixels, (size_t)width * height * sizeof(*pixels), &command->data);
}

/* Adjacent copies/fills coalesce in logical space. The GPU samples the original
 * high-resolution COPY snapshot, never the already modified destination. */
static int native_gpu_span(GlNativeGpuWork *work, uint32_t plane, int x, int y, int w, int h,
        uint32_t source, float sx, float sy, float sw, float sh, uint32_t color, uint32_t mask) {
    if (!work || w <= 0 || h <= 0) return 1;
    if (work->count>work->published_count) {
        GlNativeGpuCommand *last = &work->commands[work->count - 1u];
        /* Whole-row producers can extend a rectangle without first submitting
         * every individual pixel through the horizontal coalescer. */
        if (last->kind == NATIVE_GPU_SPAN && last->plane == plane && last->source == source &&
            last->mask == mask && last->color == color && last->x == x && last->w == w &&
            last->y + last->h == y && (source == UINT32_MAX ||
                (last->sx == sx && last->sw == sw && last->sy + last->sh == sy &&
                 last->h == last->sh && h == sh))) {
            last->h += h; last->sh += sh;
            return 1;
        }
        if (last->kind == NATIVE_GPU_SPAN && last->plane == plane && last->source == source &&
            last->mask == mask && last->color == color && last->y == y && last->h == h &&
            last->x + last->w == x && (source == UINT32_MAX ||
                (last->sy == sy && last->sh == sh && last->sx + last->sw == sx && last->w == last->sw && w == sw))) {
            last->w += w; last->sw += sw;
            if (work->count > work->published_count+1u) {
                GlNativeGpuCommand *previous = last - 1;
                if (previous->kind == NATIVE_GPU_SPAN && previous->plane == plane && previous->source == source &&
                    previous->color == color && previous->mask == mask && previous->x == last->x &&
                    previous->w == last->w && previous->y + previous->h == last->y &&
                    (source == UINT32_MAX || (previous->sx == last->sx && previous->sw == last->sw &&
                        previous->sy + previous->sh == last->sy && previous->h == previous->sh && last->h == last->sh))) {
                    previous->h += last->h; previous->sh += last->sh; work->count--;
                }
            }
            return 1;
        }
    }
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_SPAN);
    if (!command) return 0;
    command->plane = plane; command->source = source; command->color = color; command->mask = mask;
    command->x = x; command->y = y; command->w = w; command->h = h;
    command->sx = sx; command->sy = sy; command->sw = sw; command->sh = sh;
    return 1;
}

/* One draw of many SPAN rectangles: same plane, source and mask, in order. */
static int native_gpu_spans(GlNativeGpuWork *work, uint32_t plane, uint32_t source, uint32_t mask,
                            const GlNativeGpuSpanRect *rects, uint32_t count) {
    static const uint8_t pad[8];
    uint32_t offset, ignored;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    if (!work || !count) return 1;
    for (uint32_t i = 0u; i < count; ++i) {
        const int x0 = (int)floorf(rects[i].x), y0 = (int)floorf(rects[i].y);
        const int x1 = (int)ceilf(rects[i].x + rects[i].w), y1 = (int)ceilf(rects[i].y + rects[i].h);
        if (x0 < left) left = x0;
        if (y0 < top) top = y0;
        if (x1 > right) right = x1;
        if (y1 > bottom) bottom = y1;
    }
    if ((work->bytes & 7u) && !native_gpu_data(work, pad, 8u - (work->bytes & 7u), &ignored)) return 0;
    if (!native_gpu_data(work, rects, (size_t)count * sizeof(*rects), &offset)) return 0;
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_SPANS);
    if (!command) return 0;
    command->plane = plane; command->source = source; command->mask = mask;
    command->data = offset; command->mesh_count = count;
    command->x = left; command->y = top; command->w = right - left; command->h = bottom - top;
    return 1;
}

static int native_gpu_raw_texture_dirty(const GlNativeGpuWork *work, const XgRenderIrMaterialState *m) {
    const unsigned blocks = 1u << m->texture_depth;
    uint32_t mask = ((1u << blocks) - 1u) << m->texture_page_x;
    mask = (mask | (mask >> 16u)) & 65535u;
    if (work->dirty_texture_pages[m->texture_page_y] & mask)
        for (unsigned y = m->texture_page_y * 256u; y < (m->texture_page_y + 1u) * 256u; ++y)
            if (work->dirty_word_rows[y] & mask) return 1;
    if (m->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT) {
        const unsigned entries = m->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT ? 16u : 256u;
        const unsigned count = ((m->clut_x & 63u) + entries + 63u) / 64u;
        mask = ((1u << count) - 1u) << (m->clut_x / 64u);
        mask = (mask | (mask >> 16u)) & 65535u;
        if (work->dirty_word_rows[m->clut_y] & mask) return 1;
    }
    return 0;
}

static int native_gpu_replay_block(GlNativeGpuWork *work, const uint16_t *words, unsigned row, unsigned block) {
    if (work->replay_block_sources[row][block] == words) return 1;
    const uint32_t offset = row * VRAM_W + block * 64u;
    if (memcmp(work->words + offset, words + offset, 64u * sizeof(*words))) {
        GlNativeGpuCommand *patch = native_gpu_command(work, NATIVE_GPU_WORDS);
        if (!patch) return 0;
        patch->x = block * 64u; patch->y = row; patch->w = 64;
        if (!native_gpu_data(work, words + offset, 64u * sizeof(*words), &patch->data)) return 0;
        memcpy(work->words + offset, words + offset, 64u * sizeof(*words));
        work->replay_page_sources[row / 256u][block] = NULL;
    }
    work->replay_block_sources[row][block] = words;
    return 1;
}

static int native_gpu_replay_texture(GlNativeGpuWork *work, const uint16_t *words,
                                     const XgRenderIrMaterialState *material) {
    if (!words) return 0;
    /* Replay follows canonical FIFO execution and never writes raw VRAM.
     * Synchronize whole texture pages once per snapshot, instead of clipping
     * every triangle to rediscover its texels. Unused raw texels are harmless;
     * destination framebuffer/depth attachments are separate GL resources. */
    for (unsigned page = 0u; page < (1u << material->texture_depth); ++page) {
        const unsigned block = (material->texture_page_x + page) & 15u;
        const unsigned half = material->texture_page_y;
        if (work->replay_page_sources[half][block] == words) continue;
        for (unsigned row = half * 256u; row < (half + 1u) * 256u; ++row)
            if (!native_gpu_replay_block(work, words, row, block)) return 0;
        work->replay_page_sources[half][block] = words;
    }
    if (material->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT) {
        const unsigned entries = material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT ? 16u : 256u;
        const unsigned count = ((material->clut_x & 63u) + entries + 63u) / 64u;
        for (unsigned i = 0u; i < count; ++i)
            if (!native_gpu_replay_block(work, words, material->clut_y,
                    (material->clut_x / 64u + i) & 15u)) return 0;
    }
    return 1;
}

/* Synchronize the raw-word texture with the pages and CLUT one material
 * samples, before the draw that samples them. */
static int native_gpu_sync_texture(GlNativeCpuCompiler *compiler, GlNativeGpuWork *work,
                                   const XgRenderIrMaterialState *m, const int *limits) {
    if (!m->textured) return 1;
    if (compiler->canonical_dirty) {
        DirtyRect dirty = {compiler->canonical_left, compiler->canonical_top,
            compiler->canonical_right, compiler->canonical_bottom, 1};
        const unsigned shift = 2u - m->texture_depth;
        const int bounded = limits && !(m->texture_window_mask_x | m->texture_window_mask_y);
        const int x = m->texture_page_x * 64u + (bounded ? limits[0] >> shift : 0);
        const int y = m->texture_page_y * 256u + (bounded ? limits[1] : 0);
        const int w = bounded ? (limits[2] >> shift) - (limits[0] >> shift) + 1 : 64 << m->texture_depth;
        const int h = bounded ? limits[3] - limits[1] + 1 : 256;
        if (dirty_intersects_wrapped(&dirty, x, y, w, h) ||
            (m->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT &&
             dirty_intersects_wrapped(&dirty, m->clut_x, m->clut_y,
                m->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT ? 16 : 256, 1)))
            if (!native_gpu_canonical_sync(compiler)) return 0;
    }
    if (compiler->gpu_only && compiler->gpu_plane >= GL_NATIVE_GPU_PHASE_BASE)
        return work->phase_textures ? compiler->texture_snapshot != 0u
                                    : native_gpu_replay_texture(work, compiler->native_words, m);
    if (compiler->native_vram && compiler->native_words == compiler->native_vram->words &&
        !native_gpu_raw_texture_dirty(work, m)) return 1;
    if (!compiler->native_words) return 0;
    /* The shader clamps all taps to the triangle's UV range without a texture
     * window. Synchronizing unrelated framebuffer rows within that page adds
     * WORDS commands and breaks otherwise compatible GPU draw batches. */
    const int canonical = compiler->native_vram && compiler->native_words == compiler->native_vram->words;
    const int bounded = canonical && limits && !(m->texture_window_mask_x | m->texture_window_mask_y);
    const unsigned shift = 2u - m->texture_depth;
    const unsigned first_page = bounded ? (unsigned)limits[0] >> (shift + 6u) : 0u;
    const unsigned last_page = bounded ? (unsigned)limits[2] >> (shift + 6u) : (1u << m->texture_depth) - 1u;
    const unsigned first_row = m->texture_page_y * 256u + (bounded ? (unsigned)limits[1] : 0u);
    const unsigned end_row = m->texture_page_y * 256u + (bounded ? (unsigned)limits[3] + 1u : 256u);
    for (unsigned page=first_page;page<=last_page;++page) {
        const unsigned block=(m->texture_page_x+page)&15u;
        const uint16_t mask=(uint16_t)(1u<<block);
        for (unsigned row=first_row;row<end_row;++row) {
            if (canonical && !(work->dirty_word_rows[row]&mask)) continue;
            const uint32_t offset=row*VRAM_W+block*64u;
            if (canonical) work->dirty_word_rows[row]&=(uint16_t)~mask;
            if (!memcmp(work->words+offset,compiler->native_words+offset,64u*sizeof(uint16_t))) continue;
            GlNativeGpuCommand *patch=native_gpu_command(work,NATIVE_GPU_WORDS);
            if (!patch) return 0;
            patch->x=block*64u;patch->y=row;patch->w=64;
            if (!native_gpu_data(work,compiler->native_words+offset,64u*sizeof(uint16_t),&patch->data)) return 0;
            memcpy(work->words+offset,compiler->native_words+offset,64u*sizeof(uint16_t));
            if (!canonical) {
                work->dirty_word_rows[offset/VRAM_W]|=mask;
                work->dirty_texture_pages[offset/(VRAM_W*256u)]|=mask;
            }
        }
        /* A partial-row sync leaves the coarse page dirty for later draws. */
        if (canonical && end_row - first_row == 256u)
            work->dirty_texture_pages[m->texture_page_y]&=(uint16_t)~mask;
    }
    if (m->texture_depth!=XG_RENDER_IR_TEXTURE_15_BIT) {
        const unsigned entries=m->texture_depth==XG_RENDER_IR_TEXTURE_4_BIT?16u:256u;
        const unsigned blocks=((m->clut_x&63u)+entries+63u)/64u;
        for (unsigned i=0u;i<blocks;++i) {
            const unsigned block=(m->clut_x/64u+i)&15u;
            const uint16_t mask=(uint16_t)(1u<<block);
            if (canonical && !(work->dirty_word_rows[m->clut_y]&mask)) continue;
            const uint32_t offset=m->clut_y*VRAM_W+block*64u;
            if (canonical) work->dirty_word_rows[m->clut_y]&=(uint16_t)~mask;
            if (!memcmp(work->words+offset,compiler->native_words+offset,64u*sizeof(uint16_t))) continue;
            GlNativeGpuCommand *patch=native_gpu_command(work,NATIVE_GPU_WORDS);
            if (!patch) return 0;
            patch->x=block*64u;patch->y=m->clut_y;patch->w=64;
            if (!native_gpu_data(work,compiler->native_words+offset,64u*sizeof(uint16_t),&patch->data)) return 0;
            memcpy(work->words+offset,compiler->native_words+offset,64u*sizeof(uint16_t));
            if (!canonical) {
                work->dirty_word_rows[offset/VRAM_W]|=mask;
                work->dirty_texture_pages[offset/(VRAM_W*256u)]|=mask;
            }
        }
    }
    return 1;
}

/* Each immutable draw-time snapshot is uploaded once per source interval.
 * Phase draws select it by index, keeping FIFO sampling exact without replaying
 * thousands of word patches between otherwise batchable triangles. */
typedef struct GlNativeTextureCopyBatch {
    uint16_t *words;
    GlNativeRecipePixels *const *snapshots;
} GlNativeTextureCopyBatch;

static void native_phase_texture_ranges(uint32_t begin, uint32_t end, void *context) {
    const GlNativeTextureCopyBatch *batch = context;
    for (uint32_t i = begin; i < end; ++i)
        memcpy(batch->words + (size_t)i * VRAM_W * VRAM_H, batch->snapshots[i]->words,
            (size_t)VRAM_W * VRAM_H * sizeof(uint16_t));
}

static GlNativePhaseTextures *native_phase_textures_prepare(const GlNativeRecipe *recipe) {
    if (!s_native_gpu_snapshot_capacity) return NULL;
    GlNativePhaseTextures *plan = calloc(1u, sizeof(*plan));
    if (!plan) return NULL;
    const size_t records = recipe->count ? recipe->count : 1u;
    GlNativeRecipePixels **snapshots = malloc(records * sizeof(*snapshots));
    plan->layers = malloc(records * sizeof(*plan->layers));
    if (!snapshots || !plan->layers) goto failed;
    for (uint32_t i = 0u; i < recipe->count; ++i) {
        GlNativeRecipePixels *pixels = recipe->draws[i].textures;
        plan->layers[i] = 0u;
        if (!pixels) continue;
        uint32_t layer = 0u;
        while (layer < plan->count && snapshots[layer] != pixels) ++layer;
        if (layer == plan->count) {
            if (plan->count == s_native_gpu_snapshot_capacity) goto failed;
            snapshots[plan->count++] = pixels;
        }
        plan->layers[i] = layer + 1u;
    }
    if (plan->count) {
        const size_t bytes = (size_t)VRAM_W * VRAM_H * sizeof(uint16_t);
        plan->words = malloc(bytes * plan->count);
        if (!plan->words) goto failed;
        GlNativeTextureCopyBatch batch = {plan->words, snapshots};
        psx_parallel_for(plan->count, 1u, native_phase_texture_ranges, &batch);
    }
    /* GL consumes owned bytes, never compiler-owned recipe refcounts. A
     * departure recipe can be released immediately after phase capture. */
    free(snapshots);
    return plan;
failed:
    free(snapshots); free(plan->words); free(plan->layers); free(plan);
    return NULL;
}

/* Journal-level batching. A DRAW whose render state equals that of the
 * latest unpublished DRAW on its plane, but for its texture state, appends
 * its triangles (each with its own texture state) to that command, which
 * becomes (or stays) a mesh in the data blob: GL executes the same
 * triangles in the same per-plane order, with the shared state evaluated
 * once per texture change. The GL owner already groups a pure DRAW run by
 * plane, so looking back over other planes' draws keeps every plane's own
 * order. A texture sync between two draws ends the run (WORDS is not a
 * DRAW). Canonical triangles can share this ordered batch too: their texture
 * dependencies were synchronized before appending, and a WORDS patch still
 * closes the run before any dependent triangle. */
static int native_gpu_draw_state_equal(const XgSemanticDrawRecord *a, const XgSemanticDrawRecord *b,
                                       uint8_t dither) {
    const XgRenderIrMaterialState *x = &a->primitive.material, *y = &b->primitive.material;
    return a->topology == GPU_RENDER_SEMANTIC_TRIANGLES && b->topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
        !a->line_count && !b->line_count &&
        x->draw_area_left == y->draw_area_left && x->draw_area_top == y->draw_area_top &&
        x->draw_area_right == y->draw_area_right && x->draw_area_bottom == y->draw_area_bottom &&
        x->draw_offset_x == y->draw_offset_x && x->draw_offset_y == y->draw_offset_y &&
        x->texture_window_mask_x == y->texture_window_mask_x &&
        x->texture_window_mask_y == y->texture_window_mask_y &&
        x->texture_window_offset_x == y->texture_window_offset_x &&
        x->texture_window_offset_y == y->texture_window_offset_y &&
        x->dither == (dither != 0u) && x->mask_set == y->mask_set && x->mask_check == y->mask_check &&
        a->primitive.depth_policy == b->primitive.depth_policy &&
        a->primitive.depth_bias == b->primitive.depth_bias &&
        a->primitive.aa_exempt == b->primitive.aa_exempt &&
        a->primitive.sprite_texture == b->primitive.sprite_texture &&
        a->screen_space_2d == b->screen_space_2d && a->aa_exempt == b->aa_exempt &&
        a->sprite_texture == b->sprite_texture && a->native_view_effect == b->native_view_effect &&
        a->native_view_effect_index == b->native_view_effect_index &&
        !memcmp(&a->hd_texture, &b->hd_texture, sizeof(a->hd_texture));
}

static GlNativeGpuCommand *native_gpu_coalesce_target(GlNativeGpuWork *work, uint32_t plane,
                                                      const XgSemanticDrawRecord *draw, uint8_t dither,
                                                      int x, int y) {
    if (work->capture_templates) return NULL;
    for (uint32_t i = work->count, seen = 0u; i-- > work->published_count && seen < 8u; ++seen) {
        GlNativeGpuCommand *command = &work->commands[i];
        if (command->kind != NATIVE_GPU_DRAW || command->source) return NULL;
        if (command->plane != plane) continue;
        return command->x == x && command->y == y &&
            native_gpu_draw_state_equal(&command->draw, draw, dither) ? command : NULL;
    }
    return NULL;
}

static int native_gpu_mesh_data(GlNativeGpuWork *work, const GlNativeGpuMeshTriangle *triangles,
                                uint32_t count, uint32_t *offset) {
    /* 8-byte aligned triangles in this journal's blob. */
    static const uint8_t pad[8];
    uint32_t ignored;
    if ((work->bytes & 7u) && !native_gpu_data(work, pad, 8u - (work->bytes & 7u), &ignored)) return 0;
    return native_gpu_data(work, triangles, (size_t)count * sizeof(*triangles), offset);
}

static int native_gpu_coalesce_append(GlNativeGpuWork *work, GlNativeGpuCommand *run,
                                      const GlNativeGpuMeshTriangle *triangles, uint32_t count) {
    uint32_t offset;
    if (!count) return 1;
    if (run->mesh_count && run->data + (size_t)run->mesh_count * sizeof(*triangles) != work->bytes) {
        /* Another mesh was stored after this run: start a new command. */
        GlNativeGpuCommand copy = *run;
        if (!native_gpu_mesh_data(work, triangles, count, &offset)) return 0;
        GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_DRAW);
        if (!command) return 0;
        *command = copy;
        command->data = offset; command->mesh_count = count;
        return 1;
    }
    if (!run->mesh_count) {
        /* Move the run's inline triangles to the blob first. */
        GlNativeGpuMeshTriangle inline_triangles[XG_RENDER_IR_TRIANGLE_CAPACITY];
        const uint32_t inline_count = run->draw.primitive.triangle_count;
        memset(inline_triangles, 0, sizeof(inline_triangles));
        for (uint32_t t = 0u; t < inline_count; ++t) {
            inline_triangles[t].triangle = run->draw.primitive.triangles[t];
            inline_triangles[t].texture = native_texture_state(&run->draw.primitive.material);
        }
        if (!native_gpu_mesh_data(work, inline_triangles, inline_count, &offset)) return 0;
        run->data = offset;
        run->mesh_count = inline_count;
        run->draw.primitive.triangle_count = 0u;
    }
    if (!native_gpu_data(work, triangles, (size_t)count * sizeof(*triangles), &offset)) return 0;
    run->mesh_count += count;
    return 1;
}

/* The depth view's key range, now that no CPU raster writes fragments:
 * the FP32 keys (4096 / Q12 view Z, as native_depth_plane) of the on-screen
 * corners of the endpoint's depth-writing triangles. */
static void native_gpu_note_depth_range(GlNativeCpuCompiler *compiler, const XgSemanticDrawRecord *draw,
                                        const GlNativeGpuMeshTriangle *mesh, uint32_t mesh_count) {
    if (!compiler->audit || compiler->gpu_plane == 0u || compiler->gpu_plane >= GL_NATIVE_GPU_PHASE_BASE ||
        !compiler->native_views || !compiler->native_views->depth_test ||
        draw->primitive.depth_policy != GPU_RENDER_DEPTH_TEST_WRITE) return;
    const uint32_t count = mesh ? mesh_count : draw->primitive.triangle_count;
    const int32_t width = (int32_t)compiler->viewport_width, height = (int32_t)compiler->viewport_height;
    for (uint32_t t = 0u; t < count; ++t) {
        const XgRenderIrTriangle *triangle = mesh ? &mesh[t].triangle : &draw->primitive.triangles[t];
        if (mesh ? mesh[t].texture.semi_transparent : draw->primitive.material.semi_transparent) continue;
        for (unsigned v = 0u; v < 3u; ++v) {
            const XgRenderIrVertex *vertex = &triangle->vertices[v];
            const int32_t x = vertex->x >> 16, y = vertex->y >> 16;
            if (x < 0 || y < 0 || (width && x >= width) || (height && y >= height)) continue;
            const int32_t z = native_vertex_depth_q12(vertex);
            if (z <= 0) continue;
            const uint32_t key = native_depth_encode((float)(4096.0 / z));
            if (!compiler->audit->depth_key_min || key < compiler->audit->depth_key_min)
                compiler->audit->depth_key_min = key;
            if (key > compiler->audit->depth_key_max) compiler->audit->depth_key_max = key;
        }
    }
}

static int native_gpu_draw(GlNativeCpuCompiler *compiler, const XgSemanticDrawRecord *draw, int origin_y) {
    return native_gpu_draw_mesh(compiler, draw, origin_y, NULL, 0u);
}

static int native_gpu_draw_mesh(GlNativeCpuCompiler *compiler, const XgSemanticDrawRecord *draw,
                                int origin_y, const GlNativeGpuMeshTriangle *mesh, uint32_t mesh_count) {
    GlNativeGpuWork *work = compiler->gpu;
    const XgRenderIrMaterialState *m = &draw->primitive.material;
    if (!work) return 1;
    if (mesh) {
        XgRenderIrMaterialState material = *m;
        for (uint32_t t = 0u; t < mesh_count; ++t) {
            if (t && native_texture_state_equal(&mesh[t].texture, &mesh[t - 1u].texture)) continue;
            native_texture_state_apply(&material, &mesh[t].texture);
            if (!native_gpu_sync_texture(compiler, work, &material, NULL)) return 0;
        }
    } else {
        int limits[4], *footprint = NULL;
        if (m->textured && !(m->texture_window_mask_x | m->texture_window_mask_y) &&
            draw->topology == GPU_RENDER_SEMANTIC_TRIANGLES && draw->primitive.triangle_count) {
            int lo_u = INT_MAX, lo_v = INT_MAX, hi_u = INT_MIN, hi_v = INT_MIN;
            for (uint32_t t = 0u; t < draw->primitive.triangle_count; ++t)
                for (unsigned v = 0u; v < 3u; ++v) {
                    const XgRenderIrVertex *vertex = &draw->primitive.triangles[t].vertices[v];
                    const int u = native_fixed_floor(vertex->u), vv = native_fixed_floor(vertex->v);
                    if (u < lo_u) lo_u = u; if (u > hi_u) hi_u = u;
                    if (vv < lo_v) lo_v = vv; if (vv > hi_v) hi_v = vv;
                }
            /* No exclusive-edge tightening: this bounds every triangle's
             * shader clamp, including mirrored mappings. Wrapping uses the
             * full axis, exactly as gpu_uv.h does for its sample limits. */
            psx_uv_axis_limits(lo_u, hi_u, 0, &limits[0], &limits[2]);
            psx_uv_axis_limits(lo_v, hi_v, 0, &limits[1], &limits[3]);
            footprint = limits;
        }
        if (!native_gpu_sync_texture(compiler, work, m, footprint)) return 0;
    }
    /* A framebuffer capture is a VIEW texture, including the reveal margins
     * and every internal-resolution sample. Ordinary textures use raw words.
     * The transfer journal separates source-plane writes from these draws. */
    uint32_t source = 0u;
    float sx = 0.f, sy = 0.f, sw = 0.f, sh = 0.f;
    if (compiler->view_pass && compiler->native_views) {
        GlNativeViewState *views = compiler->native_views;
        const int index = native_transition_texture(views, draw);
        if (index >= 0) {
            GlNativeViewTarget *view = &views->targets[index];
            const uint32_t domain = native_view_domain(views, view);
            if (domain != UINT32_MAX && domain + 1u != compiler->gpu_plane) {
                source = domain + 1u;
                sx = m->texture_page_x * 64 - view->x;
                sy = m->texture_page_y * 256;
                sw = view->width;
                sh = VRAM_H;
            }
        }
    }
    const uint8_t dither = draw->primitive.material.dither && !compiler->dithering_disabled;
    native_gpu_note_depth_range(compiler, draw, mesh, mesh_count);
    if (!source) {
        GlNativeGpuCommand *run = native_gpu_coalesce_target(work, compiler->gpu_plane, draw, dither,
            compiler->dither_x, origin_y);
        if (run && run->texture_snapshot == compiler->texture_snapshot) {
            if (mesh) return native_gpu_coalesce_append(work, run, mesh, mesh_count);
            GlNativeGpuMeshTriangle triangles[XG_RENDER_IR_TRIANGLE_CAPACITY];
            memset(triangles, 0, sizeof(triangles));
            for (uint32_t t = 0u; t < draw->primitive.triangle_count; ++t) {
                triangles[t].triangle = draw->primitive.triangles[t];
                triangles[t].texture = native_texture_state(m);
            }
            return native_gpu_coalesce_append(work, run, triangles, draw->primitive.triangle_count);
        }
    }
    uint32_t mesh_offset = 0u;
    if (mesh && !native_gpu_mesh_data(work, mesh, mesh_count, &mesh_offset)) return 0;
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_DRAW);
    if (!command) return 0;
    command->plane = compiler->gpu_plane; command->draw = *draw;
    command->texture_snapshot = compiler->texture_snapshot;
    command->template_record = work->template_record;
    if (mesh) {
        command->data = mesh_offset;
        command->mesh_count = mesh_count;
        command->draw.primitive.triangle_count = 0u;
    }
    command->source = source;
    command->sx = sx; command->sy = sy; command->sw = sw; command->sh = sh;
    command->draw.primitive.material.dither = dither;
    command->y = origin_y; command->x = compiler->dither_x;
    return 1;
}

static int native_gpu_pixel(GlNativeGpuWork *work, uint32_t plane, int x, int y, uint32_t color, uint32_t mask) {
    if (!work) return 1;
    uint32_t offset;
    if (!native_gpu_data(work, &color, sizeof(color), &offset)) return 0;
    if (work->count>work->published_count) {
        GlNativeGpuCommand *last = &work->commands[work->count - 1u];
        if (last->kind == NATIVE_GPU_SEED && last->plane == plane && last->mask == mask &&
            last->y == y && last->h == 1 && last->x + last->w == x &&
            last->data + (uint32_t)last->w * sizeof(color) == offset) {
            last->w++;
            if (work->count > work->published_count+1u) {
                GlNativeGpuCommand *previous = last - 1;
                if (previous->kind == NATIVE_GPU_SEED && previous->plane == plane && previous->mask == mask &&
                    previous->x == last->x && previous->w == last->w && previous->y + previous->h == y &&
                    previous->data + (size_t)previous->w * previous->h * sizeof(color) == last->data) {
                    previous->h++; work->count--;
                }
            }
            return 1;
        }
    }
    GlNativeGpuCommand *command = native_gpu_command(work, NATIVE_GPU_SEED);
    if (!command) return 0;
    command->plane = plane; command->x = x; command->y = y; command->w = command->h = 1;
    command->data = offset; command->mask = mask;
    return 1;
}

static int native_recipe_feedback_reads(const GlNativeRecipe *recipe, const GlNativeRecipeReads *reads) {
    if (!gpu_vram_region_any(&recipe->feedback)) return 0;
    for (uint32_t i = 0u; i < reads->words.nonzero_words; ++i) {
        const uint32_t y = reads->indices[i] / GPU_VRAM_REGION_WORDS_PER_ROW;
        const uint32_t word = reads->indices[i] % GPU_VRAM_REGION_WORDS_PER_ROW;
        if (reads->words.rows[y][word] & recipe->feedback.rows[y][word]) return 1;
    }
    return 0;
}

static int native_recipe_unchanged_region(GlNativeRecipe *recipe, const GlNativeViewTarget *target,
                                      const GlNativeViewState *views, int x, int y, int width, int height) {
    GpuVramRect rects[4];
    const int count = gpu_vram_split_transfer(x, y, width, height, rects);
    /* Most display-page reads need the exact footprint regardless of version:
     * reject the cheap rectangle overlap before scanning the feedback bitmap. */
    for (int r = 0; r < count; ++r) {
        const GpuVramRect *box = &rects[r];
        if (box->x < target->x + target->width && target->x < box->x + box->w &&
            box->y < target->y + target->height && target->y < box->y + box->h) return 0;
    }
    if (native_recipe_feedback_region(recipe, x, y, width, height)) return 0;
    for (int r = 0; r < count; ++r) {
        const GpuVramRect *box = &rects[r];
        for (int block = box->x / 64; block <= (box->x + box->w - 1) / 64; ++block) {
            /* The page maximum proves that none of its rows changed. Only
             * pages with a newer write need the exact row-by-row fallback. */
            for (int half = box->y / 256; half <= (box->y + box->h - 1) / 256; ++half) {
                if (views->word_page_versions[half][block] <= recipe->textures->write_serial) continue;
                const int first = box->y > half * 256 ? box->y : half * 256;
                const int last = box->y + box->h < (half + 1) * 256 ? box->y + box->h : (half + 1) * 256;
                for (int row = first; row < last; ++row)
                    if (views->word_versions[row][block] > recipe->textures->write_serial) return 0;
            }
        }
    }
    return 1;
}

static int native_recipe_textures(GlNativeRecipe *recipe, const GlNativeViewTarget *target,
                               const uint16_t *words, GpuRenderSemantic *draw, uint32_t scale,
                               const GlNativeViewState *views, int mesh) {
    if (!draw->material.textured) return 1;
    const GpuRenderMaterial *m = &draw->material;
    if (recipe->textures && native_recipe_unchanged_region(recipe, target, views,
            m->texture_page_x * 64, m->texture_page_y * 256, 64 << m->texture_depth, 256) &&
        (m->texture_depth == GPU_RENDER_TEXTURE_15_BIT || native_recipe_unchanged_region(recipe, target, views,
            m->clut_x, m->clut_y, m->texture_depth == GPU_RENDER_TEXTURE_4_BIT ? 16 : 256, 1))) return 1;
    s_native_recipe_texture_read = (GlNativeRecipeDropEvent){.material = draw->material, .texture_reason = "none"};
    int changed = !recipe->textures;
    static GlNativeRecipeReads reads;
    if (mesh) native_recipe_mesh_reads(draw, &reads);
    else native_recipe_texture_reads(draw, &reads, scale, 0, recipe);
    const int feedback = native_recipe_feedback_reads(recipe, &reads) ||
        native_recipe_region(&reads.words, target->x, target->y, target->width, target->height);
    for (uint32_t i = 0u; i < reads.words.nonzero_words; ++i) {
        const uint32_t y = reads.indices[i] / GPU_VRAM_REGION_WORDS_PER_ROW;
        const uint32_t word = reads.indices[i] % GPU_VRAM_REGION_WORDS_PER_ROW;
        uint64_t bits = reads.words.rows[y][word];
        while (bits) {
            unsigned bit = 0u;
#if defined(_MSC_VER)
            while (!(bits & (UINT64_C(1) << bit))) ++bit;
#else
            bit = (unsigned)__builtin_ctzll(bits);
#endif
            if (feedback) {
                if (!native_recipe_word(recipe, target, words, (int)(word * 64u + bit), (int)y, &changed)) return 0;
                bits &= bits - 1u;
                continue;
            }
            if (changed) break;
            unsigned length = 0u;
            uint64_t run = bits >> bit;
#if defined(_MSC_VER)
            while (run & 1u) { ++length; run >>= 1u; }
#else
            length = ~run ? (unsigned)__builtin_ctzll(~run) : 64u;
#endif
            const size_t offset = (size_t)y * VRAM_W + word * 64u + bit;
            changed = memcmp(recipe->textures->words + offset, words + offset,
                length * sizeof(uint16_t)) != 0;
            bits &= ~((length == 64u ? UINT64_MAX : (UINT64_C(1) << length) - 1u) << bit);
        }
    }
    if (changed) {
        if (recipe->texture_count == UINT32_MAX) {
            s_native_recipe_texture_read.texture_reason = "snapshot_count_overflow"; return 0;
        }
        GlNativeRecipePixels *pixels = malloc(sizeof(*pixels));
        if (!pixels) { s_native_recipe_texture_read.texture_reason = "snapshot_allocation"; return 0; }
        pixels->references = 1u;
        /* Recipe capture precedes this operation's canonical raster. Its writes
         * use write_serial and must invalidate this snapshot, too. */
        pixels->write_serial = views->write_serial - 1u;
        memcpy(pixels->words, words, sizeof(pixels->words));
        native_recipe_pixels_release(recipe->textures);
        recipe->textures = pixels;
        recipe->texture_count++;
    }
    return 1;
}

static int native_motion_ref_equal(XgRenderMotionRef a, XgRenderMotionRef b) {
    return a.handle.resource_id == b.handle.resource_id &&
        a.handle.generation == b.handle.generation && a.digest == b.digest;
}

static const XgRenderTemporalComponent *native_coverage_component(const XgRenderTemporalCoverageView *view, uint64_t id) {
    uint32_t lo = 0u, hi = view->header->component_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (view->components[mid].component_id < id) lo = mid + 1u; else hi = mid;
    }
    return lo < view->header->component_count && view->components[lo].component_id == id
        ? &view->components[lo] : NULL;
}

/* Consecutive pose-bound guest draws that differ only in their polygons
 * replay as one MESH record: one phase loop and one GPU command per run
 * instead of a record, a projection slot and a command per polygon. The
 * per-polygon path projects the same LOCAL corners under the same pose;
 * runs never cross a material, texture snapshot or binding change, so the
 * OT order and every replayed triangle stay exactly as appended. */
static int native_recipe_coalescible(const GlNativeRecipeDraw *record) {
    const GpuRenderSemantic *s = &record->semantic;
    return record->motion.motion.handle.resource_id && !record->temporal_departure &&
        !record->temporal_component && s->topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
        !s->screen_space_2d && !s->native_view_effect && !s->line_count &&
        (record->mesh ? record->mesh->relocated
                      : s->triangle_count && s->triangle_count == record->motion.triangle_count);
}

static GlNativeRecipeMeshPose native_recipe_record_pose(const GlNativeRecipeDraw *record) {
    const GpuRenderInterpolationIdentity *owner = &record->semantic.interpolation_identity;
    return (GlNativeRecipeMeshPose){.scene_id = owner->scene_id, .motion_index = record->motion_index,
        .part = record->motion.motion_part_index, .producer_id = owner->producer_id,
        .identity_valid = owner->valid};
}

static void native_recipe_mesh_triangle(XgRenderNativeMeshTriangle *out,
                                        const GpuRenderSemanticTriangle *triangle,
                                        const XgRenderMotionDrawBinding *binding, uint32_t t) {
    memset(out, 0, sizeof(*out));
    native_materialize_triangle(triangle, &out->triangle);
    memcpy(out->local, binding->local[t], sizeof(out->local));
    memcpy(out->vertex_ids, binding->vertex_ids[t], sizeof(out->vertex_ids));
}

/* A lone pose-bound draw as a one-polygon run: it takes the mesh replay,
 * which clips at each phase's near plane, and later draws can join it. */
static int native_recipe_meshify(GlNativeRecipeDraw *record) {
    const GpuRenderSemantic *a = &record->semantic;
    const uint32_t count = a->triangle_count, capacity = 16u;
    GlNativeRecipeMesh *mesh = malloc(sizeof(*mesh) + (size_t)capacity * sizeof(mesh->triangles[0]));
    GlNativeTextureState *textures = malloc((size_t)capacity * sizeof(*textures));
    GlNativeRecipeMeshPose *poses = malloc((size_t)capacity * sizeof(*poses));
    if (!mesh || !textures || !poses) { free(mesh); free(textures); free(poses); return 0; }
    const GlNativeTextureState texture = native_texture_state_semantic(&a->material);
    const GlNativeRecipeMeshPose pose = native_recipe_record_pose(record);
    for (uint32_t t = 0u; t < count; ++t) {
        native_recipe_mesh_triangle(&mesh->triangles[t], &a->triangles[t], &record->motion, t);
        textures[t] = texture;
        poses[t] = pose;
    }
    *mesh = (GlNativeRecipeMesh){.references = 1u, .count = count, .capacity = capacity,
        .relocated = 1u, .textures = textures, .poses = poses};
    /* The record is now the run's template: material and binding identity
     * only, its polygons live in the mesh. */
    record->semantic.triangle_count = 0u;
    memset(record->semantic.triangles, 0, sizeof(record->semantic.triangles));
    record->motion.triangle_count = 0u;
    memset(record->motion.local, 0, sizeof(record->motion.local));
    memset(record->motion.vertex_ids, 0, sizeof(record->motion.vertex_ids));
    record->mesh = mesh;
    return 1;
}

static int native_recipe_coalesce(GlNativeRecipe *recipe, const GlNativeRecipeDraw *next) {
    if (!recipe->count || next->mesh || !native_recipe_coalescible(next)) return 0;
    GlNativeRecipeDraw *last = &recipe->draws[recipe->count - 1u];
    const GpuRenderSemantic *a = &last->semantic, *b = &next->semantic;
    const GpuRenderMaterial *x = &a->material, *y = &b->material;
    if (!native_recipe_coalescible(last) ||
        /* Texture state may differ per triangle; the rest of the material
         * (clip, offsets, window, dither, mask) is the record's. */
        x->draw_area_left != y->draw_area_left || x->draw_area_top != y->draw_area_top ||
        x->draw_area_right != y->draw_area_right || x->draw_area_bottom != y->draw_area_bottom ||
        x->draw_offset_x != y->draw_offset_x || x->draw_offset_y != y->draw_offset_y ||
        x->texture_window_mask_x != y->texture_window_mask_x ||
        x->texture_window_mask_y != y->texture_window_mask_y ||
        x->texture_window_offset_x != y->texture_window_offset_x ||
        x->texture_window_offset_y != y->texture_window_offset_y ||
        x->dither != y->dither || x->mask_set != y->mask_set || x->mask_check != y->mask_check ||
        a->depth_policy != b->depth_policy || a->depth_bias != b->depth_bias ||
        a->aa_exempt != b->aa_exempt || a->sprite_texture != b->sprite_texture ||
        /* Pose and owner identity may differ per triangle (poses). */
        last->temporal_index != next->temporal_index ||
        last->textures != next->textures || last->enhanced != next->enhanced ||
        last->projection_offset_x != next->projection_offset_x ||
        last->projection_offset_y != next->projection_offset_y ||
        last->view_origin_y != next->view_origin_y ||
        last->dither_x != next->dither_x || last->dither_y != next->dither_y ||
        memcmp(&last->hd_texture, &next->hd_texture, sizeof(last->hd_texture))) return 0;
    if (!last->mesh && !native_recipe_meshify(last)) return 0;
    GlNativeRecipeMesh *mesh = last->mesh;
    const uint32_t count = mesh->count;
    const uint32_t needed = count + b->triangle_count;
    if (mesh->references != 1u || needed > mesh->capacity) {
        /* Grow in place only while this recipe alone holds the mesh; a
         * copy-on-write recipe shares it with its retained original. */
        const int owned = mesh->references == 1u;
        uint32_t capacity = owned ? mesh->capacity : 16u;
        while (capacity < needed) capacity *= 2u;
        const size_t bytes = sizeof(*mesh) + (size_t)capacity * sizeof(mesh->triangles[0]);
        GlNativeTextureState *textures = owned
            ? realloc(mesh->textures, (size_t)capacity * sizeof(*textures))
            : malloc((size_t)capacity * sizeof(*textures));
        if (!textures) return 0;
        if (owned) mesh->textures = textures;
        GlNativeRecipeMeshPose *poses = owned
            ? realloc(mesh->poses, (size_t)capacity * sizeof(*poses))
            : malloc((size_t)capacity * sizeof(*poses));
        if (!poses) { if (!owned) free(textures); return 0; }
        if (owned) mesh->poses = poses;
        GlNativeRecipeMesh *grown = owned ? realloc(mesh, bytes) : malloc(bytes);
        if (!grown) { if (!owned) { free(textures); free(poses); } return 0; }
        grown->textures = textures;
        grown->poses = poses;
        if (!owned) {
            memcpy(grown->triangles, mesh->triangles, (size_t)count * sizeof(mesh->triangles[0]));
            memcpy(textures, mesh->textures, (size_t)count * sizeof(*textures));
            memcpy(poses, mesh->poses, (size_t)count * sizeof(*poses));
            native_recipe_mesh_release(mesh);
        }
        grown->references = 1u; grown->count = count;
        grown->capacity = capacity; grown->relocated = 1u;
        last->mesh = mesh = grown;
    }
    const GlNativeTextureState texture = native_texture_state_semantic(&b->material);
    const GlNativeRecipeMeshPose pose = native_recipe_record_pose(next);
    for (uint32_t t = 0u; t < b->triangle_count; ++t) {
        mesh->textures[mesh->count] = texture;
        mesh->poses[mesh->count] = pose;
        native_recipe_mesh_triangle(&mesh->triangles[mesh->count++], &b->triangles[t], &next->motion, t);
    }
    return 1;
}

static void native_recipe_append(GlNativeViewState *views, uint32_t index,
                              const XgRenderNativeOperation *operation, const GlNativeCompileAudit *audit,
                              const uint16_t *words,
                              int enhanced, GlNativeRecipeMesh *expanded) {
    const GpuRenderSemantic *source = &operation->semantic;
    const XgRenderMotionDrawBinding *binding = &operation->motion;
    GlNativeViewTarget *target = &views->targets[index];
    GlNativeRecipe *recipe = target->recipe;
    if (!recipe) return;
    GpuRenderSemantic draw = *source;
    GpuRenderMaterial *m = &draw.material;
    int left = m->draw_area_left > target->x ? m->draw_area_left : target->x;
    int right = m->draw_area_right < target->x + target->width - 1
        ? m->draw_area_right : target->x + target->width - 1;
    int top = m->draw_area_top > target->y ? m->draw_area_top : target->y;
    int bottom = m->draw_area_bottom < target->y + target->height - 1
        ? m->draw_area_bottom : target->y + target->height - 1;
    if (left > right || top > bottom) return;
    s_native_recipe_texture_read = (GlNativeRecipeDropEvent){.material = *m, .texture_reason = "none"};
    /* A recipe is only ever appended to or fully dropped, never truncated.
     * native_recipe_drop normally fires often enough (layout changes, motion
     * binding changes, ...) to keep it small, but a scene that never hits one
     * of those (a world-map battle) lets it grow past 100k draws. Combined
     * with native_recipe_private()'s copy-on-write — which copies the entire
     * history on every draw whenever references > 1, a routine state — cost
     * compounds with every draw ever appended. Cap it like the existing
     * UINT32_MAX/native_view_effect drops below. */
    /* Coalesced runs pack a frame into few records, so the record cap alone
     * would let such a recipe span many frames; every frame retains its own
     * poses, which copy-on-write and release then walk. Cap those too. */
    enum { XG_NATIVE_RECIPE_MAX_DRAWS = 16384u, XG_NATIVE_RECIPE_MAX_MOTIONS = 1024u };
    if (source->native_view_effect || recipe->count == UINT32_MAX ||
        recipe->count >= XG_NATIVE_RECIPE_MAX_DRAWS ||
        recipe->motion_count >= XG_NATIVE_RECIPE_MAX_MOTIONS) {
        s_native_recipe_texture_read.texture_reason = source->native_view_effect ? "native_view_effect" :
            recipe->count >= XG_NATIVE_RECIPE_MAX_DRAWS ? "draw_capacity" : "motion_capacity";
        native_recipe_drop(target); return;
    }
    const int mesh = operation->kind == XG_RENDER_NATIVE_OPERATION_MESH;
    if (binding->motion.handle.resource_id) {
        int retained = 0;
        for (uint32_t i = 0u; i < audit->consumed_motion_resources; ++i)
            retained |= native_motion_ref_equal(binding->motion, audit->motion_resources[i]);
        if (!retained || !(mesh ? xg_render_motion_mesh_binding_valid(binding)
                                : xg_render_motion_binding_valid(binding)) ||
            source->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
            binding->triangle_count != source->triangle_count) {
            s_native_recipe_texture_read.texture_reason = "motion_binding";
            native_recipe_drop(target); return;
        }
    }
    const int64_t dx = (int64_t)(m->draw_offset_x - target->x) * 65536;
    const int64_t dy = (int64_t)(m->draw_offset_y - target->y) * 65536;
    m->draw_area_left = (uint16_t)(left - target->x);
    m->draw_area_right = (uint16_t)(right - target->x);
    m->draw_area_top = (uint16_t)(top - target->y);
    m->draw_area_bottom = (uint16_t)(bottom - target->y);
    m->draw_offset_x = m->draw_offset_y = 0;
    draw.submission_command_id = 0u;
    if (!enhanced && recipe->view_width) draw.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
    for (uint32_t i = 0u; i < native_recipe_vertex_count(&draw); ++i) {
        GpuRenderSemanticVertex *v = native_recipe_vertex(&draw, i);
#define NATIVE_RELOCATE(field, delta) do { \
    const int64_t relocated = (int64_t)v->field + (delta); \
    if (relocated < INT32_MIN || relocated > INT32_MAX) { native_recipe_drop(target); return; } \
    v->field = (int32_t)relocated; \
} while (0)
        NATIVE_RELOCATE(x, dx); NATIVE_RELOCATE(y, dy);
        if (v->native_view_position) {
            NATIVE_RELOCATE(native_view_x, dx); NATIVE_RELOCATE(native_view_y, dy);
        }
        if (v->projective_position) {
            NATIVE_RELOCATE(projective_offset_x, dx); NATIVE_RELOCATE(projective_offset_y, dy);
        }
#undef NATIVE_RELOCATE
        if (!enhanced && recipe->view_width) v->native_view_position = 0u;
    }
    if (!native_recipe_private(target)) return;
    recipe = target->recipe;
    /* Copy-on-write and append fully assign every live record. Unused
     * capacity has no reader and needs no repeated zero fill. */
    GlNativeRecipeDraw *draws = xg_render_array_reserve_uninitialized(recipe->draws, sizeof(*draws),
        &recipe->draw_capacity, recipe->count + 1u, UINT32_MAX);
    if (!draws) { native_recipe_drop(target); return; }
    recipe->draws = draws;
    if (recipe->publication != views->publication) {
        native_coverage_release(recipe->publication);
        recipe->publication = views->publication;
        if (recipe->publication) recipe->publication->references++;
    }
    recipe->dithering_disabled = audit->header.display.dithering_disabled;
    recipe->depth_test = audit->header.display.native_depth_test;
    if (!native_recipe_textures(recipe, target, words, &draw, audit->header.display.render_scale, views, mesh)) { native_recipe_drop(target); return; }
    uint32_t motion_index = UINT32_MAX;
    if (binding->motion.handle.resource_id) {
        for (uint32_t i = 0u; i < recipe->motion_count; ++i)
            if (native_motion_ref_equal(binding->motion, recipe->motions[i])) { motion_index = i; break; }
        if (motion_index == UINT32_MAX) {
            XgRenderMotionRef *motions = recipe->motion_count == UINT32_MAX ? NULL :
                xg_render_array_reserve(recipe->motions, sizeof(*motions),
                    &recipe->motion_capacity, recipe->motion_count + 1u, UINT32_MAX);
            if (!motions) { native_recipe_drop(target); return; }
            recipe->motions = motions;
            if (xg_render_resource_acquire_snapshot(binding->motion.handle, binding->motion.digest) != XG_RENDER_RESOURCE_OK) {
                native_recipe_drop(target); return;
            }
            motion_index = recipe->motion_count++;
            recipe->motions[motion_index] = binding->motion;
        }
    }
    uint32_t temporal_index = UINT32_MAX;
    if (operation->temporal.coverage.resource_id) {
        const XgSemanticResourceRef ref = operation->temporal.coverage;
        int retained = 0;
        for (uint32_t i = 0u; i < audit->consumed_temporal_coverages; ++i)
            retained |= !memcmp(&ref, &audit->temporal_coverages[i], sizeof(ref));
        if (!retained) { native_recipe_drop(target); return; }
        for (uint32_t i = 0u; i < recipe->coverage_count; ++i)
            if (!memcmp(&ref, &recipe->coverages[i].reference, sizeof(ref))) { temporal_index = i; break; }
        if (temporal_index == UINT32_MAX) {
            XgRenderTemporalCoverageView view;
            GlNativeRecipeCoverage *coverages = recipe->coverage_count == UINT32_MAX ? NULL :
                xg_render_array_reserve(recipe->coverages, sizeof(*coverages),
                    &recipe->coverage_capacity, recipe->coverage_count + 1u, UINT32_MAX);
            if (!coverages) { native_recipe_drop(target); return; }
            recipe->coverages = coverages;
            if (!xg_render_temporal_coverage_view(ref, &view) ||
                !native_coverage_acquire_ref(ref)) { native_recipe_drop(target); return; }
            temporal_index = recipe->coverage_count++;
            recipe->coverages[temporal_index] = (GlNativeRecipeCoverage){ref, view};
            /* The CPU may publish the next camera before this OT reaches the
             * GPU. Keep the adjacency receipt of the DRAW's source, not the
             * latest publication at recipe completion. */
            for (uint32_t i = 0u; views->publication && i < views->publication->count; ++i) {
                const GlNativeCoverageScope *scope = &views->publication->scopes[i];
                const GlNativeRecipeCoverage *source =
                    !memcmp(&ref, &scope->current.reference, sizeof(ref)) ? &scope->current :
                    !memcmp(&ref, &scope->previous.reference, sizeof(ref)) ? &scope->previous : NULL;
                if (source) {
                    recipe->coverages[temporal_index].predecessor = source->predecessor;
                    break;
                }
            }
        }
        const XgRenderTemporalComponent *component = native_coverage_component(
            &recipe->coverages[temporal_index].view, operation->temporal.component_id);
        if (!component || component->producer_id != draw.interpolation_identity.producer_id ||
            component->scene_id != draw.interpolation_identity.scene_id) { native_recipe_drop(target); return; }
    }
    if (mesh && !expanded) { native_recipe_drop(target); return; }
    GlNativeRecipeDraw record;
    _Static_assert(offsetof(GlNativeRecipeDraw,semantic)==0u,"recipe semantic prefix");
    /* Every semantic byte is copied below; only the smaller ownership tail
     * needs clearing, including phase-only flags and padding. */
    memset((uint8_t *)&record+sizeof(draw),0,sizeof(record)-sizeof(draw));
    record.semantic=draw;record.mesh=mesh?expanded:NULL;
    record.enhanced=enhanced!=0;
    record.textures=draw.material.textured?recipe->textures:NULL;
    record.motion=*binding;record.motion_index=motion_index;
    record.temporal_component=operation->temporal.component_id;record.temporal_index=temporal_index;
    record.projection_offset_x=(int32_t)dx;record.projection_offset_y=(int32_t)dy;
    record.view_origin_y=target->y;
    record.dither_x=target->x&3u;record.dither_y=target->y&3u;
    record.hd_texture=operation->hd_texture;
    /* Never fold a later draw into one the wave already displaced. */
    if (!mesh && !(recipe->wave && recipe->wave_at == recipe->count) &&
        native_recipe_coalesce(recipe, &record)) return;
    if (mesh) expanded->references++;
    recipe->draws[recipe->count++] = record;
    if (!mesh && native_recipe_coalescible(&recipe->draws[recipe->count - 1u]))
        (void)native_recipe_meshify(&recipe->draws[recipe->count - 1u]);
    if (draw.material.textured) recipe->textures->references++;
}

static void native_recipe_begin(GlNativeViewState *views, GlNativeViewTarget *target, uint32_t color) {
    s_native_recipe_texture_read = (GlNativeRecipeDropEvent){.texture_reason = "clear_begin"};
    /* A target's frames are alike in size: start at its last recipe's draw
     * count instead of regrowing from 16 by doubling (each regrowth copies
     * every 1 KiB record so far). */
    native_recipe_drop(target);
    target->displayed = 0;
    if (!native_recipe_tracking(views)) return;
    const uint32_t expected = target->recipe_draws;
    GlNativeRecipe *recipe = malloc(sizeof(*recipe));
    if (!recipe) return;
    memset(recipe, 0, sizeof(*recipe));
    if (expected)
        recipe->draws = xg_render_array_reserve_uninitialized(NULL, sizeof(*recipe->draws),
            &recipe->draw_capacity, expected + expected / 4u, UINT32_MAX);
    recipe->references = 1u; recipe->clear_color = color;
    recipe->publication = views->publication;
    if (recipe->publication) recipe->publication->references++;
    recipe->width = target->width; recipe->height = target->height;
    recipe->origin_x = target->x; recipe->origin_y = target->y;
    recipe->view_width = native_view_eligible(views, target) ? views->width : 0u;
    recipe->view_height = views->reference_height; recipe->offset = views->offset;
    gpu_vram_region_mark_rect(&recipe->feedback, target->x, target->y,
        target->x + target->width - 1, target->y + target->height - 1);
    target->recipe = recipe;
    target->displayed = 0; /* A new frame; scanout marks it once shown. */
}

/* Frames that never clear (battle: full 3D overdraw, no FILL) still replay
 * exactly when every phase starts from what the endpoint itself drew over:
 * the VIEW plane at this declaration, captured in FIFO order on the GPU. */
static uint64_t s_native_recipe_base_serial;
static void native_recipe_begin_snapshot(GlNativeViewState *views, uint32_t index) {
    GlNativeViewTarget *target = &views->targets[index];
    const uint32_t domain = native_view_domain(views, target);
    if (!views->gpu || index >= GL_NATIVE_VIEW_TARGET_CAPACITY || domain == UINT32_MAX ||
        !native_view_eligible(views, target)) return;
    s_native_recipe_texture_read = (GlNativeRecipeDropEvent){.texture_reason = "snapshot_begin"};
    native_recipe_begin(views, target, 0u);
    GlNativeRecipe *recipe = target->recipe;
    if (!recipe) return;
    GlNativeGpuCommand *capture = native_gpu_command(views->gpu, NATIVE_GPU_BASE);
    if (!capture) { native_recipe_drop(target); return; }
    capture->plane = domain + 1u;
    capture->source = index;
    if (++s_native_recipe_base_serial == 0u) ++s_native_recipe_base_serial;
    recipe->base_serial = s_native_recipe_base_serial;
    recipe->base_slot = index;
    target->displayed = 0;
    /* COPY aliases of the previous capture in this slot lost their base. */
    for (uint32_t i = 0u; i < views->count; ++i) {
        GlNativeViewTarget *alias = &views->targets[i];
        if (alias->recipe && alias->recipe != recipe && alias->recipe->base_serial &&
            alias->recipe->base_slot == index) native_recipe_drop(alias);
    }
}

/* A game that neither clears nor declares its target (Battling redraws over
 * the buffer it showed two frames earlier) starts a new frame with its first
 * draw over a shown target, with no recipe or a snapshot one: the same
 * snapshot a declaration would start, captured before this draw. Recipes
 * dropped mid-frame are not shown and stay dropped. */
static void native_recipe_frame_draw(GlNativeViewState *views, const XgSemanticDrawRecord *draw) {
    if (!native_recipe_tracking(views)) return;
    const XgRenderIrMaterialState *dm = &draw->primitive.material;
    const int frame = native_view_cover(views, views->count, dm->draw_area_left,
        dm->draw_area_top, dm->draw_area_right - dm->draw_area_left + 1,
        dm->draw_area_bottom - dm->draw_area_top + 1);
    const GlNativeRecipe *shown = frame >= 0 ? views->targets[frame].recipe : NULL;
    if (frame >= 0 && views->targets[frame].displayed &&
        (!shown || shown->base_serial) &&
        dm->draw_area_left == views->targets[frame].x &&
        dm->draw_area_right - dm->draw_area_left + 1 == views->targets[frame].width) {
        native_recipe_begin_snapshot(views, (uint32_t)frame);
        if (views->targets[frame].recipe) views->active = frame;
    }
}

/* Canonical words a transfer leaves rasterized (copied from rasterized words)
 * or replaced by transferred data. */
static void native_recipe_raster_transfer(GlNativeViewState *views, const XgRenderNativeOperation *op) {
    if (!native_recipe_tracking(views)) return;
    const int raster = op->kind == XG_RENDER_NATIVE_OPERATION_COPY &&
        native_recipe_region(&views->raster_words, op->src_x, op->src_y, op->width, op->height);
    if (raster) gpu_vram_region_mark_transfer(&views->raster_words, op->dst_x, op->dst_y, op->width, op->height);
    else if (!op->mask_check)
        gpu_vram_region_clear_transfer(&views->raster_words, op->dst_x, op->dst_y, op->width, op->height);
}

static void native_recipe_transfer(GlNativeViewState *views, const XgRenderNativeOperation *op,
                                GlNativeRecipe *const *before, uint32_t before_count) {
    if (!native_recipe_tracking(views)) return;
    const int copy = op->kind == XG_RENDER_NATIVE_OPERATION_COPY;
    for (uint32_t i = 0u; i < views->count; ++i) {
        GlNativeViewTarget *t = &views->targets[i];
        const uint32_t dx = (t->x - op->dst_x) & (VRAM_W - 1);
        const uint32_t dy = (t->y - op->dst_y) & (VRAM_H - 1);
        const int full = (op->width == VRAM_W || dx + t->width <= op->width) &&
            (op->height == VRAM_H || dy + t->height <= op->height);
        GpuVramRect rectangles[4];
        const int count = gpu_vram_split_transfer(op->dst_x, op->dst_y, op->width, op->height, rectangles);
        int touches = 0;
        for (int r = 0; r < count; ++r)
            touches |= rectangles[r].x < t->x + t->width && t->x < rectangles[r].x + rectangles[r].w &&
                rectangles[r].y < t->y + t->height && t->y < rectangles[r].y + rectangles[r].h;
        /* Texture writes cannot retroactively change already rasterized draws.
         * A later draw captures a new immutable snapshot on its first changed read. */
        if (!touches) continue;
        s_native_recipe_texture_read = (GlNativeRecipeDropEvent){.texture_reason = copy ? "target_copy" : "target_write"};
        native_recipe_drop(t);
        if (full && op->kind == XG_RENDER_NATIVE_OPERATION_FILL && !op->mask_check) {
            native_recipe_begin(views, t, conv_1555_to_rgba8(op->fill_color) |
                (op->mask_set ? UINT32_C(0xff000000) : 0u));
        } else if (op->kind == XG_RENDER_NATIVE_OPERATION_FILL && !op->mask_check) {
            /* A clear that leaves part of the target (Battling clears
             * 320x224 at y 240 while the target is 218 lines from y 256)
             * starts a snapshot frame: the base, captured in FIFO order right
             * after this FILL, holds the clear and whatever it left, and the
             * later draws replay over it. */
            native_recipe_begin_snapshot(views, i);
        } else if (full && copy && !op->mask_set && !op->mask_check) {
            const int sx = (op->src_x + dx) & (VRAM_W - 1);
            const int sy = (op->src_y + dy) & (VRAM_H - 1);
            for (uint32_t j = 0u; j < before_count; ++j) {
                const GlNativeViewTarget *s = &views->targets[j];
                if (s->x == sx && s->y == sy && s->width == t->width && s->height == t->height &&
                    before[j]) {
                    t->recipe = before[j]; t->recipe->references++; break;
                }
            }
        }
    }
    for (uint32_t i = 0u; i < views->count; ++i) {
        GlNativeViewTarget *target = &views->targets[i];
        GlNativeRecipe *recipe = target->recipe;
        if (!recipe) continue;
        const int dependent = copy && native_recipe_region(&recipe->feedback,
            op->src_x, op->src_y, op->width, op->height);
        if (!dependent && (op->mask_check || !native_recipe_region(&recipe->feedback,
            op->dst_x, op->dst_y, op->width, op->height))) continue;
        if (!native_recipe_private(target)) continue;
        recipe = target->recipe;
        /* Snapshot before mapping overlapping/wrapped copies. Independent
         * writes break aliases; mask-checked writes cannot prove an overwrite. */
        const GpuVramRegionSet before_feedback = recipe->feedback;
        if (!op->mask_check)
            gpu_vram_region_clear_transfer(&recipe->feedback, op->dst_x, op->dst_y, op->width, op->height);
        if (dependent)
            for (uint32_t y = 0u; y < op->height; ++y)
                for (uint32_t x = 0u; x < op->width; ++x) {
                    const int sx = (op->src_x + x) & (VRAM_W - 1), sy = (op->src_y + y) & (VRAM_H - 1);
                    if (gpu_vram_region_intersects(&before_feedback, sx, sy, sx, sy)) {
                        const int dx = (op->dst_x + x) & (VRAM_W - 1), dy = (op->dst_y + y) & (VRAM_H - 1);
                        gpu_vram_region_mark_rect(&recipe->feedback, dx, dy, dx, dy);
                    }
                }
        gpu_vram_region_mark_rect(&recipe->feedback, target->x, target->y,
            target->x + target->width - 1, target->y + target->height - 1);
        recipe->feedback_cache_count = 0u;
    }
    native_recipe_raster_transfer(views, op);
}

static void native_recipe_draw_written(GlNativeCpuCompiler *compiler, const XgRenderNativeOperation *operation,
                                    const GlNativeCompileAudit *audit, const uint16_t *words) {
    GlNativeViewState *views = compiler->native_views;
    if (!native_recipe_tracking(views) && !(compiler->gpu && compiler->gpu->canonical_gpu)) return;
    const GpuRenderSemantic *semantic = &operation->semantic;
    GpuRenderSemantic draw = *semantic;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (uint32_t i = 0u; i < native_recipe_vertex_count(&draw); ++i) {
        const GpuRenderSemanticVertex *v = native_recipe_vertex(&draw, i);
        const int x = native_fixed_floor(v->x) + draw.material.draw_offset_x;
        const int y = native_fixed_floor(v->y) + draw.material.draw_offset_y;
        if (x < left) left = x; if (x > right) right = x;
        if (y < top) top = y; if (y > bottom) bottom = y;
    }
    if (left < draw.material.draw_area_left) left = draw.material.draw_area_left;
    if (top < draw.material.draw_area_top) top = draw.material.draw_area_top;
    if (right > draw.material.draw_area_right) right = draw.material.draw_area_right;
    if (bottom > draw.material.draw_area_bottom) bottom = draw.material.draw_area_bottom;
    if (left > right || top > bottom) return;
    if (draw.material.textured) {
        static GlNativeRecipeReads reads;
        int reads_ready = 0;
        for (uint32_t i = 0u; i < views->count; ++i) {
            GlNativeViewTarget *target = &views->targets[i];
            if (!target->recipe || (left >= target->x && top >= target->y &&
                right < target->x + target->width && bottom < target->y + target->height)) continue;
            /* The exact texel footprint lies inside the texture page and its
             * CLUT row: when neither meets this recipe's feedback region,
             * native_recipe_feedback_reads finds nothing. Skip computing it. */
            const GpuRenderMaterial *tm = &draw.material;
            if (!native_recipe_feedback_region(target->recipe, tm->texture_page_x * 64, tm->texture_page_y * 256,
                    64 << tm->texture_depth, 256) &&
                (tm->texture_depth == GPU_RENDER_TEXTURE_15_BIT ||
                 !native_recipe_feedback_region(target->recipe, tm->clut_x, tm->clut_y,
                    tm->texture_depth == GPU_RENDER_TEXTURE_4_BIT ? 16 : 256, 1))) continue;
            if (!reads_ready) {
                native_recipe_texture_reads(&draw, &reads, audit->header.display.render_scale, 0, NULL);
                reads_ready = 1;
            }
            if (!native_recipe_feedback_reads(target->recipe, &reads)) continue;
            if (native_recipe_private(target)) {
                gpu_vram_region_mark_rect(&target->recipe->feedback, left, top, right, bottom);
                target->recipe->feedback_cache_count = 0u;
            }
        }
    }
    /* Actual fragment writes already recorded provenance. An AABB would also
     * mark excluded right/bottom edges and transparent or masked-out pixels. */
    /* SDK clear tiles may arrive as an opaque quad rather than GP0(FILL).
     * Prove coverage from its two rectangle triangles, never from image color. */
    const GpuRenderMaterial *m = &draw.material;
    if (draw.topology != GPU_RENDER_SEMANTIC_TRIANGLES || draw.triangle_count != 2u ||
        m->textured || m->semi_transparent || m->mask_check || draw.native_view_effect ||
        m->shading != GPU_RENDER_SHADING_FLAT || operation->motion.motion.handle.resource_id) return;
    const GpuRenderSemanticVertex *first = &draw.triangles[0].vertices[0];
    for (uint32_t i = 0u; i < 6u; ++i) {
        const GpuRenderSemanticVertex *v = native_recipe_vertex(&draw, i);
        if (v->r != first->r || v->g != first->g || v->b != first->b) return;
    }
    int32_t min_x = INT32_MAX, min_y = INT32_MAX, max_x = INT32_MIN, max_y = INT32_MIN;
    for (uint32_t i = 0u; i < 6u; ++i) {
        const GpuRenderSemanticVertex *v = native_recipe_vertex(&draw, i);
        if (v->x < min_x) min_x = v->x; if (v->x > max_x) max_x = v->x;
        if (v->y < min_y) min_y = v->y; if (v->y > max_y) max_y = v->y;
    }
    if (min_x == max_x || min_y == max_y) return;
    uint32_t triangles[2] = {0u, 0u};
    for (uint32_t i = 0u; i < 6u; ++i) {
        const GpuRenderSemanticVertex *v = native_recipe_vertex(&draw, i);
        if ((v->x != min_x && v->x != max_x) || (v->y != min_y && v->y != max_y)) return;
        const uint32_t corner = (v->x == max_x ? 1u : 0u) | (v->y == max_y ? 2u : 0u);
        if (triangles[i / 3u] & (1u << corner)) return;
        triangles[i / 3u] |= 1u << corner;
    }
    const uint32_t diagonal = triangles[0] & triangles[1];
    if ((triangles[0] | triangles[1]) != 15u || (diagonal != 9u && diagonal != 6u)) return;
    /* TILE is an independent write just like FILL, including small atlas clears
     * outside display targets. Do not label its deterministic output as actor
     * framebuffer feedback. Restrict this proof to complete integer pixels. */
    if (min_x % 65536 == 0 && min_y % 65536 == 0 && max_x % 65536 == 0 && max_y % 65536 == 0) {
        int clear_right = max_x / 65536 + m->draw_offset_x - 1;
        int clear_bottom = max_y / 65536 + m->draw_offset_y - 1;
        if (clear_right > right) clear_right = right;
        if (clear_bottom > bottom) clear_bottom = bottom;
        if (compiler->gpu && compiler->gpu->canonical_gpu && left <= clear_right && top <= clear_bottom) {
            GlNativeGpuCommand *clear = native_gpu_command(compiler->gpu, NATIVE_GPU_PROVENANCE_CLEAR);
            if (!clear) return;
            clear->x = left; clear->y = top;
            clear->w = clear_right - left + 1; clear->h = clear_bottom - top + 1;
            /* Before starting a new recipe, its inherited feedback map must
             * include every earlier fragment and exclude this proven clear. */
            if (!native_gpu_canonical_sync(compiler)) {
                native_gpu_fail_note(compiler->gpu, GL_RENDERER_NATIVE_GPU_FAILURE_CANONICAL_SYNC, compiler->gpu->count, 0u, 0u);
                compiler->gpu->failed = 1; return;
            }
        }
        gpu_vram_region_clear_rect(&views->raster_words, left, top, clear_right, clear_bottom);
        for (uint32_t i = 0u; i < views->count; ++i) {
            GlNativeViewTarget *target = &views->targets[i];
            if (!target->recipe || (left >= target->x && top >= target->y &&
                clear_right < target->x + target->width && clear_bottom < target->y + target->height) ||
                !gpu_vram_region_intersects(&target->recipe->feedback, left, top, clear_right, clear_bottom)) continue;
            if (!native_recipe_private(target)) continue;
            gpu_vram_region_clear_rect(&target->recipe->feedback, left, top, clear_right, clear_bottom);
            gpu_vram_region_mark_rect(&target->recipe->feedback, target->x, target->y,
                target->x + target->width - 1, target->y + target->height - 1);
            target->recipe->feedback_cache_count = 0u;
        }
    }
    for (uint32_t i = 0u; i < views->count; ++i) {
        GlNativeViewTarget *t = &views->targets[i];
        if (m->draw_area_left > t->x || m->draw_area_top > t->y ||
            m->draw_area_right < t->x + t->width - 1 || m->draw_area_bottom < t->y + t->height - 1 ||
            (int64_t)min_x + (int64_t)m->draw_offset_x * 65536 > (int64_t)t->x * 65536 ||
            (int64_t)min_y + (int64_t)m->draw_offset_y * 65536 > (int64_t)t->y * 65536 ||
            (int64_t)max_x + (int64_t)m->draw_offset_x * 65536 < (int64_t)(t->x + t->width) * 65536 ||
            (int64_t)max_y + (int64_t)m->draw_offset_y * 65536 < (int64_t)(t->y + t->height) * 65536) continue;
        if (native_view_eligible(views, t)) {
            if (draw.screen_space_2d != GPU_RENDER_SCREEN_SPACE_2D_STRETCH || views->active < 0 ||
                views->targets[views->active].x != t->x || views->targets[views->active].width != t->width) continue;
            int has_native = 0;
            for (uint32_t v = 0u; v < 6u; ++v) has_native |= native_recipe_vertex(&draw, v)->native_view_position;
            if (has_native) continue;
        }
        /* This first, non-motion draw overwrites every target pixel. Keep the
         * draw itself so dither/mask semantics are reproduced, not guessed from
         * its final color. The initial zero never survives the proven overwrite. */
        native_recipe_begin(views, t, 0u);
        native_recipe_append(views, i, operation, audit, words, native_view_eligible(views, t), NULL);
        gpu_vram_region_clear_rect(&views->raster_words, t->x, t->y, t->x + t->width - 1, t->y + t->height - 1);
    }
}

static int native_view_raster(GlNativeCpuCompiler *compiler, uint16_t view_width, uint16_t view_offset,
                          const GlNativeViewRaster *target, const XgSemanticDrawRecord *source,
                          uint32_t operation_index, int enhanced, uint16_t raster_origin_y) {
    XgSemanticDrawRecord draw = *source;
    XgRenderIrMaterialState *material = &draw.primitive.material;
    const XgRenderIrMaterialState *original = &source->primitive.material;
    GlNativeResourceRecord resource = {0};
    GlNativeCpuSurface surface = {0};
    int left = original->draw_area_left > target->x ? original->draw_area_left : target->x;
    int right = original->draw_area_right < target->x + target->width - 1
        ? original->draw_area_right : target->x + target->width - 1;
    int top = original->draw_area_top > target->y ? original->draw_area_top : target->y;
    int bottom = original->draw_area_bottom < target->y + target->height - 1
        ? original->draw_area_bottom : target->y + target->height - 1;
    const int mode = enhanced ? source->screen_space_2d : GPU_RENDER_SCREEN_SPACE_2D_NONE;
    int64_t translation = (int64_t)view_offset * 65536;
    if (!view_width || target->width != view_width - 2u * view_offset ||
        left > right || top > bottom) return 1;
    if (mode == GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE) {
        int64_t minimum = INT64_MAX, maximum = INT64_MIN;
        for (uint32_t ti = 0u; ti < source->primitive.triangle_count; ++ti)
            for (uint32_t vi = 0u; vi < 3u; ++vi) {
                const int64_t x = source->primitive.triangles[ti].vertices[vi].x +
                    (int64_t)(original->draw_offset_x - target->x) * 65536;
                if (x < minimum) minimum = x;
                if (x > maximum) maximum = x;
            }
        if (minimum <= maximum) {
            const int64_t center = minimum + (maximum - minimum) / 2;
            const int64_t midpoint = (int64_t)target->width * 32768;
            const int64_t anchor = center < midpoint ? minimum : center > midpoint ? maximum : center;
            translation = native_div_floor(anchor * view_width, target->width) - anchor;
        }
    }
    /* Clip remains separate from the declared target. Full-target edges extend
     * to the margins; partial scissors remain translated into the center. */
    int clip_left = enhanced && left == target->x
        ? 0 : left - target->x + view_offset;
    int clip_right = enhanced && right == target->x + target->width - 1
        ? view_width - 1 : right - target->x + view_offset;
    if (mode == GPU_RENDER_SCREEN_SPACE_2D_STRETCH) {
        clip_left = (int)native_div_floor(
            (int64_t)(left - target->x) * view_width, target->width);
        clip_right = (int)(native_div_floor(
            (int64_t)(right - target->x + 1) * view_width, target->width) - 1);
    } else if (mode == GPU_RENDER_SCREEN_SPACE_2D_PRESERVE_SIZE) {
        if (left != target->x) clip_left = (int)native_div_floor(
            (int64_t)(left - target->x) * 65536 + translation, 65536);
        if (right != target->x + target->width - 1) clip_right = (int)native_div_floor(
            (int64_t)(right - target->x + 1) * 65536 + translation, 65536) - 1;
    }
    if (clip_left < 0) clip_left = 0;
    if (clip_right >= view_width) clip_right = view_width - 1;
    if (clip_left > clip_right) return 1;
    material->draw_area_left = (uint16_t)clip_left;
    material->draw_area_right = (uint16_t)clip_right;
    material->draw_area_top = (uint16_t)(top - target->y + raster_origin_y);
    material->draw_area_bottom = (uint16_t)(bottom - target->y + raster_origin_y);
    material->draw_offset_x = 0;
    material->draw_offset_y = 0;
    const uint32_t count = draw.topology == GPU_RENDER_SEMANTIC_LINES
        ? draw.line_count : draw.primitive.triangle_count;
    for (uint32_t pi = 0u; pi < count; ++pi) {
        const uint32_t vertices = draw.topology == GPU_RENDER_SEMANTIC_LINES ? 2u : 3u;
        for (uint32_t vi = 0u; vi < vertices; ++vi) {
            int32_t *px, *py;
            int32_t native_x, native_y;
            int native_position;
            if (draw.topology == GPU_RENDER_SEMANTIC_LINES) {
                GpuRenderSemanticVertex *v = &draw.lines[pi].vertices[vi];
                px = &v->x; py = &v->y; native_x = v->native_view_x; native_y = v->native_view_y;
                native_position = enhanced && v->native_view_position;
                v->native_view_position = 0;
            } else {
                XgRenderIrVertex *v = &draw.primitive.triangles[pi].vertices[vi];
                px = &v->x; py = &v->y; native_x = v->native_view_x; native_y = v->native_view_y;
                native_position = enhanced && v->native_view_position;
                v->native_view_position = false;
            }
            int64_t x = (int64_t)(native_position ? native_x : *px) +
                (int64_t)(original->draw_offset_x - target->x) * 65536;
            int64_t y = (int64_t)(native_position ? native_y : *py) +
                (int64_t)(original->draw_offset_y - target->y + raster_origin_y) * 65536;
            if (!native_position) x = mode == GPU_RENDER_SCREEN_SPACE_2D_STRETCH
                ? native_div_floor(x * view_width, target->width) : x + translation;
            if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return 0;
            *px = (int32_t)x; *py = (int32_t)y;
        }
    }
    if (compiler->mesh_count) {
        /* Meshes are host geometry on the GPU planes only, never a CPU
         * surface. The same per-vertex VIEW relocation as above. */
        if (!compiler->gpu_only) return 1;
        GlNativeGpuMeshTriangle *moved = compiler->mesh_writable
            ? compiler->mesh_writable : native_mesh_buffer(0u, compiler->mesh_count);
        if (!moved) return 0;
        const GlNativeTextureState template_texture = native_texture_state(&draw.primitive.material);
        uint32_t mesh_count = compiler->mesh_count;
        if (!compiler->mesh_writable) {
            uint32_t kept = 0u;
            for (uint32_t t = 0u; t < compiler->mesh_count; ++t) {
                uint32_t flags = 0u;
                if (compiler->mesh_flags)
                    memcpy(&flags, compiler->mesh_flags + (size_t)t * compiler->mesh_stride, sizeof(flags));
                if (flags & XG_RENDER_NATIVE_MESH_PHASE_ONLY) continue;
                memcpy(&moved[kept].triangle, compiler->mesh_base + (size_t)t * compiler->mesh_stride,
                    sizeof(moved[kept].triangle));
                if (compiler->mesh_textures)
                    memcpy(&moved[kept].texture, compiler->mesh_textures + (size_t)t * compiler->mesh_stride,
                        sizeof(moved[kept].texture));
                else moved[kept].texture = template_texture;
                ++kept;
            }
            mesh_count = kept;
            if (!mesh_count) return 1;
        }
        for (uint32_t t = 0u; t < mesh_count; ++t) {
            for (uint32_t vi = 0u; vi < 3u; ++vi) {
                XgRenderIrVertex *v = &moved[t].triangle.vertices[vi];
                const int native_position = enhanced && v->native_view_position;
                v->native_view_position = false;
                int64_t x = (int64_t)(native_position ? v->native_view_x : v->x) +
                    (int64_t)(original->draw_offset_x - target->x) * 65536;
                const int64_t y = (int64_t)(native_position ? v->native_view_y : v->y) +
                    (int64_t)(original->draw_offset_y - target->y + raster_origin_y) * 65536;
                if (!native_position) x = mode == GPU_RENDER_SCREEN_SPACE_2D_STRETCH
                    ? native_div_floor(x * view_width, target->width) : x + translation;
                if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return 0;
                v->x = (int32_t)x; v->y = (int32_t)y;
            }
        }
        return native_gpu_draw_mesh(compiler, &draw,
            compiler->gpu_plane >= GL_NATIVE_GPU_PHASE_BASE ? 0 : raster_origin_y,
            moved, mesh_count);
    }
    if (compiler->gpu_only) {
        /* Published recipes already passed source/material validation. The
         * phase projection and VIEW coordinate bounds were checked above;
         * no CPU surface or repeated generic raster validation is needed.
         * Still a VIEW pass: a transition capture samples its VIEW plane. */
        const int saved_view_pass = compiler->view_pass;
        compiler->view_pass = 1;
        const int drawn = native_gpu_draw(compiler, &draw,
            compiler->gpu_plane >= GL_NATIVE_GPU_PHASE_BASE ? 0 : raster_origin_y);
        compiler->view_pass = saved_view_pass;
        return drawn;
    }
    resource.view.descriptor.width = view_width;
    resource.view.descriptor.height = target->height;
    resource.view.descriptor.flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION;
    resource.view.descriptor.vram_y = raster_origin_y;
    resource.view.descriptor.vram_width = view_width;
    resource.view.descriptor.vram_height = target->height;
    surface = (GlNativeCpuSurface){.resource = &resource, .pixels = target->pixels,
        .depth = target->depth, .width = view_width, .height = target->height, .valid = 1};
    const uint32_t saved_draws = compiler->audit->rendered_draws;
    const uint64_t saved_pixels = compiler->audit->rendered_draw_pixels;
    const uint32_t saved_width = compiler->viewport_width, saved_height = compiler->viewport_height;
    const uint8_t saved_dither_y = compiler->dither_y;
    const int saved_view_pass = compiler->view_pass;
    compiler->viewport_width = view_width;
    compiler->viewport_height = target->height;
    compiler->dither_y = 0u; /* Raster Y is the original device row, not alias-local. */
    compiler->view_pass = 1;
    compiler->audit->view_raster_passes++;
    const int result = compiler->gpu && !compiler->gpu_only
        ? native_view_worker_draw(compiler, &surface, &draw, operation_index)
        : native_render_draw(compiler, &surface, &draw, operation_index);
    compiler->view_pass = saved_view_pass;
    compiler->viewport_width = saved_width;
    compiler->viewport_height = saved_height;
    compiler->dither_y = saved_dither_y;
    compiler->audit->rendered_draws = saved_draws;
    compiler->audit->rendered_draw_pixels = saved_pixels;
    return result;
}

static int native_view_draw(GlNativeCpuCompiler *compiler, GlNativeViewState *views,
                         const XgSemanticDrawRecord *draw, uint32_t operation_index, int marked) {
    const XgRenderIrMaterialState *material = &draw->primitive.material;
    uint64_t visited = 0u;
    if (material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom) return 1;
    if (views->depth_test)
        compiler->audit->depth_draws[draw->primitive.depth_policy <= GPU_RENDER_DEPTH_TEST_WRITE
            ? draw->primitive.depth_policy : GPU_RENDER_DEPTH_NONE]++;
    for (uint32_t i = 0u; i < views->count; ++i) {
        const GlNativeViewTarget *first = &views->targets[i];
        if ((visited & (UINT64_C(1) << i)) || !native_view_eligible(views, first)) continue;
        uint64_t rows[VRAM_H / 64u] = {0};
        uint32_t logical = 0u, domain_index = UINT32_MAX;
        for (uint32_t j = i; j < views->count; ++j) {
            const GlNativeViewTarget *alias = &views->targets[j];
            if (alias->x != first->x || alias->width != first->width) continue;
            visited |= UINT64_C(1) << j;
            if (material->draw_area_left >= alias->x + alias->width || material->draw_area_right < alias->x) continue;
            const uint32_t top = material->draw_area_top > alias->y ? material->draw_area_top : alias->y;
            const uint32_t end = (uint32_t)material->draw_area_bottom + 1u < alias->y + alias->height
                ? (uint32_t)material->draw_area_bottom + 1u : alias->y + alias->height;
            if (top >= end) continue;
            if (native_transition_overwritten(alias, draw)) {
                views->targets[j].transition_snapshot = 0;
            }
            /* GPU-only draws do not write the CPU domain. Retain its immutable
             * allocation until a transfer/CPU raster actually needs a private
             * copy; newly declared rows still go through normal seeding. */
            if ((!compiler->gpu_only || !alias->pixels) &&
                !native_view_private(views, j, compiler->native_vram->pixels)) return 0;
            domain_index = native_view_domain(views, alias);
            for (uint32_t word = top / 64u; word <= (end - 1u) / 64u; ++word) {
                const unsigned first_bit = word == top / 64u ? top & 63u : 0u;
                const unsigned last_bit = word == (end - 1u) / 64u ? (end - 1u) & 63u : 63u;
                rows[word] |= gpu_vram_region_word_mask((int)first_bit, (int)last_bit);
            }
            logical++;
        }
        if (!logical) continue;
        const GlNativeViewDomain *domain = &views->domains[domain_index];
        const GlNativeViewRaster target = {.x = domain->x, .width = domain->width,
            .height = VRAM_H, .pixels = domain->pixels,
            .depth = native_view_domain_depth(views, domain)};
        /* The declared viewport, not the availability of predivide metadata,
         * determines its full scissor. Unmarked billboards keep their size and
         * canonical position plus the same center offset as the other draws. */
        const int enhanced = views->active >= 0 &&
            views->targets[views->active].x == domain->x && views->targets[views->active].width == domain->width;
        const uint32_t before = compiler->audit->view_raster_passes;
        const uint32_t saved_gpu_plane = compiler->gpu_plane;
        compiler->gpu_plane = domain_index + 1u;
        /* Disjoint runs of the union scissor: each physical row is rasterized
         * once, even when several declared rectangles name that row. */
        for (uint32_t word = 0u; word < VRAM_H / 64u;) {
            if (!rows[word]) { ++word; continue; }
            const unsigned first_bit = native_first_bit(rows[word]);
            const uint32_t top = word * 64u + first_bit;
            uint32_t end = word * 64u;
            uint64_t remaining = ~rows[word] & (UINT64_MAX << first_bit);
            if (remaining) {
                const unsigned last_bit = native_first_bit(remaining);
                end += last_bit;
                rows[word] &= UINT64_MAX << last_bit;
            } else {
                rows[word++] = 0u;
                while (word < VRAM_H / 64u && rows[word] == UINT64_MAX)
                    rows[word++] = 0u;
                end = word * 64u;
                if (word < VRAM_H / 64u && rows[word] & 1u) {
                    const unsigned last_bit = native_first_bit(~rows[word]);
                    end += last_bit;
                    rows[word] &= UINT64_MAX << last_bit;
                }
            }
            XgSemanticDrawRecord clipped = *draw;
            clipped.primitive.material.draw_area_top = (uint16_t)top;
            clipped.primitive.material.draw_area_bottom = (uint16_t)(end - 1u);
            if (!native_view_raster(compiler, views->width, views->offset, &target, &clipped, operation_index, enhanced, 0u)) return 0;
        }
        compiler->gpu_plane = saved_gpu_plane;
        if (compiler->audit->view_raster_passes != before) {
            compiler->audit->rendered_view_draws += logical;
            if (enhanced && marked) compiler->audit->marked_view_draws += logical;
        }
    }
    return 1;
}

static void native_wave_capture_reset(GlNativeWaveCapture *wave) {
    memset(wave->seen, 0, sizeof(wave->seen));
    wave->count = wave->transfer_count = 0u;
    wave->lifted = wave->invalid = 0;
}

/* One 16x16 tile of the active target's mesh: an axis-aligned FT4 that
 * stretches its texels over the displaced cell. */
static int native_wave_capture_tile(GlNativeViewTarget *target, const XgSemanticDrawRecord *draw) {
    const XgRenderIrNativePrimitive *p = &draw->primitive;
    const XgRenderIrMaterialState *m = &p->material;
    const uint32_t index = draw->native_view_effect_index;
    GlNativeWaveCapture *wave = &target->wave;
    int64_t u0 = INT64_MAX, u1 = INT64_MIN, v0 = INT64_MAX, v1 = INT64_MIN;
    if (index >= NATIVE_WAVE_TILES) return 0;
    if (wave->seen[index]) {
        /* The next frame's mesh without a new declaration. */
        memset(wave->seen, 0, sizeof(wave->seen));
        wave->count = 0u;
        wave->invalid = 0;
        wave->lifted = 0;
    }
    wave->seen[index] = 1u;
    wave->count++;
    if (p->triangle_count != 2u || !m->textured || m->raw_texture || m->semi_transparent ||
        m->texture_depth != XG_RENDER_IR_TEXTURE_15_BIT ||
        m->texture_window_mask_x || m->texture_window_mask_y) return 0;
    for (uint32_t t = 0u; t < 2u; ++t)
        for (uint32_t i = 0u; i < 3u; ++i) {
            const XgRenderIrVertex *v = &p->triangles[t].vertices[i];
            if (v->native_view_position || v->projective_position ||
                v->x % 65536 || v->y % 65536 || v->u % 65536 || v->v % 65536) return 0;
            if (v->u < u0) u0 = v->u;
            if (v->u > u1) u1 = v->u;
            if (v->v < v0) v0 = v->v;
            if (v->v > v1) v1 = v->v;
        }
    if (u1 - u0 != 16 * 65536 || v1 - v0 != 16 * 65536) return 0;
    /* Edges follow the texel corners they carry. The guest flips its unused
     * fourteenth row upside down; rows are checked again when built. */
    int64_t left = INT64_MIN, right = INT64_MIN, top = INT64_MIN, bottom = INT64_MIN;
    for (uint32_t t = 0u; t < 2u; ++t)
        for (uint32_t i = 0u; i < 3u; ++i) {
            const XgRenderIrVertex *v = &p->triangles[t].vertices[i];
            int64_t *x = v->u == u0 ? &left : &right, *y = v->v == v0 ? &top : &bottom;
            if ((*x != INT64_MIN && *x != v->x) || (*y != INT64_MIN && *y != v->y)) return 0;
            *x = v->x; *y = v->y;
        }
    if (left == INT64_MIN || right == INT64_MIN || top == INT64_MIN || bottom == INT64_MIN) return 0;
    wave->tiles[index] = (GlNativeWaveTile){
        .left = (int16_t)(left / 65536 + m->draw_offset_x - target->x),
        .right = (int16_t)(right / 65536 + m->draw_offset_x - target->x),
        .top = (int16_t)(top / 65536 + m->draw_offset_y),
        .bottom = (int16_t)(bottom / 65536 + m->draw_offset_y),
        .texture_x = (int16_t)(m->texture_page_x * 64 + u0 / 65536),
        .texture_y = (int16_t)(m->texture_page_y * 256 + v0 / 65536),
        .clip_top = (int16_t)m->draw_area_top, .clip_bottom = (int16_t)m->draw_area_bottom,
        .after_lift = (uint8_t)(wave->lifted != 0),
    };
    return 1;
}

/* The VIEW row a tile samples: the frame as drawn, before the guest's own
 * transfers moved it (bottom rows saved aside, the rest lifted 32 rows). A
 * tile drawn before the lift reads its own target directly. */
static int native_wave_source_row(const GlNativeWaveCapture *wave, const GlNativeViewTarget *target,
                                  int x, int y, int after_lift) {
    for (uint32_t i = 0u; i < wave->transfer_count; ++i) {
        const GlNativeWaveTransfer *t = &wave->transfers[i];
        if (x >= t->dst_x && x < t->dst_x + t->width && y >= t->dst_y && y < t->dst_y + t->height)
            return t->src_y + (y - t->dst_y);
    }
    if (x < target->x || x >= target->x + target->width ||
        y < target->y || y >= target->y + target->height) return INT_MIN;
    if (after_lift && y >= wave->lift.dst_y && y < wave->lift.dst_y + wave->lift.height)
        return y - wave->lift.dst_y + wave->lift.src_y;
    return y;
}

static GlNativeWaveWarp *native_wave_build(GlNativeViewState *views, const GlNativeViewTarget *target,
                                           uint64_t vblank) {
    const GlNativeWaveCapture *wave = &target->wave;
    GlNativeWaveGeometry geometry;
    int16_t observed[NATIVE_WAVE_COLUMNS + 1];
    int have_observed = 0;
    if (wave->invalid || !wave->count) return NULL;
    memset(&geometry, 0, sizeof(geometry));
    for (uint32_t r = 0u; r < NATIVE_WAVE_ROWS; ++r) {
        const GlNativeWaveTile *row = &wave->tiles[r * NATIVE_WAVE_COLUMNS];
        const uint8_t *seen = &wave->seen[r * NATIVE_WAVE_COLUMNS];
        int source_y = INT_MIN, present = 0;
        for (uint32_t c = 0u; c < NATIVE_WAVE_COLUMNS; ++c) present += seen[c] != 0u;
        /* A collapsed row can be culled before it reaches Native. */
        if (!present) continue;
        if (present != NATIVE_WAVE_COLUMNS) return NULL;
        /* The guest collapses its unused fourteenth row below the drawing
         * area (flat or upside down): it never draws anything. */
        const GlNativeWaveTile *middle = &row[NATIVE_WAVE_COLUMNS / 2u];
        if (middle->bottom <= middle->top || middle->top > middle->clip_bottom ||
            middle->bottom <= middle->clip_top) continue;
        /* Edges must agree where they can be seen: the guest leaves column 0
         * of row 12 one row off below the drawing area. */
        for (uint32_t c = 0u; c < NATIVE_WAVE_COLUMNS; ++c) {
            const int clip_top = row[c].clip_top, clip_bottom = row[c].clip_bottom + 1;
#define NATIVE_WAVE_CLIPPED(value) ((value) < clip_top ? clip_top : (value) > clip_bottom ? clip_bottom : (value))
            if (NATIVE_WAVE_CLIPPED(row[c].top) != NATIVE_WAVE_CLIPPED(middle->top) ||
                NATIVE_WAVE_CLIPPED(row[c].bottom) != NATIVE_WAVE_CLIPPED(middle->bottom) ||
                row[c].clip_top != middle->clip_top || row[c].clip_bottom != middle->clip_bottom ||
                row[c].after_lift != middle->after_lift ||
                (c && row[c].left != row[c - 1u].right)) return NULL;
#undef NATIVE_WAVE_CLIPPED
        }
        for (uint32_t c = 0u; c < NATIVE_WAVE_COLUMNS; ++c) {
            const int y = native_wave_source_row(wave, target, row[c].texture_x, row[c].texture_y,
                row[c].after_lift);
            if (y == INT_MIN) continue; /* the guest's unsaved 15th block */
            if (source_y != INT_MIN && y != source_y) return NULL;
            source_y = y;
        }
        if (source_y == INT_MIN) return NULL;
        if (!have_observed) {
            for (uint32_t c = 0u; c < NATIVE_WAVE_COLUMNS; ++c) observed[c] = row[c].left;
            observed[NATIVE_WAVE_COLUMNS] = row[NATIVE_WAVE_COLUMNS - 1u].right;
            have_observed = 1;
        }
        /* Target-local, so the other double buffer's mesh is comparable. */
        geometry.rows[r] = (GlNativeWaveRow){
            .top = (int16_t)(middle->top - target->y), .bottom = (int16_t)(middle->bottom - target->y),
            .source_y = (int16_t)(source_y - target->y),
            .clip_top = (int16_t)(middle->clip_top - target->y),
            .clip_bottom = (int16_t)(middle->clip_bottom - target->y),
            .after_lift = middle->after_lift, .present = 1u};
    }
    if (!have_observed) return NULL;
    geometry.row_count = NATIVE_WAVE_ROWS;
    /* Columns cover the margins plus one beyond, so each displaced edge has a
     * tile behind it. The game pins x(0)=0 and x(20)=320 only at its own
     * screen edges; past them, its formula continues. */
    const int margin = (views->offset + 15) / 16 + 1;
    const uint32_t count = NATIVE_WAVE_COLUMNS + 1u + 2u * (uint32_t)margin;
    if (count > NATIVE_WAVE_MAX_BOUNDARIES) return NULL;
    geometry.first_column = (int16_t)-margin;
    geometry.boundary_count = (uint16_t)count;
    if (!gpu_native_field_wave_columns(observed, -margin, count, geometry.x)) {
        /* No parameters reproduce this mesh: keep its columns exactly and
         * leave the margins undisplaced. */
        for (uint32_t i = 0u; i < count; ++i) {
            const int c = -margin + (int)i;
            geometry.x[i] = (int16_t)(c >= 0 && c <= NATIVE_WAVE_COLUMNS ? observed[c] : 16 * c);
        }
    }
    GlNativeWaveWarp *warp = calloc(1u, sizeof(*warp));
    if (!warp) return NULL;
    warp->references = 1u;
    warp->view_width = views->width;
    warp->offset = views->offset;
    warp->origin_y = target->y;
    warp->lift = wave->lift;
    warp->lifted = wave->lifted;
    warp->current = geometry;
    const GlNativeWaveGeometry *previous = &views->wave_previous;
    /* Rows pair by guest index; one entering or leaving the drawing area
     * keeps its own endpoint (native_wave_row_from). */
    const int compatible = previous->boundary_count == geometry.boundary_count &&
        previous->first_column == geometry.first_column &&
        previous->row_count == geometry.row_count &&
        vblank > views->wave_previous_vblank && vblank - views->wave_previous_vblank <= 8u;
    if (compatible) {
        warp->previous = *previous;
        warp->has_previous = 1;
    }
    views->wave_previous = geometry;
    views->wave_previous_vblank = vblank;
    return warp;
}

static const GlNativeWaveRow *native_wave_row_from(const GlNativeWaveGeometry *was,
                                                   const GlNativeWaveRow *row, uint32_t r) {
    const GlNativeWaveRow *from = &was->rows[r];
    return from->present && from->source_y == row->source_y && from->after_lift == row->after_lift &&
        from->clip_top == row->clip_top && from->clip_bottom == row->clip_bottom ? from : row;
}

static float native_wave_lerp(int from, int to, double t) {
    return (float)(from + (to - from) * t);
}

typedef struct GlNativeWaveBatch {
    GlNativeGpuWork *gpu;
    uint32_t plane, count;
    GlNativeGpuSpanRect rects[1024];
} GlNativeWaveBatch;

static int native_wave_rect(GlNativeWaveBatch *batch, float x, float y, float w, float h,
                            float sx, float sy, float sw, float sh) {
    if (batch->count == sizeof(batch->rects) / sizeof(batch->rects[0])) {
        if (!native_gpu_spans(batch->gpu, batch->plane, batch->plane, 4u, batch->rects, batch->count))
            return 0;
        batch->count = 0u;
    }
    batch->rects[batch->count++] = (GlNativeGpuSpanRect){x, y, w, h, sx, sy, sw, sh};
    return 1;
}

/* Target-local rows [y0, y1) of one mesh row, moved by shift, as stretched
 * spans. Interpolated edges stay fractional: a phase moves the wave by
 * sub-pixel amounts instead of stepping whole guest pixels. */
static int native_wave_emit_rows(GlNativeWaveBatch *batch, const GlNativeWaveWarp *warp,
                                 const GlNativeWaveGeometry *now, const GlNativeWaveGeometry *was,
                                 uint32_t r, double t, float top, float bottom, float y0, float y1,
                                 int shift) {
    const float width = warp->view_width;
    if (y1 <= y0) return 1;
    const float sy = warp->origin_y + now->rows[r].source_y + (y0 - top) * 16.f / (bottom - top);
    const float sh = (y1 - y0) * 16.f / (bottom - top);
    for (uint32_t i = 0u; i + 1u < now->boundary_count; ++i) {
        const float left = native_wave_lerp(was->x[i], now->x[i], t) + warp->offset;
        const float right = native_wave_lerp(was->x[i + 1u], now->x[i + 1u], t) + warp->offset;
        const float x0 = left > 0.f ? left : 0.f;
        const float x1 = right < width ? right : width;
        if (right <= left || x1 <= x0) continue;
        const float sx = (float)(16 * (now->first_column + (int)i) + warp->offset) +
            (x0 - left) * 16.f / (right - left);
        const float sw = (x1 - x0) * 16.f / (right - left);
        if (!native_wave_rect(batch, x0, warp->origin_y + y0 + shift, x1 - x0, y1 - y0,
                sx, sy, sw, sh)) return 0;
    }
    return 1;
}

/* Replays the effect over one VIEW or phase plane in FIFO order, all reading
 * the plane as it was before the effect: the lift as a whole-row copy, the
 * rows drawn before it already lifted, then the rows drawn after it. t places
 * the mesh in its interval. */
static int native_wave_emit_batch(GlNativeWaveBatch *batch, const GlNativeWaveWarp *warp, double t);
static int native_wave_emit(GlNativeGpuWork *gpu, uint32_t plane, const GlNativeWaveWarp *warp, double t) {
    if (!gpu) return 1;
    if (!(t > 0.0 && t < 1.0)) t = 1.0;
    GlNativeGpuCommand *snapshot = native_gpu_command(gpu, NATIVE_GPU_SNAPSHOT);
    if (!snapshot) return 0;
    snapshot->plane = plane;
    GlNativeWaveBatch *batch = malloc(sizeof(*batch));
    if (!batch) return 0;
    batch->gpu = gpu; batch->plane = plane; batch->count = 0u;
    const int ok = native_wave_emit_batch(batch, warp, t);
    free(batch);
    return ok;
}

static int native_wave_emit_batch(GlNativeWaveBatch *batch, const GlNativeWaveWarp *warp, double t) {
    const GlNativeWaveGeometry *now = &warp->current;
    const GlNativeWaveGeometry *was = warp->has_previous ? &warp->previous : now;
    const GlNativeWaveTransfer *lift = &warp->lift;
    /* The lift in target-local rows. */
    const int source0 = lift->src_y - warp->origin_y, source1 = source0 + lift->height;
    const int covered0 = lift->dst_y - warp->origin_y, covered1 = covered0 + lift->height;
    const int shift = warp->lifted ? covered0 - source0 : 0;
    if (warp->lifted && !native_wave_rect(batch, 0.f, lift->dst_y, warp->view_width, lift->height,
            0.f, lift->src_y, warp->view_width, lift->height)) return 0;
    for (int after = 0; after < 2; ++after)
        for (uint32_t r = 0u; r < now->row_count; ++r) {
            const GlNativeWaveRow *row = &now->rows[r];
            if (!row->present || row->after_lift != after) continue;
            const GlNativeWaveRow *from = native_wave_row_from(was, row, r);
            const float top = native_wave_lerp(from->top, row->top, t);
            const float bottom = native_wave_lerp(from->bottom, row->bottom, t);
            const float y0 = top > row->clip_top ? top : row->clip_top;
            const float y1 = bottom < row->clip_bottom + 1 ? bottom : row->clip_bottom + 1;
            if (bottom <= top || y1 <= y0) continue;
            if (after || !warp->lifted) {
                if (!native_wave_emit_rows(batch, warp, now, was, r, t, top, bottom, y0, y1, 0))
                    return 0;
                continue;
            }
            /* The lift moves the part inside its source rows. Of the rest,
             * what falls in its destination rows is overwritten by it. */
            if (!native_wave_emit_rows(batch, warp, now, was, r, t, top, bottom,
                    y0 > source0 ? y0 : source0, y1 < source1 ? y1 : source1, shift)) return 0;
            const float outside[2][2] = {{y0, y1 < source0 ? y1 : source0},
                                         {y0 > source1 ? y0 : source1, y1}};
            for (int part = 0; part < 2; ++part) {
                const float a = outside[part][0], b = outside[part][1];
                if (!native_wave_emit_rows(batch, warp, now, was, r, t, top, bottom,
                        a, b < covered0 ? b : covered0, 0) ||
                    !native_wave_emit_rows(batch, warp, now, was, r, t, top, bottom,
                        a > covered1 ? a : covered1, b, 0)) return 0;
            }
        }
    return native_gpu_spans(batch->gpu, batch->plane, batch->plane, 4u, batch->rects, batch->count);
}

/* The last tile arrived: replace the guest's redraw of this target with the
 * Native one, and let phases replay it at the same point of the frame. */
static int native_wave_finish(GlNativeViewState *views, uint32_t index, uint64_t vblank,
                              GlNativeCompileAudit *audit) {
    GlNativeViewTarget *target = &views->targets[index];
    GlNativeWaveWarp *warp = native_wave_build(views, target, vblank);
    const uint32_t domain = native_view_domain(views, target);
    native_wave_capture_reset(&target->wave);
    if (!warp || domain == UINT32_MAX) {
        native_wave_release(warp);
        audit->view_wave_incomplete++;
        return 1;
    }
    const int ok = native_wave_emit(views->gpu, domain + 1u, warp, 1.0);
    if (ok) {
        audit->view_wave_rows += warp->current.row_count;
        if (target->recipe && native_recipe_private(target)) {
            GlNativeRecipe *recipe = target->recipe;
            if (recipe->wave) {
                s_native_recipe_texture_read.texture_reason = "second_wave";
                native_recipe_drop(target);
            } else {
                recipe->wave = warp;
                warp->references++;
                recipe->wave_at = recipe->count;
            }
        }
    }
    native_wave_release(warp);
    return ok;
}

/* A transfer of the wave: the lift of a whole VIEW target (1) or a block
 * saved outside every target (2) stays out of VIEW; anything else is an
 * ordinary copy (0). */
static int native_wave_transfer(GlNativeViewState *views, const XgRenderNativeOperation *op) {
    const int destination = native_view_cover(views, views->count, op->dst_x, op->dst_y,
                                              op->width, op->height);
    if (destination >= 0) {
        GlNativeViewTarget *target = &views->targets[destination];
        if (!native_view_eligible(views, target) || op->width != target->width ||
            op->src_x != op->dst_x || op->dst_x != target->x ||
            native_view_cover(views, views->count, op->src_x, op->src_y,
                              op->width, op->height) != destination) return 0;
        target->wave.lift = (GlNativeWaveTransfer){op->src_x, op->src_y, op->dst_x, op->dst_y,
            op->width, op->height};
        target->wave.lifted = 1;
        return 1;
    }
    for (uint32_t i = 0u; i < views->count; ++i) {
        const GlNativeViewTarget *target = &views->targets[i];
        if (op->dst_x < target->x + target->width && target->x < op->dst_x + op->width &&
            op->dst_y < target->y + target->height && target->y < op->dst_y + op->height) return 0;
    }
    const int source = native_view_cover(views, views->count, op->src_x, op->src_y,
                                         op->width, op->height);
    if (source < 0) return 0;
    GlNativeWaveCapture *wave = &views->targets[source].wave;
    const GlNativeWaveTransfer transfer = {op->src_x, op->src_y, op->dst_x, op->dst_y,
        op->width, op->height};
    uint32_t slot = 0u;
    while (slot < wave->transfer_count && (wave->transfers[slot].dst_x != op->dst_x ||
           wave->transfers[slot].dst_y != op->dst_y)) ++slot;
    if (slot < NATIVE_WAVE_TRANSFERS) {
        wave->transfers[slot] = transfer;
        if (slot == wave->transfer_count) wave->transfer_count++;
    }
    return 2;
}

/* StoreImage/LoadImage round trips used by FieldSetVramStpBits and the Battle
 * ripple change STP only. Compare every RGB555 word before preserving VIEW;
 * an unrelated upload, even with the same dimensions, must replace it. */
static int native_transition_upload(GlNativeViewState *views,
                                    const XgRenderNativeOperation *operation,
                                    const GlNativeResourceRecord *upload,
                                    const uint16_t *words,
                                    XgRenderNativeOperation *view_operation) {
    if (operation->kind != XG_RENDER_NATIVE_OPERATION_UPLOAD ||
        operation->mask_set || operation->mask_check) return 0;
    /* READBACK records the words the guest actually received. They can differ
     * from Native's canonical raster at triangle edges; comparing only against
     * that raster incorrectly treats an STP-only round trip as a new texture. */
    if (views->readback_capture && views->readback_rgb_digest &&
        operation->width == views->readback_width && operation->height == views->readback_height &&
        ((operation->dst_x == views->readback_x && operation->dst_y == views->readback_y) ||
         (views->readback_width == 320 && operation->dst_x == 704 && operation->dst_y == 256))) {
        uint64_t digest = UINT64_C(1469598103934665603);
        for (uint32_t y = 0; y < operation->height; ++y)
            for (uint32_t x = 0; x < operation->width; ++x) {
                const uint16_t word = native_read_u16((const uint8_t *)upload->view.bytes +
                    (size_t)y * upload->view.descriptor.row_pitch + x * 2u) & UINT16_C(0x7fff);
                digest = (digest ^ (word & 255u)) * UINT64_C(1099511628211);
                digest = (digest ^ (word >> 8u)) * UINT64_C(1099511628211);
            }
        if (digest == views->readback_rgb_digest) {
            int retained = 0;
            for (uint32_t i = 0; i < views->count; ++i) {
                GlNativeViewTarget *target = &views->targets[i];
                if (target->transition_snapshot && target->x == 704 && target->y == 256) {
                    target->transition_snapshot = 1;
                    retained = 1;
                }
            }
            views->readback_rgb_digest = 0u;
            if (retained) return 1;
        }
    }
    for (uint32_t i = 0u; i < views->count; ++i) {
        const GlNativeViewTarget *target = &views->targets[i];
        if (!target->transition_snapshot || operation->dst_x < target->x ||
            operation->dst_y < target->y ||
            operation->dst_x + operation->width > target->x + target->width ||
            operation->dst_y + operation->height > target->y + target->height)
            continue;
        int same = 1;
        for (uint32_t y = 0u; y < operation->height && same; ++y)
            for (uint32_t x = 0u; x < operation->width; ++x)
                if (((native_read_u16((const uint8_t *)upload->view.bytes +
                        (size_t)y * upload->view.descriptor.row_pitch + x * 2u) ^
                      words[(size_t)(operation->dst_y + y) * VRAM_W + operation->dst_x + x]) &
                     UINT16_C(0x7fff)) != 0u) { same = 0; break; }
        if (same) return 1;
    }
    /* RunFramebufferRadialRippleDissolve captures (0,0,320,224), sets STP,
     * then uploads at (704,256). Prove the round trip from its actual words. */
    if (operation->dst_x != 704u || operation->dst_y != 256u ||
        operation->width != 320u ||
        !native_transition_height(operation->height) ||
        native_view_cover(views, views->count, 0, 0, 320,
            operation->height < 224u ? operation->height : 224u) < 0)
        return 0;
    for (uint32_t y = 0u; y < operation->height; ++y)
        for (uint32_t x = 0u; x < operation->width; ++x)
            if (((native_read_u16((const uint8_t *)upload->view.bytes +
                    (size_t)y * upload->view.descriptor.row_pitch + x * 2u) ^
                  words[(size_t)y * VRAM_W + x]) & UINT16_C(0x7fff)) != 0u)
                return 0;
    *view_operation = *operation;
    view_operation->kind = XG_RENDER_NATIVE_OPERATION_COPY;
    view_operation->src_x = view_operation->src_y = 0u;
    return 2;
}

static int native_view_transfer(GlNativeViewState *views,
                             const XgRenderNativeOperation *operation,
                             const GlNativeResourceRecord *upload,
                             const uint32_t *canonical,
                             const uint32_t *copy_pixels,
                             GlNativeCompileAudit *audit) {
    const uint32_t original_count = views->count;
    const uint32_t *snapshot[GL_NATIVE_VIEW_TARGET_CAPACITY] = {0};
    uint32_t *old_owned[GL_NATIVE_VIEW_TARGET_CAPACITY] = {0};
    uint64_t copied_domains = 0u;
    const int copy = operation->kind == XG_RENDER_NATIVE_OPERATION_COPY;
    const int transition_copy = copy && operation->dst_x == 704u &&
        operation->dst_y == 256u && operation->width == 320u &&
        native_transition_height(operation->height) &&
        !operation->mask_set && !operation->mask_check &&
        native_view_cover(views, original_count, operation->src_x,
            operation->src_y, operation->width,
            operation->height < 224u ? operation->height : 224u) >= 0;
    int result = 0;
    if (!copy && operation->width == VRAM_W && operation->height == VRAM_H &&
        !operation->mask_check) views->reset_motion_history = 1;
    if (copy) {
        /* Retail captures can crop 240 rows to 224, or include 16 padding
         * rows after a 224-row framebuffer. The per-row COPY below preserves
         * VIEW where declared and supplies canonical padding elsewhere. */
        if (transition_copy &&
            native_view_target(views, operation->dst_x, operation->dst_y,
                operation->width, operation->height) < 0)
            goto finished;
        /* Freeze all source slices before replacing any domain allocation. */
        for (uint32_t i = 0u; i < original_count; ++i)
            if (native_view_eligible(views, &views->targets[i]) && !views->targets[i].pixels &&
                !native_view_private(views, i, canonical)) goto finished;
        for (uint32_t i = 0u; i < original_count; ++i) {
            const GlNativeViewTarget *source = &views->targets[i];
            const uint32_t dx = (source->x - operation->src_x) & (VRAM_W - 1);
            const uint32_t dy = (source->y - operation->src_y) & (VRAM_H - 1);
            snapshot[i] = source->pixels;
            if ((operation->width == VRAM_W || dx + source->width <= operation->width) &&
                (operation->height == VRAM_H || dy + source->height <= operation->height)) {
                /* Translation of a completely copied declared rectangle is
                 * itself explicit evidence of the destination target. */
                const uint16_t x = (operation->dst_x + dx) & (VRAM_W - 1);
                const uint16_t y = (operation->dst_y + dy) & (VRAM_H - 1);
                if (x + source->width <= VRAM_W && y + source->height <= VRAM_H &&
                    native_view_target(views, x, y, source->width, source->height) < 0)
                    goto finished;
            }
        }
        if (views->gpu && !native_gpu_command(views->gpu, NATIVE_GPU_SNAPSHOT)) goto finished;
    }
    for (uint32_t i = 0u; i < views->count; ++i) {
        GlNativeViewTarget *target = &views->targets[i];
        const uint32_t target_dx = (target->x - operation->dst_x) & (VRAM_W - 1);
        const uint32_t target_dy = (target->y - operation->dst_y) & (VRAM_H - 1);
        const int full_x = operation->width == VRAM_W ||
            target_dx + target->width <= operation->width;
        const int full_y = operation->height == VRAM_H ||
            target_dy + target->height <= operation->height;
        int intersects = 0;
        if (!native_view_eligible(views, target)) continue;
        for (uint32_t y = 0u; y < target->height && !intersects; ++y)
            if (((target->y + y - operation->dst_y) & (VRAM_H - 1)) < operation->height)
                for (uint32_t x = 0u; x < target->width; ++x)
                    if (((target->x + x - operation->dst_x) & (VRAM_W - 1)) < operation->width) {
                        intersects = 1; break;
                    }
        if (!intersects) continue;
        target->transition_snapshot = transition_copy && full_x && full_y &&
            target->x == 704u && target->y == 256u && target->width == 320u &&
            native_transition_height(target->height) &&
            !operation->mask_set && !operation->mask_check;
        const uint32_t domain = native_view_domain(views, target);
        if (domain == UINT32_MAX) goto finished;
        if (copy && !(copied_domains & (UINT64_C(1) << domain))) {
            if (!native_view_domain_private(views, domain, &old_owned[domain])) goto finished;
            copied_domains |= UINT64_C(1) << domain;
        }
        if (!native_view_private(views, i, canonical)) goto finished;
        uint32_t *const target_depth = native_view_target_depth(views, target);
        for (uint32_t y = 0u; y < target->height; ++y) {
            const uint32_t dy = (target->y + y - operation->dst_y) & (VRAM_H - 1);
            if (dy >= operation->height) continue;
            const int source_y = (operation->src_y + dy) & (VRAM_H - 1);
            const int source_x = (operation->src_x + target_dx) & (VRAM_W - 1);
            const int source_index = copy && full_x
                ? native_view_cover(views, original_count, source_x, source_y, target->width, 1) : -1;
            const GlNativeViewTarget *source = source_index >= 0 ? &views->targets[source_index] : NULL;
            const int wide_copy = source && snapshot[source_index] &&
                source_x == source->x && source->width == target->width;
            if (!operation->mask_set && !operation->mask_check &&
                (wide_copy || (full_x && operation->kind == XG_RENDER_NATIVE_OPERATION_FILL))) {
                uint32_t *row = target->pixels + (size_t)y * views->width;
                const uint32_t color = wide_copy ? 0u : conv_1555_to_rgba8(operation->fill_color);
                if (wide_copy)
                    memcpy(row, snapshot[source_index] + (size_t)(source_y - source->y) * views->width,
                        (size_t)views->width * sizeof(*row));
                else
                    for (uint32_t x = 0u; x < views->width; ++x) row[x] = color;
                /* A full-row copy of a VIEW domain moves its surfaces, keys
                 * included (the snapshot allocation holds its plane after the
                 * pixels). Fills and partial copies leave no certified surface. */
                if (target_depth && wide_copy)
                    memcpy(target_depth + (size_t)y * views->width,
                        snapshot[source_index] + (size_t)views->width * VRAM_H +
                            (size_t)(source_y - source->y) * views->width,
                        (size_t)views->width * sizeof(*target_depth));
                else native_depth_clear_rows(target_depth, views->width, 0, (int)y, views->width, 1);
                if (!native_gpu_span(views->gpu, domain+1u, 0, target->y+y, views->width, 1,
                    wide_copy ? native_view_domain(views, source)+1u : UINT32_MAX,
                    0, source_y, views->width, 1, color, target_depth && wide_copy ? 4u : 0u)) goto finished;
                continue;
            }
            for (uint32_t x = 0u; x < views->width; ++x) {
                const int local = (int)x - views->offset;
                const int center = local >= 0 && local < target->width;
                uint32_t color;
                uint32_t *destination = &target->pixels[(size_t)y * views->width + x];
                /* A canonical row replacement supplies no virtual pixels.
                 * Retire its old margins, but not the declared projection. A
                 * full VRAM UPLOAD is still data, not a target-state reset. */
                if (!center && full_x && !wide_copy && !operation->mask_check &&
                    operation->kind != XG_RENDER_NATIVE_OPERATION_FILL) {
                    if (!native_gpu_span(views->gpu, domain+1u, x, target->y+y, 1, 1,
                        UINT32_MAX, 0, 0, 0, 0, 0, 0)) goto finished;
                    *destination = 0u;
                    if (target_depth) target_depth[(size_t)y * views->width + x] = 0u;
                    continue;
                }
                if (!center && !wide_copy &&
                    !(operation->kind == XG_RENDER_NATIVE_OPERATION_FILL && full_x))
                    continue;
                uint32_t gpu_source = UINT32_MAX;
                int gpu_x = 0;
                if (wide_copy) {
                    color = snapshot[source_index][(size_t)(source_y - source->y) * views->width + x];
                    gpu_source = native_view_domain(views, source) + 1u;
                    gpu_x = x;
                } else {
                    const uint32_t dx = (target->x + local - operation->dst_x) & (VRAM_W - 1);
                    if (center && dx >= operation->width) continue;
                    if (operation->kind == XG_RENDER_NATIVE_OPERATION_FILL)
                        color = conv_1555_to_rgba8(operation->fill_color);
                    else if (copy) {
                        const int sx = (operation->src_x + dx) & (VRAM_W - 1);
                        const int input_index = native_view_cover(views, original_count, sx, source_y, 1, 1);
                        const GlNativeViewTarget *input = input_index >= 0 ? &views->targets[input_index] : NULL;
                        if (input && snapshot[input_index]) {
                            color = snapshot[input_index][(size_t)(source_y - input->y) * views->width +
                                (sx - input->x) + views->offset];
                            gpu_source = native_view_domain(views, input) + 1u;
                            gpu_x = sx - input->x + views->offset;
                        } else {
                            color = copy_pixels[(size_t)dy * operation->width + dx];
                            gpu_source = 0u; gpu_x = sx;
                        }
                    } else
                        color = conv_1555_to_rgba8(native_read_u16((const uint8_t *)upload->view.bytes +
                            (size_t)dy * upload->view.descriptor.row_pitch + (size_t)dx * 2u));
                }
                const uint32_t gpu_mask = operation->mask_set | (operation->mask_check << 1u);
                if (gpu_source != UINT32_MAX || operation->kind == XG_RENDER_NATIVE_OPERATION_FILL) {
                    if (!native_gpu_span(views->gpu, domain+1u, x, target->y+y, 1, 1,
                        gpu_source, gpu_x, source_y, 1, 1, gpu_source == UINT32_MAX ? color : 0u, gpu_mask)) goto finished;
                } else if (!native_gpu_pixel(views->gpu, domain+1u, x, target->y+y, color, gpu_mask)) goto finished;
                if (operation->mask_check && (*destination >> 24u)) continue;
                *destination = color | (operation->mask_set ? UINT32_C(0xff000000) : 0u);
                if (target_depth) target_depth[(size_t)y * views->width + x] = 0u;
            }
        }
        if (copy) audit->view_copies++;
        if (!copy && full_x && full_y && !operation->mask_check)
            native_wave_capture_reset(&target->wave);
    }
    result = 1;
finished:
    for (uint32_t i = 0u; i < GL_NATIVE_VIEW_TARGET_CAPACITY; ++i) free(old_owned[i]);
    return result;
}

static uint32_t native_tint_rgba(uint32_t source, uint32_t tint) {
    uint32_t result = 0u;
    for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
        uint32_t value = ((source >> shift) & 0xffu) *
                         ((tint >> shift) & 0xffu) / 255u;
        result |= value << shift;
    }
    return result;
}

static void native_alpha_over(uint32_t *destination, uint32_t source) {
    uint32_t alpha = source >> 24u;
    uint32_t inverse = 255u - alpha;
    uint32_t mask = *destination & UINT32_C(0xff000000);
    uint32_t result = 0u;
    for (uint32_t shift = 0u; shift < 24u; shift += 8u) {
        uint32_t value = (((source >> shift) & 0xffu) * alpha +
            ((*destination >> shift) & 0xffu) * inverse + 127u) / 255u;
        result |= value << shift;
    }
    *destination = result | mask;
}

static int native_ui_target_pixel(GlNativeCpuCompiler *compiler,
                              GlNativeCpuSurface *target, int x, int y,
                              uint32_t **out_pixel) {
    const XgRenderResourceDescriptor *descriptor =
        &target->resource->view.descriptor;
    int result;
    if ((descriptor->flags &
         XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) == 0u &&
        descriptor->width == compiler->audit->header.display.width &&
        descriptor->height == compiler->audit->header.display.height)
        result = native_surface_local_pixel(target, x, y, out_pixel);
    else
        result = native_surface_pixel(target,
            x + compiler->audit->header.display.display_x,
            y + compiler->audit->header.display.display_y, out_pixel);
    return result && native_pixel_in_viewport(compiler, target, *out_pixel);
}

static int native_ui_blit(GlNativeCpuCompiler *compiler, GlNativeCpuSurface *target,
                      GlNativeResourceRecord *resource,
                      GlNativeResourceRecord *clut_resource,
                      const XgSemanticUiRect *bounds,
                      const XgSemanticUiRect *clip, int clip_enabled,
                      uint32_t tint, uint32_t blocker, uint32_t record_index,
                      const XgRenderUiLayoutUvRect *source_uv,
                      uint16_t palette_index) {
    GlNativeCpuSurface *source = NULL;
    const XgRenderResourceDescriptor *descriptor;
    XgRenderUiLayoutUvRect uv;
    uint64_t source_w, source_h;
    uint64_t palette_base = 0u;
    int indexed;
    int left, top, right, bottom;
    if (!bounds || bounds->width <= 0 || bounds->height <= 0) return 1;
    if ((int64_t)bounds->x + bounds->width > INT32_MAX ||
        (int64_t)bounds->x + bounds->width < INT32_MIN ||
        (int64_t)bounds->y + bounds->height > INT32_MAX ||
        (int64_t)bounds->y + bounds->height < INT32_MIN) {
        native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                       record_index, 0u, 0u);
        return 0;
    }
    if (!native_pixel_descriptor_valid(resource, 0)) {
        native_audit_block(compiler->audit, blocker, record_index,
                       resource ? resource->reference.resource_id : 0u,
                       resource ? resource->reference.generation : 0u);
        return 0;
    }
    descriptor = &resource->view.descriptor;
    indexed = descriptor->pixel_format ==
                  XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 ||
              descriptor->pixel_format ==
                  XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8;
    uv = source_uv ? *source_uv : (XgRenderUiLayoutUvRect){
        0u, 0u, (uint64_t)descriptor->width << 16u,
        (uint64_t)descriptor->height << 16u,
    };
    if (descriptor->width > INT_MAX || descriptor->height > INT_MAX ||
        uv.left_fp16 >= uv.right_fp16 || uv.top_fp16 >= uv.bottom_fp16 ||
        uv.right_fp16 > ((uint64_t)descriptor->width << 16u) ||
        uv.bottom_fp16 > ((uint64_t)descriptor->height << 16u) ||
        (palette_index != 0u && (!indexed || !source_uv))) {
        native_audit_block(compiler->audit, blocker, record_index,
                       resource->reference.resource_id,
                       resource->reference.generation);
        return 0;
    }
    source_w = uv.right_fp16 - uv.left_fp16;
    source_h = uv.bottom_fp16 - uv.top_fp16;
    if (indexed) {
        if (!clut_resource || !native_pixel_descriptor_valid(clut_resource, 0) ||
            clut_resource->view.kind != XG_RENDER_RESOURCE_CLUT ||
            clut_resource->view.descriptor.pixel_format !=
                XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555) {
            native_audit_block(compiler->audit, blocker, record_index,
                clut_resource ? clut_resource->reference.resource_id : 0u,
                clut_resource ? clut_resource->reference.generation : 0u);
            return 0;
        }
        if (source_uv) {
            /* Glyph banks address logical texels, not row padding. Other UI
             * nodes already select their CLUT and retain row-zero sampling. */
            const uint32_t bank_entries = descriptor->pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 ? 16u : 256u;
            const XgRenderResourceDescriptor *clut =
                &clut_resource->view.descriptor;
            const uint64_t entry_count = (uint64_t)clut->width * clut->height;
            palette_base = (uint64_t)palette_index * bank_entries;
            if (palette_base > entry_count ||
                bank_entries > entry_count - palette_base) {
                native_audit_block(compiler->audit, blocker, record_index,
                    clut_resource->reference.resource_id,
                    clut_resource->reference.generation);
                return 0;
            }
        }
    } else if (!native_cpu_surface_load(compiler, resource, blocker, record_index, 1,
                                    &source)) {
        return 0;
    }
    left = bounds->x; top = bounds->y;
    right = bounds->x + bounds->width;
    bottom = bounds->y + bounds->height;
    if (clip_enabled) {
        int64_t clip_right = (int64_t)clip->x + clip->width;
        int64_t clip_bottom = (int64_t)clip->y + clip->height;
        if (clip->width < 0 || clip->height < 0 ||
            clip_right > INT32_MAX || clip_right < INT32_MIN ||
            clip_bottom > INT32_MAX || clip_bottom < INT32_MIN) {
            native_audit_block(compiler->audit,
                           GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           record_index, 0u, 0u);
            return 0;
        }
        if (left < clip->x) left = clip->x;
        if (top < clip->y) top = clip->y;
        if (right > clip_right) right = (int)clip_right;
        if (bottom > clip_bottom) bottom = (int)clip_bottom;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > compiler->audit->header.display.width)
        right = compiler->audit->header.display.width;
    if (bottom > compiler->audit->header.display.height)
        bottom = compiler->audit->header.display.height;
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        const uint64_t dx = (uint64_t)((int64_t)x - bounds->x);
        const uint64_t dy = (uint64_t)((int64_t)y - bounds->y);
        /* Keep fractional source bounds through clipping/scaling. Splitting
         * quotient/remainder avoids overflowing the 16.16 span product. */
        const int sx = (int)((uv.left_fp16 +
            (source_w / bounds->width) * dx +
            (source_w % bounds->width) * dx / bounds->width) >> 16u);
        const int sy = (int)((uv.top_fp16 +
            (source_h / bounds->height) * dy +
            (source_h % bounds->height) * dy / bounds->height) >> 16u);
        uint32_t *src_pixel = NULL, *dst_pixel;
        uint32_t source_color;
        if (!native_ui_target_pixel(compiler, target, x, y, &dst_pixel))
            continue;
        if (indexed) {
            const uint8_t *row = (const uint8_t *)resource->view.bytes +
                (size_t)sy * descriptor->row_pitch;
            int index = descriptor->pixel_format ==
                    XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4
                ? (row[(uint32_t)sx >> 1u] >> ((sx & 1) * 4)) & 15
                : row[sx];
            uint16_t word;
            int clut_x = index;
            int clut_y = 0;
            if (source_uv) {
                const uint64_t entry = palette_base + (uint32_t)index;
                const uint32_t clut_width = clut_resource->view.descriptor.width;
                clut_x = (int)(entry % clut_width);
                clut_y = (int)(entry / clut_width);
            }
            if (!native_resource_word_local(compiler, clut_resource,
                                        clut_x, clut_y, &word)) {
                native_audit_block(compiler->audit, blocker, record_index,
                    clut_resource->reference.resource_id,
                    clut_resource->reference.generation);
                return 0;
            }
            source_color = word == 0u ? 0u :
                conv_1555_to_rgba8(word) | UINT32_C(0xff000000);
        } else {
            if (!native_surface_local_pixel(source, sx, sy, &src_pixel)) continue;
            source_color = *src_pixel;
        }
        if (descriptor->pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 ||
            descriptor->pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555)
            source_color = (source_color & UINT32_C(0x00ffffff)) |
                (native_rgba_to_1555(source_color) != 0u
                    ? UINT32_C(0xff000000) : 0u);
        native_alpha_over(dst_pixel, native_tint_rgba(source_color, tint));
    }
    return 1;
}

static int native_ui_rect_intersect(XgSemanticUiRect *rectangle,
                                const XgSemanticUiRect *clip) {
    int64_t left = rectangle->x > clip->x ? rectangle->x : clip->x;
    int64_t top = rectangle->y > clip->y ? rectangle->y : clip->y;
    int64_t rectangle_right = (int64_t)rectangle->x + rectangle->width;
    int64_t rectangle_bottom = (int64_t)rectangle->y + rectangle->height;
    int64_t clip_right = (int64_t)clip->x + clip->width;
    int64_t clip_bottom = (int64_t)clip->y + clip->height;
    int64_t right = rectangle_right < clip_right
        ? rectangle_right : clip_right;
    int64_t bottom = rectangle_bottom < clip_bottom
        ? rectangle_bottom : clip_bottom;
    if (left < INT32_MIN || left > INT32_MAX || top < INT32_MIN ||
        top > INT32_MAX || right <= left || bottom <= top ||
        right - left > INT32_MAX || bottom - top > INT32_MAX) {
        rectangle->width = 0;
        rectangle->height = 0;
        return 0;
    }
    *rectangle = (XgSemanticUiRect){
        (int32_t)left, (int32_t)top,
        (int32_t)(right - left), (int32_t)(bottom - top),
    };
    return 1;
}

static int native_ui_effective_clip(GlNativeCpuCompiler *compiler,
                                const XgSemanticUiNodeRecord *node,
                                XgSemanticUiRect *out_clip,
                                int *out_clip_enabled) {
    const XgSemanticUiNodeRecord *current = node;
    uint32_t depth = 0u;
    *out_clip = node->clip;
    *out_clip_enabled = node->clip_enabled ? 1 : 0;
    while (current) {
        uint64_t parent_id;
        if (!current->visible) return 0;
        if (current != node &&
            (current->clip_enabled ||
             current->kind == XG_SEMANTIC_UI_NODE_MASK)) {
            const XgSemanticUiRect parent_clip = current->clip_enabled
                ? current->clip : current->bounds;
            if (*out_clip_enabled) {
                if (!native_ui_rect_intersect(out_clip, &parent_clip)) return 0;
            } else {
                *out_clip = parent_clip;
                *out_clip_enabled = 1;
            }
        }
        parent_id = current->parent_node_id;
        if (parent_id == 0u) break;
        current = NULL;
        for (uint32_t i = 0u; i < compiler->audit->consumed_ui_nodes; ++i)
            if (compiler->audit->ui_nodes[i].node_id == parent_id) {
                current = &compiler->audit->ui_nodes[i];
                break;
            }
        if (!current || ++depth > compiler->audit->consumed_ui_nodes)
            return -1;
    }
    return 1;
}

static int native_render_ui_node(GlNativeCpuCompiler *compiler,
                             GlNativeCpuSurface *target,
                             const XgSemanticUiNodeRecord *node,
                             uint32_t record_index) {
    GlNativeResourceRecord *resource;
    GlNativeResourceRecord *clut = NULL;
    XgSemanticUiRect effective_clip;
    int clip_enabled;
    int hierarchy = native_ui_effective_clip(
        compiler, node, &effective_clip, &clip_enabled);
    if (hierarchy == 0) return 1;
    if (hierarchy < 0) {
        native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                       record_index, 0u, 0u);
        return 0;
    }
    if (node->temporal_mode != XG_SEMANTIC_UI_TEMPORAL_DISCRETE ||
        node->kind >= XG_SEMANTIC_UI_NODE_KIND_COUNT) {
        native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                       record_index, 0u, 0u);
        return 0;
    }
    switch (node->kind) {
    case XG_SEMANTIC_UI_NODE_GROUP:
    case XG_SEMANTIC_UI_NODE_CHOICE:
    case XG_SEMANTIC_UI_NODE_MASK:
    case XG_SEMANTIC_UI_NODE_TEXT:
        return 1;
    case XG_SEMANTIC_UI_NODE_MODEL_PREVIEW:
        native_audit_block(compiler->audit,
                       GL_RENDERER_NATIVE_BLOCKER_UNSUPPORTED_UI_NODE,
                       record_index, node->model.resource_id,
                       node->model.generation);
        return 0;
    default:
        break;
    }
    resource = native_audit_resource(compiler->audit,
                                 node->resource.resource_id,
                                 node->resource.generation);
    if (!resource) {
        if (node->kind == XG_SEMANTIC_UI_NODE_GAUGE &&
            node->maximum > 0 && node->value >= 0) {
            XgSemanticUiRect gauge = node->bounds;
            int32_t value = node->value > node->maximum
                ? node->maximum : node->value;
            if (gauge.width < 0 || gauge.height < 0) {
                native_audit_block(compiler->audit,
                               GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                               record_index, 0u, 0u);
                return 0;
            }
            gauge.width = (int32_t)((int64_t)gauge.width * value /
                                    node->maximum);
            if (clip_enabled && !native_ui_rect_intersect(
                    &gauge, &effective_clip))
                return 1;
            {
                const XgSemanticUiRect display = {
                    0, 0, compiler->audit->header.display.width,
                    compiler->audit->header.display.height,
                };
                if (!native_ui_rect_intersect(&gauge, &display)) return 1;
            }
            for (int y = gauge.y; y < gauge.y + gauge.height; ++y)
                for (int x = gauge.x; x < gauge.x + gauge.width; ++x) {
                    uint32_t *pixel;
                    if (native_ui_target_pixel(compiler, target, x, y, &pixel))
                        native_alpha_over(pixel, node->color);
                }
            compiler->audit->rendered_ui_nodes++;
            return 1;
        }
        native_audit_block(compiler->audit,
            GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR, record_index,
            node->resource.resource_id, node->resource.generation);
        return 0;
    }
    if (node->clut.resource_id != 0u)
        clut = native_audit_resource(compiler->audit, node->clut.resource_id,
                                 node->clut.generation);
    if (!native_ui_blit(compiler, target, resource, clut,
                    &node->bounds, &effective_clip,
                    clip_enabled, node->color,
                    GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR,
                    record_index, NULL, 0u))
        return 0;
    compiler->audit->rendered_ui_nodes++;
    return 1;
}

static int native_render_ui_run(GlNativeCpuCompiler *compiler,
                            GlNativeCpuSurface *target,
                            const XgSemanticUiGlyphRunRecord *run,
                            uint32_t record_index) {
    GlNativeResourceRecord *atlas = native_audit_resource(
        compiler->audit, run->atlas.resource_id, run->atlas.generation);
    GlNativeResourceRecord *clut = NULL;
    const XgSemanticUiNodeRecord *node = NULL;
    XgSemanticUiRect effective_clip = run->clip;
    int clip_enabled = run->clip_enabled ? 1 : 0;
    if (!atlas || run->temporal_mode != XG_SEMANTIC_UI_TEMPORAL_DISCRETE ||
        run->placement_offset > compiler->audit->consumed_ui_glyph_placements ||
        run->placement_count > compiler->audit->consumed_ui_glyph_placements -
                                   run->placement_offset ||
        run->reveal_count > run->total_count) {
        native_audit_block(compiler->audit,
            atlas ? GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD
                  : GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR,
            record_index, run->atlas.resource_id, run->atlas.generation);
        return 0;
    }
    for (uint32_t i = 0u; i < compiler->audit->consumed_ui_nodes; ++i)
        if (compiler->audit->ui_nodes[i].node_id == run->node_id) {
            node = &compiler->audit->ui_nodes[i];
            break;
        }
    if (!node) {
        native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                       record_index, run->atlas.resource_id,
                       run->atlas.generation);
        return 0;
    }
    if (node->clut.resource_id != 0u) {
        clut = native_audit_resource(compiler->audit, node->clut.resource_id,
                                 node->clut.generation);
        if (!clut) {
            native_audit_block(compiler->audit,
                GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR, record_index,
                node->clut.resource_id, node->clut.generation);
            return 0;
        }
    }
    {
        XgSemanticUiRect node_clip;
        int node_clip_enabled;
        int hierarchy = native_ui_effective_clip(
            compiler, node, &node_clip, &node_clip_enabled);
        if (hierarchy == 0) return 1;
        if (hierarchy < 0) {
            native_audit_block(compiler->audit,
                           GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           record_index, 0u, 0u);
            return 0;
        }
        if (node_clip_enabled) {
            if (clip_enabled) {
                if (!native_ui_rect_intersect(&effective_clip, &node_clip))
                    return 1;
            } else {
                effective_clip = node_clip;
                clip_enabled = 1;
            }
        }
    }
    for (uint32_t i = 0u; i < run->placement_count; ++i) {
        const XgSemanticUiGlyphPlacementRecord *placement =
            &compiler->audit->ui_placements[run->placement_offset + i];
        XgSemanticUiRect bounds = {
            placement->x, placement->y, placement->width, placement->height,
        };
        if (placement->glyph_run_id != run->glyph_run_id ||
            placement->layer >= XG_SEMANTIC_UI_GLYPH_LAYER_COUNT ||
            placement->glyph_index >= run->total_count ||
            (placement->glyph_index < run->reveal_count &&
             !native_ui_blit(compiler, target, atlas, clut,
                        &bounds, &effective_clip,
                         clip_enabled, placement->color,
                         GL_RENDERER_NATIVE_BLOCKER_UI_RESOURCE_DESCRIPTOR,
                         record_index, &placement->source_uv,
                         placement->palette_index))) {
            if (compiler->audit->blocker == GL_RENDERER_NATIVE_BLOCKER_NONE)
                native_audit_block(compiler->audit,
                    GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD, record_index,
                    run->atlas.resource_id, run->atlas.generation);
            return 0;
        }
        if (placement->glyph_index < run->reveal_count)
            compiler->audit->rendered_ui_glyphs++;
    }
    return 1;
}

static int native_ordered_compare(const void *left_pointer,
                              const void *right_pointer) {
    const GlNativeOrderedRecord *left = (const GlNativeOrderedRecord *)left_pointer;
    const GlNativeOrderedRecord *right = (const GlNativeOrderedRecord *)right_pointer;
#define NATIVE_ORDER_COMPARE(field) \
    do { if (left->order.field != right->order.field) \
        return left->order.field < right->order.field ? -1 : 1; } while (0)
    NATIVE_ORDER_COMPARE(pass_id);
    NATIVE_ORDER_COMPARE(layer);
    NATIVE_ORDER_COMPARE(authored_depth);
    NATIVE_ORDER_COMPARE(insertion_ordinal);
    NATIVE_ORDER_COMPARE(split_ordinal);
#undef NATIVE_ORDER_COMPARE
    if (left->kind != right->kind) return left->kind < right->kind ? -1 : 1;
    return left->index < right->index ? -1 : left->index != right->index;
}

static int native_edge_texel(GlNativeCpuCompiler *compiler,
                         const XgSemanticSurfaceEdge *edge,
                         GlNativeResourceRecord *source_resource,
                         GlNativeResourceRecord *clut_resource,
                         int source_x, int source_y, uint32_t record_index,
                         uint32_t *out_color, int *out_transparent) {
    const XgRenderResourceDescriptor *descriptor =
        &source_resource->view.descriptor;
    const int windowed_x = edge->sample.texture_window_mask_x != 0u ||
            edge->sample.texture_window_offset_x != 0u
        ? native_texture_window_coord(source_x,
            edge->sample.texture_window_mask_x,
            edge->sample.texture_window_offset_x)
        : source_x;
    const int windowed_y = edge->sample.texture_window_mask_y != 0u ||
            edge->sample.texture_window_offset_y != 0u
        ? native_texture_window_coord(source_y,
            edge->sample.texture_window_mask_y,
            edge->sample.texture_window_offset_y)
        : source_y;
    const int wrapped_x = native_wrap_coord(windowed_x,
        edge->source.width, edge->sample.wrap_u);
    const int wrapped_y = native_wrap_coord(windowed_y,
        edge->source.height, edge->sample.wrap_v);
    int local_x;
    int local_y;

    if (wrapped_x < 0 || wrapped_y < 0) return 0;
    local_x = edge->source.x + wrapped_x;
    local_y = edge->source.y + wrapped_y;
    if (descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4 ||
        descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8) {
        const uint8_t *row;
        uint16_t texel;
        int palette_index;

        if (!edge->sample.palette_enabled || !clut_resource ||
            !native_pixel_descriptor_valid(clut_resource, 0) || local_x < 0 ||
            local_y < 0 || (uint32_t)local_x >= descriptor->width ||
            (uint32_t)local_y >= descriptor->height ||
            clut_resource->view.kind != XG_RENDER_RESOURCE_CLUT ||
            clut_resource->view.descriptor.pixel_format !=
                XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555 ||
            (descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4
                 ? edge->sample.texture_depth != XG_RENDER_IR_TEXTURE_4_BIT
                 : edge->sample.texture_depth != XG_RENDER_IR_TEXTURE_8_BIT))
            return 0;
        row = (const uint8_t *)source_resource->view.bytes +
            (size_t)local_y * descriptor->row_pitch;
        palette_index = descriptor->pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4
            ? (row[(uint32_t)local_x >> 1u] >> ((local_x & 1) * 4)) & 15
            : row[local_x];
        if ((clut_resource->view.descriptor.flags &
             XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) != 0u) {
            if (!native_resource_word(compiler, clut_resource,
                    (int)edge->sample.clut_x + palette_index,
                    edge->sample.clut_y, &texel))
                return 0;
        } else if (!native_resource_word_local(compiler, clut_resource,
                       (int)edge->sample.clut_x + palette_index,
                       edge->sample.clut_y, &texel)) {
            return 0;
        }
        *out_color = conv_1555_to_rgba8(texel);
        *out_transparent = texel == 0u;
        return 1;
    }
    if (edge->sample.palette_enabled) return 0;
    {
        GlNativeCpuSurface *source = NULL;
        uint32_t *pixel;
        if (!native_cpu_surface_load(compiler, source_resource,
                edge->kind == XG_SEMANTIC_SURFACE_MOVIE
                    ? GL_RENDERER_NATIVE_BLOCKER_MOVIE_LAYOUT
                    : GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_DESCRIPTOR,
                record_index, 1, &source) ||
            !native_surface_local_pixel(source, local_x, local_y, &pixel))
            return 0;
        *out_color = *pixel;
        *out_transparent =
            (edge->kind == XG_SEMANTIC_SURFACE_SAMPLE ||
             edge->kind == XG_SEMANTIC_SURFACE_FEEDBACK) &&
            descriptor->pixel_format == XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 &&
            native_rgba_to_1555(*pixel) == 0u;
        return 1;
    }
}

static int native_edge_sample(GlNativeCpuCompiler *compiler,
                          const XgSemanticSurfaceEdge *edge,
                          GlNativeResourceRecord *source_resource,
                          GlNativeResourceRecord *clut_resource,
                          uint32_t destination_x, uint32_t destination_y,
                          uint32_t record_index, uint32_t *out_color,
                          int *out_transparent) {
    const double source_x =
        ((double)destination_x + 0.5) * edge->source.width /
            edge->destination.width - 0.5;
    const double source_y =
        ((double)destination_y + 0.5) * edge->source.height /
            edge->destination.height - 0.5;

    if (edge->sample.sampler == XG_RENDER_RESOURCE_SAMPLER_NEAREST) {
        const int nearest_x = (int)(source_x + 0.5);
        const int nearest_y = (int)(source_y + 0.5);
        return native_edge_texel(compiler, edge, source_resource, clut_resource,
                             nearest_x, nearest_y, record_index, out_color,
                             out_transparent);
    }
    if (edge->sample.sampler == XG_RENDER_RESOURCE_SAMPLER_LINEAR) {
        const int x0 = (int)source_x - (source_x < (int)source_x ? 1 : 0);
        const int y0 = (int)source_y - (source_y < (int)source_y ? 1 : 0);
        const double fraction_x = source_x - x0;
        const double fraction_y = source_y - y0;
        const double weights[4] = {
            (1.0 - fraction_x) * (1.0 - fraction_y),
            fraction_x * (1.0 - fraction_y),
            (1.0 - fraction_x) * fraction_y,
            fraction_x * fraction_y,
        };
        uint32_t colors[4];
        int transparent[4];

        if (!native_edge_texel(compiler, edge, source_resource, clut_resource,
                x0, y0, record_index, &colors[0], &transparent[0]) ||
            !native_edge_texel(compiler, edge, source_resource, clut_resource,
                x0 + 1, y0, record_index, &colors[1], &transparent[1]) ||
            !native_edge_texel(compiler, edge, source_resource, clut_resource,
                x0, y0 + 1, record_index, &colors[2], &transparent[2]) ||
            !native_edge_texel(compiler, edge, source_resource, clut_resource,
                x0 + 1, y0 + 1, record_index, &colors[3], &transparent[3]))
            return 0;
        *out_color = 0u;
        for (uint32_t channel = 0u; channel < 4u; ++channel) {
            double value = 0.0;
            for (uint32_t sample = 0u; sample < 4u; ++sample)
                if (!transparent[sample])
                    value += ((colors[sample] >> (channel * 8u)) & 0xffu) *
                        weights[sample];
            *out_color |= (uint32_t)(value + 0.5) << (channel * 8u);
        }
        *out_transparent = 1;
        for (uint32_t sample = 0u; sample < 4u; ++sample)
            if (weights[sample] > 0.0 && !transparent[sample])
                *out_transparent = 0;
        return 1;
    }
    return 0;
}

static int native_apply_surface_edge(GlNativeCpuCompiler *compiler,
                                 GlNativeCpuSurface *target,
                                 const XgSemanticSurfaceEdge *edge,
                                 uint32_t record_index) {
    GlNativeResourceRecord *source_resource = native_audit_resource(
        compiler->audit, edge->source_surface_id, edge->source_generation);
    GlNativeResourceRecord *clut_resource = edge->sample.clut_resource_id == 0u
        ? NULL : native_audit_resource(compiler->audit,
            edge->sample.clut_resource_id, edge->sample.clut_generation);
    uint32_t *sampled = NULL;
    uint8_t *transparent = NULL;
    size_t count;
    const int psx_sample =
        (edge->kind == XG_SEMANTIC_SURFACE_SAMPLE ||
         edge->kind == XG_SEMANTIC_SURFACE_FEEDBACK) &&
        (edge->sample.palette_enabled ||
         (source_resource && source_resource->view.descriptor.pixel_format ==
             XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555));

    if (!source_resource || !source_resource->view_valid || !target ||
        !target->valid || edge->source.x < 0 || edge->source.y < 0 ||
        edge->source.width == 0u || edge->source.height == 0u ||
        edge->destination.width == 0u || edge->destination.height == 0u ||
        edge->source.width > INT_MAX || edge->source.height > INT_MAX ||
        edge->destination.width > INT_MAX ||
        edge->destination.height > INT_MAX ||
        edge->kind > XG_SEMANTIC_SURFACE_MOVIE ||
        edge->sample.sampler < XG_RENDER_RESOURCE_SAMPLER_NEAREST ||
        edge->sample.sampler > XG_RENDER_RESOURCE_SAMPLER_LINEAR ||
        edge->sample.wrap_u < XG_RENDER_RESOURCE_WRAP_CLAMP ||
        edge->sample.wrap_u > XG_RENDER_RESOURCE_WRAP_MIRROR ||
        edge->sample.wrap_v < XG_RENDER_RESOURCE_WRAP_CLAMP ||
        edge->sample.wrap_v > XG_RENDER_RESOURCE_WRAP_MIRROR ||
        edge->sample.blend_mode > XG_RENDER_IR_BLEND_ADD_QUARTER ||
        edge->sample.texture_depth > XG_RENDER_IR_TEXTURE_15_BIT ||
        ((edge->sample.clut_resource_id == 0u) !=
         (edge->sample.clut_generation == 0u)) ||
        (edge->effect_phase_count == 0u && edge->effect_phase != 0u) ||
        (edge->effect_phase_count != 0u &&
         edge->effect_phase >= edge->effect_phase_count) ||
        (uint64_t)(uint32_t)edge->source.x + edge->source.width >
            source_resource->view.descriptor.width ||
        (uint64_t)(uint32_t)edge->source.y + edge->source.height >
            source_resource->view.descriptor.height ||
        (edge->kind == XG_SEMANTIC_SURFACE_COPY &&
         (edge->source.width != edge->destination.width ||
          edge->source.height != edge->destination.height ||
          edge->sample.sampler != XG_RENDER_RESOURCE_SAMPLER_NEAREST ||
          edge->sample.palette_enabled ||
          edge->sample.semi_transparent))) {
        if (compiler->audit->blocker == GL_RENDERER_NATIVE_BLOCKER_NONE)
            native_audit_block(compiler->audit,
                GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_DESCRIPTOR,
                record_index, edge->source_surface_id,
                edge->source_generation);
        return 0;
    }
    if ((edge->kind == XG_SEMANTIC_SURFACE_MOVIE &&
         source_resource->view.kind != XG_RENDER_RESOURCE_MOVIE_FRAME) ||
        (edge->kind != XG_SEMANTIC_SURFACE_MOVIE &&
          source_resource->view.kind != XG_RENDER_RESOURCE_GENERATED_SURFACE &&
          source_resource->view.kind != XG_RENDER_RESOURCE_FRAMEBUFFER &&
          !((edge->kind == XG_SEMANTIC_SURFACE_COPY ||
             edge->kind == XG_SEMANTIC_SURFACE_SAMPLE) &&
            source_resource->view.kind == XG_RENDER_RESOURCE_TEXTURE)) ||
        (edge->kind == XG_SEMANTIC_SURFACE_MOVIE &&
         (source_resource->view.descriptor.pixel_format ==
              XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24) !=
             compiler->audit->header.display.depth24) ||
        (edge->sample.palette_enabled && !clut_resource) ||
        (!edge->sample.palette_enabled && clut_resource) ||
        (edge->kind == XG_SEMANTIC_SURFACE_MOVIE &&
         (edge->sample.palette_enabled ||
          source_resource->view.kind != XG_RENDER_RESOURCE_MOVIE_FRAME))) {
        native_audit_block(compiler->audit,
            edge->kind == XG_SEMANTIC_SURFACE_MOVIE
                ? GL_RENDERER_NATIVE_BLOCKER_MOVIE_LAYOUT
                : GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_DESCRIPTOR,
            record_index, edge->source_surface_id, edge->source_generation);
        return 0;
    }
    if (edge->destination.width > SIZE_MAX / edge->destination.height ||
        (count = (size_t)edge->destination.width *
             edge->destination.height) > SIZE_MAX / sizeof(*sampled) ||
        !(sampled = (uint32_t *)malloc(count * sizeof(*sampled))) ||
        !(transparent = (uint8_t *)malloc(count))) {
        free(sampled);
        native_audit_block(compiler->audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
                       record_index, edge->source_surface_id,
                       edge->source_generation);
        return 0;
    }
    for (uint32_t y = 0u; y < edge->destination.height; ++y)
        for (uint32_t x = 0u; x < edge->destination.width; ++x) {
            const size_t sample = (size_t)y * edge->destination.width + x;
            int is_transparent;
            if (!native_edge_sample(compiler, edge, source_resource, clut_resource,
                    x, y, record_index, &sampled[sample], &is_transparent)) {
                free(transparent);
                free(sampled);
                native_audit_block(compiler->audit,
                    edge->kind == XG_SEMANTIC_SURFACE_MOVIE
                        ? GL_RENDERER_NATIVE_BLOCKER_MOVIE_LAYOUT
                        : GL_RENDERER_NATIVE_BLOCKER_SURFACE_EDGE_DESCRIPTOR,
                    record_index, edge->source_surface_id,
                    edge->source_generation);
                return 0;
            }
            transparent[sample] = (uint8_t)is_transparent;
        }
    for (uint32_t y = 0u; y < edge->destination.height; ++y)
        for (uint32_t x = 0u; x < edge->destination.width; ++x) {
            const size_t sample = (size_t)y * edge->destination.width + x;
            uint32_t *target_pixel;
            const int dx = edge->destination.x + (int)x;
            const int dy = edge->destination.y + (int)y;
            const uint32_t source_color = sampled[sample];
            /* Match the GL sampler's filtered STP threshold only for PSX
             * texture sampling; copying/scanout retain their existing masks. */
            const int source_stp = psx_sample
                ? (source_color >> 24u) >= 128u : (source_color >> 24u) != 0u;
            int source_mask;
            if (!native_surface_local_pixel(target, dx, dy, &target_pixel) ||
                !native_pixel_in_viewport(compiler, target, target_pixel) ||
                transparent[sample])
                continue;
            if (edge->sample.mask_check && (*target_pixel >> 24u) != 0u)
                continue;
            source_mask = edge->sample.mask_set ||
                ((edge->sample.palette_enabled ||
                  source_resource->view.descriptor.pixel_format ==
                      XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555) &&
                 source_stp);
            if (edge->sample.semi_transparent && (!psx_sample || source_stp)) {
                *target_pixel = native_psx_fragment(*target_pixel,
                    (int)(source_color & 0xffu) >> 3u,
                    (int)((source_color >> 8u) & 0xffu) >> 3u,
                    (int)((source_color >> 16u) & 0xffu) >> 3u,
                    source_mask, 1, edge->sample.blend_mode);
            } else {
                *target_pixel = (source_color & UINT32_C(0x00ffffff)) |
                    (source_mask ? UINT32_C(0xff000000) : 0u);
            }
        }
    free(transparent);
    free(sampled);
    compiler->audit->rendered_surface_edges++;
    if (edge->kind == XG_SEMANTIC_SURFACE_MOVIE) {
        compiler->audit->endpoint_was_movie = 1;
        compiler->audit->endpoint_was_depth24 =
            source_resource->view.descriptor.pixel_format ==
                XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24;
        compiler->audit->endpoint_was_discrete = 1;
    }
    return 1;
}

/* Complete replay. Shared source-transform curves and local-vertex GTE anchors
 * preserve both endpoint planes, without screen-image warping or polygon fits. */
/* A phase's pose deltas on one mesh vertex: the per-draw replay's own rules
 * (positions, phase-continuous projective Z, depth key). False when a
 * position leaves the 16.16 range. */
static int native_mesh_vertex_move(XgRenderIrVertex *vertex, const double screen[3],
                                   const double native[3]) {
    const double x = vertex->x + screen[0] * 65536.0, y = vertex->y + screen[1] * 65536.0;
    const double native_x = vertex->native_view_x + native[0] * 65536.0;
    const double native_y = vertex->native_view_y + native[1] * 65536.0;
    if (!isfinite(x) || !isfinite(y) || x < INT32_MIN || x > INT32_MAX ||
        y < INT32_MIN || y > INT32_MAX) return 0;
    if (vertex->native_view_position &&
        (!isfinite(native_x) || !isfinite(native_y) || native_x < INT32_MIN ||
         native_x > INT32_MAX || native_y < INT32_MIN || native_y > INT32_MAX)) return 0;
    vertex->x = (int32_t)llround(x); vertex->y = (int32_t)llround(y);
    if (vertex->native_view_position) {
        vertex->native_view_x = (int32_t)llround(native_x);
        vertex->native_view_y = (int32_t)llround(native_y);
    }
    if (vertex->projective_position) {
        if (screen[2] > 0.0 && screen[2] <= 65535.0)
            vertex->projective_view_z = (int32_t)floor(screen[2]);
        else
            vertex->projective_position = false;
    }
    if (vertex->native_view_depth > 0) {
        const double depth = vertex->native_view_depth + native[2] * 4096.0;
        vertex->native_view_depth = isfinite(depth) && depth >= 1.0 && depth <= 65535.0 * 4096.0
            ? (int32_t)llround(depth) : native_depth_q12(screen[2], vertex->projective_distance);
    } else if (vertex->projective_position)
        vertex->native_view_depth = native_depth_q12(screen[2], vertex->projective_distance);
    vertex->temporal_depth_valid = false;
    return 1;
}

/* Corner endpoints shared by every phase of one interval (see
 * XgRenderMotionVertexEndpoints): offsets[i] is record i's first corner,
 * three per triangle of its pose-bound geometry. A DRAW record's corners
 * hold their endpoints (corners); a mesh record's corners name a unique
 * vertex (corner_vertices; UINT32_MAX: no enabled pose), one per pose,
 * part and LOCAL position, whose endpoints are computed once and whose
 * phase part each phase evaluates once (GlNativeMotionPhaseVertex). Unique
 * vertices of an endpoint job's records occupy slots [ranges[j][0],
 * ranges[j][1]) of the corner index space. */
typedef struct GlNativeMotionVertex {
    XgHost3dVector local;
    uint32_t motion_index, part;
} GlNativeMotionVertex;
typedef struct GlNativeMotionPhaseVertex {
    double screen[3], native[3];
    XgRenderMotionPhaseView view; /* for clipping at this phase's near plane */
    uint8_t state; /* NATIVE_MOTION_VERTEX_* */
} GlNativeMotionPhaseVertex;
enum { NATIVE_MOTION_VERTEX_ENDPOINT = 0, NATIVE_MOTION_VERTEX_PROJECTED, NATIVE_MOTION_VERTEX_INVALID };
typedef struct GlNativeMotionEndpoints {
    uint32_t *offsets;
    XgRenderMotionVertexEndpoints *corners;
    uint32_t *corner_vertices;
    GlNativeMotionVertex *vertices;
    XgRenderMotionVertexEndpoints *vertex_endpoints;
    uint32_t ranges[GL_NATIVE_MOTION_PHASE_CAPACITY][2];
    uint32_t range_count, corner_count;
} GlNativeMotionEndpoints;

static const XgRenderMotionVertexEndpoints (*native_motion_endpoints_at(
        const GlNativeMotionEndpoints *endpoints, uint32_t record, uint32_t triangle))[3] {
    if (!endpoints || !endpoints->corners) return NULL;
    return (const XgRenderMotionVertexEndpoints (*)[3])(const void *)
        (endpoints->corners + endpoints->offsets[record] + (size_t)triangle * 3u);
}

/* One phase's part of every unique mesh vertex: under its pose when that is
 * enabled for the whole interval (entities) and evaluated at this phase
 * (phase_entities). */
typedef struct GlNativePhaseVertexBatch {
    const GlNativeMotionEndpoints *endpoints;
    const GlNativeMotionEntity *entities, *phase_entities;
    const GlNativePhaseTemplates *templates;
    GlNativeMotionPhaseVertex *out;
    uint32_t offsets[GL_NATIVE_MOTION_PHASE_CAPACITY + 1u];
} GlNativePhaseVertexBatch;

static void native_motion_phase_vertex_ranges(uint32_t begin, uint32_t end, void *context) {
    const GlNativePhaseVertexBatch *batch = context;
    const GlNativeMotionEndpoints *endpoints = batch->endpoints;
    for (uint32_t r = 0u; r < endpoints->range_count; ++r)
        for (uint32_t flat = begin > batch->offsets[r] ? begin : batch->offsets[r];
             flat < end && flat < batch->offsets[r + 1u]; ++flat) {
            const uint32_t u = endpoints->ranges[r][0] + flat - batch->offsets[r];
            /* Cached records are already immutable GPU geometry. Only vertices
             * also referenced by an uncached record need phase projection. */
            if(batch->templates && batch->templates->phase_vertices_needed &&
                !batch->templates->phase_vertices_needed[u])continue;
            const GlNativeMotionVertex *vertex = &endpoints->vertices[u];
            GlNativeMotionPhaseVertex *phase = &batch->out[u];
            phase->state = NATIVE_MOTION_VERTEX_ENDPOINT;
            if (!batch->entities[vertex->motion_index].enabled) continue;
            const XgRenderMotionEvaluation *evaluation = &batch->phase_entities[vertex->motion_index].evaluation;
            if (!evaluation->interpolated) continue;
            phase->state = xg_render_motion_vertex_phase(evaluation, vertex->part, &vertex->local,
                &endpoints->vertex_endpoints[u], phase->screen, phase->native, &phase->view)
                ? NATIVE_MOTION_VERTEX_PROJECTED : NATIVE_MOTION_VERTEX_INVALID;
        }
}

static void native_motion_phase_vertices(const GlNativeMotionEndpoints *endpoints,
                                         const GlNativeMotionEntity *entities,
                                         const GlNativeMotionEntity *phase_entities,
                                         const GlNativePhaseTemplates *templates,
                                         GlNativeMotionPhaseVertex *out) {
    GlNativePhaseVertexBatch batch = {.endpoints = endpoints, .entities = entities,
        .phase_entities = phase_entities, .templates = templates, .out = out};
    for (uint32_t r = 0u; r < endpoints->range_count; ++r)
        batch.offsets[r + 1u] = batch.offsets[r] + endpoints->ranges[r][1] - endpoints->ranges[r][0];
    psx_parallel_for(batch.offsets[endpoints->range_count], 64u,
        native_motion_phase_vertex_ranges, &batch);
}

/* The near plane of a phase: just in front of the GTE divide's saturation
 * (z = distance / 2), where the continuous projection is exact. */
static double native_phase_near_plane(const XgRenderMotionPhaseView *view) {
    return view->distance * 0.5 + 8.0;
}

/* Where the phase camera puts a view-space point, in the 16.16 Native and
 * canonical coordinates of a triangle whose reference corner was moved the
 * ordinary way: the same continuous projection plus that corner's offsets
 * (relocation, Native centring), so placed points join moved ones exactly. */
typedef struct GlNativePhasePlacement {
    const XgRenderMotionPhaseView *view;
    double native_offset[2];
    int64_t canonical_offset[2];
    int native_position;
} GlNativePhasePlacement;

static void native_phase_placement(GlNativePhasePlacement *out, const XgRenderIrVertex *reference,
                                   const XgRenderMotionPhaseView *view) {
    const double q = view->distance / fmin(view->view[2], 65535.0);
    out->view = view;
    out->native_offset[0] = reference->native_view_x -
        (view->screen_offset[0] + fmax(-32768.0, fmin(view->view[0], 32767.0)) * q) * 65536.0;
    out->native_offset[1] = reference->native_view_y -
        (view->screen_offset[1] + fmax(-32768.0, fmin(view->view[1], 32767.0)) * q) * 65536.0;
    out->canonical_offset[0] = (int64_t)reference->x - reference->native_view_x;
    out->canonical_offset[1] = (int64_t)reference->y - reference->native_view_y;
    out->native_position = reference->native_view_position;
}

static int native_phase_place(const GlNativePhasePlacement *placement, const double p[3],
                              XgRenderIrVertex *vertex) {
    const double q = placement->view->distance / fmin(p[2], 65535.0);
    const double nx = (placement->view->screen_offset[0] + fmax(-32768.0, fmin(p[0], 32767.0)) * q) * 65536.0 +
        placement->native_offset[0];
    const double ny = (placement->view->screen_offset[1] + fmax(-32768.0, fmin(p[1], 32767.0)) * q) * 65536.0 +
        placement->native_offset[1];
    if (!isfinite(nx) || !isfinite(ny) || fabs(nx) > 2.0e9 || fabs(ny) > 2.0e9) return 0;
    vertex->native_view_x = (int32_t)llround(nx);
    vertex->native_view_y = (int32_t)llround(ny);
    vertex->native_view_position = placement->native_position;
    const int64_t x = vertex->native_view_x + placement->canonical_offset[0];
    const int64_t y = vertex->native_view_y + placement->canonical_offset[1];
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return 0;
    vertex->x = (int32_t)x; vertex->y = (int32_t)y;
    vertex->projective_position = true;
    vertex->projective_view_z = (int32_t)floor(p[2]);
    vertex->native_view_depth = native_depth_q12(p[2], (uint32_t)placement->view->distance);
    vertex->temporal_depth_valid = false;
    return 1;
}

/* A moved triangle that crosses its phase camera's near plane: clipped in
 * that camera's view space like any 3D renderer would, instead of drawing
 * corners behind the camera through the saturated projection (the faces
 * that leap across the screen). Each crossing interpolates the edge in view
 * space (UV and colour with it) and is placed like its corners. Writes 0..2
 * triangles at out; near flags the corners behind the plane. */
static uint32_t native_phase_clip_triangle(const XgRenderIrTriangle *moved,
                                           const XgRenderMotionPhaseView *const views[3],
                                           unsigned near_mask, const GlNativePhasePlacement *placement,
                                           XgRenderIrTriangle out[2]) {
    const double near_z = native_phase_near_plane(placement->view);
    XgRenderIrVertex polygon[4];
    unsigned count = 0u;
    for (unsigned k = 0u; k < 3u; ++k) {
        const unsigned next = (k + 1u) % 3u;
        const int a_front = !(near_mask & (1u << k)), b_front = !(near_mask & (1u << next));
        if (a_front) polygon[count++] = moved->vertices[k];
        if (a_front == b_front) continue;
        const double *a = views[k]->view, *b = views[next]->view;
        const double t = (near_z - a[2]) / (b[2] - a[2]);
        const XgRenderIrVertex *va = &moved->vertices[k], *vb = &moved->vertices[next];
        XgRenderIrVertex *c = &polygon[count++];
        *c = a_front ? *va : *vb;
        double p[3];
        for (unsigned r = 0u; r < 3u; ++r) p[r] = a[r] + t * (b[r] - a[r]);
        p[2] = near_z;
        if (!native_phase_place(placement, p, c)) return 0u;
        c->u = (int32_t)llround(va->u + t * ((double)vb->u - va->u));
        c->v = (int32_t)llround(va->v + t * ((double)vb->v - va->v));
        c->r = (uint8_t)lround(va->r + t * ((double)vb->r - va->r));
        c->g = (uint8_t)lround(va->g + t * ((double)vb->g - va->g));
        c->b = (uint8_t)lround(va->b + t * ((double)vb->b - va->b));
    }
    if (count < 3u) return 0u;
    uint32_t triangles = 0u;
    for (unsigned first = 1u; first + 1u < count; ++first) {
        out[triangles] = *moved;
        out[triangles].vertices[0] = polygon[0];
        out[triangles].vertices[1] = polygon[first];
        out[triangles].vertices[2] = polygon[first + 1u];
        ++triangles;
    }
    return triangles;
}

/* A recipe MESH at one phase: its triangles relocated exactly as the recipe
 * relocated a DRAW's semantic, then moved by their pose's phase curve
 * (each unique vertex evaluated once, phase_vertices; or two triangles at a
 * time through the shared projection cache). Without an enabled pose they
 * stay at the endpoint. Triangles whose phase projection fails are left out
 * of this phase only. */
static int native_mesh_phase_triangles(const GlNativeRecipe *recipe, const GlNativeRecipeDraw *record,
                                       uint32_t record_index,
                                       const GlNativeMotionEntity *entities,
                                       const GlNativeMotionEntity *phase_entities,
                                       const GlNativeMotionEndpoints *endpoints,
                                       const GlNativeMotionPhaseVertex *phase_vertices,
                                       GlNativeGpuMeshTriangle **out, uint32_t *out_count, int *moved_any) {
    const GlNativeRecipeMesh *mesh = record->mesh;
    *out = NULL; *out_count = 0u; *moved_any = 0;
    if (!mesh) return 0;
    const GlNativeRecipeMeshPose record_pose = {.motion_index = record->motion_index,
        .part = record->motion.motion_part_index};
    const XgRenderNativeMeshTriangle *triangles = mesh->triangles;
    /* A clipped triangle can become two. */
    GlNativeGpuMeshTriangle *moved = native_mesh_buffer(1u, mesh->count * 2u);
    if (!moved) return 0;
    const GlNativeTextureState template_texture = native_texture_state_semantic(&record->semantic.material);
    /* Coalesced draws were relocated at append, like a DRAW's semantic. */
    const int64_t dx = mesh->relocated ? 0 : record->projection_offset_x;
    const int64_t dy = mesh->relocated ? 0 : record->projection_offset_y;
    const int strip_native = !record->enhanced && recipe->view_width;
    const uint32_t *corner_vertices = phase_vertices && endpoints && endpoints->corner_vertices &&
        record->motion.motion.handle.resource_id
        ? endpoints->corner_vertices + endpoints->offsets[record_index] : NULL;
    const XgRenderMotionVertexEndpoints (*corner_endpoints)[3] =
        native_motion_endpoints_at(endpoints, record_index, 0u);
    uint32_t kept = 0u;
    for (uint32_t first = 0u, pair; first < mesh->count; first += pair) {
        double screen[2][3][3], native[2][3][3];
        const double *vertex_screen[2][3], *vertex_native[2][3];
        const XgRenderMotionPhaseView *views[3];
        unsigned near_mask = 0u, placed = 0u;
        XgRenderMotionProjectResult result = XG_RENDER_MOTION_ENDPOINT;
        if (corner_vertices) {
            /* One triangle; its corners share one pose. */
            pair = 1u;
            for (uint32_t v = 0u; v < 3u; ++v) {
                const uint32_t u = corner_vertices[(size_t)first * 3u + v];
                const GlNativeMotionPhaseVertex *phase = u == UINT32_MAX ? NULL : &phase_vertices[u];
                vertex_screen[0][v] = vertex_native[0][v] = NULL;
                views[v] = NULL;
                if (!phase || phase->state == NATIVE_MOTION_VERTEX_ENDPOINT) continue;
                if (phase->state == NATIVE_MOTION_VERTEX_INVALID) { result = XG_RENDER_MOTION_INVALID; break; }
                result = XG_RENDER_MOTION_PROJECTED;
                vertex_screen[0][v] = phase->screen; vertex_native[0][v] = phase->native;
                views[v] = &phase->view;
                if (phase->view.view[2] < native_phase_near_plane(&phase->view)) near_mask |= 1u << v;
            }
            if (result == XG_RENDER_MOTION_PROJECTED) {
                *moved_any = 1;
                /* Every corner behind this phase's near_mask plane: nothing to draw. */
                if (near_mask == 7u) continue;
                /* A corner without an endpoint Native position (its
                 * projection overflowed behind the camera) is placed from
                 * this phase's view, like a clip point. */
                for (uint32_t v = 0u; v < 3u; ++v)
                    if (!(near_mask & (1u << v)) && !strip_native &&
                        !triangles[first].triangle.vertices[v].native_view_position) placed |= 1u << v;
                for (uint32_t v = 0u; v < 3u && (near_mask | placed); ++v) if (!views[v]) near_mask = placed = 0u;
            } else near_mask = 0u;
        } else {
            /* Two triangles at a time under one pose: each triangle's own (a
             * run may cross objects) or the record's. The pose is enabled for
             * the whole interval (entities) and evaluated at this phase
             * (phase_entities, when the phase has its own copy). */
            const GlNativeRecipeMeshPose *pose = mesh->poses ? &mesh->poses[first] : &record_pose;
            pair = mesh->count - first >= 2u &&
                (!mesh->poses || native_recipe_pose_equal(pose, &mesh->poses[first + 1u])) ? 2u : 1u;
            const GlNativeMotionEntity *entity = entities && record->motion.motion.handle.resource_id &&
                pose->motion_index < recipe->motion_count && entities[pose->motion_index].enabled
                ? &(phase_entities ? phase_entities : entities)[pose->motion_index] : NULL;
            if (entity) {
                XgRenderMotionDrawBinding binding = {.motion = recipe->motions[pose->motion_index],
                    .motion_part_index = pose->part,
                    .triangle_count = pair, .native_exact = 1u};
                *moved_any = 1;
                for (uint32_t k = 0u; k < pair; ++k) {
                    memcpy(binding.local[k], triangles[first + k].local, sizeof(binding.local[k]));
                    memcpy(binding.vertex_ids[k], triangles[first + k].vertex_ids,
                           sizeof(binding.vertex_ids[k]));
                }
                result = xg_render_motion_project_shared(&entity->evaluation, &binding,
                    corner_endpoints ? corner_endpoints + first : NULL, screen, native);
                for (uint32_t k = 0u; k < pair; ++k)
                    for (uint32_t v = 0u; v < 3u; ++v) {
                        vertex_screen[k][v] = screen[k][v]; vertex_native[k][v] = native[k][v];
                    }
            }
        }
        if (result != XG_RENDER_MOTION_ENDPOINT && result != XG_RENDER_MOTION_PROJECTED) continue;
        for (uint32_t k = 0u; k < pair; ++k) {
            /* A phase-only face exists only where its camera moved, and only
             * while it faces this phase's camera. */
            const int phase_only = (triangles[first + k].flags & XG_RENDER_NATIVE_MESH_PHASE_ONLY) != 0u;
            if (phase_only && result != XG_RENDER_MOTION_PROJECTED) continue;
            /* Moved in place in this phase's buffer. */
            XgRenderIrTriangle *tri = &moved[kept].triangle;
            *tri = triangles[first + k].triangle;
            int valid = 1;
            for (uint32_t v = 0u; v < 3u && valid; ++v) {
                XgRenderIrVertex *vertex = &tri->vertices[v];
                const int64_t x = (int64_t)vertex->x + dx, y = (int64_t)vertex->y + dy;
                valid = x >= INT32_MIN && x <= INT32_MAX && y >= INT32_MIN && y <= INT32_MAX;
                vertex->x = (int32_t)x; vertex->y = (int32_t)y;
                if (vertex->native_view_position) {
                    vertex->native_view_x = (int32_t)((int64_t)vertex->native_view_x + dx);
                    vertex->native_view_y = (int32_t)((int64_t)vertex->native_view_y + dy);
                }
                if (vertex->projective_position) {
                    vertex->projective_offset_x = (int32_t)((int64_t)vertex->projective_offset_x + dx);
                    vertex->projective_offset_y = (int32_t)((int64_t)vertex->projective_offset_y + dy);
                }
                if (strip_native) vertex->native_view_position = false;
                /* A corner behind the near_mask plane is replaced by the clip,
                 * a corner without an endpoint position placed below. */
                if (valid && result == XG_RENDER_MOTION_PROJECTED && vertex_screen[k][v] &&
                    !((near_mask | placed) & (1u << v)))
                    valid = native_mesh_vertex_move(vertex, vertex_screen[k][v], vertex_native[k][v]);
            }
            if (!valid) continue;
            if (near_mask | placed) {
                /* The placement's reference: a corner moved the ordinary way. */
                unsigned reference = 0u;
                while (reference < 3u && ((near_mask | placed) & (1u << reference))) ++reference;
                if (reference == 3u) continue;
                GlNativePhasePlacement placement;
                native_phase_placement(&placement, &tri->vertices[reference], views[reference]);
                for (uint32_t v = 0u; v < 3u && valid; ++v)
                    if (placed & (1u << v)) valid = native_phase_place(&placement, views[v]->view, &tri->vertices[v]);
                if (!valid) continue;
                if (near_mask) {
                    const GlNativeTextureState texture = mesh->textures ? mesh->textures[first + k] : template_texture;
                    XgRenderIrTriangle clipped[2];
                    const uint32_t pieces = native_phase_clip_triangle(tri, views, near_mask, &placement, clipped);
                    for (uint32_t c = 0u; c < pieces; ++c) {
                        moved[kept].triangle = clipped[c];
                        moved[kept++].texture = texture;
                    }
                    continue;
                }
            }
            if (phase_only) {
                double area = 0.0;
                for (uint32_t v = 0u; v < 3u; ++v) {
                    const XgRenderIrVertex *a = &tri->vertices[v], *b = &tri->vertices[(v + 1u) % 3u];
                    const double ax = a->native_view_position ? a->native_view_x : a->x;
                    const double ay = a->native_view_position ? a->native_view_y : a->y;
                    const double bx = b->native_view_position ? b->native_view_x : b->x;
                    const double by = b->native_view_position ? b->native_view_y : b->y;
                    area += ax * by - bx * ay;
                }
                if (!(area > 0.0)) continue;
            }
            moved[kept++].texture = mesh->textures ? mesh->textures[first + k] : template_texture;
        }
    }
    *out = moved; *out_count = kept;
    return 1;
}

static int native_recipe_render_rows(const GlNativeRecipe *recipe, const GlNativeMotionEntity *entities,
                             const GlNativeVertexCache *vertices, uint32_t phase, int use_view,
                             GlNativeRecipeStats *motion_audit,
                              uint16_t crop_y, uint16_t height, uint32_t **out_pixels,
                              uint16_t row_begin, uint16_t row_end, GlNativeGpuWork *gpu,
                              const GlNativeMotionProjection *phase_projections,
                              const GlNativeMotionEntity *phase_entities,
                              const GlNativeMotionEndpoints *endpoints,
                              const GlNativeMotionPhaseVertex *phase_vertices) {
    const uint32_t width = use_view ? recipe->view_width : recipe->width;
    GlNativeCompileAudit *audit = malloc(sizeof(*audit));
    if (audit) memset(audit, 0, offsetof(GlNativeCompileAudit, passes));
    GlNativeCpuCompiler compiler = {0};
    GlNativeResourceRecord texture = {0}, clut, target_resource = {0};
    GlNativeCpuSurface native = {0}, target = {0};
    GlNativePhaseProducer producers[64] = {0};
    uint32_t producer_count = 0u;
    uint32_t *pixels = NULL;
    uint32_t *depth = NULL;
    int ok = 0;
    *out_pixels = NULL;
    if (!width || !audit || crop_y > recipe->height ||
        height > recipe->height - crop_y) goto finished;
    if (!gpu) {
        /* A captured base exists only on the GPU (the optional CPU audit). */
        if (recipe->base_serial) goto finished;
        pixels = malloc((size_t)width * recipe->height * sizeof(*pixels));
        if (!pixels) goto finished;
        for (size_t i = 0u; i < (size_t)width * recipe->height; ++i) pixels[i] = recipe->clear_color;
        /* The recipe starts at a full-target clear: its depth starts far. */
        if (recipe->depth_test && !(depth = calloc((size_t)width * recipe->height, sizeof(*depth))))
            goto finished;
    } else {
        /* Use the endpoint raster's coordinate domain and viewport. Rendering
         * the same Q16 geometry into a cropped-height viewport changes GL's
         * subpixel rounding and UV-plane origin in alternate presentations. */
        const uint16_t raster_width = use_view ? (uint16_t)width : VRAM_W;
        gpu->widths[GL_NATIVE_GPU_PHASE_BASE+phase] = raster_width;
        gpu->heights[GL_NATIVE_GPU_PHASE_BASE+phase] = VRAM_H;
        /* The phase scanout reads only rows phase_crop_y (origin_y + crop_y)
         * through + height, and every draw is clipped to them below. Seed just
         * those rows; the Field wave also reads source rows of its own plane. */
        int seed_y = recipe->wave ? 0 : (int)recipe->origin_y + crop_y;
        int seed_h = recipe->wave ? VRAM_H : height;
        if (seed_y > VRAM_H) seed_y = VRAM_H;
        if (seed_h > VRAM_H - seed_y) seed_h = VRAM_H - seed_y;
        if (gpu->capture_templates) {
            /* A geometry template has no framebuffer seed or transfer. */
        } else if (recipe->base_serial) {
            /* Same raster domain as the captured VIEW plane: copy its rows,
             * with their depth keys, exactly like a full-row VIEW copy. */
            if (!use_view || recipe->base_slot >= GL_NATIVE_VIEW_TARGET_CAPACITY ||
                !native_gpu_span(gpu, GL_NATIVE_GPU_PHASE_BASE+phase, 0, seed_y, raster_width, seed_h,
                    GL_NATIVE_GPU_BASE_SOURCE + recipe->base_slot, 0, (float)seed_y, raster_width, (float)seed_h,
                    0u, recipe->depth_test ? 4u : 0u)) goto finished;
        } else if (!native_gpu_span(gpu, GL_NATIVE_GPU_PHASE_BASE+phase, 0, seed_y, raster_width, seed_h,
            UINT32_MAX, 0,0,0,0,recipe->clear_color,0)) goto finished;
    }
    texture.view_valid = 1;
    texture.view.kind = XG_RENDER_RESOURCE_FRAMEBUFFER;
    texture.view.byte_count = (size_t)VRAM_W * VRAM_H * sizeof(uint16_t);
    texture.view.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = VRAM_W, .height = VRAM_H, .row_pitch = VRAM_W * 2u,
        .vram_width = VRAM_W, .vram_height = VRAM_H,
        .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
        .wrap_u = XG_RENDER_RESOURCE_WRAP_REPEAT, .wrap_v = XG_RENDER_RESOURCE_WRAP_REPEAT,
        .flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION,
    };
    clut = texture;
    clut.view.kind = XG_RENDER_RESOURCE_CLUT;
    clut.view.descriptor.pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555;
    native = (GlNativeCpuSurface){.resource = &texture,
        .width = VRAM_W, .height = VRAM_H, .valid = 1};
    compiler.audit = audit;
    compiler.native_vram = &native; compiler.native_texture = &texture; compiler.native_clut = &clut;
    compiler.dithering_disabled = recipe->dithering_disabled;
    compiler.viewport_width = width; compiler.viewport_height = recipe->height;
    compiler.gpu = gpu; compiler.gpu_only = gpu != NULL;
    compiler.gpu_plane = GL_NATIVE_GPU_PHASE_BASE + phase;
    target_resource.view.descriptor.width = width;
    target_resource.view.descriptor.height = recipe->height;
    target = (GlNativeCpuSurface){.resource = &target_resource, .pixels = pixels, .depth = depth,
        .width = width, .height = recipe->height, .valid = 1};
    for (uint32_t i = 0u; i < recipe->count; ++i) {
        const GlNativeRecipeDraw *record = &recipe->draws[i];
        if (gpu && gpu->capture_templates && !gpu->static_records[i]) continue;
        if (gpu) gpu->template_record = i;
        compiler.texture_snapshot = gpu && gpu->phase_textures ? gpu->phase_textures->layers[i] : 0u;
        if (gpu && !gpu->capture_templates && recipe->wave && recipe->wave_at == i &&
            !native_wave_emit(gpu, GL_NATIVE_GPU_PHASE_BASE + phase, recipe->wave, gpu->wave_phase))
            goto finished;
        if (gpu && gpu->phase_templates) {
            const uint32_t cached = gpu->phase_templates->record_commands[i];
            if (cached != UINT32_MAX) {
                if (cached != UINT32_MAX - 1u) {
                    GlNativeGpuCommand *command = native_gpu_command(gpu, NATIVE_GPU_TEMPLATE);
                    if (!command) goto finished;
                    command->plane = GL_NATIVE_GPU_PHASE_BASE + phase;
                    command->data = cached;
                }
                continue;
            }
        }
        if (record->temporal_departure &&
            !(record->motion.motion.handle.resource_id
                ? entities && record->motion_index < recipe->motion_count && entities[record->motion_index].enabled
                : vertices && vertices->draw_mesh[i] != UINT32_MAX &&
                    vertices->meshes[vertices->draw_mesh[i]].enabled)) continue;
        GpuRenderSemantic semantic = record->semantic;
        XgSemanticDrawRecord draw;
        if (semantic.material.draw_area_top < crop_y + row_begin)
            semantic.material.draw_area_top = crop_y + row_begin;
        if (semantic.material.draw_area_bottom >= crop_y + row_end)
            semantic.material.draw_area_bottom = crop_y + row_end - 1u;
        if (semantic.material.draw_area_top > semantic.material.draw_area_bottom) continue;
        compiler.dither_x = use_view ? 0u : record->dither_x;
        compiler.dither_y = use_view ? 0u : record->dither_y;
        /* Replay textures are immutable raw words; the raster target is private
         * and has no ownership/dependency on live VIEW domain storage. */
        compiler.native_words = record->textures ? record->textures->words : NULL;
        texture.view.bytes = compiler.native_words;
        clut.view.bytes = compiler.native_words;
        if (semantic.material.textured && !record->textures) goto finished;
        if (record->mesh) {
            GlNativeGpuMeshTriangle *triangles;
            uint32_t count;
            /* Enabled across the whole interval (entities); evaluated at this
             * phase (phase_entities, when the phase has its own copy). */
            int entity = 0;
            if (!native_mesh_phase_triangles(recipe, record, i, entities, phase_entities,
                    endpoints, phase_vertices, &triangles, &count, &entity)) goto finished;
            if (!count) continue;
            if (!native_materialize_native_draw(&semantic, &draw)) goto finished;
            draw.hd_texture = record->hd_texture;
            if (!gpu) {
                /* The CPU replay rasterizes the same moved triangles one at a
                 * time, through the per-draw path. */
                draw.primitive.triangle_count = 1u;
                for (uint32_t t = 0u; t < count; ++t) {
                    XgSemanticDrawRecord single = draw;
                    single.primitive.triangles[0] = triangles[t].triangle;
                    native_texture_state_apply(&single.primitive.material, &triangles[t].texture);
                    if (use_view) {
                        const GlNativeViewRaster raster = {.width = recipe->width, .height = recipe->height,
                            .pixels = pixels, .depth = depth};
                        if (!native_view_raster(&compiler, recipe->view_width, recipe->offset, &raster,
                            &single, i, record->enhanced, record->view_origin_y)) goto finished;
                    } else if (!native_render_draw(&compiler, &target, &single, i)) goto finished;
                }
                if (entity) motion_audit->motion_projected_draws++;
                continue;
            }
            compiler.mesh_base = (const uint8_t *)&triangles[0].triangle;
            compiler.mesh_textures = (const uint8_t *)&triangles[0].texture;
            compiler.mesh_stride = sizeof(*triangles);
            compiler.mesh_count = count;
            compiler.mesh_writable = triangles;
            int drawn;
            if (use_view) {
                const GlNativeViewRaster raster = {.width = recipe->width, .height = recipe->height,
                    .pixels = pixels, .depth = depth};
                drawn = native_view_raster(&compiler, recipe->view_width, recipe->offset, &raster,
                    &draw, i, record->enhanced, recipe->origin_y);
            } else {
                draw.primitive.material.draw_offset_x += recipe->origin_x;
                draw.primitive.material.draw_offset_y += recipe->origin_y;
                draw.primitive.material.draw_area_left += recipe->origin_x;
                draw.primitive.material.draw_area_right += recipe->origin_x;
                draw.primitive.material.draw_area_top += recipe->origin_y;
                draw.primitive.material.draw_area_bottom += recipe->origin_y;
                drawn = native_gpu_draw_mesh(&compiler, &draw, 0, triangles, count);
            }
            compiler.mesh_count = 0u; compiler.mesh_writable = NULL;
            if (!drawn) goto finished;
            if (entity) motion_audit->motion_projected_draws++;
            continue;
        }
        if (entities && record->motion.motion.handle.resource_id &&
            record->motion_index < recipe->motion_count && entities[record->motion_index].enabled) {
            const GlNativeMotionProjection *projection = phase_projections ? &phase_projections[i]
                : &entities[record->motion_index].projections[i];
            const double (*screen_delta)[3][3] = projection->screen;
            const double (*native_delta)[3][3] = projection->native;
            const XgRenderMotionProjectResult projected = projection->result;
            if (projected == XG_RENDER_MOTION_CLIP_REQUIRED) {
                motion_audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_CLIP_REQUIRED;
                goto finished;
            }
            if (projected != XG_RENDER_MOTION_ENDPOINT && projected != XG_RENDER_MOTION_PROJECTED) {
                motion_audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_PROJECTION_INVALID;
                goto finished;
            }
            if (projected == XG_RENDER_MOTION_PROJECTED) {
                for (uint32_t t = 0u; t < record->motion.triangle_count; ++t)
                    for (uint32_t v = 0u; v < 3u; ++v) {
                        GpuRenderSemanticVertex *vertex = &semantic.triangles[t].vertices[v];
                        /* Shared motion deltas, not an absolute per-polygon
                         * residual. Preserve each plane's actual relocation. */
                        const double x = vertex->x + screen_delta[t][v][0] * 65536.0;
                        const double y = vertex->y + screen_delta[t][v][1] * 65536.0;
                        const double native_x = vertex->native_view_x + native_delta[t][v][0] * 65536.0;
                        const double native_y = vertex->native_view_y + native_delta[t][v][1] * 65536.0;
                        /* Preflight already checked these exact positions in
                         * every phase before enabling the whole entity. */
                        vertex->x = (int32_t)llround(x); vertex->y = (int32_t)llround(y);
                        if (vertex->native_view_position) {
                            vertex->native_view_x = (int32_t)llround(native_x);
                            vertex->native_view_y = (int32_t)llround(native_y);
                        }
                        /* Perspective sampling must not flip to affine on
                         * alternate presented frames: an interpolated phase is
                         * shown next to its endpoint. screen_delta[2] is this
                         * phase's own continuous view depth, in the same units
                         * as the GTE mac[2] the endpoints carry, so adopt it
                         * with the same validity window (0, 0xffff]. Outside
                         * that window the endpoint would also have cleared the
                         * flag, so the triangle stays affine. */
                        if (vertex->projective_position) {
                            const double depth = screen_delta[t][v][2];
                            if (depth > 0.0 && depth <= 65535.0)
                                vertex->projective_view_z = (int32_t)floor(depth);
                            else
                                vertex->projective_position = 0u;
                        }
                        /* Depth test key: the endpoint's own unfloored depth plus
                         * the pose's Native view-Z displacement, like the Native
                         * positions (a static pose leaves it bit-identical).
                         * Without an endpoint depth, fall back to phase Z. */
                        if (vertex->native_view_depth > 0) {
                            const double depth = vertex->native_view_depth + native_delta[t][v][2] * 4096.0;
                            vertex->native_view_depth = isfinite(depth) && depth >= 1.0 && depth <= 65535.0 * 4096.0
                                ? (int32_t)llround(depth) : native_depth_q12(screen_delta[t][v][2],
                                    vertex->projective_distance);
                        } else if (vertex->projective_position)
                            vertex->native_view_depth = native_depth_q12(screen_delta[t][v][2],
                                vertex->projective_distance);
                        vertex->temporal_depth_valid = 0u;
                    }
                motion_audit->motion_projected_draws++;
            }
        } else if (vertices && !record->motion.motion.handle.resource_id &&
                   vertices->draw_mesh[i] != UINT32_MAX &&
                   vertices->meshes[vertices->draw_mesh[i]].enabled) {
            for (uint32_t t = 0u; t < semantic.triangle_count; ++t)
                for (uint32_t v = 0u; v < 3u; ++v) {
                    GpuRenderSemanticVertex *vertex = &semantic.triangles[t].vertices[v];
                    const uint32_t key = vertices->map[i][t * 3u + v];
                    if (key >= vertices->pair_count) goto finished;
                    const int64_t *delta = vertices->deltas[(size_t)phase * vertices->pair_count + key];
                    vertex->x = (int32_t)((int64_t)vertex->x + delta[0]);
                    vertex->y = (int32_t)((int64_t)vertex->y + delta[1]);
                    if (vertex->native_view_position) {
                        vertex->native_view_x = (int32_t)((int64_t)vertex->native_view_x + delta[2]);
                        vertex->native_view_y = (int32_t)((int64_t)vertex->native_view_y + delta[3]);
                    }
                    if (vertex->native_view_depth > 0) {
                        const int64_t depth = (int64_t)vertex->native_view_depth + delta[4];
                        vertex->native_view_depth = depth > 0 && depth <= INT32_MAX ? (int32_t)depth : 0;
                    }
                    /* The projected payload has no phase-local depth here, so
                     * the endpoint depth stays; hiding it would make this
                     * phase sample affine while its endpoint sampled
                     * perspective. Phases stop being temporal samples. */
                    vertex->temporal_depth_valid = 0u;
                }
            motion_audit->motion_projected_draws++;
        }
        if (gpu && s_native_phase_stats_enabled) {
            uint32_t moved = 0u, p = 0u;
            for (uint32_t t = 0u; t < semantic.triangle_count; ++t)
                for (uint32_t v = 0u; v < 3u; ++v) {
                    const GpuRenderSemanticVertex *a = &record->semantic.triangles[t].vertices[v];
                    const GpuRenderSemanticVertex *b = &semantic.triangles[t].vertices[v];
                    const int native_position = use_view && record->enhanced && a->native_view_position;
                    moved += native_position ? a->native_view_x != b->native_view_x || a->native_view_y != b->native_view_y
                                             : a->x != b->x || a->y != b->y;
                }
            while (p < producer_count && producers[p].producer != semantic.interpolation_identity.producer_id) ++p;
            if (p < 64u) {
                if (p == producer_count) {
                    producers[p].identity = gpu->identity; producers[p].phase = phase + 1u;
                    producers[p].producer = semantic.interpolation_identity.producer_id; ++producer_count;
                }
                producers[p].draws++; producers[p].moved_draws += moved != 0u;
                producers[p].moved_vertices += moved;
            }
        }
        if (!native_materialize_native_draw(&semantic, &draw)) goto finished;
        draw.hd_texture = record->hd_texture;
        if (use_view) {
            const GlNativeViewRaster raster = {.width = recipe->width, .height = recipe->height,
                .pixels = pixels, .depth = depth};
            if (!native_view_raster(&compiler, recipe->view_width, recipe->offset, &raster,
                &draw, i, record->enhanced, gpu ? recipe->origin_y : record->view_origin_y)) goto finished;
        } else {
            if (gpu) {
                draw.primitive.material.draw_offset_x += recipe->origin_x;
                draw.primitive.material.draw_offset_y += recipe->origin_y;
                draw.primitive.material.draw_area_left += recipe->origin_x;
                draw.primitive.material.draw_area_right += recipe->origin_x;
                draw.primitive.material.draw_area_top += recipe->origin_y;
                draw.primitive.material.draw_area_bottom += recipe->origin_y;
            }
            if (!(gpu ? native_gpu_draw(&compiler, &draw, 0)
                      : native_render_draw(&compiler, &target, &draw, i))) goto finished;
        }
    }
    if (gpu && !gpu->capture_templates && recipe->wave && recipe->wave_at == recipe->count &&
        !native_wave_emit(gpu, GL_NATIVE_GPU_PHASE_BASE + phase, recipe->wave, gpu->wave_phase))
        goto finished;
    if (!gpu) {
        memmove(pixels, pixels + (size_t)crop_y * width, (size_t)width * height * sizeof(*pixels));
        for (size_t i = 0u; i < (size_t)width * height; ++i) pixels[i] |= UINT32_C(0xff000000);
    }
    *out_pixels = pixels; pixels = NULL; ok = 1;
finished:
    if (ok && gpu && s_native_phase_stats_enabled) {
        SDL_LockMutex(s_native_state_mutex);
        for (uint32_t i = 0u; i < producer_count && s_native_phase_producer_count < 65536u; ++i)
            s_native_phase_producers[s_native_phase_producer_count++] = producers[i];
        SDL_UnlockMutex(s_native_state_mutex);
    }
    free(pixels); free(depth); free(audit);
    return ok;
}

static int native_recipe_stripe_thread(void *data) {
    GlNativeRecipeStripe *job = data;
    job->ok = native_recipe_render_rows(job->recipe, job->entities, job->vertices, job->phase,
        job->use_view, &job->stats, job->crop_y, job->height, &job->pixels, job->begin, job->end, NULL, NULL, NULL, NULL, NULL);
    return job->ok;
}

static int native_recipe_render(const GlNativeRecipe *recipe, const GlNativeMotionEntity *entities,
                             const GlNativeVertexCache *vertices, uint32_t phase, int use_view,
                             GlNativeCompileAudit *motion_audit,
                             uint16_t crop_y, uint16_t height, uint32_t **out_pixels) {
    uint32_t count = psx_host_worker_count((unsigned)SDL_GetCPUCount(), height);
    const uint32_t width = use_view ? recipe->view_width : recipe->width;
    GlNativeRecipeStripe serial = {0};
    GlNativeRecipeStripe *jobs = count > 1u ? calloc(count, sizeof(*jobs)) : &serial;
    SDL_Thread **threads = count > 1u ? calloc(count, sizeof(*threads)) : NULL;
    *out_pixels = NULL;
    if (!count) return 0;
    if (!jobs || (count > 1u && !threads)) {
        free(jobs); free(threads);
        jobs = &serial; threads = NULL; count = 1u;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        jobs[i] = (GlNativeRecipeStripe){.recipe = recipe, .entities = entities, .vertices = vertices,
            .phase = phase, .use_view = use_view, .crop_y = crop_y, .height = height,
            .begin = (uint16_t)(height * i / count), .end = (uint16_t)(height * (i + 1u) / count)};
        if (i) {
            threads[i] = SDL_CreateThread(native_recipe_stripe_thread, "native-rows", &jobs[i]);
            if (!threads[i]) (void)native_recipe_stripe_thread(&jobs[i]);
        }
    }
    (void)native_recipe_stripe_thread(&jobs[0]);
    int ok = 1;
    for (uint32_t i = 0u; i < count; ++i) {
        if (threads && threads[i]) SDL_WaitThread(threads[i], NULL);
        ok &= jobs[i].ok;
        if (motion_audit) {
            motion_audit->motion_projected_draws += jobs[i].stats.motion_projected_draws;
            if (!jobs[i].ok && jobs[i].stats.temporal_status)
                motion_audit->temporal_status = jobs[i].stats.temporal_status;
        }
    }
    if (ok) {
        /* Disjoint rows, identical draw order and absolute dither coordinates.
         * This is parallel rasterization, not temporal image interpolation. */
        *out_pixels = jobs[0].pixels;
        for (uint32_t i = 1u; i < count; ++i)
            memcpy(*out_pixels + (size_t)jobs[i].begin * width,
                jobs[i].pixels + (size_t)jobs[i].begin * width,
                (size_t)(jobs[i].end - jobs[i].begin) * width * sizeof(uint32_t));
    } else free(jobs[0].pixels);
    for (uint32_t i = 1u; i < count; ++i) free(jobs[i].pixels);
    if (jobs != &serial) free(jobs);
    free(threads);
    return ok;
}

static int native_recipe_replay_thread(void *data) {
    GlNativeRecipeReplay *job = data;
    /* Endpoint replay has no motion-audit writes. Compiler, VIEW, framebuffer
     * and raster diagnostics are private; recipe/texel snapshots are read-only. */
    job->ok = native_recipe_render(job->recipe, NULL, NULL, 0u, job->use_view, NULL,
        job->crop_y, job->height, &job->pixels);
    return job->ok;
}

static int native_motion_camera_equal(const XgRenderMotionPose *a, const XgRenderMotionPose *b) {
    /* Origin subtraction/wrapping is an ENTITY pre-view stage, not a second
     * camera. Terrain and models may express it in different local spaces. */
    return a->camera_id == b->camera_id && !memcmp(&a->camera, &b->camera, sizeof(a->camera)) &&
        !memcmp(a->screen_offset, b->screen_offset, sizeof(a->screen_offset)) &&
        a->projection_distance == b->projection_distance;
}

static int native_motion_reject(const char *reason, const XgRenderMotionPose *pose,
                             uint32_t draw_index, uint32_t producer_id) {
    if (s_native_state_mutex) SDL_LockMutex(s_native_state_mutex);
    s_native_motion_reject_events[s_native_motion_reject_total++ % GL_RENDERER_NATIVE_MOTION_REJECT_CAPACITY] =
        (GlNativeMotionRejectEvent){
        .reason = reason, .entity_id = pose ? pose->entity_id : 0u,
        .source_update = pose ? pose->source_update : 0u,
        .draw_index = draw_index, .producer_id = producer_id};
    if (s_native_state_mutex) SDL_UnlockMutex(s_native_state_mutex);
    return 0;
}

uint32_t gl_renderer_native_motion_rejects(GlRendererNativeMotionRejectEvent *out,
                                           uint32_t capacity, uint64_t *out_total) {
    uint32_t count = 0u;
    if (s_native_state_mutex) SDL_LockMutex(s_native_state_mutex);
    const uint64_t total = s_native_motion_reject_total;
    const uint64_t kept = total < GL_RENDERER_NATIVE_MOTION_REJECT_CAPACITY
        ? total : GL_RENDERER_NATIVE_MOTION_REJECT_CAPACITY;
    for (uint64_t sequence = total - kept; out && sequence < total && count < capacity; ++sequence) {
        const volatile GlNativeMotionRejectEvent *event =
            &s_native_motion_reject_events[sequence % GL_RENDERER_NATIVE_MOTION_REJECT_CAPACITY];
        out[count++] = (GlRendererNativeMotionRejectEvent){sequence, event->reason, event->entity_id,
            event->source_update, event->draw_index, event->producer_id};
    }
    if (s_native_state_mutex) SDL_UnlockMutex(s_native_state_mutex);
    if (out_total) *out_total = total;
    return count;
}

static int native_motion_mesh_compare(const void *left, const void *right) {
    const GlNativeVertexMesh *a = left, *b = right;
    if (a->scene != b->scene) return a->scene < b->scene ? -1 : 1;
    if (a->producer != b->producer) return a->producer < b->producer ? -1 : 1;
    if (a->scope != b->scope) return a->scope < b->scope ? -1 : 1;
    if (a->component != b->component) return a->component < b->component ? -1 : 1;
    return a->draw < b->draw ? -1 : a->draw != b->draw;
}

static unsigned native_motion_vertex_digit(const GlNativeVertexUse *use, unsigned pass) {
    if (pass < 4u) return (use->vertex >> (pass * 8u)) & 255u;
    if (pass < 8u) return (use->group >> ((pass - 4u) * 8u)) & 255u;
    if (pass < 16u) return (unsigned)(use->component >> ((pass - 8u) * 8u)) & 255u;
    if (pass < 20u) return (use->scope >> ((pass - 16u) * 8u)) & 255u;
    return (unsigned)(use->scene >> ((pass - 20u) * 8u)) & 255u;
}

static int native_motion_vertex_order(const GlNativeVertexUse *uses, uint32_t count,
                                     const GlNativeVertexUse ***out) {
    const GlNativeVertexUse **ordered = malloc((count ? (size_t)count : 1u) * sizeof(*ordered));
    const GlNativeVertexUse **scratch = malloc((count ? (size_t)count : 1u) * sizeof(*scratch));
    if (!ordered || !scratch) { free(ordered); free(scratch); return 0; }
    uint64_t varying[5] = {0};
    for (uint32_t i = 0u; i < count; ++i) {
        ordered[i] = &uses[i];
        varying[0] |= uses[i].vertex ^ uses[0].vertex;
        varying[1] |= uses[i].group ^ uses[0].group;
        varying[2] |= uses[i].component ^ uses[0].component;
        varying[3] |= uses[i].scope ^ uses[0].scope;
        varying[4] |= uses[i].scene ^ uses[0].scene;
    }
    /* Stable byte passes preserve the original frame/draw/corner order, which
     * is already ascending during collection. This gives the same complete
     * ordering as comparison sort without revisiting the large sample payload.
     * Most scene/component bytes are constant and need no scatter. */
    for (unsigned pass = 0u; pass < 28u && count > 1u; ++pass) {
        const unsigned field = pass < 4u ? 0u : pass < 8u ? 1u : pass < 16u ? 2u : pass < 20u ? 3u : 4u;
        const unsigned byte = pass < 4u ? pass : pass < 8u ? pass-4u :
            pass < 16u ? pass-8u : pass < 20u ? pass-16u : pass-20u;
        if (!((varying[field] >> (byte * 8u)) & 255u)) continue;
        uint32_t offsets[256] = {0};
        for (uint32_t i = 0u; i < count; ++i)
            ++offsets[native_motion_vertex_digit(ordered[i], pass)];
        uint32_t prefix = 0u;
        for (unsigned digit = 0u; digit < 256u; ++digit) {
            const uint32_t length = offsets[digit];
            offsets[digit] = prefix; prefix += length;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            const GlNativeVertexUse *use = ordered[i];
            scratch[offsets[native_motion_vertex_digit(use, pass)]++] = use;
        }
        const GlNativeVertexUse **swap = ordered; ordered = scratch; scratch = swap;
    }
    free(scratch); *out = ordered; return 1;
}

static uint32_t native_motion_mesh_root(GlNativeVertexCache *cache, uint32_t mesh) {
    while (cache->meshes[mesh].parent != mesh) {
        cache->meshes[mesh].parent = cache->meshes[cache->meshes[mesh].parent].parent;
        mesh = cache->meshes[mesh].parent;
    }
    return mesh;
}

static void native_motion_vertices_discard(GlNativeVertexCache *cache) {
    free(cache->pairs); free(cache->meshes); free(cache->map);
    free(cache->draw_mesh); free(cache->deltas);
    memset(cache, 0, sizeof(*cache));
}

static uint32_t native_recipe_record_triangles(const GlNativeRecipeDraw *draw) {
    return draw->mesh ? draw->mesh->count : draw->semantic.triangle_count;
}

static const GpuRenderSemanticVertex *native_semantic_vertex(const XgRenderIrVertex *source,
                                                             GpuRenderSemanticVertex *target) {
    memset(target, 0, sizeof(*target));
#define NATIVE_COPY_VERTEX(field) target->field = source->field
    NATIVE_COPY_VERTEX(x); NATIVE_COPY_VERTEX(y); NATIVE_COPY_VERTEX(u); NATIVE_COPY_VERTEX(v);
    NATIVE_COPY_VERTEX(r); NATIVE_COPY_VERTEX(g); NATIVE_COPY_VERTEX(b);
    NATIVE_COPY_VERTEX(native_view_x); NATIVE_COPY_VERTEX(native_view_y);
    NATIVE_COPY_VERTEX(native_view_position); NATIVE_COPY_VERTEX(native_view_depth);
    NATIVE_COPY_VERTEX(projective_view_x); NATIVE_COPY_VERTEX(projective_view_y);
    NATIVE_COPY_VERTEX(projective_view_z);
    NATIVE_COPY_VERTEX(projective_offset_x); NATIVE_COPY_VERTEX(projective_offset_y);
    NATIVE_COPY_VERTEX(projective_native_offset_x); NATIVE_COPY_VERTEX(projective_native_offset_y);
    NATIVE_COPY_VERTEX(projective_distance); NATIVE_COPY_VERTEX(projective_position);
    NATIVE_COPY_VERTEX(temporal_depth); NATIVE_COPY_VERTEX(temporal_depth_valid);
    NATIVE_COPY_VERTEX(interpolation_group_id); NATIVE_COPY_VERTEX(interpolation_vertex_id);
    NATIVE_COPY_VERTEX(interpolation_vertex_identity_valid);
#undef NATIVE_COPY_VERTEX
    return target;
}

static void native_motion_vertex_sample(GlNativeVertexSample *sample, const GpuRenderSemanticVertex *p,
                                      int32_t dx, int32_t dy, uint16_t offset) {
    memset(sample, 0, sizeof(*sample));
    sample->position[0] = (int64_t)p->x - dx; sample->position[1] = (int64_t)p->y - dy;
    sample->native = p->native_view_position != 0u;
    sample->projective = p->projective_position != 0u;
    sample->depth = p->native_view_depth > 0 ? p->native_view_depth : 0;
    if (sample->native) {
        sample->position[2] = (int64_t)p->native_view_x - dx - (int64_t)offset * 65536;
        sample->position[3] = (int64_t)p->native_view_y - dy;
    }
    if (sample->projective) {
        sample->view[0] = p->projective_view_x; sample->view[1] = p->projective_view_y;
        sample->view[2] = p->projective_view_z;
        sample->offset[0] = (int64_t)p->projective_offset_x - dx;
        sample->offset[1] = (int64_t)p->projective_offset_y - dy;
        sample->distance = p->projective_distance;
    }
}

static const GpuRenderSemanticVertex *native_coverage_vertex(const XgRenderTemporalCoverageView *view,
                                                         uint64_t component, uint32_t group, uint32_t vertex) {
    uint32_t lo = 0u, hi = view->header->sample_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const XgRenderTemporalSample *s = &view->samples[mid];
        const int before = s->component_id < component || (s->component_id == component &&
            (s->vertex.interpolation_group_id < group || (s->vertex.interpolation_group_id == group &&
             s->vertex.interpolation_vertex_id < vertex)));
        if (before) lo = mid + 1u; else hi = mid;
    }
    if (lo == view->header->sample_count) return NULL;
    const XgRenderTemporalSample *s = &view->samples[lo];
    return s->component_id == component && s->vertex.interpolation_group_id == group &&
        s->vertex.interpolation_vertex_id == vertex ? &s->vertex : NULL;
}

static const GlNativeRecipeCoverage *native_coverage_recipe_scope(
        const GlNativeRecipe *recipe, uint32_t scope) {
    const GlNativeRecipeCoverage *found = NULL;
    for (uint32_t i = 0u; i < recipe->coverage_count; ++i) {
        const GlNativeRecipeCoverage *candidate = &recipe->coverages[i];
        if (candidate->view.header->producer_scope != scope) continue;
        /* A mixed-update target has no unique source endpoint. */
        if (found) return NULL;
        found = candidate;
    }
    if (found) return found;
    /* A wholly culled producer can still supply its authenticated source. */
    for (uint32_t i = 0u; recipe->publication && i < recipe->publication->count; ++i) {
        const GlNativeRecipeCoverage *candidate = &recipe->publication->scopes[i].current;
        if (candidate->view.header->producer_scope == scope) return candidate;
    }
    return NULL;
}

static const XgRenderTemporalCoverageView *native_coverage_previous(const GlNativeRecipe *previous,
                                                                 const GlNativeRecipe *current, const GlNativeRecipeDraw *draw) {
    if (draw->temporal_index >= current->coverage_count) return NULL;
    const GlNativeRecipeCoverage *binding = &current->coverages[draw->temporal_index];
    const uint32_t scope = binding->view.header->producer_scope;
    const GlNativeRecipeCoverage *a = native_coverage_recipe_scope(previous, scope);
    const GlNativeRecipeCoverage *b = native_coverage_recipe_scope(current, scope);
    if (!a || b != binding) return NULL;
    const int same = !memcmp(&a->reference, &b->reference, sizeof(binding->reference));
    if (!same && memcmp(&a->reference, &b->predecessor, sizeof(binding->reference))) return NULL;
    const XgRenderTemporalComponent *ac = native_coverage_component(&a->view, draw->temporal_component);
    const XgRenderTemporalComponent *bc = native_coverage_component(&b->view, draw->temporal_component);
    if (!ac || !bc || (!same && !xg_render_temporal_components_compatible(a->view.header, ac, b->view.header, bc))) return NULL;
    return &a->view;
}

#include "gpu_gl_native_departures.h"

static int native_motion_texture_footprint_equal(
        const GpuRenderSemantic *a, const GpuRenderSemantic *b) {
    const GpuRenderMaterial *am = &a->material, *bm = &b->material;
    if (a->triangle_count != b->triangle_count ||
        am->textured != bm->textured || am->tpage != bm->tpage ||
        am->clut_x != bm->clut_x || am->clut_y != bm->clut_y ||
        am->texture_window_mask_x != bm->texture_window_mask_x ||
        am->texture_window_mask_y != bm->texture_window_mask_y ||
        am->texture_window_offset_x != bm->texture_window_offset_x ||
        am->texture_window_offset_y != bm->texture_window_offset_y)
        return 0;
    for (uint32_t t = 0u; t < a->triangle_count; ++t)
        for (uint32_t v = 0u; v < 3u; ++v)
            if (a->triangles[t].vertices[v].u != b->triangles[t].vertices[v].u ||
                a->triangles[t].vertices[v].v != b->triangles[t].vertices[v].v)
                return 0;
    return 1;
}

typedef struct GlNativeMotionDrawKey {
    GpuRenderInterpolationIdentity identity;
    uint64_t component;
    uint32_t scope, index;
} GlNativeMotionDrawKey;

static GlNativeMotionDrawKey native_motion_draw_key(const GlNativeRecipe *recipe, uint32_t index) {
    const GlNativeRecipeDraw *draw = &recipe->draws[index];
    return (GlNativeMotionDrawKey){.identity = draw->semantic.interpolation_identity,
        .component = draw->temporal_component, .index = index,
        .scope = draw->temporal_component
            ? recipe->coverages[draw->temporal_index].view.header->producer_scope : 0u};
}

static int native_motion_draw_key_compare(const void *left, const void *right) {
    const GlNativeMotionDrawKey *a = left, *b = right;
#define COMPARE_DRAW_KEY(field) if (a->field != b->field) return a->field < b->field ? -1 : 1
    COMPARE_DRAW_KEY(identity.scene_id);
    COMPARE_DRAW_KEY(identity.producer_id);
    COMPARE_DRAW_KEY(identity.primitive_id);
    COMPARE_DRAW_KEY(scope);
    COMPARE_DRAW_KEY(component);
#undef COMPARE_DRAW_KEY
    return 0;
}

/* Texture cels belong to primitives, not shared vertex aliases. A vertex at
 * a UV seam can occur in several faces with different texture footprints.
 * Index the previous draws once, only for the nonprojective fallback which
 * needs this check; rigid/projective geometry pays no sorting/allocation cost. */
static int native_motion_texture_keys(const GlNativeRecipe *recipe,
                                     GlNativeMotionDrawKey **out, uint32_t *count) {
    GlNativeMotionDrawKey *keys = malloc((recipe->count ? recipe->count : 1u) * sizeof(*keys));
    if (!keys) return 0;
    *count = 0u;
    for (uint32_t i = 0u; i < recipe->count; ++i) {
        const GlNativeRecipeDraw *draw = &recipe->draws[i];
        const GpuRenderSemantic *s = &draw->semantic;
        if (!draw->motion.motion.handle.resource_id && s->interpolation_identity.valid &&
            s->topology == GPU_RENDER_SEMANTIC_TRIANGLES && !s->screen_space_2d && !s->native_view_effect)
            keys[(*count)++] = native_motion_draw_key(recipe, i);
    }
    qsort(keys, *count, sizeof(*keys), native_motion_draw_key_compare);
    *out = keys;
    return 1;
}

static uint32_t native_motion_texture_previous(const GlNativeRecipe *current, uint32_t draw,
        const GlNativeMotionDrawKey *keys, uint32_t count) {
    const GlNativeMotionDrawKey key = native_motion_draw_key(current, draw);
    if (!key.identity.valid || !count) return UINT32_MAX;
    const GlNativeMotionDrawKey *match = bsearch(&key, keys, count, sizeof(*keys), native_motion_draw_key_compare);
    if (!match || (match != keys && !native_motion_draw_key_compare(match - 1, match)) ||
        (match != keys + count - 1u && !native_motion_draw_key_compare(match + 1, match))) return UINT32_MAX;
    return match->index;
}

static int native_motion_group_compare(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* The vertex mesh of an unscoped producer (meshes[0, mesh_count) are the
 * sorted unique keys), or UINT32_MAX. Not resolved to its root. */
static uint32_t native_motion_mesh_find(const GlNativeVertexCache *cache, uint64_t scene, uint32_t producer) {
    uint32_t lo = 0u, hi = cache->mesh_count;
    const GlNativeVertexMesh key = {.scene = scene, .producer = producer};
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        GlNativeVertexMesh probe = cache->meshes[mid];
        probe.draw = 0u;
        if (native_motion_mesh_compare(&probe, &key) < 0) lo = mid + 1u; else hi = mid;
    }
    if (lo == cache->mesh_count) return UINT32_MAX;
    const GlNativeVertexMesh *found = &cache->meshes[lo];
    return found->scene == scene && found->producer == producer && !found->scope && !found->component
        ? lo : UINT32_MAX;
}

static int native_motion_vertices(const GlNativeRecipe *previous, const GlNativeRecipe *current,
                                uint32_t phases, GlNativeVertexCache *cache, GlNativeVertexDiagnostics *diag) {
    size_t capacity = 0u;
    for (unsigned frame = 0u; frame < 2u; ++frame) {
        const GlNativeRecipe *recipe = frame ? current : previous;
        for (uint32_t i = 0u; i < recipe->count; ++i)
            capacity += native_recipe_record_triangles(&recipe->draws[i]) * 3u;
    }
    /* A coalesced run that crosses objects owns one vertex mesh per pose
     * owner, besides its record's. */
    size_t mesh_capacity = current->count;
    for (uint32_t i = 0u; i < current->count; ++i) {
        const GlNativeRecipeMesh *mesh = current->draws[i].mesh;
        for (uint32_t t = 0u; mesh && mesh->poses && t < mesh->count; ++t)
            mesh_capacity += !t || !native_recipe_pose_equal(&mesh->poses[t], &mesh->poses[t - 1u]);
    }
    GlNativeVertexUse *uses = malloc((capacity ? capacity : 1u) * sizeof(*uses));
    cache->pairs = malloc((capacity ? capacity : 1u) * sizeof(*cache->pairs));
    cache->meshes = calloc(mesh_capacity ? mesh_capacity : 1u, sizeof(*cache->meshes));
    cache->map = malloc((current->count ? current->count : 1u) * sizeof(*cache->map));
    cache->draw_mesh = malloc((current->count ? current->count : 1u) * sizeof(*cache->draw_mesh));
    if (!uses || !cache->pairs || !cache->meshes || !cache->map || !cache->draw_mesh) {
        free(uses); native_motion_vertices_discard(cache); return 0;
    }
    memset(cache->map, 0xff, current->count * sizeof(*cache->map));
    memset(cache->draw_mesh, 0xff, current->count * sizeof(*cache->draw_mesh));
    uint32_t mesh_uses = 0u, use_count = 0u;
    for (uint32_t i = 0u; i < current->count; ++i) {
        const GpuRenderSemantic *s = &current->draws[i].semantic;
        if (s->topology != GPU_RENDER_SEMANTIC_TRIANGLES || s->screen_space_2d || s->native_view_effect ||
            !s->interpolation_identity.scene_id ||
            !s->interpolation_identity.producer_id) continue;
        const GlNativeRecipeDraw *draw = &current->draws[i];
        cache->meshes[mesh_uses++] = (GlNativeVertexMesh){.scene = s->interpolation_identity.scene_id,
            .producer = s->interpolation_identity.producer_id, .draw = i, .component = draw->temporal_component,
            .scope = draw->temporal_component ? current->coverages[draw->temporal_index].view.header->producer_scope : 0u};
        const GlNativeRecipeMesh *mesh = draw->mesh;
        for (uint32_t t = 0u; mesh && mesh->poses && t < mesh->count; ++t) {
            const GlNativeRecipeMeshPose *pose = &mesh->poses[t];
            if ((t && native_recipe_pose_equal(pose, &mesh->poses[t - 1u])) ||
                !pose->scene_id || !pose->producer_id) continue;
            cache->meshes[mesh_uses++] = (GlNativeVertexMesh){.scene = pose->scene_id,
                .producer = pose->producer_id, .draw = UINT32_MAX};
        }
    }
    qsort(cache->meshes, mesh_uses, sizeof(*cache->meshes), native_motion_mesh_compare);
    for (uint32_t i = 0u; i < mesh_uses; ++i) {
        const GlNativeVertexMesh use = cache->meshes[i];
        if (!cache->mesh_count || use.scene != cache->meshes[cache->mesh_count - 1u].scene ||
            use.producer != cache->meshes[cache->mesh_count - 1u].producer ||
            use.scope != cache->meshes[cache->mesh_count - 1u].scope ||
            use.component != cache->meshes[cache->mesh_count - 1u].component) {
            cache->meshes[cache->mesh_count] = use;
            cache->meshes[cache->mesh_count].parent = cache->mesh_count;
            cache->mesh_count++;
        }
        if (use.draw != UINT32_MAX) cache->draw_mesh[use.draw] = cache->mesh_count - 1u;
    }
    /* Pairs only move unbound meshes; a pose-bound draw's vertices matter
     * solely where they alias an unbound draw's (same group), joining that
     * mesh and holding it discrete. Vertices of bound draws in groups no
     * unbound draw uses can join nothing unbound: leave them out. */
    uint32_t *unbound_groups = NULL, unbound_group_count = 0u;
    for (unsigned frame = 0u; frame < 2u; ++frame) {
        const GlNativeRecipe *recipe = frame ? current : previous;
        for (uint32_t i = 0u; i < recipe->count; ++i) {
            const GlNativeRecipeDraw *draw = &recipe->draws[i];
            const GpuRenderSemantic *s = &draw->semantic;
            if (draw->motion.motion.handle.resource_id ||
                s->topology != GPU_RENDER_SEMANTIC_TRIANGLES || s->screen_space_2d ||
                s->native_view_effect || !s->interpolation_identity.scene_id) continue;
            for (uint32_t t = 0u; t < s->triangle_count; ++t)
                for (uint32_t v = 0u; v < 3u; ++v) {
                    const GpuRenderSemanticVertex *p = &s->triangles[t].vertices[v];
                    if (!p->interpolation_vertex_identity_valid || !p->interpolation_group_id) continue;
                    if (unbound_group_count % 1024u == 0u) {
                        uint32_t *grown = realloc(unbound_groups,
                            (unbound_group_count + 1024u) * sizeof(*grown));
                        if (!grown) {
                            free(unbound_groups); free(uses);
                            native_motion_vertices_discard(cache); return 0;
                        }
                        unbound_groups = grown;
                    }
                    unbound_groups[unbound_group_count++] = p->interpolation_group_id;
                }
        }
    }
    if (unbound_group_count)
        qsort(unbound_groups, unbound_group_count, sizeof(*unbound_groups),
              native_motion_group_compare);
    for (unsigned frame = 0u; frame < 2u; ++frame) {
        const GlNativeRecipe *recipe = frame ? current : previous;
        for (uint32_t i = 0u; i < recipe->count; ++i) {
            const GlNativeRecipeDraw *draw = &recipe->draws[i];
            const GpuRenderSemantic *s = &draw->semantic;
            if (s->topology != GPU_RENDER_SEMANTIC_TRIANGLES || s->screen_space_2d || s->native_view_effect ||
                !s->interpolation_identity.scene_id) continue;
            const int bound = draw->motion.motion.handle.resource_id != 0u;
            if (bound && !unbound_group_count) continue;
            /* A coalesced run's polygons live in its mesh, already relocated
             * like a DRAW's; a MESH operation's expansion is not. */
            const int32_t dx = !draw->mesh || draw->mesh->relocated ? draw->projection_offset_x : 0;
            const int32_t dy = !draw->mesh || draw->mesh->relocated ? draw->projection_offset_y : 0;
            const uint32_t triangles = native_recipe_record_triangles(draw);
            const GlNativeRecipeMeshPose *poses = draw->mesh ? draw->mesh->poses : NULL;
            for (uint32_t t = 0u; t < triangles; ++t)
                for (uint32_t v = 0u; v < 3u; ++v) {
                    GpuRenderSemanticVertex mesh_vertex;
                    const GpuRenderSemanticVertex *p = draw->mesh
                        ? native_semantic_vertex(&draw->mesh->triangles[t].triangle.vertices[v], &mesh_vertex)
                        : &s->triangles[t].vertices[v];
                    /* A triangle of a run crossing objects joins its own
                     * pose owner's mesh. */
                    const uint64_t scene = poses ? poses[t].scene_id : s->interpolation_identity.scene_id;
                    const uint32_t owner_mesh = !frame ? UINT32_MAX : poses
                        ? native_motion_mesh_find(cache, poses[t].scene_id, poses[t].producer_id)
                        : cache->draw_mesh[i];
                    if (!p->interpolation_vertex_identity_valid || !p->interpolation_group_id || !scene ||
                        (frame && owner_mesh == UINT32_MAX)) continue;
                    if (bound && (!unbound_group_count ||
                        !bsearch(&p->interpolation_group_id, unbound_groups, unbound_group_count,
                                 sizeof(*unbound_groups), native_motion_group_compare))) continue;
                    GlNativeVertexUse *use = &uses[use_count++];
                    use->scene = scene;
                    use->component = draw->temporal_component;
                    use->scope = draw->temporal_component ? recipe->coverages[draw->temporal_index].view.header->producer_scope : 0u;
                    use->group = p->interpolation_group_id; use->vertex = p->interpolation_vertex_id;
                    use->frame = (uint8_t)frame; use->draw = i; use->corner = t * 3u + v;
                    use->mesh = owner_mesh;
                    /* Strip destination relocation only, never recover 3D from
                     * screen XY. Canonical and Native anchors remain distinct. */
                    native_motion_vertex_sample(&use->sample, p, dx, dy, recipe->offset);
                }
        }
    }
    free(unbound_groups);
    const GlNativeVertexUse **ordered = NULL;
    if (!native_motion_vertex_order(uses, use_count, &ordered)) {
        free(uses); native_motion_vertices_discard(cache); return 0;
    }
    for (uint32_t first = 0u; first < use_count;) {
        uint32_t end = first + 1u;
        while (end < use_count && ordered[end]->scene == ordered[first]->scene &&
            ordered[end]->scope == ordered[first]->scope && ordered[end]->component == ordered[first]->component &&
            ordered[end]->group == ordered[first]->group && ordered[end]->vertex == ordered[first]->vertex) ++end;
        if (!ordered[end - 1u]->frame) { first = end; continue; }
        GlNativeVertexPair *pair = &cache->pairs[cache->pair_count];
        memset(pair, 0, sizeof(*pair));
        uint32_t seen = 0u, mesh = UINT32_MAX;
        int conflict = 0;
        for (uint32_t j = first; j < end; ++j) {
            const GlNativeVertexUse *use = ordered[j];
            if (seen & (1u << use->frame))
                conflict |= memcmp(&pair->endpoints[use->frame], &use->sample, sizeof(use->sample)) != 0;
            else pair->endpoints[use->frame] = use->sample;
            seen |= 1u << use->frame;
            if (!use->frame) continue;
            /* Only unbound draws (at most two triangles) read the map. */
            if (use->corner < 6u) cache->map[use->draw][use->corner] = cache->pair_count;
            const uint32_t root = native_motion_mesh_root(cache, use->mesh);
            if (mesh == UINT32_MAX) mesh = root;
            else cache->meshes[root].parent = mesh = native_motion_mesh_root(cache, mesh);
        }
        pair->mesh = mesh;
        if (ordered[first]->component) {
            const GlNativeVertexUse *key = ordered[end - 1u];
            const GlNativeRecipeDraw *draw = &current->draws[key->draw];
            const XgRenderTemporalCoverageView *now = &current->coverages[draw->temporal_index].view;
            const XgRenderTemporalCoverageView *before = native_coverage_previous(previous, current, draw);
            const GpuRenderSemanticVertex *pa = before ? native_coverage_vertex(before, key->component, key->group, key->vertex) : NULL;
            const GpuRenderSemanticVertex *pb = native_coverage_vertex(now, key->component, key->group, key->vertex);
            if (!pa || !pb) conflict = 1;
            else {
                GlNativeVertexSample anchors[2];
                native_motion_vertex_sample(&anchors[0], pa, 0, 0, previous->offset);
                native_motion_vertex_sample(&anchors[1], pb, 0, 0, current->offset);
                /* Missing visibility is recoverable only from the authenticated
                 * full source snapshot. Any disagreement with a drawn endpoint
                 * rejects the atomic component, never matches by position. */
                if (memcmp(&anchors[1], &pair->endpoints[1], sizeof(anchors[1])) ||
                    ((seen & 1u) && memcmp(&anchors[0], &pair->endpoints[0], sizeof(anchors[0])))) conflict = 1;
                pair->anchor_recovered = !(seen & 1u) && !conflict;
                pair->endpoints[0] = anchors[0]; seen |= 1u;
            }
        }
        const GlNativeVertexSample *a = &pair->endpoints[0], *b = &pair->endpoints[1];
        pair->valid = seen == 3u && !conflict && a->native == b->native && a->projective == b->projective &&
            (!b->projective ||
                (a->distance && b->distance && a->view[2] > 0 && b->view[2] > 0));
        diag->vertices++;
        if (seen != 3u) diag->missing++;
        else if (conflict) diag->conflicts++;
        else if (pair->valid) diag->matched++;
        cache->pair_count++;
        first = end;
    }
    free(ordered);
    free(uses);
    for (uint32_t i = 0u; i < cache->pair_count; ++i)
        cache->pairs[i].mesh = native_motion_mesh_root(cache, cache->pairs[i].mesh);
    GlNativeMotionDrawKey *texture_keys = NULL;
    uint32_t texture_key_count = 0u;
    for (uint32_t i = 0u; i < current->count; ++i) {
        if (cache->draw_mesh[i] == UINT32_MAX) continue;
        const uint32_t root = cache->draw_mesh[i] = native_motion_mesh_root(cache, cache->draw_mesh[i]);
        GlNativeVertexMesh *mesh = &cache->meshes[root];
        if (current->draws[i].motion.motion.handle.resource_id) {
            mesh->bound = 1u;
            const GlNativeRecipeMesh *run = current->draws[i].mesh;
            for (uint32_t t = 0u; run && run->poses && t < run->count; ++t) {
                if (t && native_recipe_pose_equal(&run->poses[t], &run->poses[t - 1u])) continue;
                const uint32_t owner = native_motion_mesh_find(cache, run->poses[t].scene_id,
                    run->poses[t].producer_id);
                if (owner != UINT32_MAX) cache->meshes[native_motion_mesh_root(cache, owner)].bound = 1u;
            }
            continue;
        }
        mesh->unbound = 1u;
        int check_texture = 0;
        for (uint32_t v = 0u; v < current->draws[i].semantic.triangle_count * 3u; ++v) {
            const uint32_t key = cache->map[i][v];
            if (key == UINT32_MAX || !cache->pairs[key].valid) mesh->bad = 1u;
            else if (current->draws[i].semantic.material.textured &&
                     !cache->pairs[key].endpoints[1].projective) check_texture = 1;
        }
        if (check_texture && !mesh->bad) {
            if (!texture_keys && !native_motion_texture_keys(previous, &texture_keys, &texture_key_count)) {
                native_motion_vertices_discard(cache); return 0;
            }
            /* Billboard animation is discrete. Compare this primitive's own
             * previous footprint, rejecting missing or ambiguous identities. */
            const uint32_t prior = native_motion_texture_previous(current, i, texture_keys, texture_key_count);
            if (prior == UINT32_MAX || !native_motion_texture_footprint_equal(
                    &previous->draws[prior].semantic, &current->draws[i].semantic)) mesh->bad = 1u;
        }
    }
    free(texture_keys);
    cache->deltas = calloc((size_t)(cache->pair_count ? cache->pair_count : 1u) * phases, sizeof(*cache->deltas));
    if (!cache->deltas) { native_motion_vertices_discard(cache); return 0; }
    for (uint32_t i = 0u; i < cache->pair_count; ++i) {
        GlNativeVertexPair *pair = &cache->pairs[i];
        GlNativeVertexMesh *mesh = &cache->meshes[pair->mesh];
        if (!mesh->unbound || mesh->bound || mesh->bad) continue;
        const GlNativeVertexSample *a = &pair->endpoints[0], *b = &pair->endpoints[1];
        for (uint32_t phase = 0u; phase < phases; ++phase) {
            const double weight = 1.0 - (double)(phase + 1u) / (phases + 1u);
            double bend[2] = {0};
            if (b->projective) {
                double projected[3][2];
                for (unsigned sample = 0u; sample < 3u; ++sample) {
                    const double w = sample == 0u ? 1.0 : sample == 1u ? 0.0 : weight;
                    const double z = b->view[2] + w * ((double)a->view[2] - b->view[2]);
                    const double h = b->distance + w * ((double)a->distance - b->distance);
                    const double q = fmin(h / fmin(z, 65535.0), 131071.0 / 65536.0);
                    for (unsigned axis = 0u; axis < 2u; ++axis) {
                        const double value = b->view[axis] + w * ((double)a->view[axis] - b->view[axis]);
                        projected[sample][axis] = b->offset[axis] + w * (a->offset[axis] - b->offset[axis]) +
                            fmax(-32768.0, fmin(32767.0, value)) * q * 65536.0;
                    }
                }
                for (unsigned axis = 0u; axis < 2u; ++axis)
                    bend[axis] = (projected[2][axis] - projected[1][axis]) -
                        weight * (projected[0][axis] - projected[1][axis]);
            }
            for (unsigned axis = 0u; axis < 4u; ++axis) {
                const double delta = weight * (a->position[axis] - b->position[axis]) + bend[axis & 1u];
                if (!isfinite(delta) || delta < -4294967295.0 || delta > 4294967295.0) mesh->bad = 1u;
                else cache->deltas[(size_t)phase * cache->pair_count + i][axis] = (int64_t)llround(delta);
            }
            /* View Z is affine in the source frame interval: the same lerp the
             * bend above projects. A missing endpoint depth keeps the current one. */
            cache->deltas[(size_t)phase * cache->pair_count + i][4] = a->depth > 0 && b->depth > 0
                ? (int64_t)llround(weight * ((double)a->depth - b->depth)) : 0;
        }
    }
    /* One policy for the whole producer/component, including new visibility
     * vertices and aliases shared with another producer. Never move half a mesh. */
    for (uint32_t i = 0u; i < current->count; ++i) {
        if (cache->draw_mesh[i] == UINT32_MAX) continue;
        GlNativeVertexMesh *mesh = &cache->meshes[cache->draw_mesh[i]];
        if (!mesh->unbound || mesh->bound || mesh->bad) continue;
        for (uint32_t v = 0u; v < current->draws[i].semantic.triangle_count * 3u; ++v) {
            const GpuRenderSemanticVertex *p = &current->draws[i].semantic.triangles[v / 3u].vertices[v % 3u];
            const int64_t position[4] = {p->x, p->y, p->native_view_x, p->native_view_y};
            for (uint32_t phase = 0u; phase < phases; ++phase)
                for (unsigned axis = 0u; axis < (p->native_view_position ? 4u : 2u); ++axis) {
                    const int64_t value = position[axis] + cache->deltas[(size_t)phase * cache->pair_count + cache->map[i][v]][axis];
                    if (value < INT32_MIN || value > INT32_MAX) mesh->bad = 1u;
                }
        }
    }
    for (uint32_t i = 0u; i < cache->mesh_count; ++i) {
        GlNativeVertexMesh *mesh = &cache->meshes[i];
        mesh->enabled = mesh->parent == i && mesh->unbound && !mesh->bound && !mesh->bad;
        if (mesh->enabled) diag->active_meshes++;
        else if (mesh->parent == i && mesh->unbound) diag->discrete_meshes++;
    }
    for (uint32_t i = 0u; i < cache->pair_count; ++i)
        if (cache->meshes[cache->pairs[i].mesh].enabled &&
            memcmp(cache->pairs[i].endpoints[0].position, cache->pairs[i].endpoints[1].position,
                sizeof(cache->pairs[i].endpoints[0].position))) diag->moved_vertices++;
    return 1;
}

/* One pose-bound draw (or triangle of a run) and its entity: a single owner
 * producer/scene/component/scope, a bound current pose, recorded source
 * history, and no unbound geometry in its producer's vertex mesh. */
static void native_motion_entity_owner(const GlNativeRecipe *previous, const GlNativeRecipe *current,
                                       const GlNativeRecipeDraw *draw, uint32_t i,
                                       GlNativeMotionEntity *entity,
                                       const GpuRenderInterpolationIdentity *owner,
                                       int pose_current, int partial) {
    const uint32_t scope = draw->temporal_component ? current->coverages[draw->temporal_index].view.header->producer_scope : 0u;
    if (draw->semantic.screen_space_2d || !pose_current ||
        !owner->valid || !owner->producer_id || (entity->owner_producer &&
        (entity->owner_producer != owner->producer_id || entity->owner_scene != owner->scene_id ||
         entity->owner_component != draw->temporal_component || entity->owner_scope != scope))) {
        entity->enabled = 0;
        native_motion_reject("entity_producer_conflict", entity->current_pose, i, owner->producer_id);
    }
    entity->owner_scene = owner->scene_id; entity->owner_producer = owner->producer_id;
    entity->owner_component = draw->temporal_component; entity->owner_scope = scope;
    if (draw->temporal_component && !native_coverage_previous(previous, current, draw)) {
        entity->enabled = 0;
        native_motion_reject("component_publication_history", entity->current_pose, i, owner->producer_id);
    }
    if (partial) {
        entity->enabled = 0;
        native_motion_reject("partial_entity_binding", entity->current_pose, i, owner->producer_id);
    }
}

static void native_motion_entities(const GlNativeRecipe *previous, const GlNativeRecipe *current,
                               const XgPresentationIdentity *identity, const GlNativeVertexCache *vertices,
                               GlNativeMotionEntity *entities) {
    /* Both recipes retain these immutable poses for the entire preparation.
     * Resolve each prior resource once, rather than taking its repository lock
     * for every current/prior entity pair. Allocation failure keeps the exact
     * original lookup path. */
    const XgRenderMotionPose **previous_poses = calloc(previous->motion_count, sizeof(*previous_poses));
    if (previous_poses)
        for (uint32_t j = 0u; j < previous->motion_count; ++j)
            (void)xg_render_motion_view(previous->motions[j], &previous_poses[j]);
    for (uint32_t i = 0u; i < current->motion_count; ++i) {
        GlNativeMotionEntity *entity = &entities[i];
        const XgRenderMotionPose *pose = NULL;
        if (!xg_render_motion_view(current->motions[i], &pose) ||
            pose->presentation_epoch != identity->presentation_epoch ||
            pose->scene_generation != identity->scene_generation || pose->discontinuity)
        {
            native_motion_reject("current_pose_lifecycle", pose, UINT32_MAX, 0u); continue;
        }
        entity->current_pose = pose;
        int duplicate = 0;
        for (uint32_t j = 0u; j < previous->motion_count; ++j) {
            const XgRenderMotionPose *before = previous_poses ? previous_poses[j] : NULL;
            if ((!previous_poses && !xg_render_motion_view(previous->motions[j], &before)) || !before) continue;
            if (before->entity_id != pose->entity_id) continue;
            duplicate |= entity->previous.handle.resource_id != 0u;
            entity->previous = previous->motions[j]; entity->previous_pose = before;
        }
        const XgRenderMotionPose *before = entity->previous_pose;
        if (!before || duplicate) {
            native_motion_reject(duplicate ? "multiple_previous_poses" : "missing_previous_entity", pose, UINT32_MAX, 0u);
            continue;
        }
        if (before->presentation_epoch != pose->presentation_epoch ||
            before->scene_generation != pose->scene_generation ||
            before->continuity_generation != pose->continuity_generation ||
            before->geometry_id != pose->geometry_id || before->geometry_generation != pose->geometry_generation ||
            before->node_count != pose->node_count || before->camera_id != pose->camera_id) {
            native_motion_reject("entity_continuity", pose, UINT32_MAX, 0u); continue;
        }
        entity->enabled = 1;
    }
    /* Unbound source geometry has its own shared-vertex policy. It must not
     * disable unrelated poses, nor replace any successfully bound hierarchy. */
    for (uint32_t i = 0u; i < current->count; ++i) {
        const GlNativeRecipeDraw *draw = &current->draws[i];
        if (!draw->motion.motion.handle.resource_id || draw->motion_index >= current->motion_count) continue;
        const GlNativeRecipeMesh *run = draw->mesh;
        if (run && run->poses) {
            /* A run crossing objects: the same ownership rules for each
             * triangle's own pose, as if it were still its own draw. */
            for (uint32_t t = 0u; t < run->count; ++t) {
                const GlNativeRecipeMeshPose *pose = &run->poses[t];
                if (t && native_recipe_pose_equal(pose, &run->poses[t - 1u])) continue;
                if (pose->motion_index >= current->motion_count) continue;
                const GpuRenderInterpolationIdentity owner = {.scene_id = pose->scene_id,
                    .producer_id = pose->producer_id, .valid = pose->identity_valid};
                uint32_t owner_mesh = native_motion_mesh_find(vertices, pose->scene_id, pose->producer_id);
                while (owner_mesh != UINT32_MAX && vertices->meshes[owner_mesh].parent != owner_mesh)
                    owner_mesh = vertices->meshes[owner_mesh].parent;
                native_motion_entity_owner(previous, current, draw, i, &entities[pose->motion_index],
                    &owner, 1, owner_mesh != UINT32_MAX && vertices->meshes[owner_mesh].unbound);
            }
            continue;
        }
        native_motion_entity_owner(previous, current, draw, i, &entities[draw->motion_index],
            &draw->semantic.interpolation_identity,
            native_motion_ref_equal(draw->motion.motion, current->motions[draw->motion_index]),
            vertices->draw_mesh[i] != UINT32_MAX && vertices->meshes[vertices->draw_mesh[i]].unbound);
    }
    for (uint32_t i = 0u; i < current->motion_count; ++i) {
        if (!entities[i].owner_producer) entities[i].enabled = 0;
        for (uint32_t j = 0u; j < i; ++j) {
            const XgRenderMotionPose *a = entities[i].current_pose, *b = entities[j].current_pose;
            if (!a || !b) continue;
            if (a->entity_id == b->entity_id || (a->camera_id == b->camera_id && !native_motion_camera_equal(a, b)) ||
                (entities[i].previous_pose && entities[j].previous_pose &&
                 a->camera_id == b->camera_id && !native_motion_camera_equal(entities[i].previous_pose, entities[j].previous_pose))) {
                entities[i].enabled = entities[j].enabled = 0;
                native_motion_reject("camera_or_entity_conflict", a, UINT32_MAX, entities[i].owner_producer);
            }
        }
    }
    /* Bound geometry cannot trigger the partial-binding rule. Visit unbound
     * records once, then compare their owners; do not rescan every large draw
     * array for every entity. All producer/scene/component/scope checks remain. */
    for (int frame = 0; frame < 2; ++frame) {
        const GlNativeRecipe *recipe = frame ? current : previous;
        for (uint32_t j = 0u; j < recipe->count; ++j) {
            const GlNativeRecipeDraw *draw = &recipe->draws[j];
            if (draw->motion.motion.handle.resource_id) continue;
            const GpuRenderInterpolationIdentity *owner = &draw->semantic.interpolation_identity;
            const uint32_t scope = draw->temporal_component ? recipe->coverages[draw->temporal_index].view.header->producer_scope : 0u;
            for (uint32_t i = 0u; i < current->motion_count; ++i)
                if (owner->producer_id == entities[i].owner_producer &&
                    owner->scene_id == entities[i].owner_scene && scope == entities[i].owner_scope &&
                    draw->temporal_component == entities[i].owner_component) {
                    entities[i].enabled = 0;
                    native_motion_reject("partial_entity_binding", entities[i].current_pose, j, owner->producer_id);
                }
        }
    }
    free(previous_poses);
}

typedef struct GlNativeEntityBatch {
    const GlNativeRecipe *recipe;
    GlNativeMotionEntity *entities;
    const GlNativeMotionEntity *source;
    double alpha;
    int advance;
} GlNativeEntityBatch;

static void native_motion_evaluate_ranges(uint32_t begin, uint32_t end, void *context) {
    const GlNativeEntityBatch *batch = context;
    GlNativeMotionEntity *entities = batch->entities;
    for (uint32_t i = begin; i < end; ++i) {
        if (!entities[i].enabled) continue;
        const int evaluated=batch->advance ? xg_render_motion_advance(batch->alpha,&entities[i].evaluation) :
            xg_render_motion_evaluate(entities[i].previous,batch->recipe->motions[i],batch->alpha,&entities[i].evaluation);
        if (!evaluated ||
            (!entities[i].evaluation.interpolated && !native_motion_ref_equal(entities[i].previous, batch->recipe->motions[i]))) {
            entities[i].enabled = 0;
            native_motion_reject("pose_evaluation", entities[i].current_pose, UINT32_MAX, entities[i].owner_producer);
        }
    }
}

static void native_motion_evaluate_entities(const GlNativeRecipe *recipe, GlNativeMotionEntity *entities,
                                            double alpha, int advance, uint32_t *evaluations) {
    for (uint32_t i = 0u; i < recipe->motion_count; ++i) *evaluations += entities[i].enabled != 0;
    GlNativeEntityBatch batch = {.recipe = recipe, .entities = entities, .alpha = alpha, .advance = advance};
    psx_parallel_for(recipe->motion_count, 1u, native_motion_evaluate_ranges, &batch);
}

typedef struct GlNativeProjectionBatch {
    const GlNativeRecipe *recipe;
    GlNativeMotionEntity *entities;
    GlNativeMotionProjection *projections;
    const GlNativeMotionEndpoints *endpoints;
    uint8_t *invalid;
} GlNativeProjectionBatch;

static void native_motion_project_ranges(uint32_t begin, uint32_t end, void *context) {
    const GlNativeProjectionBatch *batch = context;
    const GlNativeRecipe *recipe = batch->recipe;
    GlNativeMotionEntity *entities = batch->entities;
    for (uint32_t i = begin; i < end; ++i) {
        const GlNativeRecipeDraw *draw = &recipe->draws[i];
        if (!draw->motion.motion.handle.resource_id || draw->motion_index >= recipe->motion_count ||
            draw->mesh) continue;
        GlNativeMotionEntity *entity = &entities[draw->motion_index];
        if (!entity->enabled) continue;
        GlNativeMotionProjection *projection = &batch->projections[i];
        const XgRenderMotionProjectResult result = projection->result = xg_render_motion_project_shared(
            &entity->evaluation, &draw->motion, native_motion_endpoints_at(batch->endpoints, i, 0u),
            projection->screen, projection->native);
        if (result == XG_RENDER_MOTION_ENDPOINT) continue;
        int valid = result == XG_RENDER_MOTION_PROJECTED;
        for (uint32_t t = 0u; valid && t < draw->semantic.triangle_count; ++t)
            for (uint32_t v = 0u; valid && v < 3u; ++v) {
                const GpuRenderSemanticVertex *p = &draw->semantic.triangles[t].vertices[v];
                const double positions[4] = {p->x + projection->screen[t][v][0] * 65536.0,
                    p->y + projection->screen[t][v][1] * 65536.0,
                    p->native_view_x + projection->native[t][v][0] * 65536.0,
                    p->native_view_y + projection->native[t][v][1] * 65536.0};
                for (unsigned axis = 0u; axis < (p->native_view_position ? 4u : 2u); ++axis)
                    valid &= isfinite(positions[axis]) && positions[axis] >= INT32_MIN && positions[axis] <= INT32_MAX;
            }
        if (!valid) {
            if (batch->invalid) batch->invalid[i] = 1u;
            else {
                entity->enabled = 0;
                native_motion_reject("entity_projection", entity->current_pose, i, entity->owner_producer);
            }
        }
    }
}

static void native_motion_project_draws(const GlNativeRecipe *recipe, GlNativeMotionEntity *entities,
                                        GlNativeMotionProjection *projections,
                                        const GlNativeMotionEndpoints *endpoints) {
    uint8_t *invalid = calloc(recipe->count ? recipe->count : 1u, 1u);
    GlNativeProjectionBatch batch = {recipe, entities, projections, endpoints, invalid};
    if (invalid) {
        psx_parallel_for(recipe->count, 32u, native_motion_project_ranges, &batch);
        /* Entity rejection stays atomic and in original draw order. Workers
         * never mutate shared entity flags while other draws read them. */
        for (uint32_t i = 0u; i < recipe->count; ++i) {
            if (!invalid[i]) continue;
            GlNativeMotionEntity *entity = &entities[recipe->draws[i].motion_index];
            if (!entity->enabled) continue;
            entity->enabled = 0;
            native_motion_reject("entity_projection", entity->current_pose, i, entity->owner_producer);
        }
    } else native_motion_project_ranges(0u, recipe->count, &batch);
    free(invalid);
}

static void native_motion_preflight(const GlNativeRecipe *recipe, GlNativeMotionEntity *entities,
                                 double alpha, int advance, GlNativeMotionProjection *projections, uint32_t *evaluations) {
    native_motion_evaluate_entities(recipe, entities, alpha, advance, evaluations);
    native_motion_project_draws(recipe, entities, projections, NULL);
}

/* Records [begin, end) of endpoint job `job`: every pose-bound DRAW corner's
 * endpoints, and every mesh corner's unique vertex (deduplicated by pose,
 * part and LOCAL position within the job) with its endpoints, all under the
 * entities' evaluated pairs (shared by all phases of the interval). */
typedef struct GlNativeEndpointBatch {
    GlNativeMotionEndpoints *endpoints;
    const GlNativeMotionEntity *entities;
    uint32_t base;
} GlNativeEndpointBatch;

static void native_motion_endpoint_ranges(uint32_t begin, uint32_t end, void *context) {
    const GlNativeEndpointBatch *batch = context;
    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t u = batch->base + i;
        const GlNativeMotionVertex *vertex = &batch->endpoints->vertices[u];
        (void)xg_render_motion_vertex_endpoints(&batch->entities[vertex->motion_index].evaluation,
            vertex->part, &vertex->local, &batch->endpoints->vertex_endpoints[u]);
    }
}

static void native_motion_endpoints_compute(const GlNativeRecipe *recipe, const GlNativeMotionEntity *entities,
                                            GlNativeMotionEndpoints *endpoints,
                                            uint32_t begin, uint32_t end, uint32_t job) {
    const uint32_t base = endpoints->offsets[begin];
    uint32_t unique = base;
    /* Open-addressed slots of this job's unique vertices. */
    uint32_t capacity = 64u;
    while (capacity < 2u * (endpoints->offsets[end] - base)) capacity *= 2u;
    uint32_t *table = endpoints->corner_vertices ? malloc((size_t)capacity * sizeof(*table)) : NULL;
    if (table) memset(table, 0xff, (size_t)capacity * sizeof(*table));
    for (uint32_t i = begin; i < end; ++i) {
        const GlNativeRecipeDraw *draw = &recipe->draws[i];
        const uint32_t first = endpoints->offsets[i];
        const uint32_t count = endpoints->offsets[i + 1u] - first;
        XgRenderMotionVertexEndpoints *corners = endpoints->corners + first;
        for (uint32_t c = 0u; c < count; ++c) corners[c].valid = 0u;
        if (!count) continue;
        const GlNativeRecipeMesh *mesh = draw->mesh;
        if (!mesh) {
            if (draw->motion_index < recipe->motion_count && entities[draw->motion_index].enabled)
                (void)xg_render_motion_project_endpoints(&entities[draw->motion_index].evaluation,
                    &draw->motion, (XgRenderMotionVertexEndpoints (*)[3])(void *)corners);
            continue;
        }
        uint32_t *corner_vertices = endpoints->corner_vertices + first;
        for (uint32_t t = 0u; t < mesh->count; ++t) {
            const uint32_t motion_index = mesh->poses ? mesh->poses[t].motion_index : draw->motion_index;
            const uint32_t part = mesh->poses ? mesh->poses[t].part : draw->motion.motion_part_index;
            const int enabled = table && motion_index < recipe->motion_count && entities[motion_index].enabled;
            for (uint32_t v = 0u; v < 3u; ++v) {
                uint32_t *corner = &corner_vertices[(size_t)t * 3u + v];
                *corner = UINT32_MAX;
                if (!enabled) continue;
                const XgHost3dVector *p = &mesh->triangles[t].local[v];
                uint64_t key = (uint16_t)p->x | (uint64_t)(uint16_t)p->y << 16u |
                    (uint64_t)(uint16_t)p->z << 32u | (uint64_t)(part & 0xffffu) << 48u;
                key = (key ^ (uint64_t)motion_index * UINT64_C(0x9e3779b97f4a7c15)) *
                    UINT64_C(0xbf58476d1ce4e5b9);
                for (uint32_t h = (uint32_t)(key >> 32u) & (capacity - 1u);; h = (h + 1u) & (capacity - 1u)) {
                    const uint32_t slot = table[h];
                    if (slot == UINT32_MAX) {
                        GlNativeMotionVertex *vertex = &endpoints->vertices[unique];
                        vertex->local = *p; vertex->motion_index = motion_index; vertex->part = part;
                        table[h] = *corner = unique++;
                        break;
                    }
                    const GlNativeMotionVertex *vertex = &endpoints->vertices[slot];
                    if (vertex->motion_index == motion_index && vertex->part == part &&
                        vertex->local.x == p->x && vertex->local.y == p->y && vertex->local.z == p->z) {
                        *corner = slot;
                        break;
                    }
                }
            }
        }
    }
    free(table);
    endpoints->ranges[job][0] = base;
    endpoints->ranges[job][1] = unique;
    GlNativeEndpointBatch batch = {endpoints, entities, base};
    psx_parallel_for(unique - base, 32u, native_motion_endpoint_ranges, &batch);
}

/* Phases read the same immutable recipe but write independent projections and
 * command journals. Only the compile owner merges/publishes those journals;
 * all OpenGL execution stays on its existing FIFO owner. */
typedef struct GlNativePhaseJob {
    const GlNativeRecipe *recipe;
    const GlNativeGpuWork *raster_source;
    const GlNativeVertexCache *vertices;
    const GlNativeMotionEntity *render_entities;
    GlNativeMotionEntity *entities;
    GlNativeMotionProjection *projections;
    GlNativeMotionEndpoints *endpoints;
    uint32_t endpoint_begin, endpoint_end; /* records of an endpoint job */
    const uint16_t *initial_words;
    const GlNativePhaseTextures *phase_textures;
    const GlNativePhaseTemplates *phase_templates;
    const uint8_t *static_records;
    int capture_templates;
    XgPresentationIdentity identity;
    GlNativeGpuWork *gpu;
    GlNativeRecipeStats stats;
    uint32_t phase, denominator, evaluations;
    uint64_t project_cpu_ns, capture_cpu_ns, geometry_cpu_ns;
    uint32_t scale;
    uint16_t crop_y, height;
    int depth_test, depth_view, wireframe;
    int ok, completed;
} GlNativePhaseJob;

typedef struct GlNativePhasePool {
    SDL_mutex *mutex;
    SDL_cond *work, *done;
    SDL_Thread **threads;
    uint32_t worker_count;
    GlNativePhaseJob *jobs;
    uint32_t next, count, completed;
    int run, capture, attempted;
} GlNativePhasePool;
static GlNativePhasePool s_native_phase_pool;

static void native_motion_entity_copy(GlNativeMotionEntity *target, const GlNativeMotionEntity *source) {
    target->enabled = source->enabled;
    if (!source->enabled) return;
    memcpy(target, source, offsetof(GlNativeMotionEntity, evaluation));
    memcpy(&target->projections, &source->projections,
        sizeof(*source) - offsetof(GlNativeMotionEntity, projections));
    XgRenderMotionEvaluation *a = &target->evaluation;
    const XgRenderMotionEvaluation *b = &source->evaluation;
    memcpy(a, b, offsetof(XgRenderMotionEvaluation, phase));
    /* Only live nodes are ever addressed by advance/project. Copy their exact
     * matrices/anchors, without touching the unused fixed-capacity tail or
     * allocating physical pages for rejected entities. */
    const size_t transform_bytes = offsetof(XgRenderMotionTransform, model_to_view) +
        b->node_count * sizeof(b->phase.model_to_view[0]);
    memcpy(&a->phase, &b->phase, transform_bytes);
    memcpy(&a->native_phase, &b->native_phase, transform_bytes);
    memcpy(&a->native_current, &b->native_current, transform_bytes);
    for (uint32_t endpoint = 0u; endpoint < 2u; ++endpoint) {
        memcpy(&a->endpoints[endpoint], &b->endpoints[endpoint], transform_bytes);
        memcpy(a->source_projection[endpoint], b->source_projection[endpoint],
            b->node_count * sizeof(b->source_projection[endpoint][0]));
        memcpy(a->curve_endpoints[endpoint], b->curve_endpoints[endpoint],
            b->node_count * sizeof(b->curve_endpoints[endpoint][0]));
    }
    memcpy(a->curve_wrap_shift, b->curve_wrap_shift, sizeof(b->curve_wrap_shift));
}

static void native_motion_copy_ranges(uint32_t begin, uint32_t end, void *context) {
    const GlNativeEntityBatch *batch = context;
    for (uint32_t i = begin; i < end; ++i)
        native_motion_entity_copy(&batch->entities[i], &batch->source[i]);
}

enum { NATIVE_PHASE_PREFLIGHT = 0, NATIVE_PHASE_CAPTURE = 1, NATIVE_PHASE_ENDPOINTS = 2 };

static void native_phase_job_execute(GlNativePhaseJob *job, int capture) {
    const uint64_t cpu_begin = native_thread_cpu_ns();
    if (capture == NATIVE_PHASE_ENDPOINTS) {
        native_motion_endpoints_compute(job->recipe, job->render_entities, job->endpoints,
            job->endpoint_begin, job->endpoint_end, job->phase);
        job->project_cpu_ns += native_thread_cpu_ns() - cpu_begin;
        return;
    }
    if (!capture) {
        GlNativeEntityBatch copies = {.entities = job->entities, .source = job->render_entities};
        psx_parallel_for(job->recipe->motion_count, 1u, native_motion_copy_ranges, &copies);
        /* Phase zero evaluated the shared endpoint curves before the copies.
         * Each other phase advances its own copy without evaluating endpoints
         * again, and can disable only its private entity flags. Every phase
         * projects its own draws over the interval's shared corner endpoints. */
        if (job->phase)
            native_motion_evaluate_entities(job->recipe, job->entities,
                (double)(job->phase + 1u) / job->denominator, 1, &job->evaluations);
        native_motion_project_draws(job->recipe, job->entities, job->projections, job->endpoints);
        job->project_cpu_ns += native_thread_cpu_ns() - cpu_begin;
        return;
    }
    GlNativeGpuWork *gpu = job->gpu = calloc(1u, sizeof(*gpu));
    job->ok = 0;
    if (!gpu) return;
    gpu->identity = job->identity;
    gpu->deferred = 1;
    gpu->phase_textures = job->phase_textures;
    gpu->phase_templates = job->phase_templates;
    gpu->capture_templates = job->capture_templates;
    gpu->static_records = job->static_records;
    gpu->scale = job->scale;
    if (job->raster_source) {
        gpu->render_width = job->raster_source->render_width;
        gpu->render_height = job->raster_source->render_height;
        memcpy(gpu->raster_scale, job->raster_source->raster_scale, sizeof(gpu->raster_scale));
        gpu->raster_page_rows = job->raster_source->raster_page_rows;
        gpu->raster_page_pixels = job->raster_source->raster_page_pixels;
    }
    gpu->wave_phase = (double)(job->phase + 1u) / job->denominator;
    gpu->depth_test = job->depth_test;
    gpu->depth_view = job->depth_view;
    gpu->wireframe = job->wireframe;
    if (!gpu->phase_textures)
        gpu->words = malloc((size_t)VRAM_W * VRAM_H * sizeof(*gpu->words));
    gpu->capacity = native_gpu_reserve_hint(job->recipe->count + 1024u);
    gpu->commands = malloc((size_t)gpu->capacity * sizeof(*gpu->commands));
    if ((!gpu->words && !gpu->phase_textures) || !gpu->commands) return;
    if (gpu->words)
        memcpy(gpu->words, job->initial_words, (size_t)VRAM_W * VRAM_H * sizeof(*gpu->words));
    if (job->phase && !gpu->phase_textures) {
        GlNativeGpuCommand *reset = native_gpu_command(gpu, NATIVE_GPU_WORDS_RESET);
        if (!reset) return;
        reset->w = VRAM_W; reset->h = VRAM_H;
    }
    uint32_t *unused = NULL;
    /* Each unique mesh vertex's part of this phase, once. */
    GlNativeMotionPhaseVertex *phase_vertices = NULL;
    if (job->endpoints && job->endpoints->corner_vertices) {
        phase_vertices = malloc((job->endpoints->corner_count ? job->endpoints->corner_count : 1u) *
            sizeof(*phase_vertices));
        if (!phase_vertices) return;
        native_motion_phase_vertices(job->endpoints, job->render_entities, job->entities,
            job->phase_templates, phase_vertices);
    }
    job->ok = native_recipe_render_rows(job->recipe, job->render_entities, job->vertices,
        job->phase, job->recipe->view_width != 0u, &job->stats, job->crop_y,
        job->height, &unused, 0u, job->height, gpu, job->projections, job->entities, job->endpoints,
        phase_vertices);
    free(phase_vertices);
    free(unused);
    /* The private word shadow only diffs texture pages while rows render;
     * merge transfers commands and data, never words. */
    free(gpu->words);
    gpu->words = NULL;
    /* Commands outlive the job until GL consumes them: drop the reserve
     * headroom (power-of-two growth plus 1024 spare commands). */
    if (job->ok && gpu->count != 0u && gpu->count < gpu->capacity) {
        GlNativeGpuCommand *fit = realloc(gpu->commands, (size_t)gpu->count * sizeof(*fit));
        if (fit) { gpu->commands = fit; gpu->capacity = gpu->count; }
    }
    const uint64_t geometry_begin = native_thread_cpu_ns();
    job->capture_cpu_ns = geometry_begin - cpu_begin;
    if (job->ok && gpu->capture_templates && !gpu->wireframe && !gpu->depth_view &&
        !s_native_depth_stats_enabled && !gl_renderer_debug_mipmaps()) {
        uint64_t triangles = 0u;
        for (uint32_t c = 0u; c < gpu->count; ++c)
            if (gpu->commands[c].kind == NATIVE_GPU_DRAW &&
                gpu->commands[c].draw.topology == GPU_RENDER_SEMANTIC_TRIANGLES)
                triangles += native_gpu_command_triangles(&gpu->commands[c]);
        gpu->gpu_setup = !gpu->render_width && native_gpu_setup_enabled(triangles);
    }
    if (job->ok && !s_native_motion_probe.target_vblank)
        gpu->geometry = native_gpu_geometry_prepare(gpu);
    job->geometry_cpu_ns = native_thread_cpu_ns() - geometry_begin;
}

static int native_phase_thread(void *unused) {
    GlNativePhasePool *pool = &s_native_phase_pool;
    (void)unused;
    SDL_LockMutex(pool->mutex);
    while (pool->run) {
        while (pool->run && pool->next == pool->count) SDL_CondWait(pool->work, pool->mutex);
        if (!pool->run) break;
        GlNativePhaseJob *job = &pool->jobs[pool->next++];
        const int capture = pool->capture;
        SDL_UnlockMutex(pool->mutex);
        native_phase_job_execute(job, capture);
        SDL_LockMutex(pool->mutex);
        job->completed = 1;
        ++pool->completed;
        SDL_CondSignal(pool->done);
    }
    SDL_UnlockMutex(pool->mutex);
    return 0;
}

void native_phase_pool_stop(void) {
    GlNativePhasePool *pool = &s_native_phase_pool;
    if (pool->mutex) {
        SDL_LockMutex(pool->mutex);
        pool->run = 0;
        if (pool->work) SDL_CondBroadcast(pool->work);
        SDL_UnlockMutex(pool->mutex);
    }
    for (uint32_t i = 0u; i < pool->worker_count; ++i)
        SDL_WaitThread(pool->threads[i], NULL);
    free(pool->threads);
    if (pool->done) SDL_DestroyCond(pool->done);
    if (pool->work) SDL_DestroyCond(pool->work);
    if (pool->mutex) SDL_DestroyMutex(pool->mutex);
    memset(pool, 0, sizeof(*pool));
}

static int native_phase_pool_start(uint32_t work_items) {
    GlNativePhasePool *pool = &s_native_phase_pool;
    if (pool->attempted && !pool->run) return 0;
    pool->attempted = 1;
    const uint32_t workers = psx_host_worker_count((unsigned)SDL_GetCPUCount(), work_items);
    if (!workers) return 0;
    if (!pool->run) {
        pool->mutex = SDL_CreateMutex();
        pool->work = SDL_CreateCond();
        pool->done = SDL_CreateCond();
        if (!pool->mutex || !pool->work || !pool->done) goto failed;
        pool->run = 1;
    }
    if (workers <= pool->worker_count) return 1;
    SDL_Thread **threads = realloc(pool->threads, (size_t)workers * sizeof(*threads));
    if (!threads) {
        if (pool->worker_count) return 1;
        goto failed;
    }
    pool->threads = threads;
    /* Grow between batches only. Readers use the pool, never this resizable
     * handle array. Keep asynchronous prefix publication on one CPU too. */
    SDL_LockMutex(pool->mutex);
    while (pool->worker_count < workers) {
        const uint32_t i = pool->worker_count;
        pool->threads[i] = SDL_CreateThread(native_phase_thread, "native-phase", NULL);
        if (!pool->threads[i]) break;
        ++pool->worker_count;
    }
    SDL_UnlockMutex(pool->mutex);
    if (!pool->worker_count) goto failed;
    return 1;
failed:
    native_phase_pool_stop();
    pool->attempted = 1;
    return 0;
}

static int native_phase_begin(GlNativePhaseJob *jobs, uint32_t count, int capture) {
    if (count < 2u || !native_phase_pool_start(count)) {
        for (uint32_t i = 0u; i < count; ++i) native_phase_job_execute(&jobs[i], capture);
        return 0;
    }
    GlNativePhasePool *pool = &s_native_phase_pool;
    SDL_LockMutex(pool->mutex);
    pool->jobs = jobs; pool->count = count; pool->next = pool->completed = 0u;
    pool->capture = capture;
    for (uint32_t i = 0u; i < count; ++i) jobs[i].completed = 0;
    SDL_CondBroadcast(pool->work);
    SDL_UnlockMutex(pool->mutex);
    return 1;
}

static void native_phase_wait_job(GlNativePhaseJob *job) {
    GlNativePhasePool *pool = &s_native_phase_pool;
    SDL_LockMutex(pool->mutex);
    while (!job->completed) SDL_CondWait(pool->done, pool->mutex);
    SDL_UnlockMutex(pool->mutex);
}

static void native_phase_finish(void) {
    GlNativePhasePool *pool = &s_native_phase_pool;
    SDL_LockMutex(pool->mutex);
    while (pool->completed != pool->count) SDL_CondWait(pool->done, pool->mutex);
    pool->jobs = NULL;
    SDL_UnlockMutex(pool->mutex);
}

static void native_phase_run(GlNativePhaseJob *jobs, uint32_t count, int capture) {
    if (native_phase_begin(jobs, count, capture)) native_phase_finish();
}

static int native_phase_merge(GlNativeGpuWork *target, GlNativePhaseJob *job,
                              uint32_t initial_words_offset) {
    GlNativeGpuWork *source = job->gpu;
    if (!job->ok || !source || source->count >= UINT32_MAX - target->count) return 0;
    target->timing.phase_project_cpu_ns += job->project_cpu_ns;
    target->timing.phase_capture_cpu_ns += job->capture_cpu_ns;
    target->timing.phase_geometry_cpu_ns += job->geometry_cpu_ns;
    if (!s_native_motion_probe.target_vblank) {
        /* Transfer the completed immutable buffers to GL without copying tens
         * of megabytes back through the serial compile owner. */
        if (source->count > UINT32_MAX - target->phase_command_count ||
            target->chunk_count >= GL_NATIVE_MOTION_PHASE_CAPACITY) return 0;
        target->phase_chunks[target->chunk_count++] = (GlNativeGpuChunk){
            .commands = source->commands, .data = source->data,
            .count = source->count, .bytes = source->bytes, .geometry = source->geometry};
        target->phase_command_count += source->count;
        target->phase_bytes += source->bytes;
        source->commands = NULL; source->data = NULL;
        source->geometry = NULL;
        const uint32_t plane = GL_NATIVE_GPU_PHASE_BASE + job->phase;
        target->widths[plane] = source->widths[plane];
        target->heights[plane] = source->heights[plane];
        return 1;
    }
    const uint32_t required = target->count + source->count + 1u;
    if (required > target->capacity) {
        void *commands = xg_render_append_reserve(target->commands, sizeof(*target->commands),
            &target->capacity, target->count, required, UINT32_MAX, &target->retired_buffers);
        if (!commands) return 0;
        target->commands = commands;
    }
    /* Mesh triangles keep their 8-byte alignment in the merged blob. */
    static const uint8_t pad[8];
    if (source->bytes && (target->bytes & 7u)) {
        uint32_t ignored;
        if (!native_gpu_data(target, pad, 8u - (target->bytes & 7u), &ignored)) return 0;
    }
    uint32_t data_offset = target->bytes;
    if (source->bytes && !native_gpu_data(target, source->data, source->bytes, &data_offset)) return 0;
    /* Each private journal starts from the same raw-word shadow. Restore that
     * exact baseline before executing its patches, rather than depending on
     * a different phase's final texture state. One shared payload per batch. */
    if (job->phase) {
        GlNativeGpuCommand *reset = native_gpu_command(target, NATIVE_GPU_WORDS);
        if (!reset) return 0;
        reset->w = VRAM_W; reset->h = VRAM_H; reset->data = initial_words_offset;
    }
    GlNativeGpuCommand *commands = target->commands + target->count;
    memcpy(commands, source->commands, (size_t)source->count * sizeof(*commands));
    for (uint32_t i = 0u; i < source->count; ++i)
        if (commands[i].kind == NATIVE_GPU_WORDS || commands[i].kind == NATIVE_GPU_SEED ||
            commands[i].kind == NATIVE_GPU_SPANS ||
            (commands[i].kind == NATIVE_GPU_DRAW && commands[i].mesh_count))
            commands[i].data += data_offset;
    target->count += source->count;
    const uint32_t plane = GL_NATIVE_GPU_PHASE_BASE + job->phase;
    target->widths[plane] = source->widths[plane];
    target->heights[plane] = source->heights[plane];
    return 1;
}

static void native_phase_jobs_discard(GlNativePhaseJob *jobs, uint32_t count) {
    for (uint32_t i = 0u; i < count; ++i) {
        free(jobs[i].entities);
        if (jobs[i].gpu) {
            native_gpu_buffers_free(jobs[i].gpu);
            free(jobs[i].gpu->words);
            free(jobs[i].gpu);
        }
    }
}

/* Equal anchored endpoints make the alpha-dependent terms exactly zero.
 * Equal evaluated canonical/Native transforms then prove the replayed geometry
 * is identical at every phase, including clipping and depth. */
static int native_phase_entity_static(const GlNativeMotionEntity *entities,
        const GlNativePhaseJob *jobs, uint32_t phases, uint32_t entity) {
    if (!entities[entity].enabled) return 1;
    const XgRenderMotionEvaluation *first = &jobs[0].entities[entity].evaluation;
    const size_t bytes = offsetof(XgRenderMotionTransform, model_to_view) +
        first->node_count * sizeof(first->phase.model_to_view[0]);
    if (memcmp(&first->endpoints[0], &first->endpoints[1], bytes) ||
        memcmp(first->source_projection[0], first->source_projection[1],
            first->node_count * sizeof(first->source_projection[0][0]))) return 0;
    for (uint32_t phase = 1u; phase < phases; ++phase) {
        const XgRenderMotionEvaluation *other = &jobs[phase].entities[entity].evaluation;
        if (other->node_count != first->node_count ||
            memcmp(&first->phase, &other->phase, bytes) ||
            memcmp(&first->native_phase, &other->native_phase, bytes)) return 0;
    }
    return 1;
}

static GlNativePhaseTemplates *native_phase_templates_prepare(const GlNativeRecipe *recipe,
        const GlNativeMotionEntity *entities, const GlNativeVertexCache *vertices,
        const GlNativePhaseJob *jobs, uint32_t phases) {
    GlNativePhaseTemplates *cache = calloc(1u, sizeof(*cache));
    uint8_t *records = calloc(recipe->count ? recipe->count : 1u, 1u);
    uint8_t *stationary = calloc(recipe->motion_count ? recipe->motion_count : 1u, 1u);
    GlNativePhaseJob capture = jobs[0];
    capture.gpu = NULL;
    if (!cache || !records || !stationary) goto failed;
    cache->record_commands = malloc((recipe->count ? recipe->count : 1u) * sizeof(*cache->record_commands));
    if (!cache->record_commands) goto failed;
    for (uint32_t i = 0u; i < recipe->motion_count; ++i)
        stationary[i] = native_phase_entity_static(entities, jobs, phases, i);
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < recipe->count; ++i) {
        const GlNativeRecipeDraw *record = &recipe->draws[i];
        int fixed = record->semantic.topology == GPU_RENDER_SEMANTIC_TRIANGLES &&
            !record->hd_texture.valid && !record->semantic.native_view_effect;
        if (fixed && !record->motion.motion.handle.resource_id) {
            const uint32_t mesh = vertices->draw_mesh ? vertices->draw_mesh[i] : UINT32_MAX;
            fixed = mesh == UINT32_MAX || !vertices->meshes[mesh].enabled;
        } else if (fixed) {
            const uint32_t n = record->mesh && record->mesh->poses ? record->mesh->count : 1u;
            for (uint32_t t = 0u; t < n && fixed; ++t) {
                const uint32_t entity = record->mesh && record->mesh->poses
                    ? record->mesh->poses[t].motion_index : record->motion_index;
                fixed = entity < recipe->motion_count && stationary[entity];
            }
        }
        records[i] = fixed;
        count += fixed;
        cache->record_commands[i] = fixed ? UINT32_MAX - 1u : UINT32_MAX;
    }
    if (!count) goto failed;
    capture.capture_templates = 1;
    capture.static_records = records;
    native_phase_job_execute(&capture, NATIVE_PHASE_CAPTURE);
    if (!capture.ok || !capture.gpu || !capture.gpu->geometry) goto failed;
    for (uint32_t i = 0u; i < capture.gpu->count; ++i) {
        const GlNativeGpuCommand *command = &capture.gpu->commands[i];
        if (command->kind != NATIVE_GPU_DRAW || command->template_record >= recipe->count ||
            cache->record_commands[command->template_record] != UINT32_MAX - 1u) goto failed;
        cache->record_commands[command->template_record] = i;
    }
    cache->commands = capture.gpu->commands; capture.gpu->commands = NULL;
    cache->data = capture.gpu->data; capture.gpu->data = NULL;
    cache->geometry = capture.gpu->geometry; capture.gpu->geometry = NULL;
    cache->count = capture.gpu->count;
    const GlNativeMotionEndpoints *endpoints=jobs[0].endpoints;
    if(endpoints && endpoints->corner_vertices) {
        cache->phase_vertices_needed=calloc(endpoints->corner_count?endpoints->corner_count:1u,1u);
        if(cache->phase_vertices_needed)
            for(uint32_t i=0u;i<recipe->count;++i) {
                if(!recipe->draws[i].mesh || cache->record_commands[i]!=UINT32_MAX)continue;
                for(uint32_t c=endpoints->offsets[i];c<endpoints->offsets[i+1u];++c) {
                    const uint32_t u=endpoints->corner_vertices[c];
                    if(u!=UINT32_MAX)cache->phase_vertices_needed[u]=1u;
                }
            }
    }
    native_gpu_buffers_free(capture.gpu); free(capture.gpu);
    free(records); free(stationary);
    if (getenv("PSX_NATIVE_TEXTURE_PROFILE"))
        SDL_Log("Native geometry templates: %u of %u records", count, recipe->count);
    return cache;
failed:
    if (capture.gpu) { native_gpu_buffers_free(capture.gpu); free(capture.gpu->words); free(capture.gpu); }
    if (cache) { free(cache->record_commands); free(cache); }
    free(records); free(stationary);
    return NULL;
}

static int native_motion_layout_equal(const GlNativeMotionHistory *history, const GlNativeCompileAudit *audit) {
    const XgSemanticDisplayState *a = &history->display, *b = &audit->header.display;
    /* Display origins can alternate with double buffering. The pixel digest
     * describes the complete cropped image, not a particular VRAM page. */
    return history->valid && history->identity.presentation_epoch == audit->header.identity.presentation_epoch &&
        history->identity.scene_generation == audit->header.identity.scene_generation &&
        history->scene.disc_id == audit->header.scene.disc_id &&
        history->scene.executable_identity == audit->header.scene.executable_identity &&
        history->scene.primary_overlay_identity == audit->header.scene.primary_overlay_identity &&
        history->scene.companion_set_identity == audit->header.scene.companion_set_identity &&
        history->scene.authored_scene_id == audit->header.scene.authored_scene_id &&
        history->scene.authored_submode == audit->header.scene.authored_submode &&
        history->scene.module == audit->header.scene.module &&
        history->storage_width == audit->endpoint_storage_width &&
        a->width == b->width && a->height == b->height && a->aspect_num == b->aspect_num &&
        a->aspect_den == b->aspect_den && a->depth24 == b->depth24 && a->interlaced == b->interlaced &&
        a->disabled == b->disabled && a->native_width == b->native_width &&
        a->native_height == b->native_height && a->native_offset_x == b->native_offset_x &&
        a->temporal_hz == b->temporal_hz &&
        a->render_width == b->render_width && a->render_height == b->render_height &&
        a->dithering_disabled == b->dithering_disabled &&
        a->native_depth_test == b->native_depth_test &&
        (a->render_scale ? a->render_scale : 1u) == (b->render_scale ? b->render_scale : 1u);
}

static void native_motion_discard(GlNativeMotionPrepared *prepared) {
    for (uint32_t i = 0u; i < GL_NATIVE_MOTION_PHASE_CAPACITY; ++i) free(prepared->phases[i].pixels);
    native_recipe_release(prepared->history.recipe);
    memset(prepared, 0, sizeof(*prepared));
}

void native_motion_probe_discard(void) {
    GlNativeMotionProbe *probe = &s_native_motion_probe;
    native_recipe_release(probe->recipe); native_recipe_release(probe->previous_recipe);
    native_motion_vertices_discard(&probe->vertices);
    free(probe->entities); free(probe->projections); free(probe->commands); free(probe->previous_pixels);
    for (uint32_t i = 0u; i <= GL_NATIVE_MOTION_PHASE_CAPACITY; ++i) free(probe->readback_pixels[i]);
    memset(probe, 0, sizeof(*probe));
}

/* Meshes are rebuilt every source frame: equal by content, like semantic. */
static int native_recipe_mesh_equal(const GlNativeRecipeMesh *a, const GlNativeRecipeMesh *b) {
    if (a == b) return 1;
    if (!a || !b || a->count != b->count || a->relocated != b->relocated ||
        !a->textures != !b->textures ||
        memcmp(a->triangles, b->triangles, (size_t)a->count * sizeof(a->triangles[0]))) return 0;
    for (uint32_t t = 0u; a->textures && t < a->count; ++t)
        if (!native_texture_state_equal(&a->textures[t], &b->textures[t])) return 0;
    if (!a->poses != !b->poses) return 0;
    for (uint32_t t = 0u; a->poses && t < a->count; ++t)
        if (!native_recipe_pose_equal(&a->poses[t], &b->poses[t])) return 0;
    return 1;
}

static int native_motion_recipe_equal(const GlNativeRecipe *a, const GlNativeRecipe *b) {
    if (a == b) return 1; /* Retained recipes are immutable through COPY/COW. */
    if (!a || !b || a->count != b->count || a->motion_count != b->motion_count || a->coverage_count != b->coverage_count ||
        a->dithering_disabled != b->dithering_disabled ||
        a->depth_test != b->depth_test ||
        a->clear_color != b->clear_color || a->base_serial != b->base_serial ||
        a->width != b->width || a->height != b->height || a->view_width != b->view_width ||
        a->view_height != b->view_height || a->offset != b->offset ||
        (a->motion_count && memcmp(a->motions, b->motions, a->motion_count * sizeof(a->motions[0])))) return 0;
    for (uint32_t i = 0u; i < a->coverage_count; ++i)
        if (memcmp(&a->coverages[i].reference, &b->coverages[i].reference, sizeof(a->coverages[i].reference))) return 0;
    if (a->publication != b->publication) return 0;
    /* A wave animates over a still scene: its mesh is part of the pose. */
    if (!a->wave != !b->wave || a->wave_at != b->wave_at ||
        (a->wave && !native_wave_geometry_equal(a->wave, b->wave))) return 0;
    for (uint32_t i = 0u; i < a->count; ++i) {
        const GlNativeRecipeDraw *x = &a->draws[i], *y = &b->draws[i];
        /* Pixel equality alone cannot prove temporal equality. Include all
         * authored geometry/bindings, but not draw-time texture allocation IDs. */
        if (memcmp(&x->semantic, &y->semantic, sizeof(x->semantic)) ||
            memcmp(&x->motion, &y->motion, sizeof(x->motion)) ||
            x->projection_offset_x != y->projection_offset_x ||
            x->projection_offset_y != y->projection_offset_y ||
            x->motion_index != y->motion_index ||
            x->temporal_index != y->temporal_index || x->temporal_component != y->temporal_component ||
            x->enhanced != y->enhanced || x->view_origin_y != y->view_origin_y ||
            x->dither_x != y->dither_x || x->dither_y != y->dither_y ||
            memcmp(&x->hd_texture, &y->hd_texture, sizeof(x->hd_texture)) ||
            !native_recipe_mesh_equal(x->mesh, y->mesh)) return 0;
    }
    return 1;
}

static void native_motion_cost_update(unsigned stage, uint64_t elapsed, uint64_t draws) {
    if (!draws || !elapsed) return;
    const uint64_t sample = elapsed / draws ? elapsed / draws : 1u;
    uint64_t *estimate = &s_native_phase_cost_per_draw[stage];
    if (!*estimate) *estimate = sample;
    else if (sample > *estimate) *estimate += (sample - *estimate) / 8u;
    else *estimate -= (*estimate - sample) / 8u;
}

static void native_motion_prepare(GlNativeCompileAudit *audit, GlNativeViewState *views,
                               const uint32_t *canonical, const uint32_t *endpoint_pixels,
                               GlNativeMotionPrepared *prepared, int deadline_known, uint64_t deadline_ns) {
    /* Diagnostic workload control only: compare the full interpolated workload
     * without adaptive phase counts hiding GPU throughput changes. The actual
     * presenter deadlines are unaffected; this does not establish displayed FPS. */
    const char *benchmark_phases = getenv("PSX_NATIVE_BENCH_ALL_PHASES");
    if (benchmark_phases && benchmark_phases[0] == '1') deadline_known = 0;
    const XgRenderSourceCommitHeader *header = &audit->header;
    const uint64_t prepare_started = SDL_GetPerformanceCounter();
    const uint16_t temporal_hz = header->display.temporal_hz;
    const GlNativeMotionHistory *old = &s_native_motion_history;
    GlNativeVertexCache vertices = {0};
    GlNativeVertexDiagnostics vertex_diag = {.vblank = header->identity.guest_vblank_sequence};
    GlNativeMotionEntity *entities = NULL;
    GlNativeMotionProjection *projections = NULL;
    GlNativeMotionEndpoints endpoints = {0};
    GlNativePhaseJob phase_jobs[GL_NATIVE_MOTION_PHASE_CAPACITY] = {0};
    uint32_t phase_job_count = 0u, phase_initial_words_offset = 0u;
    int phase_capture_active = 0;
    GlNativeRecipe *phase_recipe = NULL;
    uint32_t *check = NULL;
    const uint32_t gpu_checkpoint = views->gpu ? views->gpu->count : 0u;
    prepared->history = (GlNativeMotionHistory){.identity = header->identity, .scene = header->scene, .display = header->display,
        .pixel_digest = audit->endpoint_pixel_digest, .reference_digest = audit->endpoint_pixel_digest,
        .storage_width = audit->endpoint_storage_width, .valid = 1};
    {
        const int shown = native_view_cover(views, views->count, header->display.display_x,
            header->display.display_y, header->display.width, header->display.height);
        if (shown >= 0) views->targets[shown].displayed = 1;
    }
    if (!temporal_hz) {
        /* OFF still executes every source operation and the Native geometry.
         * It needs neither recipe replay nor pose evaluations/phase buffers. */
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_SMOOTHING_DISABLED;
        return;
    }
    if (header->display.disabled || header->display.depth24 || header->scene.module == XG_SEMANTIC_MODULE_MOVIE) {
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_DISCRETE_DISPLAY;
        return;
    }
    audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_NO_RECIPE;
    const int index = native_view_cover(views, views->count, header->display.display_x,
        header->display.display_y, header->display.width, header->display.height);
    if (index < 0) return;
    GlNativeViewTarget *target = &views->targets[index];
    GlNativeRecipe *recipe = target->recipe;
    if (!recipe || target->x != header->display.display_x || target->width != header->display.width ||
        (recipe->view_width ? recipe->view_width : recipe->width) != audit->endpoint_storage_width) return;
    const uint16_t crop_y = header->display.display_y - target->y;
    const size_t pixel_bytes = (size_t)audit->endpoint_storage_width * header->display.height * sizeof(uint32_t);
    /* Optional exhaustive pixel audit. Normal play already rasterized canonical
     * VRAM and VIEW during FIFO execution; it only needs the actual phase here.
     * Construction/ownership/dependency checks remain unconditional. */
    GlNativeRecipeReplay replays[2] = {
        {.recipe = recipe, .crop_y = crop_y, .height = header->display.height},
        {.recipe = recipe, .crop_y = crop_y, .height = header->display.height, .use_view = 1}};
    SDL_Thread *replay_threads[2] = {0};
    const uint32_t replay_count = s_native_recipe_audit_enabled ? (recipe->view_width ? 2u : 1u) : 0u;
    for (uint32_t i = 0u; i < replay_count; ++i) {
        replay_threads[i] = SDL_CreateThread(native_recipe_replay_thread, "native-endpoint", &replays[i]);
        if (!replay_threads[i]) (void)native_recipe_replay_thread(&replays[i]);
    }
    recipe->references++;
    prepared->history.recipe = recipe; prepared->history.crop_y = crop_y;
    if (!views->reset_motion_history && !header->discontinuity &&
        native_motion_layout_equal(old, audit) && old->crop_y == crop_y &&
        old->reference_digest == prepared->history.reference_digest &&
        native_motion_recipe_equal(old->recipe, recipe)) {
        /* A duplicate candidate still needs its actual GPU image classification,
         * but an identical authored temporal recipe needs no phase replay. */
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_STATIC_POSE;
        goto finished;
    }
    /* Keep the new logical endpoint/poses even when its visual phase budget
     * has expired. Never reanchor a FIFO deadline to compile completion. */
    if (deadline_known && (!deadline_ns || SDL_GetTicksNS() >= deadline_ns)) {
        audit->temporal_status = deadline_ns ? GL_RENDERER_NATIVE_TEMPORAL_DEADLINE_EXPIRED
                                            : GL_RENDERER_NATIVE_TEMPORAL_WHOLE_ONLY;
        goto finished;
    }
    audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_NO_HISTORY;
    if (views->reset_motion_history || header->discontinuity || !old->recipe ||
        !native_motion_layout_equal(old, audit) || old->crop_y != crop_y ||
        old->recipe->width != recipe->width || old->recipe->height != recipe->height ||
        header->identity.guest_cycle <= old->identity.guest_cycle) goto finished;
    /* Guest CPU time, not worker latency or an invented host deadline. */
    const uint64_t cycles = header->identity.guest_cycle - old->identity.guest_cycle;
    const uint64_t seconds = cycles / UINT64_C(33868800);
    if (seconds >= UINT64_MAX / UINT64_C(1000000000)) goto finished;
    /* Non-const: partial publish (below) shrinks it to the rendered prefix
     * so the presenter maps phases over the covered sub-interval. */
    uint64_t interval = seconds * UINT64_C(1000000000) +
        (cycles % UINT64_C(33868800)) * UINT64_C(1000000000) / UINT64_C(33868800);
    uint32_t denominator = native_phase_denominator(
        interval, temporal_hz, GL_NATIVE_MOTION_PHASE_CAPACITY + 1u);
    /* Request the actual cadence on every source. A late source must not leave
     * later sources capped at one phase for seconds. Estimate this batch from
     * measured computation cost and its remaining accepted deadline instead. */
    if (deadline_known && s_native_phase_cost_per_draw[1] && s_native_phase_cost_per_draw[2]) {
        const uint64_t now_ns = SDL_GetTicksNS();
        const uint64_t margin = 2u * (UINT64_C(1000000000) / temporal_hz);
        const uint64_t fixed = s_native_phase_cost_per_draw[0] * recipe->count;
        const uint64_t per_phase = (s_native_phase_cost_per_draw[1] +
            s_native_phase_cost_per_draw[2]) * recipe->count;
        const uint64_t available = now_ns < deadline_ns ? deadline_ns - now_ns : 0u;
        const uint64_t fit = per_phase && available > margin + fixed
            ? (available - margin - fixed) / per_phase : 1u;
        const uint64_t budget_denom = fit ? fit + 1u : 2u;
        if (denominator > budget_denom) denominator = (uint32_t)budget_denom;
    }
    /* Non-const: a tick deadline can time-box generation to fewer phases
     * (partial publish below reassigns both to the rendered prefix). */
    uint32_t phase_count = denominator - 1u;
    s_native_phase_budget = phase_count;
    if (!phase_count) {
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_SOURCE_CADENCE;
        goto finished;
    }
    audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_ALLOCATION;
    const uint64_t fixed_started = SDL_GetTicksNS();
    if (!native_motion_departures(old->recipe, recipe, &phase_recipe)) goto finished;
    if (phase_recipe) recipe = phase_recipe;
    entities = calloc(recipe->motion_count ? recipe->motion_count : 1u, sizeof(*entities));
    audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_ALLOCATION;
    if (!entities) goto finished;
    if (!native_motion_vertices(old->recipe, recipe, phase_count, &vertices, &vertex_diag)) goto finished;
    native_motion_entities(old->recipe, recipe, &header->identity, &vertices, entities);
    /* Every enabled draw writes its result and projected coordinates before
     * replay reads them. Discrete/unbound draws never read these entries. */
    projections = malloc((size_t)phase_count * (recipe->count ? recipe->count : 1u) * sizeof(*projections));
    if (!projections) goto finished;
    const uint64_t preflight_started = SDL_GetTicksNS();
    native_motion_cost_update(0u, preflight_started - fixed_started, recipe->count);
    if (views->gpu && views->gpu->scanout) {
        /* Evaluate endpoint curves once. Copies retain those exact curves and
         * source GTE anchors; workers only advance/project their own phase. */
        native_motion_evaluate_entities(recipe, entities, 1.0 / denominator, 0, &audit->motion_evaluations);
        /* Every corner's endpoint projection and anchors are the same in all
         * phases: compute them once, split across the workers, instead of
         * again in each phase's thread. */
        endpoints.offsets = malloc(((size_t)recipe->count + 1u) * sizeof(*endpoints.offsets));
        if (!endpoints.offsets) goto finished;
        uint32_t corners = 0u;
        for (uint32_t i = 0u; i < recipe->count; ++i) {
            endpoints.offsets[i] = corners;
            const GlNativeRecipeDraw *draw = &recipe->draws[i];
            if (draw->motion.motion.handle.resource_id) {
                const uint32_t triangles = draw->mesh ? draw->mesh->count : draw->motion.triangle_count;
                if (triangles > (UINT32_MAX - corners) / 3u) goto finished;
                corners += triangles * 3u;
            }
        }
        endpoints.offsets[recipe->count] = corners;
        endpoints.corner_count = corners;
        endpoints.range_count = phase_count;
        endpoints.corners = malloc((corners ? corners : 1u) * sizeof(*endpoints.corners));
        endpoints.corner_vertices = malloc((corners ? corners : 1u) * sizeof(*endpoints.corner_vertices));
        endpoints.vertices = malloc((corners ? corners : 1u) * sizeof(*endpoints.vertices));
        endpoints.vertex_endpoints = malloc((corners ? corners : 1u) * sizeof(*endpoints.vertex_endpoints));
        if (!endpoints.corners || !endpoints.corner_vertices || !endpoints.vertices ||
            !endpoints.vertex_endpoints) goto finished;
        phase_job_count = phase_count;
        const size_t entity_bytes = (size_t)recipe->motion_count * sizeof(*entities);
        for (uint32_t phase = 0u; phase < phase_count; ++phase) {
            GlNativePhaseJob *job = &phase_jobs[phase];
            *job = (GlNativePhaseJob){.recipe = recipe, .raster_source = views->gpu, .vertices = &vertices,
                .render_entities = entities, .projections = projections + (size_t)phase * recipe->count,
                .endpoints = &endpoints,
                .initial_words = views->gpu->words, .identity = views->gpu->identity,
                .phase = phase, .denominator = denominator, .crop_y = crop_y,
                .height = header->display.height, .scale = views->gpu->scale,
                .depth_test = views->gpu->depth_test, .depth_view = views->gpu->depth_view,
                .wireframe = views->gpu->wireframe};
            job->entities = malloc(entity_bytes ? entity_bytes : sizeof(*entities));
            if (!job->entities) goto finished;
        }
        /* Split by corners: one mesh record can hold most of them. */
        for (uint32_t phase = 0u, record = 0u; phase < phase_count; ++phase) {
            const uint64_t limit = (uint64_t)corners * (phase + 1u) / phase_count;
            phase_jobs[phase].endpoint_begin = record;
            while (record < recipe->count && (phase + 1u == phase_count || endpoints.offsets[record] < limit))
                ++record;
            phase_jobs[phase].endpoint_end = record;
        }
        native_phase_run(phase_jobs, phase_count, NATIVE_PHASE_ENDPOINTS);
        native_phase_run(phase_jobs, phase_count, NATIVE_PHASE_PREFLIGHT);
        /* Keep entity-wide rejection atomic across the entire interval, as in
         * serial preflight: one invalid phase disables it in every replay. */
        for (uint32_t phase = 0u; phase < phase_count; ++phase) {
            audit->motion_evaluations += phase_jobs[phase].evaluations;
            for (uint32_t i = 0u; i < recipe->motion_count; ++i)
                entities[i].enabled &= phase_jobs[phase].entities[i].enabled;
        }
    } else {
        for (uint32_t phase = 0u; phase < phase_count; ++phase)
            native_motion_preflight(recipe, entities, (double)(phase + 1u) / denominator, phase != 0u,
                projections + (size_t)phase * recipe->count, &audit->motion_evaluations);
    }
    const uint64_t replay_started = SDL_GetTicksNS();
    /* Wall time includes worker completion; parent thread CPU time excludes
     * their work and would underestimate the deadline budget after splitting. */
    native_motion_cost_update(1u, replay_started - preflight_started,
        (uint64_t)recipe->count * phase_count);
    uint32_t active = vertex_diag.active_meshes;
    for (uint32_t i = 0u; i < recipe->motion_count; ++i) active += entities[i].enabled != 0;
    if (!active) {
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_SOURCE_STATE_REQUIRED;
        goto finished;
    }
    const uint64_t phase_loop_started = SDL_GetPerformanceCounter();
    /* Pipeline margin: stop rendering ~2 ticks before the deadline so the
     * batch publishes while ticks are still early (phases display) instead
     * of at/past it (every tick due-whole). Phase 0 keeps the raw deadline
     * (nothing to publish yet); light batches finish before the margin
     * anyway. */
    uint64_t phase_stop_ns = 0u;
    int have_phase_stop = 0;
    if (deadline_known && temporal_hz != 0u) {
        const uint64_t tick_ns = UINT64_C(1000000000) / temporal_hz;
        const uint64_t margin_ns = 2u * tick_ns;
        phase_stop_ns = deadline_ns > margin_ns ? deadline_ns - margin_ns : 0u;
        have_phase_stop = 1;
    }
    if (phase_job_count) {
        if (deadline_known && SDL_GetTicksNS() >= deadline_ns) {
            audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_DEADLINE_EXPIRED;
            goto phases_failed;
        }
        if (s_native_motion_probe.target_vblank && !native_gpu_data(views->gpu, views->gpu->words,
            (size_t)VRAM_W * VRAM_H * sizeof(*views->gpu->words), &phase_initial_words_offset)) goto phases_failed;
        const char *shared_textures = getenv("PSX_NATIVE_SHARED_PHASE_TEXTURES");
        if ((!shared_textures || shared_textures[0] != '0') && !s_native_motion_probe.target_vblank) {
            views->gpu->phase_textures = native_phase_textures_prepare(recipe);
            views->gpu->owns_phase_textures = views->gpu->phase_textures != NULL;
            for (uint32_t phase = 0u; phase < phase_count; ++phase)
                phase_jobs[phase].phase_textures = views->gpu->phase_textures;
        }
        const char *static_geometry = getenv("PSX_NATIVE_STATIC_PHASE_GEOMETRY");
        if ((!static_geometry || static_geometry[0] != '0') && views->gpu->phase_textures &&
            !s_native_motion_probe.target_vblank && phase_count > 1u &&
            !views->gpu->wireframe && !views->gpu->depth_view) {
            views->gpu->phase_templates = native_phase_templates_prepare(recipe, entities, &vertices,
                phase_jobs, phase_count);
            views->gpu->owns_phase_templates = views->gpu->phase_templates != NULL;
            for (uint32_t phase = 0u; phase < phase_count; ++phase)
                phase_jobs[phase].phase_templates = views->gpu->phase_templates;
        }
        phase_capture_active = native_phase_begin(phase_jobs, phase_count, NATIVE_PHASE_CAPTURE);
    }
    for (uint32_t phase = 0u; phase < phase_count; ++phase) {
        /* Publish the next FIFO phase as soon as it is captured, while later
         * phases continue on the other CPU workers and GL executes this one. */
        if (phase_capture_active) native_phase_wait_job(&phase_jobs[phase]);
        const uint64_t phase_limit_ns =
            (phase == 0u || !have_phase_stop) ? deadline_ns : phase_stop_ns;
        if (deadline_known && SDL_GetTicksNS() >= phase_limit_ns) {
            if (phase == 0u) {
                audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_DEADLINE_EXPIRED;
                goto phases_failed;
            }
            /* Time-boxed generation: publish the phases already rendered
             * instead of discarding all work. They cover the first
             * phase/phase_count of the interval (fractions were preflighted
             * against the full denominator), so shrink the interval to
             * match — the presenter maps phases uniformly over it and
             * holds the whole after. Strictly better than whole-only:
             * at 240 Hz ticks a 7-phase batch rarely fits its deadline,
             * and discarding 4 rendered phases produced the all-whole
             * output (phased 0/s) plus the wasted renders. */
            phase_count = phase;
            interval = interval * (uint64_t)phase / denominator;
            s_native_phase_budget = phase;
            break;
        }
        /* Preflight validated every phase before enabling any entity. Replay
         * consumes those exact projections, without reevaluating the hierarchy. */
        for (uint32_t i = 0u; i < recipe->motion_count; ++i)
            entities[i].projections = projections + (size_t)phase * recipe->count;
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_REPLAY_FAILED;
        /* Only the presented phase plane is needed. Canonical endpoint VRAM
         * remains untouched; the optional audit checks the endpoint separately. */
        if (views->gpu && views->gpu->scanout) {
            GlNativePhaseJob *job = &phase_jobs[phase];
            if (!native_phase_merge(views->gpu, job, phase_initial_words_offset)) {
                if (job->stats.temporal_status) audit->temporal_status = job->stats.temporal_status;
                goto phases_failed;
            }
            audit->motion_projected_draws += job->stats.motion_projected_draws;
            /* The private journals are complete. GL can consume the ordered
             * prefix while the owner merges the remaining phases. */
            native_gpu_publish_prefix(views->gpu, 0);
        } else {
            if (!native_recipe_render(recipe, entities, &vertices, phase, recipe->view_width != 0u,
                    audit, crop_y, header->display.height, &check)) goto phases_failed;
            prepared->phases[phase].pixels = check; check = NULL;
            /* Per-phase digests have no consumer (chain/history use the base
             * and whole digests); hashing full frames xN per batch just to
             * discard costs ~1/3 of the phase loop. Audit builds keep it. */
            if (s_native_recipe_audit_enabled)
                prepared->phases[phase].digest = pres_hash_bytes((const uint8_t *)prepared->phases[phase].pixels, pixel_bytes);
            else
                prepared->phases[phase].digest = 0u;
            prepared->phases[phase].upload_state = GL_NATIVE_ENDPOINT_UPLOAD_STAGED;
        }
    }
    prepared->count = phase_count;
    if (phase_count > 0u) {
        native_motion_cost_update(2u, SDL_GetTicksNS() - replay_started,
            (uint64_t)recipe->count * phase_count);
        const uint64_t phase_loop_ended = SDL_GetPerformanceCounter();
        const uint64_t freq = SDL_GetPerformanceFrequency();
        if (freq != 0u && phase_loop_ended >= phase_loop_started) {
            audit->phase_gen_ns += (phase_loop_ended - phase_loop_started) *
                UINT64_C(1000000000) / freq;
            audit->phase_gen_count += phase_count;
        }
    }
    if (views->gpu) {
        views->gpu->phase_count = phase_count;
        views->gpu->phase_crop_y = recipe->origin_y + crop_y;
        views->gpu->phase_crop_x = recipe->view_width ? 0u : header->display.display_x;
    }
    prepared->interval_ns = interval; prepared->previous_digest = old->pixel_digest;
    audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_READY;
    audit->temporal_phase_count = prepared->count; audit->temporal_interval_ns = interval;
    const uint64_t vblanks = header->identity.guest_vblank_sequence - old->identity.guest_vblank_sequence;
    audit->temporal_interval_vblanks = vblanks <= UINT32_MAX ? (uint32_t)vblanks : 0u;
    audit->endpoint_was_discrete = 0;
    goto finished;
phases_failed:
    native_gpu_discard_phases(views->gpu, gpu_checkpoint);
    free(check); check = NULL;
    for (uint32_t i = 0u; i < GL_NATIVE_MOTION_PHASE_CAPACITY; ++i) {
        free(prepared->phases[i].pixels); prepared->phases[i].pixels = NULL;
    }
finished:
    if (phase_capture_active) native_phase_finish();
    native_phase_jobs_discard(phase_jobs, phase_job_count);
    for (uint32_t i = 0u; i < replay_count; ++i)
        if (replay_threads[i]) SDL_WaitThread(replay_threads[i], NULL);
    if (replay_count) {
        audit->recipe_validation_performed = 1;
        int exact = replays[0].ok;
        for (uint32_t y = 0u; y < header->display.height && exact; ++y)
            for (uint32_t x = 0u; x < header->display.width; ++x)
                if (replays[0].pixels[(size_t)y * header->display.width + x] != (UINT32_C(0xff000000) |
                    canonical[(size_t)(header->display.display_y + y) * VRAM_W + header->display.display_x + x])) {
                    exact = 0; break;
                }
        audit->recipe_canonical_match = exact;
        audit->recipe_view_match = !recipe->view_width ||
            (replays[1].ok && memcmp(replays[1].pixels, endpoint_pixels, pixel_bytes) == 0);
        if (!exact || !audit->recipe_view_match) {
            native_gpu_discard_phases(views->gpu, gpu_checkpoint);
            audit->temporal_status = !replays[0].ok || (recipe->view_width && !replays[1].ok)
                ? GL_RENDERER_NATIVE_TEMPORAL_REPLAY_FAILED : GL_RENDERER_NATIVE_TEMPORAL_REPLAY_MISMATCH;
            native_recipe_release(prepared->history.recipe); prepared->history.recipe = NULL;
            for (uint32_t i = 0u; i < GL_NATIVE_MOTION_PHASE_CAPACITY; ++i) {
                free(prepared->phases[i].pixels); prepared->phases[i].pixels = NULL;
            }
            prepared->count = 0u;
            prepared->interval_ns = 0u;
            prepared->previous_digest = 0u;
            audit->temporal_phase_count = 0u;
            audit->temporal_interval_ns = 0u;
            audit->temporal_interval_vblanks = 0u;
            audit->endpoint_was_discrete = 1;
        }
    }
    if (vertices.pairs) {
        vertex_diag.phases = prepared->count;
        vertex_diag.prepare_ns = (uint64_t)((SDL_GetPerformanceCounter() - prepare_started) *
            (1000000000.0 / SDL_GetPerformanceFrequency()));
        s_native_vertex_events[s_native_vertex_event_total++ % 64u] = vertex_diag;
        if (vertex_diag.active_meshes) s_native_vertex_phase_total += prepared->count;
    }
    for (uint32_t i = 0u; i < replay_count; ++i) free(replays[i].pixels);
    if (s_native_motion_probe.target_vblank == header->identity.guest_vblank_sequence &&
        !s_native_motion_probe.recipe && prepared->count && views->gpu && views->gpu->scanout) {
        GlNativeMotionProbe *probe = &s_native_motion_probe;
        probe->identity = header->identity; probe->previous_identity = old->identity;
        probe->recipe = recipe; recipe->references++;
        probe->previous_recipe = old->recipe; old->recipe->references++;
        probe->vertices = vertices; memset(&vertices, 0, sizeof(vertices));
        probe->entities = entities; entities = NULL;
        probe->projections = projections; projections = NULL;
        probe->first_command = gpu_checkpoint; probe->phase_count = prepared->count;
        probe->scanout_height = header->display.height;
    }
    native_motion_vertices_discard(&vertices); free(projections); free(entities);
    free(endpoints.offsets); free(endpoints.corners); free(endpoints.corner_vertices);
    free(endpoints.vertices); free(endpoints.vertex_endpoints);
    native_recipe_release(phase_recipe);
}

static int native_compile_native_work(XgRenderSourceCommitHandle commit,
                                  GlNativeCompileAudit *audit,
                                  uint32_t **out_vram, uint16_t **out_words, uint32_t **out_pixels,
                                  GlNativeViewState *views, GlNativeGpuWork *gpu,
                                  const GlNativeViewState *base_views,
                                  const uint32_t *base_vram, const uint16_t *base_words,
                                  const XgPresentationIdentity *base_identity) {
    const size_t canvas_bytes = (size_t)VRAM_W * VRAM_H * sizeof(uint32_t);
    const size_t word_bytes = (size_t)VRAM_W * VRAM_H * sizeof(uint16_t);
    const XgPresentationIdentity *identity = &audit->header.identity;
    const XgSemanticDisplayState captured_display = audit->header.display;
    const XgSemanticDisplayState *display = &captured_display;
    GlNativeCpuCompiler compiler = {0};
    GlNativeResourceRecord texture = {0}, clut;
    GlNativeCpuSurface canvas = {0};
    uint32_t *copy_pixels = NULL;
    uint16_t *copy_words = NULL;
    uint32_t *endpoint_pixels = NULL;
    GlNativeRecipe *copy_recipes[GL_NATIVE_VIEW_TARGET_CAPACITY] = {0};
    uint32_t copy_recipe_count = 0u;
    uint32_t operation_index = 0u;
    *out_vram = NULL;
    *out_words = NULL;
    *out_pixels = NULL;
    native_views_copy(views, base_views);
    /* A speculative base will have committed/reset its history before this
     * successor is promoted. Its one-shot reset flag is not inherited. */
    if (base_views != &s_native_views) views->reset_motion_history = 0;
    if (views->publication) views->publication->references++;
    views->gpu = gpu;
    views->owned = 0u;
    for (uint32_t i = 0u; i < views->count; ++i)
        if (views->targets[i].recipe) views->targets[i].recipe->references++;
    audit->all_records_consumed = 0;
    if (audit->blocker != GL_RENDERER_NATIVE_BLOCKER_NONE) return 0;
    if (display->render_scale > GL_MAX_INTERNAL_SCALE) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_DISPLAY, 0u, 0u, 0u);
        goto failed;
    }
    if ((display->native_width || display->native_height || display->native_offset_x) &&
        (!display->native_width || !display->native_height ||
         display->native_width > VRAM_W ||
         display->native_width <= 2u * display->native_offset_x)) goto failed;
    if (views->width != display->native_width ||
        views->reference_height != display->native_height ||
        views->offset != display->native_offset_x ||
        views->depth_test != (int)display->native_depth_test) {
        /* Layout changes preserve declarations, but not old projection pixels.
         * A later TARGET/draw seeds only the center from our own device plane. */
        views->reset_motion_history = 1;
        for (uint32_t i = 0u; i < views->count; ++i) {
            views->targets[i].pixels = NULL;
            views->targets[i].transition_snapshot = 0;
            native_wave_capture_reset(&views->targets[i].wave);
            native_recipe_drop(&views->targets[i]);
        }
        /* Different horizontal projection: staging drops domain references,
         * while the committed allocations stay alive until publication. */
        memset(views->domains, 0, sizeof(views->domains));
        views->domain_count = 0u;
        views->readback_rgb_digest = 0u;
        views->readback_capture = 0;
    }
    views->width = display->native_width;
    views->reference_height = display->native_height;
    views->offset = display->native_offset_x;
    /* Toggling the option resets the domains above: allocation layout differs. */
    views->depth_test = display->native_depth_test ? 1 : 0;
    if (views->temporal_hz != display->temporal_hz) views->reset_motion_history = 1;
    views->temporal_hz = display->temporal_hz;
    if (!native_recipe_tracking(views))
        for (uint32_t i = 0u; i < views->count; ++i) native_recipe_drop(&views->targets[i]);
    if (audit->header.discontinuity) views->reset_motion_history = 1;
    if (identity->source_sequence == 0u || identity->presentation_epoch == 0u ||
        (base_identity->source_sequence != 0u &&
         (identity->presentation_epoch < base_identity->presentation_epoch ||
          (identity->presentation_epoch == base_identity->presentation_epoch &&
           identity->source_sequence <= base_identity->source_sequence))))
        goto failed;
    if (audit->header.display_boundary &&
        (!display->width || !display->height || display->width > VRAM_W ||
         display->height > VRAM_H || display->display_x >= VRAM_W ||
         display->display_y >= VRAM_H || !display->aspect_num || !display->aspect_den)) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_DISPLAY, 0u, 0u, 0u);
        goto failed;
    }
    /* The compile owner also owns speculative successors. A spare is private
     * storage there too: compare its versions against the actual predecessor,
     * rather than copying the full VRAM just because that base is staged. */
    const int reuse_canvas = s_native_spare_vram && s_native_spare_words && base_vram;
    if (reuse_canvas) {
        canvas.pixels = s_native_spare_vram; s_native_spare_vram = NULL;
        canvas.words = s_native_spare_words; s_native_spare_words = NULL;
        /* Version writes cover every canonical raster/transfer mutation. Keep
         * adjacent changed blocks together, without copying untouched textures
         * and offscreen VRAM on every display-only or small transaction. */
        for (unsigned y = 0u; y < VRAM_H; ++y) {
            for (unsigned block = 0u; block < GPU_VRAM_REGION_WORDS_PER_ROW;) {
                if (s_native_spare_versions[y][block] == base_views->word_versions[y][block]) {
                    ++block; continue;
                }
                const unsigned first = block++;
                while (block < GPU_VRAM_REGION_WORDS_PER_ROW &&
                       s_native_spare_versions[y][block] != base_views->word_versions[y][block]) ++block;
                const size_t offset = (size_t)y * VRAM_W + first * 64u;
                const size_t count = (block - first) * 64u;
                memcpy(canvas.pixels + offset, base_vram + offset, count * sizeof(*canvas.pixels));
                memcpy(canvas.words + offset, base_words + offset, count * sizeof(*canvas.words));
            }
        }
    } else {
        canvas.pixels = (uint32_t *)malloc(canvas_bytes);
        canvas.words = (uint16_t *)malloc(word_bytes);
        if (!canvas.pixels || !canvas.words) goto allocation_failed;
        if (base_vram) {
            memcpy(canvas.pixels, base_vram, canvas_bytes);
            memcpy(canvas.words, base_words, word_bytes);
        } else {
            memset(canvas.pixels, 0, canvas_bytes);
            memset(canvas.words, 0, word_bytes);
        }
    }
    if(gpu) {
        /* Seed from this FIFO batch's own canonical input, not the live guest.
         * Subsequent draw-time patches still capture every intervening change. */
        if(!native_gpu_data(gpu,canvas.words,word_bytes,&gpu->initial_words_offset))goto allocation_failed;
        memcpy(gpu->words,canvas.words,word_bytes);
    }
    texture.view_valid = 1;
    texture.view.kind = XG_RENDER_RESOURCE_FRAMEBUFFER;
    texture.view.bytes = canvas.words;
    texture.view.byte_count = word_bytes;
    texture.view.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = VRAM_W, .height = VRAM_H, .row_pitch = VRAM_W * 2u,
        .vram_width = VRAM_W, .vram_height = VRAM_H,
        .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
        .wrap_u = XG_RENDER_RESOURCE_WRAP_REPEAT,
        .wrap_v = XG_RENDER_RESOURCE_WRAP_REPEAT,
        .flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION,
    };
    clut = texture;
    clut.view.kind = XG_RENDER_RESOURCE_CLUT;
    clut.view.descriptor.pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555;
    canvas.resource = &texture;
    canvas.width = VRAM_W; canvas.height = VRAM_H; canvas.valid = 1;
    compiler.audit = audit;
    compiler.dithering_disabled = display->dithering_disabled;
    compiler.native_vram = &canvas;
    compiler.native_words = canvas.words;
    compiler.raster_words = native_recipe_tracking(views) ? &views->raster_words : NULL;
    compiler.native_texture = &texture;
    compiler.native_clut = &clut;
    compiler.viewport_width = VRAM_W;
    compiler.viewport_height = VRAM_H;
    compiler.gpu = gpu;
    compiler.native_views = views;
    /* Native VIEW planes are GPU-owned at every internal scale, including 1x.
     * Host meshes and DMA-aperture enhancements have no CPU raster equivalent.
     * Keep the software VIEW only for CPU presentation and pixel audits. */
    const int view_cpu = !gpu ||
        s_native_recipe_audit_enabled || gl_renderer_native_guest_reference_enabled();
    if (gpu) {
        if (!native_gpu_seed(gpu, 0u, VRAM_W, VRAM_H, canvas.pixels, NULL)) goto allocation_failed;
        for (uint32_t i = 0u; i < views->domain_count; ++i) {
            if (!views->domains[i].pixels) continue;
            const int seeding = !gpu->widths[i+1u] && (gpu->fresh || !gpu->base_planes[i+1u]);
            /* A seed resets depth; never mutate the committed domain for it. */
            if ((views->depth_test || !view_cpu) && seeding &&
                !native_view_domain_private(views, i, NULL)) goto allocation_failed;
            if (seeding && !view_cpu) {
                /* Without the CPU VIEW raster, a new GPU plane starts from the
                 * canonical frame in the centre (margins as last recorded). */
                GlNativeViewDomain *domain = &views->domains[i];
                for (uint32_t y = 0u; y < VRAM_H; ++y)
                    if (domain->initialized[y])
                        memcpy(domain->pixels + (size_t)y * views->width + views->offset,
                            canvas.pixels + (size_t)y * VRAM_W + domain->x,
                            (size_t)domain->width * sizeof(uint32_t));
            }
            if (!native_gpu_seed(gpu, i+1u, views->width, VRAM_H,
                views->domains[i].pixels, native_view_domain_depth(views, &views->domains[i])))
                goto allocation_failed;
        }
    }

    uint32_t publication_index = 0u;
    for (; operation_index <= audit->header.native_operation_count; ++operation_index) {
        while (publication_index < audit->consumed_temporal_publications &&
            audit->temporal_publications[publication_index].before_operation == operation_index) {
            if (!native_coverage_publish(views, audit->temporal_publications[publication_index].coverage)) goto allocation_failed;
            audit->applied_temporal_publications++;
            publication_index++;
        }
        if (operation_index == audit->header.native_operation_count) break;
        /* The preceding operation is complete. Published command/data ranges
         * never move or coalesce again while the GL owner consumes them. */
        native_gpu_publish_prefix(gpu,0);
        XgRenderNativeOperation operation;
        GlNativeResourceRecord upload = {0};
        if (xg_render_source_commit_copy_native_operation(
                commit, operation_index, &operation) != XG_RENDER_SOURCE_COMMIT_OK)
            goto failed;
        audit->record_digest = native_audit_bytes(
            audit->record_digest, &operation, sizeof(operation));
        audit->consumed_native_operations++;
        if (views->write_serial == UINT64_MAX) goto failed;
        views->write_serial++;
        if (operation.kind != XG_RENDER_NATIVE_OPERATION_DRAW &&
            operation.kind != XG_RENDER_NATIVE_OPERATION_MESH &&
            !native_gpu_canonical_sync(&compiler)) goto failed;
        if (operation.kind == XG_RENDER_NATIVE_OPERATION_MESH) {
            /* Host geometry: the VIEW/phase GPU planes only, never guest VRAM
             * or a CPU surface; one command per raster run, one recipe draw. */
            const XgRenderNativeMeshInstance *instance;
            const XgRenderNativeMeshGeometry *geometry;
            XgSemanticDrawRecord draw;
            if (!xg_render_native_mesh_instance_view(operation.mesh, &instance) ||
                !xg_render_native_mesh_geometry_view(operation.mesh_geometry, &geometry) ||
                !native_materialize_native_draw(&operation.semantic, &draw)) goto failed;
            if (!native_gpu_canonical_texture_sync(&compiler, &operation.semantic)) goto failed;
            /* Expanded once; the endpoint and every recipe share it. */
            GlNativeRecipeMesh *expanded = malloc(sizeof(*expanded) +
                (size_t)xg_render_native_mesh_instance_triangles(instance) * sizeof(expanded->triangles[0]));
            if (!expanded) goto allocation_failed;
            expanded->references = 1u;
            expanded->count = expanded->capacity = xg_render_native_mesh_instance_triangles(instance);
            expanded->relocated = 0u;
            expanded->textures = NULL;
            expanded->poses = NULL;
            if (!xg_render_native_mesh_expand(instance, geometry, expanded->triangles)) {
                free(expanded); goto failed;
            }
            native_recipe_frame_draw(views, &draw);
            if (compiler.gpu) {
                compiler.gpu_only = 1;
                compiler.mesh_base = (const uint8_t *)&expanded->triangles[0].triangle;
                compiler.mesh_textures = NULL;
                compiler.mesh_flags = (const uint8_t *)&expanded->triangles[0].flags;
                compiler.mesh_stride = sizeof(expanded->triangles[0]);
                compiler.mesh_count = expanded->count;
                const int drawn = native_view_draw(&compiler, views, &draw, operation_index, 1);
                compiler.mesh_count = 0u;
                compiler.mesh_flags = NULL;
                compiler.gpu_only = 0;
                if (!drawn) { native_recipe_mesh_release(expanded); goto failed; }
            }
            for (uint32_t i = 0u; i < views->count; ++i) {
                GlNativeViewTarget *target = &views->targets[i];
                native_recipe_append(views, i, &operation, audit, canvas.words,
                    views->active >= 0 && native_view_eligible(views, target) &&
                    views->targets[views->active].x == target->x &&
                    views->targets[views->active].width == target->width, expanded);
            }
            native_recipe_mesh_release(expanded);
            continue;
        }
        if (operation.kind != XG_RENDER_NATIVE_OPERATION_DRAW &&
            !native_view_worker_drain(&compiler, 0, 0, VRAM_W - 1, VRAM_H - 1)) goto failed;
        if (operation.kind == XG_RENDER_NATIVE_OPERATION_TARGET) {
            const int index = native_view_target(views, operation.dst_x, operation.dst_y,
                                             operation.width, operation.height);
            if (index < 0 || (!views->targets[index].pixels &&
                             !native_view_private(views, (uint32_t)index, canvas.pixels)))
                goto allocation_failed;
            {
                /* No recipe yet, or a snapshot frame that already reached the
                 * scanout or that the game left for another target: this
                 * declaration starts a new frame. A FILL clear still replaces
                 * it with a clear-based recipe. */
                const GlNativeViewTarget *declared = &views->targets[index];
                if (!declared->recipe || (declared->recipe->base_serial &&
                        (declared->displayed || views->active != index)))
                    native_recipe_begin_snapshot(views, (uint32_t)index);
            }
            views->active = index;
            views->targets[index].declaration = ++views->declaration_sequence;
            native_wave_capture_reset(&views->targets[index].wave);
            continue;
        }
        if (operation.kind == XG_RENDER_NATIVE_OPERATION_READBACK) {
            views->readback_rgb_digest = operation.readback_rgb_digest;
            views->readback_x = operation.dst_x; views->readback_y = operation.dst_y;
            views->readback_width = operation.width; views->readback_height = operation.height;
            views->readback_capture = 0;
            const int source = native_view_cover(views, views->count,
                operation.dst_x, operation.dst_y, operation.width, operation.height);
            if (source >= 0 && views->targets[source].transition_snapshot == 1)
                views->readback_capture = 1;
            else if (operation.width == 320 && native_transition_height(operation.height) &&
                native_view_cover(views, views->count, operation.dst_x, operation.dst_y, 320,
                    operation.height < 224u ? operation.height : 224u) >= 0) {
                /* Freeze the rendered source at StoreImage, before the guest can
                 * clear/reuse its framebuffer. This COPY affects VIEW only;
                 * the subsequent LoadImage must prove the RGB receipt. */
                XgRenderNativeOperation capture = {.kind = XG_RENDER_NATIVE_OPERATION_COPY,
                    .src_x = operation.dst_x, .src_y = operation.dst_y,
                    .dst_x = 704, .dst_y = 256, .width = 320, .height = operation.height};
                if (!copy_pixels) copy_pixels = malloc(canvas_bytes);
                if (!copy_pixels) goto allocation_failed;
                for (uint32_t y = 0; y < operation.height; ++y)
                    memcpy(copy_pixels + (size_t)y * 320,
                        canvas.pixels + (size_t)((operation.dst_y + y) & (VRAM_H - 1)) * VRAM_W + operation.dst_x,
                        320 * sizeof(*copy_pixels));
                if (!native_view_transfer(views, &capture, NULL, canvas.pixels, copy_pixels,
                    audit)) goto allocation_failed;
                for (uint32_t i = 0; i < views->count; ++i)
                    if (views->targets[i].x == 704 && views->targets[i].y == 256 &&
                        views->targets[i].transition_snapshot) views->targets[i].transition_snapshot = 2;
                views->readback_capture = 1;
            }
            continue;
        }
        if (operation.kind == XG_RENDER_NATIVE_OPERATION_DRAW) {
            XgSemanticDrawRecord draw;
            if (!native_materialize_native_draw(&operation.semantic, &draw)) goto failed;
            draw.hd_texture = operation.hd_texture;
            if (!native_gpu_canonical_texture_sync(&compiler, &operation.semantic)) goto failed;
            const XgRenderIrMaterialState *wave_area = &draw.primitive.material;
            const int wave_target = draw.native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
                gpu && !view_cpu
                ? native_view_cover(views, views->count, wave_area->draw_area_left,
                    wave_area->draw_area_top, wave_area->draw_area_right - wave_area->draw_area_left + 1,
                    wave_area->draw_area_bottom - wave_area->draw_area_top + 1) : -1;
            if (wave_target >= 0 && native_view_eligible(views, &views->targets[wave_target])) {
                /* Guest VRAM keeps the guest's own redraw; VIEW gets the
                 * whole effect at once when its last tile arrives. The mesh
                 * may follow a transfer without any new declaration. */
                GlNativeViewTarget *target = &views->targets[wave_target];
                if (!native_wave_capture_tile(target, &draw)) target->wave.invalid = 1;
                if ((draw.native_view_effect_index == NATIVE_WAVE_LAST_TILE ||
                     target->wave.count == NATIVE_WAVE_TILES) &&
                    (!native_view_worker_drain(&compiler, 0, 0, VRAM_W - 1, VRAM_H - 1) ||
                     !native_wave_finish(views, (uint32_t)wave_target,
                        audit->header.identity.guest_vblank_sequence, audit))) goto allocation_failed;
                compiler.gpu_only = 0;
                if (!native_render_draw(&compiler, &canvas, &draw, operation_index)) goto failed;
                native_recipe_draw_written(&compiler, &operation, audit, canvas.words);
                continue;
            }
            int marked = draw.screen_space_2d != 0u || draw.native_view_effect != 0u;
            for (uint32_t ti = 0u; ti < draw.primitive.triangle_count; ++ti)
                for (uint32_t vi = 0u; vi < 3u; ++vi)
                    marked |= draw.primitive.triangles[ti].vertices[vi].native_view_position;
            for (uint32_t li = 0u; li < draw.line_count; ++li)
                for (uint32_t vi = 0u; vi < 2u; ++vi)
                    marked |= draw.lines[li].vertices[vi].native_view_position;
            XgSemanticDrawRecord view_draw = draw;
            const int transition = native_transition_texture(views, &draw) >= 0;
            if (transition) {
                /* The complete 320-wide capture denotes the complete VIEW,
                 * rather than its central 4:3 crop. Apply the authored mesh
                 * deformation to the wider image, preserving canonical XY. */
                view_draw.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_STRETCH;
                view_draw.interpolation_id = 0u;
                for (uint32_t t = 0u; t < view_draw.primitive.triangle_count; ++t)
                    for (uint32_t v = 0u; v < 3u; ++v) {
                        view_draw.primitive.triangles[t].vertices[v].native_view_position = false;
                        view_draw.primitive.triangles[t].vertices[v].projective_position = false;
                    }
                marked = 1;
            }
            if (!transition) native_recipe_frame_draw(views, &draw);
            /* Packets a host enhancement adds in the GPU-DMA aperture are
             * never software rasterized: they reach the GPU planes only.
             * Nothing guest-visible derives from them. */
            const uint64_t host_command = operation.semantic.submission_command_id;
            const int host_draw = compiler.gpu && host_command >= UINT64_C(0x00800000) &&
                                  host_command < UINT64_C(0x01000000);
            compiler.gpu_only = host_draw || !view_cpu;
            if (!native_view_draw(&compiler, views, &view_draw, operation_index, marked)) goto failed;
            compiler.gpu_only = host_draw;
            for (uint32_t i = 0u; i < views->count; ++i) {
                GlNativeViewTarget *target = &views->targets[i];
                const XgRenderIrMaterialState *m = &draw.primitive.material;
                if (transition && m->draw_area_left < target->x + target->width &&
                    m->draw_area_right >= target->x && m->draw_area_top < target->y + target->height &&
                    m->draw_area_bottom >= target->y) {
                    /* Phase recipes retain canonical texture words. They cannot
                     * replay this GPU framebuffer dependency at full resolution;
                     * hold the actual endpoint at the effect's authored cadence. */
                    native_recipe_drop(target);
                } else native_recipe_append(views, i, &operation, audit, canvas.words,
                    views->active >= 0 && native_view_eligible(views, target) &&
                    views->targets[views->active].x == target->x &&
                    views->targets[views->active].width == target->width, NULL);
            }
            if (!native_render_draw(&compiler, &canvas, &draw, operation_index))
                goto failed;
            compiler.gpu_only = 0;
            native_recipe_draw_written(&compiler, &operation, audit, canvas.words);
            if (gpu && gpu->failed) goto failed;
            continue;
        }
        if ((operation.kind != XG_RENDER_NATIVE_OPERATION_UPLOAD &&
             operation.kind != XG_RENDER_NATIVE_OPERATION_COPY &&
             operation.kind != XG_RENDER_NATIVE_OPERATION_FILL) ||
            !operation.width || !operation.height || operation.width > VRAM_W ||
            operation.height > VRAM_H || operation.dst_x >= VRAM_W ||
            operation.dst_y >= VRAM_H)
            goto failed;
        if (operation.kind == XG_RENDER_NATIVE_OPERATION_UPLOAD) {
            upload.reference = operation.upload;
            if (xg_render_resource_view((XgRenderResourceHandle){
                    operation.upload.resource_id, operation.upload.generation},
                    &upload.view) != XG_RENDER_RESOURCE_OK || !upload.view.bytes) {
                native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_VIEW, operation_index,
                    operation.upload.resource_id, operation.upload.generation);
                goto failed;
            }
            /* Import validates the digest and owns a byte copy; this sealed
             * commit retains that immutable generation through compilation. */
            if (upload.view.content_digest != operation.upload.content_digest) {
                native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_RESOURCE_DIGEST, operation_index,
                    operation.upload.resource_id, operation.upload.generation);
                goto failed;
            }
            upload.view_valid = 1;
            if (!native_pixel_descriptor_valid(&upload, 0) ||
                (upload.view.descriptor.pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 &&
                 upload.view.descriptor.pixel_format != XG_RENDER_RESOURCE_PIXEL_FORMAT_CLUT555) ||
                upload.view.descriptor.width != operation.width ||
                upload.view.descriptor.height != operation.height) {
                native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_NATIVE_OPERATION, operation_index,
                    operation.upload.resource_id, operation.upload.generation);
                goto failed;
            }
        } else if (operation.kind == XG_RENDER_NATIVE_OPERATION_COPY) {
            if (operation.src_x >= VRAM_W || operation.src_y >= VRAM_H) goto failed;
            if (!copy_pixels) copy_pixels = malloc(canvas_bytes);
            if (!copy_words) copy_words = malloc(word_bytes);
            if (!copy_pixels || !copy_words) goto allocation_failed;
            /* Snapshot every wrapped source pixel before the first write. */
            for (uint32_t y = 0u; y < operation.height; ++y) {
                const size_t row = (size_t)((operation.src_y + y) & (VRAM_H - 1)) * VRAM_W;
                const uint32_t first = operation.width < VRAM_W - operation.src_x
                    ? operation.width : VRAM_W - operation.src_x;
                const size_t output = (size_t)y * operation.width;
                memcpy(copy_pixels + output, canvas.pixels + row + operation.src_x, first * sizeof(*copy_pixels));
                memcpy(copy_words + output, canvas.words + row + operation.src_x, first * sizeof(*copy_words));
                if (first < operation.width) {
                    memcpy(copy_pixels + output + first, canvas.pixels + row, (operation.width-first) * sizeof(*copy_pixels));
                    memcpy(copy_words + output + first, canvas.words + row, (operation.width-first) * sizeof(*copy_words));
                }
            }
        }

        XgRenderNativeOperation view_operation = operation;
        const int transition_upload = native_transition_upload(
            views, &operation, &upload, canvas.words, &view_operation);
        if (transition_upload == 2) {
            if (!copy_pixels) copy_pixels = malloc(canvas_bytes);
            if (!copy_pixels) goto allocation_failed;
            for (uint32_t y = 0u; y < operation.height; ++y)
                memcpy(copy_pixels + (size_t)y * operation.width,
                    canvas.pixels + (size_t)y * VRAM_W,
                    operation.width * sizeof(*copy_pixels));
        }
        /* The wave's own transfers (gpu_native_field_wave_transfer) leave
         * VIEW alone when Native replays the effect on the GPU planes. */
        const int wave_transfer = operation.kind == XG_RENDER_NATIVE_OPERATION_COPY &&
            operation.semantic.native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
            gpu && !view_cpu && !operation.mask_set && !operation.mask_check
            ? native_wave_transfer(views, &operation) : 0;
        copy_recipe_count = 0u;
        if (operation.kind == XG_RENDER_NATIVE_OPERATION_COPY) {
            copy_recipe_count = views->count;
            for (uint32_t i = 0u; i < copy_recipe_count; ++i) {
                copy_recipes[i] = views->targets[i].recipe;
                if (copy_recipes[i]) copy_recipes[i]->references++;
            }
        }
        if (transition_upload != 1 && !wave_transfer &&
            !native_view_transfer(views, &view_operation, &upload, canvas.pixels, copy_pixels,
                              audit)) goto allocation_failed;
        /* Wave transfers leave VIEW untouched, but their canonical COPY still
         * reads the pre-write image. They bypass native_view_transfer(), which
         * normally captures it: refresh it here even if an older snapshot
         * exists, so overlapping lifts and saved blocks use current pixels. */
        if (wave_transfer && !native_gpu_command(gpu, NATIVE_GPU_SNAPSHOT))
            goto allocation_failed;
        if (!operation.mask_set && !operation.mask_check &&
            operation.kind != XG_RENDER_NATIVE_OPERATION_UPLOAD) {
            /* COPY reads the pre-write snapshot, including overlapping moves.
             * Split runs at both source and destination wrap boundaries, then
             * update the same dependency blocks once per run instead of pixel. */
            const int copy = operation.kind == XG_RENDER_NATIVE_OPERATION_COPY;
            const uint32_t fill = conv_1555_to_rgba8(operation.fill_color);
            for (uint32_t y = 0u; y < operation.height; ++y) {
                const uint32_t dy = (operation.dst_y+y) & (VRAM_H-1);
                const uint32_t sy = (operation.src_y+y) & (VRAM_H-1);
                for (uint32_t x = 0u; x < operation.width;) {
                    const uint32_t dx = (operation.dst_x+x) & (VRAM_W-1);
                    const uint32_t sx = (operation.src_x+x) & (VRAM_W-1);
                    uint32_t length = operation.width-x;
                    if (length > VRAM_W-dx) length = VRAM_W-dx;
                    if (copy && length > VRAM_W-sx) length = VRAM_W-sx;
                    const size_t offset = (size_t)dy * VRAM_W + dx;
                    if (copy) {
                        const size_t input = (size_t)y * operation.width + x;
                        memcpy(canvas.pixels+offset, copy_pixels+input, length*sizeof(*copy_pixels));
                        memcpy(canvas.words+offset, copy_words+input, length*sizeof(*copy_words));
                    } else {
                        for (uint32_t p = 0u; p < length; ++p) {
                            canvas.pixels[offset+p] = fill;
                            canvas.words[offset+p] = operation.fill_color;
                        }
                    }
                    for (uint32_t block = dx/64u; block <= (dx+length-1u)/64u; ++block) {
                        views->word_versions[dy][block] = views->write_serial;
                        views->word_page_versions[dy / 256u][block] = views->write_serial;
                        if (gpu) {
                            gpu->dirty_word_rows[dy] |= 1u << block;
                            gpu->dirty_texture_pages[dy / 256u] |= 1u << block;
                        }
                    }
                    if (!native_gpu_span(gpu, 0u, dx, dy, length, 1,
                        copy ? 0u : UINT32_MAX, sx, sy, length, 1, copy ? 0u : fill, 0)) goto allocation_failed;
                    x += length;
                }
            }
        } else for (uint32_t y = 0u; y < operation.height; ++y)
            for (uint32_t x = 0u; x < operation.width; ++x) {
                const size_t offset =
                    (size_t)((operation.dst_y + y) & (VRAM_H - 1)) * VRAM_W +
                    ((operation.dst_x + x) & (VRAM_W - 1));
                uint32_t *destination = &canvas.pixels[offset];
                uint32_t color;
                uint16_t word;
                if (operation.kind == XG_RENDER_NATIVE_OPERATION_COPY) {
                    color = copy_pixels[(size_t)y * operation.width + x];
                    word = copy_words[(size_t)y * operation.width + x];
                } else {
                    word = operation.kind == XG_RENDER_NATIVE_OPERATION_FILL
                        ? operation.fill_color
                        : native_read_u16((const uint8_t *)upload.view.bytes +
                            (size_t)y * upload.view.descriptor.row_pitch + (size_t)x * 2u);
                    color = conv_1555_to_rgba8(word);
                }
                if (gpu) {
                    const int gx = (operation.dst_x+x)&(VRAM_W-1), gy = (operation.dst_y+y)&(VRAM_H-1);
                    const uint32_t mask = operation.mask_set | (operation.mask_check << 1u);
                    if (operation.kind == XG_RENDER_NATIVE_OPERATION_UPLOAD) {
                        if (!native_gpu_pixel(gpu, 0u, gx, gy, color, mask)) goto allocation_failed;
                    } else if (!native_gpu_span(gpu, 0u, gx, gy, 1, 1,
                        operation.kind == XG_RENDER_NATIVE_OPERATION_COPY ? 0u : UINT32_MAX,
                        (operation.src_x+x)&(VRAM_W-1), (operation.src_y+y)&(VRAM_H-1), 1, 1,
                        operation.kind == XG_RENDER_NATIVE_OPERATION_COPY ? 0u : color, mask)) goto allocation_failed;
                }
                if (operation.mask_check && (*destination >> 24u) != 0u) continue;
                *destination = color | (operation.mask_set ? UINT32_C(0xff000000) : 0u);
                canvas.words[offset] = word | (operation.mask_set ? UINT16_C(0x8000) : 0u);
                views->word_versions[offset / VRAM_W][(offset % VRAM_W) / 64u] = views->write_serial;
                views->word_page_versions[offset / (VRAM_W * 256u)][(offset % VRAM_W) / 64u] = views->write_serial;
                if (gpu) {
                    gpu->dirty_word_rows[offset / VRAM_W] |= 1u << ((offset % VRAM_W) / 64u);
                    gpu->dirty_texture_pages[offset / (VRAM_W * 256u)] |= 1u << ((offset % VRAM_W) / 64u);
                }
            }
        if (wave_transfer == 1) native_recipe_raster_transfer(views, &operation);
        else native_recipe_transfer(views, &operation, copy_recipes, copy_recipe_count);
        for (uint32_t i = 0u; i < copy_recipe_count; ++i) native_recipe_release(copy_recipes[i]);
        copy_recipe_count = 0u;
    }
    if (!native_view_worker_finish(&compiler) || !native_gpu_canonical_sync(&compiler)) goto failed;
    if (audit->header.display_boundary) {
        const int use_view = !display->depth24 && views->width &&
            display->width == views->width - 2u * views->offset &&
            (uint32_t)display->aspect_num * views->reference_height >=
                (uint32_t)display->aspect_den * display->width;
        const uint32_t storage_width = use_view ? views->width : display->width;
        int row_views[VRAM_H];
        /* Scanout can include unrendered lines or cross target boundaries (for
         * example 216/224 active lines in a 240-line projection). Resolve each
         * current device row; absence of a row never changes the output aspect. */
        for (uint32_t y = 0u; y < display->height; ++y) {
            const int row_y = (display->display_y + y) & (VRAM_H - 1);
            row_views[y] = use_view ? native_view_cover(views, views->count,
                display->display_x, row_y, display->width, 1) : -1;
            if (row_views[y] >= 0 && !views->targets[row_views[y]].pixels &&
                !native_view_private(views, (uint32_t)row_views[y], canvas.pixels))
                goto allocation_failed;
        }
        audit->endpoint_storage_width = storage_width;
        if (gpu) {
            gpu->scanout = !display->depth24;
            gpu->disabled = display->disabled; gpu->depth24 = display->depth24;
            gpu->scanout_x = display->display_x; gpu->scanout_y = display->display_y;
            gpu->scanout_width = storage_width; gpu->scanout_height = display->height;
            gpu->scanout_canonical_width = display->width; gpu->scanout_offset = use_view ? views->offset : 0u;
            for (uint32_t y = 0; y < display->height; ++y)
                gpu->row_planes[y] = row_views[y] < 0 ? 0 : (int)native_view_domain(views, &views->targets[row_views[y]]) + 1;
        }
        const char *skip_unchanged=getenv("PSX_NATIVE_SKIP_UNCHANGED_SCANOUT");
        if (gpu && gpu->scanout && (!skip_unchanged || skip_unchanged[0]!='0') &&
            !gpu->fresh && !view_cpu && !views->reset_motion_history &&
            !audit->header.discontinuity && native_motion_layout_equal(&s_native_motion_history,audit) &&
            native_identity_equal(base_identity,&s_native_motion_history.identity) &&
            display->display_x==s_native_motion_history.display.display_x &&
            display->display_y==s_native_motion_history.display.display_y && native_gpu_preserves_scanout(gpu)) {
            /* The last endpoint is still the exact device base. Auxiliary
             * snapshots and offscreen VRAM uploads cannot change its image.
             * Execute them, keeping that endpoint and its interval instead
             * of copying/hashing both CPU and GPU scanouts again. */
            gpu->scanout_unchanged=1;
            audit->endpoint_pixel_digest=s_native_motion_history.reference_digest;
        } else {
        endpoint_pixels = (uint32_t *)malloc(
            (size_t)storage_width * display->height * sizeof(uint32_t));
        if (!endpoint_pixels) goto allocation_failed;
        /* Resolve the source once per row. Keeping scanout conversion separate
         * from the serial digest lets the flat copy/alpha loops vectorize. */
        for (uint32_t y = 0u; y < display->height; ++y) {
            const uint32_t row_y = (display->display_y + y) & (VRAM_H - 1);
            const size_t row = (size_t)row_y * VRAM_W;
            uint32_t *output = endpoint_pixels + (size_t)y * storage_width;
            if (display->disabled) {
                for (uint32_t x = 0u; x < storage_width; ++x)
                    output[x] = UINT32_C(0xff000000);
            } else if (use_view && row_views[y] >= 0 && view_cpu) {
                /* Without the CPU VIEW raster the GPU composes the scanout;
                 * this logical reference is the canonical frame below. */
                const GlNativeViewTarget *view = &views->targets[row_views[y]];
                const uint32_t *input = view->pixels + (size_t)(row_y - view->y) * storage_width;
                for (uint32_t x = 0u; x < storage_width; ++x)
                    output[x] = input[x] | UINT32_C(0xff000000);
            } else if (display->depth24) {
                native_scanout_row_24(output, canvas.words + row, display->display_x, storage_width);
            } else {
                const uint32_t offset = use_view ? views->offset : 0u;
                for (uint32_t x = 0u; x < offset; ++x)
                    output[x] = UINT32_C(0xff000000);
                native_scanout_row_15(output + offset, canvas.words + row, display->display_x, display->width);
                for (uint32_t x = offset + display->width; x < storage_width; ++x)
                    output[x] = UINT32_C(0xff000000);
            }
        }
        const size_t endpoint_count = (size_t)storage_width * display->height;
        audit->endpoint_pixel_digest = pres_hash_pixels_visible(endpoint_pixels, endpoint_count,
            &audit->endpoint_visible_pixels);
        }
    }
    audit->endpoint_was_depth24 = display->depth24;
    audit->endpoint_was_discrete = 1;
    audit->all_records_consumed = 1;
    *out_vram = canvas.pixels;
    *out_words = canvas.words;
    *out_pixels = endpoint_pixels;
    free(copy_pixels);
    free(copy_words);
    return 1;

allocation_failed:
    native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE, operation_index, 0u, 0u);
failed:
    (void)native_view_worker_finish(&compiler);
    for (uint32_t i = 0u; i < copy_recipe_count; ++i) native_recipe_release(copy_recipes[i]);
    native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_NATIVE_OPERATION, operation_index, 0u, 0u);
    free(copy_pixels);
    free(copy_words);
    free(canvas.pixels);
    free(canvas.words);
    free(endpoint_pixels);
    return 0;
}

static int native_compile_cpu(GlNativeCompileAudit *audit, uint32_t **out_pixels,
                          GlNativeCpuCompiler *out_stores) {
    GlNativeCpuCompiler compiler;
    GlNativeOrderedRecord *ordered = NULL;
    size_t ordered_capacity = (size_t)audit->consumed_draws +
        audit->consumed_surface_edges + audit->consumed_ui_nodes +
        audit->consumed_ui_glyph_runs;
    uint32_t completed_pass_mask = 0u;
    uint32_t presentation_output_count = 0u;
    int result = 0, display_x = 0, display_y = 0;
    memset(&compiler, 0, sizeof(compiler));
    compiler.audit = audit;
    compiler.dithering_disabled = audit->header.display.dithering_disabled;
    *out_pixels = NULL;
    if (audit->blocker != GL_RENDERER_NATIVE_BLOCKER_NONE) goto finished;
    if (audit->header.identity.source_sequence == 0u ||
        (s_native_persistent_identity.source_sequence != 0u &&
         (audit->header.identity.presentation_epoch <
              s_native_persistent_identity.presentation_epoch ||
          (audit->header.identity.presentation_epoch ==
               s_native_persistent_identity.presentation_epoch &&
           audit->header.identity.source_sequence <=
               s_native_persistent_identity.source_sequence)))) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_SURFACE_HISTORY, 0u, 0u, 0u);
        goto finished;
    }
    if (audit->header.display.disabled || audit->header.display.width == 0u ||
        audit->header.display.height == 0u ||
        audit->header.display.aspect_num == 0u ||
        audit->header.display.aspect_den == 0u) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_DISPLAY,
                       0u, 0u, 0u);
        goto finished;
    }
    for (uint32_t i = 0u; i < audit->consumed_draws; ++i) {
        int found = 0;
        for (uint32_t p = 0u; p < audit->consumed_passes; ++p)
            found |= audit->draws[i].order.pass_id == audit->passes[p].pass_id;
        if (!found) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           i, 0u, 0u);
            goto finished;
        }
    }
    for (uint32_t i = 0u; i < audit->consumed_ui_nodes; ++i) {
        int found = 0;
        for (uint32_t p = 0u; p < audit->consumed_passes; ++p)
            found |= audit->ui_nodes[i].order.pass_id == audit->passes[p].pass_id;
        if (!found) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           i, 0u, 0u);
            goto finished;
        }
    }
    for (uint32_t i = 0u; i < audit->consumed_ui_glyph_runs; ++i) {
        int found = 0;
        for (uint32_t p = 0u; p < audit->consumed_passes; ++p)
            found |= audit->ui_runs[i].order.pass_id == audit->passes[p].pass_id;
        if (!found) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           i, 0u, 0u);
            goto finished;
        }
    }
    for (uint32_t i = 0u; i < audit->consumed_surface_edges; ++i) {
        int found = 0;
        for (uint32_t p = 0u; p < audit->consumed_passes; ++p)
            found |= audit->edges[i].order.pass_id == audit->passes[p].pass_id &&
                     audit->edges[i].target_surface_id ==
                         audit->passes[p].target_surface_id &&
                     audit->edges[i].target_generation ==
                         audit->passes[p].target_generation;
        if (!found) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           i, audit->edges[i].target_surface_id,
                           audit->edges[i].target_generation);
            goto finished;
        }
    }
    if (ordered_capacity != 0u) {
        ordered = (GlNativeOrderedRecord *)malloc(
            ordered_capacity * sizeof(*ordered));
        if (!ordered) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
                           0u, 0u, 0u);
            goto finished;
        }
    }
    for (uint32_t pi = 0u; pi < audit->consumed_passes; ++pi) {
        const XgSemanticPassRecord *pass = &audit->passes[pi];
        GlNativeResourceRecord *target_resource = native_audit_resource(
            audit, pass->target_surface_id, pass->target_generation);
        GlNativeCpuSurface *target = NULL;
        size_t ordered_count = 0u;
        if (pass->pass_id >= 32u ||
            (completed_pass_mask & (UINT32_C(1) << pass->pass_id)) != 0u ||
            (pass->dependency_mask & ~completed_pass_mask) != 0u) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_PASS_DEPENDENCY,
                           pi, pass->target_surface_id,
                           pass->target_generation);
            goto finished;
        }
        if (!target_resource ||
            (target_resource->view.kind !=
                 XG_RENDER_RESOURCE_GENERATED_SURFACE &&
             target_resource->view.kind != XG_RENDER_RESOURCE_FRAMEBUFFER)) {
            native_audit_block(audit,
                GL_RENDERER_NATIVE_BLOCKER_TARGET_SURFACE_DESCRIPTOR,
                pi, pass->target_surface_id, pass->target_generation);
            goto finished;
        }
        if (!native_cpu_surface_load(&compiler, target_resource,
                 GL_RENDERER_NATIVE_BLOCKER_TARGET_SURFACE_DESCRIPTOR,
                 pi, pass->load_operation == XG_SEMANTIC_PASS_LOAD, &target))
            goto finished;
        if (pass->viewport_width == 0u || pass->viewport_height == 0u ||
            pass->viewport_x > target->width ||
            pass->viewport_y > target->height ||
            pass->viewport_width > target->width - pass->viewport_x ||
            pass->viewport_height > target->height - pass->viewport_y ||
            (pass->presentation_output &&
             (!pass->store ||
              (pass->viewport_width != audit->header.display.width ||
               pass->viewport_height != audit->header.display.height)))) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           pi, pass->target_surface_id,
                           pass->target_generation);
            goto finished;
        }
        compiler.viewport_x = pass->viewport_x;
        compiler.viewport_y = pass->viewport_y;
        compiler.viewport_width = pass->viewport_width;
        compiler.viewport_height = pass->viewport_height;
        switch (pass->load_operation) {
        case XG_SEMANTIC_PASS_LOAD:
            audit->pass_loads++;
            break;
        case XG_SEMANTIC_PASS_CLEAR:
            for (uint32_t y = 0u; y < pass->viewport_height; ++y)
                memset(target->pixels +
                           (size_t)(pass->viewport_y + y) * target->width +
                           pass->viewport_x,
                       0, (size_t)pass->viewport_width *
                              sizeof(*target->pixels));
            audit->pass_clears++;
            break;
        case XG_SEMANTIC_PASS_DISCARD:
            /* Discard makes prior contents unavailable to the pass; unlike
             * CLEAR it does not author a clear color into the attachment. */
            audit->pass_discards++;
            break;
        default:
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_INVALID_RECORD,
                           pi, pass->target_surface_id,
                           pass->target_generation);
            goto finished;
        }
        for (uint32_t i = 0u; i < audit->consumed_surface_edges; ++i)
            if (audit->edges[i].order.pass_id == pass->pass_id)
                ordered[ordered_count++] = (GlNativeOrderedRecord){
                    audit->edges[i].order, i, GL_NATIVE_ORDERED_EDGE};
        for (uint32_t i = 0u; i < audit->consumed_draws; ++i)
            if (audit->draws[i].order.pass_id == pass->pass_id)
                ordered[ordered_count++] = (GlNativeOrderedRecord){
                    audit->draws[i].order, i, GL_NATIVE_ORDERED_DRAW};
        for (uint32_t i = 0u; i < audit->consumed_ui_nodes; ++i)
            if (audit->ui_nodes[i].order.pass_id == pass->pass_id)
                ordered[ordered_count++] = (GlNativeOrderedRecord){
                    audit->ui_nodes[i].order, i, GL_NATIVE_ORDERED_UI_NODE};
        for (uint32_t i = 0u; i < audit->consumed_ui_glyph_runs; ++i)
            if (audit->ui_runs[i].order.pass_id == pass->pass_id)
                ordered[ordered_count++] = (GlNativeOrderedRecord){
                    audit->ui_runs[i].order, i, GL_NATIVE_ORDERED_UI_RUN};
        qsort(ordered, ordered_count, sizeof(*ordered), native_ordered_compare);
        for (size_t oi = 0u; oi < ordered_count; ++oi) {
            const GlNativeOrderedRecord *record = &ordered[oi];
            if (record->kind == GL_NATIVE_ORDERED_EDGE) {
                if (!native_apply_surface_edge(&compiler, target,
                                           &audit->edges[record->index],
                                           record->index))
                    goto finished;
            } else if (record->kind == GL_NATIVE_ORDERED_DRAW) {
                if (!native_render_draw(&compiler, target,
                                    &audit->draws[record->index],
                                    record->index))
                    goto finished;
            } else if (record->kind == GL_NATIVE_ORDERED_UI_NODE) {
                if (!native_render_ui_node(&compiler, target,
                                       &audit->ui_nodes[record->index],
                                       record->index))
                    goto finished;
            } else if (!native_render_ui_run(&compiler, target,
                                         &audit->ui_runs[record->index],
                                         record->index)) {
                goto finished;
            }
        }
        audit->rendered_passes++;
        completed_pass_mask |= UINT32_C(1) << pass->pass_id;
        if (pass->store) {
            target->stored = 1;
            audit->pass_stores++;
            if (pass->presentation_output) {
                compiler.last_display_surface = target;
                display_x = (int)pass->viewport_x;
                display_y = (int)pass->viewport_y;
                presentation_output_count++;
            }
        } else {
            target->valid = 0;
            target->stored = 0;
            if (compiler.last_display_surface == target)
                compiler.last_display_surface = NULL;
        }
    }
    if (presentation_output_count != 1u || !compiler.last_display_surface) {
        native_audit_block(audit,
            GL_RENDERER_NATIVE_BLOCKER_NO_STORED_DISPLAY_SURFACE,
            0u, 0u, 0u);
        goto finished;
    }
    if (audit->header.display.width > SIZE_MAX /
            audit->header.display.height ||
        (size_t)audit->header.display.width * audit->header.display.height >
            SIZE_MAX / sizeof(**out_pixels)) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
                       0u, 0u, 0u);
        goto finished;
    }
    *out_pixels = (uint32_t *)malloc(
        (size_t)audit->header.display.width * audit->header.display.height *
        sizeof(**out_pixels));
    if (!*out_pixels) {
        native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,
                       0u, 0u, 0u);
        goto finished;
    }
    for (uint32_t y = 0u; y < audit->header.display.height; ++y) {
        memcpy(*out_pixels + (size_t)y * audit->header.display.width,
            compiler.last_display_surface->pixels +
                (size_t)(display_y + (int)y) *
                    compiler.last_display_surface->width + display_x,
            (size_t)audit->header.display.width * sizeof(**out_pixels));
        for (uint32_t x = 0u; x < audit->header.display.width; ++x)
            if (((*out_pixels)[(size_t)y * audit->header.display.width + x] &
                    UINT32_C(0x00ffffff)) != 0u)
                audit->endpoint_visible_pixels++;
    }
    if (audit->consumed_ui_nodes != 0u ||
        audit->consumed_ui_glyph_runs != 0u)
        audit->endpoint_was_discrete = 1;
    audit->endpoint_was_depth24 = audit->header.display.depth24 ? 1 : 0;
    if (audit->header.display.depth24 || !audit->header.temporally_eligible)
        audit->endpoint_was_discrete = 1;
    audit->endpoint_pixel_digest = pres_hash_bytes(
        (const uint8_t *)*out_pixels,
        (size_t)audit->header.display.width * audit->header.display.height *
            sizeof(**out_pixels));
    out_stores->audit = audit;
    for (uint32_t i = 0u; i < XG_RENDER_SCENE_RESOURCE_CAPACITY; ++i) {
        GlNativeCpuSurface *surface = &compiler.surfaces[i];
        if (!surface->stored) continue;
        out_stores->surfaces[i] = *surface;
        surface->pixels = NULL;
    }
    result = 1;

finished:
    free(ordered);
    native_cpu_compiler_destroy(&compiler);
    if (!result) {
        free(*out_pixels);
        *out_pixels = NULL;
    }
    return result;
}

static int native_gpu_canonical_enabled(const XgRenderSourceCommitHeader *header) {
    const char *option = getenv("PSX_NATIVE_GPU_CANONICAL");
    (void)header;
    /* Guest-resolution VRAM is also a CPU input (textures, palettes, copies).
     * Keeping that raster on the CPU avoids repeated synchronous GPU readbacks
     * before Native phase generation. VIEW geometry and phases stay on the GPU.
     * Retain the GPU canonical path as an explicit diagnostic A/B override. */
    return option && option[0] != '0' && s_native_gpu_thread;
}

static GlNativeGpuWork *native_gpu_work_create(XgRenderSourceCommitHandle commit,
        const GlNativeCompileAudit *audit, const GlNativeViewState *base_views,
        uint32_t base_scale, const GlNativeGpuWork *base_gpu, int deferred) {
    const uint32_t scale = audit->header.display.render_scale ? audit->header.display.render_scale : 1u;
    if (scale > GL_MAX_INTERNAL_SCALE) return NULL;
    GlNativeGpuWork *gpu = calloc(1u, sizeof(*gpu));
    if (!gpu) return NULL;
    if (s_native_command_reserve && s_native_command_reserve <= SIZE_MAX / sizeof(*gpu->commands)) {
        gpu->commands = malloc((size_t)s_native_command_reserve * sizeof(*gpu->commands));
        if (gpu->commands) gpu->capacity = s_native_command_reserve;
    }
    if (s_native_data_reserve) {
        gpu->data = malloc(s_native_data_reserve);
        if (gpu->data) gpu->data_capacity = s_native_data_reserve;
    }
    gpu->words = malloc((size_t)VRAM_W * VRAM_H * sizeof(*gpu->words));
    if (!gpu->words) { native_gpu_buffers_free(gpu); free(gpu); return NULL; }
    gpu->scale = scale; gpu->commit = commit; gpu->identity = audit->header.identity;
    const XgSemanticDisplayState *display = &audit->header.display;
    GlNativeRasterLayout raster;
    if (!native_raster_layout_build(display, scale, &raster)) {
        native_gpu_buffers_free(gpu); free(gpu->words); free(gpu); return NULL;
    }
    if (raster.width) {
        /* The display may crop a larger authored framebuffer. Keep the
         * framebuffer origin, rather than the crop, on an integer pixel. */
        raster.page_rows = base_gpu ? base_gpu->raster_page_rows : s_native_raster_layout.page_rows;
        if (raster.page_rows < display->height) raster.page_rows = 0u;
        const int shown = native_view_cover(base_views, base_views->count,
            display->display_x, display->display_y, display->width, display->height);
        const uint32_t origin = shown >= 0 ? base_views->targets[shown].y : display->display_y;
        if (origin >= display->height) raster.page_rows = origin;
        if (!raster.page_rows) {
            for (uint32_t i = 0; i < base_views->count; ++i) {
                const GlNativeViewTarget *target = &base_views->targets[i];
                if (target->x == display->display_x && target->width == display->width &&
                    target->height >= display->height && target->y >= display->height &&
                    (!raster.page_rows || target->y < raster.page_rows)) raster.page_rows = target->y;
            }
        }
    }
    gpu->render_width = raster.width;
    gpu->render_height = raster.height;
    gpu->raster_scale[0] = raster.scale_x;
    gpu->raster_scale[1] = raster.scale_y;
    gpu->raster_page_rows = raster.page_rows;
    if (raster.page_rows && display->height)
        raster.page_pixels = native_resolution_page_pixels(raster.page_rows, raster.height, display->height);
    gpu->raster_page_pixels = raster.page_pixels;

    gpu->canonical_gpu = native_gpu_canonical_enabled(&audit->header);
    if (gpu->canonical_gpu) {
        gpu->canonical_pixels = malloc((size_t)VRAM_W * VRAM_H * sizeof(*gpu->canonical_pixels));
        gpu->canonical_provenance = malloc((size_t)VRAM_W * VRAM_H * sizeof(*gpu->canonical_provenance));
        if (!gpu->canonical_pixels || !gpu->canonical_provenance) {
            native_gpu_buffers_free(gpu); free(gpu->words); free(gpu); return NULL;
        }
    }
    /* A prepared predecessor may create a plane that is absent from the
     * committed state today. Its successor must preserve that full-resolution
     * plane instead of replacing it with a logical-grid CPU seed. */
    for (uint32_t i = 0u; i < GL_NATIVE_GPU_PHASE_BASE; ++i)
        gpu->base_planes[i] = base_gpu ? base_gpu->widths[i] != 0u : g_native_backend->plane_resident(i);
    gpu->deferred = deferred;
    const GlNativeRasterLayout base_raster = base_gpu
        ? (GlNativeRasterLayout){base_gpu->render_width, base_gpu->render_height,
            base_gpu->raster_scale[0], base_gpu->raster_scale[1], base_gpu->raster_page_rows, base_gpu->raster_page_pixels}
        : s_native_raster_layout;
    gpu->fresh = scale != base_scale || !native_raster_layout_equal(&raster, &base_raster) ||
        gpu->canonical_gpu != (base_gpu ? base_gpu->canonical_gpu : s_native_gpu_canonical) ||
        base_views->width != audit->header.display.native_width ||
        base_views->offset != audit->header.display.native_offset_x ||
        base_views->reference_height != audit->header.display.native_height ||
        base_views->depth_test != (int)audit->header.display.native_depth_test;
    gpu->depth_test = audit->header.display.native_depth_test;
    gpu->depth_view = SDL_AtomicGet(&s_native_depth_view_option);
    gpu->depth_view_log[0] = s_native_depth_view_log_min;
    gpu->depth_view_log[1] = s_native_depth_view_log_max;
    gpu->wireframe = SDL_AtomicGet(&s_native_wireframe_option);
    if (gpu->wireframe) gpu->depth_view = GL_NATIVE_DEPTH_VIEW_OFF;
    if (gpu->depth_view != s_native_gpu_depth_view_applied ||
        gpu->wireframe != s_native_gpu_wireframe_applied) gpu->fresh = 1;
    return gpu;
}

static void native_prefetch_free(GlNativePrefetch *prefetch) {
    if (!prefetch->leased) return;
    GlNativeGpuPending *prepared = &prefetch->prepared;
    if (prepared->views) { native_views_discard(prepared->views, 0); free(prepared->views); }
    free(prepared->audit); free(prepared->pixels); free(prepared->vram); free(prepared->words);
    if (prefetch->gpu) {
        /* Deferred journals have no GL resources or consumer. */
        native_gpu_buffers_free(prefetch->gpu); free(prefetch->gpu->words); free(prefetch->gpu);
    }
    (void)xg_render_source_commit_release_read(prefetch->commit);
    SDL_LockMutex(s_native_state_mutex);
    ++s_native_compiler_diag.gpu.prefetch_discarded;
    SDL_UnlockMutex(s_native_state_mutex);
    memset(prefetch, 0, sizeof(*prefetch));
}

void native_prefetch_discard(void) {
    /* Children can borrow their preceding private VIEW allocations. */
    while (s_native_prefetch_count)
        native_prefetch_free(&s_native_prefetch[--s_native_prefetch_count]);
}

/* The compile owner is otherwise idle while the current GPU transaction
 * finishes. Capture exactly one successor against the current private CPU
 * result. Published VRAM, VIEW, motion history, GL planes and ACK order remain
 * unchanged until that successor becomes the real FIFO head. */
static int native_prefetch_one(GlNativeGpuWork *current) {
    const GlNativePrefetch *previous = s_native_prefetch_count
        ? &s_native_prefetch[s_native_prefetch_count - 1u] : NULL;
    const GlNativeGpuPending *base = previous ? &previous->prepared : &s_native_gpu_pending;
    const GlNativeGpuWork *base_gpu = previous ? previous->gpu : current;
    XgRenderSourceCommitHandle next;
    XgRenderSourceCommitHeader header;
    uint64_t deadline;
    if (current->canonical_gpu || !base->audit || !base->views || !base->vram || !base->words ||
        current->depth_view || current->wireframe || s_native_motion_probe.target_vblank ||
        s_native_recipe_audit_enabled || gl_renderer_native_guest_reference_enabled() ||
        !xg_render_worker_acquire_next_source(current->commit, base_gpu->commit, &next, &header, &deadline)) return 0;
    /* A deferred successor cannot service canonical GPU readbacks. In
     * particular, do not prefetch across a switch that disables interpolation. */
    if (!header.native_work || native_gpu_canonical_enabled(&header)) {
        (void)xg_render_source_commit_release_read(next);
        return 0;
    }
    GlNativePrefetch local = {.commit = next, .base_identity = base_gpu->identity, .leased = 1};
    GlNativePrefetch *prefetch = &local;
    GlNativeGpuPending *prepared = &prefetch->prepared;
    const uint64_t begin = SDL_GetTicksNS(), cpu_begin = native_thread_cpu_ns();
    prepared->audit = malloc(sizeof(*prepared->audit));
    if (!prepared->audit || !native_consume_source_commit(next, prepared->audit)) goto failed;
    prefetch->gpu = native_gpu_work_create(next, prepared->audit, base->views, base_gpu->scale, base_gpu, 1);
    prepared->views = calloc(1u, sizeof(*prepared->views));
    if (!prefetch->gpu || !prepared->views) goto failed;
    prefetch->gpu->timing = (GlNativeWorkTiming){.begin_ns = begin, .deadline_ns = deadline,
        .operations = header.native_operation_count, .fresh = prefetch->gpu->fresh};
    const uint64_t native_begin = native_thread_cpu_ns();
    if (!native_compile_native_work(next, prepared->audit, &prepared->vram, &prepared->words,
        &prepared->pixels, prepared->views, prefetch->gpu, base->views, base->vram, base->words,
        &base_gpu->identity) || prefetch->gpu->failed) goto failed;
    prefetch->gpu->geometry = native_gpu_geometry_prepare(prefetch->gpu);
    prefetch->gpu->timing.native_cpu_ns = native_thread_cpu_ns() - native_begin;
    prefetch->cpu_ns = native_thread_cpu_ns() - cpu_begin;
    SDL_LockMutex(s_native_state_mutex);
    ++s_native_compiler_diag.gpu.prefetch_prepared;
    s_native_compiler_diag.gpu.prefetch_cpu_ns += prefetch->cpu_ns;
    SDL_UnlockMutex(s_native_state_mutex);
    s_native_prefetch[s_native_prefetch_count++] = local;
    return 1;
failed:
    native_prefetch_free(&local);
    return 0;
}

static void native_prefetch_next(GlNativeGpuWork *current) {
    while (s_native_prefetch_count < GL_NATIVE_PREFETCH_CAPACITY) {
        SDL_LockMutex(s_native_state_mutex);
        const int can_prepare = current->state != NATIVE_GPU_READY &&
            !current->failed && !current->render_failed && !current->cancel_requested;
        SDL_UnlockMutex(s_native_state_mutex);
        if (!can_prepare || !native_prefetch_one(current)) break;
    }
}

void native_gpu_pending_discard(void) {
    native_prefetch_discard();
    GlNativeGpuPending *pending = &s_native_gpu_pending;
    if (pending->fence_reserved) {
        SDL_LockMutex(s_native_state_mutex);
        s_native_fences[pending->fence_index].state = GL_NATIVE_FENCE_STATE_FREE;
        s_native_fences[pending->fence_index].kind = GL_NATIVE_FENCE_KIND_NONE;
        SDL_UnlockMutex(s_native_state_mutex);
    }
    native_motion_discard(&pending->motion);
    if (pending->views) { native_views_discard(pending->views, 0); free(pending->views); }
    free(pending->audit); free(pending->pixels); free(pending->vram); free(pending->words);
    memset(pending, 0, sizeof(*pending));
}

XgRenderCompileResult native_worker_compile(XgRenderSourceCommitHandle commit,
                              XgRenderCompiledEndpoint *out_endpoint,
                              uint64_t *out_compile_fence, void *user_data) {
    GlNativeCompileAudit *audit = NULL;
    GlNativeCpuCompiler staged_stores = {0};
    GlNativeMotionPrepared motion = {0};
    GlNativeViewState *staged_views = NULL;
    uint32_t *compiled_pixels = NULL;
    uint32_t *native_vram = NULL;
    uint16_t *native_words = NULL;
    XgRenderCompileResult result = XG_RENDER_COMPILE_FAILED;
    uint64_t fence_handle = 0u;
    uint64_t backend_generation = 0u;
    uint32_t endpoint_generation = 0u;
    int endpoint_index = -1;
    int fence_index = -1;
    int fence_reserved = 0;
    int needs_endpoint;
    int native_work, display_boundary;
    uint64_t source_deadline_ns = 0u;
    uint64_t prepared_cpu_ns = 0u;
    int deadline_known;
    GlNativeGpuWork *gpu = NULL;
    GlNativeWorkTiming timing={.begin_ns=SDL_GetTicksNS()};
    const uint64_t compile_cpu_started=native_thread_cpu_ns();
    (void)user_data;

    if (!out_endpoint || !out_compile_fence) return XG_RENDER_COMPILE_FAILED;
    memset(out_endpoint, 0, sizeof(*out_endpoint));
    *out_compile_fence = 0u;
    if (!s_native_active || !s_native_state_mutex) return XG_RENDER_COMPILE_FAILED;
    SDL_LockMutex(s_native_state_mutex);
    s_native_compiler_diag.compile_attempts++;
    GlNativeGpuWork *pending_work = s_native_gpu_work;
    if (pending_work) {
        const int same = pending_work->commit.slot == commit.slot && pending_work->commit.generation == commit.generation;
        if (pending_work->state == 0) {
            s_native_gpu_work = NULL;
            SDL_UnlockMutex(s_native_state_mutex);
            native_gpu_pending_discard();
            native_gpu_buffers_free(pending_work); free(pending_work->words);
            for(unsigned i=0;i<=GL_NATIVE_MOTION_PHASE_CAPACITY;++i)free(pending_work->readback_pixels[i]);
            free(pending_work->reference_pixels); free(pending_work);
            SDL_LockMutex(s_native_state_mutex);
        } else if (same && pending_work->state == NATIVE_GPU_READY && s_native_gpu_pending.audit) {
            gpu = pending_work;
            audit = s_native_gpu_pending.audit; staged_views = s_native_gpu_pending.views;
            compiled_pixels = s_native_gpu_pending.pixels; native_vram = s_native_gpu_pending.vram;
            native_words = s_native_gpu_pending.words; motion = s_native_gpu_pending.motion;
            needs_endpoint = s_native_gpu_pending.needs_endpoint;
            endpoint_index = s_native_gpu_pending.endpoint_index; fence_index = s_native_gpu_pending.fence_index;
            fence_reserved = s_native_gpu_pending.fence_reserved;
            memset(&s_native_gpu_pending, 0, sizeof(s_native_gpu_pending));
            native_work = 1; display_boundary = audit->header.display_boundary;
            SDL_UnlockMutex(s_native_state_mutex);
            if (gpu->failed) { native_gpu_fail_publish(gpu); native_audit_block(audit,GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,0,0,0); goto compile_failed; }
            goto gpu_ready;
        } else {
            if (!same && (pending_work->state == NATIVE_GPU_QUEUED || pending_work->state == NATIVE_GPU_READY))
                pending_work->state = NATIVE_GPU_CANCELLED;
            const int prepare_next = same && s_native_gpu_pending.audit &&
                pending_work->state != NATIVE_GPU_CANCELLED && pending_work->state != NATIVE_GPU_PUBLISHED;
            SDL_UnlockMutex(s_native_state_mutex);
            if (prepare_next) native_prefetch_next(pending_work);
            return XG_RENDER_COMPILE_WOULD_BLOCK;
        }
    }
    SDL_UnlockMutex(s_native_state_mutex);

    if (s_native_prefetch_count) {
        GlNativePrefetch *prefetch = &s_native_prefetch[0];
        const int wire = SDL_AtomicGet(&s_native_wireframe_option);
        const int depth_view = wire ? GL_NATIVE_DEPTH_VIEW_OFF : SDL_AtomicGet(&s_native_depth_view_option);
        if (prefetch->commit.slot == commit.slot && prefetch->commit.generation == commit.generation &&
            native_identity_equal(&prefetch->base_identity, &s_native_native_vram_identity) &&
            prefetch->gpu->wireframe == wire && prefetch->gpu->depth_view == depth_view) {
            audit = prefetch->prepared.audit; staged_views = prefetch->prepared.views;
            compiled_pixels = prefetch->prepared.pixels; native_vram = prefetch->prepared.vram;
            native_words = prefetch->prepared.words; gpu = prefetch->gpu;
            timing = gpu->timing; prepared_cpu_ns = prefetch->cpu_ns;
            (void)xg_render_source_commit_release_read(prefetch->commit);
            --s_native_prefetch_count;
            if (s_native_prefetch_count) s_native_prefetch[0] = s_native_prefetch[1];
            memset(&s_native_prefetch[s_native_prefetch_count], 0, sizeof(s_native_prefetch[0]));
            SDL_LockMutex(s_native_state_mutex);
            ++s_native_compiler_diag.gpu.prefetch_used;
            SDL_UnlockMutex(s_native_state_mutex);
            deadline_known = xg_render_worker_source_deadline(commit, &source_deadline_ns);
            timing.deadline_ns = source_deadline_ns;
            timing.previous_vblank = s_native_motion_history.identity.guest_vblank_sequence;
            timing.previous_cycle = s_native_motion_history.identity.guest_cycle;
            gpu->timing = timing; gpu->deferred = 0;
            s_native_gpu_depth_view_applied = gpu->depth_view;
            s_native_gpu_wireframe_applied = gpu->wireframe;
            native_work = 1; display_boundary = audit->header.display_boundary;
            needs_endpoint = display_boundary;
            /* Start the captured canonical prefix while phases are prepared.
             * Only this real FIFO-head callback can publish it to OpenGL. */
            native_gpu_publish_prefix(gpu, 0);
            goto native_compiled;
        }
        native_prefetch_discard();
    }

    audit = (GlNativeCompileAudit *)malloc(sizeof(*audit));
    if (!audit) {
        SDL_LockMutex(s_native_state_mutex);
        s_native_compiler_diag.compile_failures++;
        s_native_compiler_diag.last_blocker =
            GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE;
        SDL_UnlockMutex(s_native_state_mutex);
        return XG_RENDER_COMPILE_FAILED;
    }
    if (!native_consume_source_commit(commit, audit)) goto compile_failed;
    deadline_known = xg_render_worker_source_deadline(commit, &source_deadline_ns);
    timing.deadline_ns = source_deadline_ns;
    timing.previous_vblank = s_native_motion_history.identity.guest_vblank_sequence;
    timing.previous_cycle = s_native_motion_history.identity.guest_cycle;
    timing.operations = audit->header.native_operation_count;
    native_work = audit->header.native_work;
    display_boundary = audit->header.display_boundary;
    needs_endpoint = !native_work || display_boundary;
    if (native_work) {
        const uint32_t scale = audit->header.display.render_scale ? audit->header.display.render_scale : 1u;
        if (scale > GL_MAX_INTERNAL_SCALE) {
            native_audit_block(audit,GL_RENDERER_NATIVE_BLOCKER_INVALID_DISPLAY,0,0,0); goto compile_failed;
        }
        gpu = native_gpu_work_create(commit, audit, &s_native_views, s_native_gpu_scale, NULL, 0);
        if (!gpu) goto compile_failed;
        gpu->timing = timing;
        s_native_gpu_depth_view_applied = gpu->depth_view;
        s_native_gpu_wireframe_applied = gpu->wireframe;
        gpu->timing.fresh = gpu->fresh;
        staged_views = malloc(sizeof(*staged_views));
        if (!staged_views) {
            native_audit_block(audit, GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE, 0u, 0u, 0u);
            goto compile_failed;
        }
        const uint64_t native_started = native_thread_cpu_ns();
        if (!native_compile_native_work(commit, audit, &native_vram, &native_words, &compiled_pixels, staged_views, gpu,
            &s_native_views, s_native_native_vram, s_native_native_words, &s_native_native_vram_identity))
            goto compile_failed;
        if (gpu) gpu->timing.native_cpu_ns = native_thread_cpu_ns() - native_started;
    } else if (!native_compile_cpu(audit, &compiled_pixels, &staged_stores)) {
        goto compile_failed;
    }
native_compiled:
    timing.reference_digest = audit->endpoint_pixel_digest;
    if (gpu) gpu->timing.reference_digest = timing.reference_digest;
    if (native_work && needs_endpoint && gpu && gpu->scanout_unchanged) {
        needs_endpoint=0;
        audit->temporal_status=GL_RENDERER_NATIVE_TEMPORAL_DUPLICATE;
    }
    if (native_work && needs_endpoint && !staged_views->reset_motion_history &&
        !audit->header.discontinuity && native_motion_layout_equal(&s_native_motion_history, audit) &&
        (gpu ? s_native_motion_history.reference_digest : s_native_motion_history.pixel_digest) == audit->endpoint_pixel_digest &&
        (!gpu || (!gpu->fresh && gpu->count == 0u))) {
        /* Every FIFO mutation still commits below. Do not replace the published
         * history/timestamp or interrupt a phase interval for a duplicate VBlank. */
        needs_endpoint = 0;
        audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_DUPLICATE;
    }
    if (needs_endpoint) {
        SDL_LockMutex(s_native_state_mutex);
        for (uint32_t i = 0u; i < GL_NATIVE_ENDPOINT_CAPACITY; ++i)
            if (!s_native_endpoints[i].in_use) { endpoint_index = (int)i; break; }
        for (uint32_t i = 0u; i < GL_NATIVE_FENCE_CAPACITY; ++i)
            if (s_native_fences[i].state == GL_NATIVE_FENCE_STATE_FREE) { fence_index = (int)i; break; }
        if (endpoint_index >= 0 && fence_index >= 0) {
            s_native_fences[fence_index].state = GL_NATIVE_FENCE_STATE_PENDING;
            s_native_fences[fence_index].kind = GL_NATIVE_FENCE_KIND_NONE;
            s_native_fences[fence_index].sync = NULL;
            fence_reserved = 1;
        }
        SDL_UnlockMutex(s_native_state_mutex);
        if (!fence_reserved) {
            /* Only private staging exists; rollback leaves VRAM, VIEW, history
             * and FIFO application identity unchanged for retry. */
            native_audit_block(audit, endpoint_index < 0 ? GL_RENDERER_NATIVE_BLOCKER_ENDPOINT_CAPACITY
                : GL_RENDERER_NATIVE_BLOCKER_FENCE_CAPACITY, 0u, 0u, 0u);
            result = XG_RENDER_COMPILE_WOULD_BLOCK;
            goto compile_failed;
        }
    }
    if ((!native_work || display_boundary) && (!gpu || !gpu->scanout)) {
        audit->endpoint_was_discrete = 1;
        native_guest_reference_compare(audit, compiled_pixels, audit->endpoint_pixel_digest);
    }
    if (native_work && needs_endpoint) {
        /* The base is complete before temporal preflight starts. Let the GPU
         * execute it now; replay resources become immutable at the first
         * published phase chunk. Probe/audit retain sealed execution. */
        if (gpu && !gpu->canonical_gpu && !s_native_motion_probe.target_vblank && !s_native_recipe_audit_enabled)
            native_gpu_publish_prefix(gpu, -2);
        const uint64_t phase_started = native_thread_cpu_ns();
        if (gpu) gpu->timing.phase_begin_ns = SDL_GetTicksNS();
        native_motion_prepare(audit, staged_views, native_vram, compiled_pixels, &motion, deadline_known, source_deadline_ns);
        if (gpu) gpu->timing.phase_cpu_ns = native_thread_cpu_ns() - phase_started;
    }

    if (gpu) {
        if (gpu->failed) { native_gpu_fail_publish(gpu); native_audit_block(audit,GL_RENDERER_NATIVE_BLOCKER_GL_RESOURCE,0,0,0); goto compile_failed; }
        if (!needs_endpoint && !gpu->fresh && gpu->count == 0u) {
            free(gpu->words); native_gpu_buffers_free(gpu); free(gpu);
            gpu = NULL; staged_views->gpu = NULL;
        }
    }
    if (gpu) {
        gpu->scanout = gpu->scanout && needs_endpoint;
        gpu->timing.compile_cpu_ns=prepared_cpu_ns+native_thread_cpu_ns()-compile_cpu_started;
        gpu->timing.gpu=1;
        gpu->phase_count = motion.count;
        s_native_gpu_pending = (GlNativeGpuPending){.audit=audit,.views=staged_views,.motion=motion,
            .pixels=compiled_pixels,.vram=native_vram,.words=native_words,.needs_endpoint=needs_endpoint,
            .endpoint_index=endpoint_index,.fence_index=fence_index,.fence_reserved=fence_reserved};
        /* No borrowed resource byte pointer is consumed after the callback.
         * Native GPU commands own their uploads; compiler-only replay cache
         * keys are never dereferenced by GL. Recipes retain motion inputs. */
        for (uint32_t i=0;i<audit->consumed_resources;++i) {
            audit->resources[i].view.bytes=NULL; audit->resources[i].view_valid=0;
        }
        /* Keep two batches: 30 Hz sources alternate large and small work at
         * 60 vblanks. Round like normal growth and allow later shrinkage. */
        s_native_command_reserve = native_gpu_reserve_hint(
            gpu->count > s_native_previous_commands ? gpu->count : s_native_previous_commands);
        s_native_data_reserve = native_gpu_reserve_hint(
            gpu->bytes > s_native_previous_bytes ? gpu->bytes : s_native_previous_bytes);
        s_native_previous_commands = gpu->count;
        s_native_previous_bytes = gpu->bytes;
        native_gpu_publish_prefix(gpu,1);
        return XG_RENDER_COMPILE_WOULD_BLOCK;
    }

gpu_ready:
    if(gpu)timing=gpu->timing;
    if (gpu && gpu->scanout) {
        SDL_LockMutex(s_native_state_mutex);
        timing.scale=gpu->scale;timing.width=gpu->image_widths[0];timing.height=gpu->image_heights[0];
        timing.commands=gpu->count+gpu->phase_command_count;timing.phases=gpu->phase_count;
        if (s_native_phase_hashes_enabled)
            memcpy(timing.phase_digests, gpu->image_digests + 1u,
                gpu->phase_count * sizeof(*timing.phase_digests));
        s_native_compiler_diag.gpu.hash_ns+=timing.hash_end_ns-timing.hash_begin_ns;
        s_native_compiler_diag.gpu.last_image_digest=gpu->image_digests[0];
        s_native_compiler_diag.gpu.last_image_identity=gpu->identity;
        s_native_compiler_diag.gpu.render_scale=gpu->scale;
        s_native_compiler_diag.gpu.storage_width=gpu->image_widths[0];
        s_native_compiler_diag.gpu.storage_height=gpu->image_heights[0];
        SDL_UnlockMutex(s_native_state_mutex);
        /* The reference is the original logical sample grid, taken from the
         * actual GPU result; never compare a CPU stand-in as a GPU endpoint. */
        if (gpu->reference_pixels) native_guest_reference_compare(audit,gpu->reference_pixels,
            pres_hash_bytes((const uint8_t *)gpu->reference_pixels,
                (size_t)gpu->scanout_width*gpu->scanout_height*sizeof(uint32_t)));
        if (!staged_views->reset_motion_history && !audit->header.discontinuity &&
            native_motion_layout_equal(&s_native_motion_history, audit) &&
            s_native_motion_history.pixel_digest == gpu->image_digests[0] &&
            s_native_motion_history.reference_digest == motion.history.reference_digest &&
            s_native_motion_history.crop_y == motion.history.crop_y &&
            native_motion_recipe_equal(s_native_motion_history.recipe, motion.history.recipe)) {
            /* Storage commands still commit and ACK. Only a proven duplicate
             * visual/temporal endpoint leaves the original source clock intact. */
            needs_endpoint = 0;
            audit->temporal_status = GL_RENDERER_NATIVE_TEMPORAL_DUPLICATE;
            audit->temporal_phase_count = audit->temporal_interval_vblanks = 0u;
            audit->temporal_interval_ns = 0u;
            SDL_LockMutex(s_native_state_mutex);
            s_native_fences[fence_index].state = GL_NATIVE_FENCE_STATE_FREE;
            s_native_fences[fence_index].kind = GL_NATIVE_FENCE_KIND_NONE;
            fence_reserved = 0;
            SDL_UnlockMutex(s_native_state_mutex);
        }
        audit->endpoint_pixel_digest = gpu->image_digests[0];
        GlNativeMotionProbe *probe = &s_native_motion_probe;
        const uint64_t vblank = audit->header.identity.guest_vblank_sequence;
        if (probe->target_vblank && needs_endpoint && vblank < probe->target_vblank &&
            probe->target_vblank - vblank <= GL_NATIVE_MOTION_PHASE_CAPACITY + 1u) {
            free(probe->previous_pixels);
            probe->previous_pixels = gpu->readback_pixels[0]; gpu->readback_pixels[0] = NULL;
            probe->previous_width = gpu->image_widths[0]; probe->previous_height = gpu->image_heights[0];
            probe->previous_image_identity = gpu->identity; probe->previous_digest = gpu->image_digests[0];
        } else if (probe->recipe && native_identity_equal(&probe->identity, &gpu->identity) && !probe->commands) {
            probe->commands = gpu->commands; gpu->commands = NULL;
            probe->count = gpu->count;
            for (uint32_t i = 0u; i <= gpu->phase_count; ++i) {
                probe->image_widths[i] = gpu->image_widths[i]; probe->image_heights[i] = gpu->image_heights[i];
                probe->readback_pixels[i] = gpu->readback_pixels[i]; gpu->readback_pixels[i] = NULL;
                probe->image_digests[i] = gpu->image_digests[i];
            }
        }
        audit->endpoint_visible_pixels = gpu->visible_pixels;
        audit->endpoint_storage_width = gpu->image_widths[0];
        audit->endpoint_storage_height = gpu->image_heights[0];
        motion.history.pixel_digest = gpu->image_digests[0];
        /* READY GPU images have no CPU storage-sized buffer. Retire the 1x
         * construction references rather than exposing them as high-res pixels. */
        free(compiled_pixels); compiled_pixels = NULL;
        for (uint32_t i=0;i<motion.count;++i) {
            free(motion.phases[i].pixels); motion.phases[i].pixels=NULL;
        }
    }

    SDL_LockMutex(s_native_state_mutex);
    if (native_work) {
        s_native_coverage_publications += audit->applied_temporal_publications;
        if (staged_views->temporal_hz != s_native_views.temporal_hz ||
            (gpu && gpu->fresh) ||
            (audit->header.display.render_scale ? audit->header.display.render_scale : 1u) !=
                (s_native_motion_history.display.render_scale ? s_native_motion_history.display.render_scale : 1u))
            s_native_temporal_generation++;
        if (gpu) {
            g_native_backend->apply_planes(gpu);
            s_native_gpu_scale=gpu->scale;
            s_native_raster_layout = (GlNativeRasterLayout){gpu->render_width, gpu->render_height,
                gpu->raster_scale[0], gpu->raster_scale[1], gpu->raster_page_rows, gpu->raster_page_pixels};
            s_native_gpu_canonical=gpu->canonical_gpu;
            s_native_compiler_diag.gpu.applied++;
            if (gpu->scanout) s_native_compiler_diag.gpu.last_reference_digest=motion.history.reference_digest;
        }
        if (staged_views->reset_motion_history || needs_endpoint) {
            native_recipe_release(s_native_motion_history.recipe);
            s_native_motion_history = motion.history;
            motion.history.recipe = NULL;
        }
        for (uint32_t i = 0u; i < s_native_views.count; ++i) {
            native_recipe_release(s_native_views.targets[i].recipe);
        }
        for (uint32_t i = 0u; i < s_native_views.domain_count; ++i) {
            int retained = 0;
            for (uint32_t j = 0u; j < staged_views->domain_count; ++j)
                retained |= s_native_views.domains[i].pixels == staged_views->domains[j].pixels;
            if (!retained) free(s_native_views.domains[i].pixels);
        }
        staged_views->gpu = NULL;
        native_coverage_release(s_native_views.publication);
        native_canvas_spare_clear();
        s_native_spare_vram = s_native_native_vram;
        s_native_spare_words = s_native_native_words;
        memcpy(s_native_spare_versions, s_native_views.word_versions, sizeof(s_native_spare_versions));
        native_views_copy(&s_native_views, staged_views);
        s_native_views.owned = 0u;
        s_native_views.reset_motion_history = 0;
        staged_views->owned = 0u;
        /* Publish both representations under the same lock/identity. Failed
         * compilation or WOULD_BLOCK retains the previous pair unchanged. */
        s_native_native_vram = native_vram;
        s_native_native_words = native_words;
        native_vram = NULL;
        native_words = NULL;
        s_native_native_vram_identity = audit->header.identity;
    } else {
        native_recipe_release(s_native_motion_history.recipe);
        memset(&s_native_motion_history, 0, sizeof(s_native_motion_history));
        native_cpu_surface_commit_stores(&staged_stores);
    }
    if (!needs_endpoint) {
        if (gpu) {
            gpu->state=NATIVE_GPU_PUBLISHED;
            SDL_CondSignal(s_native_gpu_condition);
            gpu=NULL;
        }
        SDL_UnlockMutex(s_native_state_mutex);
        result = XG_RENDER_COMPILE_APPLIED;
        goto compile_complete;
    }

    /* Publish both service handles under one lock only after every endpoint
     * field is initialized.  Existing per-slot GL names are presenter-owned
     * cache and deliberately survive software endpoint retirement. */
    GlNativeEndpointSlot *endpoint = &s_native_endpoints[endpoint_index];
    GlNativeFenceSlot *fence = &s_native_fences[fence_index];
    endpoint_generation = native_next_generation(endpoint->generation);
    backend_generation = s_native_backend_generation;
    endpoint->generation = endpoint_generation;
    endpoint->temporal_generation = s_native_temporal_generation;
    endpoint->staged_pixels = compiled_pixels;
    endpoint->mask_valid = 0;
    endpoint->identity = audit->header.identity;
    endpoint->metadata = (GlRendererNativeEndpointMetadata){
        .display_x = audit->header.display.display_x,
        .display_y = audit->header.display.display_y,
        .aspect_num = audit->header.display.aspect_num,
        .aspect_den = audit->header.display.aspect_den,
        .width = audit->header.display.width,
        .height = audit->header.display.height,
        .storage_width = audit->endpoint_storage_width,
        .storage_height = audit->endpoint_storage_height,
        .format = GL_RENDERER_NATIVE_ENDPOINT_FORMAT_RGBA8,
        .depth24 = audit->header.display.depth24 ? 1 : 0,
        .interlaced = audit->header.display.interlaced ? 1 : 0,
        .disabled = audit->header.display.disabled ? 1 : 0,
        .temporal_hz = audit->header.display.temporal_hz,
        .render_scale = (uint16_t)(gpu && gpu->scanout ? gpu->scale : 1u),
        .render_width = (uint16_t)(gpu && gpu->scanout ? gpu->render_width : 0u),
        .render_height = (uint16_t)(gpu && gpu->scanout ? gpu->render_height : 0u),
        .temporal_phase_count = motion.count,
        .temporal_interval_vblanks = audit->temporal_interval_vblanks,
        .temporal_interval_ns = motion.interval_ns,
        .pixel_digest = audit->endpoint_pixel_digest,
        .temporal_previous_pixel_digest = motion.previous_digest,
    };
    if (audit->header.display.native_width != 0u &&
        audit->header.display.width == audit->header.display.native_width - 2u*audit->header.display.native_offset_x &&
        (uint32_t)audit->header.display.aspect_num*audit->header.display.native_height >
            (uint32_t)audit->header.display.aspect_den*audit->header.display.width &&
        audit->header.display.native_height != 0u) {
        endpoint->metadata.aspect_num = audit->header.display.native_width;
        endpoint->metadata.aspect_den = audit->header.display.native_height;
    }
    endpoint->digest = audit->header.digest;
    endpoint->record_audit_digest = audit->record_digest;
    endpoint->pixel_digest = audit->endpoint_pixel_digest;
    for (uint32_t i = 0u; i < motion.count; ++i) {
        endpoint->phases[i].pixels = motion.phases[i].pixels;
        endpoint->phases[i].mask_valid = 0;
        endpoint->phases[i].digest = motion.phases[i].digest;
        endpoint->phases[i].upload_state = GL_NATIVE_ENDPOINT_UPLOAD_STAGED;
        motion.phases[i].pixels = NULL;
    }
    endpoint->backend_generation = backend_generation;
    endpoint->fence_refs = 1u;
    endpoint->upload_state = GL_NATIVE_ENDPOINT_UPLOAD_STAGED;
    if (gpu && gpu->scanout) g_native_backend->adopt_images(endpoint, gpu, motion.count);
    endpoint->release_pending = 0;
    endpoint->presenter_busy = 0;
    endpoint->in_use = 1;
    s_native_pipeline_diag.last_identity = endpoint->identity;
    s_native_pipeline_diag.last_commit_digest = endpoint->digest;
    s_native_pipeline_diag.last_record_audit_digest =
        endpoint->record_audit_digest;
    s_native_pipeline_diag.last_endpoint_pixel_digest = endpoint->pixel_digest;
    s_native_pipeline_diag.last_endpoint_handle = native_pack_handle(
        (uint32_t)endpoint_index, endpoint_generation);
    s_native_pipeline_diag.last_backend_generation = backend_generation;
    s_native_pipeline_diag.last_width = endpoint->metadata.width;
    s_native_pipeline_diag.last_height = endpoint->metadata.height;
    s_native_pipeline_diag.last_format = endpoint->metadata.format;

    fence->generation = native_next_generation(fence->generation);
    fence->sync = NULL;
    fence->endpoint_slot = (uint32_t)endpoint_index;
    fence->endpoint_generation = endpoint_generation;
    fence->kind = GL_NATIVE_FENCE_KIND_SOFTWARE_READY;
    fence->state = GL_NATIVE_FENCE_STATE_READY;
    fence_handle = native_pack_handle((uint32_t)fence_index, fence->generation);
    compiled_pixels = NULL; /* endpoint owns staging after atomic publication */
    if (gpu) {
        gpu->state=NATIVE_GPU_PUBLISHED;
        SDL_CondSignal(s_native_gpu_condition);
        gpu=NULL;
    }
    SDL_UnlockMutex(s_native_state_mutex);

    *out_endpoint = (XgRenderCompiledEndpoint){
        .opaque_handle = native_pack_handle(
            (uint32_t)endpoint_index, endpoint_generation),
        .identity = audit->header.identity,
        .digest = audit->header.digest,
        .width = audit->header.display.width,
        .height = audit->header.display.height,
        .format = GL_RENDERER_NATIVE_ENDPOINT_FORMAT_RGBA8,
        .backend_generation = backend_generation,
        .temporal_phase_count = motion.count,
        .temporal_interval_ns = motion.interval_ns,
        .pixel_digest = audit->endpoint_pixel_digest,
        .temporal_previous_pixel_digest = motion.previous_digest,
    };
    *out_compile_fence = fence_handle;
    result = XG_RENDER_COMPILE_ENDPOINT_READY;

compile_complete:
    timing.end_ns=SDL_GetTicksNS();timing.identity=audit->header.identity;timing.digest=audit->endpoint_pixel_digest;
    timing.temporal_status=audit->temporal_status;
    if(!timing.gpu)timing.compile_cpu_ns=native_thread_cpu_ns()-compile_cpu_started;
    /* Keep recent completions after long sessions; startup-only timings hide
     * the backlog that develops later in a running scene. */
    s_native_work_timing[s_native_work_timing_count++ % 8192u]=timing;
    free(compiled_pixels);
    native_motion_discard(&motion);
    free(staged_views);
    native_cpu_compiler_destroy(&staged_stores);
    native_publish_compile_audit(audit, result);
    free(audit);
    return result;

compile_failed:
    native_prefetch_discard();
    if (gpu) {
        SDL_LockMutex(s_native_state_mutex);
        const int queued = s_native_gpu_work == gpu;
        if (queued) {
            gpu->cancel_requested=1;
            if(gpu->state==NATIVE_GPU_WAIT_INPUT)gpu->state=NATIVE_GPU_CANCELLED;
            SDL_CondBroadcast(s_native_gpu_condition);
        }
        SDL_UnlockMutex(s_native_state_mutex);
        if (!queued) { native_gpu_buffers_free(gpu); free(gpu->words); free(gpu->reference_pixels); free(gpu); }
    }
    native_motion_discard(&motion);
    if (fence_reserved) {
        SDL_LockMutex(s_native_state_mutex);
        s_native_fences[fence_index].state = GL_NATIVE_FENCE_STATE_FREE;
        s_native_fences[fence_index].kind = GL_NATIVE_FENCE_KIND_NONE;
        SDL_UnlockMutex(s_native_state_mutex);
    }
    native_cpu_compiler_destroy(&staged_stores);
    if (staged_views) {
        native_views_discard(staged_views, 0);
        free(staged_views);
    }
    free(native_vram);
    free(native_words);
    free(compiled_pixels);
    native_publish_compile_audit(audit, result);
    free(audit);
    return result;
}
