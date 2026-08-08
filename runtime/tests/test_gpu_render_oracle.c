#include "gpu_render_oracle.h"
#include "../src/gpu_render_oracle_internal.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#if !defined(GPU_RENDER_ORACLE_TESTING)
#error "This synthetic contract requires GPU_RENDER_ORACLE_TESTING"
#endif

_Static_assert(GPU_RENDER_ORACLE_EVENT_METADATA_ONLY == 1,
               "events must expose normalized metadata only");
_Static_assert(GPU_RENDER_ORACLE_EVENT_CAPACITY > 1u,
               "one event slot is reserved for terminal incompleteness");
_Static_assert(sizeof(GpuRenderOracleEvent) < 512u,
               "metadata events must remain bounded");
_Static_assert(sizeof(((GpuRenderOracleDevice *)0)->events[0]) ==
                   sizeof(GpuRenderOracleEvent),
               "persistent event storage must contain only event metadata");

typedef struct {
    GpuRenderOracleDevice device;
} OracleFixture;

static int require(int condition) {
    return condition;
}

#define REQUIRE(condition) do { if (!require(condition)) return 0; } while (0)

static GpuRenderOracleDrawState draw_state(void) {
    GpuRenderOracleDrawState state = {0};

    state.texture_page_x = 5u;
    state.texture_page_y = 1u;
    state.clut_x = 17u;
    state.clut_y = 29u;
    state.semi_transparency = 2u;
    state.texture_depth = 1u;
    state.dither = 1u;
    state.texture_window_mask_x = 3u;
    state.texture_window_mask_y = 4u;
    state.texture_window_offset_x = 6u;
    state.texture_window_offset_y = 7u;
    state.draw_area_left = 12u;
    state.draw_area_top = 13u;
    state.draw_area_right = 500u;
    state.draw_area_bottom = 400u;
    state.offset_x = 4;
    state.offset_y = -3;
    state.mask_set = 1u;
    state.mask_check = 1u;
    return state;
}

static GpuRenderOracleDisplayState display_state(void) {
    GpuRenderOracleDisplayState state = {0};

    state.display_x = 320u;
    state.display_y = 16u;
    state.horizontal_start = 600u;
    state.horizontal_end = 2800u;
    state.vertical_start = 20u;
    state.vertical_end = 260u;
    state.width = 320u;
    state.height = 240u;
    state.depth24 = 0u;
    state.disabled = 0u;
    return state;
}

static GpuRenderOracleTransfer transfer_state(
    GpuRenderOracleTransferDirection direction, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint64_t expected_words) {
    GpuRenderOracleTransfer transfer = {0};

    transfer.direction = direction;
    transfer.x = x;
    transfer.y = y;
    transfer.width = width;
    transfer.height = height;
    transfer.expected_words = expected_words;
    return transfer;
}

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

static int equal_draw_state(const GpuRenderOracleDrawState *left,
                            const GpuRenderOracleDrawState *right) {
    return left->texture_page_x == right->texture_page_x &&
           left->texture_page_y == right->texture_page_y &&
           left->clut_x == right->clut_x && left->clut_y == right->clut_y &&
           left->semi_transparency == right->semi_transparency &&
           left->texture_depth == right->texture_depth &&
           left->dither == right->dither &&
           left->texture_window_mask_x == right->texture_window_mask_x &&
           left->texture_window_mask_y == right->texture_window_mask_y &&
           left->texture_window_offset_x == right->texture_window_offset_x &&
           left->texture_window_offset_y == right->texture_window_offset_y &&
           left->draw_area_left == right->draw_area_left &&
           left->draw_area_top == right->draw_area_top &&
           left->draw_area_right == right->draw_area_right &&
           left->draw_area_bottom == right->draw_area_bottom &&
           left->offset_x == right->offset_x && left->offset_y == right->offset_y &&
           left->mask_set == right->mask_set && left->mask_check == right->mask_check;
}

static int equal_display_state(const GpuRenderOracleDisplayState *left,
                               const GpuRenderOracleDisplayState *right) {
    return left->display_x == right->display_x &&
           left->display_y == right->display_y &&
           left->horizontal_start == right->horizontal_start &&
           left->horizontal_end == right->horizontal_end &&
           left->vertical_start == right->vertical_start &&
           left->vertical_end == right->vertical_end && left->width == right->width &&
           left->height == right->height && left->depth24 == right->depth24 &&
           left->disabled == right->disabled;
}

static int equal_transfer(const GpuRenderOracleTransfer *left,
                          const GpuRenderOracleTransfer *right) {
    return left->direction == right->direction && left->x == right->x &&
           left->y == right->y && left->width == right->width &&
           left->height == right->height &&
           left->expected_words == right->expected_words &&
           left->observed_words == right->observed_words &&
           left->written_pixels == right->written_pixels &&
           left->skipped_pixels == right->skipped_pixels &&
           left->wrapped == right->wrapped;
}

