/* The backend-neutral half of Native presentation (shared by the OpenGL and the
 * Vulkan presenter): which endpoint image a compose call shows, the endpoint's
 * presenter ownership, the upload-state bookkeeping and the diagnostics. The
 * backends keep the drawing. */
#include "native_work_internal.h"

NativeImageSlot native_endpoint_image(const GlNativeEndpointSlot *endpoint, uint32_t index) {
    if (index) return endpoint->phases[index - 1u];
    return (NativeImageSlot){.color = endpoint->image.color, .target = endpoint->image.target,
        .width = endpoint->image.width, .height = endpoint->image.height,
        .mask = endpoint->image.mask, .mask_valid = endpoint->mask_valid,
        .pixels = endpoint->staged_pixels, .digest = endpoint->pixel_digest, .upload_state = endpoint->upload_state};
}

void native_endpoint_store_image(GlNativeEndpointSlot *endpoint, uint32_t index, const NativeImageSlot *image) {
    if (index) endpoint->phases[index - 1u] = *image;
    else {
        endpoint->image.color = image->color; endpoint->image.target = image->target;
        endpoint->image.width = image->width; endpoint->image.height = image->height;
        endpoint->image.mask = image->mask; endpoint->mask_valid = image->mask_valid;
        endpoint->staged_pixels = image->pixels; endpoint->upload_state = image->upload_state;
    }
}

float native_aa_halton(unsigned index, unsigned base) {
    float value = 0.f, place = 1.f;
    while (index) {
        place /= (float)base;
        value += place * (float)(index % base);
        index /= base;
    }
    return value - 0.5f;
}

static void native_present_refuse(uint32_t blocker)
{
    s_native_pipeline_diag.compose_failures++;
    s_native_pipeline_diag.last_present_blocker = blocker;
}

