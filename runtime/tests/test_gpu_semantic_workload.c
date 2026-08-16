#include "gpu_semantic_workload.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static int require(int condition) {
    return condition;
}

#define REQUIRE(condition) do { if (!require(condition)) return 0; } while (0)

static GpuRenderSemantic triangle(int32_t x, uint8_t identity) {
    GpuRenderSemantic semantic = {0};
    size_t i;

    semantic.topology = GPU_RENDER_SEMANTIC_TRIANGLES;
    semantic.triangle_count = 1u;
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, identity, 1u};
    semantic.material.textured = 1u;
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_8_BIT;
    semantic.triangles[0].split_count = 1u;
    for (i = 0; i < 3u; ++i) {
        GpuRenderSemanticVertex *vertex = &semantic.triangles[0].vertices[i];
        vertex->x = x + (int32_t)i;
        vertex->y = x + 10 + (int32_t)i;
        vertex->native_view_x = x + 20 + (int32_t)i;
        vertex->native_view_y = x + 30 + (int32_t)i;
        vertex->native_view_position = 1u;
        vertex->u = identity + (int32_t)i;
        vertex->v = identity + 3 + (int32_t)i;
        vertex->r = identity;
        vertex->g = identity + 1u;
        vertex->b = identity + 2u;
    }
    return semantic;
}

static GpuRenderSemantic line(int32_t x, uint8_t identity) {
    GpuRenderSemantic semantic = {0};
    size_t i;

    semantic.topology = GPU_RENDER_SEMANTIC_LINES;
    semantic.line_count = 1u;
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, identity, 1u};
    semantic.material.shading = GPU_RENDER_SHADING_GOURAUD;
    for (i = 0; i < 2u; ++i) {
        GpuRenderSemanticVertex *vertex = &semantic.lines[0].vertices[i];
        vertex->x = x + (int32_t)i;
        vertex->y = x + 2 + (int32_t)i;
        vertex->native_view_x = x + 4 + (int32_t)i;
        vertex->native_view_y = x + 6 + (int32_t)i;
        vertex->u = identity;
        vertex->r = identity;
    }
    return semantic;
}

static GpuRenderSemantic unkeyed_triangle(int32_t x, uint8_t appearance) {
    GpuRenderSemantic semantic = triangle(x, appearance);
    semantic.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    return semantic;
}

static GpuRenderSemantic unkeyed_triangle_pixels(int32_t x,
                                                  uint8_t appearance) {
    GpuRenderSemantic semantic = unkeyed_triangle(0, appearance);
    const int32_t offset = x * INT32_C(65536);

    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        semantic.triangles[0].vertices[vertex].x += offset;
        semantic.triangles[0].vertices[vertex].native_view_x += offset;
    }
    return semantic;
}

static GpuRenderSemantic quad_pixels(int32_t left, int32_t top,
                                     int32_t right, int32_t bottom,
                                     uint8_t identity, int reversed) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    const int32_t x[4] = {
        reversed ? right : left, reversed ? left : right,
        reversed ? left : right, reversed ? right : left,
    };
    const int32_t y[4] = {top, top, bottom, bottom};
    const int32_t u[4] = {0, 16, 16, 0};
    const int32_t v[4] = {0, 0, 16, 16};
    GpuRenderSemantic semantic = {0};

    semantic.topology = GPU_RENDER_SEMANTIC_TRIANGLES;
    semantic.triangle_count = 2u;
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 1u, identity, 1u};
    semantic.material.textured = 1u;
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_8_BIT;
    for (size_t triangle = 0u; triangle < 2u; ++triangle) {
        semantic.triangles[triangle].split_index = (uint8_t)triangle;
        semantic.triangles[triangle].split_count = 2u;
        for (size_t vertex = 0u; vertex < 3u; ++vertex) {
            const size_t source = split[triangle][vertex];
            GpuRenderSemanticVertex *out =
                &semantic.triangles[triangle].vertices[vertex];

            out->x = x[source] * INT32_C(65536);
            out->y = y[source] * INT32_C(65536);
            out->u = u[source] * INT32_C(65536);
            out->v = v[source] * INT32_C(65536);
            out->r = out->g = out->b = 128u;
        }
    }
    return semantic;
}

static GpuRenderSemantic mesh_triangle(
        uint8_t identity, const uint32_t vertex_ids[3],
        const int32_t x[3], const int32_t y[3]) {
    GpuRenderSemantic semantic = {0};

    semantic.topology = GPU_RENDER_SEMANTIC_TRIANGLES;
    semantic.triangle_count = 1u;
    semantic.interpolation_identity =
        (GpuRenderInterpolationIdentity){1u, 0x123u, identity, 1u};
    semantic.material.textured = 1u;
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_8_BIT;
    semantic.triangles[0].split_count = 1u;
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex *out =
            &semantic.triangles[0].vertices[vertex];

        out->x = x[vertex] * INT32_C(65536);
        out->y = y[vertex] * INT32_C(65536);
        out->u = (int32_t)vertex * INT32_C(65536);
        out->v = (int32_t)identity * INT32_C(65536);
        out->r = out->g = out->b = 128u;
        out->native_view_x = out->x + INT32_C(16384);
        out->native_view_y = out->y + INT32_C(16384);
        out->native_view_position = 1u;
        out->interpolation_group_id = 0x456u;
        out->interpolation_vertex_id = vertex_ids[vertex];
        out->interpolation_vertex_identity_valid = 1u;
    }
    return semantic;
}

static void shift_mesh_x(GpuRenderSemantic *semantic, int32_t pixels) {
    const int32_t delta = pixels * INT32_C(65536);

    for (size_t triangle = 0u; triangle < semantic->triangle_count;
         ++triangle)
        for (size_t vertex = 0u; vertex < 3u; ++vertex) {
            semantic->triangles[triangle].vertices[vertex].x += delta;
            semantic->triangles[triangle].vertices[vertex].native_view_x +=
                delta;
        }
}

static int seal_one(const GpuRenderSemantic *semantic) {
    GpuRenderSemantic midpoint;
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(semantic, &midpoint) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    return 1;
}