static int equal_source_span(const GpuRenderOracleSourceSpan *left,
                             const GpuRenderOracleSourceSpan *right) {
    return left->kind_mask == right->kind_mask &&
           left->first_word == right->first_word &&
           left->last_word == right->last_word &&
           left->first_container == right->first_container &&
           left->last_container == right->last_container &&
           left->word_count == right->word_count &&
           left->discontinuous == right->discontinuous;
}

static int equal_command_summary(const GpuRenderOracleCommandSummary *left,
                                 const GpuRenderOracleCommandSummary *right) {
    return left->opcode == right->opcode &&
           left->command_class == right->command_class &&
           left->flags == right->flags && left->word_count == right->word_count &&
           left->vertex_count == right->vertex_count &&
           left->rectangle_count == right->rectangle_count &&
           left->clut_x == right->clut_x && left->clut_y == right->clut_y &&
           left->texture_page_x == right->texture_page_x &&
           left->texture_page_y == right->texture_page_y;
}

static int same_normalized_command_facts(const GpuRenderOracleEvent *left,
                                         const GpuRenderOracleEvent *right) {
    return left->sequence == right->sequence &&
           left->vram_serial == right->vram_serial && left->kind == right->kind &&
           left->mutation == right->mutation &&
           left->incomplete_reason == right->incomplete_reason &&
           equal_command_summary(&left->command, &right->command) &&
           equal_draw_state(&left->draw, &right->draw) &&
           equal_display_state(&left->display, &right->display) &&
           equal_transfer(&left->transfer, &right->transfer);
}

static int expected_draw_command(const GpuRenderOracleEvent *event,
                                 const GpuRenderOracleDrawState *draw,
                                 uint64_t word_count) {
    return event->command.opcode == UINT8_C(0x20) &&
           event->command.command_class ==
               GPU_RENDER_ORACLE_COMMAND_CLASS_PRIMITIVE &&
           event->command.flags == UINT32_C(0x76) &&
           event->command.word_count == word_count &&
           event->command.vertex_count == 3u &&
           event->command.rectangle_count == 0u &&
           event->command.clut_x == draw->clut_x &&
           event->command.clut_y == draw->clut_y &&
           event->command.texture_page_x == draw->texture_page_x &&
           event->command.texture_page_y == draw->texture_page_y &&
           equal_draw_state(&event->draw, draw);
}

static int expected_source_span(const GpuRenderOracleEvent *event,
                                GpuRenderOracleSourceKind source,
                                uint64_t first_word, uint64_t last_word,
                                uint64_t first_container,
                                uint64_t last_container,
                                uint64_t word_count, int discontinuous) {
    GpuRenderOracleSourceSpan expected = {0};

    expected.kind_mask = source_mask(source);
    expected.first_word = first_word;
    expected.last_word = last_word;
    expected.first_container = first_container;
    expected.last_container = last_container;
    expected.word_count = word_count;
    expected.discontinuous = (uint8_t)(discontinuous != 0);
    return equal_source_span(&event->source, &expected);
}

static int same_frozen_device_except_drop_count(
    const GpuRenderOracleDevice *before, const GpuRenderOracleDevice *after,
    uint64_t expected_drops) {
    GpuRenderOracleDevice normalized = *after;

    if (after->dropped_events != expected_drops) {
        return 0;
    }
    normalized.dropped_events = before->dropped_events;
    return memcmp(before, &normalized, sizeof(normalized)) == 0;
}

static int same_frozen_device_except_drop_and_serial(
    const GpuRenderOracleDevice *before, const GpuRenderOracleDevice *after,
    uint64_t expected_drops) {
    GpuRenderOracleDevice normalized = *after;

    if (after->dropped_events != expected_drops ||
        before->global_vram_serial == UINT64_MAX ||
        after->global_vram_serial != before->global_vram_serial + 1u) {
        return 0;
    }
    normalized.dropped_events = before->dropped_events;
    normalized.global_vram_serial = before->global_vram_serial;
    return memcmp(before, &normalized, sizeof(normalized)) == 0;
}

#define REQUIRE_ONE_FROZEN_DROP(fixture, expression) do { \
    GpuRenderOracleDevice frozen_before = (fixture)->device; \
    REQUIRE((expression) == GPU_RENDER_ORACLE_RESULT_OK); \
    REQUIRE(same_frozen_device_except_drop_count( \
        &frozen_before, &(fixture)->device, frozen_before.dropped_events + 1u)); \
} while (0)

#define REQUIRE_ONE_FROZEN_MUTATION_DROP(fixture, expression) do { \
    GpuRenderOracleDevice frozen_before = (fixture)->device; \
    REQUIRE((expression) == GPU_RENDER_ORACLE_RESULT_OK); \
    REQUIRE(same_frozen_device_except_drop_and_serial( \
        &frozen_before, &(fixture)->device, frozen_before.dropped_events + 1u)); \
} while (0)

