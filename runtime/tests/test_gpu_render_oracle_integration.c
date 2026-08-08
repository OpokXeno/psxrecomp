#include "gpu_render_oracle.h"

#include "dma.h"
#include "gpu.h"
#include "guest_render_native_stream.h"

#include <stdint.h>
#include <string.h>

#ifndef GPU_RENDER_ORACLE_SOURCE_CONTRACT
typedef struct {
    GpuRenderOracleSourceKind kind;
    uint64_t word_ordinal;
    uint64_t container_ordinal;
} GpuRenderOracleSource;
#endif

GpuRenderOracleResult gpu_render_oracle_capture_begin(void);
GpuRenderOracleResult gpu_render_oracle_capture_end(void);
GpuRenderOracleResult gpu_render_oracle_capture_snapshot(GpuRenderOracleSnapshot *out);
GpuRenderOracleResult gpu_render_oracle_capture_read_event(uint64_t index,
                                                            GpuRenderOracleEvent *out);
extern void psx_write_word(uint32_t address, uint32_t value);

void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                        uint64_t next_generation,
                                        uint32_t address, uint32_t size) {
    (void)previous_generation;
    (void)next_generation;
    (void)address;
    (void)size;
}

static void (*const structured_source_setter)(const GpuRenderOracleSource *) =
    gpu_set_gp0_source;

_Static_assert(sizeof(((GpuRenderOracleSource *)0)->word_ordinal) == sizeof(uint64_t),
               "source word ordinals must stay 64-bit");
_Static_assert(sizeof(((GpuRenderOracleTransfer *)0)->expected_words) == sizeof(uint64_t),
               "transfer counts must stay 64-bit");
_Static_assert(sizeof(((GpuRenderOracleTransfer *)0)->observed_words) == sizeof(uint64_t),
               "observed transfer counts must stay 64-bit");

enum {
    GP0_MMIO = 0x1f801810u,
    GP1_MMIO = 0x1f801814u,
    DMA2_MADR = 0x1f8010a0u,
    DMA2_BCR = 0x1f8010a4u,
    DMA2_CHCR = 0x1f8010a8u,
    DMA_DPCR = 0x1f8010f0u,
    DMA2_ENABLE = 1u << 11,
    DMA2_BLOCK = 0x01000201u,
    DMA2_LINKED = 0x01000401u,
    DMA2_BURST = 0x01000001u,
    BLOCK_BASE = 0x1000u,
    BURST_BASE = 0x1100u,
    LINKED_FIRST = 0x1200u,
    LINKED_SECOND = 0x1300u,
    FOLLOWUP_BASE = 0x3000u,
};

typedef struct {
    uint64_t vram_fingerprint;
    GpuDisplayInfo display;
    uint32_t gpuread;
    uint32_t gpustat;
} GuestSurface;

typedef struct {
    uint64_t gp0;
    uint64_t gp1;
    uint64_t nop;
    uint64_t fill;
    uint64_t draw;
    uint64_t env;
    uint64_t copy;
} CounterState;

typedef struct {
    GuestSurface guest;
    CounterState delta;
    uint64_t initial_vram_serial;
    GpuRenderOracleEvent upload;
    GpuRenderOracleEvent readback;
    GpuRenderOracleEvent first_gp1;
    GpuRenderOracleEvent dma_gp1;
    GpuRenderOracleEvent last_gp1;
    GpuRenderOracleSnapshot snapshot;
} FullRun;

typedef enum {
    ROUTE_MMIO,
    ROUTE_DMA2_BLOCK,
    ROUTE_DMA2_LINKED,
    ROUTE_DMA2_BURST,
} Route;

static int require(int condition) { return condition; }
#define REQUIRE(condition) do { if (!require(condition)) return 0; } while (0)

static uint32_t xy(uint32_t x, uint32_t y) { return x | (y << 16); }