bool native_present_select(NativePresentSel *sel, const XgRenderCompiledEndpoint *compiled,
                           uint64_t alpha_numerator, uint64_t alpha_denominator,
                           uint64_t *out_completion_fence, int thread_ok)
{
    memset(sel, 0, sizeof(*sel));
    if (out_completion_fence) *out_completion_fence = 0u;
    uint32_t slot = 0u, generation = 0u;
    if (!compiled || !out_completion_fence || alpha_denominator == 0u ||
        alpha_numerator == 0u || alpha_numerator > alpha_denominator || !thread_ok || !s_native_state_mutex ||
        !native_unpack_handle(compiled->opaque_handle, GL_NATIVE_ENDPOINT_CAPACITY, &slot, &generation)) {
        if (s_native_state_mutex) {
            SDL_LockMutex(s_native_state_mutex);
            s_native_pipeline_diag.compose_attempts++;
            native_present_refuse(GL_RENDERER_NATIVE_PRESENT_BLOCKER_INVALID_ARGUMENT);
            SDL_UnlockMutex(s_native_state_mutex);
        }
        return false;
    }
    sel->slot = slot; sel->generation = generation;
    SDL_LockMutex(s_native_state_mutex);
    GlNativeEndpointSlot *live = &s_native_endpoints[slot];
    s_native_pipeline_diag.compose_attempts++;
    s_native_pipeline_diag.last_identity = compiled->identity;
    s_native_pipeline_diag.last_commit_digest = compiled->digest;
    s_native_pipeline_diag.last_endpoint_handle = compiled->opaque_handle;
    s_native_pipeline_diag.last_backend_generation = compiled->backend_generation;
    s_native_pipeline_diag.last_width = compiled->width;
    s_native_pipeline_diag.last_height = compiled->height;
    s_native_pipeline_diag.last_format = compiled->format;
    const uint32_t mismatch = native_endpoint_mismatch_mask(compiled, live, generation);
    if (mismatch != 0u || live->presenter_busy) {
        s_native_pipeline_diag.last_endpoint_mismatch_mask = mismatch;
        native_present_refuse(GL_RENDERER_NATIVE_PRESENT_BLOCKER_ENDPOINT_MISMATCH);
        SDL_UnlockMutex(s_native_state_mutex);
        return false;
    }
    if (alpha_numerator != alpha_denominator) {
        const uint32_t denominator = live->metadata.temporal_phase_count + 1u;
        if (denominator <= 1u || denominator > GL_NATIVE_MOTION_PHASE_CAPACITY + 1u ||
            alpha_numerator > UINT64_MAX / denominator || (alpha_numerator * denominator) % alpha_denominator != 0u) {
            native_present_refuse(GL_RENDERER_NATIVE_PRESENT_BLOCKER_INVALID_ARGUMENT);
            SDL_UnlockMutex(s_native_state_mutex);
            return false;
        }
        /* A captured rate transition expires old phases, including OFF->ON at
         * the same rate. The retained authored endpoint remains a valid hold. */
        if (live->temporal_generation == s_native_temporal_generation)
            sel->image_index = (uint32_t)(alpha_numerator * denominator / alpha_denominator);
    }
    sel->image = native_endpoint_image(live, sel->image_index);
    if (sel->image.upload_state != GL_NATIVE_ENDPOINT_UPLOAD_STAGED &&
        sel->image.upload_state != GL_NATIVE_ENDPOINT_UPLOAD_READY) {
        native_present_refuse(GL_RENDERER_NATIVE_PRESENT_BLOCKER_STAGING_MISSING);
        SDL_UnlockMutex(s_native_state_mutex);
        return false;
    }
    if (sel->image.upload_state == GL_NATIVE_ENDPOINT_UPLOAD_READY && !sel->image.color) {
        native_present_refuse(GL_RENDERER_NATIVE_PRESENT_BLOCKER_UPLOAD);
        SDL_UnlockMutex(s_native_state_mutex);
        return false;
    }
    s_native_pipeline_diag.last_endpoint_mismatch_mask = 0u;
    live->presenter_busy = 1;
    if (sel->image.upload_state == GL_NATIVE_ENDPOINT_UPLOAD_STAGED) {
        sel->staged_pixels = sel->image.pixels;
        sel->image.pixels = NULL;
        sel->image.upload_state = GL_NATIVE_ENDPOINT_UPLOAD_IN_PROGRESS;
        native_endpoint_store_image(live, sel->image_index, &sel->image);
        sel->upload_required = 1;
        s_native_pipeline_diag.upload_attempts++;
    }
    sel->endpoint = *live;
    sel->endpoint.image.color = sel->image.color; sel->endpoint.image.target = sel->image.target;
    sel->endpoint.pixel_digest = sel->image.digest;
    SDL_UnlockMutex(s_native_state_mutex);
    return true;
}

int native_present_upload_result(NativePresentSel *sel, int upload_ok)
{
    SDL_LockMutex(s_native_state_mutex);
    GlNativeEndpointSlot *live = &s_native_endpoints[sel->slot];
    if (!live->in_use || live->generation != sel->generation || !live->presenter_busy) {
        upload_ok = 0;
    } else {
        sel->image.color = sel->endpoint.image.color; sel->image.target = sel->endpoint.image.target;
        sel->image.width = sel->endpoint.metadata.storage_width; sel->image.height = sel->endpoint.metadata.storage_height;
        sel->image.upload_state = upload_ok ? GL_NATIVE_ENDPOINT_UPLOAD_READY : GL_NATIVE_ENDPOINT_UPLOAD_FAILED;
        native_endpoint_store_image(live, sel->image_index, &sel->image);
        sel->endpoint = *live;
        sel->endpoint.image.color = sel->image.color; sel->endpoint.image.target = sel->image.target;
        sel->endpoint.pixel_digest = sel->image.digest;
    }
    if (upload_ok) s_native_pipeline_diag.upload_successes++;
    else s_native_pipeline_diag.upload_failures++;
    sel->upload_result_recorded = 1;
    SDL_UnlockMutex(s_native_state_mutex);
    return upload_ok;
}

