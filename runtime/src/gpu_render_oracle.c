#include "gpu_render_oracle_internal.h"

#include <limits.h>
#include <string.h>

enum {
    GPU_RENDER_ORACLE_OPERATION_NONE = 0,
    GPU_RENDER_ORACLE_OPERATION_COMMAND,
    GPU_RENDER_ORACLE_OPERATION_UPLOAD,
    GPU_RENDER_ORACLE_OPERATION_READBACK,
    GPU_RENDER_ORACLE_OPERATION_POLYLINE,
};

static uint32_t source_mask(GpuRenderOracleSourceKind source) {
    switch (source) {
    case GPU_RENDER_ORACLE_SOURCE_UNKNOWN:
        return GPU_RENDER_ORACLE_SOURCE_MASK_UNKNOWN;
    case GPU_RENDER_ORACLE_SOURCE_MMIO:
        return GPU_RENDER_ORACLE_SOURCE_MASK_MMIO;
    case GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK:
        return GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BLOCK;
    case GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST:
        return GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_LINKED_LIST;
    case GPU_RENDER_ORACLE_SOURCE_DMA2_BURST:
        return GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BURST;
    }
    return 0u;
}

static int source_is_valid(GpuRenderOracleSourceKind source) {
    return source_mask(source) != 0u;
}

static int command_is_valid(GpuRenderOracleCommandKind command) {
    return command >= GPU_RENDER_ORACLE_COMMAND_DRAW &&
           command <= GPU_RENDER_ORACLE_COMMAND_ENV_E6;
}

static void clear_journal(GpuRenderOracleDevice *device) {
    device->journal_frozen = device->global_vram_serial_overflowed;
    device->operation_kind = GPU_RENDER_ORACLE_OPERATION_NONE;
    device->operation_command = GPU_RENDER_ORACLE_COMMAND_NONE;
    device->event_count = 0u;
    device->dropped_events = 0u;
    device->operation_observed_words = 0u;
    device->operation_start_event = GPU_RENDER_ORACLE_EVENT_CAPACITY;
    device->incomplete_reason = device->global_vram_serial_overflowed
                                    ? GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW
                                    : GPU_RENDER_ORACLE_INCOMPLETE_NONE;
    memset(&device->operation_source, 0, sizeof(device->operation_source));
    memset(&device->operation_packet, 0, sizeof(device->operation_packet));
    memset(device->events, 0, sizeof(device->events));
}

static int capture_is_active(const GpuRenderOracleDevice *device) {
    return device->enabled != 0u &&
           device->phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_ACTIVE;
}

static GpuRenderOracleResult inactive_result(
    const GpuRenderOracleDevice *device) {
    if (device->enabled == 0u) {
        return GPU_RENDER_ORACLE_RESULT_DISABLED;
    }
    return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
}