static int same_terminal_event(const GpuRenderOracleEvent *left,
                               const GpuRenderOracleEvent *right) {
    return left->sequence == right->sequence && left->kind == right->kind &&
           left->mutation == right->mutation &&
           left->vram_serial == right->vram_serial &&
           left->incomplete_reason == right->incomplete_reason &&
           left->source.kind_mask == right->source.kind_mask &&
           left->source.first_word == right->source.first_word &&
           left->source.last_word == right->source.last_word &&
           left->source.first_container == right->source.first_container &&
           left->source.last_container == right->source.last_container &&
           left->source.word_count == right->source.word_count &&
           left->source.discontinuous == right->source.discontinuous;
}

static int fixture_init(OracleFixture *fixture) {
    gpu_render_oracle_device_init(&fixture->device);
    return gpu_render_oracle_capture_set_enabled(&fixture->device, 1) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int fixture_begin(OracleFixture *fixture) {
    return gpu_render_oracle_device_capture_begin(&fixture->device) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int fixture_end(OracleFixture *fixture) {
    return gpu_render_oracle_device_capture_end(&fixture->device) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int get_event(const OracleFixture *fixture, uint64_t index,
                     GpuRenderOracleEvent *event) {
    return gpu_render_oracle_device_event_get(&fixture->device, index, event) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int get_snapshot(const OracleFixture *fixture,
                        GpuRenderOracleSnapshot *snapshot) {
    return gpu_render_oracle_device_capture_snapshot(&fixture->device, snapshot) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int begin_gp0(OracleFixture *fixture, GpuRenderOracleCommandKind command,
                     GpuRenderOracleSourceKind source) {
    return gpu_render_oracle_gp0_begin(&fixture->device, command, source) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int source_word(OracleFixture *fixture, uint64_t ordinal,
                       uint64_t container) {
    return gpu_render_oracle_gp0_source_word(&fixture->device, ordinal, container) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static int complete_gp0(OracleFixture *fixture,
                        GpuRenderOracleMutationKind mutation,
                        const GpuRenderOracleDrawState *draw,
                        const GpuRenderOracleTransfer *transfer) {
    return gpu_render_oracle_gp0_complete(&fixture->device, mutation, draw, transfer) ==
           GPU_RENDER_ORACLE_RESULT_OK;
}

static GpuRenderOraclePacket packet(uint8_t opcode, uint16_t parser_word_count,
                                    GpuRenderOraclePacketClass parser_class,
                                    int task11_family_eligible) {
    GpuRenderOraclePacket parsed = {0};

    parsed.opcode = opcode;
    parsed.parser_word_count = parser_word_count;
    parsed.parser_class = parser_class;
    parsed.task11_family_eligible = (uint8_t)(task11_family_eligible != 0);
    return parsed;
}

static int capture_draw(GpuRenderOracleSourceKind source, uint64_t first_word,
                        uint64_t container, GpuRenderOracleEvent *event) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_DRAW, source));
    REQUIRE(source_word(&fixture, first_word, container));
    REQUIRE(source_word(&fixture, first_word + 1u, container));
    REQUIRE(source_word(&fixture, first_word + 2u, container));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_DRAW, &draw, NULL));
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 0u, event));
    REQUIRE(gpu_render_oracle_event_is_metadata_only(event));
    return 1;
}

static int test_disabled_default_and_lifecycle(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleSnapshot snapshot = {0};
    GpuRenderOracleEvent event = {0};

    gpu_render_oracle_device_init(&fixture.device);
    REQUIRE(!gpu_render_oracle_capture_enabled(&fixture.device));
    REQUIRE(gpu_render_oracle_device_capture_begin(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_DISABLED);
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_DISABLED);
    REQUIRE(snapshot.event_count == 0u && snapshot.dropped_events == 0u);
    REQUIRE(gpu_render_oracle_capture_set_enabled(&fixture.device, 1) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_ACTIVE);
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_ENDED);
    REQUIRE(gpu_render_oracle_device_event_get(&fixture.device, 0u, &event) ==
            GPU_RENDER_ORACLE_RESULT_OUT_OF_RANGE);
    return 1;
}

static int test_disabled_capture_still_advances_global_vram_serial(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleSnapshot before = {0};
    GpuRenderOracleSnapshot after = {0};

    gpu_render_oracle_device_init(&fixture.device);
    REQUIRE(get_snapshot(&fixture, &before));
    gpu_render_oracle_hook_gp0_complete(
        &fixture.device, GPU_RENDER_ORACLE_MUTATION_FILL, NULL, NULL);
    REQUIRE(get_snapshot(&fixture, &after));
    REQUIRE(!after.enabled &&
            after.phase == GPU_RENDER_ORACLE_CAPTURE_PHASE_DISABLED &&
            after.event_count == 0u &&
            after.global_vram_serial == before.global_vram_serial + 1u &&
            !after.global_vram_serial_overflowed);
    return 1;
}

