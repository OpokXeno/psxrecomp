#ifndef PSX_GUEST_RENDER_NATIVE_STREAM_H
#define PSX_GUEST_RENDER_NATIVE_STREAM_H

#include "gpu_render.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef GUEST_RENDER_NATIVE_STREAM_CAPACITY
#define GUEST_RENDER_NATIVE_STREAM_CAPACITY 4096u
#endif

#define GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT 256u
#define GUEST_RENDER_NATIVE_STREAM_HOTSPOT_CAPACITY 64u
#define GUEST_RENDER_NATIVE_STREAM_HOTSPOT_REGION_SIZE 4096u
#define GUEST_RENDER_NATIVE_STREAM_PAYLOAD_WRITER_COUNT 9u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GuestRenderNativeStreamStatus {
    GUEST_RENDER_NATIVE_STREAM_OK = 0,
    GUEST_RENDER_NATIVE_STREAM_DISABLED,
    GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT,
    GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED,
    GUEST_RENDER_NATIVE_STREAM_DUPLICATE_COMMAND,
    GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID,
    GUEST_RENDER_NATIVE_STREAM_NOT_FOUND,
} GuestRenderNativeStreamStatus;

typedef struct GuestRenderNativeSourceWriter {
    uint32_t pc;
    uint32_t function;
    uint32_t return_address;
} GuestRenderNativeSourceWriter;

typedef struct GuestRenderNativeSourceHotspot {
    uint64_t count;
    uint64_t error;
    uint32_t source_region_start;
    uint32_t representative_source_address;
    uint32_t representative_packet_pc;
    uint32_t representative_packet_function;
    uint32_t representative_packet_return_address;
    uint32_t first_frame;
    uint32_t last_frame;
    uint32_t representative_writer_pc;
    uint32_t representative_writer_function;
    uint32_t representative_writer_return_address;
    uint32_t representative_next_word_writer_pc;
    uint32_t representative_next_word_writer_function;
    uint32_t representative_next_word_writer_return_address;
    GuestRenderNativeSourceWriter representative_payload_writers[
        GUEST_RENDER_NATIVE_STREAM_PAYLOAD_WRITER_COUNT];
    uint8_t opcode;
} GuestRenderNativeSourceHotspot;

typedef struct GuestRenderNativeGpuState {
    uint64_t sequence;
    uint32_t command_word;
    uint32_t source_word_address;
    uint16_t draw_mode;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    uint8_t dither;
    uint8_t draw_to_display;
    uint8_t texture_disable;
    uint8_t mask_set;
    uint8_t mask_check;
} GuestRenderNativeGpuState;

typedef bool (*GuestRenderNativeSourceWriterObserver)(
    uint32_t source_word_address, GuestRenderNativeSourceWriter *out_writer);

