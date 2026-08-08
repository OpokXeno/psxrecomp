#ifndef PSXRECOMP_GPU_RENDER_ORACLE_H
#define PSXRECOMP_GPU_RENDER_ORACLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_RENDER_ORACLE_EVENT_METADATA_ONLY 1
#ifndef GPU_RENDER_ORACLE_EVENT_CAPACITY
#define GPU_RENDER_ORACLE_EVENT_CAPACITY 64u
#endif

enum {
    GPU_RENDER_ORACLE_COMMAND_FLAG_SEMI_TRANSPARENCY_MASK = UINT32_C(0x03),
    GPU_RENDER_ORACLE_COMMAND_FLAG_TEXTURE_DEPTH_SHIFT = 2,
    GPU_RENDER_ORACLE_COMMAND_FLAG_DITHER = UINT32_C(1) << 4,
    GPU_RENDER_ORACLE_COMMAND_FLAG_MASK_SET = UINT32_C(1) << 5,
    GPU_RENDER_ORACLE_COMMAND_FLAG_MASK_CHECK = UINT32_C(1) << 6,
};

enum {
    GPU_RENDER_ORACLE_SOURCE_MASK_UNKNOWN = UINT32_C(1) << 0,
    GPU_RENDER_ORACLE_SOURCE_MASK_MMIO = UINT32_C(1) << 1,
    GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BLOCK = UINT32_C(1) << 2,
    GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_LINKED_LIST = UINT32_C(1) << 3,
    GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BURST = UINT32_C(1) << 4,
};

typedef enum {
    GPU_RENDER_ORACLE_RESULT_OK = 0,
    GPU_RENDER_ORACLE_RESULT_DISABLED,
    GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT,
    GPU_RENDER_ORACLE_RESULT_INVALID_STATE,
    GPU_RENDER_ORACLE_RESULT_OUT_OF_RANGE,
    GPU_RENDER_ORACLE_RESULT_NOT_FROZEN,
    GPU_RENDER_ORACLE_RESULT_INCOMPLETE,
} GpuRenderOracleResult;

typedef enum {
    GPU_RENDER_ORACLE_CAPTURE_PHASE_DISABLED = 0,
    GPU_RENDER_ORACLE_CAPTURE_PHASE_READY,
    GPU_RENDER_ORACLE_CAPTURE_PHASE_ACTIVE,
    GPU_RENDER_ORACLE_CAPTURE_PHASE_ENDED,
} GpuRenderOracleCapturePhase;

typedef enum {
    GPU_RENDER_ORACLE_SOURCE_UNKNOWN = 0,
    GPU_RENDER_ORACLE_SOURCE_MMIO,
    GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
    GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
    GPU_RENDER_ORACLE_SOURCE_DMA2_BURST,
} GpuRenderOracleSourceKind;

typedef enum {
    GPU_RENDER_ORACLE_COMMAND_NONE = 0,
    GPU_RENDER_ORACLE_COMMAND_DRAW,
    GPU_RENDER_ORACLE_COMMAND_FILL,
    GPU_RENDER_ORACLE_COMMAND_COPY,
    GPU_RENDER_ORACLE_COMMAND_UPLOAD,
    GPU_RENDER_ORACLE_COMMAND_READBACK,
    GPU_RENDER_ORACLE_COMMAND_POLYLINE,
    GPU_RENDER_ORACLE_COMMAND_ENV_E1,
    GPU_RENDER_ORACLE_COMMAND_ENV_E2,
    GPU_RENDER_ORACLE_COMMAND_ENV_E3,
    GPU_RENDER_ORACLE_COMMAND_ENV_E4,
    GPU_RENDER_ORACLE_COMMAND_ENV_E5,
    GPU_RENDER_ORACLE_COMMAND_ENV_E6,
} GpuRenderOracleCommandKind;

typedef enum {
    GPU_RENDER_ORACLE_COMMAND_CLASS_NONE = 0,
    GPU_RENDER_ORACLE_COMMAND_CLASS_PRIMITIVE,
    GPU_RENDER_ORACLE_COMMAND_CLASS_VRAM,
    GPU_RENDER_ORACLE_COMMAND_CLASS_TRANSFER,
    GPU_RENDER_ORACLE_COMMAND_CLASS_ENVIRONMENT,
} GpuRenderOracleCommandClass;