static int test_disabled_capture_serial_overflow_fails_closed(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleSnapshot snapshot = {0};

    gpu_render_oracle_device_init(&fixture.device);
    gpu_render_oracle_test_seed_vram_serial(&fixture.device, UINT64_MAX);
    gpu_render_oracle_hook_gp0_complete(
        &fixture.device, GPU_RENDER_ORACLE_MUTATION_COPY, NULL, NULL);
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.global_vram_serial == UINT64_MAX &&
            snapshot.global_vram_serial_overflowed &&
            snapshot.journal_frozen && snapshot.event_count == 0u &&
            snapshot.incomplete_reason ==
                GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW);
    REQUIRE(gpu_render_oracle_capture_set_enabled(&fixture.device, 1) ==
            GPU_RENDER_ORACLE_RESULT_INCOMPLETE);
    REQUIRE(gpu_render_oracle_device_capture_begin(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_INCOMPLETE);
    return 1;
}

static int test_sources_normalize_identically(void) {
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleEvent unknown = {0};
    GpuRenderOracleEvent mmio = {0};
    GpuRenderOracleEvent block = {0};
    GpuRenderOracleEvent linked = {0};
    GpuRenderOracleEvent burst = {0};

    REQUIRE(capture_draw(GPU_RENDER_ORACLE_SOURCE_UNKNOWN, 10u, 1u, &unknown));
    REQUIRE(capture_draw(GPU_RENDER_ORACLE_SOURCE_MMIO, 10u, 2u, &mmio));
    REQUIRE(capture_draw(GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, 10u, 3u, &block));
    REQUIRE(capture_draw(GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST, 10u, 4u,
                         &linked));
    REQUIRE(capture_draw(GPU_RENDER_ORACLE_SOURCE_DMA2_BURST, 10u, 5u, &burst));
    REQUIRE(same_normalized_command_facts(&mmio, &block));
    REQUIRE(same_normalized_command_facts(&mmio, &linked));
    REQUIRE(same_normalized_command_facts(&mmio, &burst));
    REQUIRE(same_normalized_command_facts(&unknown, &mmio));
    REQUIRE(expected_draw_command(&unknown, &draw, 3u));
    REQUIRE(expected_draw_command(&mmio, &draw, 3u));
    REQUIRE(expected_draw_command(&block, &draw, 3u));
    REQUIRE(expected_draw_command(&linked, &draw, 3u));
    REQUIRE(expected_draw_command(&burst, &draw, 3u));
    REQUIRE(expected_source_span(&unknown, GPU_RENDER_ORACLE_SOURCE_UNKNOWN,
                                 10u, 12u, 1u, 1u, 3u, 0));
    REQUIRE(expected_source_span(&mmio, GPU_RENDER_ORACLE_SOURCE_MMIO,
                                 10u, 12u, 2u, 2u, 3u, 0));
    REQUIRE(expected_source_span(&block, GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                                 10u, 12u, 3u, 3u, 3u, 0));
    REQUIRE(expected_source_span(&linked,
                                 GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
                                 10u, 12u, 4u, 4u, 3u, 0));
    REQUIRE(expected_source_span(&burst, GPU_RENDER_ORACLE_SOURCE_DMA2_BURST,
                                 10u, 12u, 5u, 5u, 3u, 0));
    return 1;
}

static int test_discontinuous_source_containers(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleEvent event = {0};
    GpuRenderOracleDrawState draw = draw_state();

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_DRAW,
                      GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST));
    REQUIRE(source_word(&fixture, 100u, 40u));
    REQUIRE(source_word(&fixture, 101u, 40u));
    REQUIRE(source_word(&fixture, 102u, 97u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_DRAW, &draw, NULL));
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 0u, &event));
    REQUIRE(event.source.kind_mask ==
            source_mask(GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST));
    REQUIRE(event.source.first_word == 100u && event.source.last_word == 102u);
    REQUIRE(event.source.first_container == 40u && event.source.last_container == 97u);
    REQUIRE(event.source.word_count == 3u && event.source.discontinuous);
    return 1;
}

static int test_e1_to_e6_draw_state_and_mutations(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState state = draw_state();
    GpuRenderOracleEvent draw_event = {0};
    GpuRenderOracleEvent fill_event = {0};
    GpuRenderOracleEvent copy_event = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    for (uint32_t command = GPU_RENDER_ORACLE_COMMAND_ENV_E1;
         command <= GPU_RENDER_ORACLE_COMMAND_ENV_E6; ++command) {
        REQUIRE(begin_gp0(&fixture, (GpuRenderOracleCommandKind)command,
                          GPU_RENDER_ORACLE_SOURCE_MMIO));
        REQUIRE(source_word(&fixture, command, 1u));
        REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_NONE, &state,
                             NULL));
    }
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_DRAW,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 40u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_DRAW, &state, NULL));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_FILL,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 41u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_FILL, &state, NULL));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_COPY,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 42u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_COPY, &state, NULL));
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 6u, &draw_event));
    REQUIRE(get_event(&fixture, 7u, &fill_event));
    REQUIRE(get_event(&fixture, 8u, &copy_event));
    REQUIRE(draw_event.kind == GPU_RENDER_ORACLE_EVENT_GP0_COMMAND &&
            draw_event.mutation == GPU_RENDER_ORACLE_MUTATION_DRAW);
    REQUIRE(fill_event.kind == GPU_RENDER_ORACLE_EVENT_GP0_COMMAND &&
            fill_event.mutation == GPU_RENDER_ORACLE_MUTATION_FILL);
    REQUIRE(copy_event.kind == GPU_RENDER_ORACLE_EVENT_GP0_COMMAND &&
            copy_event.mutation == GPU_RENDER_ORACLE_MUTATION_COPY);
    REQUIRE(equal_draw_state(&draw_event.draw, &state));
    REQUIRE(draw_event.vram_serial < fill_event.vram_serial &&
            fill_event.vram_serial < copy_event.vram_serial);
    return 1;
}