static int test_triangle_and_int64_midpoint(void) {
    GpuRenderSemantic previous = triangle(0, 1u);
    GpuRenderSemantic current = triangle(0, 1u);
    GpuRenderSemantic out;
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.triangles[0].vertices[0].x = INT32_MIN;
    current.triangles[0].vertices[0].x = INT32_MAX;
    previous.triangles[0].vertices[0].y = -100;
    current.triangles[0].vertices[0].y = 200;
    previous.triangles[0].vertices[0].native_view_x = -300;
    current.triangles[0].vertices[0].native_view_x = 500;
    previous.triangles[0].vertices[0].native_view_y = -700;
    current.triangles[0].vertices[0].native_view_y = 100;
    for (size_t vertex = 1u; vertex < 3u; ++vertex) {
        previous.triangles[0].vertices[vertex].x =
            current.triangles[0].vertices[vertex].x = (int32_t)vertex - 1;
        previous.triangles[0].vertices[vertex].y =
            current.triangles[0].vertices[vertex].y = 1000;
        previous.triangles[0].vertices[vertex].native_view_x =
            current.triangles[0].vertices[vertex].native_view_x =
                (int32_t)vertex - 1;
        previous.triangles[0].vertices[vertex].native_view_y =
            current.triangles[0].vertices[vertex].native_view_y = 1000;
    }
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 0);
    REQUIRE(out.triangles[0].vertices[0].y == 50);
    REQUIRE(out.triangles[0].vertices[0].native_view_x == 100);
    REQUIRE(out.triangles[0].vertices[0].native_view_y == -300);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_previous_frame_usable());
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.snapped_count == 0u);
    REQUIRE(diagnostics.matched_vertex_count == 3u);
    REQUIRE(diagnostics.position_changed_vertex_count == 1u);
    REQUIRE(diagnostics.position_delta_fixed != 0u);
    REQUIRE(diagnostics.midpoint_distinct_vertex_count == 1u);
    REQUIRE(diagnostics.midpoint_collapsed_vertex_count == 0u);
    REQUIRE(diagnostics.midpoint_formula_failure_count == 0u);
    return 1;
}

static int test_subpixel_midpoint_collapse_is_reported(void) {
    GpuRenderSemantic previous = triangle(0, 1u);
    GpuRenderSemantic current = previous;
    GpuRenderSemantic out;
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.triangles[0].vertices[0].native_view_x = 0;
    previous.triangles[0].vertices[0].native_view_y = 0;
    previous.triangles[0].vertices[1].native_view_x = 100;
    previous.triangles[0].vertices[1].native_view_y = 0;
    previous.triangles[0].vertices[2].native_view_x = 0;
    previous.triangles[0].vertices[2].native_view_y = 100;
    current = previous;
    current.triangles[0].vertices[0].native_view_x++;
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].native_view_x ==
            previous.triangles[0].vertices[0].native_view_x);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.position_changed_vertex_count == 1u);
    REQUIRE(diagnostics.midpoint_distinct_vertex_count == 0u);
    REQUIRE(diagnostics.midpoint_collapsed_vertex_count == 1u);
    REQUIRE(diagnostics.midpoint_formula_failure_count == 0u);
    return 1;
}

static int test_rational_phases(void) {
    GpuRenderSemantic previous = triangle(0, 1u);
    GpuRenderSemantic current = triangle(80, 1u);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic out;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 4u, phases, 3u) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(phases[0].triangles[0].vertices[0].x == 20);
    REQUIRE(phases[1].triangles[0].vertices[0].x == 40);
    REQUIRE(phases[2].triangles[0].vertices[0].x == 60);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_interpolated_phase(
                0u, 3u, 4u, &out) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 60);

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase)
        REQUIRE(phases[phase].triangles[0].vertices[0].x ==
                (int32_t)(phase + 1u) * 10);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    return 1;
}

static int test_projective_midpoint_reprojects_after_view_interpolation(void) {
    static const int32_t view_x[3] = {0, 192, 0};
    static const int32_t view_y[3] = {0, 0, 192};
    static const int32_t previous_x[3] = {160, 256, 160};
    static const int32_t previous_y[3] = {120, 120, 216};
    static const int32_t current_x[3] = {160, 192, 160};
    static const int32_t current_y[3] = {120, 120, 152};
    GpuRenderSemantic previous = triangle(0, 2u);
    GpuRenderSemantic current = triangle(0, 2u);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex *a =
            &previous.triangles[0].vertices[vertex];
        GpuRenderSemanticVertex *b =
            &current.triangles[0].vertices[vertex];

        a->x = previous_x[vertex] * INT32_C(65536);
        a->y = previous_y[vertex] * INT32_C(65536);
        b->x = current_x[vertex] * INT32_C(65536);
        b->y = current_y[vertex] * INT32_C(65536);
        a->native_view_x = a->native_view_y = 0;
        b->native_view_x = b->native_view_y = 0;
        a->native_view_position = b->native_view_position = 0u;
        a->projective_view_x = b->projective_view_x = view_x[vertex];
        a->projective_view_y = b->projective_view_y = view_y[vertex];
        a->projective_view_z = 512;
        b->projective_view_z = 1536;
        a->projective_offset_x = b->projective_offset_x =
            160 * INT32_C(65536);
        a->projective_offset_y = b->projective_offset_y =
            120 * INT32_C(65536);
        a->projective_distance = b->projective_distance = 256u;
        a->projective_position = b->projective_position = 1u;
    }
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 2u, phases, 1u) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(phases[0].triangles[0].vertices[1].x ==
            208 * INT32_C(65536));
    REQUIRE(phases[0].triangles[0].vertices[2].y ==
            168 * INT32_C(65536));
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.total_projective_phase_vertices == 3u);
    return 1;
}

static int test_line_topology(void) {
    GpuRenderSemantic previous = line(100, 7u);
    GpuRenderSemantic current = line(200, 7u);
    GpuRenderSemantic out;

    previous.line_count = 2u;
    current.line_count = 2u;
    previous.lines[1] = previous.lines[0];
    current.lines[1] = current.lines[0];
    previous.lines[1].vertices[0].x = 400;
    current.lines[1].vertices[0].x = 600;
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(seal_one(&current));
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.lines[0].vertices[0].x == 150);
    REQUIRE(out.lines[0].vertices[1].y == 153);
    REQUIRE(out.lines[1].vertices[0].x == 500);
    return 1;
}

static int test_unkeyed_line_translation_snaps(void) {
    GpuRenderSemantic previous = line(0, 7u);
    GpuRenderSemantic current = line(0, 7u);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    current.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    previous.lines[0].vertices[0].x = 32 * INT32_C(65536);
    previous.lines[0].vertices[0].y = 30 * INT32_C(65536);
    previous.lines[0].vertices[1].x = 48 * INT32_C(65536);
    previous.lines[0].vertices[1].y = 30 * INT32_C(65536);
    current.lines[0].vertices[0].x = 40 * INT32_C(65536);
    current.lines[0].vertices[0].y = 34 * INT32_C(65536);
    current.lines[0].vertices[1].x = 56 * INT32_C(65536);
    current.lines[0].vertices[1].y = 34 * INT32_C(65536);
    previous.material.draw_area_top = 224u;
    previous.material.draw_area_bottom = 447u;
    previous.material.draw_offset_y = 224;
    current.material.draw_area_bottom = 223u;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase)
        REQUIRE(phases[phase].lines[0].vertices[0].x ==
                    current.lines[0].vertices[0].x &&
                phases[phase].lines[0].vertices[0].y ==
                    current.lines[0].vertices[0].y);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.moved_count == 0u);

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&current));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.exact_match_count == 1u);
    return 1;
}

