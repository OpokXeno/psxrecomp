#include "guest_render_native_stream.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0;                                                            \
    }                                                                        \
} while (0)

static GpuRenderSemantic make_semantic(uint8_t red) {
    GpuRenderSemantic semantic;

    memset(&semantic, 0, sizeof(semantic));
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.material.draw_area_right = 319u;
    semantic.material.draw_area_bottom = 239u;
    semantic.triangle_count = 1u;
    semantic.triangles[0].split_count = 1u;
    for (uint8_t index = 0u; index < 3u; ++index) {
        semantic.triangles[0].vertices[index].x =
            (int32_t)index * INT32_C(65536);
        semantic.triangles[0].vertices[index].y =
            (int32_t)index * INT32_C(65536);
        semantic.triangles[0].vertices[index].r = red;
        semantic.triangles[0].vertices[index].g = 2u;
        semantic.triangles[0].vertices[index].b = 3u;
    }
    return semantic;
}

static GpuRenderSemantic make_line_semantic(void) {
    GpuRenderSemantic semantic;

    memset(&semantic, 0, sizeof(semantic));
    semantic.material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
    semantic.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
    semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    semantic.material.draw_area_right = 319u;
    semantic.material.draw_area_bottom = 239u;
    semantic.topology = GPU_RENDER_SEMANTIC_LINES;
    semantic.line_count = 2u;
    for (uint8_t line = 0u; line < semantic.line_count; ++line) {
        for (uint8_t vertex = 0u; vertex < 2u; ++vertex) {
            semantic.lines[line].vertices[vertex].x =
                (int32_t)(line + vertex) * INT32_C(65536);
            semantic.lines[line].vertices[vertex].y =
                (int32_t)(line + vertex) * INT32_C(65536);
            semantic.lines[line].vertices[vertex].r = UINT8_C(0xff);
        }
    }
    return semantic;
}

static bool observe_source_writer(
        uint32_t source_word_address,
        GuestRenderNativeSourceWriter *out_writer) {
    if (out_writer == NULL) return false;
    if (source_word_address == 0x00123400u)
        *out_writer = (GuestRenderNativeSourceWriter){
            .pc = 0x80043c48u,
            .function = 0x80043c24u,
            .return_address = 0x800271c4u,
        };
    else if (source_word_address == 0x00123404u)
        *out_writer = (GuestRenderNativeSourceWriter){
            .pc = 0x8002e428u,
            .function = 0x8002e268u,
            .return_address = 0x800257dcu,
        };
    else
        return false;
    return true;
}

static GpuRenderSemantic resolver_semantic;
static GuestRenderNativeStreamMissContext resolver_expected;
static GuestRenderNativeStreamCommandIdentity resolver_identity;
static size_t resolved_semantic_observer_calls;
static uint64_t resolved_semantic_observer_command_id;

static bool observe_resolved_semantic(
        uint64_t command_id, const GpuRenderSemantic *semantic) {
    if (semantic == NULL) return false;
    resolved_semantic_observer_command_id = command_id;
    ++resolved_semantic_observer_calls;
    return true;
}

static bool contextual_resolver(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    if (context == NULL || out_visual_id == NULL || out_semantic == NULL ||
        memcmp(context, &resolver_expected, sizeof(*context)) != 0)
        return false;
    *out_visual_id = context->visual_id;
    *out_semantic = resolver_semantic;
    return true;
}

static bool visual_agnostic_resolver(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    if (context == NULL || out_visual_id == NULL || out_semantic == NULL ||
        context->command_id != resolver_identity.command_id ||
        context->container_id != resolver_identity.container_id ||
        context->source_kind != resolver_identity.source_kind ||
        context->opcode != resolver_identity.opcode ||
        context->word_count != resolver_identity.word_count)
        return false;
    *out_visual_id = context->visual_id;
    *out_semantic = resolver_semantic;
    return true;
}