static int test_gp1_follows_prior_mutations(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleDisplayState display = display_state();
    GpuRenderOracleEvent draw_event = {0};
    GpuRenderOracleEvent fill_event = {0};
    GpuRenderOracleEvent copy_event = {0};
    GpuRenderOracleEvent gp1_event = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_DRAW,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 1u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_DRAW, &draw, NULL));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_FILL,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 2u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_FILL, &draw, NULL));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_COPY,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 3u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_COPY, &draw, NULL));
    REQUIRE(gpu_render_oracle_gp1_complete(&fixture.device, &display) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 0u, &draw_event));
    REQUIRE(get_event(&fixture, 1u, &fill_event));
    REQUIRE(get_event(&fixture, 2u, &copy_event));
    REQUIRE(get_event(&fixture, 3u, &gp1_event));
    REQUIRE(gp1_event.kind == GPU_RENDER_ORACLE_EVENT_GP1);
    REQUIRE(draw_event.sequence < fill_event.sequence &&
            fill_event.sequence < copy_event.sequence &&
            copy_event.sequence < gp1_event.sequence);
    REQUIRE(copy_event.vram_serial == gp1_event.vram_serial);
    REQUIRE(equal_display_state(&gp1_event.display, &display));
    return 1;
}

static int test_upload_counts_without_payload(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleTransfer odd = transfer_state(
        GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM, 12u, 13u, 3u, 1u, 2u);
    GpuRenderOracleTransfer wrapped = transfer_state(
        GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM, 1023u, 511u, 3u, 2u, 3u);
    GpuRenderOracleEvent odd_begin = {0};
    GpuRenderOracleEvent odd_event = {0};
    GpuRenderOracleEvent wrapped_begin = {0};
    GpuRenderOracleEvent wrapped_event = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_UPLOAD,
                      GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK));
    REQUIRE(source_word(&fixture, 20u, 2u));
    REQUIRE(gpu_render_oracle_upload_word(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_upload_word(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_UPLOAD, &draw, &odd));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_UPLOAD,
                      GPU_RENDER_ORACLE_SOURCE_DMA2_BURST));
    REQUIRE(source_word(&fixture, 30u, 3u));
    REQUIRE(gpu_render_oracle_upload_word(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_upload_word(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_upload_word(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_UPLOAD, &draw,
                         &wrapped));
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 0u, &odd_begin));
    REQUIRE(get_event(&fixture, 1u, &odd_event));
    REQUIRE(get_event(&fixture, 2u, &wrapped_begin));
    REQUIRE(get_event(&fixture, 3u, &wrapped_event));
    REQUIRE(odd_begin.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_BEGIN &&
            wrapped_begin.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_BEGIN);
    REQUIRE(odd_event.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_END &&
            odd_event.mutation == GPU_RENDER_ORACLE_MUTATION_UPLOAD);
    REQUIRE(odd_event.transfer.expected_words == 2u &&
            odd_event.transfer.observed_words == 2u &&
            odd_event.transfer.written_pixels == 3u &&
            odd_event.transfer.skipped_pixels == 0u && !odd_event.transfer.wrapped);
    REQUIRE(wrapped_event.transfer.expected_words == 3u &&
            wrapped_event.transfer.observed_words == 3u &&
            wrapped_event.transfer.written_pixels == 6u &&
            wrapped_event.transfer.skipped_pixels == 0u && wrapped_event.transfer.wrapped);
    REQUIRE(gpu_render_oracle_event_is_metadata_only(&odd_event));
    REQUIRE(gpu_render_oracle_event_is_metadata_only(&wrapped_event));
    return 1;
}

static int test_gpuread_counts_without_payload(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleTransfer readback = transfer_state(
        GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU, 40u, 41u, 5u, 1u, 3u);
    GpuRenderOracleEvent begin_event = {0};
    GpuRenderOracleEvent event = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_READBACK,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 60u, 1u));
    REQUIRE(gpu_render_oracle_gpuread_word(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_gpuread_word(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_gpuread_word(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_NONE, &draw,
                         &readback));
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 0u, &begin_event));
    REQUIRE(get_event(&fixture, 1u, &event));
    REQUIRE(begin_event.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_BEGIN &&
            event.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_END);
    REQUIRE(event.transfer.direction == GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU);
    REQUIRE(event.transfer.expected_words == 3u && event.transfer.observed_words == 3u &&
            event.transfer.written_pixels == 0u && event.transfer.skipped_pixels == 0u);
    REQUIRE(gpu_render_oracle_event_is_metadata_only(&event));
    return 1;
}