static int mismatch_snaps(void (*mutate)(GpuRenderSemantic *)) {
    GpuRenderSemantic previous = triangle(10, 9u);
    GpuRenderSemantic current = triangle(30, 9u);
    GpuRenderSemantic out;

    mutate(&current);
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(seal_one(&current));
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == current.triangles[0].vertices[0].x);
    return 1;
}

static int mutable_state_keeps_identity(void (*mutate)(GpuRenderSemantic *)) {
    GpuRenderSemantic previous = triangle(10, 9u);
    GpuRenderSemantic current = triangle(30, 9u);
    GpuRenderSemantic out;

    mutate(&current);
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(seal_one(&current));
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 20);
    return 1;
}

static void change_material(GpuRenderSemantic *semantic) {
    semantic->material.clut_x = 1u;
}

static void change_topology(GpuRenderSemantic *semantic) {
    *semantic = line(30, 9u);
}

static void change_uv(GpuRenderSemantic *semantic) {
    ++semantic->triangles[0].vertices[1].u;
}

static void change_color(GpuRenderSemantic *semantic) {
    ++semantic->triangles[0].vertices[2].g;
}

static void change_native_mode(GpuRenderSemantic *semantic) {
    semantic->triangles[0].vertices[0].native_view_position = 0u;
}

static void change_split_topology(GpuRenderSemantic *semantic) {
    semantic->triangles[0].split_count = 2u;
}

static void change_screen_space_mode(GpuRenderSemantic *semantic) {
    semantic->screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_STRETCH;
}

static int test_all_compatibility_mismatches(void) {
    REQUIRE(mismatch_snaps(change_material));
    REQUIRE(mismatch_snaps(change_topology));
    REQUIRE(mismatch_snaps(change_uv));
    REQUIRE(mutable_state_keeps_identity(change_color));
    REQUIRE(mismatch_snaps(change_native_mode));
    REQUIRE(mismatch_snaps(change_split_topology));
    REQUIRE(mismatch_snaps(change_screen_space_mode));
    return 1;
}

static int test_clip_origin_change_does_not_translate_geometry(void) {
    GpuRenderSemantic previous = triangle(10 * INT32_C(65536), 9u);
    GpuRenderSemantic current = previous;
    GpuRenderSemantic out;
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.material.draw_area_left = 0u;
    previous.material.draw_area_right = 319u;
    current.material.draw_area_left = 2u;
    current.material.draw_area_right = 321u;
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(seal_one(&current));
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x ==
            current.triangles[0].vertices[0].x);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.moved_count == 0u);
    return 1;
}

static int test_used_better_retrospective_candidate_snaps(void) {
    GpuRenderSemantic previous_near = unkeyed_triangle_pixels(0, 7u);
    GpuRenderSemantic previous_far = unkeyed_triangle_pixels(100, 7u);
    GpuRenderSemantic current_exact = unkeyed_triangle_pixels(0, 7u);
    GpuRenderSemantic current_near = unkeyed_triangle_pixels(10, 7u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_near, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_far, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current_exact, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current_near, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x ==
            current_near.triangles[0].vertices[0].x);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.ambiguous_count == 1u);
    return 1;
}

static int test_birth_and_death_same_ordinal(void) {
    GpuRenderSemantic a0 = triangle(0, 1u);
    GpuRenderSemantic dead = triangle(10, 2u);
    GpuRenderSemantic b0 = triangle(20, 3u);
    GpuRenderSemantic a1 = triangle(100, 1u);
    GpuRenderSemantic born = triangle(200, 4u);
    GpuRenderSemantic b1 = triangle(300, 3u);
    GpuRenderSemantic out;
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&a0, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&dead, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&b0, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&a1, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 50);
    REQUIRE(gpu_semantic_workload_record(&born, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 200);
    REQUIRE(gpu_semantic_workload_record(&b1, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 160);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 50);
    REQUIRE(gpu_semantic_workload_interpolated(1u, &out) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 200);
    REQUIRE(gpu_semantic_workload_interpolated(2u, &out) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 160);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 2u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.previous_usable);
    REQUIRE(diagnostics.last_seal_eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH);
    REQUIRE(diagnostics.total_partial_incomplete_match_frames == 1u);
    return 1;
}

static int test_duplicate_and_reorder_same_ordinal(void) {
    GpuRenderSemantic first = triangle(0, 1u);
    GpuRenderSemantic duplicate = triangle(50, 1u);
    GpuRenderSemantic moved_first = triangle(100, 1u);
    GpuRenderSemantic a = triangle(10, 2u);
    GpuRenderSemantic b = triangle(20, 3u);
    GpuRenderSemantic moved_a = triangle(110, 2u);
    GpuRenderSemantic moved_b = triangle(120, 3u);
    GpuRenderSemantic out;
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&first));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&moved_first, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 50);
    REQUIRE(gpu_semantic_workload_record(&duplicate, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_CONFLICT);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.moved_count == 0u);
    REQUIRE(diagnostics.snapped_count == 2u);
    REQUIRE(diagnostics.ambiguous_count == 1u);
    REQUIRE(!diagnostics.previous_usable);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_CONFLICT);
    REQUIRE(gpu_semantic_workload_discard_current() ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_previous_frame_usable());

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&a, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&b, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&moved_b, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&moved_a, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 70);
    REQUIRE(gpu_semantic_workload_interpolated(1u, &out) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 60);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 2u);
    REQUIRE(diagnostics.ambiguous_count == 0u);
    return 1;
}

static int test_unkeyed_retrospective_match_and_color(void) {
    GpuRenderSemantic previous_a = unkeyed_triangle(0, 11u);
    GpuRenderSemantic previous_b = unkeyed_triangle(100, 23u);
    GpuRenderSemantic current_b = unkeyed_triangle(120, 23u);
    GpuRenderSemantic current_a = unkeyed_triangle(40, 11u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    current_a.triangles[0].vertices[0].r = 31u;
    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_a, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_b, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current_b, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 110);
    REQUIRE(gpu_semantic_workload_record(&current_a, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 20);
    REQUIRE(recorded.triangles[0].vertices[0].r == 21u);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_previous_frame_usable());
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 2u);
    REQUIRE(diagnostics.moved_count == 2u);
    REQUIRE(diagnostics.unkeyed_count == 2u);
    REQUIRE(diagnostics.snapped_count == 0u);
    return 1;
}

