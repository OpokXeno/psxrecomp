/* Private implementation included after the recipe/coverage helpers.
 * Visibility belongs to the sampled camera, not the newer endpoint's draw list.
 * Recover screen departures only from adjacent, compatible source publications;
 * never extrapolate a missing vertex or carry a face into endpoint history. */
typedef struct GlNativeDepartureKey {
    GpuRenderInterpolationIdentity identity;
    uint32_t index;
} GlNativeDepartureKey;

static int native_departure_key_compare(const void *left, const void *right) {
    const GlNativeDepartureKey *a = left, *b = right;
#define COMPARE_KEY(field) if (a->identity.field != b->identity.field) \
    return a->identity.field < b->identity.field ? -1 : 1
    COMPARE_KEY(scene_id);
    COMPARE_KEY(producer_id);
    COMPARE_KEY(primitive_id);
#undef COMPARE_KEY
    return 0;
}

static int native_departure_outside(const GlNativeRecipe *recipe, const GlNativeRecipeDraw *draw) {
    int64_t left = INT64_MAX, right = INT64_MIN, top = INT64_MAX, bottom = INT64_MIN;
    for (uint32_t t = 0u; t < draw->semantic.triangle_count; ++t)
        for (uint32_t v = 0u; v < 3u; ++v) {
            const GpuRenderSemanticVertex *p = &draw->semantic.triangles[t].vertices[v];
            const int wide = recipe->view_width && draw->enhanced && p->native_view_position;
            const int64_t x = wide ? p->native_view_x : p->x;
            const int64_t y = wide ? p->native_view_y : p->y;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    const int64_t width = recipe->view_width ? recipe->view_width : recipe->width;
    return right < 0 || left >= width * 65536 || bottom < 0 || top >= (int64_t)recipe->height * 65536;
}

static int native_departure_reproject(const GlNativeRecipeDraw *before,
                                    const GlNativeRecipeCoverage *coverage,
                                    GlNativeRecipeDraw *draw) {
    *draw = *before;
    for (uint32_t t = 0u; t < draw->semantic.triangle_count; ++t)
        for (uint32_t v = 0u; v < 3u; ++v) {
            GpuRenderSemanticVertex *p = &draw->semantic.triangles[t].vertices[v];
            if (!p->interpolation_vertex_identity_valid) return 0;
            const GpuRenderSemanticVertex *now = native_coverage_vertex(&coverage->view,
                draw->temporal_component, p->interpolation_group_id, p->interpolation_vertex_id);
            if (!now || !now->projective_position || now->projective_view_z <= 0 ||
                !p->projective_position || p->projective_view_z <= 0 ||
                now->native_view_position != p->native_view_position) return 0;
            /* Preserve authored topology/material/UVs; replace ONLY positions
             * with this invocation's source samples and the recipe relocation. */
#define REPROJECT(field, offset) do { \
    const int64_t value = (int64_t)now->field + (offset); \
    if (value < INT32_MIN || value > INT32_MAX) return 0; \
    p->field = (int32_t)value; \
} while (0)
            REPROJECT(x, draw->projection_offset_x);
            REPROJECT(y, draw->projection_offset_y);
            REPROJECT(native_view_x, draw->projection_offset_x);
            REPROJECT(native_view_y, draw->projection_offset_y);
            REPROJECT(projective_offset_x, draw->projection_offset_x);
            REPROJECT(projective_offset_y, draw->projection_offset_y);
#undef REPROJECT
            p->projective_view_x = now->projective_view_x;
            p->projective_view_y = now->projective_view_y;
            p->projective_view_z = now->projective_view_z;
            p->native_view_depth = now->native_view_depth;
            p->projective_distance = now->projective_distance;
        }
    draw->temporal_departure = 1u;
    return 1;
}