static int test_polyline_segments_and_abort(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleEvent begin_event = {0};
    GpuRenderOracleEvent segment_a = {0};
    GpuRenderOracleEvent segment_b = {0};
    GpuRenderOracleEvent end_event = {0};
    GpuRenderOracleEvent abort_event = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_POLYLINE,
                      GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST));
    REQUIRE(source_word(&fixture, 70u, 10u));
    REQUIRE(gpu_render_oracle_polyline_segment(&fixture.device, &draw) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_polyline_segment(&fixture.device, &draw) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_polyline_end(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_UPLOAD,
                      GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK));
    REQUIRE(source_word(&fixture, 80u, 11u));
    REQUIRE(gpu_render_oracle_gp0_abort(&fixture.device) == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_event(&fixture, 0u, &begin_event));
    REQUIRE(get_event(&fixture, 1u, &segment_a));
    REQUIRE(get_event(&fixture, 2u, &segment_b));
    REQUIRE(get_event(&fixture, 3u, &end_event));
    REQUIRE(get_event(&fixture, 5u, &abort_event));
    REQUIRE(begin_event.kind == GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_BEGIN);
    REQUIRE(segment_a.kind == GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_SEGMENT);
    REQUIRE(segment_b.kind == GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_SEGMENT);
    REQUIRE(end_event.kind == GPU_RENDER_ORACLE_EVENT_GP0_POLYLINE_END);
    REQUIRE(abort_event.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_ABORT);
    REQUIRE(begin_event.sequence < segment_a.sequence &&
            segment_a.sequence < segment_b.sequence && segment_b.sequence < end_event.sequence);
    return 1;
}

static int test_open_operation_and_capacity_terminal_event(void) {
    OracleFixture fixture = {0};
    OracleFixture overflow = {0};
    GpuRenderOracleEvent open_event = {0};
    GpuRenderOracleEvent terminal = {0};
    GpuRenderOracleEvent terminal_again = {0};
    GpuRenderOracleSnapshot snapshot = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_DRAW,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 90u, 1u));
    REQUIRE(gpu_render_oracle_device_capture_end(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_INCOMPLETE);
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_OPEN_OPERATION);
    REQUIRE(get_event(&fixture, snapshot.event_count - 1u, &open_event));
    REQUIRE(open_event.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE &&
            open_event.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_OPEN_OPERATION);

    REQUIRE(fixture_init(&overflow));
    REQUIRE(fixture_begin(&overflow));
    for (uint64_t index = 0u; index < GPU_RENDER_ORACLE_EVENT_CAPACITY + 1u; ++index) {
        REQUIRE(gpu_render_oracle_gp1_complete(&overflow.device, NULL) ==
                GPU_RENDER_ORACLE_RESULT_OK);
    }
    REQUIRE(fixture_end(&overflow));
    REQUIRE(get_snapshot(&overflow, &snapshot));
    REQUIRE(snapshot.event_count == GPU_RENDER_ORACLE_EVENT_CAPACITY);
    REQUIRE(snapshot.dropped_events == 2u);
    REQUIRE(get_event(&overflow, GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u, &terminal));
    REQUIRE(get_event(&overflow, GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u,
                      &terminal_again));
    REQUIRE(terminal.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE &&
            terminal.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY &&
            same_terminal_event(&terminal, &terminal_again));
    return 1;
}

static int test_capacity_drop_accounting_and_frozen_ingestion_noops(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleSnapshot snapshot = {0};
    GpuRenderOracleEvent terminal = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleDisplayState display = display_state();

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    for (uint64_t index = 0u; index < GPU_RENDER_ORACLE_EVENT_CAPACITY; ++index) {
        REQUIRE(gpu_render_oracle_gp1_complete(&fixture.device, NULL) ==
                GPU_RENDER_ORACLE_RESULT_OK);
    }
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.event_count == GPU_RENDER_ORACLE_EVENT_CAPACITY);
    REQUIRE(snapshot.dropped_events == 1u);
    REQUIRE(snapshot.journal_frozen);
    REQUIRE(get_event(&fixture, GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u, &terminal));
    REQUIRE(terminal.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE);
    REQUIRE(terminal.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY);

    display.display_x = 999u;
    draw.texture_page_x = 999u;
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_gp1_complete(&fixture.device,
                                                           &display));
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_gp0_begin(
                                &fixture.device, GPU_RENDER_ORACLE_COMMAND_DRAW,
                                GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_gp0_source_word(&fixture.device,
                                                              99u, 99u));
    REQUIRE_ONE_FROZEN_MUTATION_DROP(
        &fixture, gpu_render_oracle_gp0_complete(
                      &fixture.device, GPU_RENDER_ORACLE_MUTATION_DRAW,
                      &draw, NULL));
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_gp0_abort(&fixture.device));
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_upload_word(&fixture.device));
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_gpuread_word(&fixture.device));
    REQUIRE_ONE_FROZEN_MUTATION_DROP(
        &fixture, gpu_render_oracle_polyline_segment(&fixture.device, &draw));
    REQUIRE_ONE_FROZEN_DROP(&fixture,
                            gpu_render_oracle_polyline_end(&fixture.device));
    REQUIRE(get_snapshot(&fixture, &snapshot));
    REQUIRE(snapshot.dropped_events == 10u);
    REQUIRE(get_event(&fixture, GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u, &terminal));
    REQUIRE(terminal.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE);
    REQUIRE(terminal.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_CAPACITY);
    REQUIRE(fixture_end(&fixture));
    return 1;
}

