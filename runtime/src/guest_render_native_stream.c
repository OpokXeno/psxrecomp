#include "guest_render_native_stream.h"

#include <stdlib.h>
#include <string.h>

#ifdef GUEST_RENDER_NATIVE_STREAM_TESTING
static uint64_t native_stream_frame_count;
#else
extern uint64_t s_frame_count;
#define native_stream_frame_count s_frame_count
#endif

typedef struct GuestRenderNativeStreamEntry {
    GpuRenderTransactionId visual_id;
    uint64_t command_id;
    GuestRenderNativeSourceWriter command_writer;
    uint64_t reservation_id;
    size_t reservation_slot;
    GpuRenderSemantic semantic;
    bool command_writer_valid;
    bool active;
} GuestRenderNativeStreamEntry;

typedef struct GuestRenderNativeCommandGeneration {
    uint64_t command_id;
    GpuRenderTransactionId visual_id;
    uint64_t epoch;
} GuestRenderNativeCommandGeneration;

typedef struct GuestRenderNativeCommandIndex {
    uint64_t command_id;
    uint32_t entry_index;
    uint32_t count;
    uint8_t state;
} GuestRenderNativeCommandIndex;

#define GUEST_RENDER_NATIVE_CONSUMED_CAPACITY 256u
#define GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY 8192u