typedef struct GuestRenderNativeStreamSnapshot {
    GpuRenderTransactionId visual_id;
    size_t staged_count;
    size_t command_generation_count;
    uint64_t total_staged;
    uint64_t total_consumed;
    uint64_t total_consumed_keyed;
    uint64_t total_consumed_unkeyed;
    uint64_t total_rasterized_keyed;
    uint64_t total_rasterized_unkeyed;
    uint64_t total_not_found;
    uint64_t total_original_draws;
    uint8_t first_original_draw_opcode;
    uint8_t last_original_draw_opcode;
    uint64_t total_parser_replay_commands;
    uint64_t total_parser_replay_draws;
    uint64_t total_native_line_segments;
    uint64_t total_shared_fmv_frames;
    uint64_t total_shared_fmv_pixels;
    uint32_t last_shared_fmv_width;
    uint32_t last_shared_fmv_height;
    bool last_shared_fmv_depth24;
    uint64_t total_independent_fmv_frames;
    uint64_t total_independent_fmv_pixels;
    uint32_t last_independent_fmv_width;
    uint32_t last_independent_fmv_height;
    bool last_independent_fmv_depth24;
    uint64_t total_ui_ot_adapter_calls;
    uint64_t total_guest_gp0_commands;
    uint64_t total_shared_vram_presents;
    uint64_t total_native_lists;
    uint64_t total_native_packets;
    uint64_t total_native_bound_packets;
    uint64_t total_native_state_packets;
    uint64_t total_native_unbound_packets;
    uint64_t total_native_producer_bound_draws;
    uint64_t total_native_packet_derived_draws;
    uint64_t total_native_unsupported_packets;
    uint64_t total_independent_vram_presents;
    uint8_t first_native_unsupported_opcode;
    uint8_t last_native_unsupported_opcode;
    uint8_t first_native_unbound_opcode;
    uint8_t last_native_unbound_opcode;
    uint32_t first_native_unbound_source;
    uint32_t first_native_unsupported_source;
    uint32_t first_native_unbound_pc;
    uint32_t first_native_unbound_function;
    uint32_t first_native_unsupported_pc;
    uint32_t first_native_unsupported_function;
    uint32_t first_native_unbound_return_address;
    uint32_t first_native_unsupported_return_address;
    uint64_t native_opcode_counts[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_state_opcode_counts[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_unbound_opcode_counts[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_producer_bound_opcode_counts[
        GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_packet_derived_opcode_counts[
        GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_gte_bound_opcode_counts[
        GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_gte_zero_opcode_counts[
        GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_gte_partial_opcode_counts[
        GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_gte_nonprojective_opcode_counts[
        GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint64_t native_unsupported_opcode_counts[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unbound_source_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unbound_pc_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unsupported_pc_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unbound_return_address_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unsupported_return_address_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    GuestRenderNativeSourceHotspot
        native_unbound_source_hotspots[GUEST_RENDER_NATIVE_STREAM_HOTSPOT_CAPACITY];
    GuestRenderNativeGpuState last_native_state;
    uint64_t total_visual_states;
    uint64_t total_superseded;
    uint64_t stage_failure_count;
    uint64_t first_stage_failure_command_id;
    GpuRenderTransactionId first_stage_failure_visual_id;
    GuestRenderNativeStreamStatus first_stage_failure_status;
    uint64_t last_command_id;
    uint64_t last_unbound_reserve_command_id;
    size_t last_unbound_reserve_candidate_count;
    size_t last_unbound_reserve_active_count;
    size_t last_unbound_reserve_available_count;
    uint32_t last_unbound_reserve_miss_failure_mask;
    uint64_t first_unbound_reserve_command_id;
    size_t first_unbound_reserve_candidate_count;
    size_t first_unbound_reserve_active_count;
    size_t first_unbound_reserve_available_count;
    uint32_t first_unbound_reserve_miss_failure_mask;
    GuestRenderNativeStreamStatus last_status;
    GuestRenderNativeStreamStatus last_stage_status;
    GuestRenderNativeStreamStatus last_consume_status;
    bool enabled;
} GuestRenderNativeStreamSnapshot;

typedef void (*GuestRenderNativeStreamMaterialObserver)(
        uint64_t command_id, const GpuRenderMaterial *material);
typedef bool (*GuestRenderNativeStreamResolvedSemanticObserver)(
        uint64_t command_id, const GpuRenderSemantic *semantic);

typedef enum GuestRenderNativeStreamSourceKind {
    GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN = 0,
    GUEST_RENDER_NATIVE_STREAM_SOURCE_MMIO,
    GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK,
    GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
    GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BURST,
} GuestRenderNativeStreamSourceKind;

typedef struct GuestRenderNativeStreamCommandIdentity {
    uint64_t command_id;
    uint64_t container_id;
    GuestRenderNativeSourceWriter command_writer;
    GuestRenderNativeSourceWriter container_writer;
    GuestRenderNativeStreamSourceKind source_kind;
    uint8_t opcode;
    size_t word_count;
    bool command_writer_valid;
    bool container_writer_valid;
} GuestRenderNativeStreamCommandIdentity;

typedef struct GuestRenderNativeStreamReserveDiagnostic {
    uint64_t command_id;
    GpuRenderTransactionId last_visual_id;
    size_t candidate_count;
    size_t active_count;
    size_t available_count;
    uint32_t miss_failure_mask;
} GuestRenderNativeStreamReserveDiagnostic;

typedef struct GuestRenderNativeStreamMissContext {
    GpuRenderTransactionId visual_id;
    uint64_t command_id;
    uint64_t container_id;
    const GpuRenderSemantic *packet_semantic;
    GuestRenderNativeSourceWriter command_writer;
    GuestRenderNativeSourceWriter container_writer;
    GuestRenderNativeStreamSourceKind source_kind;
    uint8_t opcode;
    size_t word_count;
    bool command_writer_valid;
    bool container_writer_valid;
} GuestRenderNativeStreamMissContext;

typedef bool (*GuestRenderNativeStreamMissResolver)(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic);

void guest_render_native_stream_set_enabled(bool enabled);
void guest_render_native_stream_set_shared_packet_bindings(bool enabled);
bool guest_render_native_stream_shared_packet_bindings_enabled(void);
void guest_render_native_stream_set_material_observer(
        GuestRenderNativeStreamMaterialObserver observer);
void guest_render_native_stream_set_resolved_semantic_observer(
        GuestRenderNativeStreamResolvedSemanticObserver observer);
void guest_render_native_stream_set_miss_resolver(
        GuestRenderNativeStreamMissResolver resolver);
void guest_render_native_stream_set_source_writer_observer(
        GuestRenderNativeSourceWriterObserver observer);
bool guest_render_native_stream_source_writer(
        uint32_t source_word_address,
        GuestRenderNativeSourceWriter *out_writer);
bool guest_render_native_stream_enabled(void);
void guest_render_native_stream_clear(void);
bool guest_render_native_stream_has_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id);
bool guest_render_native_stream_match_exact(
        const GuestRenderNativeStreamCommandIdentity *identity,
        const GpuRenderSemantic *packet_semantic,
        GpuRenderTransactionId *out_visual_id);
GuestRenderNativeStreamStatus guest_render_native_stream_reserve_exact(
        uint64_t reservation_id,
        const GuestRenderNativeStreamCommandIdentity *identity,
        const GpuRenderSemantic *packet_semantic,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic);
GuestRenderNativeStreamStatus guest_render_native_stream_consume_reserved(
        uint64_t reservation_id,
        const GuestRenderNativeStreamCommandIdentity *identity,
        GpuRenderTransactionId visual_id,
        const GpuRenderSemantic *reserved_semantic,
        GpuRenderSemantic *out_semantic);
void guest_render_native_stream_release_reservation(uint64_t reservation_id);
void guest_render_native_stream_reserve_diagnostic(
        GuestRenderNativeStreamReserveDiagnostic *out_diagnostic);
bool guest_render_native_stream_resolve_active_miss(
        const GuestRenderNativeStreamCommandIdentity *identity,
        const GpuRenderSemantic *packet_semantic,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic);
GuestRenderNativeStreamStatus guest_render_native_stream_note_resolved_consumed(
        GpuRenderTransactionId visual_id, uint64_t command_id,
        const GpuRenderSemantic *semantic);
bool guest_render_native_stream_has_active_bindings(void);
bool guest_render_native_stream_resolve_miss(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic);
bool guest_render_native_stream_last_consumed(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        GpuRenderSemantic *out_semantic);
bool guest_render_native_stream_last_consumed_command(
        uint64_t exact_command_id, GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic);
void guest_render_native_stream_note_rasterized(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        const GpuRenderSemantic *semantic);
void guest_render_native_stream_note_parser_replay_command(uint8_t opcode);
void guest_render_native_stream_note_native_line_segment(void);
void guest_render_native_stream_note_ui_ot_adapter(void);
void guest_render_native_stream_note_guest_gp0_command(void);
void guest_render_native_stream_note_shared_vram_present(void);
void guest_render_native_stream_note_native_list(void);
void guest_render_native_stream_note_native_packet(uint8_t opcode, bool bound,
                                                   bool supported);
void guest_render_native_stream_note_native_packet_source(
    uint8_t opcode, bool bound, bool supported, uint32_t source_word_address);
void guest_render_native_stream_note_native_packet_attribution(
    uint8_t opcode, bool bound, bool supported, uint32_t source_word_address,
    uint32_t source_pc, uint32_t source_function,
    uint32_t source_return_address);
void guest_render_native_stream_note_native_draw_source(
    uint8_t opcode, bool producer_bound);
void guest_render_native_stream_note_gte_binding(
    uint8_t opcode, uint8_t matched_vertices, uint8_t projective_vertices,
    uint8_t expected_vertices, bool bound);
void guest_render_native_stream_note_native_state(
    const GuestRenderNativeGpuState *state);
void guest_render_native_stream_note_independent_vram_present(void);
void guest_render_native_stream_note_original_draw(uint8_t opcode);
void guest_render_native_stream_note_shared_fmv_present(uint32_t width,
                                                        uint32_t height,
                                                        bool depth24);
void guest_render_native_stream_note_independent_fmv_present(uint32_t width,
                                                             uint32_t height,
                                                             bool depth24);

GuestRenderNativeStreamStatus guest_render_native_stream_stage_exact(
    GpuRenderTransactionId visual_id,
    uint64_t exact_command_id,
    const GpuRenderSemantic *semantic);

GuestRenderNativeStreamStatus guest_render_native_stream_activate_visual(
    GpuRenderTransactionId visual_id);
void guest_render_native_stream_abandon_visual(
    GpuRenderTransactionId visual_id);
void guest_render_native_stream_suspend_visual(
    GpuRenderTransactionId visual_id);
bool guest_render_native_stream_has_staged_predecessor(
    GpuRenderTransactionId visual_id);

GuestRenderNativeStreamStatus guest_render_native_stream_consume_exact(
    GpuRenderTransactionId visual_id,
    uint64_t exact_command_id,
    GpuRenderSemantic *out_semantic);

GuestRenderNativeStreamStatus guest_render_native_stream_snapshot(
    GuestRenderNativeStreamSnapshot *out_snapshot);

#ifdef GUEST_RENDER_NATIVE_STREAM_TESTING
void guest_render_native_stream_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