static int native_motion_departures(const GlNativeRecipe *previous, const GlNativeRecipe *current,
                                    GlNativeRecipe **out) {
    *out = NULL;
    if (!previous->publication || !current->publication) return 1;
    GlNativeDepartureKey *keys = malloc((current->count ? current->count : 1u) * sizeof(*keys));
    if (!keys) return 0;
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < current->count; ++i)
        if (current->draws[i].semantic.interpolation_identity.valid)
            keys[count++] = (GlNativeDepartureKey){current->draws[i].semantic.interpolation_identity, i};
    qsort(keys, count, sizeof(*keys), native_departure_key_compare);
    GlNativeViewTarget phase = {0};
    /* Walk backwards so an inserted departure precedes the next surviving draw
     * in the previous OT order. Current draws retain their exact relative order. */
    uint32_t successor = current->count;
    for (uint32_t i = previous->count; i-- > 0u;) {
        const GlNativeRecipeDraw *before = &previous->draws[i];
        const GlNativeDepartureKey key = {before->semantic.interpolation_identity, 0u};
        const GlNativeDepartureKey *match = key.identity.valid
            ? bsearch(&key, keys, count, sizeof(*keys), native_departure_key_compare) : NULL;
        if (match) {
            successor = match->index;
            continue;
        }
        if (!before->temporal_component || !key.identity.valid ||
            before->temporal_index >= previous->coverage_count ||
            before->semantic.topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
            before->semantic.screen_space_2d || before->semantic.native_view_effect ||
            native_departure_outside(previous, before)) continue;
        const GlNativeRecipeCoverage *old = &previous->coverages[before->temporal_index];
        const GlNativeRecipeCoverage *now = native_coverage_recipe_scope(
            current, old->view.header->producer_scope);
        if (!now || memcmp(&now->predecessor, &old->reference, sizeof(old->reference))) continue;
        const XgRenderTemporalComponent *a = native_coverage_component(&old->view, before->temporal_component);
        const XgRenderTemporalComponent *b = native_coverage_component(&now->view, before->temporal_component);
        if (!a || !b || !xg_render_temporal_components_compatible(old->view.header, a, now->view.header, b)) continue;
        GlNativeRecipeDraw draw;
        if (!native_departure_reproject(before, now, &draw) ||
            !native_departure_outside(current, &draw)) continue;
        if (draw.motion.motion.handle.resource_id) {
            const XgRenderMotionPose *old_pose, *new_pose;
            if (!xg_render_motion_view(draw.motion.motion, &old_pose)) continue;
            uint32_t j = 0u;
            for (; j < current->motion_count; ++j)
                if (xg_render_motion_view(current->motions[j], &new_pose) && new_pose->entity_id == old_pose->entity_id) break;
            if (j == current->motion_count) continue;
            draw.motion.motion = current->motions[j];
            draw.motion_index = j;
        }
        if (!phase.recipe) {
            phase.recipe = (GlNativeRecipe *)current;
            phase.recipe->references++;
            if (!native_recipe_private(&phase)) goto failed;
        }
        GlNativeRecipe *recipe = phase.recipe;
        if (recipe->count == UINT32_MAX) goto failed;
        GlNativeRecipeDraw *draws = xg_render_array_reserve(recipe->draws,
            sizeof(*draws), &recipe->draw_capacity, recipe->count + 1u, UINT32_MAX);
        if (!draws) goto failed;
        recipe->draws = draws;
        uint32_t coverage = 0u;
        for (; coverage < recipe->coverage_count; ++coverage)
            if (!memcmp(&recipe->coverages[coverage].reference, &now->reference, sizeof(old->reference))) break;
        if (coverage == recipe->coverage_count) {
            if (coverage == UINT32_MAX) goto failed;
            GlNativeRecipeCoverage *coverages = xg_render_array_reserve(recipe->coverages,
                sizeof(*coverages), &recipe->coverage_capacity, coverage + 1u, UINT32_MAX);
            if (!coverages) goto failed;
            recipe->coverages = coverages;
            if (!native_coverage_acquire_ref(now->reference)) goto failed;
            recipe->coverages[recipe->coverage_count++] = *now;
        }
        draw.temporal_index = coverage;
        if (draw.textures) draw.textures->references++;
        memmove(&recipe->draws[successor + 1u], &recipe->draws[successor],
            (recipe->count - successor) * sizeof(*recipe->draws));
        recipe->draws[successor] = draw;
        recipe->count++;
        for (uint32_t j = 0u; j < count; ++j)
            if (keys[j].index >= successor) keys[j].index++;
    }
    free(keys);
    *out = phase.recipe;
    return 1;
failed:
    free(keys);
    native_recipe_release(phase.recipe);
    return 0;
}