static struct {
    GuestRenderNativeStreamEntry entries[GUEST_RENDER_NATIVE_STREAM_CAPACITY];
    GuestRenderNativeStreamEntry consumed[GUEST_RENDER_NATIVE_CONSUMED_CAPACITY];
    GpuRenderTransactionId active_visuals[GUEST_RENDER_NATIVE_STREAM_CAPACITY];
    GpuRenderTransactionId active_representatives[
        GUEST_RENDER_NATIVE_STREAM_CAPACITY];
    size_t active_visual_count;
    size_t active_representative_count;
    bool active_representatives_dirty;
    size_t consumed_cursor;
    GpuRenderTransactionId last_visual_id;
    GpuRenderTransactionId last_consumed_visual_id;
    size_t count;
    uint64_t total_staged;
    uint64_t total_consumed;
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
    uint64_t native_unsupported_opcode_counts[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unbound_source_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unbound_pc_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unsupported_pc_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unbound_return_address_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    uint32_t native_unsupported_return_address_by_opcode[GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT];
    GuestRenderNativeSourceHotspot
        native_unbound_source_hotspots[GUEST_RENDER_NATIVE_STREAM_HOTSPOT_CAPACITY];
    size_t reservation_indices[GUEST_RENDER_NATIVE_STREAM_CAPACITY];
    size_t reservation_count;
    size_t reservation_consumed;
    uint64_t reservation_batch_id;
    GuestRenderNativeGpuState last_native_state;
    bool attribution_initialized;
    uint64_t total_visual_states;
    uint64_t total_superseded;
    uint64_t stage_failure_count;
    uint64_t first_stage_failure_command_id;
    GpuRenderTransactionId first_stage_failure_visual_id;
    GuestRenderNativeStreamStatus first_stage_failure_status;
    uint64_t last_command_id;
    GuestRenderNativeStreamStatus last_status;
    GuestRenderNativeStreamStatus last_stage_status;
    GuestRenderNativeStreamStatus last_consume_status;
    GuestRenderNativeStreamMaterialObserver material_observer;
    GuestRenderNativeStreamMissResolver miss_resolver;
    GuestRenderNativeSourceWriterObserver source_writer_observer;
    GuestRenderNativeStreamReserveDiagnostic reserve_diagnostic;
    GuestRenderNativeCommandGeneration *command_generations;
    size_t command_generation_count;
    size_t command_generation_capacity;
    uint64_t command_generation_epoch;
    GuestRenderNativeCommandIndex command_index[
        GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY];
    bool shared_packet_bindings_enabled;
    bool enabled;
} stream;

static void note_consumed(uint64_t command_id, GpuRenderTransactionId visual_id,
                          const GpuRenderSemantic *semantic) {
    GuestRenderNativeStreamEntry *entry =
        &stream.consumed[stream.consumed_cursor %
                         GUEST_RENDER_NATIVE_CONSUMED_CAPACITY];
    entry->command_id = command_id;
    entry->visual_id = visual_id;
    entry->semantic = *semantic;
    ++stream.consumed_cursor;
}

static GuestRenderNativeStreamStatus stage_result(
        GuestRenderNativeStreamStatus status) {
    stream.last_stage_status = status;
    return stream.last_status = status;
}

static GuestRenderNativeStreamStatus consume_result(
        GuestRenderNativeStreamStatus status) {
    stream.last_consume_status = status;
    return stream.last_status = status;
}

static void ensure_attribution_initialized(void) {
    size_t opcode;

    if (stream.attribution_initialized) return;
    for (opcode = 0u;
         opcode < GUEST_RENDER_NATIVE_STREAM_OPCODE_COUNT; ++opcode) {
        stream.native_unbound_source_by_opcode[opcode] = UINT32_MAX;
        stream.native_unbound_pc_by_opcode[opcode] = UINT32_MAX;
        stream.native_unsupported_pc_by_opcode[opcode] = UINT32_MAX;
        stream.native_unbound_return_address_by_opcode[opcode] = UINT32_MAX;
        stream.native_unsupported_return_address_by_opcode[opcode] = UINT32_MAX;
    }
    stream.attribution_initialized = true;
}

static void note_unbound_source_hotspot(
        uint8_t opcode, uint32_t source_word_address, uint32_t source_pc,
        uint32_t source_function, uint32_t source_return_address) {
    GuestRenderNativeSourceHotspot *free_entry = NULL;
    GuestRenderNativeSourceHotspot *minimum = NULL;
    GuestRenderNativeSourceWriter writer = {0};
    GuestRenderNativeSourceWriter next_word_writer = {0};
    GuestRenderNativeSourceWriter payload_writers[
        GUEST_RENDER_NATIVE_STREAM_PAYLOAD_WRITER_COUNT] = {{0}};
    const uint32_t source_region_start =
        source_word_address &
        ~(uint32_t)(GUEST_RENDER_NATIVE_STREAM_HOTSPOT_REGION_SIZE - 1u);
    size_t index;
    size_t writer_index;

    for (index = 0u; index < GUEST_RENDER_NATIVE_STREAM_HOTSPOT_CAPACITY;
         ++index) {
        GuestRenderNativeSourceHotspot *entry =
            &stream.native_unbound_source_hotspots[index];
        if (entry->count != 0u && entry->opcode == opcode &&
            entry->source_region_start == source_region_start) {
            ++entry->count;
            entry->last_frame = (uint32_t)native_stream_frame_count;
            return;
        }
        if (entry->count == 0u && free_entry == NULL) free_entry = entry;
        if (entry->count != 0u &&
            (minimum == NULL || entry->count < minimum->count))
            minimum = entry;
    }
    if (stream.source_writer_observer != NULL)
        (void)stream.source_writer_observer(source_word_address, &writer);
    if (stream.source_writer_observer != NULL &&
        source_word_address <= UINT32_MAX - 4u)
        (void)stream.source_writer_observer(
            source_word_address + 4u, &next_word_writer);
    if (stream.source_writer_observer != NULL) {
        for (writer_index = 0u;
             writer_index < GUEST_RENDER_NATIVE_STREAM_PAYLOAD_WRITER_COUNT;
             ++writer_index) {
            const uint64_t address = (uint64_t)source_word_address +
                writer_index * sizeof(uint32_t);
            if (address > UINT32_MAX) break;
            (void)stream.source_writer_observer(
                (uint32_t)address, &payload_writers[writer_index]);
        }
    }
    if (free_entry != NULL) {
        *free_entry = (GuestRenderNativeSourceHotspot){
            .count = 1u,
            .source_region_start = source_region_start,
            .representative_source_address = source_word_address,
            .representative_packet_pc = source_pc,
            .representative_packet_function = source_function,
            .representative_packet_return_address = source_return_address,
            .first_frame = (uint32_t)native_stream_frame_count,
            .last_frame = (uint32_t)native_stream_frame_count,
            .representative_writer_pc = writer.pc,
            .representative_writer_function = writer.function,
            .representative_writer_return_address = writer.return_address,
            .representative_next_word_writer_pc = next_word_writer.pc,
            .representative_next_word_writer_function = next_word_writer.function,
            .representative_next_word_writer_return_address =
                next_word_writer.return_address,
            .opcode = opcode,
        };
        memcpy(free_entry->representative_payload_writers, payload_writers,
               sizeof(payload_writers));
    } else if (minimum != NULL) {
        const uint64_t previous_count = minimum->count;
        *minimum = (GuestRenderNativeSourceHotspot){
            .count = previous_count + 1u,
            .error = previous_count,
            .source_region_start = source_region_start,
            .representative_source_address = source_word_address,
            .representative_packet_pc = source_pc,
            .representative_packet_function = source_function,
            .representative_packet_return_address = source_return_address,
            .first_frame = (uint32_t)native_stream_frame_count,
            .last_frame = (uint32_t)native_stream_frame_count,
            .representative_writer_pc = writer.pc,
            .representative_writer_function = writer.function,
            .representative_writer_return_address = writer.return_address,
            .representative_next_word_writer_pc = next_word_writer.pc,
            .representative_next_word_writer_function = next_word_writer.function,
            .representative_next_word_writer_return_address =
                next_word_writer.return_address,
            .opcode = opcode,
        };
        memcpy(minimum->representative_payload_writers, payload_writers,
               sizeof(payload_writers));
    }
}

static bool visual_ids_equal(GpuRenderTransactionId left,
                             GpuRenderTransactionId right);
static size_t remove_visual_entries(GpuRenderTransactionId visual_id);
static void deactivate_visual_id(GpuRenderTransactionId visual_id);
static GuestRenderNativeStreamStatus stage_failure(
        GuestRenderNativeStreamStatus status,
        GpuRenderTransactionId visual_id,
        uint64_t command_id) {
    if (stream.stage_failure_count == 0u) {
        stream.first_stage_failure_visual_id = visual_id;
        stream.first_stage_failure_command_id = command_id;
        stream.first_stage_failure_status = status;
    }
    ++stream.stage_failure_count;
    stream.total_superseded += remove_visual_entries(visual_id);
    deactivate_visual_id(visual_id);
    return stage_result(status);
}

static bool visual_ids_equal(GpuRenderTransactionId left,
                             GpuRenderTransactionId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

static void invalidate_active_representatives(void) {
    stream.active_representatives_dirty = true;
}

static void rebuild_active_representatives(void) {
    if (!stream.active_representatives_dirty) return;

    stream.active_representative_count = 0u;
    for (size_t index = 0u; index < stream.active_visual_count; ++index) {
        const GpuRenderTransactionId visual = stream.active_visuals[index];
        size_t representative = SIZE_MAX;

        for (size_t candidate = 0u;
             candidate < stream.active_representative_count; ++candidate) {
            if (stream.active_representatives[candidate].scene_epoch ==
                visual.scene_epoch) {
                representative = candidate;
                break;
            }
        }
        if (representative == SIZE_MAX) {
            stream.active_representatives[
                stream.active_representative_count++] = visual;
        } else if (visual.state_sequence >
                   stream.active_representatives[representative].state_sequence) {
            stream.active_representatives[representative] = visual;
        }
    }
    stream.active_representatives_dirty = false;
}

static bool visual_id_precedes(GpuRenderTransactionId left,
                               GpuRenderTransactionId right) {
    return left.scene_epoch < right.scene_epoch ||
           (left.scene_epoch == right.scene_epoch &&
           left.state_sequence < right.state_sequence);
}

static bool source_writers_equal(GuestRenderNativeSourceWriter left,
                                 GuestRenderNativeSourceWriter right) {
    return left.pc == right.pc && left.function == right.function &&
        left.return_address == right.return_address;
}

static void reservation_index_reset(void) {
    stream.reservation_count = 0u;
    stream.reservation_consumed = 0u;
    stream.reservation_batch_id = 0u;
}

static void reservation_index_begin(uint64_t reservation_id) {
    if (stream.reservation_batch_id == reservation_id) return;
    stream.reservation_count = 0u;
    stream.reservation_consumed = 0u;
    stream.reservation_batch_id = reservation_id;
}

static void reservation_index_update_after_move(size_t from, size_t to) {
    const size_t slot = to < stream.count
        ? stream.entries[to].reservation_slot : SIZE_MAX;

    if (from == to || to >= stream.count ||
        stream.entries[to].reservation_id == 0u ||
        slot >= stream.reservation_count ||
        stream.reservation_indices[slot] != from)
        return;
    stream.reservation_indices[slot] = to;
}

static bool visual_has_active_entries(GpuRenderTransactionId visual_id) {
    for (size_t index = 0u; index < stream.active_visual_count; ++index) {
        if (visual_ids_equal(stream.active_visuals[index], visual_id))
            return true;
    }
    return false;
}

static bool visual_has_stream_entries(GpuRenderTransactionId visual_id) {
    for (size_t index = 0u; index < stream.count; ++index) {
        if (visual_ids_equal(stream.entries[index].visual_id, visual_id))
            return true;
    }
    return false;
}

static bool activate_visual_id(GpuRenderTransactionId visual_id) {
    for (size_t index = 0u; index < stream.active_visual_count;) {
        const GpuRenderTransactionId previous = stream.active_visuals[index];
        if (previous.scene_epoch == visual_id.scene_epoch &&
            !visual_ids_equal(previous, visual_id) &&
            !visual_has_stream_entries(previous)) {
            deactivate_visual_id(previous);
            continue;
        }
        ++index;
    }
    for (size_t index = 0u; index < stream.active_visual_count; ++index) {
        if (visual_ids_equal(stream.active_visuals[index], visual_id))
            return true;
    }
    if (stream.active_visual_count == GUEST_RENDER_NATIVE_STREAM_CAPACITY)
        return false;
    stream.active_visuals[stream.active_visual_count++] = visual_id;
    invalidate_active_representatives();
    return true;
}

static void deactivate_visual_id(GpuRenderTransactionId visual_id) {
    for (size_t index = 0u; index < stream.active_visual_count; ++index) {
        if (!visual_ids_equal(stream.active_visuals[index], visual_id))
            continue;
        --stream.active_visual_count;
        if (index != stream.active_visual_count)
            stream.active_visuals[index] =
                stream.active_visuals[stream.active_visual_count];
        memset(&stream.active_visuals[stream.active_visual_count], 0,
               sizeof(stream.active_visuals[stream.active_visual_count]));
        invalidate_active_representatives();
        return;
    }
}

static bool visual_has_command(GpuRenderTransactionId visual_id,
                               uint64_t command_id) {
    for (size_t index = 0u; index < stream.count; ++index) {
        if (visual_ids_equal(stream.entries[index].visual_id, visual_id) &&
            stream.entries[index].command_id == command_id)
            return true;
    }
    return false;
}

static uint64_t command_generation_hash(uint64_t command_id) {
    uint64_t value = command_id;

    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31u);
}

static GuestRenderNativeCommandIndex *command_index_find(
        uint64_t command_id) {
    const size_t start = (size_t)(command_generation_hash(command_id) &
                                  (GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY -
                                   1u));

    for (size_t probe = 0u;
         probe < GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY; ++probe) {
        GuestRenderNativeCommandIndex *entry = &stream.command_index[
            (start + probe) & (GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY - 1u)];

        if (entry->state == 0u) return NULL;
        if (entry->state == 1u && entry->command_id == command_id)
            return entry;
    }
    return NULL;
}

static bool command_index_add(uint64_t command_id, size_t entry_index) {
    const size_t start = (size_t)(command_generation_hash(command_id) &
                                  (GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY -
                                   1u));
    GuestRenderNativeCommandIndex *tombstone = NULL;

    for (size_t probe = 0u;
         probe < GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY; ++probe) {
        GuestRenderNativeCommandIndex *entry = &stream.command_index[
            (start + probe) & (GUEST_RENDER_NATIVE_COMMAND_INDEX_CAPACITY - 1u)];

        if (entry->state == 1u && entry->command_id == command_id) {
            ++entry->count;
            return true;
        }
        if (entry->state == 2u && tombstone == NULL) tombstone = entry;
        if (entry->state != 0u) continue;
        if (tombstone != NULL) entry = tombstone;
        *entry = (GuestRenderNativeCommandIndex){
            .command_id = command_id,
            .entry_index = (uint32_t)entry_index,
            .count = 1u,
            .state = 1u,
        };
        return true;
    }
    if (tombstone != NULL) {
        *tombstone = (GuestRenderNativeCommandIndex){
            .command_id = command_id,
            .entry_index = (uint32_t)entry_index,
            .count = 1u,
            .state = 1u,
        };
        return true;
    }
    return false;
}

static void command_index_remove_entry(size_t removed_index) {
    const uint64_t command_id = stream.entries[removed_index].command_id;
    GuestRenderNativeCommandIndex *indexed = command_index_find(command_id);

    if (indexed == NULL) return;
    if (indexed->count > 1u) {
        --indexed->count;
        if (indexed->entry_index == removed_index) {
            for (size_t index = 0u; index < stream.count; ++index) {
                if (index != removed_index &&
                    stream.entries[index].command_id == command_id) {
                    indexed->entry_index = (uint32_t)index;
                    break;
                }
            }
        }
        return;
    }
    indexed->state = 2u;
    indexed->count = 0u;
}

static void command_index_update_move(size_t from, size_t to) {
    GuestRenderNativeCommandIndex *indexed;

    if (from == to || to >= stream.count) return;
    indexed = command_index_find(stream.entries[to].command_id);
    if (indexed != NULL && indexed->entry_index == from)
        indexed->entry_index = (uint32_t)to;
}

static void command_index_clear(void) {
    memset(stream.command_index, 0, sizeof(stream.command_index));
}

static GuestRenderNativeCommandGeneration *command_generation_find_in(
        GuestRenderNativeCommandGeneration *entries, size_t capacity,
        uint64_t command_id, uint64_t epoch) {
    const size_t start = (size_t)(command_generation_hash(command_id) &
                                  (capacity - 1u));

    for (size_t probe = 0u; probe < capacity; ++probe) {
        GuestRenderNativeCommandGeneration *entry =
            &entries[(start + probe) & (capacity - 1u)];
        if (entry->epoch != epoch) return NULL;
        if (entry->command_id == command_id) return entry;
    }
    return NULL;
}

static GuestRenderNativeCommandGeneration *command_generation_insert(
        uint64_t command_id) {
    const size_t start = (size_t)(command_generation_hash(command_id) &
                                  (stream.command_generation_capacity - 1u));

    for (size_t probe = 0u; probe < stream.command_generation_capacity; ++probe) {
        GuestRenderNativeCommandGeneration *entry =
            &stream.command_generations[
                (start + probe) & (stream.command_generation_capacity - 1u)];
        if (entry->epoch == stream.command_generation_epoch &&
            entry->command_id == command_id)
            return entry;
        if (entry->epoch != stream.command_generation_epoch) {
            entry->epoch = stream.command_generation_epoch;
            entry->command_id = command_id;
            ++stream.command_generation_count;
            return entry;
        }
    }
    return NULL;
}

static bool command_has_newer_generation(uint64_t command_id,
                                          GpuRenderTransactionId visual_id) {
    const GuestRenderNativeCommandGeneration *generation =
        stream.command_generations == NULL
            ? NULL
            : command_generation_find_in(stream.command_generations,
                                         stream.command_generation_capacity,
                                         command_id,
                                         stream.command_generation_epoch);
    return generation != NULL &&
        visual_id_precedes(visual_id, generation->visual_id);
}

static GuestRenderNativeCommandGeneration *command_generation(
        uint64_t command_id) {
    return stream.command_generations == NULL
        ? NULL
        : command_generation_find_in(stream.command_generations,
                                     stream.command_generation_capacity,
                                     command_id,
                                     stream.command_generation_epoch);
}

static bool reserve_command_generations(size_t required) {
    GuestRenderNativeCommandGeneration *entries;
    GuestRenderNativeCommandGeneration *old_entries =
        stream.command_generations;
    size_t old_capacity = stream.command_generation_capacity;
    size_t capacity;
    uint64_t epoch = stream.command_generation_epoch;

    /* Keep the open-addressed table below 75% occupancy. Capacity is always a
     * power of two so lookup stays bounded and does not regress to the old
     * O(number-of-ever-seen-packets) scan. */
    if (required <= stream.command_generation_capacity -
                       stream.command_generation_capacity / 4u)
        return true;
    capacity = stream.command_generation_capacity != 0u
        ? stream.command_generation_capacity : 64u;
    while (required > capacity - capacity / 4u) {
        if (capacity > SIZE_MAX / 2u) return false;
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*entries)) return false;
    if (epoch == 0u) epoch = 1u;
    entries = (GuestRenderNativeCommandGeneration *)malloc(
        capacity * sizeof(*entries));
    if (entries == NULL) return false;
    memset(entries, 0, capacity * sizeof(*entries));
    if (old_entries != NULL) {
        for (size_t index = 0u; index < old_capacity; ++index) {
            GuestRenderNativeCommandGeneration *old = &old_entries[index];
            GuestRenderNativeCommandGeneration *entry;

            if (old->epoch != stream.command_generation_epoch) continue;
            entry = command_generation_find_in(entries, capacity,
                                               old->command_id, epoch);
            if (entry != NULL) continue;
            {
                const size_t start = (size_t)(command_generation_hash(
                    old->command_id) & (capacity - 1u));
                for (size_t probe = 0u; probe < capacity; ++probe) {
                    entry = &entries[(start + probe) & (capacity - 1u)];
                    if (entry->epoch == epoch) continue;
                    *entry = *old;
                    entry->epoch = epoch;
                    break;
                }
            }
        }
        free(old_entries);
    }
    stream.command_generations = entries;
    stream.command_generation_capacity = capacity;
    stream.command_generation_epoch = epoch;
    return true;
}