static int test_unkeyed_semitransparent_primitive_snaps(void) {
    GpuRenderSemantic previous = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic current = unkeyed_triangle_pixels(40, 11u);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.material.semi_transparent = 1u;
    previous.material.blend_mode = GPU_RENDER_BLEND_SUBTRACT;
    current.material.semi_transparent = 1u;
    current.material.blend_mode = GPU_RENDER_BLEND_SUBTRACT;
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase)
        REQUIRE(phases[phase].triangles[0].vertices[0].x ==
                current.triangles[0].vertices[0].x);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.retrospective_semitransparent_rejected == 1u);
    REQUIRE(diagnostics.total_retrospective_semitransparent_rejected == 1u);
    return 1;
}

static int test_unkeyed_large_translation_snaps(void) {
    GpuRenderSemantic previous = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic current = unkeyed_triangle_pixels(65, 11u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x ==
            current.triangles[0].vertices[0].x);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.moved_count == 0u);
    return 1;
}

static int test_exact_unkeyed_semitransparent_primitive_matches(void) {
    GpuRenderSemantic static_blend = unkeyed_triangle_pixels(40, 11u);
    GpuRenderSemantic previous_moving = unkeyed_triangle_pixels(0, 23u);
    GpuRenderSemantic current_moving = unkeyed_triangle_pixels(60, 23u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    static_blend.material.semi_transparent = 1u;
    static_blend.material.blend_mode = GPU_RENDER_BLEND_SUBTRACT;
    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_moving, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&static_blend, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&static_blend, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current_moving, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&static_blend, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&static_blend, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 3u);
    REQUIRE(diagnostics.snapped_count == 0u);
    REQUIRE(diagnostics.ambiguous_count == 0u);
    REQUIRE(diagnostics.exact_match_count == 2u);
    REQUIRE(diagnostics.exact_semitransparent_match_count == 2u);
    REQUIRE(diagnostics.total_exact_semitransparent_matches == 2u);
    REQUIRE(diagnostics.moved_count == 1u);
    REQUIRE(diagnostics.previous_usable);
    REQUIRE(diagnostics.last_seal_eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_ELIGIBLE);
    REQUIRE(diagnostics.total_eligible_frames == 1u);
    REQUIRE(diagnostics.total_rejected_no_previous_frames == 1u);
    return 1;
}

static int test_keyed_semitransparent_primitive_interpolates(void) {
    GpuRenderSemantic previous = triangle(0, 11u);
    GpuRenderSemantic current = triangle(80, 11u);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];

    previous.material.semi_transparent = 1u;
    previous.material.blend_mode = GPU_RENDER_BLEND_SUBTRACT;
    current.material.semi_transparent = 1u;
    current.material.blend_mode = GPU_RENDER_BLEND_SUBTRACT;
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase)
        REQUIRE(phases[phase].triangles[0].vertices[0].x ==
                (int32_t)(phase + 1u) * 10);
    return 1;
}

static int test_keyed_quad_winding_flip_snaps(void) {
    GpuRenderSemantic previous = quad_pixels(0, 0, 32, 48, 12u, 0);
    GpuRenderSemantic current = quad_pixels(0, 0, 32, 48, 12u, 1);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase) {
        REQUIRE(phases[phase].triangles[0].vertices[0].x ==
                current.triangles[0].vertices[0].x);
        REQUIRE(phases[phase].triangles[0].vertices[1].x ==
                current.triangles[0].vertices[1].x);
    }
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    return 1;
}

static int test_keyed_quad_translation_interpolates(void) {
    GpuRenderSemantic previous = quad_pixels(0, 0, 32, 48, 12u, 0);
    GpuRenderSemantic current = quad_pixels(80, 0, 112, 48, 12u, 0);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase)
        REQUIRE(phases[phase].triangles[0].vertices[0].x ==
                (int32_t)(phase + 1u) * 10 * INT32_C(65536));
    return 1;
}

static int test_keyed_quad_animation_cell_snaps(void) {
    GpuRenderSemantic previous = quad_pixels(0, 0, 32, 48, 12u, 0);
    GpuRenderSemantic current = quad_pixels(80, -4, 120, 52, 12u, 0);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    for (size_t triangle = 0u; triangle < current.triangle_count; ++triangle)
        for (size_t vertex = 0u; vertex < 3u; ++vertex)
            current.triangles[triangle].vertices[vertex].u +=
                32 * INT32_C(65536);
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase) {
        for (size_t triangle = 0u; triangle < current.triangle_count;
             ++triangle) {
            for (size_t vertex = 0u; vertex < 3u; ++vertex) {
                REQUIRE(phases[phase].triangles[triangle].vertices[vertex].x ==
                        current.triangles[triangle].vertices[vertex].x);
                REQUIRE(phases[phase].triangles[triangle].vertices[vertex].y ==
                        current.triangles[triangle].vertices[vertex].y);
                REQUIRE(phases[phase].triangles[triangle].vertices[vertex].u ==
                        current.triangles[triangle].vertices[vertex].u);
                REQUIRE(phases[phase].triangles[triangle].vertices[vertex].v ==
                        current.triangles[triangle].vertices[vertex].v);
            }
        }
    }
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    return 1;
}

static int test_unkeyed_dialogue_line_growth_snaps(void) {
    GpuRenderSemantic previous = quad_pixels(28, 24, 116, 37, 12u, 0);
    GpuRenderSemantic current = quad_pixels(28, 24, 120, 37, 12u, 0);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    current.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    for (size_t triangle = 0u; triangle < 2u; ++triangle) {
        for (size_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *previous_vertex =
                &previous.triangles[triangle].vertices[vertex];
            GpuRenderSemanticVertex *current_vertex =
                &current.triangles[triangle].vertices[vertex];

            if (previous_vertex->x == 116 * INT32_C(65536))
                previous_vertex->u = 88 * INT32_C(65536);
            if (current_vertex->x == 120 * INT32_C(65536))
                current_vertex->u = 92 * INT32_C(65536);
        }
    }
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase) {
        for (size_t triangle = 0u; triangle < 2u; ++triangle) {
            for (size_t vertex = 0u; vertex < 3u; ++vertex) {
                REQUIRE(phases[phase].triangles[triangle].vertices[vertex].x ==
                        current.triangles[triangle].vertices[vertex].x);
                REQUIRE(phases[phase].triangles[triangle].vertices[vertex].u ==
                        current.triangles[triangle].vertices[vertex].u);
            }
        }
    }
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    return 1;
}