static int test_rejected_transfer_is_observationally_inert(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState rejected_draw = draw_state();
    GpuRenderOracleDrawState accepted_draw = draw_state();
    GpuRenderOracleTransfer rejected = transfer_state(
        GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU, 1u, 2u, 3u, 4u, 6u);
    GpuRenderOracleTransfer accepted = transfer_state(
        GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM, 1u, 2u, 3u, 4u, 6u);
    GpuRenderOracleDevice before;

    rejected_draw.texture_page_x = 777u;
    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_UPLOAD,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 1u, 1u));
    before = fixture.device;
    REQUIRE(gpu_render_oracle_gp0_complete(&fixture.device,
                                            GPU_RENDER_ORACLE_MUTATION_UPLOAD,
                                            &rejected_draw, &rejected) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_STATE);
    REQUIRE(memcmp(&before, &fixture.device, sizeof(before)) == 0);
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_UPLOAD,
                         &accepted_draw, &accepted));
    REQUIRE(fixture_end(&fixture));
    return 1;
}

static int test_metadata_only_predicate_rejects_uninitialized_event(void) {
    GpuRenderOracleEvent event = {0};

    REQUIRE(!gpu_render_oracle_event_is_metadata_only(&event));
    return 1;
}

static int test_parsed_packet_metadata_freezes_ineligible(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOraclePacket textured = packet(
        UINT8_C(0x24), UINT16_C(7),
        GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED, 1);
    GpuRenderOraclePacket malformed = packet(
        UINT8_C(0xff), 0u, GPU_RENDER_ORACLE_PACKET_CLASS_MALFORMED, 0);
    GpuRenderOracleEvent terminal = {0};
    GpuRenderOracleDevice frozen_before;

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    for (uint64_t index = 0u; index < GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u;
         ++index) {
        REQUIRE(gpu_render_oracle_gp1_complete(&fixture.device, NULL) ==
                GPU_RENDER_ORACLE_RESULT_OK);
    }
    REQUIRE(gpu_render_oracle_gp0_begin_parsed(
                &fixture.device, GPU_RENDER_ORACLE_COMMAND_DRAW,
                GPU_RENDER_ORACLE_SOURCE_MMIO, &textured) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(source_word(&fixture, 1u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_DRAW, &draw, NULL));
    REQUIRE(get_event(&fixture, GPU_RENDER_ORACLE_EVENT_CAPACITY - 1u, &terminal));
    REQUIRE(terminal.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE &&
            terminal.packet.opcode == UINT8_C(0x24) &&
            terminal.packet.parser_word_count == UINT16_C(7) &&
            !terminal.packet.task11_family_eligible);
    frozen_before = fixture.device;
    REQUIRE(gpu_render_oracle_gp0_begin_parsed(
                &fixture.device, GPU_RENDER_ORACLE_COMMAND_DRAW,
                GPU_RENDER_ORACLE_SOURCE_MMIO, &malformed) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(same_frozen_device_except_drop_count(
        &frozen_before, &fixture.device, frozen_before.dropped_events + 1u));
    REQUIRE(fixture_end(&fixture));
    return 1;
}

static int test_counter_and_serial_overflow(void) {
    OracleFixture counter = {0};
    OracleFixture serial = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleEvent event = {0};
    GpuRenderOracleSnapshot snapshot = {0};

    REQUIRE(fixture_init(&counter));
    REQUIRE(fixture_begin(&counter));
    gpu_render_oracle_test_seed_next_sequence(&counter.device, UINT64_MAX);
    REQUIRE(gpu_render_oracle_gp1_complete(&counter.device, NULL) ==
            GPU_RENDER_ORACLE_RESULT_INCOMPLETE);
    REQUIRE(get_snapshot(&counter, &snapshot));
    REQUIRE(snapshot.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW);
    REQUIRE(get_event(&counter, 0u, &event));
    REQUIRE(event.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE &&
            event.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_COUNTER_OVERFLOW);

    REQUIRE(fixture_init(&serial));
    REQUIRE(fixture_begin(&serial));
    gpu_render_oracle_test_seed_vram_serial(&serial.device, UINT64_MAX);
    REQUIRE(begin_gp0(&serial, GPU_RENDER_ORACLE_COMMAND_FILL,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&serial, 100u, 1u));
    REQUIRE(complete_gp0(&serial, GPU_RENDER_ORACLE_MUTATION_FILL, &draw, NULL) == 0);
    REQUIRE(get_snapshot(&serial, &snapshot));
    REQUIRE(snapshot.global_vram_serial == UINT64_MAX &&
            snapshot.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW);
    REQUIRE(get_event(&serial, 0u, &event));
    REQUIRE(event.kind == GPU_RENDER_ORACLE_EVENT_INCOMPLETE &&
            event.incomplete_reason == GPU_RENDER_ORACLE_INCOMPLETE_SERIAL_OVERFLOW);
    return 1;
}

static int test_disable_begin_preserves_global_vram_serial(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleDrawState draw = draw_state();
    GpuRenderOracleSnapshot before = {0};
    GpuRenderOracleSnapshot disabled = {0};
    GpuRenderOracleSnapshot restarted = {0};

    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(begin_gp0(&fixture, GPU_RENDER_ORACLE_COMMAND_FILL,
                      GPU_RENDER_ORACLE_SOURCE_MMIO));
    REQUIRE(source_word(&fixture, 110u, 1u));
    REQUIRE(complete_gp0(&fixture, GPU_RENDER_ORACLE_MUTATION_FILL, &draw, NULL));
    REQUIRE(fixture_end(&fixture));
    REQUIRE(get_snapshot(&fixture, &before));
    REQUIRE(before.global_vram_serial > 0u);
    REQUIRE(gpu_render_oracle_capture_set_enabled(&fixture.device, 0) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_device_capture_begin(&fixture.device) ==
            GPU_RENDER_ORACLE_RESULT_DISABLED);
    REQUIRE(get_snapshot(&fixture, &disabled));
    REQUIRE(disabled.global_vram_serial == before.global_vram_serial);
    gpu_render_oracle_hook_gp0_complete(
        &fixture.device, GPU_RENDER_ORACLE_MUTATION_DRAW, NULL, NULL);
    REQUIRE(get_snapshot(&fixture, &disabled));
    REQUIRE(disabled.global_vram_serial == before.global_vram_serial + 1u);
    REQUIRE(gpu_render_oracle_capture_set_enabled(&fixture.device, 1) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(get_snapshot(&fixture, &restarted));
    REQUIRE(restarted.global_vram_serial == before.global_vram_serial + 1u);
    return 1;
}

static int test_invalid_null_and_out_of_range_arguments(void) {
    OracleFixture fixture = {0};
    GpuRenderOracleSnapshot snapshot = {0};
    GpuRenderOracleEvent event = {0};

    REQUIRE(gpu_render_oracle_capture_set_enabled(NULL, 1) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_device_capture_begin(NULL) == GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_device_capture_end(NULL) == GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_device_capture_snapshot(NULL, &snapshot) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_device_capture_snapshot(&fixture.device, NULL) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_device_event_get(NULL, 0u, &event) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_device_event_get(&fixture.device, 0u, NULL) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_gp0_source_word(NULL, 0u, 0u) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(gpu_render_oracle_gp0_complete(NULL, GPU_RENDER_ORACLE_MUTATION_NONE,
                                           NULL, NULL) ==
            GPU_RENDER_ORACLE_RESULT_INVALID_ARGUMENT);
    REQUIRE(fixture_init(&fixture));
    REQUIRE(fixture_begin(&fixture));
    REQUIRE(gpu_render_oracle_device_event_get(&fixture.device,
                                        GPU_RENDER_ORACLE_EVENT_CAPACITY, &event) ==
            GPU_RENDER_ORACLE_RESULT_OUT_OF_RANGE);
    REQUIRE(fixture_end(&fixture));
    return 1;
}

int main(void) {
    int passed = 1;

    passed &= test_disabled_default_and_lifecycle();
    passed &= test_disabled_capture_still_advances_global_vram_serial();
    passed &= test_disabled_capture_serial_overflow_fails_closed();
    passed &= test_sources_normalize_identically();
    passed &= test_discontinuous_source_containers();
    passed &= test_e1_to_e6_draw_state_and_mutations();
    passed &= test_gp1_follows_prior_mutations();
    passed &= test_upload_counts_without_payload();
    passed &= test_gpuread_counts_without_payload();
    passed &= test_polyline_segments_and_abort();
    passed &= test_open_operation_and_capacity_terminal_event();
    passed &= test_capacity_drop_accounting_and_frozen_ingestion_noops();
    passed &= test_rejected_transfer_is_observationally_inert();
    passed &= test_counter_and_serial_overflow();
    passed &= test_disable_begin_preserves_global_vram_serial();
    passed &= test_metadata_only_predicate_rejects_uninitialized_event();
    passed &= test_parsed_packet_metadata_freezes_ineligible();
    passed &= test_invalid_null_and_out_of_range_arguments();
    return passed ? 0 : 1;
}
