#include "gpu_semantic_workload.h"

#include <limits.h>
#include <string.h>

_Static_assert(GPU_SEMANTIC_WORKLOAD_CAPACITY >= 4096u,
               "online workloads require at least 4096 entries");
_Static_assert(GPU_SEMANTIC_WORKLOAD_CAPACITY <= INT32_MAX,
               "match indices must fit in int32_t");

typedef struct GpuSemanticFrame {
    GpuRenderSemantic items[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    uint8_t participation[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    size_t count;
    size_t participating_count;
    size_t authoritative_count;
    size_t unkeyed_count;
    bool overflowed;
    bool conflicted;
} GpuSemanticFrame;

typedef struct GpuSemanticPhasePosition {
    GpuRenderFixed16_16 x;
    GpuRenderFixed16_16 y;
    GpuRenderFixed16_16 native_x;
    GpuRenderFixed16_16 native_y;
} GpuSemanticPhasePosition;

#define GPU_SEMANTIC_ANCHOR_CAPACITY 32768u
typedef struct GpuSemanticAnchorFrame {
    GpuRenderInterpolationVertexAnchor items[GPU_SEMANTIC_ANCHOR_CAPACITY];
    size_t count;
    bool overflowed;
} GpuSemanticAnchorFrame;

#define GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY 8192u
#define GPU_SEMANTIC_VERTEX_HASH_CAPACITY 65536u
#define GPU_SEMANTIC_MAX_VERTICES \
    (GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY * 3u)
#define GPU_SEMANTIC_RETROSPECTIVE_CANDIDATE_LIMIT 1024u
/* Unkeyed matching is a fail-closed visual fallback. Bound aggregate work to
 * one candidate per maximum-capacity item so dense scenes snap excess
 * primitives instead of turning a nominally linear frame into quadratic work. */
#define GPU_SEMANTIC_RETROSPECTIVE_FRAME_CANDIDATE_LIMIT \
    GPU_SEMANTIC_WORKLOAD_CAPACITY
#define GPU_SEMANTIC_RETROSPECTIVE_AMBIGUITY_MARGIN 64u
#define GPU_SEMANTIC_RETROSPECTIVE_TRANSLATION_LIMIT 64u

_Static_assert((GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY &
                (GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY - 1u)) == 0u,
               "workload hash capacity must be a power of two");
_Static_assert(GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY >=
                   GPU_SEMANTIC_WORKLOAD_CAPACITY * 2u,
               "workload hash table needs bounded probe headroom");
_Static_assert((GPU_SEMANTIC_VERTEX_HASH_CAPACITY &
                (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u)) == 0u,
               "vertex hash capacity must be a power of two");
_Static_assert(GPU_SEMANTIC_VERTEX_HASH_CAPACITY >=
                   GPU_SEMANTIC_WORKLOAD_CAPACITY *
                       GPU_SEMANTIC_MAX_VERTICES * 2u,
               "vertex hash table needs bounded probe headroom");

static struct {
    GpuSemanticFrame frames[2];
    int32_t previous_match[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    int32_t previous_hash[GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY];
    int32_t current_hash[GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY];
    int32_t previous_retrospective_head[GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY];
    int32_t previous_retrospective_next[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    int32_t previous_vertex_hash[GPU_SEMANTIC_VERTEX_HASH_CAPACITY];
    int32_t previous_anchor_hash[GPU_SEMANTIC_VERTEX_HASH_CAPACITY];
    int32_t current_anchor_hash[GPU_SEMANTIC_VERTEX_HASH_CAPACITY];
    int32_t phase_vertex_hash[GPU_SEMANTIC_VERTEX_HASH_CAPACITY];
    uint16_t previous_hash_touched[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    uint16_t current_hash_touched[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    uint16_t previous_retrospective_touched[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    uint16_t previous_vertex_hash_touched[GPU_SEMANTIC_MAX_VERTICES *
                                          GPU_SEMANTIC_WORKLOAD_CAPACITY];
    uint16_t previous_anchor_hash_touched[GPU_SEMANTIC_ANCHOR_CAPACITY];
    uint16_t current_anchor_hash_touched[GPU_SEMANTIC_ANCHOR_CAPACITY];
    uint16_t phase_vertex_hash_touched[GPU_SEMANTIC_MAX_VERTICES *
                                       GPU_SEMANTIC_WORKLOAD_CAPACITY];
    size_t previous_hash_touched_count;
    size_t current_hash_touched_count;
    size_t previous_retrospective_touched_count;
    size_t previous_vertex_hash_touched_count;
    size_t previous_anchor_hash_touched_count;
    size_t current_anchor_hash_touched_count;
    size_t phase_vertex_hash_touched_count;
    GpuSemanticPhasePosition phase_positions
        [GPU_SEMANTIC_INTERPOLATION_MAX_PHASES]
        [GPU_SEMANTIC_WORKLOAD_CAPACITY * GPU_SEMANTIC_MAX_VERTICES];
    bool previous_used[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    bool previous_corresponded[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    bool current_moved[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    bool source_geometry_match[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    GpuSemanticWorkloadMatchKind match_kind[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    GpuSemanticWorkloadMatchKind fallback_kind[GPU_SEMANTIC_WORKLOAD_CAPACITY];
    GpuSemanticAnchorFrame anchor_frames[2];
    unsigned int sealed_index;
    unsigned int building_index;
    bool has_sealed;
    bool building;
    GpuSemanticWorkloadDiagnostics diagnostics;
    GpuSemanticWorkloadMotionDiagnostics last_motion;
} workload;
static uint64_t workload_epoch;

static void clear_touched_hash(int32_t *table, const uint16_t *touched,
                               size_t *count) {
    for (size_t index = 0u; index < *count; ++index)
        table[touched[index]] = 0;
    *count = 0u;
}

static void clear_workload_hashes(void) {
    clear_touched_hash(workload.previous_hash,
                       workload.previous_hash_touched,
                       &workload.previous_hash_touched_count);
    clear_touched_hash(workload.current_hash,
                       workload.current_hash_touched,
                       &workload.current_hash_touched_count);
    clear_touched_hash(workload.previous_retrospective_head,
                       workload.previous_retrospective_touched,
                       &workload.previous_retrospective_touched_count);
    clear_touched_hash(workload.previous_vertex_hash,
                       workload.previous_vertex_hash_touched,
                       &workload.previous_vertex_hash_touched_count);
    clear_touched_hash(workload.previous_anchor_hash,
                       workload.previous_anchor_hash_touched,
                       &workload.previous_anchor_hash_touched_count);
    clear_touched_hash(workload.current_anchor_hash,
                       workload.current_anchor_hash_touched,
                       &workload.current_anchor_hash_touched_count);
    clear_touched_hash(workload.phase_vertex_hash,
                       workload.phase_vertex_hash_touched,
                       &workload.phase_vertex_hash_touched_count);
}

static bool identity_equal(const GpuRenderInterpolationIdentity *a,
                           const GpuRenderInterpolationIdentity *b) {
    return a->valid && b->valid && a->scene_id == b->scene_id &&
           a->producer_id == b->producer_id &&
           a->primitive_id == b->primitive_id;
}

static size_t identity_hash(const GpuRenderInterpolationIdentity *identity) {
    uint64_t value = identity->scene_id;

    value ^= ((uint64_t)identity->producer_id << 32u) |
             identity->primitive_id;

    value ^= value >> 33u;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33u;
    return (size_t)value & (GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY - 1u);
}

static size_t hash_entry_index(int32_t entry) {
    return entry > 0 ? (size_t)(entry - 1) : (size_t)(-entry - 1);
}

static bool hash_insert(int32_t *table, uint16_t *touched,
                        size_t *touched_count,
                        const GpuSemanticFrame *frame, size_t item_index) {
    const GpuRenderInterpolationIdentity *identity =
        &frame->items[item_index].interpolation_identity;
    size_t slot;

    if (!identity->valid) return true;
    slot = identity_hash(identity);
    for (size_t probe = 0u; probe < GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY;
         ++probe) {
        const int32_t entry = table[slot];

        if (entry == 0) {
            touched[(*touched_count)++] = (uint16_t)slot;
            table[slot] = (int32_t)item_index + 1;
            return true;
        }
        if (identity_equal(
                identity,
                &frame->items[hash_entry_index(entry)].interpolation_identity)) {
            if (entry > 0) table[slot] = -entry;
            return false;
        }
        slot = (slot + 1u) & (GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY - 1u);
    }
    return false;
}

static int32_t hash_lookup(const int32_t *table,
                           const GpuSemanticFrame *frame,
                           const GpuRenderInterpolationIdentity *identity,
                           bool *out_ambiguous) {
    size_t slot;

    *out_ambiguous = false;
    if (!identity->valid) return -1;
    slot = identity_hash(identity);
    for (size_t probe = 0u; probe < GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY;
         ++probe) {
        const int32_t entry = table[slot];
        size_t item_index;

        if (entry == 0) return -1;
        item_index = hash_entry_index(entry);
        if (identity_equal(identity,
                           &frame->items[item_index].interpolation_identity)) {
            if (entry < 0) {
                *out_ambiguous = true;
                return -1;
            }
            return (int32_t)item_index;
        }
        slot = (slot + 1u) & (GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY - 1u);
    }
    *out_ambiguous = true;
    return -1;
}

static bool semantic_valid(const GpuRenderSemantic *semantic) {
    if (semantic->native_view_effect >
            GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID ||
        (semantic->native_view_effect == GPU_RENDER_NATIVE_VIEW_EFFECT_NONE &&
         semantic->native_view_effect_index != 0u) ||
        (semantic->native_view_effect ==
             GPU_RENDER_NATIVE_VIEW_EFFECT_WAVE_GRID &&
         semantic->native_view_effect_index >= 20u * 17u))
        return false;
    if (semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        return semantic->triangle_count > 0u &&
               semantic->triangle_count <= GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY &&
               semantic->line_count == 0u;
    }
    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        return semantic->line_count > 0u &&
               semantic->line_count <= GPU_RENDER_SEMANTIC_LINE_CAPACITY &&
               semantic->triangle_count == 0u;
    }
    return false;
}

static uint64_t magnitude_i64(int64_t value) {
    return value < 0
        ? (uint64_t)(-(value + 1)) + 1u
        : (uint64_t)value;
}

/* Coordinate deltas span 33 bits, so compare the two 66-bit products by
 * sign and magnitude instead of overflowing a signed cross product. */
static int product_difference_sign(int64_t a, int64_t b,
                                   int64_t c, int64_t d) {
    const int first_sign = a == 0 || b == 0
        ? 0 : ((a < 0) == (b < 0) ? 1 : -1);
    const int second_sign = c == 0 || d == 0
        ? 0 : ((c < 0) == (d < 0) ? 1 : -1);
    const uint64_t first_magnitude = magnitude_i64(a) * magnitude_i64(b);
    const uint64_t second_magnitude = magnitude_i64(c) * magnitude_i64(d);

    if (first_sign == 0) return -second_sign;
    if (second_sign == 0) return first_sign;
    if (first_sign != second_sign) return first_sign;
    if (first_magnitude == second_magnitude) return 0;
    if (first_sign > 0)
        return first_magnitude > second_magnitude ? 1 : -1;
    return first_magnitude > second_magnitude ? -1 : 1;
}

static int triangle_orientation(const GpuRenderSemanticTriangle *triangle,
                                bool native_view) {
    const GpuRenderSemanticVertex *v0 = &triangle->vertices[0];
    const GpuRenderSemanticVertex *v1 = &triangle->vertices[1];
    const GpuRenderSemanticVertex *v2 = &triangle->vertices[2];
    const int64_t x0 = native_view && v0->native_view_position
        ? v0->native_view_x : v0->x;
    const int64_t y0 = native_view && v0->native_view_position
        ? v0->native_view_y : v0->y;
    const int64_t x1 = native_view && v1->native_view_position
        ? v1->native_view_x : v1->x;
    const int64_t y1 = native_view && v1->native_view_position
        ? v1->native_view_y : v1->y;
    const int64_t x2 = native_view && v2->native_view_position
        ? v2->native_view_x : v2->x;
    const int64_t y2 = native_view && v2->native_view_position
        ? v2->native_view_y : v2->y;

    return product_difference_sign(
        x1 - x0, y2 - y0, y1 - y0, x2 - x0);
}

static bool triangle_winding_compatible(
        const GpuRenderSemanticTriangle *a,
        const GpuRenderSemanticTriangle *b) {
    const int canonical_a = triangle_orientation(a, false);
    const int canonical_b = triangle_orientation(b, false);
    const int native_a = triangle_orientation(a, true);
    const int native_b = triangle_orientation(b, true);

    return (canonical_a == 0 || canonical_b == 0 ||
            canonical_a == canonical_b) &&
           (native_a == 0 || native_b == 0 || native_a == native_b);
}

static bool retrospective_material_compatible(const GpuRenderMaterial *a,
                                               const GpuRenderMaterial *b);

/* A UV change selects different texture content. Geometry may interpolate only
 * while the sampled footprint remains identical between source frames. */
static bool texture_footprint_compatible(
        const GpuRenderSemantic *a, const GpuRenderSemantic *b) {
    size_t primitive;
    size_t vertex;

    if (!a->material.textured) return true;
    if (a->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        for (primitive = 0u; primitive < a->triangle_count; ++primitive)
            for (vertex = 0u; vertex < 3u; ++vertex)
                if (a->triangles[primitive].vertices[vertex].u !=
                        b->triangles[primitive].vertices[vertex].u ||
                    a->triangles[primitive].vertices[vertex].v !=
                        b->triangles[primitive].vertices[vertex].v)
                    return false;
    } else {
        for (primitive = 0u; primitive < a->line_count; ++primitive)
            for (vertex = 0u; vertex < 2u; ++vertex)
                if (a->lines[primitive].vertices[vertex].u !=
                        b->lines[primitive].vertices[vertex].u ||
                    a->lines[primitive].vertices[vertex].v !=
                        b->lines[primitive].vertices[vertex].v)
                    return false;
    }
    return true;
}

static bool semantic_compatible(const GpuRenderSemantic *a,
                                 const GpuRenderSemantic *b) {
    size_t primitive;
    size_t vertex;

    if (!semantic_valid(a) || !semantic_valid(b) ||
        a->topology != b->topology ||
        a->screen_space_2d != b->screen_space_2d ||
        a->native_view_effect != b->native_view_effect ||
        a->native_view_effect_index != b->native_view_effect_index ||
        a->triangle_count != b->triangle_count ||
        a->line_count != b->line_count ||
        !retrospective_material_compatible(&a->material, &b->material) ||
        !texture_footprint_compatible(a, b)) {
        return false;
    }

    if (a->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        for (primitive = 0; primitive < a->triangle_count; ++primitive) {
            if (a->triangles[primitive].split_index !=
                    b->triangles[primitive].split_index ||
                a->triangles[primitive].split_count !=
                    b->triangles[primitive].split_count ||
                !triangle_winding_compatible(&a->triangles[primitive],
                                             &b->triangles[primitive])) {
                return false;
            }
            for (vertex = 0; vertex < 3u; ++vertex) {
                if (a->triangles[primitive].vertices[vertex]
                        .native_view_position !=
                    b->triangles[primitive].vertices[vertex]
                        .native_view_position) {
                    return false;
                }
            }
        }
    } else {
        for (primitive = 0; primitive < a->line_count; ++primitive) {
            for (vertex = 0; vertex < 2u; ++vertex) {
                if (a->lines[primitive].vertices[vertex]
                        .native_view_position !=
                    b->lines[primitive].vertices[vertex]
                        .native_view_position) {
                    return false;
                }
            }
        }
    }
    return true;
}

static size_t semantic_vertex_count(const GpuRenderSemantic *semantic) {
    return semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES
        ? (size_t)semantic->triangle_count * 3u
        : (size_t)semantic->line_count * 2u;
}

static const GpuRenderSemanticVertex *semantic_vertex_at(
        const GpuRenderSemantic *semantic, size_t index) {
    if (semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES)
        return &semantic->triangles[index / 3u].vertices[index % 3u];
    return &semantic->lines[index / 2u].vertices[index % 2u];
}

static GpuRenderSemanticVertex *semantic_vertex_at_mutable(
        GpuRenderSemantic *semantic, size_t index) {
    if (semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES)
        return &semantic->triangles[index / 3u].vertices[index % 3u];
    return &semantic->lines[index / 2u].vertices[index % 2u];
}

static void record_motion_diagnostics(
        size_t current_order, size_t previous_order,
        const GpuRenderSemantic *previous,
        const GpuRenderSemantic *current,
        const GpuRenderSemantic *midpoint,
        size_t position_changed_vertex_count,
        uint64_t position_delta_fixed) {
    workload.last_motion = (GpuSemanticWorkloadMotionDiagnostics){
        .epoch = workload.diagnostics.epoch,
        .source_frame = workload.diagnostics.sealed_frames + 1u,
        .sequence = workload.diagnostics.total_recorded + 1u,
        .match_kind = workload.match_kind[current_order],
        .fallback_kind = workload.fallback_kind[current_order],
        .current_order = current_order,
        .previous_order = previous_order,
        .position_changed_vertex_count = position_changed_vertex_count,
        .position_delta_fixed = position_delta_fixed,
        .current = *current,
        .midpoint = *midpoint,
        .valid = true,
        .previous_valid = previous != NULL,
    };
    if (previous != NULL) workload.last_motion.previous = *previous;
}

static bool semantic_has_projective_position(
        const GpuRenderSemantic *semantic) {
    const size_t count = semantic_vertex_count(semantic);

    for (size_t index = 0u; index < count; ++index) {
        if (semantic_vertex_at(semantic, index)->projective_position)
            return true;
    }
    return false;
}

static bool semantic_is_retirable_mesh(
        const GpuRenderSemantic *semantic) {
    const size_t count = semantic_vertex_count(semantic);

    if (!semantic->interpolation_identity.valid ||
        semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->material.semi_transparent || count == 0u ||
        !semantic_has_projective_position(semantic))
        return false;
    for (size_t index = 0u; index < count; ++index)
        if (!semantic_vertex_at(semantic, index)
                 ->interpolation_vertex_identity_valid)
            return false;
    return true;
}

static size_t vertex_identity_hash(
        const GpuRenderSemantic *semantic,
        const GpuRenderSemanticVertex *vertex) {
    uint64_t value = semantic->interpolation_identity.scene_id;

    value ^= (uint64_t)semantic->interpolation_identity.producer_id << 32u;
    value ^= ((uint64_t)vertex->interpolation_group_id << 32u) |
             vertex->interpolation_vertex_id;
    value ^= value >> 33u;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33u;
    return (size_t)value & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
}

static size_t anchor_identity_hash(
        const GpuRenderInterpolationVertexAnchor *anchor) {
    uint64_t value = anchor->scene_id;

    value ^= (uint64_t)anchor->producer_id << 32u;
    value ^= ((uint64_t)anchor->vertex.interpolation_group_id << 32u) |
             anchor->vertex.interpolation_vertex_id;
    value ^= value >> 33u;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33u;
    return (size_t)value & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
}

static bool anchor_identity_equal(
        const GpuRenderInterpolationVertexAnchor *anchor,
        const GpuRenderSemantic *semantic,
        const GpuRenderSemanticVertex *vertex) {
    return anchor->producer_id ==
               semantic->interpolation_identity.producer_id &&
           anchor->scene_id == semantic->interpolation_identity.scene_id &&
           anchor->vertex.interpolation_vertex_identity_valid &&
           vertex->interpolation_vertex_identity_valid &&
           anchor->vertex.interpolation_group_id ==
               vertex->interpolation_group_id &&
           anchor->vertex.interpolation_vertex_id ==
               vertex->interpolation_vertex_id;
}

static bool vertex_identity_equal(
        const GpuRenderSemantic *a_semantic,
        const GpuRenderSemanticVertex *a,
        const GpuRenderSemantic *b_semantic,
        const GpuRenderSemanticVertex *b) {
    return a->interpolation_vertex_identity_valid &&
           b->interpolation_vertex_identity_valid &&
           a_semantic->interpolation_identity.scene_id ==
               b_semantic->interpolation_identity.scene_id &&
           a_semantic->interpolation_identity.producer_id ==
               b_semantic->interpolation_identity.producer_id &&
           a->interpolation_group_id == b->interpolation_group_id &&
           a->interpolation_vertex_id == b->interpolation_vertex_id;
}

static size_t vertex_hash_entry_index(int32_t entry) {
    return entry > 0 ? (size_t)(entry - 1) : (size_t)(-entry - 1);
}

static void previous_vertex_hash_insert(const GpuSemanticFrame *frame,
                                        size_t item_index,
                                        size_t vertex_index) {
    const GpuRenderSemantic *semantic = &frame->items[item_index];
    const GpuRenderSemanticVertex *vertex =
        semantic_vertex_at(semantic, vertex_index);
    const size_t flat_index = item_index * GPU_SEMANTIC_MAX_VERTICES +
        vertex_index;
    size_t slot;

    if (!vertex->interpolation_vertex_identity_valid) return;
    slot = vertex_identity_hash(semantic, vertex);
    for (size_t probe = 0u; probe < GPU_SEMANTIC_VERTEX_HASH_CAPACITY;
         ++probe) {
        const int32_t entry = workload.previous_vertex_hash[slot];

        if (entry == 0) {
            workload.previous_vertex_hash_touched[
                workload.previous_vertex_hash_touched_count++] =
                    (uint16_t)slot;
            workload.previous_vertex_hash[slot] = (int32_t)flat_index + 1;
            return;
        }
        {
            const size_t existing_flat = vertex_hash_entry_index(entry);
            const size_t existing_item =
                existing_flat / GPU_SEMANTIC_MAX_VERTICES;
            const size_t existing_vertex =
                existing_flat % GPU_SEMANTIC_MAX_VERTICES;
            const GpuRenderSemantic *existing_semantic =
                &frame->items[existing_item];
            const GpuRenderSemanticVertex *existing =
                semantic_vertex_at(existing_semantic, existing_vertex);

            if (vertex_identity_equal(
                    semantic, vertex, existing_semantic, existing))
                return;
        }
        slot = (slot + 1u) & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
    }
}

static void anchor_hash_insert(
        int32_t *table, uint16_t *touched, size_t *touched_count,
        const GpuSemanticAnchorFrame *frame, size_t anchor_index) {
    const GpuRenderInterpolationVertexAnchor *anchor =
        &frame->items[anchor_index];
    size_t slot;

    if (anchor->producer_id == 0u ||
        !anchor->vertex.interpolation_vertex_identity_valid)
        return;
    slot = anchor_identity_hash(anchor);
    for (size_t probe = 0u; probe < GPU_SEMANTIC_VERTEX_HASH_CAPACITY;
         ++probe) {
        const int32_t entry = table[slot];

        if (entry == 0) {
            touched[(*touched_count)++] = (uint16_t)slot;
            table[slot] = (int32_t)anchor_index + 1;
            return;
        }
        {
            const GpuRenderInterpolationVertexAnchor *existing =
                &frame->items[(size_t)(entry - 1)];

            if (existing->scene_id == anchor->scene_id &&
                existing->producer_id == anchor->producer_id &&
                existing->vertex.interpolation_group_id ==
                    anchor->vertex.interpolation_group_id &&
                existing->vertex.interpolation_vertex_id ==
                    anchor->vertex.interpolation_vertex_id)
                return;
        }
        slot = (slot + 1u) & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
    }
}

static bool anchor_lookup(
        const int32_t *table, const GpuSemanticAnchorFrame *anchors,
        const GpuRenderSemantic *semantic,
        const GpuRenderSemanticVertex *vertex,
        const GpuRenderInterpolationVertexAnchor **out_anchor) {
    const GpuRenderInterpolationVertexAnchor key = {
        .scene_id = semantic->interpolation_identity.scene_id,
        .producer_id = semantic->interpolation_identity.producer_id,
        .vertex = *vertex,
    };
    size_t slot;

    if (!vertex->interpolation_vertex_identity_valid) return false;
    slot = anchor_identity_hash(&key);
    for (size_t probe = 0u; probe < GPU_SEMANTIC_VERTEX_HASH_CAPACITY;
         ++probe) {
        const int32_t entry = table[slot];
        const GpuRenderInterpolationVertexAnchor *candidate;

        if (entry == 0) return false;
        candidate = &anchors->items[(size_t)(entry - 1)];
        if (anchor_identity_equal(candidate, semantic, vertex)) {
            *out_anchor = candidate;
            return true;
        }
        slot = (slot + 1u) & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
    }
    return false;
}

static bool previous_vertex_lookup(
        const GpuRenderSemantic *semantic,
        const GpuRenderSemanticVertex *vertex,
        const GpuRenderSemanticVertex **out_previous_vertex,
        const GpuRenderMaterial **out_previous_material) {
    const GpuSemanticFrame *previous;
    size_t slot;

    if (!workload.has_sealed || !vertex->interpolation_vertex_identity_valid)
        return false;
    previous = &workload.frames[
        workload.building ? workload.sealed_index : workload.sealed_index ^ 1u];
    {
        const GpuSemanticAnchorFrame *anchors = &workload.anchor_frames[
            workload.building ? workload.sealed_index
                              : workload.sealed_index ^ 1u];
        const GpuRenderInterpolationVertexAnchor *anchor;

        if (anchor_lookup(workload.previous_anchor_hash, anchors,
                          semantic, vertex, &anchor)) {
            *out_previous_vertex = &anchor->vertex;
            *out_previous_material = &anchor->material;
            return true;
        }
    }
    slot = vertex_identity_hash(semantic, vertex);
    for (size_t probe = 0u; probe < GPU_SEMANTIC_VERTEX_HASH_CAPACITY;
         ++probe) {
        const int32_t entry = workload.previous_vertex_hash[slot];
        size_t flat_index;
        size_t item_index;
        size_t vertex_index;
        const GpuRenderSemantic *candidate_semantic;
        const GpuRenderSemanticVertex *candidate;

        if (entry == 0) return false;
        flat_index = vertex_hash_entry_index(entry);
        item_index = flat_index / GPU_SEMANTIC_MAX_VERTICES;
        vertex_index = flat_index % GPU_SEMANTIC_MAX_VERTICES;
        candidate_semantic = &previous->items[item_index];
        candidate = semantic_vertex_at(candidate_semantic, vertex_index);
        if (vertex_identity_equal(
                semantic, vertex, candidate_semantic, candidate)) {
            *out_previous_vertex = candidate;
            *out_previous_material = &candidate_semantic->material;
            return true;
        }
        slot = (slot + 1u) & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
    }
    return false;
}

static uint64_t abs_difference_i64(int64_t a, int64_t b) {
    return a >= b ? (uint64_t)(a - b) : (uint64_t)(b - a);
}

static int64_t interpolate_i64(int64_t previous, int64_t current,
                               unsigned int numerator,
                               unsigned int denominator) {
    return (previous * (denominator - numerator) + current * numerator) /
           denominator;
}

static void record_semantic_position_delta(
        const GpuRenderSemantic *semantic, uint64_t position_delta_fixed) {
    const uint64_t vertex_count = semantic_vertex_count(semantic);
    const uint64_t pixel_unit = UINT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS;

    if (!semantic->interpolation_identity.valid && position_delta_fixed != 0u &&
        vertex_count != 0u) {
        ++workload.diagnostics.total_unkeyed_moved_matches;
        if (position_delta_fixed > vertex_count * 32u * pixel_unit)
            ++workload.diagnostics.total_unkeyed_motion_over_32px;
        if (position_delta_fixed > vertex_count * 64u * pixel_unit)
            ++workload.diagnostics.total_unkeyed_motion_over_64px;
        if (position_delta_fixed > vertex_count * 128u * pixel_unit)
            ++workload.diagnostics.total_unkeyed_motion_over_128px;
        if (position_delta_fixed > vertex_count * 192u * pixel_unit)
            ++workload.diagnostics.total_unkeyed_motion_over_192px;
        if (position_delta_fixed > vertex_count * 240u * pixel_unit)
            ++workload.diagnostics.total_unkeyed_motion_over_240px;
    } else if (semantic->interpolation_identity.valid &&
               position_delta_fixed != 0u && vertex_count != 0u) {
        ++workload.diagnostics.total_keyed_moved_matches;
        if (position_delta_fixed > vertex_count * 32u * pixel_unit)
            ++workload.diagnostics.total_keyed_motion_over_32px;
        if (position_delta_fixed > vertex_count * 64u * pixel_unit)
            ++workload.diagnostics.total_keyed_motion_over_64px;
        if (position_delta_fixed > vertex_count * 128u * pixel_unit)
            ++workload.diagnostics.total_keyed_motion_over_128px;
        if (position_delta_fixed > vertex_count * 192u * pixel_unit)
            ++workload.diagnostics.total_keyed_motion_over_192px;
        if (position_delta_fixed > vertex_count * 240u * pixel_unit)
            ++workload.diagnostics.total_keyed_motion_over_240px;
        if (position_delta_fixed >
            workload.diagnostics.max_keyed_semantic_position_delta_fixed) {
            workload.diagnostics.max_keyed_semantic_position_delta_fixed =
                position_delta_fixed;
            workload.diagnostics.max_keyed_semantic_identity_scene =
                semantic->interpolation_identity.scene_id;
            workload.diagnostics.max_keyed_semantic_identity_producer =
                semantic->interpolation_identity.producer_id;
            workload.diagnostics.max_keyed_semantic_identity_primitive =
                semantic->interpolation_identity.primitive_id;
        }
    }
    if (position_delta_fixed <=
        workload.diagnostics.max_semantic_position_delta_fixed)
        return;
    workload.diagnostics.max_semantic_position_delta_fixed =
        position_delta_fixed;
    workload.diagnostics.max_semantic_identity_scene =
        semantic->interpolation_identity.scene_id;
    workload.diagnostics.max_semantic_identity_producer =
        semantic->interpolation_identity.producer_id;
    workload.diagnostics.max_semantic_identity_primitive =
        semantic->interpolation_identity.primitive_id;
    workload.diagnostics.max_semantic_identity_valid =
        semantic->interpolation_identity.valid != 0u;
}

static int64_t target_relocation(int current_origin, int current_end,
                                 int current_offset, int previous_origin,
                                 int previous_end, int previous_offset) {
    const int64_t delta = (int64_t)current_origin - previous_origin;

    if ((int64_t)current_end - current_origin !=
            (int64_t)previous_end - previous_origin ||
        delta == 0 || delta != (int64_t)current_offset - previous_offset)
        return 0;
    return delta * (INT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS);
}

static bool retrospective_material_compatible(const GpuRenderMaterial *a,
                                               const GpuRenderMaterial *b) {
    const int64_t draw_area_delta_x =
        (int64_t)a->draw_area_left - b->draw_area_left;
    const int64_t draw_area_delta_y =
        (int64_t)a->draw_area_top - b->draw_area_top;

    return a->tpage == b->tpage &&
           a->texture_page_x == b->texture_page_x &&
           a->texture_page_y == b->texture_page_y &&
           a->clut_x == b->clut_x && a->clut_y == b->clut_y &&
           a->texture_depth == b->texture_depth &&
           a->texture_window_mask_x == b->texture_window_mask_x &&
           a->texture_window_mask_y == b->texture_window_mask_y &&
           a->texture_window_offset_x == b->texture_window_offset_x &&
           a->texture_window_offset_y == b->texture_window_offset_y &&
           a->shading == b->shading && a->textured == b->textured &&
           a->raw_texture == b->raw_texture &&
           a->semi_transparent == b->semi_transparent &&
           a->blend_mode == b->blend_mode && a->dither == b->dither &&
           a->mask_set == b->mask_set && a->mask_check == b->mask_check &&
           a->draw_area_right - a->draw_area_left ==
               b->draw_area_right - b->draw_area_left &&
           a->draw_area_bottom - a->draw_area_top ==
               b->draw_area_bottom - b->draw_area_top &&
           (draw_area_delta_x == 0 ||
            draw_area_delta_x ==
                (int64_t)a->draw_offset_x - b->draw_offset_x) &&
           (draw_area_delta_y == 0 ||
            draw_area_delta_y ==
                (int64_t)a->draw_offset_y - b->draw_offset_y);
}

static bool retrospective_appearance_compatible(
        const GpuRenderSemantic *current,
        const GpuRenderSemantic *previous) {
    const size_t count = semantic_vertex_count(current);
    const uint64_t uv_limit = current->material.textured ? count * 32u : 0u;
    const uint64_t color_limit = count * 192u;
    uint64_t uv_delta = 0u;
    uint64_t color_delta = 0u;

    for (size_t index = 0u; index < count; ++index) {
        const GpuRenderSemanticVertex *a = semantic_vertex_at(current, index);
        const GpuRenderSemanticVertex *b = semantic_vertex_at(previous, index);

        uv_delta += abs_difference_i64(a->u, b->u) >>
            GPU_RENDER_FIXED_FRACTION_BITS;
        uv_delta += abs_difference_i64(a->v, b->v) >>
            GPU_RENDER_FIXED_FRACTION_BITS;
        color_delta += abs_difference_i64(a->r, b->r);
        color_delta += abs_difference_i64(a->g, b->g);
        color_delta += abs_difference_i64(a->b, b->b);
        if (uv_delta > uv_limit || color_delta > color_limit) return false;
    }
    return true;
}

static bool retrospective_compatible(const GpuRenderSemantic *current,
                                      const GpuRenderSemantic *previous) {
    return current->interpolation_identity.scene_id ==
               previous->interpolation_identity.scene_id &&
            semantic_compatible(current, previous) &&
            retrospective_appearance_compatible(current, previous);
}

static uint64_t retrospective_hash_mix(uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6u) +
            (hash >> 2u);
    return hash;
}

static size_t retrospective_class_hash(const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material = &semantic->material;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);

#define MIX_RETROSPECTIVE(value) \
    hash = retrospective_hash_mix(hash, (uint64_t)(value))
    MIX_RETROSPECTIVE(semantic->interpolation_identity.scene_id);
    MIX_RETROSPECTIVE(semantic->topology);
    MIX_RETROSPECTIVE(semantic->screen_space_2d);
    MIX_RETROSPECTIVE(semantic->native_view_effect);
    MIX_RETROSPECTIVE(semantic->native_view_effect_index);
    MIX_RETROSPECTIVE(semantic->triangle_count);
    MIX_RETROSPECTIVE(semantic->line_count);
    MIX_RETROSPECTIVE(material->tpage);
    MIX_RETROSPECTIVE(material->texture_page_x);
    MIX_RETROSPECTIVE(material->texture_page_y);
    MIX_RETROSPECTIVE(material->clut_x);
    MIX_RETROSPECTIVE(material->clut_y);
    MIX_RETROSPECTIVE(material->texture_depth);
    MIX_RETROSPECTIVE(material->texture_window_mask_x);
    MIX_RETROSPECTIVE(material->texture_window_mask_y);
    MIX_RETROSPECTIVE(material->texture_window_offset_x);
    MIX_RETROSPECTIVE(material->texture_window_offset_y);
    MIX_RETROSPECTIVE(material->shading);
    MIX_RETROSPECTIVE(material->textured);
    MIX_RETROSPECTIVE(material->raw_texture);
    MIX_RETROSPECTIVE(material->semi_transparent);
    MIX_RETROSPECTIVE(material->blend_mode);
    MIX_RETROSPECTIVE(material->dither);
    MIX_RETROSPECTIVE(material->mask_set);
    MIX_RETROSPECTIVE(material->mask_check);
    MIX_RETROSPECTIVE(material->draw_area_right - material->draw_area_left);
    MIX_RETROSPECTIVE(material->draw_area_bottom - material->draw_area_top);
    if (semantic->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        for (size_t primitive = 0u; primitive < semantic->triangle_count;
             ++primitive) {
            MIX_RETROSPECTIVE(semantic->triangles[primitive].split_index);
            MIX_RETROSPECTIVE(semantic->triangles[primitive].split_count);
            for (size_t vertex = 0u; vertex < 3u; ++vertex) {
                MIX_RETROSPECTIVE(semantic->triangles[primitive]
                                      .vertices[vertex].native_view_position);
                if (material->textured) {
                    MIX_RETROSPECTIVE(semantic->triangles[primitive]
                                          .vertices[vertex].u);
                    MIX_RETROSPECTIVE(semantic->triangles[primitive]
                                          .vertices[vertex].v);
                }
            }
        }
    } else {
        for (size_t primitive = 0u; primitive < semantic->line_count;
             ++primitive)
            for (size_t vertex = 0u; vertex < 2u; ++vertex) {
                MIX_RETROSPECTIVE(semantic->lines[primitive]
                                      .vertices[vertex].native_view_position);
                if (material->textured) {
                    MIX_RETROSPECTIVE(semantic->lines[primitive]
                                          .vertices[vertex].u);
                    MIX_RETROSPECTIVE(semantic->lines[primitive]
                                          .vertices[vertex].v);
                }
            }
    }
#undef MIX_RETROSPECTIVE
    hash ^= hash >> 33u;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33u;
    return (size_t)hash & (GPU_SEMANTIC_WORKLOAD_HASH_CAPACITY - 1u);
}

static void retrospective_hash_insert(const GpuSemanticFrame *frame,
                                      size_t item_index) {
    const GpuRenderSemantic *semantic = &frame->items[item_index];
    const size_t slot = retrospective_class_hash(semantic);

    if (semantic->interpolation_identity.valid) return;
    if (workload.previous_retrospective_head[slot] == 0)
        workload.previous_retrospective_touched[
            workload.previous_retrospective_touched_count++] =
                (uint16_t)slot;
    workload.previous_retrospective_next[item_index] =
        workload.previous_retrospective_head[slot];
    workload.previous_retrospective_head[slot] = (int32_t)item_index + 1;
}

static bool retrospective_exact_equal(const GpuRenderSemantic *current,
                                       const GpuRenderSemantic *previous) {
    const int64_t unit = INT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS;
    const size_t count = semantic_vertex_count(current);
    const int64_t target_delta_x = target_relocation(
        current->material.draw_area_left, current->material.draw_area_right,
        current->material.draw_offset_x, previous->material.draw_area_left,
        previous->material.draw_area_right,
        previous->material.draw_offset_x);
    const int64_t target_delta_y = target_relocation(
        current->material.draw_area_top, current->material.draw_area_bottom,
        current->material.draw_offset_y, previous->material.draw_area_top,
        previous->material.draw_area_bottom,
        previous->material.draw_offset_y);

    if (count == 0u) return false;
    for (size_t index = 0u; index < count; ++index) {
        const GpuRenderSemanticVertex *a = semantic_vertex_at(current, index);
        const GpuRenderSemanticVertex *b = semantic_vertex_at(previous, index);
        const int64_t current_x = (int64_t)a->x +
            (int64_t)current->material.draw_offset_x * unit;
        const int64_t current_y = (int64_t)a->y +
            (int64_t)current->material.draw_offset_y * unit;
        const int64_t previous_x = (int64_t)b->x +
            (int64_t)previous->material.draw_offset_x * unit + target_delta_x;
        const int64_t previous_y = (int64_t)b->y +
            (int64_t)previous->material.draw_offset_y * unit + target_delta_y;

        if (current_x != previous_x || current_y != previous_y ||
            a->u != b->u || a->v != b->v ||
            a->r != b->r || a->g != b->g || a->b != b->b)
            return false;
        if (a->native_view_position) {
            const int64_t current_native_x = (int64_t)a->native_view_x +
                (int64_t)current->material.draw_offset_x * unit;
            const int64_t current_native_y = (int64_t)a->native_view_y +
                (int64_t)current->material.draw_offset_y * unit;
            const int64_t previous_native_x = (int64_t)b->native_view_x +
                (int64_t)previous->material.draw_offset_x * unit +
                target_delta_x;
            const int64_t previous_native_y = (int64_t)b->native_view_y +
                (int64_t)previous->material.draw_offset_y * unit +
                target_delta_y;

            if (current_native_x != previous_native_x ||
                current_native_y != previous_native_y)
                return false;
        }
    }
    return retrospective_compatible(current, previous);
}

/* Exact unkeyed copies are safe even when repeated: every candidate produces
 * the same phase. This is the only retrospective path allowed for blending
 * primitives, so a moving semitransparent packet still fails closed. */
static int32_t retrospective_exact_match(const GpuSemanticFrame *previous,
                                         const GpuRenderSemantic *semantic,
                                         bool *out_conflict) {
    int32_t entry;
    size_t candidate_count = 0u;
    bool used_exact = false;

    *out_conflict = false;
    if (previous == NULL || previous->overflowed) return -1;
    entry = workload.previous_retrospective_head[
        retrospective_class_hash(semantic)];
    while (entry != 0) {
        const size_t index = (size_t)(entry - 1);

        entry = workload.previous_retrospective_next[index];
        ++candidate_count;
        ++workload.diagnostics.retrospective_candidates;
        if (candidate_count > GPU_SEMANTIC_RETROSPECTIVE_CANDIDATE_LIMIT ||
            workload.diagnostics.retrospective_candidates >
                GPU_SEMANTIC_RETROSPECTIVE_FRAME_CANDIDATE_LIMIT) {
            ++workload.diagnostics.retrospective_budget_exhausted;
            *out_conflict = true;
            return -1;
        }
        if (!retrospective_exact_equal(semantic, &previous->items[index]))
            continue;
        if (workload.previous_used[index]) {
            used_exact = true;
            continue;
        }
        return (int32_t)index;
    }
    *out_conflict = used_exact;
    return -1;
}

/* Address-free matching for packet fallback and producers without authored
 * identities. Packet arenas and OT traversal order are intentionally absent.
 * Resource/material state narrows the candidates, then geometry relative to
 * the first vertex distinguishes primitives while allowing camera motion. */
static bool retrospective_score(const GpuRenderSemantic *current,
                                 const GpuRenderSemantic *previous,
                                 uint64_t *out_score) {
    const int64_t unit = INT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS;
    const size_t count = semantic_vertex_count(current);
    const GpuRenderSemanticVertex *current_anchor;
    const GpuRenderSemanticVertex *previous_anchor;
    int64_t current_sum_x = 0;
    int64_t current_sum_y = 0;
    int64_t previous_sum_x = 0;
    int64_t previous_sum_y = 0;
    uint64_t shape_delta = 0;
    uint64_t native_shape_delta = 0;
    uint64_t motion_delta_fixed = 0;
    uint64_t native_motion_delta_fixed = 0;
    uint64_t uv_delta = 0;
    uint64_t color_delta = 0;
    uint64_t score;
    const int64_t target_delta_x = target_relocation(
        current->material.draw_area_left, current->material.draw_area_right,
        current->material.draw_offset_x, previous->material.draw_area_left,
        previous->material.draw_area_right,
        previous->material.draw_offset_x);
    const int64_t target_delta_y = target_relocation(
        current->material.draw_area_top, current->material.draw_area_bottom,
        current->material.draw_offset_y, previous->material.draw_area_top,
        previous->material.draw_area_bottom,
        previous->material.draw_offset_y);

    if (out_score == NULL || count == 0u ||
        !retrospective_compatible(current, previous))
        return false;
    current_anchor = semantic_vertex_at(current, 0u);
    previous_anchor = semantic_vertex_at(previous, 0u);
    for (size_t index = 0u; index < count; ++index) {
        const GpuRenderSemanticVertex *a = semantic_vertex_at(current, index);
        const GpuRenderSemanticVertex *b = semantic_vertex_at(previous, index);
        const int64_t ax = (int64_t)a->x +
            (int64_t)current->material.draw_offset_x * unit;
        const int64_t ay = (int64_t)a->y +
            (int64_t)current->material.draw_offset_y * unit;
        const int64_t bx = (int64_t)b->x +
            (int64_t)previous->material.draw_offset_x * unit +
            target_delta_x;
        const int64_t by = (int64_t)b->y +
            (int64_t)previous->material.draw_offset_y * unit +
            target_delta_y;

        current_sum_x += ax;
        current_sum_y += ay;
        previous_sum_x += bx;
        previous_sum_y += by;
        shape_delta += abs_difference_i64(
            (int64_t)a->x - current_anchor->x,
            (int64_t)b->x - previous_anchor->x) / (uint64_t)unit;
        shape_delta += abs_difference_i64(
            (int64_t)a->y - current_anchor->y,
            (int64_t)b->y - previous_anchor->y) / (uint64_t)unit;
        motion_delta_fixed += abs_difference_i64(ax, bx) +
            abs_difference_i64(ay, by);
        uv_delta += abs_difference_i64(a->u, b->u) / (uint64_t)unit;
        uv_delta += abs_difference_i64(a->v, b->v) / (uint64_t)unit;
        color_delta += abs_difference_i64(a->r, b->r);
        color_delta += abs_difference_i64(a->g, b->g);
        color_delta += abs_difference_i64(a->b, b->b);
        if (a->native_view_position) {
            const int64_t native_ax = (int64_t)a->native_view_x +
                (int64_t)current->material.draw_offset_x * unit;
            const int64_t native_ay = (int64_t)a->native_view_y +
                (int64_t)current->material.draw_offset_y * unit;
            const int64_t native_bx = (int64_t)b->native_view_x +
                (int64_t)previous->material.draw_offset_x * unit +
                target_delta_x;
            const int64_t native_by = (int64_t)b->native_view_y +
                (int64_t)previous->material.draw_offset_y * unit +
                target_delta_y;

            native_shape_delta += abs_difference_i64(
                (int64_t)a->native_view_x - current_anchor->native_view_x,
                (int64_t)b->native_view_x - previous_anchor->native_view_x) /
                (uint64_t)unit;
            native_shape_delta += abs_difference_i64(
                (int64_t)a->native_view_y - current_anchor->native_view_y,
                (int64_t)b->native_view_y - previous_anchor->native_view_y) /
                (uint64_t)unit;
            native_motion_delta_fixed +=
                abs_difference_i64(native_ax, native_bx) +
                abs_difference_i64(native_ay, native_by);
        }
    }
    {
        const uint64_t translation =
            (abs_difference_i64(current_sum_x, previous_sum_x) +
             abs_difference_i64(current_sum_y, previous_sum_y)) /
            ((uint64_t)count * (uint64_t)unit);

        /* Without an authored identity, a primitive crossing more than one
         * fifth of the 320px viewport in one source frame is not a safe
         * temporal match. Snap it instead of making unrelated geometry fly. */
        if (translation > GPU_SEMANTIC_RETROSPECTIVE_TRANSLATION_LIMIT ||
            motion_delta_fixed > count *
                GPU_SEMANTIC_RETROSPECTIVE_TRANSLATION_LIMIT *
                (uint64_t)unit ||
            native_motion_delta_fixed > count *
                GPU_SEMANTIC_RETROSPECTIVE_TRANSLATION_LIMIT *
                (uint64_t)unit ||
            shape_delta > count * 32u ||
            native_shape_delta > count * 32u || uv_delta > count * 32u)
            return false;
        score = translation * 8u + shape_delta * 16u +
            native_shape_delta * 16u + uv_delta * 4u + color_delta;
    }
    *out_score = score;
    return true;
}

static int32_t retrospective_match(const GpuSemanticFrame *previous,
                                   const GpuRenderSemantic *semantic,
                                   bool *out_ambiguous) {
    uint64_t best_score = UINT64_MAX;
    uint64_t runner_up_score = UINT64_MAX;
    uint64_t used_score = UINT64_MAX;
    int32_t best_index = -1;
    int32_t entry;
    size_t candidate_count = 0u;

    *out_ambiguous = false;
    if (previous == NULL || previous->overflowed) return -1;
    entry = workload.previous_retrospective_head[
        retrospective_class_hash(semantic)];
    while (entry != 0) {
        const size_t index = (size_t)(entry - 1);
        uint64_t score;

        entry = workload.previous_retrospective_next[index];
        ++candidate_count;
        ++workload.diagnostics.retrospective_candidates;
        if (candidate_count > GPU_SEMANTIC_RETROSPECTIVE_CANDIDATE_LIMIT ||
            workload.diagnostics.retrospective_candidates >
                GPU_SEMANTIC_RETROSPECTIVE_FRAME_CANDIDATE_LIMIT) {
            ++workload.diagnostics.retrospective_budget_exhausted;
            *out_ambiguous = true;
            return -1;
        }
        if (!retrospective_score(
                semantic, &previous->items[index], &score))
            continue;
        if (workload.previous_used[index]) {
            if (score < used_score) used_score = score;
            continue;
        }
        if (score < best_score) {
            runner_up_score = best_score;
            best_score = score;
            best_index = (int32_t)index;
        } else if (score < runner_up_score) {
            runner_up_score = score;
        }
    }
    if (best_index < 0) {
        *out_ambiguous = used_score != UINT64_MAX;
        return -1;
    }
    if ((runner_up_score != UINT64_MAX &&
         runner_up_score - best_score <=
             GPU_SEMANTIC_RETROSPECTIVE_AMBIGUITY_MARGIN) ||
        (used_score != UINT64_MAX &&
         (used_score < best_score ||
          used_score - best_score <=
              GPU_SEMANTIC_RETROSPECTIVE_AMBIGUITY_MARGIN))) {
        *out_ambiguous = true;
        return -1;
    }
    return best_index;
}

static bool projective_payload_valid(const GpuRenderSemanticVertex *vertex) {
    const int64_t unit = INT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS;
    int64_t projected_x;
    int64_t projected_y;
    int64_t endpoint_x;
    int64_t endpoint_y;

    if (!vertex->projective_position ||
        vertex->projective_view_x < -0x8000 ||
        vertex->projective_view_x > 0x7fff ||
        vertex->projective_view_y < -0x8000 ||
        vertex->projective_view_y > 0x7fff ||
        vertex->projective_view_z <= 0 ||
        vertex->projective_view_z > 0xffff ||
        vertex->projective_distance == 0u ||
        (uint32_t)vertex->projective_view_z * 2u <=
            vertex->projective_distance) {
        return false;
    }
    projected_x = (int64_t)vertex->projective_offset_x +
        (int64_t)vertex->projective_view_x *
            vertex->projective_distance * unit /
            vertex->projective_view_z;
    projected_y = (int64_t)vertex->projective_offset_y +
        (int64_t)vertex->projective_view_y *
            vertex->projective_distance * unit /
            vertex->projective_view_z;
    endpoint_x = vertex->native_view_position
        ? (int64_t)vertex->native_view_x -
              vertex->projective_native_offset_x
        : vertex->x;
    endpoint_y = vertex->native_view_position
        ? (int64_t)vertex->native_view_y -
              vertex->projective_native_offset_y
        : vertex->y;
    return abs_difference_i64(projected_x, endpoint_x) <= 2u * (uint64_t)unit &&
        abs_difference_i64(projected_y, endpoint_y) <= 2u * (uint64_t)unit;
}

static bool interpolate_projective_position(
        GpuRenderSemanticVertex *out,
        const GpuRenderSemanticVertex *previous,
        const GpuRenderMaterial *current_material,
        const GpuRenderMaterial *previous_material,
        int64_t target_delta_x, int64_t target_delta_y,
        unsigned int numerator, unsigned int denominator) {
    const int64_t unit = INT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS;
    int64_t view_x;
    int64_t view_y;
    int64_t view_z;
    int64_t distance;
    int64_t offset_x;
    int64_t offset_y;
    int64_t projected_x;
    int64_t projected_y;
    int64_t native_x;
    int64_t native_y;

    if (!projective_payload_valid(previous) ||
        !projective_payload_valid(out))
        return false;
    view_x = interpolate_i64(previous->projective_view_x,
                             out->projective_view_x,
                             numerator, denominator);
    view_y = interpolate_i64(previous->projective_view_y,
                             out->projective_view_y,
                             numerator, denominator);
    view_z = interpolate_i64(previous->projective_view_z,
                             out->projective_view_z,
                             numerator, denominator);
    distance = interpolate_i64(previous->projective_distance,
                               out->projective_distance,
                               numerator, denominator);
    offset_x = interpolate_i64(
        (int64_t)previous->projective_offset_x +
            (int64_t)previous_material->draw_offset_x * unit +
            target_delta_x,
        (int64_t)out->projective_offset_x +
            (int64_t)current_material->draw_offset_x * unit,
        numerator, denominator);
    offset_y = interpolate_i64(
        (int64_t)previous->projective_offset_y +
            (int64_t)previous_material->draw_offset_y * unit +
            target_delta_y,
        (int64_t)out->projective_offset_y +
            (int64_t)current_material->draw_offset_y * unit,
        numerator, denominator);
    if (view_z <= 0 || distance <= 0 || view_z * 2 <= distance)
        return false;
    projected_x = offset_x + view_x * distance * unit / view_z -
        (int64_t)current_material->draw_offset_x * unit;
    projected_y = offset_y + view_y * distance * unit / view_z -
        (int64_t)current_material->draw_offset_y * unit;
    native_x = projected_x + interpolate_i64(
        previous->projective_native_offset_x,
        out->projective_native_offset_x, numerator, denominator);
    native_y = projected_y + interpolate_i64(
        previous->projective_native_offset_y,
        out->projective_native_offset_y, numerator, denominator);
    if (projected_x < INT32_MIN || projected_x > INT32_MAX ||
        projected_y < INT32_MIN || projected_y > INT32_MAX ||
        (out->native_view_position &&
         (native_x < INT32_MIN || native_x > INT32_MAX ||
          native_y < INT32_MIN || native_y > INT32_MAX)))
        return false;
    out->x = (GpuRenderFixed16_16)projected_x;
    out->y = (GpuRenderFixed16_16)projected_y;
    out->projective_view_x = (int32_t)view_x;
    out->projective_view_y = (int32_t)view_y;
    out->projective_view_z = (int32_t)view_z;
    out->projective_distance = (uint16_t)distance;
    if (out->native_view_position) {
        out->native_view_x = (GpuRenderFixed16_16)native_x;
        out->native_view_y = (GpuRenderFixed16_16)native_y;
    }
    ++workload.diagnostics.total_projective_phase_vertices;
    return true;
}

static bool interpolate_vertex(GpuRenderSemanticVertex *out,
                               const GpuRenderSemanticVertex *previous,
                               const GpuRenderSemanticVertex *position_previous,
                               const GpuRenderMaterial *current_material,
                               const GpuRenderMaterial *position_previous_material,
                               unsigned int numerator,
                               unsigned int denominator,
                               bool interpolate_color,
                               size_t *out_position_changed_vertices,
                               uint64_t *out_position_delta_fixed,
                               size_t *out_midpoint_distinct_vertices,
                               size_t *out_midpoint_collapsed_vertices,
                               size_t *out_midpoint_formula_failures) {
    const int64_t unit = INT64_C(1) << GPU_RENDER_FIXED_FRACTION_BITS;
    const GpuRenderFixed16_16 current_x = out->x;
    const GpuRenderFixed16_16 current_y = out->y;
    const GpuRenderFixed16_16 current_native_x = out->native_view_x;
    const GpuRenderFixed16_16 current_native_y = out->native_view_y;
    const uint8_t current_r = out->r;
    const uint8_t current_g = out->g;
    const uint8_t current_b = out->b;
    const int64_t target_delta_x = target_relocation(
        current_material->draw_area_left, current_material->draw_area_right,
        current_material->draw_offset_x,
        position_previous_material->draw_area_left,
        position_previous_material->draw_area_right,
        position_previous_material->draw_offset_x);
    const int64_t target_delta_y = target_relocation(
        current_material->draw_area_top, current_material->draw_area_bottom,
        current_material->draw_offset_y,
        position_previous_material->draw_area_top,
        position_previous_material->draw_area_bottom,
        position_previous_material->draw_offset_y);
    const int64_t previous_x = (int64_t)position_previous->x +
        (int64_t)position_previous_material->draw_offset_x * unit +
        target_delta_x;
    const int64_t previous_y = (int64_t)position_previous->y +
        (int64_t)position_previous_material->draw_offset_y * unit +
        target_delta_y;
    const int64_t current_effective_x = (int64_t)current_x +
        (int64_t)current_material->draw_offset_x * unit;
    const int64_t current_effective_y = (int64_t)current_y +
        (int64_t)current_material->draw_offset_y * unit;
    const int64_t previous_native_x =
        (int64_t)position_previous->native_view_x +
        (int64_t)position_previous_material->draw_offset_x * unit +
        target_delta_x;
    const int64_t previous_native_y =
        (int64_t)position_previous->native_view_y +
        (int64_t)position_previous_material->draw_offset_y * unit +
        target_delta_y;
    const int64_t current_effective_native_x = (int64_t)current_native_x +
        (int64_t)current_material->draw_offset_x * unit;
    const int64_t current_effective_native_y = (int64_t)current_native_y +
        (int64_t)current_material->draw_offset_y * unit;
    const int64_t previous_present_x = out->native_view_position
        ? previous_native_x : previous_x;
    const int64_t previous_present_y = out->native_view_position
        ? previous_native_y : previous_y;
    const int64_t current_present_x = out->native_view_position
        ? current_effective_native_x : current_effective_x;
    const int64_t current_present_y = out->native_view_position
        ? current_effective_native_y : current_effective_y;
    const uint64_t position_delta =
        abs_difference_i64(previous_present_x, current_present_x) +
        abs_difference_i64(previous_present_y, current_present_y);
    int64_t midpoint_present_x;
    int64_t midpoint_present_y;
    int64_t midpoint;
    bool projective_interpolation;

    if (position_delta != 0u) ++*out_position_changed_vertices;
    *out_position_delta_fixed += position_delta;

    projective_interpolation = interpolate_projective_position(
        out, position_previous, current_material,
        position_previous_material, target_delta_x, target_delta_y,
        numerator, denominator);
    if (!projective_interpolation) {
        midpoint = interpolate_i64(previous_x, current_effective_x,
                                   numerator, denominator) -
            (int64_t)current_material->draw_offset_x * unit;
        out->x = midpoint >= INT32_MIN && midpoint <= INT32_MAX
            ? (GpuRenderFixed16_16)midpoint : current_x;
        midpoint = interpolate_i64(previous_y, current_effective_y,
                                   numerator, denominator) -
            (int64_t)current_material->draw_offset_y * unit;
        out->y = midpoint >= INT32_MIN && midpoint <= INT32_MAX
            ? (GpuRenderFixed16_16)midpoint : current_y;
        if (out->native_view_position) {
            midpoint = interpolate_i64(previous_native_x,
                                       current_effective_native_x,
                                       numerator, denominator) -
                (int64_t)current_material->draw_offset_x * unit;
            out->native_view_x = midpoint >= INT32_MIN && midpoint <= INT32_MAX
                ? (GpuRenderFixed16_16)midpoint : current_native_x;
            midpoint = interpolate_i64(previous_native_y,
                                       current_effective_native_y,
                                       numerator, denominator) -
                (int64_t)current_material->draw_offset_y * unit;
            out->native_view_y = midpoint >= INT32_MIN && midpoint <= INT32_MAX
                ? (GpuRenderFixed16_16)midpoint : current_native_y;
        }
    }
    if (position_previous->temporal_depth_valid && out->temporal_depth_valid) {
        midpoint = interpolate_i64(
            position_previous->temporal_depth, out->temporal_depth,
            numerator, denominator);
        if (midpoint >= INT32_MIN && midpoint <= INT32_MAX)
            out->temporal_depth = (int32_t)midpoint;
    }
    midpoint_present_x = (int64_t)(out->native_view_position
        ? out->native_view_x : out->x) +
        (int64_t)current_material->draw_offset_x * unit;
    midpoint_present_y = (int64_t)(out->native_view_position
        ? out->native_view_y : out->y) +
        (int64_t)current_material->draw_offset_y * unit;
    if (position_delta != 0u) {
        if (midpoint_present_x != previous_present_x ||
            midpoint_present_y != previous_present_y) {
            if (midpoint_present_x != current_present_x ||
                midpoint_present_y != current_present_y)
                ++*out_midpoint_distinct_vertices;
            else
                ++*out_midpoint_collapsed_vertices;
        } else {
            ++*out_midpoint_collapsed_vertices;
        }
        if (!projective_interpolation &&
            (midpoint_present_x != interpolate_i64(
                 previous_present_x, current_present_x,
                 numerator, denominator) ||
             midpoint_present_y != interpolate_i64(
                 previous_present_y, current_present_y,
                 numerator, denominator)))
            ++*out_midpoint_formula_failures;
    }
    /* UV is discrete on the PS1 rasterizer. Keep the current sample footprint;
     * averaging odd texel deltas would create invalid half-texel semantics. */
    if (interpolate_color) {
        out->r = (uint8_t)((previous->r * (denominator - numerator) +
                            current_r * numerator) / denominator);
        out->g = (uint8_t)((previous->g * (denominator - numerator) +
                            current_g * numerator) / denominator);
        out->b = (uint8_t)((previous->b * (denominator - numerator) +
                            current_b * numerator) / denominator);
    }
    return previous_x != current_effective_x ||
           previous_y != current_effective_y ||
           (interpolate_color &&
            (previous->r != current_r || previous->g != current_g ||
             previous->b != current_b)) ||
           (out->native_view_position &&
            (previous_native_x != current_effective_native_x ||
             previous_native_y != current_effective_native_y));
}

static bool interpolate_semantic(GpuRenderSemantic *out,
                                  const GpuRenderSemantic *previous,
                                  unsigned int numerator,
                                  unsigned int denominator,
                                  size_t *out_position_changed_vertices,
                                  uint64_t *out_position_delta_fixed,
                                   size_t *out_midpoint_distinct_vertices,
                                   size_t *out_midpoint_collapsed_vertices,
                                   size_t *out_midpoint_formula_failures) {
    size_t primitive;
    size_t vertex;
    bool moved = false;

    if (out->topology == GPU_RENDER_SEMANTIC_TRIANGLES) {
        for (primitive = 0; primitive < out->triangle_count; ++primitive)
            for (vertex = 0; vertex < 3u; ++vertex) {
                const GpuRenderSemanticVertex *position_previous =
                    &previous->triangles[primitive].vertices[vertex];
                const GpuRenderMaterial *position_previous_material =
                    &previous->material;
                const GpuRenderSemanticVertex *current_vertex =
                    &out->triangles[primitive].vertices[vertex];

                /* A matched primitive is already a coherent temporal source.
                 * An ambiguous mesh lookup must not snap only one vertex. */
                (void)previous_vertex_lookup(
                    out, current_vertex, &position_previous,
                    &position_previous_material);
                moved |= interpolate_vertex(
                    &out->triangles[primitive].vertices[vertex],
                    &previous->triangles[primitive].vertices[vertex],
                    position_previous, &out->material,
                    position_previous_material, numerator, denominator, true,
                    out_position_changed_vertices,
                    out_position_delta_fixed,
                    out_midpoint_distinct_vertices,
                    out_midpoint_collapsed_vertices,
                    out_midpoint_formula_failures);
            }
    } else {
        for (primitive = 0; primitive < out->line_count; ++primitive)
            for (vertex = 0; vertex < 2u; ++vertex) {
                const GpuRenderSemanticVertex *position_previous =
                    &previous->lines[primitive].vertices[vertex];
                const GpuRenderMaterial *position_previous_material =
                    &previous->material;
                const GpuRenderSemanticVertex *current_vertex =
                    &out->lines[primitive].vertices[vertex];

                (void)previous_vertex_lookup(
                    out, current_vertex, &position_previous,
                    &position_previous_material);
                moved |= interpolate_vertex(
                    &out->lines[primitive].vertices[vertex],
                    &previous->lines[primitive].vertices[vertex],
                    position_previous, &out->material,
                    position_previous_material, numerator, denominator, true,
                    out_position_changed_vertices,
                    out_position_delta_fixed,
                    out_midpoint_distinct_vertices,
                    out_midpoint_collapsed_vertices,
                    out_midpoint_formula_failures);
            }
    }
    return moved;
}

static bool interpolate_source_geometry(
        GpuRenderSemantic *out, unsigned int numerator,
        unsigned int denominator, bool *out_moved,
        size_t *out_position_changed_vertices,
        uint64_t *out_position_delta_fixed,
        size_t *out_midpoint_distinct_vertices,
        size_t *out_midpoint_collapsed_vertices,
        size_t *out_midpoint_formula_failures) {
    const size_t count = semantic_vertex_count(out);

    if (out_moved == NULL || count == 0u) return false;
    for (size_t index = 0u; index < count; ++index) {
        const GpuRenderSemanticVertex *previous_vertex;
        const GpuRenderMaterial *previous_material;

        if (!previous_vertex_lookup(
                out, semantic_vertex_at(out, index), &previous_vertex,
                &previous_material) ||
            previous_vertex->native_view_position !=
                semantic_vertex_at(out, index)->native_view_position)
            return false;
    }
    *out_moved = false;
    for (size_t index = 0u; index < count; ++index) {
        GpuRenderSemanticVertex *vertex =
            semantic_vertex_at_mutable(out, index);
        const GpuRenderSemanticVertex *previous_vertex;
        const GpuRenderMaterial *previous_material;

        if (!previous_vertex_lookup(
                out, vertex, &previous_vertex, &previous_material))
            return false;
        *out_moved |= interpolate_vertex(
            vertex, vertex, previous_vertex, &out->material,
            previous_material, numerator, denominator, false,
            out_position_changed_vertices, out_position_delta_fixed,
            out_midpoint_distinct_vertices, out_midpoint_collapsed_vertices,
            out_midpoint_formula_failures);
    }
    return true;
}

void gpu_semantic_workload_reset(void) {
    clear_workload_hashes();
    if (workload_epoch != UINT64_MAX) ++workload_epoch;
    for (size_t index = 0u; index < 2u; ++index) {
        workload.frames[index].count = 0u;
        workload.frames[index].participating_count = 0u;
        workload.frames[index].authoritative_count = 0u;
        workload.frames[index].unkeyed_count = 0u;
        workload.frames[index].overflowed = false;
        workload.frames[index].conflicted = false;
        workload.anchor_frames[index].count = 0u;
        workload.anchor_frames[index].overflowed = false;
    }
    workload.sealed_index = 0u;
    workload.building_index = 0u;
    workload.has_sealed = false;
    workload.building = false;
    workload.diagnostics = (GpuSemanticWorkloadDiagnostics){
        .epoch = workload_epoch,
    };
    workload.last_motion = (GpuSemanticWorkloadMotionDiagnostics){ 0 };
}

GpuSemanticWorkloadStatus gpu_semantic_workload_begin(void) {
    GpuSemanticFrame *building;

    if (workload.building) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    }
    workload.building_index = workload.has_sealed
                                  ? workload.sealed_index ^ 1u
                                  : 0u;
    building = &workload.frames[workload.building_index];
    GpuSemanticAnchorFrame *building_anchors =
        &workload.anchor_frames[workload.building_index];
    building->count = 0u;
    building->participating_count = 0u;
    building->authoritative_count = 0u;
    building->unkeyed_count = 0u;
    building->overflowed = false;
    building->conflicted = false;
    building_anchors->count = 0u;
    building_anchors->overflowed = false;
    clear_workload_hashes();
    if (workload.has_sealed) {
        const GpuSemanticFrame *previous =
            &workload.frames[workload.sealed_index];
        const GpuSemanticAnchorFrame *previous_anchors =
            &workload.anchor_frames[workload.sealed_index];

        memset(workload.previous_used, 0,
               previous->count * sizeof(workload.previous_used[0]));
        memset(workload.previous_corresponded, 0,
               previous->count * sizeof(workload.previous_corresponded[0]));
        for (size_t index = 0u; index < previous_anchors->count; ++index)
            anchor_hash_insert(
                workload.previous_anchor_hash,
                workload.previous_anchor_hash_touched,
                &workload.previous_anchor_hash_touched_count,
                previous_anchors, index);
        for (size_t index = 0u; index < previous->count; ++index) {
            (void)hash_insert(
                workload.previous_hash, workload.previous_hash_touched,
                &workload.previous_hash_touched_count, previous, index);
            retrospective_hash_insert(previous, index);
            for (size_t vertex = 0u;
                 vertex < semantic_vertex_count(&previous->items[index]);
                 ++vertex)
                previous_vertex_hash_insert(previous, index, vertex);
        }
    }
    workload.building = true;
    workload.diagnostics.current_count = 0u;
    workload.diagnostics.current_participating_count = 0u;
    workload.diagnostics.current_overflowed = false;
    workload.diagnostics.building = true;
    workload.diagnostics.previous_count = workload.has_sealed
        ? workload.frames[workload.sealed_index].count : 0u;
    workload.diagnostics.previous_participating_count = workload.has_sealed
        ? workload.frames[workload.sealed_index].participating_count : 0u;
    workload.diagnostics.previous_usable = workload.has_sealed &&
        !workload.frames[workload.sealed_index].overflowed;
    workload.diagnostics.matched_count = 0u;
    workload.diagnostics.snapped_count = 0u;
    workload.diagnostics.ambiguous_count = 0u;
    workload.diagnostics.moved_count = 0u;
    workload.diagnostics.unkeyed_count = 0u;
    workload.diagnostics.exact_match_count = 0u;
    workload.diagnostics.exact_semitransparent_match_count = 0u;
    workload.diagnostics.source_geometry_match_count = 0u;
    workload.diagnostics.matched_vertex_count = 0u;
    workload.diagnostics.position_changed_vertex_count = 0u;
    workload.diagnostics.position_delta_fixed = 0u;
    workload.diagnostics.midpoint_distinct_vertex_count = 0u;
    workload.diagnostics.midpoint_collapsed_vertex_count = 0u;
    workload.diagnostics.midpoint_formula_failure_count = 0u;
    workload.diagnostics.retrospective_candidates = 0u;
    workload.diagnostics.retrospective_budget_exhausted = 0u;
    workload.diagnostics.retrospective_semitransparent_rejected = 0u;
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_record_anchors(
        const GpuRenderInterpolationVertexAnchor *anchors, size_t count) {
    GpuSemanticAnchorFrame *building;

    if (anchors == NULL && count != 0u)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    if (!workload.building)
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    building = &workload.anchor_frames[workload.building_index];
    if (count > GPU_SEMANTIC_ANCHOR_CAPACITY - building->count) {
        building->overflowed = true;
        return GPU_SEMANTIC_WORKLOAD_CAPACITY_EXCEEDED;
    }
    for (size_t index = 0u; index < count; ++index) {
        if (anchors[index].producer_id == 0u ||
            !anchors[index].vertex.interpolation_vertex_identity_valid)
            return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    }
    const size_t first = building->count;
    memcpy(&building->items[first], anchors,
           count * sizeof(*anchors));
    building->count += count;
    for (size_t index = first; index < building->count; ++index)
        anchor_hash_insert(
            workload.current_anchor_hash,
            workload.current_anchor_hash_touched,
            &workload.current_anchor_hash_touched_count, building, index);
    return GPU_SEMANTIC_WORKLOAD_OK;
}

static GpuSemanticWorkloadStatus gpu_semantic_workload_record_internal(
    const GpuRenderSemantic *semantic, GpuRenderSemantic *out_midpoint,
    bool generate_midpoint, GpuSemanticWorkloadParticipation participation) {
    GpuSemanticFrame *building;
    const GpuSemanticFrame *previous;
    size_t index;
    int32_t previous_index = -1;
    bool ambiguous = false;
    bool current_identity_unique;
    bool matched_exact = false;
    bool previous_compatible = false;
    GpuSemanticWorkloadMatchKind snap_kind;

    if (semantic == NULL || (generate_midpoint && out_midpoint == NULL)) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    }
    if (!workload.building) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    }
    building = &workload.frames[workload.building_index];
    if (generate_midpoint) *out_midpoint = *semantic;
    if (building->conflicted) return GPU_SEMANTIC_WORKLOAD_CONFLICT;
    if (building->count == GPU_SEMANTIC_WORKLOAD_CAPACITY) {
        building->overflowed = true;
        workload.diagnostics.current_overflowed = true;
        ++workload.diagnostics.total_dropped;
        return GPU_SEMANTIC_WORKLOAD_CAPACITY_EXCEEDED;
    }
    for (size_t vertex_index = 0u;
         vertex_index < semantic_vertex_count(semantic); ++vertex_index) {
        const GpuRenderSemanticVertex *vertex =
            semantic_vertex_at(semantic, vertex_index);

        if (!vertex->projective_position) continue;
        ++workload.diagnostics.total_projective_input_vertices;
        if (projective_payload_valid(vertex))
            ++workload.diagnostics.total_projective_valid_input_vertices;
    }
    index = building->count++;
    building->items[index] = *semantic;
    building->participation[index] = (uint8_t)participation;
    if (participation != GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY)
        ++building->participating_count;
    if (participation ==
        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT)
        ++building->authoritative_count;
    workload.previous_match[index] = -1;
    workload.current_moved[index] = false;
    workload.source_geometry_match[index] = false;
    workload.match_kind[index] = GPU_SEMANTIC_WORKLOAD_MATCH_UNKNOWN;
    workload.fallback_kind[index] = GPU_SEMANTIC_WORKLOAD_MATCH_UNKNOWN;
    current_identity_unique = hash_insert(
        workload.current_hash, workload.current_hash_touched,
        &workload.current_hash_touched_count, building, index);
    previous = workload.has_sealed ? &workload.frames[workload.sealed_index] : NULL;
    snap_kind = previous == NULL || previous->overflowed
        ? GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_NO_PREVIOUS
        : semantic->interpolation_identity.valid
            ? GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_NOT_FOUND
            : GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_UNKEYED;
    if (!semantic->interpolation_identity.valid) {
        bool exact_conflict = false;

        ++building->unkeyed_count;
        ++workload.diagnostics.unkeyed_count;
        ++workload.diagnostics.total_unkeyed;
        previous_index = retrospective_exact_match(
            previous, semantic, &exact_conflict);
        matched_exact = previous_index >= 0;
        ambiguous = exact_conflict;
        if (previous_index < 0 && !ambiguous &&
            semantic->material.semi_transparent) {
            if (previous != NULL && !previous->overflowed) {
                ++workload.diagnostics
                      .retrospective_semitransparent_rejected;
                ++workload.diagnostics
                      .total_retrospective_semitransparent_rejected;
            }
        } else if (previous_index < 0 && !ambiguous &&
                   semantic->material.textured &&
                   semantic->topology != GPU_RENDER_SEMANTIC_LINES) {
            previous_index = retrospective_match(previous, semantic, &ambiguous);
        }
        if (ambiguous)
            snap_kind = GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_AMBIGUOUS;
    } else if (!current_identity_unique) {
        building->conflicted = true;
        for (size_t match_index = 0u; match_index < building->count;
             ++match_index) {
            workload.previous_match[match_index] = -1;
            workload.match_kind[match_index] =
                GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_AMBIGUOUS;
            workload.fallback_kind[match_index] =
                GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_AMBIGUOUS;
        }
        ++workload.diagnostics.ambiguous_count;
        ++workload.diagnostics.total_ambiguous;
        workload.diagnostics.matched_count = 0u;
        workload.diagnostics.moved_count = 0u;
        workload.diagnostics.matched_vertex_count = 0u;
        workload.diagnostics.source_geometry_match_count = 0u;
        workload.diagnostics.position_changed_vertex_count = 0u;
        workload.diagnostics.position_delta_fixed = 0u;
        workload.diagnostics.midpoint_distinct_vertex_count = 0u;
        workload.diagnostics.midpoint_collapsed_vertex_count = 0u;
        workload.diagnostics.midpoint_formula_failure_count = 0u;
        workload.diagnostics.snapped_count = building->count;
        workload.diagnostics.total_snapped += building->count;
        workload.diagnostics.previous_usable = false;
        ++workload.diagnostics.total_recorded;
        workload.diagnostics.current_count = building->count;
        return GPU_SEMANTIC_WORKLOAD_CONFLICT;
    } else if (previous != NULL && !previous->overflowed) {
        previous_index = hash_lookup(
            workload.previous_hash, previous,
            &semantic->interpolation_identity, &ambiguous);
        if (ambiguous)
            snap_kind = GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_AMBIGUOUS;
    }
    if (previous_index >= 0 &&
        workload.previous_used[(size_t)previous_index])
        snap_kind = GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_ALREADY_USED;
    else if (previous_index >= 0) {
        previous_compatible = !semantic->interpolation_identity.valid ||
            semantic_compatible(semantic, &previous->items[previous_index]);
        if (!previous_compatible)
            snap_kind = GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_INCOMPATIBLE;
    }
    if (previous_index >= 0 &&
        !workload.previous_used[(size_t)previous_index] &&
        previous_compatible) {
        const size_t matched_vertices = semantic_vertex_count(semantic);
        size_t position_changed_vertices = 0u;
        uint64_t position_delta_fixed = 0u;
        size_t midpoint_distinct_vertices = 0u;
        size_t midpoint_collapsed_vertices = 0u;
        size_t midpoint_formula_failures = 0u;
        bool moved;

        workload.previous_used[(size_t)previous_index] = true;
        workload.previous_corresponded[(size_t)previous_index] = true;
        workload.previous_match[index] = previous_index;
        workload.match_kind[index] = semantic->interpolation_identity.valid
            ? GPU_SEMANTIC_WORKLOAD_MATCH_IDENTITY
            : GPU_SEMANTIC_WORKLOAD_MATCH_RETROSPECTIVE;
        if (generate_midpoint) {
            moved = interpolate_semantic(
                out_midpoint, &previous->items[previous_index], 1u, 2u,
                &position_changed_vertices, &position_delta_fixed,
                &midpoint_distinct_vertices, &midpoint_collapsed_vertices,
                &midpoint_formula_failures);
            if (moved) {
                workload.current_moved[index] = true;
                record_motion_diagnostics(
                    index, (size_t)previous_index,
                    &previous->items[previous_index], semantic, out_midpoint,
                    position_changed_vertices, position_delta_fixed);
                if (participation !=
                        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY)
                    ++workload.diagnostics.moved_count;
                ++workload.diagnostics.total_moved;
            }
            workload.diagnostics.matched_vertex_count += matched_vertices;
            workload.diagnostics.position_changed_vertex_count +=
                position_changed_vertices;
            workload.diagnostics.position_delta_fixed += position_delta_fixed;
            workload.diagnostics.midpoint_distinct_vertex_count +=
                midpoint_distinct_vertices;
            workload.diagnostics.midpoint_collapsed_vertex_count +=
                midpoint_collapsed_vertices;
            workload.diagnostics.midpoint_formula_failure_count +=
                midpoint_formula_failures;
            workload.diagnostics.total_matched_vertices += matched_vertices;
            workload.diagnostics.total_position_changed_vertices +=
                position_changed_vertices;
            workload.diagnostics.total_position_delta_fixed +=
                position_delta_fixed;
            record_semantic_position_delta(semantic, position_delta_fixed);
            workload.diagnostics.total_midpoint_distinct_vertices +=
                midpoint_distinct_vertices;
            workload.diagnostics.total_midpoint_collapsed_vertices +=
                midpoint_collapsed_vertices;
            workload.diagnostics.total_midpoint_formula_failures +=
                midpoint_formula_failures;
        }
        if (participation !=
                GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY)
            ++workload.diagnostics.matched_count;
        ++workload.diagnostics.total_matched;
        if (matched_exact) {
            ++workload.diagnostics.exact_match_count;
            ++workload.diagnostics.total_exact_matches;
            if (semantic->material.semi_transparent) {
                ++workload.diagnostics.exact_semitransparent_match_count;
                ++workload.diagnostics.total_exact_semitransparent_matches;
            }
        }
    } else {
        size_t position_changed_vertices = 0u;
        uint64_t position_delta_fixed = 0u;
        size_t midpoint_distinct_vertices = 0u;
        size_t midpoint_collapsed_vertices = 0u;
        size_t midpoint_formula_failures = 0u;
        bool source_geometry_moved = false;

        workload.fallback_kind[index] = snap_kind;

        if (generate_midpoint && interpolate_source_geometry(
                out_midpoint, 1u, 2u, &source_geometry_moved,
                &position_changed_vertices, &position_delta_fixed,
                &midpoint_distinct_vertices, &midpoint_collapsed_vertices,
                &midpoint_formula_failures)) {
            workload.source_geometry_match[index] = true;
            workload.match_kind[index] =
                GPU_SEMANTIC_WORKLOAD_MATCH_SOURCE_GEOMETRY;
            ++workload.diagnostics.source_geometry_match_count;
            ++workload.diagnostics.total_source_geometry_matches;
            if (source_geometry_moved) {
                workload.current_moved[index] = true;
                record_motion_diagnostics(
                    index, 0u, NULL, semantic, out_midpoint,
                    position_changed_vertices, position_delta_fixed);
                if (participation !=
                        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY)
                    ++workload.diagnostics.moved_count;
                ++workload.diagnostics.total_moved;
            }
            workload.diagnostics.position_changed_vertex_count +=
                position_changed_vertices;
            workload.diagnostics.position_delta_fixed += position_delta_fixed;
            workload.diagnostics.midpoint_distinct_vertex_count +=
                midpoint_distinct_vertices;
            workload.diagnostics.midpoint_collapsed_vertex_count +=
                midpoint_collapsed_vertices;
            workload.diagnostics.midpoint_formula_failure_count +=
                midpoint_formula_failures;
            workload.diagnostics.total_position_changed_vertices +=
                position_changed_vertices;
            workload.diagnostics.total_position_delta_fixed +=
                position_delta_fixed;
            record_semantic_position_delta(semantic, position_delta_fixed);
            workload.diagnostics.total_midpoint_distinct_vertices +=
                midpoint_distinct_vertices;
            workload.diagnostics.total_midpoint_collapsed_vertices +=
                midpoint_collapsed_vertices;
            workload.diagnostics.total_midpoint_formula_failures +=
                midpoint_formula_failures;
        }
        if (previous_index >= 0 &&
            !workload.previous_used[(size_t)previous_index])
            workload.previous_corresponded[(size_t)previous_index] = true;
        if (previous_index >= 0 &&
            workload.previous_used[(size_t)previous_index])
            ambiguous = true;
        if (ambiguous) {
            ++workload.diagnostics.ambiguous_count;
            ++workload.diagnostics.total_ambiguous;
        }
        if (!workload.source_geometry_match[index])
            workload.match_kind[index] = snap_kind;
        ++workload.diagnostics.snapped_count;
        ++workload.diagnostics.total_snapped;
    }
    ++workload.diagnostics.total_recorded;
    workload.diagnostics.current_count = building->count;
    workload.diagnostics.current_participating_count =
        building->participating_count;
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_record(
        const GpuRenderSemantic *semantic,
        GpuRenderSemantic *out_midpoint) {
    return gpu_semantic_workload_record_internal(
        semantic, out_midpoint, true,
        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT);
}

GpuSemanticWorkloadStatus gpu_semantic_workload_record_endpoint(
        const GpuRenderSemantic *semantic) {
    return gpu_semantic_workload_record_internal(
        semantic, NULL, false,
        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY);
}

GpuSemanticWorkloadStatus
gpu_semantic_workload_mark_last_temporal_history_only(void) {
    GpuSemanticFrame *building;
    size_t index;

    if (!workload.building)
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    building = &workload.frames[workload.building_index];
    if (building->count == 0u)
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    index = building->count - 1u;
    if (building->participation[index] !=
            GPU_SEMANTIC_WORKLOAD_PARTICIPATION_TEMPORAL_PHASE)
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    building->participation[index] =
        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY;
    --building->participating_count;
    if (workload.previous_match[index] >= 0 &&
        workload.diagnostics.matched_count != 0u)
        --workload.diagnostics.matched_count;
    if (workload.current_moved[index] &&
        workload.diagnostics.moved_count != 0u)
        --workload.diagnostics.moved_count;
    workload.diagnostics.current_participating_count =
        building->participating_count;
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_seal(void) {
    GpuSemanticFrame *current;
    const GpuSemanticFrame *previous;
    GpuSemanticWorkloadEligibility eligibility;

    if (!workload.building) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    }
    current = &workload.frames[workload.building_index];
    if (current->conflicted) return GPU_SEMANTIC_WORKLOAD_CONFLICT;
    previous = workload.has_sealed
        ? &workload.frames[workload.sealed_index] : NULL;
    workload.diagnostics.last_seal_previous_count =
        previous != NULL ? previous->count : 0u;
    workload.diagnostics.last_seal_current_count = current->count;
    workload.diagnostics.last_seal_previous_participating_count =
        previous != NULL ? previous->participating_count : 0u;
    workload.diagnostics.last_seal_current_participating_count =
        current->participating_count;
    workload.diagnostics.last_seal_previous_unkeyed_count =
        previous != NULL ? previous->unkeyed_count : 0u;
    workload.diagnostics.last_seal_current_unkeyed_count =
        current->unkeyed_count;
    workload.diagnostics.last_seal_matched_count =
        workload.diagnostics.matched_count;
    workload.diagnostics.last_seal_snapped_count =
        workload.diagnostics.snapped_count;
    workload.diagnostics.last_seal_ambiguous_count =
        workload.diagnostics.ambiguous_count;
    workload.diagnostics.last_seal_moved_count =
        workload.diagnostics.moved_count;
    workload.diagnostics.last_seal_exact_match_count =
        workload.diagnostics.exact_match_count;
    workload.diagnostics.last_seal_exact_semitransparent_match_count =
        workload.diagnostics.exact_semitransparent_match_count;
    workload.diagnostics.last_seal_previous_unmatched_count = 0u;
    workload.diagnostics.last_seal_previous_unmatched_keyed_count = 0u;
    workload.diagnostics.last_seal_previous_unmatched_projective_count = 0u;
    if (previous != NULL) {
        for (size_t index = 0u; index < previous->count; ++index) {
            const GpuRenderSemantic *semantic;

            if (workload.previous_corresponded[index] ||
                previous->participation[index] ==
                    GPU_SEMANTIC_WORKLOAD_PARTICIPATION_HISTORY_ONLY)
                continue;
            semantic = &previous->items[index];
            ++workload.diagnostics.last_seal_previous_unmatched_count;
            ++workload.diagnostics.total_previous_unmatched;
            if (semantic->interpolation_identity.valid) {
                ++workload.diagnostics
                      .last_seal_previous_unmatched_keyed_count;
                ++workload.diagnostics.total_previous_unmatched_keyed;
            }
            if (semantic_has_projective_position(semantic)) {
                ++workload.diagnostics
                      .last_seal_previous_unmatched_projective_count;
                ++workload.diagnostics.total_previous_unmatched_projective;
            }
        }
    }
    workload.diagnostics.last_seal_previous_overflowed =
        previous != NULL && previous->overflowed;
    workload.diagnostics.last_seal_current_overflowed = current->overflowed;
    if (!workload.has_sealed) {
        eligibility = GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_NO_PREVIOUS;
        ++workload.diagnostics.total_rejected_no_previous_frames;
    } else if (previous->overflowed || current->overflowed) {
        eligibility = GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_OVERFLOW;
        ++workload.diagnostics.total_rejected_overflow_frames;
    } else if (current->participating_count !=
                   previous->participating_count &&
               workload.diagnostics.moved_count != 0u) {
        eligibility =
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH;
        ++workload.diagnostics.total_partial_count_mismatch_frames;
    } else if (current->participating_count !=
               previous->participating_count) {
        eligibility = GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_COUNT_MISMATCH;
        ++workload.diagnostics.total_rejected_count_mismatch_frames;
    } else if (workload.diagnostics.matched_count !=
                   current->participating_count &&
               workload.diagnostics.moved_count != 0u) {
        eligibility =
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH;
        ++workload.diagnostics.total_partial_incomplete_match_frames;
    } else if (workload.diagnostics.matched_count !=
               current->participating_count) {
        eligibility = GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_INCOMPLETE_MATCH;
        ++workload.diagnostics.total_rejected_incomplete_match_frames;
    } else if (workload.diagnostics.moved_count == 0u) {
        eligibility = GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_STATIC;
        ++workload.diagnostics.total_rejected_static_frames;
    } else {
        eligibility = GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_ELIGIBLE;
        ++workload.diagnostics.total_eligible_frames;
    }
    workload.diagnostics.last_seal_eligibility = eligibility;
    workload.diagnostics.previous_usable =
        eligibility == GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_ELIGIBLE ||
        eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH ||
        eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH;
    workload.diagnostics.current_count = current->count;
    workload.diagnostics.current_participating_count =
        current->participating_count;
    workload.diagnostics.current_overflowed = current->overflowed;
    workload.diagnostics.building = false;
    ++workload.diagnostics.sealed_frames;
    workload.sealed_index = workload.building_index;
    workload.has_sealed = true;
    workload.building = false;
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_discard_current(void) {
    GpuSemanticFrame *current;

    if (!workload.building) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    }
    current = &workload.frames[workload.building_index];
    current->count = 0u;
    current->participating_count = 0u;
    current->authoritative_count = 0u;
    current->unkeyed_count = 0u;
    current->overflowed = false;
    current->conflicted = false;
    workload.anchor_frames[workload.building_index].count = 0u;
    workload.anchor_frames[workload.building_index].overflowed = false;
    workload.diagnostics.current_count = 0u;
    workload.diagnostics.current_participating_count = 0u;
    workload.diagnostics.current_overflowed = false;
    workload.diagnostics.building = false;
    workload.diagnostics.previous_count = workload.has_sealed
        ? workload.frames[workload.sealed_index].count : 0u;
    workload.diagnostics.previous_participating_count = workload.has_sealed
        ? workload.frames[workload.sealed_index].participating_count : 0u;
    workload.diagnostics.previous_usable = workload.has_sealed &&
        !workload.frames[workload.sealed_index].overflowed;
    workload.diagnostics.matched_count = 0u;
    workload.diagnostics.snapped_count = 0u;
    workload.diagnostics.ambiguous_count = 0u;
    workload.diagnostics.moved_count = 0u;
    workload.diagnostics.unkeyed_count = 0u;
    workload.diagnostics.exact_match_count = 0u;
    workload.diagnostics.exact_semitransparent_match_count = 0u;
    workload.diagnostics.source_geometry_match_count = 0u;
    workload.diagnostics.matched_vertex_count = 0u;
    workload.diagnostics.position_changed_vertex_count = 0u;
    workload.diagnostics.position_delta_fixed = 0u;
    workload.diagnostics.midpoint_distinct_vertex_count = 0u;
    workload.diagnostics.midpoint_collapsed_vertex_count = 0u;
    workload.diagnostics.midpoint_formula_failure_count = 0u;
    workload.diagnostics.retrospective_candidates = 0u;
    workload.diagnostics.retrospective_budget_exhausted = 0u;
    workload.diagnostics.retrospective_semitransparent_rejected = 0u;
    workload.building = false;
    return GPU_SEMANTIC_WORKLOAD_OK;
}

bool gpu_semantic_workload_current_frame_has_work(void) {
    if (workload.building) {
        return workload.frames[workload.building_index].authoritative_count !=
               0u;
    }
    return workload.has_sealed &&
           workload.frames[workload.sealed_index].authoritative_count != 0u;
}

size_t gpu_semantic_workload_current_count(void) {
    if (workload.building) {
        return workload.frames[workload.building_index].count;
    }
    return workload.has_sealed ? workload.frames[workload.sealed_index].count : 0u;
}

bool gpu_semantic_workload_previous_frame_usable(void) {
    return workload.diagnostics.previous_usable;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_interpolated(
    size_t index, GpuRenderSemantic *out_semantic) {
    return gpu_semantic_workload_interpolated_phase(
        index, 1u, 2u, out_semantic);
}

GpuSemanticWorkloadStatus gpu_semantic_workload_interpolated_phase(
    size_t index, unsigned int numerator, unsigned int denominator,
    GpuRenderSemantic *out_semantic) {
    const GpuSemanticFrame *current;
    const GpuSemanticFrame *previous;
    const GpuRenderSemantic *previous_semantic;
    size_t position_changed_vertices = 0u;
    uint64_t position_delta_fixed = 0u;
    size_t midpoint_distinct_vertices = 0u;
    size_t midpoint_collapsed_vertices = 0u;
    size_t midpoint_formula_failures = 0u;

    if (out_semantic == NULL || denominator < 2u ||
        denominator > GPU_SEMANTIC_INTERPOLATION_MAX_PHASES + 1u ||
        numerator == 0u || numerator >= denominator) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    }
    if (workload.building || !workload.has_sealed) {
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    }
    current = &workload.frames[workload.sealed_index];
    if (index >= current->count) {
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    }
    *out_semantic = current->items[index];
    if (workload.previous_match[index] < 0) {
        bool moved = false;

        if (workload.source_geometry_match[index])
            (void)interpolate_source_geometry(
                out_semantic, numerator, denominator, &moved,
                &position_changed_vertices, &position_delta_fixed,
                &midpoint_distinct_vertices, &midpoint_collapsed_vertices,
                &midpoint_formula_failures);
        return GPU_SEMANTIC_WORKLOAD_OK;
    }
    previous = &workload.frames[workload.sealed_index ^ 1u];
    previous_semantic = &previous->items[workload.previous_match[index]];
    (void)interpolate_semantic(out_semantic, previous_semantic,
                               numerator, denominator,
                               &position_changed_vertices,
                               &position_delta_fixed,
                               &midpoint_distinct_vertices,
                               &midpoint_collapsed_vertices,
                               &midpoint_formula_failures);
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_previous_order(
        const GpuRenderInterpolationIdentity *identity,
        size_t *out_previous_order) {
    GpuSemanticWorkloadMatchInfo match;
    GpuSemanticWorkloadStatus status;

    if (identity == NULL || out_previous_order == NULL || !identity->valid)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    status = gpu_semantic_workload_match_info(identity, &match);
    if (status != GPU_SEMANTIC_WORKLOAD_OK) return status;
    if (!match.previous_order_valid)
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    *out_previous_order = match.previous_order;
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_match_info(
        const GpuRenderInterpolationIdentity *identity,
        GpuSemanticWorkloadMatchInfo *out_match) {
    const GpuSemanticFrame *current;
    int32_t current_index;
    bool ambiguous;

    if (identity == NULL || out_match == NULL || !identity->valid)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    if (!workload.building && !workload.has_sealed)
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    current = &workload.frames[workload.building
        ? workload.building_index : workload.sealed_index];
    current_index = hash_lookup(
        workload.current_hash, current, identity, &ambiguous);
    if (current_index < 0 || ambiguous)
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    *out_match = (GpuSemanticWorkloadMatchInfo){
        .kind = workload.match_kind[(size_t)current_index],
        .fallback_kind = workload.fallback_kind[(size_t)current_index],
        .participation = (GpuSemanticWorkloadParticipation)
            current->participation[(size_t)current_index],
        .current_order = (size_t)current_index,
        .previous_order = workload.previous_match[(size_t)current_index] >= 0
            ? (size_t)workload.previous_match[(size_t)current_index] : 0u,
        .previous_order_valid =
            workload.previous_match[(size_t)current_index] >= 0,
    };
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_current(
        const GpuRenderInterpolationIdentity *identity,
        GpuRenderSemantic *out_semantic) {
    GpuSemanticWorkloadMatchInfo match;
    const GpuSemanticFrame *current;
    GpuSemanticWorkloadStatus status;

    if (out_semantic == NULL) return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    status = gpu_semantic_workload_match_info(identity, &match);
    if (status != GPU_SEMANTIC_WORKLOAD_OK) return status;
    current = &workload.frames[workload.building
        ? workload.building_index : workload.sealed_index];
    if (match.current_order >= current->count)
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    *out_semantic = current->items[match.current_order];
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_previous(
        const GpuRenderInterpolationIdentity *identity,
        GpuRenderSemantic *out_semantic) {
    const GpuSemanticFrame *previous;
    int32_t previous_index;
    bool ambiguous;

    if (identity == NULL || out_semantic == NULL || !identity->valid)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    if (!workload.building || !workload.has_sealed)
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    previous = &workload.frames[workload.sealed_index];
    previous_index = hash_lookup(
        workload.previous_hash, previous, identity, &ambiguous);
    if (previous_index < 0 || ambiguous)
        return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    *out_semantic = previous->items[(size_t)previous_index];
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_last_motion(
        GpuSemanticWorkloadMotionDiagnostics *out_motion) {
    if (out_motion == NULL)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    *out_motion = workload.last_motion;
    return out_motion->valid
        ? GPU_SEMANTIC_WORKLOAD_OK : GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
}

static bool material_position_equal(const GpuRenderMaterial *a,
                                    const GpuRenderMaterial *b) {
    return a->draw_area_left == b->draw_area_left &&
           a->draw_area_top == b->draw_area_top &&
           a->draw_area_right == b->draw_area_right &&
           a->draw_area_bottom == b->draw_area_bottom &&
           a->draw_offset_x == b->draw_offset_x &&
           a->draw_offset_y == b->draw_offset_y;
}

static bool anchor_producer_scene_present(
        const GpuSemanticAnchorFrame *anchors,
        const GpuRenderSemantic *semantic) {
    for (size_t index = 0u; index < anchors->count; ++index) {
        const GpuRenderInterpolationVertexAnchor *candidate =
            &anchors->items[index];

        if (candidate->scene_id ==
                semantic->interpolation_identity.scene_id &&
            candidate->producer_id ==
                semantic->interpolation_identity.producer_id)
            return true;
    }
    return false;
}

static void copy_material_position(GpuRenderMaterial *target,
                                   const GpuRenderMaterial *source) {
    target->draw_area_left = source->draw_area_left;
    target->draw_area_top = source->draw_area_top;
    target->draw_area_right = source->draw_area_right;
    target->draw_area_bottom = source->draw_area_bottom;
    target->draw_offset_x = source->draw_offset_x;
    target->draw_offset_y = source->draw_offset_y;
}

static void copy_vertex_position(GpuRenderSemanticVertex *target,
                                 const GpuRenderSemanticVertex *source) {
    target->x = source->x;
    target->y = source->y;
    target->native_view_x = source->native_view_x;
    target->native_view_y = source->native_view_y;
    target->native_view_position = source->native_view_position;
    target->projective_view_x = source->projective_view_x;
    target->projective_view_y = source->projective_view_y;
    target->projective_view_z = source->projective_view_z;
    target->projective_offset_x = source->projective_offset_x;
    target->projective_offset_y = source->projective_offset_y;
    target->projective_native_offset_x = source->projective_native_offset_x;
    target->projective_native_offset_y = source->projective_native_offset_y;
    target->projective_distance = source->projective_distance;
    target->projective_position = source->projective_position;
    target->temporal_depth = source->temporal_depth;
    target->temporal_depth_valid = source->temporal_depth_valid;
}

typedef enum RetiredCurrentGeometryStatus {
    RETIRED_CURRENT_GEOMETRY_OK = 0,
    RETIRED_CURRENT_GEOMETRY_ANCHOR_OVERFLOW,
    RETIRED_CURRENT_GEOMETRY_SCENE_MISMATCH,
    RETIRED_CURRENT_GEOMETRY_MISSING_ANCHOR,
    RETIRED_CURRENT_GEOMETRY_POSITION_MODE_MISMATCH,
    RETIRED_CURRENT_GEOMETRY_MATERIAL_POSITION_MISMATCH,
} RetiredCurrentGeometryStatus;

static RetiredCurrentGeometryStatus retired_current_geometry_status(
        const GpuRenderSemantic *previous,
        const GpuRenderMaterial **out_position_material) {
    const GpuSemanticAnchorFrame *current_anchors =
        &workload.anchor_frames[workload.sealed_index];
    const GpuRenderMaterial *position_material = NULL;
    const size_t count = semantic_vertex_count(previous);

    if (current_anchors->overflowed)
        return RETIRED_CURRENT_GEOMETRY_ANCHOR_OVERFLOW;
    for (size_t index = 0u; index < count; ++index) {
        const GpuRenderSemanticVertex *vertex =
            semantic_vertex_at(previous, index);
        const GpuRenderInterpolationVertexAnchor *anchor;

        if (!anchor_lookup(workload.current_anchor_hash, current_anchors,
                           previous, vertex, &anchor)) {
            if (!anchor_producer_scene_present(current_anchors, previous) &&
                current_anchors->count != 0u)
                return RETIRED_CURRENT_GEOMETRY_SCENE_MISMATCH;
            return RETIRED_CURRENT_GEOMETRY_MISSING_ANCHOR;
        }
        if (anchor->vertex.native_view_position !=
                vertex->native_view_position)
            return RETIRED_CURRENT_GEOMETRY_POSITION_MODE_MISMATCH;
        if (position_material == NULL)
            position_material = &anchor->material;
        else if (!material_position_equal(position_material,
                                          &anchor->material))
            return RETIRED_CURRENT_GEOMETRY_MATERIAL_POSITION_MISMATCH;
    }
    if (position_material == NULL)
        return RETIRED_CURRENT_GEOMETRY_MISSING_ANCHOR;
    if (out_position_material != NULL)
        *out_position_material = position_material;
    return RETIRED_CURRENT_GEOMETRY_OK;
}

static bool retired_current_geometry(
        const GpuRenderSemantic *previous,
        const GpuRenderMaterial **out_position_material) {
    return retired_current_geometry_status(previous, out_position_material) ==
        RETIRED_CURRENT_GEOMETRY_OK;
}

static bool interpolate_retired_source_geometry(
        const GpuRenderSemantic *previous, unsigned int numerator,
        unsigned int denominator, GpuRenderSemantic *out) {
    const GpuSemanticAnchorFrame *current_anchors =
        &workload.anchor_frames[workload.sealed_index];
    const GpuRenderMaterial *position_material;
    const size_t count = semantic_vertex_count(previous);

    if (!retired_current_geometry(previous, &position_material)) return false;
    *out = *previous;
    copy_material_position(&out->material, position_material);
    for (size_t index = 0u; index < count; ++index) {
        const GpuRenderSemanticVertex *local_previous_vertex =
            semantic_vertex_at(previous, index);
        const GpuRenderSemanticVertex *position_previous = NULL;
        const GpuRenderMaterial *position_previous_material = NULL;
        GpuRenderSemanticVertex *out_vertex =
            semantic_vertex_at_mutable(out, index);
        const GpuRenderInterpolationVertexAnchor *anchor;
        size_t changed = 0u;
        size_t distinct = 0u;
        size_t collapsed = 0u;
        size_t failures = 0u;
        uint64_t delta = 0u;

        if (!previous_vertex_lookup(
                previous, local_previous_vertex, &position_previous,
                &position_previous_material))
            return false;
        if (!anchor_lookup(workload.current_anchor_hash, current_anchors,
                           previous, local_previous_vertex, &anchor))
            return false;
        copy_vertex_position(out_vertex, &anchor->vertex);
        (void)interpolate_vertex(
            out_vertex, local_previous_vertex, position_previous,
            &out->material, position_previous_material, numerator,
            denominator, false, &changed, &delta, &distinct, &collapsed,
            &failures);
    }
    return true;
}

size_t gpu_semantic_workload_retired_count(void) {
    const GpuSemanticFrame *previous;
    size_t count = 0u;

    if (workload.building || !workload.has_sealed) return 0u;
    previous = &workload.frames[workload.sealed_index ^ 1u];
    for (size_t index = 0u; index < previous->count; ++index)
        if (previous->participation[index] ==
                GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT &&
            !workload.previous_corresponded[index] &&
            semantic_is_retirable_mesh(&previous->items[index]) &&
            retired_current_geometry(&previous->items[index], NULL))
            ++count;
    return count;
}

void gpu_semantic_workload_retired_diagnostics(
        uint32_t producer_id,
        GpuSemanticWorkloadRetiredDiagnostics *out_diagnostics) {
    const GpuSemanticFrame *previous;
    GpuSemanticWorkloadRetiredDiagnostics diagnostics = {0};

    if (out_diagnostics == NULL) return;
    if (!workload.building && workload.has_sealed) {
        previous = &workload.frames[workload.sealed_index ^ 1u];
        for (size_t index = 0u; index < previous->count; ++index) {
            const GpuRenderSemantic *semantic = &previous->items[index];

            if (previous->participation[index] !=
                    GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT ||
                workload.previous_corresponded[index] ||
                !semantic_is_retirable_mesh(semantic) ||
                semantic->interpolation_identity.producer_id != producer_id)
                continue;
            const RetiredCurrentGeometryStatus status =
                retired_current_geometry_status(semantic, NULL);

            ++diagnostics.unmatched;
            switch (status) {
            case RETIRED_CURRENT_GEOMETRY_OK:
                ++diagnostics.eligible;
                break;
            case RETIRED_CURRENT_GEOMETRY_ANCHOR_OVERFLOW:
                ++diagnostics.anchor_overflow;
                break;
            case RETIRED_CURRENT_GEOMETRY_SCENE_MISMATCH:
                ++diagnostics.scene_mismatch;
                break;
            case RETIRED_CURRENT_GEOMETRY_MISSING_ANCHOR:
                if (diagnostics.missing_anchor == 0u) {
                    const GpuSemanticAnchorFrame *anchors =
                        &workload.anchor_frames[workload.sealed_index];

                    diagnostics.first_missing_primitive_id =
                        semantic->interpolation_identity.primitive_id;
                    for (size_t vertex_index = 0u;
                         vertex_index < semantic_vertex_count(semantic);
                         ++vertex_index) {
                        const GpuRenderSemanticVertex *vertex =
                            semantic_vertex_at(semantic, vertex_index);
                        const GpuRenderInterpolationVertexAnchor *anchor;

                        if (anchor_lookup(
                                workload.current_anchor_hash, anchors,
                                semantic, vertex, &anchor))
                            continue;
                        diagnostics.first_missing_group_id =
                            vertex->interpolation_group_id;
                        diagnostics.first_missing_vertex_id =
                            vertex->interpolation_vertex_id;
                        break;
                    }
                }
                ++diagnostics.missing_anchor;
                break;
            case RETIRED_CURRENT_GEOMETRY_POSITION_MODE_MISMATCH:
                ++diagnostics.position_mode_mismatch;
                break;
            case RETIRED_CURRENT_GEOMETRY_MATERIAL_POSITION_MISMATCH:
                ++diagnostics.material_position_mismatch;
                break;
            }
        }
    }
    *out_diagnostics = diagnostics;
}

static void append_retired_issue(
        const GpuRenderSemantic *semantic,
        const GpuRenderSemanticVertex *vertex, size_t previous_order,
        GpuSemanticWorkloadRetiredIssueReason reason,
        GpuSemanticWorkloadRetiredIssue *out_issues, size_t capacity,
        size_t *in_out_count) {
    const size_t index = (*in_out_count)++;

    if (out_issues == NULL || index >= capacity) return;
    out_issues[index] = (GpuSemanticWorkloadRetiredIssue){
        .scene_id = semantic->interpolation_identity.scene_id,
        .producer_id = semantic->interpolation_identity.producer_id,
        .primitive_id = semantic->interpolation_identity.primitive_id,
        .group_id = vertex != NULL ? vertex->interpolation_group_id : 0u,
        .vertex_id = vertex != NULL ? vertex->interpolation_vertex_id : 0u,
        .previous_order = (uint32_t)previous_order,
        .reason = (uint32_t)reason,
    };
}

size_t gpu_semantic_workload_retired_issues(
        GpuSemanticWorkloadRetiredIssue *out_issues, size_t capacity) {
    const GpuSemanticFrame *previous;
    const GpuSemanticAnchorFrame *anchors;
    size_t count = 0u;

    if (out_issues == NULL && capacity != 0u) return 0u;
    if (workload.building || !workload.has_sealed) return 0u;
    previous = &workload.frames[workload.sealed_index ^ 1u];
    anchors = &workload.anchor_frames[workload.sealed_index];
    for (size_t previous_order = 0u; previous_order < previous->count;
         ++previous_order) {
        const GpuRenderSemantic *semantic = &previous->items[previous_order];
        const GpuRenderMaterial *position_material = NULL;

        if (previous->participation[previous_order] !=
                GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT ||
            workload.previous_corresponded[previous_order] ||
            !semantic_is_retirable_mesh(semantic))
            continue;
        if (anchors->overflowed) {
            append_retired_issue(
                semantic, NULL, previous_order,
                GPU_SEMANTIC_RETIRED_ISSUE_ANCHOR_OVERFLOW,
                out_issues, capacity, &count);
            continue;
        }
        for (size_t vertex_index = 0u;
             vertex_index < semantic_vertex_count(semantic); ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                semantic_vertex_at(semantic, vertex_index);
            const GpuRenderInterpolationVertexAnchor *anchor;

            if (!anchor_lookup(workload.current_anchor_hash, anchors,
                               semantic, vertex, &anchor)) {
                const GpuSemanticWorkloadRetiredIssueReason reason =
                    !anchor_producer_scene_present(anchors, semantic) &&
                            anchors->count != 0u
                        ? GPU_SEMANTIC_RETIRED_ISSUE_SCENE_MISMATCH
                        : GPU_SEMANTIC_RETIRED_ISSUE_MISSING_ANCHOR;
                append_retired_issue(
                    semantic, vertex, previous_order, reason,
                    out_issues, capacity, &count);
                continue;
            }
            if (anchor->vertex.native_view_position !=
                    vertex->native_view_position) {
                append_retired_issue(
                    semantic, vertex, previous_order,
                    GPU_SEMANTIC_RETIRED_ISSUE_POSITION_MODE_MISMATCH,
                    out_issues, capacity, &count);
                continue;
            }
            if (position_material == NULL) {
                position_material = &anchor->material;
            } else if (!material_position_equal(
                           position_material, &anchor->material)) {
                append_retired_issue(
                    semantic, vertex, previous_order,
                    GPU_SEMANTIC_RETIRED_ISSUE_MATERIAL_POSITION_MISMATCH,
                    out_issues, capacity, &count);
            }
        }
    }
    return count;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_retired(
        size_t retired_index, GpuRenderSemantic *out_semantic,
        size_t *out_previous_order) {
    const GpuSemanticFrame *previous;
    size_t ordinal = 0u;

    if (out_semantic == NULL || out_previous_order == NULL)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    if (workload.building || !workload.has_sealed)
        return GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION;
    previous = &workload.frames[workload.sealed_index ^ 1u];
    for (size_t index = 0u; index < previous->count; ++index) {
        if (previous->participation[index] !=
                GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT ||
            workload.previous_corresponded[index] ||
            !semantic_is_retirable_mesh(&previous->items[index]) ||
            !retired_current_geometry(&previous->items[index], NULL))
            continue;
        if (ordinal++ != retired_index) continue;
        *out_semantic = previous->items[index];
        *out_previous_order = index;
        return GPU_SEMANTIC_WORKLOAD_OK;
    }
    return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
}

static void reconcile_phase_vertex_positions(
        const GpuRenderSemantic *semantic, size_t item_index,
        GpuRenderSemantic *phases, size_t phase_count,
        bool insert_missing);

GpuSemanticWorkloadStatus gpu_semantic_workload_retired_phases(
        size_t retired_index, unsigned int denominator,
        GpuRenderSemantic *out_phases, size_t phase_count,
        size_t *out_previous_order) {
    GpuRenderSemantic previous;
    GpuSemanticWorkloadStatus status;

    if (out_phases == NULL || out_previous_order == NULL || denominator < 2u ||
        denominator > GPU_SEMANTIC_INTERPOLATION_MAX_PHASES + 1u ||
        phase_count != (size_t)denominator - 1u)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    status = gpu_semantic_workload_retired(
        retired_index, &previous, out_previous_order);
    if (status != GPU_SEMANTIC_WORKLOAD_OK) return status;
    for (size_t phase = 0u; phase < phase_count; ++phase)
        if (!interpolate_retired_source_geometry(
                &previous, (unsigned int)phase + 1u, denominator,
                &out_phases[phase]))
            return GPU_SEMANTIC_WORKLOAD_NOT_FOUND;
    reconcile_phase_vertex_positions(
        &previous, SIZE_MAX, out_phases, phase_count, false);
    return GPU_SEMANTIC_WORKLOAD_OK;
}

static void reconcile_phase_vertex_positions(
        const GpuRenderSemantic *semantic, size_t item_index,
        GpuRenderSemantic *phases, size_t phase_count,
        bool insert_missing) {
    const GpuSemanticFrame *current =
        &workload.frames[workload.building_index];
    const size_t vertex_count = semantic_vertex_count(semantic);

    for (size_t vertex_index = 0u; vertex_index < vertex_count;
         ++vertex_index) {
        const GpuRenderSemanticVertex *vertex =
            semantic_vertex_at(semantic, vertex_index);
        size_t slot;

        if (!vertex->interpolation_vertex_identity_valid) continue;
        slot = vertex_identity_hash(semantic, vertex);
        for (size_t probe = 0u; probe < GPU_SEMANTIC_VERTEX_HASH_CAPACITY;
             ++probe) {
            const int32_t entry = workload.phase_vertex_hash[slot];

            if (entry == 0) {
                if (!insert_missing) break;
                const size_t flat_index =
                    item_index * GPU_SEMANTIC_MAX_VERTICES + vertex_index;

                workload.phase_vertex_hash_touched[
                    workload.phase_vertex_hash_touched_count++] =
                        (uint16_t)slot;
                workload.phase_vertex_hash[slot] = (int32_t)flat_index + 1;
                for (size_t phase = 0u; phase < phase_count; ++phase) {
                    const GpuRenderSemanticVertex *phase_vertex =
                        semantic_vertex_at(&phases[phase], vertex_index);

                    workload.phase_positions[phase][flat_index] =
                        (GpuSemanticPhasePosition){
                            .x = phase_vertex->x,
                            .y = phase_vertex->y,
                            .native_x = phase_vertex->native_view_x,
                            .native_y = phase_vertex->native_view_y,
                        };
                }
                break;
            }
            {
                const size_t existing_flat = (size_t)(entry - 1);
                const size_t existing_item =
                    existing_flat / GPU_SEMANTIC_MAX_VERTICES;
                const size_t existing_vertex =
                    existing_flat % GPU_SEMANTIC_MAX_VERTICES;
                const GpuRenderSemantic *existing_semantic =
                    &current->items[existing_item];
                const GpuRenderSemanticVertex *existing =
                    semantic_vertex_at(existing_semantic, existing_vertex);

                if (vertex_identity_equal(
                        semantic, vertex, existing_semantic, existing)) {
                    for (size_t phase = 0u; phase < phase_count; ++phase) {
                        GpuRenderSemanticVertex *phase_vertex =
                            semantic_vertex_at_mutable(
                                &phases[phase], vertex_index);
                        const GpuSemanticPhasePosition *position =
                            &workload.phase_positions[phase][existing_flat];

                        phase_vertex->x = position->x;
                        phase_vertex->y = position->y;
                        phase_vertex->native_view_x = position->native_x;
                        phase_vertex->native_view_y = position->native_y;
                    }
                    break;
                }
            }
            slot = (slot + 1u) & (GPU_SEMANTIC_VERTEX_HASH_CAPACITY - 1u);
        }
    }
}

static GpuSemanticWorkloadStatus gpu_semantic_workload_record_phases_internal(
    const GpuRenderSemantic *semantic, unsigned int denominator,
    GpuRenderSemantic *out_phases, size_t phase_count,
    GpuSemanticWorkloadParticipation participation) {
    GpuRenderSemantic midpoint;
    GpuSemanticWorkloadStatus status;
    uint64_t projective_phase_vertices_before;
    size_t index;

    if (semantic == NULL || out_phases == NULL || denominator < 2u ||
        denominator > GPU_SEMANTIC_INTERPOLATION_MAX_PHASES + 1u ||
        phase_count != (size_t)denominator - 1u)
        return GPU_SEMANTIC_WORKLOAD_INVALID_ARGUMENT;
    projective_phase_vertices_before =
        workload.diagnostics.total_projective_phase_vertices;
    status = gpu_semantic_workload_record_internal(
        semantic, &midpoint, true, participation);
    if (status != GPU_SEMANTIC_WORKLOAD_OK) return status;
    workload.diagnostics.total_projective_phase_vertices =
        projective_phase_vertices_before;
    index = workload.frames[workload.building_index].count - 1u;
    for (size_t phase = 0u; phase < phase_count; ++phase) {
        const int32_t previous_index = workload.previous_match[index];

        out_phases[phase] = *semantic;
        if (previous_index >= 0) {
            const GpuSemanticFrame *previous =
                &workload.frames[workload.sealed_index];
            size_t changed = 0u;
            size_t distinct = 0u;
            size_t collapsed = 0u;
            size_t failures = 0u;
            uint64_t delta = 0u;
            (void)interpolate_semantic(
                &out_phases[phase], &previous->items[previous_index],
                (unsigned int)phase + 1u, denominator,
                &changed, &delta,
                &distinct, &collapsed, &failures);
        } else if (workload.source_geometry_match[index]) {
            bool moved = false;
            size_t changed = 0u;
            size_t distinct = 0u;
            size_t collapsed = 0u;
            size_t failures = 0u;
            uint64_t delta = 0u;

            (void)interpolate_source_geometry(
                &out_phases[phase], (unsigned int)phase + 1u, denominator,
                &moved, &changed, &delta, &distinct, &collapsed,
                &failures);
        }
    }
    reconcile_phase_vertex_positions(
        semantic, index, out_phases, phase_count, true);
    return GPU_SEMANTIC_WORKLOAD_OK;
}

GpuSemanticWorkloadStatus gpu_semantic_workload_record_phases(
    const GpuRenderSemantic *semantic, unsigned int denominator,
    GpuRenderSemantic *out_phases, size_t phase_count) {
    return gpu_semantic_workload_record_phases_internal(
        semantic, denominator, out_phases, phase_count,
        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_AUTHORITATIVE_CURRENT);
}

GpuSemanticWorkloadStatus gpu_semantic_workload_record_temporal_phases(
    const GpuRenderSemantic *semantic, unsigned int denominator,
    GpuRenderSemantic *out_phases, size_t phase_count) {
    return gpu_semantic_workload_record_phases_internal(
        semantic, denominator, out_phases, phase_count,
        GPU_SEMANTIC_WORKLOAD_PARTICIPATION_TEMPORAL_PHASE);
}

void gpu_semantic_workload_diagnostics(
    GpuSemanticWorkloadDiagnostics *out_diagnostics) {
    if (out_diagnostics != NULL) {
        *out_diagnostics = workload.diagnostics;
    }
}