static int test_unkeyed_textured_translation_interpolates(void) {
    GpuRenderSemantic previous = quad_pixels(28, 24, 116, 37, 12u, 0);
    GpuRenderSemantic current = quad_pixels(36, 24, 124, 37, 12u, 0);
    GpuRenderSemantic phases[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    current.interpolation_identity = (GpuRenderInterpolationIdentity){0};
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current, 8u, phases, 7u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 7u; ++phase)
        REQUIRE(phases[phase].triangles[0].vertices[0].x ==
                (28 + (int32_t)phase + 1) * INT32_C(65536));
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.snapped_count == 0u);
    return 1;
}

static int test_shared_mesh_vertices_keep_one_phase_when_material_snaps(void) {
    static const uint32_t ids_a[3] = {0u, 1u, 2u};
    static const uint32_t ids_b[3] = {2u, 1u, 3u};
    static const int32_t x_a[3] = {0, 16, 0};
    static const int32_t y_a[3] = {0, 0, 16};
    static const int32_t x_b[3] = {0, 16, 16};
    static const int32_t y_b[3] = {16, 0, 16};
    GpuRenderSemantic previous_a =
        mesh_triangle(1u, ids_a, x_a, y_a);
    GpuRenderSemantic previous_b =
        mesh_triangle(2u, ids_b, x_b, y_b);
    GpuRenderSemantic current_a = previous_a;
    GpuRenderSemantic current_b = previous_b;
    GpuRenderSemantic phases_a[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic phases_b[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    shift_mesh_x(&current_a, 8);
    shift_mesh_x(&current_b, 8);
    for (size_t vertex = 0u; vertex < 3u; ++vertex)
        current_a.triangles[0].vertices[vertex].u +=
            32 * INT32_C(65536);

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_a, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_b, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current_a, 4u, phases_a, 3u) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current_b, 4u, phases_b, 3u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t phase = 0u; phase < 3u; ++phase) {
        const int32_t expected_delta =
            (int32_t)(phase + 1u) * 2 * INT32_C(65536);

        REQUIRE(phases_a[phase].triangles[0].vertices[1].x ==
                previous_a.triangles[0].vertices[1].x + expected_delta);
        REQUIRE(phases_a[phase].triangles[0].vertices[2].x ==
                phases_b[phase].triangles[0].vertices[0].x);
        REQUIRE(phases_a[phase].triangles[0].vertices[1].x ==
                phases_b[phase].triangles[0].vertices[1].x);
        REQUIRE(phases_a[phase].triangles[0].vertices[2].native_view_x ==
                phases_b[phase].triangles[0].vertices[0].native_view_x);
        REQUIRE(phases_a[phase].triangles[0].vertices[0].u ==
                current_a.triangles[0].vertices[0].u);
    }
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    {
        GpuSemanticWorkloadMatchInfo match;

        REQUIRE(gpu_semantic_workload_match_info(
                    &current_a.interpolation_identity, &match) ==
                GPU_SEMANTIC_WORKLOAD_OK);
        REQUIRE(match.kind == GPU_SEMANTIC_WORKLOAD_MATCH_SOURCE_GEOMETRY);
        REQUIRE(match.fallback_kind ==
                GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_INCOMPATIBLE);
        REQUIRE(match.current_order == 0u);
        REQUIRE(!match.previous_order_valid);
        REQUIRE(gpu_semantic_workload_match_info(
                    &current_b.interpolation_identity, &match) ==
                GPU_SEMANTIC_WORKLOAD_OK);
        REQUIRE(match.kind == GPU_SEMANTIC_WORKLOAD_MATCH_IDENTITY);
        REQUIRE(match.fallback_kind == GPU_SEMANTIC_WORKLOAD_MATCH_UNKNOWN);
        REQUIRE(match.current_order == 1u);
        REQUIRE(match.previous_order_valid && match.previous_order == 1u);
    }
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.source_geometry_match_count == 1u);
    REQUIRE(diagnostics.total_source_geometry_matches == 1u);
    REQUIRE(diagnostics.moved_count == 2u);
    REQUIRE(diagnostics.max_semantic_position_delta_fixed ==
            24u * UINT64_C(65536));
    REQUIRE(diagnostics.max_semantic_identity_scene == 1u);
    REQUIRE(diagnostics.max_semantic_identity_producer == 0x123u);
    REQUIRE(diagnostics.max_semantic_identity_primitive == 1u);
    REQUIRE(diagnostics.max_semantic_identity_valid);
    REQUIRE(diagnostics.previous_usable);
    return 1;
}

static int test_conflicting_previous_shared_vertex_keeps_motion_coherent(void) {
    static const uint32_t ids_a[3] = {0u, 1u, 2u};
    static const uint32_t ids_b[3] = {2u, 3u, 4u};
    static const int32_t x_a[3] = {0, 16, 32};
    static const int32_t x_b[3] = {96, 112, 128};
    static const int32_t x_current_a[3] = {160, 176, 192};
    static const int32_t x_current_b[3] = {192, 208, 224};
    static const int32_t y[3] = {0, 0, 16};
    GpuRenderSemantic previous_a = mesh_triangle(1u, ids_a, x_a, y);
    GpuRenderSemantic previous_b = mesh_triangle(2u, ids_b, x_b, y);
    GpuRenderSemantic current_a =
        mesh_triangle(1u, ids_a, x_current_a, y);
    GpuRenderSemantic current_b =
        mesh_triangle(2u, ids_b, x_current_b, y);
    GpuRenderSemantic phase_a[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic phase_b[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic recorded;

    current_b.triangles[0].vertices[0].x += INT32_C(32768);
    current_b.triangles[0].vertices[0].native_view_x += INT32_C(32768);
    current_a.triangles[0].vertices[0].u += INT32_C(65536);

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_a, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_b, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current_a, 2u, phase_a, 1u) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current_b, 2u, phase_b, 1u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        REQUIRE(phase_a[0].triangles[0].vertices[vertex].x !=
                current_a.triangles[0].vertices[vertex].x);
        REQUIRE(phase_b[0].triangles[0].vertices[vertex].x !=
                current_b.triangles[0].vertices[vertex].x);
    }
    REQUIRE(phase_a[0].triangles[0].vertices[2].x ==
            112 * INT32_C(65536));
    REQUIRE(phase_a[0].triangles[0].vertices[2].x ==
            phase_b[0].triangles[0].vertices[0].x);
    {
        GpuSemanticWorkloadDiagnostics diagnostics;

        gpu_semantic_workload_diagnostics(&diagnostics);
        REQUIRE(diagnostics.source_geometry_match_count == 1u);
    }
    return 1;
}