static int test_disabled_and_validation(void) {
    const GpuRenderTransactionId id = {1u, 1u};
    GpuRenderSemantic semantic = make_semantic(1u);

    guest_render_native_stream_test_reset();
    CHECK(guest_render_native_stream_stage_exact(id, 0x100u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_DISABLED);
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              (GpuRenderTransactionId){0u, 1u}, 0x100u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    semantic.triangle_count = 0u;
    CHECK(guest_render_native_stream_stage_exact(id, 0x100u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    return 1;
}

static int test_identity_and_single_consumption(void) {
    const GpuRenderTransactionId first = {2u, 4u};
    const GpuRenderTransactionId newer = {2u, 5u};
    const GpuRenderTransactionId stale = {2u, 3u};
    const GpuRenderSemantic semantic_a = make_semantic(10u);
    const GpuRenderSemantic semantic_b = make_semantic(20u);
    GpuRenderTransactionId consumed_id;
    GpuRenderSemantic consumed;
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(first, 0x104u, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(first, 0x108u, &semantic_b) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(first, 0x104u, &semantic_b) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(newer, 0x10cu, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(stale, 0x104u, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID);
    CHECK(guest_render_native_stream_activate_visual(first) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              first, 0x108u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_id = first;
    CHECK(consumed_id.scene_epoch == first.scene_epoch);
    CHECK(consumed_id.state_sequence == first.state_sequence);
    CHECK(consumed.triangles[0].vertices[0].r == 20u);
    CHECK(guest_render_native_stream_consume_exact(
              first, 0x108u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
    CHECK(guest_render_native_stream_consume_exact(
              first, 0x104u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(newer, 0x10cu, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(newer) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              newer, 0x10cu, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_id = newer;
    CHECK(consumed_id.scene_epoch == newer.scene_epoch);
    CHECK(consumed_id.state_sequence == newer.state_sequence);
    CHECK(consumed.triangles[0].vertices[0].r == 10u);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.staged_count == 0u);
    CHECK(snapshot.total_staged == 5u);
    CHECK(snapshot.total_consumed == 3u);
    CHECK(snapshot.total_not_found == 1u);
    CHECK(snapshot.total_original_draws == 0u);
    CHECK(snapshot.total_visual_states == 2u);
    CHECK(snapshot.total_superseded == 2u);
    return 1;
}

static int test_reused_packet_address_expires_stale_visual(void) {
    const GpuRenderTransactionId first = {2u, 6u};
    const GpuRenderTransactionId second = {2u, 7u};
    const GpuRenderSemantic semantic_a = make_semantic(30u);
    const GpuRenderSemantic semantic_b = make_semantic(40u);
    GpuRenderTransactionId consumed_id;
    GpuRenderSemantic consumed;
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(first, 0x180u, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(second, 0x180u, &semantic_b) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(first) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.staged_count == 2u);
    CHECK(snapshot.total_superseded == 0u);

    CHECK(guest_render_native_stream_activate_visual(second) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              first, 0x180u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
    CHECK(guest_render_native_stream_consume_exact(
              second, 0x180u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_id = second;
    CHECK(consumed_id.scene_epoch == second.scene_epoch);
    CHECK(consumed_id.state_sequence == second.state_sequence);
    CHECK(consumed.triangles[0].vertices[0].r == 40u);
    CHECK(guest_render_native_stream_stage_exact(
              first, 0x180u, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID);
    return 1;
}

static int test_many_resolved_generations_use_bounded_lookup_state(void) {
    const GpuRenderSemantic semantic = make_semantic(45u);
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    for (uint64_t index = 0u; index < 4096u; ++index) {
        const GpuRenderTransactionId visual = {1u, index + 1u};
        CHECK(guest_render_native_stream_note_resolved_consumed(
                  visual, UINT64_C(0x100000) + index * 4u, &semantic) ==
              GUEST_RENDER_NATIVE_STREAM_OK);
    }
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.command_generation_count == 4096u);
    CHECK(guest_render_native_stream_note_resolved_consumed(
              (GpuRenderTransactionId){1u, 0u}, UINT64_C(0x100000),
              &semantic) == GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID);
    guest_render_native_stream_clear();
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.command_generation_count == 0u);
    CHECK(guest_render_native_stream_note_resolved_consumed(
              (GpuRenderTransactionId){1u, 1u}, UINT64_C(0x100000),
              &semantic) == GUEST_RENDER_NATIVE_STREAM_OK);
    return 1;
}

static int test_interleaved_visuals_remain_independently_active(void) {
    const GpuRenderTransactionId first = {10u, 1u};
    const GpuRenderTransactionId second = {10u, 2u};
    const GpuRenderSemantic semantic_a = make_semantic(61u);
    const GpuRenderSemantic semantic_b = make_semantic(62u);
    GpuRenderSemantic consumed;
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(first, 0x800u, &semantic_a) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(second, 0x900u, &semantic_b) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(first) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(second) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_has_exact(first, 0x800u));
    CHECK(guest_render_native_stream_has_exact(second, 0x900u));
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.staged_count == 2u);
    CHECK(snapshot.total_superseded == 0u);
    CHECK(guest_render_native_stream_consume_exact(
              second, 0x900u, &consumed) == GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(consumed.triangles[0].vertices[0].r == 62u);
    CHECK(guest_render_native_stream_consume_exact(
              first, 0x800u, &consumed) == GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(consumed.triangles[0].vertices[0].r == 61u);
    return 1;
}

static int test_exact_match_rejects_wrong_writer_provenance(void) {
    const GpuRenderTransactionId id = {11u, 3u};
    const GpuRenderSemantic semantic = make_semantic(63u);
    GuestRenderNativeStreamCommandIdentity identity = {
        .command_id = 0x00123400u,
        .container_id = 0x00123000u,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x20u,
        .word_count = 4u,
        .command_writer_valid = true,
        .command_writer = {
            .pc = 0x80043c48u,
            .function = 0x80043c24u,
            .return_address = 0x800271c4u,
        },
    };
    GpuRenderTransactionId matched;
    GpuRenderSemantic reserved_semantic;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_source_writer_observer(observe_source_writer);
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              id, identity.command_id, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_reserve_exact(
              31u, &identity, &semantic, &matched, &reserved_semantic) ==
           GUEST_RENDER_NATIVE_STREAM_OK);
    guest_render_native_stream_release_reservation(31u);
    CHECK(guest_render_native_stream_match_exact(
        &identity, &semantic, &matched));
    CHECK(matched.scene_epoch == id.scene_epoch &&
          matched.state_sequence == id.state_sequence);
    identity.command_writer.pc++;
    CHECK(!guest_render_native_stream_match_exact(
        &identity, &semantic, &matched));
    return 1;
}

static int test_packet_semantic_rejects_reused_address(void) {
    const GpuRenderTransactionId id = {11u, 4u};
    GpuRenderSemantic staged = make_semantic(64u);
    GpuRenderSemantic packet;
    const GuestRenderNativeStreamCommandIdentity identity = {
        .command_id = 0x00123500u,
        .container_id = 0x001234fcu,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x2eu,
        .word_count = 9u,
    };
    GpuRenderTransactionId matched;
    GpuRenderSemantic reserved;

    staged.material.textured = 1u;
    staged.material.raw_texture = 1u;
    packet = staged;
    packet.material.raw_texture = 0u;
    packet.material.semi_transparent = 1u;
    packet.triangles[0].vertices[0].x += INT32_C(16) * INT32_C(65536);

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              id, identity.command_id, &staged) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_reserve_exact(
              32u, &identity, &packet, &matched, &reserved) ==
          GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
    CHECK(!guest_render_native_stream_match_exact(
        &identity, &packet, &matched));
    CHECK(guest_render_native_stream_has_exact(id, identity.command_id));
    CHECK(guest_render_native_stream_reserve_exact(
              33u, &identity, &staged, &matched, &reserved) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    guest_render_native_stream_release_reservation(33u);
    return 1;
}

static int test_line_semantic(void) {
    const GpuRenderTransactionId id = {3u, 1u};
    GpuRenderSemantic semantic = make_line_semantic();
    GpuRenderSemantic consumed;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(id, 0x200u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              id, 0x200u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(consumed.topology == GPU_RENDER_SEMANTIC_LINES);
    CHECK(consumed.line_count == 2u);
    semantic.line_count = 0u;
    CHECK(guest_render_native_stream_stage_exact(id, 0x204u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    return 1;
}

static int test_original_draw_telemetry(void) {
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_note_original_draw(0x2cu);
    guest_render_native_stream_note_original_draw(0x3cu);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_original_draws == 2u);
    CHECK(snapshot.first_original_draw_opcode == 0x2cu);
    CHECK(snapshot.last_original_draw_opcode == 0x3cu);
    return 1;
}

static int test_double_buffer_reuse_preserves_pending_bindings(void) {
    const GpuRenderTransactionId first = {4u, 1u};
    const GpuRenderTransactionId newer = {5u, 0u};
    const GpuRenderTransactionId stale = {4u, 2u};
    const GpuRenderSemantic first_semantic = make_semantic(40u);
    const GpuRenderSemantic newer_semantic = make_semantic(50u);
    GpuRenderTransactionId consumed_id;
    GpuRenderSemantic consumed;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              first, 0x300u, &first_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(
              newer, 0x300u, &newer_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(
              stale, 0x300u, &first_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID);
    CHECK(guest_render_native_stream_activate_visual(newer) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              first, 0x300u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    CHECK(guest_render_native_stream_consume_exact(
              newer, 0x300u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_id = newer;
    CHECK(consumed_id.scene_epoch == newer.scene_epoch);
    CHECK(consumed.triangles[0].vertices[0].r == 50u);
    return 1;
}

static int test_raster_at_consume_tracks_double_buffer_lifetime(void) {
    const uint64_t packet_address = UINT64_C(0x000b0f30);
    const GpuRenderTransactionId upper_buffer = {5u, 1u};
    const GpuRenderTransactionId lower_buffer = {5u, 2u};
    const GpuRenderSemantic staged_upper = make_semantic(80u);
    const GpuRenderSemantic staged_lower = make_semantic(90u);
    GpuRenderSemantic rasterized;
    GpuRenderSemantic observed;
    GpuRenderTransactionId consumed_id;
    GpuRenderTransactionId observed_id;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              upper_buffer, packet_address, &staged_upper) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(
              lower_buffer, packet_address, &staged_lower) ==
          GUEST_RENDER_NATIVE_STREAM_OK);

    CHECK(guest_render_native_stream_activate_visual(upper_buffer) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              upper_buffer, packet_address, &rasterized) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_id = upper_buffer;
    CHECK(consumed_id.scene_epoch == upper_buffer.scene_epoch &&
          consumed_id.state_sequence == upper_buffer.state_sequence);
    rasterized.material.draw_area_top = 224u;
    rasterized.material.draw_area_bottom = 447u;
    rasterized.material.draw_offset_y = 224;
    rasterized.material.dither = 1u;
    rasterized.material.tpage = UINT16_C(0x009a);
    rasterized.material.texture_page_x = 10u;
    rasterized.material.texture_page_y = 1u;
    rasterized.material.texture_depth = GPU_RENDER_TEXTURE_8_BIT;
    rasterized.material.clut_x = 256u;
    rasterized.material.clut_y = 245u;
    guest_render_native_stream_note_rasterized(
        upper_buffer, packet_address, &rasterized);
    CHECK(guest_render_native_stream_last_consumed(
              upper_buffer, packet_address, &observed));
    observed_id = upper_buffer;
    CHECK(observed_id.state_sequence == upper_buffer.state_sequence);
    CHECK(observed.material.draw_area_top == 224u &&
          observed.material.draw_area_bottom == 447u &&
          observed.material.draw_offset_y == 224 &&
          observed.material.dither == 1u &&
          observed.material.tpage == UINT16_C(0x009a) &&
          observed.material.clut_x == 256u &&
          observed.material.clut_y == 245u);

    return 1;
}

static int test_ot_consumption_order_owns_draw_order(void) {
    const GpuRenderTransactionId id = {6u, 3u};
    const GpuRenderSemantic first_in_memory = make_semantic(60u);
    const GpuRenderSemantic first_in_ot = make_semantic(70u);
    GpuRenderTransactionId consumed_id;
    GpuRenderSemantic consumed;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              id, 0x400u, &first_in_memory) == GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(
              id, 0x500u, &first_in_ot) == GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              id, 0x500u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(consumed.triangles[0].vertices[0].r == 70u);
    CHECK(guest_render_native_stream_consume_exact(
              id, 0x400u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(consumed.triangles[0].vertices[0].r == 60u);
    return 1;
}

static int test_generic_native_telemetry(void) {
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_note_parser_replay_command(0x24u);
    guest_render_native_stream_note_parser_replay_command(0x02u);
    guest_render_native_stream_note_parser_replay_command(0x80u);
    guest_render_native_stream_note_native_line_segment();
    guest_render_native_stream_note_native_line_segment();
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_parser_replay_commands == 3u);
    CHECK(snapshot.total_parser_replay_draws == 1u);
    CHECK(snapshot.total_native_line_segments == 2u);
    return 1;
}

static int test_native_packet_coverage_telemetry(void) {
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_source_writer_observer(observe_source_writer);
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_note_native_packet_attribution(
        0x64u, false, false, 0x00123400u, 0x800465d4u, 0x1fc03fc0u,
        0x800467a0u);
    guest_render_native_stream_note_native_packet_source(
        0x2eu, true, true, 0x00123440u);
    guest_render_native_stream_note_native_packet_source(
        0x64u, false, false, 0x00123480u);
    guest_render_native_stream_note_native_packet_source(
        0x64u, false, false, 0x00123ffcu);
    guest_render_native_stream_note_native_draw_source(0x2eu, true);
    guest_render_native_stream_note_native_draw_source(0x24u, false);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_native_packets == 4u);
    CHECK(snapshot.total_native_bound_packets == 1u);
    CHECK(snapshot.total_native_unbound_packets == 3u);
    CHECK(snapshot.total_native_unsupported_packets == 3u);
    CHECK(snapshot.total_native_producer_bound_draws == 1u);
    CHECK(snapshot.total_native_packet_derived_draws == 1u);
    CHECK(snapshot.native_producer_bound_opcode_counts[0x2eu] == 1u);
    CHECK(snapshot.native_packet_derived_opcode_counts[0x24u] == 1u);
    CHECK(snapshot.first_native_unbound_opcode == 0x64u);
    CHECK(snapshot.last_native_unbound_opcode == 0x64u);
    CHECK(snapshot.first_native_unbound_source == 0x00123400u);
    CHECK(snapshot.first_native_unsupported_source == 0x00123400u);
    CHECK(snapshot.native_opcode_counts[0x64u] == 3u);
    CHECK(snapshot.native_unbound_opcode_counts[0x64u] == 3u);
    CHECK(snapshot.native_unsupported_opcode_counts[0x64u] == 3u);
    CHECK(snapshot.native_unbound_source_by_opcode[0x64u] == 0x00123400u);
    CHECK(snapshot.native_unbound_pc_by_opcode[0x64u] == 0x800465d4u);
    CHECK(snapshot.native_unsupported_pc_by_opcode[0x64u] == 0x800465d4u);
    CHECK(snapshot.first_native_unbound_return_address == 0x800467a0u);
    CHECK(snapshot.first_native_unsupported_return_address == 0x800467a0u);
    CHECK(snapshot.native_unbound_return_address_by_opcode[0x64u] ==
          0x800467a0u);
    CHECK(snapshot.native_unsupported_return_address_by_opcode[0x64u] ==
          0x800467a0u);
    CHECK(snapshot.native_unbound_pc_by_opcode[0x2eu] == UINT32_MAX);
    CHECK(snapshot.native_unbound_source_by_opcode[0x2eu] == UINT32_MAX);
    CHECK(snapshot.native_unbound_source_hotspots[0].opcode == 0x64u);
    CHECK(snapshot.native_unbound_source_hotspots[0].source_region_start ==
          0x00123000u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_source_address ==
          0x00123400u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_writer_pc ==
          0x80043c48u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_writer_function ==
          0x80043c24u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_writer_return_address ==
          0x800271c4u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_next_word_writer_pc ==
          0x8002e428u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_next_word_writer_function ==
          0x8002e268u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_next_word_writer_return_address ==
          0x800257dcu);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_payload_writers[0].pc ==
          0x80043c48u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_payload_writers[1].pc ==
          0x8002e428u);
    CHECK(snapshot.native_unbound_source_hotspots[0].representative_payload_writers[2].pc == 0u);
    CHECK(snapshot.native_unbound_source_hotspots[0].count == 3u);
    CHECK(snapshot.native_unbound_source_hotspots[0].error == 0u);
    return 1;
}

static int test_native_state_is_not_an_unbound_primitive(void) {
    GuestRenderNativeStreamSnapshot snapshot;
    GuestRenderNativeGpuState state;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    memset(&state, 0, sizeof(state));
    state.command_word = UINT32_C(0xe100043e);
    state.source_word_address = UINT32_C(0x0005a240);
    state.draw_mode = UINT16_C(0x043e);
    state.draw_area_right = 319u;
    state.draw_area_bottom = 239u;
    guest_render_native_stream_note_native_state(&state);
    state.command_word = UINT32_C(0xe2000010);
    state.source_word_address = UINT32_C(0x001fff50);
    state.texture_window_mask_x = 16u;
    guest_render_native_stream_note_native_state(&state);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_native_packets == 2u);
    CHECK(snapshot.total_native_bound_packets == 0u);
    CHECK(snapshot.total_native_state_packets == 2u);
    CHECK(snapshot.total_native_unbound_packets == 0u);
    CHECK(snapshot.total_native_unsupported_packets == 0u);
    CHECK(snapshot.native_opcode_counts[0xe1u] == 1u);
    CHECK(snapshot.native_opcode_counts[0xe2u] == 1u);
    CHECK(snapshot.native_state_opcode_counts[0xe1u] == 1u);
    CHECK(snapshot.native_state_opcode_counts[0xe2u] == 1u);
    CHECK(snapshot.native_unbound_opcode_counts[0xe1u] == 0u);
    CHECK(snapshot.last_native_state.sequence == 2u);
    CHECK(snapshot.last_native_state.command_word == UINT32_C(0xe2000010));
    CHECK(snapshot.last_native_state.source_word_address == UINT32_C(0x001fff50));
    CHECK(snapshot.last_native_state.texture_window_mask_x == 16u);
    return 1;
}

static int test_e1_e6_runtime_census_reclassification(void) {
    static const uint64_t expected[6] = {
        7286u, 6463u, 1741u, 1741u, 1741u, 2497u,
    };
    GuestRenderNativeStreamSnapshot snapshot;
    GuestRenderNativeGpuState state;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    memset(&state, 0, sizeof(state));
    for (uint8_t index = 0u; index < 6u; ++index) {
        state.command_word = (uint32_t)(0xe1u + index) << 24u;
        for (uint64_t count = 0u; count < expected[index]; ++count)
            guest_render_native_stream_note_native_state(&state);
    }
    for (uint64_t count = 0u; count < 38796u; ++count)
        guest_render_native_stream_note_native_packet(0x64u, false, true);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_native_packets == 60265u);
    CHECK(snapshot.total_native_state_packets == 21469u);
    CHECK(snapshot.total_native_unbound_packets == 38796u);
    for (uint8_t index = 0u; index < 6u; ++index) {
        const uint8_t opcode = (uint8_t)(0xe1u + index);
        CHECK(snapshot.native_state_opcode_counts[opcode] == expected[index]);
        CHECK(snapshot.native_unbound_opcode_counts[opcode] == 0u);
    }
    return 1;
}

static int test_native_fmv_telemetry(void) {
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_note_shared_fmv_present(320u, 240u, true);
    guest_render_native_stream_note_independent_fmv_present(368u, 240u, false);
    guest_render_native_stream_note_guest_gp0_command();
    guest_render_native_stream_note_shared_vram_present();
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_shared_fmv_frames == 1u);
    CHECK(snapshot.total_shared_fmv_pixels == (uint64_t)320u * 240u);
    CHECK(snapshot.last_shared_fmv_width == 320u);
    CHECK(snapshot.last_shared_fmv_height == 240u);
    CHECK(snapshot.last_shared_fmv_depth24);
    CHECK(snapshot.total_independent_fmv_frames == 1u);
    CHECK(snapshot.total_independent_fmv_pixels == (uint64_t)368u * 240u);
    CHECK(snapshot.last_independent_fmv_width == 368u);
    CHECK(snapshot.last_independent_fmv_height == 240u);
    CHECK(!snapshot.last_independent_fmv_depth24);
    CHECK(snapshot.total_guest_gp0_commands == 1u);
    CHECK(snapshot.total_shared_vram_presents == 1u);
    return 1;
}

static int test_capacity_and_clear(void) {
    const GpuRenderTransactionId id = {3u, 1u};
    const GpuRenderSemantic semantic = make_semantic(30u);
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(id, 0x200u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(id, 0x204u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(id, 0x208u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(id, 0x20cu, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(id, 0x210u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.staged_count == 0u);
    CHECK(snapshot.stage_failure_count == 1u);
    guest_render_native_stream_clear();
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.enabled);
    CHECK(snapshot.staged_count == 0u);
    CHECK(snapshot.total_staged == 4u);
    return 1;
}

static int test_abandon_visual_preserves_lossless_accounting(void) {
    const GpuRenderTransactionId id = {4u, 2u};
    const GpuRenderSemantic semantic = make_semantic(31u);
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(id, 0x220u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    guest_render_native_stream_abandon_visual(id);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.staged_count == 0u);
    CHECK(snapshot.total_staged == 1u);
    CHECK(snapshot.total_superseded == 1u);
    return 1;
}

static int test_clear_preserves_lossless_accounting(void) {
    const GpuRenderTransactionId id = {5u, 3u};
    const GpuRenderSemantic semantic = make_semantic(32u);
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(id, 0x224u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(id, 0x228u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    guest_render_native_stream_clear();
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.staged_count == 0u);
    CHECK(snapshot.total_staged == 2u);
    CHECK(snapshot.total_consumed == 0u);
    CHECK(snapshot.total_superseded == 2u);
    return 1;
}

static int test_contextual_resolver_rejects_incomplete_identity(void) {
    const GpuRenderTransactionId id = {8u, 2u};
    const GpuRenderSemantic staged = make_semantic(11u);
    GpuRenderSemantic resolved;
    GuestRenderNativeStreamMissContext context;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_set_miss_resolver(contextual_resolver);
    CHECK(guest_render_native_stream_stage_exact(id, 0x600u, &staged) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    resolver_semantic = make_semantic(99u);
    resolver_expected = (GuestRenderNativeStreamMissContext){
        .visual_id = id,
        .command_id = 0x604u,
        .container_id = 0x500u,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x20u,
        .word_count = 4u,
    };
    context = resolver_expected;
    context.source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN;
    CHECK(!guest_render_native_stream_resolve_miss(&context, &resolved));
    context = resolver_expected;
    context.visual_id.state_sequence++;
    CHECK(!guest_render_native_stream_resolve_miss(&context, &resolved));
    context = resolver_expected;
    context.word_count = 0u;
    CHECK(!guest_render_native_stream_resolve_miss(&context, &resolved));
    CHECK(guest_render_native_stream_resolve_miss(
        &resolver_expected, &resolved));
    CHECK(resolved.triangles[0].vertices[0].r == 99u);
    return 1;
}

static int test_suspend_and_reactivate_preserve_exact_visual(void) {
    const GpuRenderTransactionId id = {9u, 4u};
    const GpuRenderSemantic semantic = make_semantic(12u);
    GpuRenderSemantic consumed;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(id, 0x700u, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    guest_render_native_stream_suspend_visual(id);
    CHECK(!guest_render_native_stream_has_active_bindings());
    CHECK(!guest_render_native_stream_has_exact(id, 0x700u));
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_has_exact(id, 0x700u));
    CHECK(guest_render_native_stream_consume_exact(id, 0x700u, &consumed) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    return 1;
}

static int test_preflight_reservation_is_single_consumer_and_abortable(void) {
    const GpuRenderTransactionId id = {12u, 5u};
    const GpuRenderSemantic semantic = make_semantic(77u);
    const GuestRenderNativeStreamCommandIdentity first = {
        .command_id = 0x800u,
        .container_id = 0x700u,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK,
        .opcode = 0x64u,
        .word_count = 4u,
    };
    const GuestRenderNativeStreamCommandIdentity second = {
        .command_id = 0x900u,
        .container_id = 0x900u,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK,
        .opcode = 0x64u,
        .word_count = 4u,
    };
    GpuRenderTransactionId reserved_visual;
    GpuRenderSemantic reserved_semantic;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(
              id, first.command_id, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(
              id, second.command_id, &semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_reserve_exact(
              41u, &first, &semantic, &reserved_visual,
              &reserved_semantic) ==
           GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(!guest_render_native_stream_match_exact(
        &first, &semantic, &reserved_visual));
    CHECK(guest_render_native_stream_consume_reserved(
              41u, &first, id, &semantic, &reserved_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_reserved(
              41u, &first, id, &semantic, &reserved_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);

    CHECK(guest_render_native_stream_reserve_exact(
              42u, &second, &semantic, &reserved_visual,
              &reserved_semantic) ==
           GUEST_RENDER_NATIVE_STREAM_OK);
    guest_render_native_stream_release_reservation(42u);
    CHECK(guest_render_native_stream_match_exact(
        &second, &semantic, &reserved_visual));
    return 1;
}

static int test_equivalent_active_miss_resolutions_commit_once(void) {
    const GpuRenderTransactionId older = {20u, 3u};
    const GpuRenderTransactionId newer = {21u, 1u};
    const GpuRenderSemantic staged = make_semantic(70u);
    GpuRenderTransactionId resolved_visual;
    GpuRenderSemantic resolved_semantic;
    GuestRenderNativeStreamReserveDiagnostic diagnostic;
    GuestRenderNativeStreamSnapshot snapshot;

    guest_render_native_stream_test_reset();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_stage_exact(older, 0xa00u, &staged) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(older) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              older, 0xa00u, &resolved_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_stage_exact(newer, 0xb00u, &staged) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(newer) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_consume_exact(
              newer, 0xb00u, &resolved_semantic) ==
          GUEST_RENDER_NATIVE_STREAM_OK);

    resolver_identity = (GuestRenderNativeStreamCommandIdentity){
        .command_id = 0xc00u,
        .container_id = 0xbfcu,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x2cu,
        .word_count = 9u,
    };
    resolver_semantic = make_semantic(88u);
    guest_render_native_stream_set_miss_resolver(visual_agnostic_resolver);
    resolved_semantic_observer_calls = 0u;
    resolved_semantic_observer_command_id = 0u;
    guest_render_native_stream_set_resolved_semantic_observer(
        observe_resolved_semantic);
    CHECK(guest_render_native_stream_resolve_active_miss(
        &resolver_identity, &resolver_semantic, &resolved_visual,
        &resolved_semantic));
    CHECK(resolved_semantic_observer_calls == 0u);
    CHECK(resolved_visual.scene_epoch == newer.scene_epoch);
    CHECK(resolved_visual.state_sequence == newer.state_sequence);
    CHECK(resolved_semantic.triangles[0].vertices[0].r == 88u);
    guest_render_native_stream_reserve_diagnostic(&diagnostic);
    CHECK(diagnostic.active_count == 2u);
    CHECK(diagnostic.available_count == 2u);
    CHECK(guest_render_native_stream_note_resolved_consumed(
              resolved_visual, resolver_identity.command_id,
              &resolved_semantic) == GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(resolved_semantic_observer_calls == 1u);
    CHECK(resolved_semantic_observer_command_id == resolver_identity.command_id);
    CHECK(guest_render_native_stream_snapshot(&snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(snapshot.total_staged == 3u);
    CHECK(snapshot.total_consumed == 3u);
    return 1;
}

int main(void) {
    if (!test_disabled_and_validation()) return 1;
    if (!test_identity_and_single_consumption()) return 1;
    if (!test_reused_packet_address_expires_stale_visual()) return 1;
    if (!test_many_resolved_generations_use_bounded_lookup_state()) return 1;
    if (!test_interleaved_visuals_remain_independently_active()) return 1;
    if (!test_exact_match_rejects_wrong_writer_provenance()) return 1;
    if (!test_packet_semantic_rejects_reused_address()) return 1;
    if (!test_line_semantic()) return 1;
    if (!test_original_draw_telemetry()) return 1;
    if (!test_double_buffer_reuse_preserves_pending_bindings()) return 1;
    if (!test_raster_at_consume_tracks_double_buffer_lifetime()) return 1;
    if (!test_ot_consumption_order_owns_draw_order()) return 1;
    if (!test_generic_native_telemetry()) return 1;
    if (!test_native_packet_coverage_telemetry()) return 1;
    if (!test_native_state_is_not_an_unbound_primitive()) return 1;
    if (!test_e1_e6_runtime_census_reclassification()) return 1;
    if (!test_native_fmv_telemetry()) return 1;
    if (!test_capacity_and_clear()) return 1;
    if (!test_abandon_visual_preserves_lossless_accounting()) return 1;
    if (!test_clear_preserves_lossless_accounting()) return 1;
    if (!test_contextual_resolver_rejects_incomplete_identity()) return 1;
    if (!test_suspend_and_reactivate_preserve_exact_visual()) return 1;
    if (!test_preflight_reservation_is_single_consumer_and_abortable()) return 1;
    if (!test_equivalent_active_miss_resolutions_commit_once()) return 1;
    puts("guest_render_native_stream: all tests passed");
    return 0;
}