typedef enum {
    GPU_RENDER_ORACLE_PACKET_CLASS_NONE = 0,
    GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_UNTEXTURED,
    GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED,
    GPU_RENDER_ORACLE_PACKET_CLASS_VARIABLE,
    GPU_RENDER_ORACLE_PACKET_CLASS_MALFORMED,
} GpuRenderOraclePacketClass;

typedef enum {
    GPU_RENDER_ORACLE_EVENT_GP0_COMMAND = 0,
    GPU_RENDER_ORACLE_EVENT_GP1,
    GPU_RENDER_ORACLE_EVENT_TRANSFER_BEGIN,
    GPU_RENDER_ORACLE_EVENT_TRANSFER_END,
    GPU_RENDER_ORACLE_EVENT_TRANSFER_ABORT,
    GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_BEGIN,
    GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_SEGMENT,
    GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_END,
    GPU_RENDER_ORACLE_EVENT_INCOMPLETE,
} GpuRenderOracleEventKind;

typedef enum {
    GPU_RENDER_ORACLE_MUTATION_NONE = 0,
    GPU_RENDER_ORACLE_MUTATION_DRAW,
    GPU_RENDER_ORACLE_MUTATION_FILL,
    GPU_RENDER_ORACLE_MUTATION_COPY,
    GPU_RENDER_ORACLE_MUTATION_UPLOAD,
} GpuRenderOracleMutationKind;

typedef enum {
    GPU_RENDER_ORACLE_INCOMPLETE_NONE = 0,
    GPU_RENDER_ORACLE_INCOMPLETE_OPEN_OPERATION,
    GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY,
    GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW,
    GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW,
} GpuRenderOracleIncompleteReason;

typedef enum {
    GPU_RENDER_ORACLE_TRANSFER_NONE = 0,
    GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM,
    GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU,
} GpuRenderOracleTransferDirection;

typedef struct {
    uint32_t kind_mask;
    uint64_t first_word;
    uint64_t last_word;
    uint64_t first_container;
    uint64_t last_container;
    uint64_t word_count;
    uint8_t discontinuous;
} GpuRenderOracleSourceSpan;

typedef struct {
    uint16_t texture_page_x;
    uint16_t texture_page_y;
    uint16_t clut_x;
    uint16_t clut_y;
    uint8_t semi_transparency;
    uint8_t texture_depth;
    uint8_t dither;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t offset_x;
    int16_t offset_y;
    uint8_t mask_set;
    uint8_t mask_check;
} GpuRenderOracleDrawState;

typedef struct {
    uint16_t display_x;
    uint16_t display_y;
    uint16_t horizontal_start;
    uint16_t horizontal_end;
    uint16_t vertical_start;
    uint16_t vertical_end;
    uint16_t width;
    uint16_t height;
    uint8_t depth24;
    uint8_t disabled;
} GpuRenderOracleDisplayState;

typedef struct {
    GpuRenderOracleTransferDirection direction;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint64_t expected_words;
    uint64_t observed_words;
    uint64_t written_pixels;
    uint64_t skipped_pixels;
    uint8_t wrapped;
} GpuRenderOracleTransfer;

typedef struct {
    uint8_t opcode;
    GpuRenderOracleCommandClass command_class;
    uint32_t flags;
    uint64_t word_count;
    uint64_t vertex_count;
    uint64_t rectangle_count;
    uint16_t clut_x;
    uint16_t clut_y;
    uint16_t texture_page_x;
    uint16_t texture_page_y;
} GpuRenderOracleCommandSummary;

typedef struct {
    uint8_t opcode;
    uint8_t task11_family_eligible;
    uint16_t parser_word_count;
    GpuRenderOraclePacketClass parser_class;
} GpuRenderOraclePacket;