void native_present_commit(NativePresentSel *sel, uint64_t present_sequence)
{
    SDL_LockMutex(s_native_state_mutex);
    GlNativeEndpointSlot *live = &s_native_endpoints[sel->slot];
    if (live->in_use && live->generation == sel->generation) {
        live->presenter_busy = 0;
        native_endpoint_maybe_recycle_locked(sel->slot, sel->generation);
    }
    s_native_pending_present_sequence = present_sequence;
    s_native_pending_present_valid = 1;
    s_native_pipeline_diag.compose_successes++;
    s_native_pipeline_diag.last_present_blocker = GL_RENDERER_NATIVE_PRESENT_BLOCKER_NONE;
    s_native_pipeline_diag.last_present_sequence = present_sequence;
    SDL_UnlockMutex(s_native_state_mutex);
}

void native_present_abort(NativePresentSel *sel, uint32_t blocker)
{
    SDL_LockMutex(s_native_state_mutex);
    GlNativeEndpointSlot *live = &s_native_endpoints[sel->slot];
    if (live->in_use && live->generation == sel->generation) {
        /* A failed presentation attempt must not consume a retained hold.
         * Restore unuploaded pixels, or keep an already complete texture. */
        NativeImageSlot image = native_endpoint_image(live, sel->image_index);
        if (sel->staged_pixels) {
            image.pixels = sel->staged_pixels;
            sel->staged_pixels = NULL;
            image.upload_state = GL_NATIVE_ENDPOINT_UPLOAD_STAGED;
        } else if (image.upload_state != GL_NATIVE_ENDPOINT_UPLOAD_READY) {
            image.upload_state = GL_NATIVE_ENDPOINT_UPLOAD_FAILED;
        }
        native_endpoint_store_image(live, sel->image_index, &image);
        live->presenter_busy = 0;
        native_endpoint_maybe_recycle_locked(sel->slot, sel->generation);
    }
    if (sel->upload_required && !sel->upload_result_recorded) s_native_pipeline_diag.upload_failures++;
    s_native_pending_present_sequence = 0u;
    s_native_pending_present_valid = 0;
    native_present_refuse(blocker != 0u ? blocker : GL_RENDERER_NATIVE_PRESENT_BLOCKER_GL_DRAW);
    SDL_UnlockMutex(s_native_state_mutex);
    free(sel->staged_pixels);
    sel->staged_pixels = NULL;
}

/* ---- presentation geometry and filter choice shared by the draw halves ---- */
void native_letterbox_rect(int ox, int oy, int ow, int oh, int num, int den, int *x, int *y, int *w, int *h)
{
    int dw = ow, dh = (ow * den) / num;
    if (dh > oh) { dh = oh; dw = (oh * num) / den; }
    *x = ox + (ow - dw) / 2;
    *y = oy + (oh - dh) / 2;
    *w = dw; *h = dh;
}

void native_present_rect(const GlRendererNativeEndpointMetadata *metadata, int window_w, int window_h,
                         int stretch_num, int stretch_den, int *x, int *y, int *w, int *h)
{
    if (stretch_num > 0 && !metadata->depth24)
        native_letterbox_rect(0, 0, window_w, window_h, stretch_num, stretch_den, x, y, w, h);
    else
        native_letterbox_rect(0, 0, window_w, window_h, metadata->aspect_num, metadata->aspect_den, x, y, w, h);
}

int native_present_aa_mode(const GlRendererNativeEndpointMetadata *metadata, int mask_valid, int aa_setting, int *aa_nearest)
{
    /* CPU endpoints have no class mask: a nearest present leaves all guest
     * sprites/UI untouched rather than guessing from their final colours. */
    const int mode = metadata->depth24 || !mask_valid ? 0 : aa_setting;
    /* GPU scanout also serves 1x. Do not turn FXAA into whole-image bilinear
     * magnification there: filter edges at the window pixel scale. */
    *aa_nearest = mode == 1 && metadata->render_scale <= 1u;
    return mode;
}