static void remove_entry_at(size_t removed_index) {
    const size_t last_index = stream.count - 1u;

    if (removed_index >= stream.count) return;
    command_index_remove_entry(removed_index);
    if (removed_index != last_index) {
        stream.entries[removed_index] = stream.entries[last_index];
        reservation_index_update_after_move(last_index, removed_index);
        command_index_update_move(last_index, removed_index);
    }
    --stream.count;
    memset(&stream.entries[stream.count], 0,
           sizeof(stream.entries[stream.count]));
}

static size_t remove_visual_entries(GpuRenderTransactionId visual_id) {
    size_t index = 0u;
    size_t removed = 0u;

    while (index < stream.count) {
        if (!visual_ids_equal(stream.entries[index].visual_id, visual_id)) {
            ++index;
            continue;
        }
        ++removed;
        remove_entry_at(index);
    }
    return removed;
}

static bool semantic_is_valid(const GpuRenderSemantic *semantic) {
    const GpuRenderMaterial *material;
    uint16_t encoded_depth;

    if (!semantic) return false;
    if (semantic->screen_space_2d > 1u) return false;
    material = &semantic->material;
    if (material->texture_depth != GPU_RENDER_TEXTURE_4_BIT &&
        material->texture_depth != GPU_RENDER_TEXTURE_8_BIT &&
        material->texture_depth != GPU_RENDER_TEXTURE_15_BIT)
        return false;
    if (material->blend_mode != GPU_RENDER_BLEND_AVERAGE &&
        material->blend_mode != GPU_RENDER_BLEND_ADD &&
        material->blend_mode != GPU_RENDER_BLEND_SUBTRACT &&
        material->blend_mode != GPU_RENDER_BLEND_ADD_QUARTER)
        return false;
    if (material->shading != GPU_RENDER_SHADING_FLAT &&
        material->shading != GPU_RENDER_SHADING_GOURAUD)
        return false;
    encoded_depth = (uint16_t)((material->tpage >> 7u) & 3u);
    if (material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4u) & 1u) ||
        material->blend_mode !=
            (GpuRenderBlendMode)((material->tpage >> 5u) & 3u) ||
        material->texture_depth != (GpuRenderTextureDepth)encoded_depth ||
        material->clut_x > 1023u || (material->clut_x & 15u) != 0u ||
        material->clut_y > 511u ||
        material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom ||
        material->draw_area_right > 1023u ||
        material->draw_area_bottom > 511u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        material->textured > 1u || material->raw_texture > 1u ||
        material->semi_transparent > 1u || material->dither > 1u ||
        material->mask_set > 1u || material->mask_check > 1u ||
        (material->raw_texture && !material->textured))
        return false;

    if (semantic->topology == GPU_RENDER_SEMANTIC_LINES) {
        if (semantic->screen_space_2d || material->textured ||
            material->raw_texture ||
            semantic->triangle_count != 0u || semantic->line_count == 0u ||
            semantic->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY)
            return false;
        for (uint8_t line_index = 0u;
             line_index < semantic->line_count; ++line_index) {
            for (uint8_t vertex_index = 0u; vertex_index < 2u; ++vertex_index) {
                const GpuRenderSemanticVertex *vertex =
                    &semantic->lines[line_index].vertices[vertex_index];

                if (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                    ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                    return false;
                if (vertex->native_view_position > 1u ||
                    (!vertex->native_view_position &&
                     (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                    return false;
                if (material->shading == GPU_RENDER_SHADING_FLAT &&
                    vertex_index != 0u &&
                    (vertex->r != semantic->lines[line_index].vertices[0].r ||
                     vertex->g != semantic->lines[line_index].vertices[0].g ||
                     vertex->b != semantic->lines[line_index].vertices[0].b))
                    return false;
            }
        }
        return true;
    }
    if (semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->line_count != 0u || semantic->triangle_count == 0u ||
        semantic->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY)
        return false;

    for (uint8_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];

        if (triangle->split_index != triangle_index ||
            triangle->split_count != semantic->triangle_count)
            return false;
        for (uint8_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];

            if (((uint32_t)vertex->x & UINT32_C(0xffff)) != 0u ||
                ((uint32_t)vertex->y & UINT32_C(0xffff)) != 0u ||
                ((uint32_t)vertex->u & UINT32_C(0xffff)) != 0u ||
                ((uint32_t)vertex->v & UINT32_C(0xffff)) != 0u)
                return false;
            if (vertex->native_view_position > 1u ||
                (semantic->screen_space_2d && vertex->native_view_position) ||
                (!vertex->native_view_position &&
                 (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                return false;
            if (material->shading == GPU_RENDER_SHADING_FLAT &&
                vertex_index != 0u &&
                (vertex->r != triangle->vertices[0].r ||
                 vertex->g != triangle->vertices[0].g ||
                 vertex->b != triangle->vertices[0].b))
                return false;
        }
    }
    return true;
}

static void clear_entries(void) {
    stream.total_superseded += stream.count;
    for (size_t index = 0u; index < stream.count; ++index)
        memset(&stream.entries[index], 0, sizeof(stream.entries[index]));
    memset(&stream.last_visual_id, 0, sizeof(stream.last_visual_id));
    memset(&stream.last_consumed_visual_id, 0,
           sizeof(stream.last_consumed_visual_id));
    stream.count = 0u;
    stream.active_visual_count = 0u;
    stream.active_representative_count = 0u;
    stream.active_representatives_dirty = true;
    stream.consumed_cursor = 0u;
    reservation_index_reset();
    command_index_clear();
    if (stream.command_generations != NULL) {
        ++stream.command_generation_epoch;
        if (stream.command_generation_epoch == 0u) {
            memset(stream.command_generations, 0,
                   stream.command_generation_capacity *
                       sizeof(*stream.command_generations));
            stream.command_generation_epoch = 1u;
        }
    }
    stream.command_generation_count = 0u;
}

void guest_render_native_stream_set_enabled(bool enabled) {
    if (stream.enabled == enabled) return;
    clear_entries();
    stream.enabled = enabled;
    stream.last_status = enabled ? GUEST_RENDER_NATIVE_STREAM_OK
                                 : GUEST_RENDER_NATIVE_STREAM_DISABLED;
}

void guest_render_native_stream_set_shared_packet_bindings(bool enabled) {
    stream.shared_packet_bindings_enabled = enabled;
}

bool guest_render_native_stream_shared_packet_bindings_enabled(void) {
    return stream.shared_packet_bindings_enabled;
}

void guest_render_native_stream_set_material_observer(
        GuestRenderNativeStreamMaterialObserver observer) {
    stream.material_observer = observer;
}

void guest_render_native_stream_set_miss_resolver(
        GuestRenderNativeStreamMissResolver resolver) {
    stream.miss_resolver = resolver;
}

void guest_render_native_stream_set_source_writer_observer(
        GuestRenderNativeSourceWriterObserver observer) {
    stream.source_writer_observer = observer;
}

bool guest_render_native_stream_source_writer(
        uint32_t source_word_address,
        GuestRenderNativeSourceWriter *out_writer) {
    return stream.enabled && out_writer != NULL &&
        stream.source_writer_observer != NULL &&
        stream.source_writer_observer(source_word_address, out_writer);
}

bool guest_render_native_stream_enabled(void) { return stream.enabled; }

void guest_render_native_stream_clear(void) {
    clear_entries();
    stream.last_status = stream.enabled ? GUEST_RENDER_NATIVE_STREAM_OK
                                         : GUEST_RENDER_NATIVE_STREAM_DISABLED;
}

bool guest_render_native_stream_has_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id) {
    if (!stream.enabled || exact_command_id == UINT64_MAX)
        return false;
    for (size_t index = 0u; index < stream.count; ++index) {
        if (stream.entries[index].active &&
            visual_ids_equal(stream.entries[index].visual_id, visual_id) &&
            stream.entries[index].command_id == exact_command_id)
            return true;
    }
    return false;
}

bool guest_render_native_stream_match_exact(
        const GuestRenderNativeStreamCommandIdentity *identity,
        GpuRenderTransactionId *out_visual_id) {
    const GuestRenderNativeStreamEntry *matched = NULL;

    if (!stream.enabled || identity == NULL || out_visual_id == NULL ||
        identity->command_id == UINT64_MAX ||
        identity->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN ||
        identity->word_count == 0u)
        return false;
    for (size_t index = 0u; index < stream.count; ++index) {
        const GuestRenderNativeStreamEntry *entry = &stream.entries[index];

        if (!entry->active || entry->reservation_id != 0u ||
            entry->command_id != identity->command_id)
            continue;
        if (entry->command_writer_valid &&
            (!identity->command_writer_valid ||
             !source_writers_equal(entry->command_writer,
                                   identity->command_writer)))
            continue;
        if (matched != NULL) return false;
        matched = entry;
    }
    if (matched == NULL) return false;
    *out_visual_id = matched->visual_id;
    return true;
}

GuestRenderNativeStreamStatus guest_render_native_stream_reserve_exact(
        uint64_t reservation_id,
        const GuestRenderNativeStreamCommandIdentity *identity,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    GuestRenderNativeStreamEntry *matched = NULL;

    if (!stream.enabled)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_DISABLED);
    if (reservation_id == 0u || identity == NULL || out_visual_id == NULL ||
        out_semantic == NULL || identity->command_id == UINT64_MAX ||
        identity->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN ||
        identity->word_count == 0u)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    reservation_index_begin(reservation_id);
    memset(&stream.reserve_diagnostic, 0, sizeof(stream.reserve_diagnostic));
    stream.reserve_diagnostic.command_id = identity->command_id;
    {
        GuestRenderNativeCommandIndex *indexed = command_index_find(
            identity->command_id);
        const bool unique_indexed = indexed != NULL && indexed->count == 1u &&
            indexed->entry_index < stream.count &&
            stream.entries[indexed->entry_index].command_id ==
                identity->command_id;

        if (unique_indexed) {
            GuestRenderNativeStreamEntry *entry =
                &stream.entries[indexed->entry_index];

            stream.reserve_diagnostic.candidate_count = 1u;
            stream.reserve_diagnostic.last_visual_id = entry->visual_id;
            if (entry->active) ++stream.reserve_diagnostic.active_count;
            if (entry->active && entry->reservation_id == 0u) {
                stream.reserve_diagnostic.available_count = 1u;
                matched = entry;
            }
        } else {
            for (size_t index = 0u; index < stream.count; ++index) {
                GuestRenderNativeStreamEntry *entry = &stream.entries[index];

                if (entry->command_id != identity->command_id) continue;
                ++stream.reserve_diagnostic.candidate_count;
                stream.reserve_diagnostic.last_visual_id = entry->visual_id;
                if (entry->active) ++stream.reserve_diagnostic.active_count;
                if (!entry->active || entry->reservation_id != 0u) continue;
                ++stream.reserve_diagnostic.available_count;
                if (matched != NULL)
                    return consume_result(
                        GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
                matched = entry;
            }
        }
    }
    if (matched == NULL)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
    if (stream.reservation_count == GUEST_RENDER_NATIVE_STREAM_CAPACITY)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
    matched->command_writer = identity->command_writer;
    matched->command_writer_valid = identity->command_writer_valid;
    matched->reservation_id = reservation_id;
    matched->reservation_slot = stream.reservation_count;
    stream.reservation_indices[stream.reservation_count++] =
        (size_t)(matched - stream.entries);
    *out_visual_id = matched->visual_id;
    *out_semantic = matched->semantic;
    return consume_result(GUEST_RENDER_NATIVE_STREAM_OK);
}

GuestRenderNativeStreamStatus guest_render_native_stream_consume_reserved(
        uint64_t reservation_id,
        const GuestRenderNativeStreamCommandIdentity *identity,
        GpuRenderTransactionId visual_id,
         const GpuRenderSemantic *reserved_semantic,
         GpuRenderSemantic *out_semantic) {
    size_t matched = SIZE_MAX;
    size_t reservation_slot = SIZE_MAX;

    if (!stream.enabled)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_DISABLED);
    if (reservation_id == 0u || identity == NULL ||
        reserved_semantic == NULL || out_semantic == NULL)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    if (stream.reservation_batch_id == reservation_id &&
        stream.reservation_consumed < stream.reservation_count) {
        const size_t candidate = stream.reservation_indices[
            stream.reservation_consumed];
        if (candidate < stream.count &&
            stream.entries[candidate].active &&
            stream.entries[candidate].reservation_id == reservation_id &&
            stream.entries[candidate].reservation_slot ==
                stream.reservation_consumed &&
            stream.entries[candidate].command_id == identity->command_id &&
            visual_ids_equal(stream.entries[candidate].visual_id, visual_id)) {
            matched = candidate;
            reservation_slot = stream.reservation_consumed;
        }
    }
    /* Preserve the old search as a diagnostic path for an out-of-order
     * consumer; the DMA2 authoritative path always consumes in reservation
     * order and therefore uses the indexed lookup above. */
    if (matched == SIZE_MAX) {
        for (size_t index = 0u; index < stream.count; ++index) {
            const GuestRenderNativeStreamEntry *entry = &stream.entries[index];
            if (entry->active && entry->reservation_id == reservation_id &&
                entry->command_id == identity->command_id &&
                visual_ids_equal(entry->visual_id, visual_id)) {
                matched = index;
                reservation_slot = entry->reservation_slot;
                break;
            }
        }
    }
    if (matched == SIZE_MAX) {
        ++stream.total_not_found;
        return consume_result(GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
    }
    if (memcmp(&stream.entries[matched].semantic, reserved_semantic,
               sizeof(*reserved_semantic)) != 0)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    *out_semantic = stream.entries[matched].semantic;
    note_consumed(identity->command_id, visual_id, out_semantic);
    if (stream.material_observer != NULL)
        stream.material_observer(identity->command_id, &out_semantic->material);
    if (!visual_ids_equal(stream.last_consumed_visual_id, visual_id)) {
        stream.last_consumed_visual_id = visual_id;
        ++stream.total_visual_states;
    }
    ++stream.total_consumed;
    stream.last_command_id = identity->command_id;
    remove_entry_at(matched);
    if (stream.reservation_batch_id == reservation_id &&
        reservation_slot == stream.reservation_consumed) {
        ++stream.reservation_consumed;
        if (stream.reservation_consumed == stream.reservation_count)
            reservation_index_reset();
    }
    return consume_result(GUEST_RENDER_NATIVE_STREAM_OK);
}

void guest_render_native_stream_release_reservation(uint64_t reservation_id) {
    if (reservation_id == 0u) return;
    for (size_t index = 0u; index < stream.count; ++index) {
        if (stream.entries[index].reservation_id == reservation_id) {
            stream.entries[index].reservation_id = 0u;
            stream.entries[index].reservation_slot = SIZE_MAX;
        }
    }
    if (stream.reservation_batch_id == reservation_id)
        reservation_index_reset();
}

void guest_render_native_stream_reserve_diagnostic(
        GuestRenderNativeStreamReserveDiagnostic *out_diagnostic) {
    if (out_diagnostic != NULL)
        *out_diagnostic = stream.reserve_diagnostic;
}

static bool resolve_miss_internal(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic, bool require_active_visual) {
    return stream.enabled && context != NULL && out_semantic != NULL &&
        (!require_active_visual || visual_has_active_entries(context->visual_id)) &&
        context->command_id != UINT64_MAX &&
        context->source_kind != GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN &&
        context->word_count != 0u && stream.miss_resolver != NULL &&
        stream.miss_resolver(context, &(GpuRenderTransactionId){0},
                             out_semantic) &&
        semantic_is_valid(out_semantic);
}

bool guest_render_native_stream_resolve_active_miss(
        const GuestRenderNativeStreamCommandIdentity *identity,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    GpuRenderTransactionId matched_visual = {0};
    GpuRenderSemantic matched_semantic;
    bool matched = false;

    if (!stream.enabled || identity == NULL || out_visual_id == NULL ||
        out_semantic == NULL || identity->command_id == UINT64_MAX)
        return false;
    rebuild_active_representatives();
    stream.reserve_diagnostic.active_count = stream.active_visual_count;
    stream.reserve_diagnostic.available_count = 0u;
    for (size_t index = 0u;
         index < stream.active_representative_count; ++index) {
        const GpuRenderTransactionId visual =
            stream.active_representatives[index];
        GuestRenderNativeStreamMissContext context;
        GpuRenderSemantic semantic;
        context = (GuestRenderNativeStreamMissContext){
            .visual_id = visual,
            .command_id = identity->command_id,
            .container_id = identity->container_id,
            .command_writer = identity->command_writer,
            .container_writer = identity->container_writer,
            .source_kind = identity->source_kind,
            .opcode = identity->opcode,
            .word_count = identity->word_count,
            .command_writer_valid = identity->command_writer_valid,
            .container_writer_valid = identity->container_writer_valid,
        };
        stream.reserve_diagnostic.last_visual_id = visual;
        if (!resolve_miss_internal(&context, &semantic, false))
            continue;
        ++stream.reserve_diagnostic.available_count;
        if (matched) return false;
        matched = true;
        matched_visual = visual;
        matched_semantic = semantic;
    }
    if (stream.active_visual_count == 0u) {
        GuestRenderNativeStreamMissContext context = {
            .command_id = identity->command_id,
            .container_id = identity->container_id,
            .command_writer = identity->command_writer,
            .container_writer = identity->container_writer,
            .source_kind = identity->source_kind,
            .opcode = identity->opcode,
            .word_count = identity->word_count,
            .command_writer_valid = identity->command_writer_valid,
            .container_writer_valid = identity->container_writer_valid,
        };
        GpuRenderTransactionId resolved_visual = {0};

        if (stream.miss_resolver == NULL)
            return false;
        if (!stream.miss_resolver(
                &context, &resolved_visual, &matched_semantic)) {
            stream.reserve_diagnostic.last_visual_id = resolved_visual;
            return false;
        }
        if (
            resolved_visual.scene_epoch == 0u ||
            !semantic_is_valid(&matched_semantic))
            return false;
        stream.reserve_diagnostic.last_visual_id = resolved_visual;
        stream.reserve_diagnostic.available_count = 1u;
        *out_visual_id = resolved_visual;
        *out_semantic = matched_semantic;
        return true;
    }
    if (!matched) return false;
    *out_visual_id = matched_visual;
    *out_semantic = matched_semantic;
    return true;
}

GuestRenderNativeStreamStatus guest_render_native_stream_note_resolved_consumed(
        GpuRenderTransactionId visual_id, uint64_t command_id,
        const GpuRenderSemantic *semantic) {
    GuestRenderNativeCommandGeneration *latest;

    if (!stream.enabled)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_DISABLED);
    if (visual_id.scene_epoch == 0u || command_id == UINT64_MAX ||
        !semantic_is_valid(semantic))
        return consume_result(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    if (command_has_newer_generation(command_id, visual_id))
        return consume_result(GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID);
    latest = command_generation(command_id);
    if (latest == NULL) {
        if (!reserve_command_generations(stream.command_generation_count + 1u))
            return consume_result(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
        latest = command_generation_insert(command_id);
        if (latest == NULL)
            return consume_result(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
    }
    latest->visual_id = visual_id;
    note_consumed(command_id, visual_id, semantic);
    if (stream.material_observer != NULL)
        stream.material_observer(command_id, &semantic->material);
    if (!visual_ids_equal(stream.last_consumed_visual_id, visual_id)) {
        stream.last_consumed_visual_id = visual_id;
        ++stream.total_visual_states;
    }
    stream.last_visual_id = visual_id;
    stream.last_command_id = command_id;
    ++stream.total_staged;
    ++stream.total_consumed;
    return consume_result(GUEST_RENDER_NATIVE_STREAM_OK);
}

bool guest_render_native_stream_has_active_bindings(void) {
    return stream.enabled && stream.active_visual_count != 0u;
}

bool guest_render_native_stream_resolve_miss(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    return resolve_miss_internal(context, out_semantic, true);
}

bool guest_render_native_stream_last_consumed(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        GpuRenderSemantic *out_semantic) {
    const size_t available = stream.consumed_cursor <
            GUEST_RENDER_NATIVE_CONSUMED_CAPACITY
        ? stream.consumed_cursor : GUEST_RENDER_NATIVE_CONSUMED_CAPACITY;

    if (!out_semantic) return false;
    for (size_t age = 0u; age < available; ++age) {
        const size_t sequence = stream.consumed_cursor - age - 1u;
        const GuestRenderNativeStreamEntry *entry =
            &stream.consumed[sequence % GUEST_RENDER_NATIVE_CONSUMED_CAPACITY];
        if (!visual_ids_equal(entry->visual_id, visual_id) ||
            entry->command_id != exact_command_id)
            continue;
        *out_semantic = entry->semantic;
        return true;
    }
    return false;
}

bool guest_render_native_stream_last_consumed_command(
        uint64_t exact_command_id, GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    const size_t available = stream.consumed_cursor <
            GUEST_RENDER_NATIVE_CONSUMED_CAPACITY
        ? stream.consumed_cursor : GUEST_RENDER_NATIVE_CONSUMED_CAPACITY;

    if (out_visual_id == NULL || out_semantic == NULL ||
        exact_command_id == UINT64_MAX)
        return false;
    for (size_t age = 0u; age < available; ++age) {
        const size_t sequence = stream.consumed_cursor - age - 1u;
        const GuestRenderNativeStreamEntry *entry =
            &stream.consumed[sequence % GUEST_RENDER_NATIVE_CONSUMED_CAPACITY];
        if (entry->command_id != exact_command_id) continue;
        *out_visual_id = entry->visual_id;
        *out_semantic = entry->semantic;
        return true;
    }
    return false;
}

void guest_render_native_stream_note_rasterized(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        const GpuRenderSemantic *semantic) {
    const size_t available = stream.consumed_cursor <
            GUEST_RENDER_NATIVE_CONSUMED_CAPACITY
        ? stream.consumed_cursor : GUEST_RENDER_NATIVE_CONSUMED_CAPACITY;

    if (!semantic) return;
    for (size_t age = 0u; age < available; ++age) {
        const size_t sequence = stream.consumed_cursor - age - 1u;
        GuestRenderNativeStreamEntry *entry =
            &stream.consumed[sequence % GUEST_RENDER_NATIVE_CONSUMED_CAPACITY];
        if (!visual_ids_equal(entry->visual_id, visual_id) ||
            entry->command_id != exact_command_id)
            continue;
        entry->semantic = *semantic;
        return;
    }
}

void guest_render_native_stream_note_parser_replay_command(uint8_t opcode) {
    if (!stream.enabled) return;
    ++stream.total_parser_replay_commands;
    if (opcode >= 0x20u && opcode <= 0x7fu)
        ++stream.total_parser_replay_draws;
}

void guest_render_native_stream_note_native_line_segment(void) {
    if (!stream.enabled) return;
    ++stream.total_native_line_segments;
}

void guest_render_native_stream_note_ui_ot_adapter(void) {
    if (!stream.enabled) return;
    ++stream.total_ui_ot_adapter_calls;
}

void guest_render_native_stream_note_guest_gp0_command(void) {
    if (!stream.enabled) return;
    ++stream.total_guest_gp0_commands;
}

void guest_render_native_stream_note_shared_vram_present(void) {
    if (!stream.enabled) return;
    ++stream.total_shared_vram_presents;
}

void guest_render_native_stream_note_native_list(void) {
    if (!stream.enabled) return;
    ++stream.total_native_lists;
}

void guest_render_native_stream_note_native_packet_attribution(
        uint8_t opcode, bool bound, bool supported,
        uint32_t source_word_address, uint32_t source_pc,
        uint32_t source_function, uint32_t source_return_address) {
    if (!stream.enabled) return;
    ensure_attribution_initialized();
    ++stream.total_native_packets;
    ++stream.native_opcode_counts[opcode];
    if (bound) ++stream.total_native_bound_packets;
    else {
        note_unbound_source_hotspot(opcode, source_word_address, source_pc,
                                    source_function, source_return_address);
        if (stream.native_unbound_source_by_opcode[opcode] == UINT32_MAX)
            stream.native_unbound_source_by_opcode[opcode] = source_word_address;
        if (stream.native_unbound_pc_by_opcode[opcode] == UINT32_MAX)
            stream.native_unbound_pc_by_opcode[opcode] = source_pc;
        if (stream.native_unbound_return_address_by_opcode[opcode] == UINT32_MAX)
            stream.native_unbound_return_address_by_opcode[opcode] =
                source_return_address;
        if (stream.total_native_unbound_packets == 0u) {
            stream.first_native_unbound_opcode = opcode;
            stream.first_native_unbound_source = source_word_address;
            stream.first_native_unbound_pc = source_pc;
            stream.first_native_unbound_function = source_function;
            stream.first_native_unbound_return_address = source_return_address;
        }
        stream.last_native_unbound_opcode = opcode;
        ++stream.total_native_unbound_packets;
        ++stream.native_unbound_opcode_counts[opcode];
    }
    if (!supported) {
        if (stream.native_unsupported_pc_by_opcode[opcode] == UINT32_MAX)
            stream.native_unsupported_pc_by_opcode[opcode] = source_pc;
        if (stream.native_unsupported_return_address_by_opcode[opcode] == UINT32_MAX)
            stream.native_unsupported_return_address_by_opcode[opcode] =
                source_return_address;
        if (stream.total_native_unsupported_packets == 0u) {
            stream.first_native_unsupported_opcode = opcode;
            stream.first_native_unsupported_source = source_word_address;
            stream.first_native_unsupported_pc = source_pc;
            stream.first_native_unsupported_function = source_function;
            stream.first_native_unsupported_return_address =
                source_return_address;
        }
        stream.last_native_unsupported_opcode = opcode;
        ++stream.total_native_unsupported_packets;
        ++stream.native_unsupported_opcode_counts[opcode];
    }
}

void guest_render_native_stream_note_native_state(
        const GuestRenderNativeGpuState *state) {
    uint8_t opcode;

    if (!stream.enabled || state == NULL) return;
    opcode = (uint8_t)(state->command_word >> 24u);
    if (opcode < 0xe1u || opcode > 0xe6u) return;
    ++stream.total_native_packets;
    ++stream.total_native_state_packets;
    ++stream.native_opcode_counts[opcode];
    ++stream.native_state_opcode_counts[opcode];
    stream.last_native_state = *state;
    stream.last_native_state.sequence = stream.total_native_state_packets;
}

void guest_render_native_stream_note_native_packet_source(
        uint8_t opcode, bool bound, bool supported,
        uint32_t source_word_address) {
    guest_render_native_stream_note_native_packet_attribution(
        opcode, bound, supported, source_word_address, UINT32_MAX, UINT32_MAX,
        UINT32_MAX);
}

void guest_render_native_stream_note_native_packet(uint8_t opcode, bool bound,
                                                   bool supported) {
    guest_render_native_stream_note_native_packet_source(
        opcode, bound, supported, UINT32_MAX);
}

void guest_render_native_stream_note_independent_vram_present(void) {
    if (!stream.enabled) return;
    ++stream.total_independent_vram_presents;
}

static void note_fmv_present(uint64_t *frame_count, uint64_t *pixel_count,
                             uint32_t *last_width, uint32_t *last_height,
                             bool *last_depth24, uint32_t width,
                             uint32_t height, bool depth24) {
    if (width == 0u || height == 0u) return;
    ++*frame_count;
    *pixel_count += (uint64_t)width * (uint64_t)height;
    *last_width = width;
    *last_height = height;
    *last_depth24 = depth24;
}

void guest_render_native_stream_note_shared_fmv_present(uint32_t width,
                                                        uint32_t height,
                                                        bool depth24) {
    if (!stream.enabled) return;
    note_fmv_present(&stream.total_shared_fmv_frames,
                     &stream.total_shared_fmv_pixels,
                     &stream.last_shared_fmv_width,
                     &stream.last_shared_fmv_height,
                     &stream.last_shared_fmv_depth24,
                     width, height, depth24);
}

void guest_render_native_stream_note_independent_fmv_present(uint32_t width,
                                                             uint32_t height,
                                                             bool depth24) {
    if (!stream.enabled) return;
    note_fmv_present(&stream.total_independent_fmv_frames,
                     &stream.total_independent_fmv_pixels,
                     &stream.last_independent_fmv_width,
                     &stream.last_independent_fmv_height,
                     &stream.last_independent_fmv_depth24,
                     width, height, depth24);
}

void guest_render_native_stream_note_original_draw(uint8_t opcode) {
    if (!stream.enabled) return;
    if (stream.total_original_draws == 0u)
        stream.first_original_draw_opcode = opcode;
    stream.last_original_draw_opcode = opcode;
    ++stream.total_original_draws;
}

GuestRenderNativeStreamStatus guest_render_native_stream_stage_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        const GpuRenderSemantic *semantic) {
    GuestRenderNativeStreamEntry *entry;

    if (!stream.enabled)
        return stage_failure(GUEST_RENDER_NATIVE_STREAM_DISABLED,
                             visual_id, exact_command_id);
    if (visual_id.scene_epoch == 0u || exact_command_id == UINT64_MAX ||
        !semantic_is_valid(semantic))
        return stage_failure(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT,
                             visual_id, exact_command_id);
    if (command_has_newer_generation(exact_command_id, visual_id))
        return stage_failure(GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID,
                             visual_id, exact_command_id);
    for (size_t index = 0u; index < stream.count; ++index) {
        if (stream.entries[index].command_id != exact_command_id) continue;
        if (visual_ids_equal(visual_id, stream.entries[index].visual_id)) {
            stream.entries[index].semantic = *semantic;
            stream.last_visual_id = visual_id;
            ++stream.total_staged;
            ++stream.total_superseded;
            stream.last_command_id = exact_command_id;
            return stage_result(GUEST_RENDER_NATIVE_STREAM_OK);
        }
        if (visual_id_precedes(visual_id, stream.entries[index].visual_id))
            return stage_failure(GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID,
                                 visual_id, exact_command_id);
    }
    if (stream.count == GUEST_RENDER_NATIVE_STREAM_CAPACITY)
        return stage_failure(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED,
                             visual_id, exact_command_id);
    {
        const size_t entry_index = stream.count;

        entry = &stream.entries[stream.count++];
        if (!command_index_add(exact_command_id, entry_index)) {
            --stream.count;
            memset(entry, 0, sizeof(*entry));
            return stage_failure(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED,
                                 visual_id, exact_command_id);
        }
    }
    memset(entry, 0, sizeof(*entry));
    entry->reservation_slot = SIZE_MAX;
    entry->visual_id = visual_id;
    entry->command_id = exact_command_id;
    entry->semantic = *semantic;
    stream.last_visual_id = visual_id;
    ++stream.total_staged;
    stream.last_command_id = exact_command_id;
    return stage_result(GUEST_RENDER_NATIVE_STREAM_OK);
}

GuestRenderNativeStreamStatus guest_render_native_stream_activate_visual(
        GpuRenderTransactionId visual_id) {
    size_t missing_generations = 0u;
    bool found = false;

    if (!stream.enabled)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_DISABLED);
    if (visual_id.scene_epoch == 0u)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    for (size_t index = 0u; index < stream.count; ++index) {
        if (!visual_ids_equal(stream.entries[index].visual_id, visual_id))
            continue;
        found = true;
        if (command_has_newer_generation(stream.entries[index].command_id,
                                         visual_id))
            return consume_result(GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID);
        if (command_generation(stream.entries[index].command_id) == NULL)
            ++missing_generations;
    }
    if (!found) return consume_result(GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
    if (!reserve_command_generations(stream.command_generation_count +
                                     missing_generations))
        return consume_result(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
    if (!activate_visual_id(visual_id))
        return consume_result(GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
    for (size_t index = 0u; index < stream.count; ++index) {
        GuestRenderNativeStreamEntry *entry = &stream.entries[index];

        if (!visual_ids_equal(entry->visual_id, visual_id)) continue;
        memset(&entry->command_writer, 0, sizeof(entry->command_writer));
        entry->command_writer_valid = false;
        entry->active = true;
        {
            GuestRenderNativeCommandGeneration *latest =
                command_generation(entry->command_id);
            if (latest == NULL) {
                latest = command_generation_insert(entry->command_id);
                if (latest == NULL)
                    return consume_result(
                        GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED);
            }
            latest->visual_id = visual_id;
        }
    }
    for (size_t index = 0u; index < stream.count;) {
        GuestRenderNativeStreamEntry *entry = &stream.entries[index];

        if (visual_ids_equal(entry->visual_id, visual_id) ||
            !visual_id_precedes(entry->visual_id, visual_id) ||
            !visual_has_command(visual_id, entry->command_id)) {
            ++index;
            continue;
        }
        ++stream.total_superseded;
        remove_entry_at(index);
    }
    return consume_result(GUEST_RENDER_NATIVE_STREAM_OK);
}

void guest_render_native_stream_abandon_visual(
        GpuRenderTransactionId visual_id) {
    stream.total_superseded += remove_visual_entries(visual_id);
    deactivate_visual_id(visual_id);
}

void guest_render_native_stream_suspend_visual(
        GpuRenderTransactionId visual_id) {
    for (size_t index = 0u; index < stream.count; ++index) {
        if (visual_ids_equal(stream.entries[index].visual_id, visual_id))
            stream.entries[index].active = false;
    }
    deactivate_visual_id(visual_id);
}

bool guest_render_native_stream_has_staged_predecessor(
        GpuRenderTransactionId visual_id) {
    if (!stream.enabled || visual_id.scene_epoch == 0u) return false;
    for (size_t index = 0u; index < stream.count; ++index) {
        if (stream.entries[index].visual_id.scene_epoch ==
                visual_id.scene_epoch &&
            stream.entries[index].visual_id.state_sequence <=
                visual_id.state_sequence)
            return true;
    }
    return false;
}

GuestRenderNativeStreamStatus guest_render_native_stream_consume_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        GpuRenderSemantic *out_semantic) {
    size_t matched = SIZE_MAX;

    if (!stream.enabled)
        return consume_result(GUEST_RENDER_NATIVE_STREAM_DISABLED);
    if (!out_semantic || exact_command_id == UINT64_MAX ||
        !visual_has_active_entries(visual_id))
        return consume_result(GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT);
    for (size_t index = 0u; index < stream.count; ++index) {
        if (!stream.entries[index].active ||
            stream.entries[index].reservation_id != 0u ||
            !visual_ids_equal(stream.entries[index].visual_id, visual_id) ||
            stream.entries[index].command_id != exact_command_id)
            continue;
        matched = index;
        break;
    }
    if (matched != SIZE_MAX) {
        *out_semantic = stream.entries[matched].semantic;
        note_consumed(exact_command_id, visual_id, out_semantic);
        if (stream.material_observer != NULL)
            stream.material_observer(exact_command_id,
                                     &out_semantic->material);
        if (!visual_ids_equal(stream.last_consumed_visual_id,
                               visual_id)) {
            stream.last_consumed_visual_id = visual_id;
            ++stream.total_visual_states;
        }
        ++stream.total_consumed;
        stream.last_command_id = exact_command_id;
        remove_entry_at(matched);
        return consume_result(GUEST_RENDER_NATIVE_STREAM_OK);
    }
    ++stream.total_not_found;
    return consume_result(GUEST_RENDER_NATIVE_STREAM_NOT_FOUND);
}

GuestRenderNativeStreamStatus guest_render_native_stream_snapshot(
        GuestRenderNativeStreamSnapshot *out_snapshot) {
    if (!out_snapshot) return GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT;
    if (stream.enabled) ensure_attribution_initialized();
    out_snapshot->visual_id = stream.last_visual_id;
    out_snapshot->staged_count = stream.count;
    out_snapshot->command_generation_count = stream.command_generation_count;
    out_snapshot->total_staged = stream.total_staged;
    out_snapshot->total_consumed = stream.total_consumed;
    out_snapshot->total_not_found = stream.total_not_found;
    out_snapshot->total_original_draws = stream.total_original_draws;
    out_snapshot->first_original_draw_opcode =
        stream.first_original_draw_opcode;
    out_snapshot->last_original_draw_opcode =
        stream.last_original_draw_opcode;
    out_snapshot->total_parser_replay_commands =
        stream.total_parser_replay_commands;
    out_snapshot->total_parser_replay_draws =
        stream.total_parser_replay_draws;
    out_snapshot->total_native_line_segments =
        stream.total_native_line_segments;
    out_snapshot->total_shared_fmv_frames = stream.total_shared_fmv_frames;
    out_snapshot->total_shared_fmv_pixels = stream.total_shared_fmv_pixels;
    out_snapshot->last_shared_fmv_width = stream.last_shared_fmv_width;
    out_snapshot->last_shared_fmv_height = stream.last_shared_fmv_height;
    out_snapshot->last_shared_fmv_depth24 = stream.last_shared_fmv_depth24;
    out_snapshot->total_independent_fmv_frames =
        stream.total_independent_fmv_frames;
    out_snapshot->total_independent_fmv_pixels =
        stream.total_independent_fmv_pixels;
    out_snapshot->last_independent_fmv_width =
        stream.last_independent_fmv_width;
    out_snapshot->last_independent_fmv_height =
        stream.last_independent_fmv_height;
    out_snapshot->last_independent_fmv_depth24 =
        stream.last_independent_fmv_depth24;
    out_snapshot->total_ui_ot_adapter_calls =
        stream.total_ui_ot_adapter_calls;
    out_snapshot->total_guest_gp0_commands =
        stream.total_guest_gp0_commands;
    out_snapshot->total_shared_vram_presents =
        stream.total_shared_vram_presents;
    out_snapshot->total_native_lists = stream.total_native_lists;
    out_snapshot->total_native_packets = stream.total_native_packets;
    out_snapshot->total_native_bound_packets =
        stream.total_native_bound_packets;
    out_snapshot->total_native_state_packets =
        stream.total_native_state_packets;
    out_snapshot->total_native_unbound_packets =
        stream.total_native_unbound_packets;
    out_snapshot->total_native_unsupported_packets =
        stream.total_native_unsupported_packets;
    out_snapshot->total_independent_vram_presents =
        stream.total_independent_vram_presents;
    out_snapshot->first_native_unsupported_opcode =
        stream.first_native_unsupported_opcode;
    out_snapshot->last_native_unsupported_opcode =
        stream.last_native_unsupported_opcode;
    out_snapshot->first_native_unbound_opcode =
        stream.first_native_unbound_opcode;
    out_snapshot->last_native_unbound_opcode =
        stream.last_native_unbound_opcode;
    out_snapshot->first_native_unbound_source =
        stream.first_native_unbound_source;
    out_snapshot->first_native_unsupported_source =
        stream.first_native_unsupported_source;
    out_snapshot->first_native_unbound_pc = stream.first_native_unbound_pc;
    out_snapshot->first_native_unbound_function =
        stream.first_native_unbound_function;
    out_snapshot->first_native_unsupported_pc =
        stream.first_native_unsupported_pc;
    out_snapshot->first_native_unsupported_function =
        stream.first_native_unsupported_function;
    memcpy(out_snapshot->native_opcode_counts,
           stream.native_opcode_counts,
           sizeof(out_snapshot->native_opcode_counts));
    memcpy(out_snapshot->native_state_opcode_counts,
           stream.native_state_opcode_counts,
           sizeof(out_snapshot->native_state_opcode_counts));
    memcpy(out_snapshot->native_unbound_opcode_counts,
           stream.native_unbound_opcode_counts,
           sizeof(out_snapshot->native_unbound_opcode_counts));
    memcpy(out_snapshot->native_unsupported_opcode_counts,
           stream.native_unsupported_opcode_counts,
           sizeof(out_snapshot->native_unsupported_opcode_counts));
    memcpy(out_snapshot->native_unbound_source_by_opcode,
           stream.native_unbound_source_by_opcode,
           sizeof(out_snapshot->native_unbound_source_by_opcode));
    memcpy(out_snapshot->native_unbound_pc_by_opcode,
           stream.native_unbound_pc_by_opcode,
           sizeof(out_snapshot->native_unbound_pc_by_opcode));
    memcpy(out_snapshot->native_unsupported_pc_by_opcode,
           stream.native_unsupported_pc_by_opcode,
           sizeof(out_snapshot->native_unsupported_pc_by_opcode));
    memcpy(out_snapshot->native_unbound_return_address_by_opcode,
           stream.native_unbound_return_address_by_opcode,
           sizeof(out_snapshot->native_unbound_return_address_by_opcode));
    memcpy(out_snapshot->native_unsupported_return_address_by_opcode,
           stream.native_unsupported_return_address_by_opcode,
           sizeof(out_snapshot->native_unsupported_return_address_by_opcode));
    memcpy(out_snapshot->native_unbound_source_hotspots,
           stream.native_unbound_source_hotspots,
           sizeof(out_snapshot->native_unbound_source_hotspots));
    out_snapshot->last_native_state = stream.last_native_state;
    out_snapshot->first_native_unbound_return_address =
        stream.first_native_unbound_return_address;
    out_snapshot->first_native_unsupported_return_address =
        stream.first_native_unsupported_return_address;
    out_snapshot->total_visual_states = stream.total_visual_states;
    out_snapshot->total_superseded = stream.total_superseded;
    out_snapshot->stage_failure_count = stream.stage_failure_count;
    out_snapshot->first_stage_failure_command_id =
        stream.first_stage_failure_command_id;
    out_snapshot->first_stage_failure_visual_id =
        stream.first_stage_failure_visual_id;
    out_snapshot->first_stage_failure_status =
        stream.first_stage_failure_status;
    out_snapshot->last_command_id = stream.last_command_id;
    out_snapshot->last_status = stream.last_status;
    out_snapshot->last_stage_status = stream.last_stage_status;
    out_snapshot->last_consume_status = stream.last_consume_status;
    out_snapshot->enabled = stream.enabled;
    return GUEST_RENDER_NATIVE_STREAM_OK;
}

#ifdef GUEST_RENDER_NATIVE_STREAM_TESTING
void guest_render_native_stream_test_reset(void) {
    free(stream.command_generations);
    memset(&stream, 0, sizeof(stream));
}
#endif