static int test_shared_vertex_identity_is_namespaced_by_producer(void) {
    static const uint32_t ids[3] = {0u, 1u, 2u};
    static const int32_t x_a[3] = {0, 16, 32};
    static const int32_t x_b[3] = {96, 112, 128};
    static const int32_t y[3] = {0, 0, 16};
    GpuRenderSemantic previous_a = mesh_triangle(1u, ids, x_a, y);
    GpuRenderSemantic previous_b = mesh_triangle(2u, ids, x_b, y);
    GpuRenderSemantic current_a = previous_a;
    GpuRenderSemantic current_b = previous_b;
    GpuRenderSemantic phase_a[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic phase_b[GPU_SEMANTIC_INTERPOLATION_MAX_PHASES];
    GpuRenderSemantic recorded;

    previous_b.interpolation_identity.producer_id = 0x124u;
    current_b.interpolation_identity.producer_id = 0x124u;
    shift_mesh_x(&current_a, 8);
    shift_mesh_x(&current_b, 8);

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_a, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_b, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current_a, 2u, phase_a, 1u) == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(
                &current_b, 2u, phase_b, 1u) == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        REQUIRE(phase_a[0].triangles[0].vertices[vertex].x ==
                previous_a.triangles[0].vertices[vertex].x +
                    4 * INT32_C(65536));
        REQUIRE(phase_b[0].triangles[0].vertices[vertex].x ==
                previous_b.triangles[0].vertices[vertex].x +
                    4 * INT32_C(65536));
    }
    return 1;
}

static int retrospective_appearance_limit_case(uint32_t uv_delta,
                                               uint32_t color_delta,
                                               int should_match) {
    GpuRenderSemantic previous = unkeyed_triangle(0, 0u);
    GpuRenderSemantic current = unkeyed_triangle(40, 0u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    current.triangles[0].vertices[0].u +=
        (int32_t)(uv_delta << GPU_RENDER_FIXED_FRACTION_BITS);
    for (size_t vertex = 0u; vertex < 3u; ++vertex)
        current.triangles[0].vertices[vertex].r = 192u;
    if (color_delta > 576u)
        current.triangles[0].vertices[0].r = 193u;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x ==
            (should_match ? 20 : 40));
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == (should_match ? 1u : 0u));
    REQUIRE(diagnostics.snapped_count == (should_match ? 0u : 1u));
    return 1;
}

static int test_retrospective_appearance_limits(void) {
    REQUIRE(retrospective_appearance_limit_case(0u, 576u, 1));
    REQUIRE(retrospective_appearance_limit_case(1u, 576u, 0));
    REQUIRE(retrospective_appearance_limit_case(0u, 577u, 0));
    return 1;
}

static int test_unkeyed_ambiguity_is_local(void) {
    GpuRenderSemantic previous_a = unkeyed_triangle(0, 11u);
    GpuRenderSemantic previous_b = unkeyed_triangle(20, 11u);
    GpuRenderSemantic previous_unique = unkeyed_triangle(100, 23u);
    GpuRenderSemantic ambiguous = unkeyed_triangle(10, 11u);
    GpuRenderSemantic current_unique = unkeyed_triangle(140, 23u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_a, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_b, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_unique, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&ambiguous, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 10);
    REQUIRE(gpu_semantic_workload_record(&current_unique, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 120);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_previous_frame_usable());
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.ambiguous_count == 1u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.previous_usable);
    REQUIRE(diagnostics.last_seal_eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH);
    return 1;
}

static int test_unkeyed_scene_boundary_snaps(void) {
    GpuRenderSemantic previous = unkeyed_triangle(0, 11u);
    GpuRenderSemantic current = unkeyed_triangle(40, 11u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    previous.interpolation_identity.scene_id = 1u;
    current.interpolation_identity.scene_id = 2u;
    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(seal_one(&current));
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(!diagnostics.previous_usable);
    REQUIRE(diagnostics.last_seal_eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_INCOMPLETE_MATCH);
    return 1;
}

static int unkeyed_birth_order_case(int survivor_first) {
    GpuRenderSemantic previous_left = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic previous_survivor = unkeyed_triangle_pixels(20, 11u);
    GpuRenderSemantic born = unkeyed_triangle_pixels(9, 11u);
    GpuRenderSemantic survivor = unkeyed_triangle_pixels(21, 11u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_left, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_survivor, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    if (survivor_first) {
        REQUIRE(gpu_semantic_workload_record(&survivor, &recorded) ==
                GPU_SEMANTIC_WORKLOAD_OK);
        REQUIRE(recorded.triangles[0].vertices[0].x ==
                20 * INT32_C(65536) + INT32_C(32768));
        REQUIRE(gpu_semantic_workload_record(&born, &recorded) ==
                GPU_SEMANTIC_WORKLOAD_OK);
        REQUIRE(recorded.triangles[0].vertices[0].x ==
                9 * INT32_C(65536));
    } else {
        REQUIRE(gpu_semantic_workload_record(&born, &recorded) ==
                GPU_SEMANTIC_WORKLOAD_OK);
        REQUIRE(recorded.triangles[0].vertices[0].x ==
                9 * INT32_C(65536));
        REQUIRE(gpu_semantic_workload_record(&survivor, &recorded) ==
                GPU_SEMANTIC_WORKLOAD_OK);
        REQUIRE(recorded.triangles[0].vertices[0].x ==
                20 * INT32_C(65536) + INT32_C(32768));
    }
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 1u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.ambiguous_count == 1u);
    REQUIRE(diagnostics.moved_count == 1u);
    REQUIRE(diagnostics.previous_usable);
    REQUIRE(diagnostics.last_seal_eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_INCOMPLETE_MATCH);
    return 1;
}

static int test_unkeyed_birth_is_local_and_order_independent(void) {
    REQUIRE(unkeyed_birth_order_case(0));
    REQUIRE(unkeyed_birth_order_case(1));
    return 1;
}

static int test_unkeyed_crossing_snaps_locally(void) {
    GpuRenderSemantic previous_left = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic previous_right = unkeyed_triangle_pixels(20, 11u);
    GpuRenderSemantic current_right = unkeyed_triangle_pixels(11, 11u);
    GpuRenderSemantic current_left = unkeyed_triangle_pixels(9, 11u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_left, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&previous_right, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current_right, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 11 * INT32_C(65536));
    REQUIRE(gpu_semantic_workload_record(&current_left, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 9 * INT32_C(65536));
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 2u);
    REQUIRE(diagnostics.ambiguous_count == 2u);
    return 1;
}

static int test_unkeyed_candidate_budget_fails_closed(void) {
    GpuRenderSemantic semantic = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic current = unkeyed_triangle_pixels(100, 11u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t index = 0u; index < 1025u; ++index) {
        semantic.triangles[0].vertices[0].r = (uint8_t)index;
        REQUIRE(gpu_semantic_workload_record(&semantic, &recorded) ==
                GPU_SEMANTIC_WORKLOAD_OK);
    }
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(recorded.triangles[0].vertices[0].x == 100 * INT32_C(65536));
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.matched_count == 0u);
    REQUIRE(diagnostics.snapped_count == 1u);
    REQUIRE(diagnostics.ambiguous_count == 1u);
    REQUIRE(diagnostics.retrospective_candidates == 1025u);
    REQUIRE(diagnostics.retrospective_budget_exhausted == 1u);
    return 1;
}