static GpuRenderOracleResult note_dropped_event(GpuRenderOracleDevice *device) {
    if (device->dropped_events == UINT64_MAX) {
        return GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
    }
    ++device->dropped_events;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

static GpuRenderOracleCommandSummary command_summary(
    GpuRenderOracleCommandKind command, const GpuRenderOracleSourceSpan *source,
    const GpuRenderOracleDrawState *draw) {
    GpuRenderOracleCommandSummary summary = {0};

    if (source != NULL) {
        summary.word_count = source->word_count;
    }
    if (draw != NULL && command != GPU_RENDER_ORACLE_COMMAND_NONE) {
        summary.texture_page_x = draw->texture_page_x;
        summary.texture_page_y = draw->texture_page_y;
        summary.clut_x = draw->clut_x;
        summary.clut_y = draw->clut_y;
        summary.flags =
            (uint32_t)(draw->semi_transparency &
                       GPU_RENDER_ORACLE_COMMAND_FLAG_SEMI_TRANSPARENCY_MASK) |
            ((uint32_t)draw->texture_depth
             << GPU_RENDER_ORACLE_COMMAND_FLAG_TEXTURE_DEPTH_SHIFT) |
            (draw->dither != 0u ? GPU_RENDER_ORACLE_COMMAND_FLAG_DITHER : 0u) |
            (draw->mask_set != 0u ? GPU_RENDER_ORACLE_COMMAND_FLAG_MASK_SET : 0u) |
            (draw->mask_check != 0u
                 ? GPU_RENDER_ORACLE_COMMAND_FLAG_MASK_CHECK
                 : 0u);
    }

    switch (command) {
    case GPU_RENDER_ORACLE_COMMAND_NONE:
        break;
    case GPU_RENDER_ORACLE_COMMAND_DRAW:
        summary.opcode = UINT8_C(0x20);
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_PRIMITIVE;
        summary.vertex_count = 3u;
        break;
    case GPU_RENDER_ORACLE_COMMAND_FILL:
        summary.opcode = UINT8_C(0x02);
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_VRAM;
        summary.rectangle_count = 1u;
        break;
    case GPU_RENDER_ORACLE_COMMAND_COPY:
        summary.opcode = UINT8_C(0x80);
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_VRAM;
        summary.rectangle_count = 1u;
        break;
    case GPU_RENDER_ORACLE_COMMAND_UPLOAD:
        summary.opcode = UINT8_C(0xa0);
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_TRANSFER;
        summary.rectangle_count = 1u;
        break;
    case GPU_RENDER_ORACLE_COMMAND_READBACK:
        summary.opcode = UINT8_C(0xc0);
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_TRANSFER;
        summary.rectangle_count = 1u;
        break;
    case GPU_RENDER_ORACLE_COMMAND_POLYLINE:
        summary.opcode = UINT8_C(0x48);
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_PRIMITIVE;
        break;
    case GPU_RENDER_ORACLE_COMMAND_ENV_E1:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E2:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E3:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E4:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E5:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E6:
        summary.opcode = (uint8_t)(UINT8_C(0xe1) +
                                   (command - GPU_RENDER_ORACLE_COMMAND_ENV_E1));
        summary.command_class = GPU_RENDER_ORACLE_COMMAND_CLASS_ENVIRONMENT;
        break;
    }
    return summary;
}

static GpuRenderOracleEvent make_event(
    const GpuRenderOracleDevice *device, GpuRenderOracleEventKind kind,
    GpuRenderOracleMutationKind mutation,
    GpuRenderOracleIncompleteReason incomplete_reason,
    GpuRenderOracleCommandKind command,
    const GpuRenderOracleSourceSpan *source,
    const GpuRenderOracleTransfer *transfer) {
    GpuRenderOracleEvent event = {0};

    event.vram_serial = device->global_vram_serial;
    event.kind = kind;
    event.mutation = mutation;
    event.incomplete_reason = incomplete_reason;
    if (source != NULL) {
        event.source = *source;
    }
    event.draw = device->draw_state;
    event.display = device->display_state;
    if (transfer != NULL) {
        event.transfer = *transfer;
    }
    event.command = command_summary(command, source, &event.draw);
    event.packet = device->operation_packet;
    return event;
}

static GpuRenderOracleResult freeze_incomplete(
    GpuRenderOracleDevice *device, GpuRenderOracleIncompleteReason reason,
    const GpuRenderOracleEvent *context) {
    GpuRenderOracleEvent terminal = {0};

    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (context != NULL) {
        terminal = *context;
    }
    terminal.sequence = device->next_sequence;
    terminal.vram_serial = device->global_vram_serial;
    terminal.kind = GPU_RENDER_ORACLE_EVENT_INCOMPLETE;
    terminal.mutation = GPU_RENDER_ORACLE_MUTATION_NONE;
    terminal.incomplete_reason = reason;
    terminal.packet.task11_family_eligible = 0u;
    if (device->event_count < GPU_RENDER_ORACLE_EVENT_CAPACITY) {
        device->events[device->event_count] = terminal;
        ++device->event_count;
    }
    if (device->next_sequence != UINT64_MAX) {
        ++device->next_sequence;
    }
    device->journal_frozen = 1u;
    device->incomplete_reason = reason;
    if (reason == GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY) {
        return note_dropped_event(device);
    }
    return GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
}

static GpuRenderOracleResult append_event(GpuRenderOracleDevice *device,
                                          GpuRenderOracleEvent *event) {
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (device->event_count >= GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u) {
        return freeze_incomplete(device, GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY,
                                 event);
    }
    if (device->next_sequence == UINT64_MAX) {
        return freeze_incomplete(device,
                                 GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
                                 event);
    }
    event->sequence = device->next_sequence;
    ++device->next_sequence;
    device->events[device->event_count] = *event;
    ++device->event_count;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

static GpuRenderOracleResult advance_vram_serial(
    GpuRenderOracleDevice *device, GpuRenderOracleMutationKind mutation,
    const GpuRenderOracleEvent *context) {
    if (mutation == GPU_RENDER_ORACLE_MUTATION_NONE) {
        return GPU_RENDER_ORACLE_RESULT_OK;
    }
    if (mutation < GPU_RENDER_ORACLE_MUTATION_DRAW ||
        mutation > GPU_RENDER_ORACLE_MUTATION_UPLOAD) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->global_vram_serial_overflowed != 0u) {
        return GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
    }
    if (device->global_vram_serial == UINT64_MAX) {
        device->global_vram_serial_overflowed = 1u;
        device->incomplete_reason = GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW;
        if (capture_is_active(device) && device->journal_frozen == 0u) {
            return freeze_incomplete(
                device, GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW, context);
        }
        device->journal_frozen = 1u;
        return GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
    }
    ++device->global_vram_serial;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

static GpuRenderOracleResult operation_result(
    GpuRenderOracleDevice *device) {
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        return inactive_result(device);
    }
    return GPU_RENDER_ORACLE_RESULT_OK;
}

static void clear_operation(GpuRenderOracleDevice *device) {
    device->operation_kind = GPU_RENDER_ORACLE_OPERATION_NONE;
    device->operation_observed_words = 0u;
    device->operation_start_event = GPU_RENDER_ORACLE_EVENT_CAPACITY;
    memset(&device->operation_source, 0, sizeof(device->operation_source));
    memset(&device->operation_packet, 0, sizeof(device->operation_packet));
}

static int mutation_matches_command(GpuRenderOracleCommandKind command,
                                    GpuRenderOracleMutationKind mutation) {
    switch (command) {
    case GPU_RENDER_ORACLE_COMMAND_NONE:
        return 0;
    case GPU_RENDER_ORACLE_COMMAND_DRAW:
        return mutation == GPU_RENDER_ORACLE_MUTATION_DRAW;
    case GPU_RENDER_ORACLE_COMMAND_FILL:
        return mutation == GPU_RENDER_ORACLE_MUTATION_FILL;
    case GPU_RENDER_ORACLE_COMMAND_COPY:
        return mutation == GPU_RENDER_ORACLE_MUTATION_COPY;
    case GPU_RENDER_ORACLE_COMMAND_UPLOAD:
        return mutation == GPU_RENDER_ORACLE_MUTATION_UPLOAD;
    case GPU_RENDER_ORACLE_COMMAND_READBACK:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E1:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E2:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E3:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E4:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E5:
    case GPU_RENDER_ORACLE_COMMAND_ENV_E6:
        return mutation == GPU_RENDER_ORACLE_MUTATION_NONE;
    case GPU_RENDER_ORACLE_COMMAND_POLYLINE:
        return 0;
    }
    return 0;
}

static GpuRenderOracleTransfer completed_transfer(
    const GpuRenderOracleDevice *device, const GpuRenderOracleTransfer *transfer) {
    GpuRenderOracleTransfer completed = *transfer;
    uint64_t pixels = (uint64_t)transfer->width * (uint64_t)transfer->height;

    completed.observed_words = device->operation_observed_words;
    if (transfer->direction == GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM) {
        completed.written_pixels = pixels;
    } else {
        completed.written_pixels = 0u;
    }
    completed.skipped_pixels = 0u;
    completed.wrapped = (uint8_t)(transfer->width != 0u && transfer->height != 0u &&
                                  ((uint32_t)transfer->x + transfer->width > 1024u ||
                                   (uint32_t)transfer->y + transfer->height > 512u));
    return completed;
}

void gpu_render_oracle_device_init(GpuRenderOracleDevice *device) {
    if (device == NULL) {
        return;
    }
    memset(device, 0, sizeof(*device));
    device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_DISABLED;
    device->next_sequence = 1u;
    device->operation_start_event = GPU_RENDER_ORACLE_EVENT_CAPACITY;
}

int gpu_render_oracle_capture_enabled(const GpuRenderOracleDevice *device) {
    return device != NULL && device->enabled != 0u;
}

GpuRenderOracleResult gpu_render_oracle_capture_set_enabled(
    GpuRenderOracleDevice *device, int enabled) {
    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (enabled == 0) {
        device->enabled = 0u;
        device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_DISABLED;
        clear_journal(device);
        return GPU_RENDER_ORACLE_RESULT_OK;
    }
    device->enabled = 1u;
    if (device->phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_DISABLED) {
        device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_READY;
    }
    return device->global_vram_serial_overflowed
               ? GPU_RENDER_ORACLE_RESULT_INCOMPLETE
               : GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_device_capture_begin(
    GpuRenderOracleDevice *device) {
    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->enabled == 0u) {
        return GPU_RENDER_ORACLE_RESULT_DISABLED;
    }
    if (device->global_vram_serial_overflowed != 0u) {
        return GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
    }
    if (device->phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_ACTIVE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    clear_journal(device);
    device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_ACTIVE;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_device_capture_end(
    GpuRenderOracleDevice *device) {
    GpuRenderOracleEvent context;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (!capture_is_active(device)) {
        return inactive_result(device);
    }
    if (device->journal_frozen != 0u) {
        device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_ENDED;
        return device->incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY
                   ? GPU_RENDER_ORACLE_RESULT_OK
                   : GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
    }
    if (device->operation_kind != GPU_RENDER_ORACLE_OPERATION_NONE) {
        context = make_event(device, GPU_RENDER_ORACLE_EVENT_INCOMPLETE,
                             GPU_RENDER_ORACLE_MUTATION_NONE,
                             GPU_RENDER_ORACLE_INCOMPLETE_OPEN_OPERATION,
                             device->operation_command,
                             &device->operation_source, NULL);
        (void)freeze_incomplete(device,
                                GPU_RENDER_ORACLE_INCOMPLETE_OPEN_OPERATION,
                                &context);
        device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_ENDED;
        return GPU_RENDER_ORACLE_RESULT_INCOMPLETE;
    }
    device->phase = GPU_RENDER_ORACLE_CAPTURE_PHASE_ENDED;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_device_capture_snapshot(
    const GpuRenderOracleDevice *device, GpuRenderOracleSnapshot *snapshot) {
    if (device == NULL || snapshot == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    snapshot->phase = device->phase;
    snapshot->event_count = device->event_count;
    snapshot->dropped_events = device->dropped_events;
    snapshot->global_vram_serial = device->global_vram_serial;
    snapshot->incomplete_reason = device->incomplete_reason;
    snapshot->enabled = device->enabled;
    snapshot->journal_frozen = device->journal_frozen;
    snapshot->global_vram_serial_overflowed =
        device->global_vram_serial_overflowed;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_device_event_get(
    const GpuRenderOracleDevice *device, uint64_t index,
    GpuRenderOracleEvent *event) {
    if (device == NULL || event == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (index >= GPU_RENDER_ORACLE_EVENT_CAPACITY || index >= device->event_count) {
        return GPU_RENDER_ORACLE_RESULT_OUT_OF_RANGE;
    }
    if (device->phase != GPU_RENDER_ORACLE_CAPTURE_PHASE_ENDED &&
        device->journal_frozen == 0u) {
        return GPU_RENDER_ORACLE_RESULT_NOT_FROZEN;
    }
    *event = device->events[index];
    return GPU_RENDER_ORACLE_RESULT_OK;
}

int gpu_render_oracle_event_is_metadata_only(const GpuRenderOracleEvent *event) {
    return event != NULL && event->sequence != 0u &&
           event->kind <= GPU_RENDER_ORACLE_EVENT_INCOMPLETE &&
           event->mutation <= GPU_RENDER_ORACLE_MUTATION_UPLOAD &&
           event->incomplete_reason <=
               GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW &&
            event->command.command_class <=
                GPU_RENDER_ORACLE_COMMAND_CLASS_ENVIRONMENT &&
            event->packet.parser_class <= GPU_RENDER_ORACLE_PACKET_CLASS_MALFORMED &&
            event->packet.task11_family_eligible <= 1u &&
            event->transfer.direction <= GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU;
}

static GpuRenderOracleResult gpu_render_oracle_gp0_begin_with_packet(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source, const GpuRenderOraclePacket *packet) {
    GpuRenderOracleEvent event;
    GpuRenderOracleResult result;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    result = operation_result(device);
    if (result != GPU_RENDER_ORACLE_RESULT_OK) {
        return result;
    }
    if (!command_is_valid(command) || !source_is_valid(source) ||
        device->operation_kind != GPU_RENDER_ORACLE_OPERATION_NONE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    device->operation_command = command;
    device->operation_source.kind_mask = source_mask(source);
    if (packet != NULL) {
        device->operation_packet = *packet;
    }
    device->operation_observed_words = 0u;
    device->operation_start_event = GPU_RENDER_ORACLE_EVENT_CAPACITY;

    if (command == GPU_RENDER_ORACLE_COMMAND_UPLOAD ||
        command == GPU_RENDER_ORACLE_COMMAND_READBACK) {
        device->operation_kind = command == GPU_RENDER_ORACLE_COMMAND_UPLOAD
                                     ? GPU_RENDER_ORACLE_OPERATION_UPLOAD
                                     : GPU_RENDER_ORACLE_OPERATION_READBACK;
        event = make_event(device, GPU_RENDER_ORACLE_EVENT_TRANSFER_BEGIN,
                           GPU_RENDER_ORACLE_MUTATION_NONE,
                           GPU_RENDER_ORACLE_INCOMPLETE_NONE, command,
                           &device->operation_source, NULL);
        result = append_event(device, &event);
        if (result != GPU_RENDER_ORACLE_RESULT_OK) {
            clear_operation(device);
            return result;
        }
        device->operation_start_event = (uint32_t)(device->event_count - 1u);
        return GPU_RENDER_ORACLE_RESULT_OK;
    }
    if (command == GPU_RENDER_ORACLE_COMMAND_POLYLINE) {
        device->operation_kind = GPU_RENDER_ORACLE_OPERATION_POLYLINE;
        event = make_event(device, GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_BEGIN,
                           GPU_RENDER_ORACLE_MUTATION_NONE,
                           GPU_RENDER_ORACLE_INCOMPLETE_NONE, command,
                           &device->operation_source, NULL);
        result = append_event(device, &event);
        if (result != GPU_RENDER_ORACLE_RESULT_OK) {
            clear_operation(device);
            return result;
        }
        device->operation_start_event = (uint32_t)(device->event_count - 1u);
        return GPU_RENDER_ORACLE_RESULT_OK;
    }
    device->operation_kind = GPU_RENDER_ORACLE_OPERATION_COMMAND;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_gp0_begin(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source) {
    return gpu_render_oracle_gp0_begin_with_packet(device, command, source, NULL);
}

GpuRenderOracleResult gpu_render_oracle_gp0_begin_parsed(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source, const GpuRenderOraclePacket *packet) {
    if (packet == NULL ||
        packet->parser_class == GPU_RENDER_ORACLE_PACKET_CLASS_NONE ||
        packet->parser_class > GPU_RENDER_ORACLE_PACKET_CLASS_MALFORMED ||
        packet->task11_family_eligible > 1u) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    return gpu_render_oracle_gp0_begin_with_packet(device, command, source, packet);
}

GpuRenderOracleResult gpu_render_oracle_gp0_source_word(
    GpuRenderOracleDevice *device, uint64_t word_ordinal,
    uint64_t container_ordinal) {
    GpuRenderOracleSourceSpan *source;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        return inactive_result(device);
    }
    if (device->operation_kind == GPU_RENDER_ORACLE_OPERATION_NONE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    source = &device->operation_source;
    if (source->word_count == UINT64_MAX) {
        GpuRenderOracleEvent context = make_event(
            device, GPU_RENDER_ORACLE_EVENT_INCOMPLETE,
            GPU_RENDER_ORACLE_MUTATION_NONE,
            GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
            device->operation_command, source, NULL);
        return freeze_incomplete(device,
                                 GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
                                 &context);
    }
    if (source->word_count == 0u) {
        source->first_word = word_ordinal;
        source->first_container = container_ordinal;
    } else if (container_ordinal != source->last_container ||
               source->last_word == UINT64_MAX ||
               word_ordinal != source->last_word + 1u) {
        source->discontinuous = 1u;
    }
    source->last_word = word_ordinal;
    source->last_container = container_ordinal;
    ++source->word_count;
    if (device->operation_start_event < device->event_count) {
        GpuRenderOracleEvent *event =
            &device->events[device->operation_start_event];
        event->source = *source;
        event->command.word_count = source->word_count;
    }
    return GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_gp0_complete(
    GpuRenderOracleDevice *device, GpuRenderOracleMutationKind mutation,
    const GpuRenderOracleDrawState *draw,
    const GpuRenderOracleTransfer *transfer) {
    GpuRenderOracleEvent event;
    GpuRenderOracleTransfer completed;
    GpuRenderOracleResult result;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (mutation < GPU_RENDER_ORACLE_MUTATION_NONE ||
        mutation > GPU_RENDER_ORACLE_MUTATION_UPLOAD) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        result = advance_vram_serial(device, mutation, NULL);
        if (result != GPU_RENDER_ORACLE_RESULT_OK) return result;
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        result = advance_vram_serial(device, mutation, NULL);
        if (result != GPU_RENDER_ORACLE_RESULT_OK) return result;
        return inactive_result(device);
    }
    if (device->operation_kind != GPU_RENDER_ORACLE_OPERATION_COMMAND &&
        device->operation_kind != GPU_RENDER_ORACLE_OPERATION_UPLOAD &&
        device->operation_kind != GPU_RENDER_ORACLE_OPERATION_READBACK) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    if (!mutation_matches_command(device->operation_command, mutation) ||
        ((device->operation_kind == GPU_RENDER_ORACLE_OPERATION_UPLOAD ||
          device->operation_kind == GPU_RENDER_ORACLE_OPERATION_READBACK) &&
         transfer == NULL)) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    if (device->operation_kind == GPU_RENDER_ORACLE_OPERATION_UPLOAD ||
        device->operation_kind == GPU_RENDER_ORACLE_OPERATION_READBACK) {
        completed = completed_transfer(device, transfer);
        if ((device->operation_kind == GPU_RENDER_ORACLE_OPERATION_UPLOAD &&
             completed.direction != GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM) ||
            (device->operation_kind == GPU_RENDER_ORACLE_OPERATION_READBACK &&
             completed.direction != GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU)) {
            return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
        }
    }
    if (draw != NULL) {
        device->draw_state = *draw;
    }
    event = make_event(device, GPU_RENDER_ORACLE_EVENT_GP0_COMMAND, mutation,
                       GPU_RENDER_ORACLE_INCOMPLETE_NONE,
                       device->operation_command, &device->operation_source,
                       NULL);
    result = advance_vram_serial(device, mutation, &event);
    if (result != GPU_RENDER_ORACLE_RESULT_OK) {
        clear_operation(device);
        return result;
    }
    event.vram_serial = device->global_vram_serial;
    if (device->operation_kind == GPU_RENDER_ORACLE_OPERATION_UPLOAD ||
        device->operation_kind == GPU_RENDER_ORACLE_OPERATION_READBACK) {
        event.kind = GPU_RENDER_ORACLE_EVENT_TRANSFER_END;
        event.transfer = completed;
        if (device->operation_start_event < device->event_count) {
            GpuRenderOracleEvent *begin =
                &device->events[device->operation_start_event];
            begin->transfer = completed;
            begin->draw = device->draw_state;
            begin->command = command_summary(device->operation_command,
                                             &device->operation_source,
                                             &begin->draw);
        }
    }
    result = append_event(device, &event);
    clear_operation(device);
    return result;
}

GpuRenderOracleResult gpu_render_oracle_gp0_abort(
    GpuRenderOracleDevice *device) {
    GpuRenderOracleEvent event;
    GpuRenderOracleResult result;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        return inactive_result(device);
    }
    if (device->operation_kind == GPU_RENDER_ORACLE_OPERATION_NONE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    event = make_event(device, GPU_RENDER_ORACLE_EVENT_TRANSFER_ABORT,
                       GPU_RENDER_ORACLE_MUTATION_NONE,
                       GPU_RENDER_ORACLE_INCOMPLETE_NONE,
                       device->operation_command, &device->operation_source,
                       NULL);
    if (device->operation_kind == GPU_RENDER_ORACLE_OPERATION_POLYLINE) {
        event.kind = GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_END;
    }
    event.packet.task11_family_eligible = 0u;
    result = append_event(device, &event);
    clear_operation(device);
    return result;
}

GpuRenderOracleResult gpu_render_oracle_gp1_complete(
    GpuRenderOracleDevice *device, const GpuRenderOracleDisplayState *display) {
    GpuRenderOracleEvent event;
    GpuRenderOracleResult result;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    result = operation_result(device);
    if (result != GPU_RENDER_ORACLE_RESULT_OK) {
        return result;
    }
    if (device->operation_kind != GPU_RENDER_ORACLE_OPERATION_NONE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    if (display != NULL) {
        device->display_state = *display;
    }
    event = make_event(device, GPU_RENDER_ORACLE_EVENT_GP1,
                       GPU_RENDER_ORACLE_MUTATION_NONE,
                       GPU_RENDER_ORACLE_INCOMPLETE_NONE,
                       GPU_RENDER_ORACLE_COMMAND_NONE, NULL, NULL);
    return append_event(device, &event);
}

static GpuRenderOracleResult observe_transfer_word(
    GpuRenderOracleDevice *device, uint8_t operation_kind) {
    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        return inactive_result(device);
    }
    if (device->operation_kind != operation_kind) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    if (device->operation_observed_words == UINT64_MAX) {
        GpuRenderOracleEvent context = make_event(
            device, GPU_RENDER_ORACLE_EVENT_INCOMPLETE,
            GPU_RENDER_ORACLE_MUTATION_NONE,
            GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
            device->operation_command, &device->operation_source, NULL);
        return freeze_incomplete(device,
                                 GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
                                 &context);
    }
    ++device->operation_observed_words;
    return GPU_RENDER_ORACLE_RESULT_OK;
}

GpuRenderOracleResult gpu_render_oracle_upload_word(
    GpuRenderOracleDevice *device) {
    return observe_transfer_word(device, GPU_RENDER_ORACLE_OPERATION_UPLOAD);
}

GpuRenderOracleResult gpu_render_oracle_gpuread_word(
    GpuRenderOracleDevice *device) {
    return observe_transfer_word(device, GPU_RENDER_ORACLE_OPERATION_READBACK);
}

GpuRenderOracleResult gpu_render_oracle_polyline_segment(
    GpuRenderOracleDevice *device, const GpuRenderOracleDrawState *draw) {
    GpuRenderOracleEvent event;
    GpuRenderOracleResult result;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        result = advance_vram_serial(
            device, GPU_RENDER_ORACLE_MUTATION_DRAW, NULL);
        if (result != GPU_RENDER_ORACLE_RESULT_OK) return result;
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        result = advance_vram_serial(
            device, GPU_RENDER_ORACLE_MUTATION_DRAW, NULL);
        if (result != GPU_RENDER_ORACLE_RESULT_OK) return result;
        return inactive_result(device);
    }
    if (device->operation_kind != GPU_RENDER_ORACLE_OPERATION_POLYLINE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    if (device->operation_observed_words == UINT64_MAX) {
        GpuRenderOracleEvent context = make_event(
            device, GPU_RENDER_ORACLE_EVENT_INCOMPLETE,
            GPU_RENDER_ORACLE_MUTATION_NONE,
            GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
            device->operation_command, &device->operation_source, NULL);
        return freeze_incomplete(device,
                                 GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
                                 &context);
    }
    if (draw != NULL) {
        device->draw_state = *draw;
    }
    ++device->operation_observed_words;
    event = make_event(device, GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_SEGMENT,
                       GPU_RENDER_ORACLE_MUTATION_DRAW,
                       GPU_RENDER_ORACLE_INCOMPLETE_NONE,
                       device->operation_command, &device->operation_source,
                       NULL);
    result = advance_vram_serial(device, GPU_RENDER_ORACLE_MUTATION_DRAW, &event);
    if (result != GPU_RENDER_ORACLE_RESULT_OK) {
        clear_operation(device);
        return result;
    }
    event.vram_serial = device->global_vram_serial;
    event.command.vertex_count = device->operation_observed_words;
    return append_event(device, &event);
}

GpuRenderOracleResult gpu_render_oracle_polyline_end(
    GpuRenderOracleDevice *device) {
    GpuRenderOracleEvent event;
    GpuRenderOracleResult result;

    if (device == NULL) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT;
    }
    if (device->journal_frozen != 0u) {
        return note_dropped_event(device);
    }
    if (!capture_is_active(device)) {
        return inactive_result(device);
    }
    if (device->operation_kind != GPU_RENDER_ORACLE_OPERATION_POLYLINE) {
        return GPU_RENDER_ORACLE_RESULT_INVALID_STATE;
    }
    event = make_event(device, GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_END,
                       GPU_RENDER_ORACLE_MUTATION_NONE,
                       GPU_RENDER_ORACLE_INCOMPLETE_NONE,
                       device->operation_command, &device->operation_source,
                       NULL);
    result = append_event(device, &event);
    clear_operation(device);
    return result;
}

#if defined(GPU_RENDER_ORACLE_TESTING)
void gpu_render_oracle_test_seed_next_sequence(GpuRenderOracleDevice *device,
                                                uint64_t next_sequence) {
    if (device != NULL) {
        device->next_sequence = next_sequence;
    }
}

void gpu_render_oracle_test_seed_vram_serial(GpuRenderOracleDevice *device,
                                              uint64_t vram_serial) {
    if (device != NULL) {
        device->global_vram_serial = vram_serial;
    }
}

void gpu_render_oracle_test_seed_event_count(GpuRenderOracleDevice *device,
                                              uint64_t event_count) {
    if (device != NULL &&
        event_count < GPU_RENDER_ORACLE_EVENT_CAPACITY) {
        device->event_count = event_count;
    }
}
#endif