static uint64_t vram_fingerprint(void) {
    const uint16_t *pixel = gpu_get_vram();
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t remaining = 1024u * 512u;

    while (remaining-- != 0u) {
        hash ^= *pixel++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static GuestSurface guest_surface(void) {
    GuestSurface surface = {0};

    surface.vram_fingerprint = vram_fingerprint();
    gpu_get_display_info(&surface.display);
    surface.gpuread = gpu_read_gpuread();
    surface.gpustat = gpu_read_gpustat();
    return surface;
}

static CounterState counters(void) {
    CounterState state = {0};

    state.gp0 = gpu_get_gp0_count();
    state.gp1 = gpu_get_gp1_count();
    gpu_get_gp0_stats(&state.nop, &state.fill, &state.draw, &state.env, &state.copy);
    return state;
}

static CounterState counter_delta(const CounterState *before, const CounterState *after) {
    CounterState delta = {
        after->gp0 - before->gp0, after->gp1 - before->gp1,
        after->nop - before->nop, after->fill - before->fill,
        after->draw - before->draw, after->env - before->env,
        after->copy - before->copy,
    };
    return delta;
}

static void mmio_gp0(uint32_t value) { psx_write_word(GP0_MMIO, value); }
static void mmio_gp1(uint32_t value) { psx_write_word(GP1_MMIO, value); }
static void dma_feed(uint32_t address, uint32_t words, uint32_t control);

static void environment_mmio(void) {
    mmio_gp0(UINT32_C(0xe10000c9)); mmio_gp0(UINT32_C(0xe2001020));
    mmio_gp0(UINT32_C(0xe3001003)); mmio_gp0(UINT32_C(0xe4002408));
    mmio_gp0(UINT32_C(0xe53ff802)); mmio_gp0(UINT32_C(0xe6000003));
}

static void triangle_mmio(void) {
    mmio_gp0(UINT32_C(0x20ffffff)); mmio_gp0(xy(32u, 32u));
    mmio_gp0(xy(40u, 32u)); mmio_gp0(xy(32u, 40u));
}

static void textured_triangle_mmio(void) {
    mmio_gp0(UINT32_C(0x24ffffff)); mmio_gp0(xy(48u, 48u));
    mmio_gp0(0u); mmio_gp0(xy(56u, 48u));
    mmio_gp0(0u); mmio_gp0(xy(48u, 56u)); mmio_gp0(0u);
}

static void textured_quad_mmio(void) {
    mmio_gp0(UINT32_C(0x2cffffff)); mmio_gp0(xy(64u, 64u));
    mmio_gp0(0u); mmio_gp0(xy(72u, 64u)); mmio_gp0(0u);
    mmio_gp0(xy(64u, 72u)); mmio_gp0(0u); mmio_gp0(xy(72u, 72u));
    mmio_gp0(0u);
}

static void write_textured_triangle(uint32_t address) {
    psx_write_word(address + 0u, UINT32_C(0x24ffffff));
    psx_write_word(address + 4u, xy(48u, 48u));
    psx_write_word(address + 8u, 0u);
    psx_write_word(address + 12u, xy(56u, 48u));
    psx_write_word(address + 16u, 0u);
    psx_write_word(address + 20u, xy(48u, 56u));
    psx_write_word(address + 24u, 0u);
}

static void linked_textured_triangle(void) {
    psx_write_word(LINKED_FIRST, UINT32_C(0x04001300));
    psx_write_word(LINKED_FIRST + 4u, UINT32_C(0x24ffffff));
    psx_write_word(LINKED_FIRST + 8u, xy(48u, 48u));
    psx_write_word(LINKED_FIRST + 12u, 0u);
    psx_write_word(LINKED_FIRST + 16u, xy(56u, 48u));
    psx_write_word(LINKED_SECOND, UINT32_C(0x03ffffff));
    psx_write_word(LINKED_SECOND + 4u, 0u);
    psx_write_word(LINKED_SECOND + 8u, xy(48u, 56u));
    psx_write_word(LINKED_SECOND + 12u, 0u);
    dma_feed(LINKED_FIRST, 0u, DMA2_LINKED);
}

static void write_environment_triangle(uint32_t address) {
    psx_write_word(address + 0u, UINT32_C(0xe10000c9)); psx_write_word(address + 4u, UINT32_C(0xe2001020));
    psx_write_word(address + 8u, UINT32_C(0xe3001003)); psx_write_word(address + 12u, UINT32_C(0xe4002408));
    psx_write_word(address + 16u, UINT32_C(0xe53ff802)); psx_write_word(address + 20u, UINT32_C(0xe6000003));
    psx_write_word(address + 24u, UINT32_C(0x20ffffff)); psx_write_word(address + 28u, xy(32u, 32u));
    psx_write_word(address + 32u, xy(40u, 32u)); psx_write_word(address + 36u, xy(32u, 40u));
}

static void dma_feed(uint32_t address, uint32_t words, uint32_t control) {
    dma_init();
    dma_write(DMA_DPCR, DMA2_ENABLE);
    dma_write(DMA2_MADR, address);
    dma_write(DMA2_BCR, control == DMA2_BLOCK ? words | (UINT32_C(1) << 16) : words);
    dma_write(DMA2_CHCR, control);
}

static void linked_environment_triangle(void) {
    psx_write_word(LINKED_FIRST, UINT32_C(0x08001300));
    write_environment_triangle(LINKED_FIRST + 4u);
    psx_write_word(LINKED_SECOND, UINT32_C(0x02ffffff));
    psx_write_word(LINKED_SECOND + 4u, xy(40u, 32u));
    psx_write_word(LINKED_SECOND + 8u, xy(32u, 40u));
    dma_feed(LINKED_FIRST, 0u, DMA2_LINKED);
}

static void route_environment_triangle(Route route) {
    switch (route) {
    case ROUTE_MMIO:
        environment_mmio(); triangle_mmio();
        break;
    case ROUTE_DMA2_BLOCK:
        write_environment_triangle(BLOCK_BASE); dma_feed(BLOCK_BASE, 10u, DMA2_BLOCK);
        break;
    case ROUTE_DMA2_LINKED:
        linked_environment_triangle();
        break;
    case ROUTE_DMA2_BURST:
        write_environment_triangle(BURST_BASE); dma_feed(BURST_BASE, 10u, DMA2_BURST);
        break;
    }
}

static void full_mmio_script(void) {
    environment_mmio(); triangle_mmio();
    mmio_gp0(UINT32_C(0x020000ff)); mmio_gp0(xy(8u, 8u)); mmio_gp0(xy(2u, 2u));
    mmio_gp0(UINT32_C(0x80000000)); mmio_gp0(xy(8u, 8u)); mmio_gp0(xy(12u, 8u)); mmio_gp0(xy(2u, 2u));
    mmio_gp0(UINT32_C(0xa0000000)); mmio_gp0(xy(20u, 20u)); mmio_gp0(xy(3u, 1u));
    mmio_gp0(UINT32_C(0x22221111)); mmio_gp0(UINT32_C(0x00003333));
    mmio_gp0(UINT32_C(0xc0000000)); mmio_gp0(xy(20u, 20u)); mmio_gp0(xy(3u, 1u));
    (void)gpu_read_gpuread(); (void)gpu_read_gpuread();
    mmio_gp1(UINT32_C(0x03000001)); mmio_gp1(UINT32_C(0x04000002)); mmio_gp1(UINT32_C(0x0500402a));
    mmio_gp1(UINT32_C(0x06120020)); mmio_gp1(UINT32_C(0x0703c014)); mmio_gp1(UINT32_C(0x08000001)); mmio_gp1(UINT32_C(0x03000000));
}

static int full_run(int capture, FullRun *out) {
    CounterState before;
    CounterState after;
    GpuRenderOracleSnapshot initial_oracle;

    memset(out, 0, sizeof(*out));
    gpu_init();
    dma_init();
    REQUIRE(gpu_render_oracle_capture_snapshot(&initial_oracle) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    out->initial_vram_serial = initial_oracle.global_vram_serial;
    before = counters();
    if (capture)
        REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    full_mmio_script();
    out->guest = guest_surface();
    after = counters();
    out->delta = counter_delta(&before, &after);
    REQUIRE(gpu_render_oracle_capture_snapshot(&out->snapshot) == GPU_RENDER_ORACLE_RESULT_OK);
    if (!capture)
        return out->snapshot.event_count == 0u;
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(10u, &out->upload) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(12u, &out->readback) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(13u, &out->first_gp1) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(14u, &out->dma_gp1) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(19u, &out->last_gp1) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    return 1;
}

static int source_draw(Route route, GpuRenderOracleEvent *event) {
    gpu_init();
    dma_init();
    REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    route_environment_triangle(route);
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    return gpu_render_oracle_capture_read_event(6u, event) == GPU_RENDER_ORACLE_RESULT_OK;
}

static int source_textured_draw(int quad, GpuRenderOracleEvent *event) {
    gpu_init();
    dma_init();
    REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    if (quad)
        textured_quad_mmio();
    else
        textured_triangle_mmio();
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    return gpu_render_oracle_capture_read_event(0u, event) == GPU_RENDER_ORACLE_RESULT_OK;
}

static int source_textured_triangle_route(Route route, GpuRenderOracleEvent *event) {
    gpu_init();
    dma_init();
    REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    switch (route) {
    case ROUTE_MMIO:
        textured_triangle_mmio();
        break;
    case ROUTE_DMA2_BLOCK:
        write_textured_triangle(BLOCK_BASE); dma_feed(BLOCK_BASE, 7u, DMA2_BLOCK);
        break;
    case ROUTE_DMA2_LINKED:
        linked_textured_triangle();
        break;
    case ROUTE_DMA2_BURST:
        write_textured_triangle(BURST_BASE); dma_feed(BURST_BASE, 7u, DMA2_BURST);
        break;
    }
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    return gpu_render_oracle_capture_read_event(0u, event) == GPU_RENDER_ORACLE_RESULT_OK;
}

static int same_draw_facts(const GpuRenderOracleEvent *left,
                           const GpuRenderOracleEvent *right) {
    return left->kind == right->kind && left->mutation == right->mutation &&
           memcmp(&left->command, &right->command, sizeof(left->command)) == 0 &&
           memcmp(&left->draw, &right->draw, sizeof(left->draw)) == 0;
}

static int test_capture_preserves_guest_outputs(void) {
    FullRun disabled;
    FullRun enabled;

    REQUIRE(full_run(0, &disabled));
    REQUIRE(full_run(1, &enabled));
    REQUIRE(memcmp(&disabled.guest, &enabled.guest, sizeof(disabled.guest)) == 0);
    REQUIRE(memcmp(&disabled.delta, &enabled.delta, sizeof(disabled.delta)) == 0);
    REQUIRE(disabled.snapshot.global_vram_serial ==
                disabled.initial_vram_serial + 4u &&
            enabled.snapshot.global_vram_serial ==
                enabled.initial_vram_serial + 4u &&
            !disabled.snapshot.global_vram_serial_overflowed &&
            !enabled.snapshot.global_vram_serial_overflowed);
    REQUIRE(enabled.snapshot.event_count == 20u);
    REQUIRE(enabled.upload.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_END && enabled.upload.transfer.direction == GPU_RENDER_ORACLE_TRANSFER_CPU_TO_VRAM && enabled.upload.transfer.expected_words == UINT64_C(2) && enabled.upload.transfer.observed_words == UINT64_C(2) && enabled.upload.transfer.written_pixels == UINT64_C(3));
    REQUIRE(enabled.readback.kind == GPU_RENDER_ORACLE_EVENT_TRANSFER_END && enabled.readback.transfer.direction == GPU_RENDER_ORACLE_TRANSFER_VRAM_TO_CPU && enabled.readback.transfer.expected_words == UINT64_C(2) && enabled.readback.transfer.observed_words == UINT64_C(2) && enabled.readback.transfer.written_pixels == 0u);
    REQUIRE(enabled.readback.sequence < enabled.first_gp1.sequence && enabled.first_gp1.display.disabled == 1u && enabled.first_gp1.sequence < enabled.dma_gp1.sequence && enabled.dma_gp1.display.disabled == 1u && enabled.dma_gp1.sequence < enabled.last_gp1.sequence && enabled.last_gp1.display.disabled == 0u);
    REQUIRE(enabled.last_gp1.display.display_x == enabled.guest.display.display_x && enabled.last_gp1.display.display_y == enabled.guest.display.display_y);
    return 1;
}

static int test_textured_draws_keep_normalized_command_class(void) {
    GpuRenderOracleEvent triangle = {0};
    GpuRenderOracleEvent quad = {0};

    REQUIRE(source_textured_draw(0, &triangle));
    REQUIRE(source_textured_draw(1, &quad));
    REQUIRE(triangle.kind == GPU_RENDER_ORACLE_EVENT_GP0_COMMAND &&
            triangle.mutation == GPU_RENDER_ORACLE_MUTATION_DRAW &&
            triangle.command.opcode == UINT8_C(0x20) &&
            triangle.command.command_class ==
                GPU_RENDER_ORACLE_COMMAND_CLASS_PRIMITIVE &&
            triangle.command.word_count == UINT64_C(7));
    REQUIRE(quad.kind == GPU_RENDER_ORACLE_EVENT_GP0_COMMAND &&
            quad.mutation == GPU_RENDER_ORACLE_MUTATION_DRAW &&
            quad.command.opcode == UINT8_C(0x20) &&
            quad.command.command_class ==
                GPU_RENDER_ORACLE_COMMAND_CLASS_PRIMITIVE &&
            quad.command.word_count == UINT64_C(9));
    return 1;
}

static int test_textured_draws_retain_exact_parser_metadata(void) {
    GpuRenderOracleEvent triangle = {0};
    GpuRenderOracleEvent quad = {0};

    REQUIRE(source_textured_draw(0, &triangle));
    REQUIRE(source_textured_draw(1, &quad));
    REQUIRE(triangle.command.opcode == UINT8_C(0x20) &&
            quad.command.opcode == UINT8_C(0x20));
    REQUIRE(triangle.packet.opcode == UINT8_C(0x24) &&
            triangle.packet.parser_word_count == UINT16_C(7) &&
            triangle.packet.parser_class ==
                GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED &&
            triangle.packet.task11_family_eligible);
    REQUIRE(quad.packet.opcode == UINT8_C(0x2c) &&
            quad.packet.parser_word_count == UINT16_C(9) &&
            quad.packet.parser_class ==
                GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED &&
            quad.packet.task11_family_eligible);
    return 1;
}

static int test_exact_packet_metadata_preserves_mmio_dma_parity(void) {
    GpuRenderOracleEvent mmio = {0};
    GpuRenderOracleEvent block = {0};
    GpuRenderOracleEvent linked = {0};
    GpuRenderOracleEvent burst = {0};

    REQUIRE(source_textured_triangle_route(ROUTE_MMIO, &mmio));
    REQUIRE(source_textured_triangle_route(ROUTE_DMA2_BLOCK, &block));
    REQUIRE(source_textured_triangle_route(ROUTE_DMA2_LINKED, &linked));
    REQUIRE(source_textured_triangle_route(ROUTE_DMA2_BURST, &burst));
    REQUIRE(same_draw_facts(&mmio, &block));
    REQUIRE(same_draw_facts(&mmio, &linked));
    REQUIRE(same_draw_facts(&mmio, &burst));
    REQUIRE(mmio.packet.opcode == UINT8_C(0x24) &&
            block.packet.opcode == UINT8_C(0x24) &&
            linked.packet.opcode == UINT8_C(0x24) &&
            burst.packet.opcode == UINT8_C(0x24) &&
            mmio.packet.parser_word_count == UINT16_C(7) &&
            block.packet.parser_word_count == UINT16_C(7) &&
            linked.packet.parser_word_count == UINT16_C(7) &&
            burst.packet.parser_word_count == UINT16_C(7) &&
            mmio.packet.task11_family_eligible &&
            block.packet.task11_family_eligible &&
            linked.packet.task11_family_eligible &&
            burst.packet.task11_family_eligible);
    REQUIRE(mmio.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_MMIO &&
            block.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BLOCK &&
            linked.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_LINKED_LIST &&
            burst.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BURST &&
            !burst.source.discontinuous &&
            linked.source.discontinuous);
    return 1;
}

static int test_parser_packet_exclusions(void) {
    GpuRenderOracleEvent untextured = {0};
    GpuRenderOracleEvent variable = {0};
    GpuRenderOracleEvent incomplete = {0};

    gpu_init();
    dma_init();
    REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    triangle_mmio();
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(0u, &untextured) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(untextured.packet.parser_class ==
                GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_UNTEXTURED &&
            !untextured.packet.task11_family_eligible);

    gpu_init();
    REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    mmio_gp0(UINT32_C(0x48000000)); mmio_gp0(xy(8u, 8u));
    mmio_gp0(xy(16u, 8u)); mmio_gp0(UINT32_C(0x50005000));
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(gpu_render_oracle_capture_read_event(0u, &variable) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(variable.packet.parser_class == GPU_RENDER_ORACLE_PACKET_CLASS_VARIABLE &&
            variable.packet.parser_word_count == 0u &&
            !variable.packet.task11_family_eligible);

    gpu_init();
    REQUIRE(gpu_render_oracle_capture_begin() == GPU_RENDER_ORACLE_RESULT_OK);
    mmio_gp0(UINT32_C(0x24ffffff));
    REQUIRE(gpu_render_oracle_capture_end() == GPU_RENDER_ORACLE_RESULT_INCOMPLETE);
    REQUIRE(gpu_render_oracle_capture_read_event(0u, &incomplete) ==
            GPU_RENDER_ORACLE_RESULT_OK);
    REQUIRE(incomplete.packet.opcode == UINT8_C(0x24) &&
            incomplete.packet.parser_word_count == UINT16_C(7) &&
            incomplete.packet.parser_class ==
                GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED &&
            !incomplete.packet.task11_family_eligible);
    return 1;
}

static int test_real_routes_normalize_without_erasing_provenance(void) {
    GpuRenderOracleEvent mmio = {0};
    GpuRenderOracleEvent block = {0};
    GpuRenderOracleEvent linked = {0};
    GpuRenderOracleEvent burst = {0};

    REQUIRE(source_draw(ROUTE_MMIO, &mmio));
    REQUIRE(source_draw(ROUTE_DMA2_BLOCK, &block));
    REQUIRE(source_draw(ROUTE_DMA2_LINKED, &linked));
    REQUIRE(source_draw(ROUTE_DMA2_BURST, &burst));
    REQUIRE(same_draw_facts(&mmio, &block));
    REQUIRE(same_draw_facts(&mmio, &linked));
    REQUIRE(same_draw_facts(&mmio, &burst));
    REQUIRE(mmio.vram_serial < block.vram_serial &&
            block.vram_serial < linked.vram_serial &&
            linked.vram_serial < burst.vram_serial);
    REQUIRE(mmio.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_MMIO && block.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BLOCK && linked.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_LINKED_LIST && burst.source.kind_mask == GPU_RENDER_ORACLE_SOURCE_MASK_DMA2_BURST);
    REQUIRE(mmio.source.word_count == UINT64_C(4) && !mmio.source.discontinuous);
    REQUIRE(block.source.first_word == (BLOCK_BASE + 24u) / 4u && block.source.last_word == (BLOCK_BASE + 36u) / 4u && block.source.first_container == BLOCK_BASE / 4u);
    REQUIRE(block.source.last_container == BLOCK_BASE / 4u && !block.source.discontinuous);
    REQUIRE(linked.source.first_word == (LINKED_FIRST + 28u) / 4u && linked.source.last_word == (LINKED_SECOND + 8u) / 4u && linked.source.first_container == LINKED_FIRST / 4u);
    REQUIRE(linked.source.last_container == LINKED_SECOND / 4u && linked.source.discontinuous);
    REQUIRE(burst.source.first_word == (BURST_BASE + 24u) / 4u && burst.source.last_word == (BURST_BASE + 36u) / 4u && burst.source.first_container == BURST_BASE / 4u);
    REQUIRE(burst.source.last_container == BURST_BASE / 4u && !burst.source.discontinuous);
    return 1;
}

static int submit_native_word(uint32_t word, uint32_t address,
                              GpuRenderOracleSourceKind kind,
                              uint32_t container) {
    const GpuRenderOracleSource source = {
        kind, address, address / 4u, container / 4u
    };
    return gpu_native_submit_gp0_word(word, &source);
}

static uint32_t mmio_submission_hook_calls;
static uint32_t mmio_submission_hook_command;
static GpuRenderTransactionId mmio_submission_hook_visual;
static GpuRenderSemantic mmio_submission_hook_semantic;
static int mmio_submission_hook_staged;

static void stage_mmio_submission(void) {
    ++mmio_submission_hook_calls;
    if (guest_render_native_stream_stage_exact(
            mmio_submission_hook_visual, mmio_submission_hook_command,
            &mmio_submission_hook_semantic) !=
            GUEST_RENDER_NATIVE_STREAM_OK ||
        guest_render_native_stream_activate_visual(
            mmio_submission_hook_visual) != GUEST_RENDER_NATIVE_STREAM_OK)
        return;
    mmio_submission_hook_staged = 1;
}

static int test_native_mmio_submission_finalizes_before_preflight(void) {
    const uint32_t command = UINT32_C(0x001fff9c);
    const uint32_t ft4[] = {
        UINT32_C(0x640000ff), xy(120u, 88u), UINT32_C(0x00000000),
        xy(8u, 8u),
    };
    GpuNativeDrawEnvironment environment;
    GuestRenderNativeStreamSnapshot stream;
    GpuNativePacketStreamSnapshot pending;
    uint64_t consumed_before;

    gpu_init();
    guest_render_native_stream_set_enabled(true);
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_before = stream.total_consumed;
    gpu_native_environment_get(&environment);
    REQUIRE(gpu_native_semantic_from_gp0(
                ft4, 4, &environment, &mmio_submission_hook_semantic) == 1);
    mmio_submission_hook_calls = 0u;
    mmio_submission_hook_command = command;
    mmio_submission_hook_visual = (GpuRenderTransactionId){ 1u, 1u };
    mmio_submission_hook_staged = 0;
    gpu_set_submission_hook(stage_mmio_submission);

    for (uint32_t index = 0u; index < 3u; ++index)
        REQUIRE(submit_native_word(
            ft4[index], command + index * 4u,
            GPU_RENDER_ORACLE_SOURCE_MMIO, GP0_MMIO));
    (void)submit_native_word(
        ft4[3], command + 3u * 4u,
        GPU_RENDER_ORACLE_SOURCE_MMIO, GP0_MMIO);

    REQUIRE(mmio_submission_hook_calls == 1u);
    REQUIRE(mmio_submission_hook_staged);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.reservation_consume_status == 5u);
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(stream.total_consumed == consumed_before + 1u);
    gpu_set_submission_hook(NULL);
    gpu_native_packet_stream_reset();
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_native_non_draw_commands_preserve_gp0_semantics(void) {
    GuestRenderNativeStreamSnapshot before = {0};
    GuestRenderNativeStreamSnapshot after = {0};
    const uint64_t gp0_before = gpu_get_gp0_count();

    gpu_init();
    guest_render_native_stream_set_enabled(true);
    REQUIRE(guest_render_native_stream_snapshot(&before) ==
            GUEST_RENDER_NATIVE_STREAM_OK);

    /* A linked-list tag is a DMA control word. Its KSEG alias must not become
     * a fake GP0(00h), while a payload NOP at the following word is real. */
    REQUIRE(submit_native_word(UINT32_C(0x00ffffff), UINT32_C(0x80001200),
                GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
                UINT32_C(0x00001200)));
    REQUIRE(submit_native_word(UINT32_C(0x00ffffff), UINT32_MAX,
                GPU_RENDER_ORACLE_SOURCE_UNKNOWN, 0u));
    REQUIRE(submit_native_word(UINT32_C(0x00000000), UINT32_C(0x80001204),
                GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
                UINT32_C(0x00001200)));
    REQUIRE(submit_native_word(UINT32_C(0x01000000), UINT32_C(0x00001400),
                GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                UINT32_C(0x00001400)));

    /* Header plus two payload words: 2x2 at the bottom-right wraps X and Y
     * independently through the canonical GP0(A0h) state machine. */
    REQUIRE(submit_native_word(UINT32_C(0xa0000000), UINT32_C(0x00001404),
                GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                UINT32_C(0x00001400)));
    REQUIRE(submit_native_word(xy(1023u, 511u), UINT32_C(0x00001408),
                GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                UINT32_C(0x00001400)));
    REQUIRE(submit_native_word(xy(2u, 2u), UINT32_C(0x0000140c),
                GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                UINT32_C(0x00001400)));
    REQUIRE(submit_native_word(UINT32_C(0x22221111), UINT32_C(0x00001410),
                GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                UINT32_C(0x00001400)));
    REQUIRE(submit_native_word(UINT32_C(0x44443333), UINT32_C(0x00001414),
                GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
                UINT32_C(0x00001400)));

    REQUIRE(gpu_vram_peek(1023, 511) == UINT16_C(0x1111));
    REQUIRE(gpu_vram_peek(0, 511) == UINT16_C(0x2222));
    REQUIRE(gpu_vram_peek(1023, 0) == UINT16_C(0x3333));
    REQUIRE(gpu_vram_peek(0, 0) == UINT16_C(0x4444));
    REQUIRE(gpu_get_gp0_count() - gp0_before == 7u);
    REQUIRE(guest_render_native_stream_snapshot(&after) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(after.total_native_packets - before.total_native_packets == 3u);
    REQUIRE(after.total_native_bound_packets -
                before.total_native_bound_packets == 3u);
    REQUIRE(after.total_native_unbound_packets ==
            before.total_native_unbound_packets);
    REQUIRE(after.native_opcode_counts[0x00] -
                before.native_opcode_counts[0x00] == 1u);
    REQUIRE(after.native_opcode_counts[0x01] -
                before.native_opcode_counts[0x01] == 1u);
    REQUIRE(after.native_opcode_counts[0xa0] -
                before.native_opcode_counts[0xa0] == 1u);
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int begin_mmio_native_upload(uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height) {
    return submit_native_word(UINT32_C(0xa0000000), GP0_MMIO,
                              GPU_RENDER_ORACLE_SOURCE_MMIO, GP0_MMIO) &&
        submit_native_word(xy(x, y), GP0_MMIO,
                           GPU_RENDER_ORACLE_SOURCE_MMIO, GP0_MMIO) &&
        submit_native_word(xy(width, height), GP0_MMIO,
                           GPU_RENDER_ORACLE_SOURCE_MMIO, GP0_MMIO);
}

static void reset_native_upload_test(void) {
    gpu_init();
    guest_render_native_stream_set_enabled(true);
    guest_render_native_stream_clear();
    gpu_native_packet_stream_reset();
}

static int test_mmio_upload_payload_continues_through_dma(void) {
    GpuNativePacketStreamSnapshot pending;
    GuestRenderNativeStreamSnapshot stream;
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic;
    const GpuRenderTransactionId visual_id = {20u, 1u};
    const uint32_t draw[] = {
        UINT32_C(0x640000ff), xy(72u, 72u), UINT32_C(0x00000000),
        xy(8u, 8u),
    };
    const uint32_t payload_word = UINT32_C(0x12341234);
    uint64_t nop_before;
    uint64_t state_before;

    reset_native_upload_test();
    REQUIRE(begin_mmio_native_upload(128u, 96u, 64u, 48u));
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.active && pending.opcode == 0xa0u &&
            pending.count == 3u && pending.expected == 1539u &&
            pending.source.kind == GPU_RENDER_ORACLE_SOURCE_MMIO);
    for (uint32_t index = 0u; index < 1536u; ++index)
        psx_write_word(BLOCK_BASE + index * 4u, payload_word);
    dma_feed(BLOCK_BASE, 1536u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(!pending.active && pending.count == 0u);
    REQUIRE(gpu_vram_peek(128, 96) == UINT16_C(0x1234));
    REQUIRE(gpu_vram_peek(191, 143) == UINT16_C(0x1234));
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    nop_before = stream.native_opcode_counts[0x00];
    state_before = stream.native_state_opcode_counts[0xe1];
    psx_write_word(FOLLOWUP_BASE + 0u, UINT32_C(0x00000000));
    psx_write_word(FOLLOWUP_BASE + 4u, UINT32_C(0xe1000000));
    dma_feed(FOLLOWUP_BASE, 2u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(!pending.active && pending.count == 0u);
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(stream.native_opcode_counts[0x00] == nop_before + 1u);
    REQUIRE(stream.native_state_opcode_counts[0xe1] == state_before + 1u);

    gpu_native_environment_get(&environment);
    REQUIRE(gpu_native_semantic_from_gp0(
                draw, 4, &environment, &semantic) == 1);
    REQUIRE(guest_render_native_stream_stage_exact(
                visual_id, FOLLOWUP_BASE + 8u, &semantic) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(guest_render_native_stream_activate_visual(visual_id) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    for (uint32_t index = 0u; index < 4u; ++index)
        psx_write_word(FOLLOWUP_BASE + 8u + index * 4u, draw[index]);
    dma_feed(FOLLOWUP_BASE + 8u, 4u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(stream.total_consumed == 1u);
    REQUIRE(pending.active && pending.opcode == 0x64u &&
            pending.source.word_address == FOLLOWUP_BASE + 8u);
    gpu_native_packet_stream_reset();

    reset_native_upload_test();
    REQUIRE(begin_mmio_native_upload(32u, 24u, 4u, 2u));
    psx_write_word(BLOCK_BASE + 0u, UINT32_C(0x22221111));
    psx_write_word(BLOCK_BASE + 4u, UINT32_C(0x44443333));
    dma_feed(BLOCK_BASE, 2u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.active && pending.count == 5u && pending.expected == 7u);
    psx_write_word(BURST_BASE + 0u, UINT32_C(0x66665555));
    psx_write_word(BURST_BASE + 4u, UINT32_C(0x88887777));
    dma_feed(BURST_BASE, 2u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(!pending.active);
    REQUIRE(gpu_vram_peek(32, 24) == UINT16_C(0x1111));
    REQUIRE(gpu_vram_peek(35, 25) == UINT16_C(0x8888));
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_mmio_upload_overflow_and_trailing_preflight(void) {
    GpuNativePacketStreamSnapshot pending;
    const uint32_t overflow[2] = {
        UINT32_C(0x22221111), UINT32_C(0x44443333),
    };
    uint16_t before;
    uint16_t draw_before;

    reset_native_upload_test();
    REQUIRE(begin_mmio_native_upload(48u, 40u, 2u, 1u));
    REQUIRE(!gpu_native_preflight_pending_gp0_words(overflow, 2u));
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.active && pending.count == 3u && pending.expected == 4u);

    psx_write_word(BLOCK_BASE + 0u, UINT32_C(0x22221111));
    psx_write_word(BLOCK_BASE + 4u, UINT32_C(0xe1000000));
    dma_feed(BLOCK_BASE, 2u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(!pending.active);
    REQUIRE(gpu_vram_peek(48, 40) == UINT16_C(0x1111));
    REQUIRE(gpu_vram_peek(49, 40) == UINT16_C(0x2222));

    reset_native_upload_test();
    before = gpu_vram_peek(64, 56);
    draw_before = gpu_vram_peek(11, 11);
    REQUIRE(begin_mmio_native_upload(64u, 56u, 2u, 1u));
    psx_write_word(BLOCK_BASE + 0u, UINT32_C(0x44443333));
    psx_write_word(BLOCK_BASE + 4u, UINT32_C(0x40ffffff));
    psx_write_word(BLOCK_BASE + 8u, xy(10u, 10u));
    psx_write_word(BLOCK_BASE + 12u, xy(20u, 10u));
    dma_feed(BLOCK_BASE, 4u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.active && pending.count == 3u && pending.expected == 4u);
    REQUIRE(gpu_vram_peek(64, 56) == before);
    REQUIRE(gpu_vram_peek(11, 10) == draw_before);
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_reserved_ft4_rejects_mutation_and_releases_atomically(void) {
    const GpuRenderTransactionId visual_id = {21u, 1u};
    const uint32_t packet[] = {
        UINT32_C(0x640000ff), xy(96u, 80u), UINT32_C(0x00000000),
        xy(8u, 8u),
    };
    uint32_t mutated[4];
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK,
        FOLLOWUP_BASE, FOLLOWUP_BASE / 4u, FOLLOWUP_BASE / 4u,
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic;
    GuestRenderNativeStreamSnapshot stream;
    GpuNativePacketStreamSnapshot pending;
    uint16_t before;
    uint64_t consumed_before;

    reset_native_upload_test();
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_before = stream.total_consumed;
    gpu_native_environment_get(&environment);
    REQUIRE(gpu_native_semantic_from_gp0(
                packet, 4, &environment, &semantic) == 1);
    REQUIRE(guest_render_native_stream_stage_exact(
                visual_id, FOLLOWUP_BASE, &semantic) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(guest_render_native_stream_activate_visual(visual_id) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(gpu_native_preflight_reservation_begin());
    REQUIRE(gpu_native_preflight_gp0_packet(packet, 4u, &source));
    REQUIRE(gpu_native_preflight_reservation_seal());

    memcpy(mutated, packet, sizeof(mutated));
    mutated[3] = xy(16u, 8u);
    before = gpu_vram_peek(96, 80);
    for (uint32_t index = 0u; index < 3u; ++index)
        REQUIRE(submit_native_word(
            mutated[index], FOLLOWUP_BASE + index * 4u,
            GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, FOLLOWUP_BASE));
    REQUIRE(!submit_native_word(
        mutated[3], FOLLOWUP_BASE + 12u,
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, FOLLOWUP_BASE));
    REQUIRE(gpu_vram_peek(96, 80) == before);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.active && pending.opcode == 0x64u &&
            pending.source.word_address == FOLLOWUP_BASE);
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(stream.total_consumed == consumed_before);

    gpu_native_preflight_reservation_abort();
    gpu_native_packet_stream_reset();
    REQUIRE(guest_render_native_stream_has_exact(visual_id, FOLLOWUP_BASE));

    REQUIRE(gpu_native_preflight_reservation_begin());
    REQUIRE(gpu_native_preflight_gp0_packet(packet, 4u, &source));
    REQUIRE(gpu_native_preflight_reservation_seal());
    for (uint32_t index = 0u; index < 3u; ++index)
        REQUIRE(submit_native_word(
            packet[index], FOLLOWUP_BASE + 4u + index * 4u,
            GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, FOLLOWUP_BASE));
    REQUIRE(!submit_native_word(
        packet[3], FOLLOWUP_BASE + 16u,
        GPU_RENDER_ORACLE_SOURCE_DMA2_BLOCK, FOLLOWUP_BASE));
    REQUIRE(gpu_vram_peek(96, 80) == before);
    gpu_native_preflight_reservation_abort();
    gpu_native_packet_stream_reset();
    REQUIRE(guest_render_native_stream_has_exact(visual_id, FOLLOWUP_BASE));

    REQUIRE(gpu_native_preflight_reservation_begin());
    REQUIRE(gpu_native_preflight_gp0_packet(packet, 4u, &source));
    gpu_native_preflight_reservation_abort();
    REQUIRE(guest_render_native_stream_has_exact(visual_id, FOLLOWUP_BASE));
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_exact_a0_dma_then_attributed_mmio_ft4(void) {
    const uint32_t start = UINT32_C(0x001fe79c);
    const uint32_t command = UINT32_C(0x001fff9c);
    const GpuRenderTransactionId visual_id = {22u, 1u};
    const uint32_t ft4[] = {
        UINT32_C(0x640000ff), xy(120u, 88u), UINT32_C(0x00000000),
        xy(8u, 8u),
    };
    GpuNativeDrawEnvironment environment;
    GpuRenderSemantic semantic;
    GuestRenderNativeStreamSnapshot stream;
    GpuNativePacketStreamSnapshot pending;
    uint64_t consumed_before;

    reset_native_upload_test();
    REQUIRE(begin_mmio_native_upload(160u, 112u, 64u, 48u));
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.active && pending.opcode == 0xa0u &&
            pending.count == 3u && pending.expected == 1539u);
    for (uint32_t index = 0u; index < 1536u; ++index)
        psx_write_word(start + index * 4u, UINT32_C(0x34563456));
    for (uint32_t index = 0u; index < 4u; ++index)
        psx_write_word(command + index * 4u, ft4[index]);

    gpu_native_environment_get(&environment);
    REQUIRE(gpu_native_semantic_from_gp0(
                ft4, 4, &environment, &semantic) == 1);
    REQUIRE(guest_render_native_stream_stage_exact(
                visual_id, command, &semantic) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(guest_render_native_stream_activate_visual(visual_id) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    consumed_before = stream.total_consumed;

    dma_feed(start, 1536u, DMA2_BLOCK);
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(!pending.active && pending.count == 0u);
    for (uint32_t index = 0u; index < 4u; ++index) {
        const GpuRenderOracleSource source = {
            GPU_RENDER_ORACLE_SOURCE_MMIO,
            command + index * 4u,
            (command + index * 4u) / 4u,
            command / 4u,
        };
        gpu_set_gp0_source(&source);
        gpu_write_gp0(ft4[index]);
    }
    REQUIRE(gpu_native_packet_stream_snapshot(&pending));
    REQUIRE(pending.reservation_consume_status == 5u);
    REQUIRE(pending.reserved_command_id == command &&
            pending.actual_command_id == command);
    REQUIRE(pending.reserved_container_id == command &&
            pending.actual_container_id == command);
    REQUIRE(pending.reserved_source_kind ==
                GUEST_RENDER_NATIVE_STREAM_SOURCE_MMIO &&
            pending.actual_source_kind ==
                GUEST_RENDER_NATIVE_STREAM_SOURCE_MMIO);
    REQUIRE(pending.reserved_opcode == 0x64u &&
            pending.actual_opcode == 0x64u &&
            pending.reserved_word_count == 4u &&
            pending.actual_word_count == 4u &&
            pending.reserved_packet_hash == pending.actual_packet_hash);
    REQUIRE(guest_render_native_stream_snapshot(&stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(stream.total_consumed == consumed_before + 1u);
    REQUIRE(gpu_vram_peek(160, 112) == UINT16_C(0x3456));
    gpu_native_packet_stream_reset();
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_native_unbound_submission_rejects_software_backend(void) {
    const GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_DMA2_LINKED_LIST,
        UINT32_C(0x00001804), UINT32_C(0x00001804) / 4u,
        UINT32_C(0x00001800) / 4u,
    };
    const uint32_t polygon[] = {
        UINT32_C(0x200000ff), xy(10u, 10u), xy(20u, 10u), xy(10u, 20u),
    };
    const uint32_t draw[] = {
        UINT32_C(0x400000ff), xy(10u, 10u), xy(20u, 10u),
    };
    GpuNativePacketStreamSnapshot reservation;
    GuestRenderNativeStreamSnapshot before_stream;
    GuestRenderNativeStreamSnapshot after_stream;
    uint16_t before;

    gpu_init();
    guest_render_native_stream_set_enabled(true);
    REQUIRE(guest_render_native_stream_snapshot(&before_stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(gpu_native_preflight_reservation_begin());
    REQUIRE(!gpu_native_preflight_gp0_packet(
        polygon, sizeof(polygon) / sizeof(polygon[0]), &source));
    REQUIRE(gpu_native_packet_stream_snapshot(&reservation));
    REQUIRE(reservation.reservation_phase == 1u &&
            reservation.reservation_count == 0u);
    gpu_native_preflight_reservation_abort();
    before = gpu_vram_peek(11, 10);
    REQUIRE(!gpu_native_submit_gp0_packet(
        draw, sizeof(draw) / sizeof(draw[0]), NULL, &source));
    REQUIRE(gpu_vram_peek(11, 10) == before);
    REQUIRE(guest_render_native_stream_snapshot(&after_stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(after_stream.total_native_unbound_packets ==
            before_stream.total_native_unbound_packets + 1u);
    REQUIRE(after_stream.total_native_unsupported_packets ==
            before_stream.total_native_unsupported_packets + 1u);
    REQUIRE(after_stream.total_original_draws ==
            before_stream.total_original_draws);
    REQUIRE(after_stream.total_parser_replay_commands ==
            before_stream.total_parser_replay_commands);
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_native_unbound_routes_reject_software_backend(void) {
    const uint32_t address = UINT32_C(0x001fff80);
    GpuRenderOracleSource source = {
        GPU_RENDER_ORACLE_SOURCE_MMIO, address, address / 4u, address / 4u,
    };
    GuestRenderNativeStreamSnapshot before_stream;
    GuestRenderNativeStreamSnapshot snapshot;
    uint16_t before;

    gpu_init();
    guest_render_native_stream_set_enabled(true);
    REQUIRE(guest_render_native_stream_snapshot(&before_stream) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    before = gpu_vram_peek(11, 10);
    gpu_set_gp0_source(&source);
    gpu_write_gp0(UINT32_C(0x40ffffff));
    source.word_address += 4u;
    ++source.word_ordinal;
    gpu_set_gp0_source(&source);
    gpu_write_gp0(xy(10u, 10u));
    source.word_address += 4u;
    ++source.word_ordinal;
    gpu_set_gp0_source(&source);
    gpu_write_gp0(xy(20u, 10u));
    source.word_address += 4u;
    ++source.word_ordinal;
    gpu_set_gp0_source(&source);
    REQUIRE(gpu_vram_peek(11, 10) == before);

    gpu_init();
    before = gpu_vram_peek(33, 32);
    psx_write_word(BLOCK_BASE + 0u, UINT32_C(0x40ffffff));
    psx_write_word(BLOCK_BASE + 4u, xy(32u, 32u));
    psx_write_word(BLOCK_BASE + 8u, xy(40u, 32u));
    dma_feed(BLOCK_BASE, 3u, DMA2_BLOCK);
    REQUIRE(gpu_vram_peek(33, 32) == before);

    gpu_init();
    before = gpu_vram_peek(49, 48);
    psx_write_word(BURST_BASE + 0u, UINT32_C(0x40ffffff));
    psx_write_word(BURST_BASE + 4u, xy(48u, 48u));
    psx_write_word(BURST_BASE + 8u, xy(56u, 48u));
    dma_feed(BURST_BASE, 3u, DMA2_BURST);
    REQUIRE(gpu_vram_peek(49, 48) == before);
    REQUIRE(guest_render_native_stream_snapshot(&snapshot) ==
            GUEST_RENDER_NATIVE_STREAM_OK);
    REQUIRE(snapshot.total_original_draws == 0u);
    REQUIRE(snapshot.total_parser_replay_commands == 0u);
    REQUIRE(snapshot.total_native_unbound_packets >
            before_stream.total_native_unbound_packets);
    REQUIRE(snapshot.total_native_unsupported_packets >
            before_stream.total_native_unsupported_packets);
    guest_render_native_stream_set_enabled(false);
    return 1;
}

int main(void) {
    (void)structured_source_setter;
    return test_capture_preserves_guest_outputs() &&
                    test_textured_draws_keep_normalized_command_class() &&
                    test_textured_draws_retain_exact_parser_metadata() &&
                    test_exact_packet_metadata_preserves_mmio_dma_parity() &&
                    test_parser_packet_exclusions() &&
                    test_real_routes_normalize_without_erasing_provenance() &&
                    test_native_non_draw_commands_preserve_gp0_semantics() &&
                    test_mmio_upload_payload_continues_through_dma() &&
                    test_mmio_upload_overflow_and_trailing_preflight() &&
                    test_reserved_ft4_rejects_mutation_and_releases_atomically() &&
                    test_exact_a0_dma_then_attributed_mmio_ft4() &&
                    test_native_unbound_submission_rejects_software_backend() &&
                    test_native_unbound_routes_reject_software_backend() &&
                    test_native_mmio_submission_finalizes_before_preflight()
                ? 0
                : 1;
}