typedef struct {
    uint64_t sequence;
    uint64_t vram_serial;
    GpuRenderOracleEventKind kind;
    GpuRenderOracleMutationKind mutation;
    GpuRenderOracleIncompleteReason incomplete_reason;
    GpuRenderOracleSourceSpan source;
    GpuRenderOracleCommandSummary command;
    GpuRenderOraclePacket packet;
    GpuRenderOracleDrawState draw;
    GpuRenderOracleDisplayState display;
    GpuRenderOracleTransfer transfer;
} GpuRenderOracleEvent;

typedef struct {
    GpuRenderOracleCapturePhase phase;
    uint64_t event_count;
    uint64_t dropped_events;
    uint64_t global_vram_serial;
    GpuRenderOracleIncompleteReason incomplete_reason;
    uint8_t enabled;
    uint8_t journal_frozen;
    uint8_t global_vram_serial_overflowed;
} GpuRenderOracleSnapshot;

typedef struct {
    uint8_t enabled;
    GpuRenderOracleCapturePhase phase;
    uint8_t journal_frozen;
    uint8_t operation_kind;
    GpuRenderOracleCommandKind operation_command;
    uint64_t next_sequence;
    uint64_t global_vram_serial;
    uint8_t global_vram_serial_overflowed;
    uint64_t event_count;
    uint64_t dropped_events;
    uint64_t operation_observed_words;
    uint32_t operation_start_event;
    GpuRenderOracleIncompleteReason incomplete_reason;
    GpuRenderOracleSourceSpan operation_source;
    GpuRenderOraclePacket operation_packet;
    GpuRenderOracleDrawState draw_state;
    GpuRenderOracleDisplayState display_state;
    GpuRenderOracleEvent events[GPU_RENDER_ORACLE_EVENT_CAPACITY];
} GpuRenderOracleDevice;

void gpu_render_oracle_device_init(GpuRenderOracleDevice *device);
int gpu_render_oracle_capture_enabled(const GpuRenderOracleDevice *device);
GpuRenderOracleResult gpu_render_oracle_capture_set_enabled(
    GpuRenderOracleDevice *device, int enabled);
GpuRenderOracleResult gpu_render_oracle_device_capture_begin(
    GpuRenderOracleDevice *device);
GpuRenderOracleResult gpu_render_oracle_device_capture_end(
    GpuRenderOracleDevice *device);
GpuRenderOracleResult gpu_render_oracle_device_capture_snapshot(
    const GpuRenderOracleDevice *device, GpuRenderOracleSnapshot *snapshot);
GpuRenderOracleResult gpu_render_oracle_device_event_get(
    const GpuRenderOracleDevice *device, uint64_t index,
    GpuRenderOracleEvent *event);
int gpu_render_oracle_event_is_metadata_only(const GpuRenderOracleEvent *event);

GpuRenderOracleResult gpu_render_oracle_gp0_begin(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source);
GpuRenderOracleResult gpu_render_oracle_gp0_begin_parsed(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source, const GpuRenderOraclePacket *packet);
GpuRenderOracleResult gpu_render_oracle_gp0_source_word(
    GpuRenderOracleDevice *device, uint64_t word_ordinal,
    uint64_t container_ordinal);
GpuRenderOracleResult gpu_render_oracle_gp0_complete(
    GpuRenderOracleDevice *device, GpuRenderOracleMutationKind mutation,
    const GpuRenderOracleDrawState *draw,
    const GpuRenderOracleTransfer *transfer);
GpuRenderOracleResult gpu_render_oracle_gp0_abort(
    GpuRenderOracleDevice *device);
GpuRenderOracleResult gpu_render_oracle_gp1_complete(
    GpuRenderOracleDevice *device, const GpuRenderOracleDisplayState *display);
GpuRenderOracleResult gpu_render_oracle_upload_word(
    GpuRenderOracleDevice *device);
GpuRenderOracleResult gpu_render_oracle_gpuread_word(
    GpuRenderOracleDevice *device);
GpuRenderOracleResult gpu_render_oracle_polyline_segment(
    GpuRenderOracleDevice *device, const GpuRenderOracleDrawState *draw);
GpuRenderOracleResult gpu_render_oracle_polyline_end(
    GpuRenderOracleDevice *device);

#ifdef __cplusplus
}
#endif

#endif