static int test_overflow_and_reset(void) {
    GpuRenderSemantic semantic = triangle(1, 1u);
    GpuRenderSemantic out;
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;
    size_t i;

    gpu_semantic_workload_reset();
    REQUIRE(!gpu_semantic_workload_current_frame_has_work());
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    for (i = 0; i < GPU_SEMANTIC_WORKLOAD_CAPACITY; ++i) {
        semantic.interpolation_identity.primitive_id = (uint32_t)i + 1u;
        semantic.triangles[0].vertices[0].u = (int32_t)i;
        REQUIRE(gpu_semantic_workload_record(&semantic, &recorded) == GPU_SEMANTIC_WORKLOAD_OK);
    }
    semantic.interpolation_identity.primitive_id =
        GPU_SEMANTIC_WORKLOAD_CAPACITY + 1u;
    REQUIRE(gpu_semantic_workload_record(&semantic, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_CAPACITY_EXCEEDED);
    REQUIRE(gpu_semantic_workload_current_frame_has_work());
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.current_count == GPU_SEMANTIC_WORKLOAD_CAPACITY);
    REQUIRE(diagnostics.current_overflowed);
    REQUIRE(diagnostics.total_dropped == 1u);
    REQUIRE(!diagnostics.previous_usable);

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_current_count() == 0u);
    REQUIRE(gpu_semantic_workload_interpolated(0u, &out) ==
            GPU_SEMANTIC_WORKLOAD_INVALID_TRANSITION);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.sealed_frames == 0u);
    REQUIRE(diagnostics.total_recorded == 0u);
    REQUIRE(diagnostics.total_dropped == 0u);
    return 1;
}

static int test_discard_preserves_last_sealed_source(void) {
    GpuRenderSemantic previous = triangle(10, 5u);
    GpuRenderSemantic current = triangle(30, 5u);
    GpuRenderSemantic out;

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_discard_current() ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_previous_frame_usable());
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &out) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(out.triangles[0].vertices[0].x == 20);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_previous_frame_usable());
    return 1;
}

static int test_last_seal_snapshot_survives_next_begin(void) {
    GpuRenderSemantic previous = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic current = unkeyed_triangle_pixels(40, 11u);
    GpuRenderSemantic born = triangle(200, 9u);
    GpuRenderSemantic recorded;
    GpuSemanticWorkloadDiagnostics diagnostics;
    uint64_t epoch;

    gpu_semantic_workload_reset();
    gpu_semantic_workload_diagnostics(&diagnostics);
    epoch = diagnostics.epoch;
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&born, &recorded) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.epoch == epoch);
    REQUIRE(diagnostics.current_count == 0u);
    REQUIRE(diagnostics.last_seal_previous_count == 1u);
    REQUIRE(diagnostics.last_seal_current_count == 2u);
    REQUIRE(diagnostics.last_seal_previous_unkeyed_count == 1u);
    REQUIRE(diagnostics.last_seal_current_unkeyed_count == 1u);
    REQUIRE(diagnostics.last_seal_matched_count == 1u);
    REQUIRE(diagnostics.last_seal_snapped_count == 1u);
    REQUIRE(diagnostics.last_seal_moved_count == 1u);
    REQUIRE(diagnostics.last_seal_eligibility ==
            GPU_SEMANTIC_WORKLOAD_ELIGIBILITY_PARTIAL_COUNT_MISMATCH);
    REQUIRE(diagnostics.previous_usable);
    REQUIRE(diagnostics.total_partial_count_mismatch_frames == 1u);
    REQUIRE(!diagnostics.last_seal_previous_overflowed);
    REQUIRE(!diagnostics.last_seal_current_overflowed);
    REQUIRE(gpu_semantic_workload_discard_current() ==
            GPU_SEMANTIC_WORKLOAD_OK);

    gpu_semantic_workload_reset();
    gpu_semantic_workload_diagnostics(&diagnostics);
    REQUIRE(diagnostics.epoch > epoch);
    return 1;
}

static int test_last_motion_diagnostics_include_unkeyed_match(void) {
    GpuRenderSemantic previous = unkeyed_triangle_pixels(0, 11u);
    GpuRenderSemantic current = unkeyed_triangle_pixels(8, 11u);
    GpuRenderSemantic midpoint;
    GpuSemanticWorkloadMotionDiagnostics motion;

    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_last_motion(&motion) ==
            GPU_SEMANTIC_WORKLOAD_NOT_FOUND);
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record(&current, &midpoint) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_last_motion(&motion) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(motion.valid);
    REQUIRE(motion.previous_valid);
    REQUIRE(motion.match_kind == GPU_SEMANTIC_WORKLOAD_MATCH_RETROSPECTIVE);
    REQUIRE(motion.current_order == 0u);
    REQUIRE(motion.previous_order == 0u);
    REQUIRE(motion.position_changed_vertex_count == 3u);
    REQUIRE(motion.position_delta_fixed != 0u);
    REQUIRE(motion.previous.triangles[0].vertices[0].x ==
            previous.triangles[0].vertices[0].x);
    REQUIRE(motion.current.triangles[0].vertices[0].x ==
            current.triangles[0].vertices[0].x);
    REQUIRE(motion.midpoint.triangles[0].vertices[0].x ==
            midpoint.triangles[0].vertices[0].x);
    return 1;
}

static int test_hidden_mesh_anchors_supply_previous_geometry(void) {
    static const uint32_t ids[3] = {10u, 11u, 12u};
    static const int32_t previous_x[3] = {0, 16, 0};
    static const int32_t current_x[3] = {8, 24, 8};
    static const int32_t y[3] = {0, 0, 16};
    GpuRenderSemantic previous =
        mesh_triangle(41u, ids, previous_x, y);
    GpuRenderSemantic current =
        mesh_triangle(41u, ids, current_x, y);
    GpuRenderInterpolationVertexAnchor anchors[3];
    GpuRenderSemantic phase[1];
    GpuSemanticWorkloadMatchInfo match;

    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        anchors[vertex] = (GpuRenderInterpolationVertexAnchor){
            .scene_id = previous.interpolation_identity.scene_id,
            .producer_id = previous.interpolation_identity.producer_id,
            .material = previous.material,
            .vertex = previous.triangles[0].vertices[vertex],
        };
    }
    gpu_semantic_workload_reset();
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_anchors(anchors, 3u) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_phases(&current, 2u, phase, 1u) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    for (size_t vertex = 0u; vertex < 3u; ++vertex)
        REQUIRE(phase[0].triangles[0].vertices[vertex].x ==
                (previous.triangles[0].vertices[vertex].x +
                 current.triangles[0].vertices[vertex].x) / 2);
    REQUIRE(gpu_semantic_workload_match_info(
                &current.interpolation_identity, &match) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(match.kind == GPU_SEMANTIC_WORKLOAD_MATCH_SOURCE_GEOMETRY);
    REQUIRE(match.fallback_kind ==
            GPU_SEMANTIC_WORKLOAD_MATCH_SNAPPED_NOT_FOUND);
    return 1;
}

static void make_projective_mesh(GpuRenderSemantic *semantic) {
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex *position =
            &semantic->triangles[0].vertices[vertex];

        position->projective_view_x = 0;
        position->projective_view_y = 0;
        position->projective_view_z = 1024;
        position->projective_offset_x = position->x;
        position->projective_offset_y = position->y;
        position->projective_native_offset_x =
            position->native_view_x - position->x;
        position->projective_native_offset_y =
            position->native_view_y - position->y;
        position->projective_distance = 256u;
        position->projective_position = 1u;
    }
}

static int test_retired_mesh_follows_current_anchors(void) {
    static const uint32_t ids[3] = {20u, 21u, 22u};
    static const int32_t previous_x[3] = {0, 16, 0};
    static const int32_t current_x[3] = {8, 24, 8};
    static const int32_t y[3] = {0, 0, 16};
    GpuRenderSemantic previous =
        mesh_triangle(42u, ids, previous_x, y);
    GpuRenderSemantic current =
        mesh_triangle(43u, ids, current_x, y);
    GpuRenderInterpolationVertexAnchor anchors[3];
    GpuRenderSemantic phases[3];
    size_t previous_order = SIZE_MAX;

    make_projective_mesh(&previous);
    make_projective_mesh(&current);
    previous.material.tpage = 1u;
    current.material.tpage = 2u;
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        anchors[vertex] = (GpuRenderInterpolationVertexAnchor){
            .scene_id = previous.interpolation_identity.scene_id,
            .producer_id = previous.interpolation_identity.producer_id,
            .material = current.material,
            .vertex = current.triangles[0].vertices[vertex],
        };
    }

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_anchors(anchors, 3u) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_retired_count() == 1u);
    REQUIRE(gpu_semantic_workload_retired_phases(
                0u, 4u, phases, 3u, &previous_order) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(previous_order == 0u);
    REQUIRE(phases[0].triangles[0].vertices[0].x ==
            2 * INT32_C(65536));
    REQUIRE(phases[1].triangles[0].vertices[0].x ==
            4 * INT32_C(65536));
    REQUIRE(phases[2].triangles[0].vertices[0].x ==
            6 * INT32_C(65536));
    REQUIRE(phases[1].triangles[0].vertices[0].u ==
            previous.triangles[0].vertices[0].u);
    REQUIRE(phases[1].triangles[0].vertices[0].r ==
            previous.triangles[0].vertices[0].r);
    REQUIRE(phases[1].material.tpage == previous.material.tpage);

    gpu_semantic_workload_reset();
    REQUIRE(seal_one(&previous));
    REQUIRE(gpu_semantic_workload_begin() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_record_anchors(anchors, 2u) ==
            GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_seal() == GPU_SEMANTIC_WORKLOAD_OK);
    REQUIRE(gpu_semantic_workload_retired_count() == 0u);
    return 1;
}

int main(void) {
    if (!test_triangle_and_int64_midpoint()) return 1;
    if (!test_subpixel_midpoint_collapse_is_reported()) return 2;
    if (!test_rational_phases()) return 3;
    if (!test_projective_midpoint_reprojects_after_view_interpolation()) return 4;
    if (!test_line_topology()) return 5;
    if (!test_unkeyed_line_translation_snaps()) return 36;
    if (!test_all_compatibility_mismatches()) return 5;
    if (!test_birth_and_death_same_ordinal()) return 6;
    if (!test_duplicate_and_reorder_same_ordinal()) return 7;
    if (!test_unkeyed_retrospective_match_and_color()) return 8;
    if (!test_unkeyed_semitransparent_primitive_snaps()) return 9;
    if (!test_unkeyed_large_translation_snaps()) return 10;
    if (!test_exact_unkeyed_semitransparent_primitive_matches()) return 11;
    if (!test_keyed_semitransparent_primitive_interpolates()) return 12;
    if (!test_keyed_quad_winding_flip_snaps()) return 13;
    if (!test_keyed_quad_translation_interpolates()) return 15;
    if (!test_keyed_quad_animation_cell_snaps()) return 16;
    if (!test_unkeyed_dialogue_line_growth_snaps()) return 17;
    if (!test_unkeyed_textured_translation_interpolates()) return 18;
    if (!test_shared_mesh_vertices_keep_one_phase_when_material_snaps())
        return 19;
    if (!test_conflicting_previous_shared_vertex_keeps_motion_coherent())
        return 31;
    if (!test_shared_vertex_identity_is_namespaced_by_producer()) return 32;
    if (!test_retrospective_appearance_limits()) return 20;
    if (!test_unkeyed_ambiguity_is_local()) return 21;
    if (!test_unkeyed_scene_boundary_snaps()) return 22;
    if (!test_unkeyed_birth_is_local_and_order_independent()) return 23;
    if (!test_unkeyed_crossing_snaps_locally()) return 24;
    if (!test_unkeyed_candidate_budget_fails_closed()) return 25;
    if (!test_overflow_and_reset()) return 26;
    if (!test_discard_preserves_last_sealed_source()) return 27;
    if (!test_last_seal_snapshot_survives_next_begin()) return 28;
    if (!test_last_motion_diagnostics_include_unkeyed_match()) return 35;
    if (!test_clip_origin_change_does_not_translate_geometry()) return 29;
    if (!test_used_better_retrospective_candidate_snaps()) return 30;
    if (!test_hidden_mesh_anchors_supply_previous_geometry()) return 33;
    if (!test_retired_mesh_follows_current_anchors()) return 34;
    return 0;
}
