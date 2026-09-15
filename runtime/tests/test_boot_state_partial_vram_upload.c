#include "boot_state.h"
#include "dma.h"
#include "game_identity.h"
#include "gpu.h"
#include "memory.h"
#include "ram_provenance.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[PSX_MAIN_RAM_RETAIL_SIZE];
static uint8_t scratchpad[1024];
static uint8_t spu_ram[1];
static uint32_t cycles_since_vblank;
static uint32_t upload_commit_calls;
static uint16_t upload_commit_pixels[6];
static uint32_t restore_event_calls;
static uint32_t upload_event_calls;
static uint32_t checkpoint_write_calls;
static uint32_t checkpoint_prepare_calls;
static uint32_t checkpoint_restore_calls;
static uint32_t checkpoint_cancel_calls;
static uint32_t checkpoint_native_state;
static uint32_t checkpoint_prepared_state;
static int checkpoint_reject_restore;
static uint8_t ram_before_fault[PSX_MAIN_RAM_RETAIL_SIZE];
static uint32_t enhancement_state;

uint32_t i_stat;
uint32_t i_mask;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
int g_ls_replay_active;
int g_event_step_conservative;
int psx_in_device_service;
uint64_t psx_next_service_cycle;
uint32_t g_psx_icache_tv[1024];
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_SIZE - 1u;
uint64_t g_io_openbus_writes;

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return scratchpad; }
uint32_t memory_get_ram_size(void) { return sizeof(ram); }
uint32_t memory_get_ram_word_mask(void) { return g_psx_ram_mask & ~3u; }
int memory_developer_ram_enabled(void) { return 0; }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address) { return address; }
int gl_renderer_select_texture_bank(uint16_t id) { return id == 0u; }
uint32_t psx_mod_memory_layout_cookie(void) { return 0x1234u; }
uint32_t psx_mod_memory_snapshot_bytes(void) { return sizeof(enhancement_state); }
void psx_mod_memory_snapshot_write(uint8_t *out) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(enhancement_state >> (8u * i));
}
int psx_mod_memory_snapshot_read(const uint8_t *in, uint32_t size) {
    if (!in || size != 4u) return 0;
    enhancement_state = (uint32_t)in[0] | ((uint32_t)in[1] << 8u) |
        ((uint32_t)in[2] << 16u) | ((uint32_t)in[3] << 24u);
    return 1;
}

uint32_t psx_read_word(uint32_t address) {
    uint32_t offset = address & g_psx_ram_mask;
    return (uint32_t)ram[offset] | ((uint32_t)ram[offset + 1u] << 8u) |
           ((uint32_t)ram[offset + 2u] << 16u) |
           ((uint32_t)ram[offset + 3u] << 24u);
}
uint16_t psx_read_half(uint32_t address) {
    uint32_t offset = address & g_psx_ram_mask;
    return (uint16_t)((uint16_t)ram[offset] |
                      ((uint16_t)ram[offset + 1u] << 8u));
}
uint32_t psx_mod_read_word(uint32_t address) { return psx_read_word(address); }
uint8_t psx_read_byte(uint32_t address) {
    return ram[address & g_psx_ram_mask];
}
void psx_write_word(uint32_t address, uint32_t value) {
    uint32_t offset = address & g_psx_ram_mask;
    ram_provenance_invalidate_range(offset, 4u);
    ram[offset] = (uint8_t)value;
    ram[offset + 1u] = (uint8_t)(value >> 8u);
    ram[offset + 2u] = (uint8_t)(value >> 16u);
    ram[offset + 3u] = (uint8_t)(value >> 24u);
}

uint32_t dirty_ram_get_bitmap_word(uint32_t index) { (void)index; return 0; }
uint32_t dirty_ram_get_bitmap_word_count(void) {
    return (uint32_t)sizeof(ram) / (4096u * 32u);
}
void dirty_ram_set_bitmap_words(const uint32_t *words, uint32_t count) {
    (void)words;
    (void)count;
}
void dirty_ram_mark_executable_range(uint32_t address, uint32_t length) {
    (void)address;
    (void)length;
}

void psx_devices_service_to_now(void) {}
void psx_advance_cycles_slow(uint32_t cycles) { (void)cycles; }
uint32_t interrupts_get_cycles_since_vblank(void) {
    return cycles_since_vblank;
}
void interrupts_set_cycles_since_vblank(uint32_t value) {
    cycles_since_vblank = value;
}

void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                         uint16_t target[3], int32_t irq_line[3],
                         uint32_t frac[3]) {
    memset(counter, 0, 3u * sizeof(*counter));
    memset(mode, 0, 3u * sizeof(*mode));
    memset(target, 0, 3u * sizeof(*target));
    memset(irq_line, 0, 3u * sizeof(*irq_line));
    memset(frac, 0, 3u * sizeof(*frac));
}
void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                         const uint16_t target[3], const int32_t irq_line[3],
                         const uint32_t frac[3]) {
    (void)counter;
    (void)mode;
    (void)target;
    (void)irq_line;
    (void)frac;
}

#define EMPTY_SNAPSHOT(name) \
    uint32_t name##_snapshot_bytes(void) { return 0; } \
    void name##_snapshot_write(uint8_t *out) { (void)out; } \
    int name##_snapshot_read(const uint8_t *in, uint32_t len) { \
        (void)in; \
        return len == 0; \
    }

EMPTY_SNAPSHOT(spu)
EMPTY_SNAPSHOT(cdrom)
EMPTY_SNAPSHOT(sio)
EMPTY_SNAPSHOT(mdec)

uint32_t spu_get_ram_bytes(void) { return sizeof(spu_ram); }
void spu_ram_copy_out(uint8_t *out, uint32_t len) {
    if (out != NULL && len == sizeof(spu_ram)) memcpy(out, spu_ram, len);
}
int spu_ram_copy_in(const uint8_t *in, uint32_t len) {
    if (in == NULL || len != sizeof(spu_ram)) return 0;
    memcpy(spu_ram, in, len);
    return 1;
}

void psx_kernel_bless_note_range(uint32_t physical, uint32_t length) {
    (void)physical;
    (void)length;
}
void overlay_watch_invalidate_after_ram_restore(void) {}
void gte_canonicalize_cpu_state(CPUState *cpu) { (void)cpu; }

static uint32_t xy(uint32_t x, uint32_t y) { return x | (y << 16u); }

static size_t vram_index(size_t transfer_index) {
    const size_t x = (1023u + transfer_index % 3u) & 1023u;
    const size_t y = (511u + transfer_index / 3u) & 511u;
    return y * 1024u + x;
}

static void capture_upload_commit(
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count) {
    if (x != 1023u || y != 511u || width != 3u || height != 2u ||
        pixel_count != 6u)
        return;
    memcpy(upload_commit_pixels, pixels, sizeof(upload_commit_pixels));
    upload_commit_calls++;
}

static void capture_vram_event(const GpuVramEvent *event) {
    if (event == NULL) return;
    if (event->operation == GPU_VRAM_EVENT_RESTORE &&
        event->destination_x == 0u && event->destination_y == 0u &&
        event->width == 1024u && event->height == 512u)
        restore_event_calls++;
    else if (event->operation == GPU_VRAM_EVENT_UPLOAD)
        upload_event_calls++;
}

static uint32_t checkpoint_size(void) { return 4u; }

static int checkpoint_write(uint8_t *out, uint32_t size) {
    if (out == NULL || size != 4u) return 0;
    out[0] = (uint8_t)checkpoint_native_state;
    out[1] = (uint8_t)(checkpoint_native_state >> 8u);
    out[2] = (uint8_t)(checkpoint_native_state >> 16u);
    out[3] = (uint8_t)(checkpoint_native_state >> 24u);
    checkpoint_write_calls++;
    return 1;
}

static int checkpoint_restore_prepare(const uint8_t *checkpoint, uint32_t size,
                                       void **out_prepared) {
    if (checkpoint == NULL || size != 4u || out_prepared == NULL)
        return 0;
    *out_prepared = NULL;
    checkpoint_prepare_calls++;
    if (checkpoint_reject_restore) return 0;
    checkpoint_prepared_state =
        (uint32_t)checkpoint[0] | ((uint32_t)checkpoint[1] << 8u) |
        ((uint32_t)checkpoint[2] << 16u) |
        ((uint32_t)checkpoint[3] << 24u);
    *out_prepared = &checkpoint_prepared_state;
    return 1;
}

static void checkpoint_restore_commit(void *prepared) {
    if (prepared == NULL) return;
    checkpoint_native_state = *(const uint32_t *)prepared;
    checkpoint_restore_calls++;
}

static void checkpoint_restore_cancel(void *prepared) {
    if (prepared == NULL) return;
    checkpoint_cancel_calls++;
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static uint8_t *find_raw_section(uint8_t *state, size_t state_size,
                                 uint32_t wanted_tag, uint32_t *out_size) {
    uint8_t *cursor = state + BOOT_STATE_HEADER_WIRE_BYTES;
    uint8_t *end = state + state_size;
    uint32_t section_count;

    if (state_size < BOOT_STATE_HEADER_WIRE_BYTES) return NULL;
    section_count = read_u32(state + 28u);
    for (uint32_t index = 0u; index < section_count; ++index) {
        uint32_t tag;
        uint32_t flags;
        uint64_t size;
        if ((size_t)(end - cursor) < 16u) return NULL;
        tag = read_u32(cursor);
        flags = read_u32(cursor + 4u);
        size = read_u64(cursor + 8u);
        cursor += 16u;
        if (size > (uint64_t)(end - cursor)) return NULL;
        if (tag == wanted_tag) {
            if (flags != 0u || size > UINT32_MAX) return NULL;
            *out_size = (uint32_t)size;
            return cursor;
        }
        cursor += (size_t)size;
    }
    return NULL;
}

int main(void) {
    static const uint16_t saved_pixels[6] = {
        UINT16_C(0x1111), UINT16_C(0x2222), UINT16_C(0x7002),
        UINT16_C(0x7003), UINT16_C(0x7004), UINT16_C(0x7005),
    };
    static const uint16_t final_pixels[6] = {
        UINT16_C(0x1111), UINT16_C(0x2222), UINT16_C(0x3333),
        UINT16_C(0x4444), UINT16_C(0x5555), UINT16_C(0x6666),
    };
    const uint32_t bios_checksum = UINT32_C(0x12345678);
    const uint32_t entry_pc = UINT32_C(0x80010000);
    CPUState cpu = {0};
    uint8_t *state = NULL;
    uint8_t *malformed = NULL;
    uint8_t *before_reject = NULL;
    uint8_t *after_reject = NULL;
    uint8_t *before_fault = NULL;
    uint8_t *after_fault = NULL;
    size_t state_size = 0;
    size_t before_reject_size = 0;
    size_t after_reject_size = 0;
    size_t before_fault_size = 0;
    size_t after_fault_size = 0;
    uint16_t *vram;
    uint64_t serial_before;
    CPUState cpu_before_fault;
    RamProvenanceSource active_source;
    RamProvenanceSource completed_source = {
        .kind = RAM_PROVENANCE_SOURCE_MDEC_DMA1,
        .format = 3u,
    };
    RamProvenanceSource newer_source = {
        .kind = RAM_PROVENANCE_SOURCE_MDEC_DMA1,
        .format = 2u,
    };
    RamProvenanceSource restored_source;
    const uint32_t dma_base = UINT32_C(0x00013000);
    const uint32_t completed_base = UINT32_C(0x00015000);
    const uint32_t newer_base = UINT32_C(0x00017000);
    const BootStateNativeCheckpointHooks checkpoint_hooks = {
        .snapshot_size = checkpoint_size,
        .snapshot_write = checkpoint_write,
        .restore_prepare = checkpoint_restore_prepare,
        .restore_commit = checkpoint_restore_commit,
        .restore_cancel = checkpoint_restore_cancel,
    };

    if (psx_game_identity_runtime() == NULL) return 1;
    if (!ram_provenance_init(sizeof(ram))) return 1;
    ram_provenance_set_cpu_tracking(true);
    dma_init();
    {
        extern int test_mdec_dma_read_ready;
        extern uint32_t test_mdec_dma_read_value;
        extern uint32_t test_mdec_dma_output_format;
        test_mdec_dma_read_ready = 1;
        test_mdec_dma_read_value = UINT32_C(0x44332211);
        test_mdec_dma_output_format = 2u;
    }
    dma_write(UINT32_C(0x1f8010f0), UINT32_C(0x00000080));
    dma_write(UINT32_C(0x1f801090), dma_base);
    dma_write(UINT32_C(0x1f801094), UINT32_C(0x00010002));
    dma_write(UINT32_C(0x1f801098), UINT32_C(0x01000200));
    dma_advance(14u);
    if (!ram_provenance_source_word(dma_base, &active_source)) return 1;
    completed_source.receipt = ram_provenance_publish_event();
    ram_provenance_note_source_word(completed_base, &completed_source);
    gpu_init();
    gpu_set_vram_upload_commit_hook(capture_upload_commit);
    gpu_set_vram_event_hook(capture_vram_event);
    boot_state_set_native_checkpoint_hooks(&checkpoint_hooks);
    checkpoint_native_state = UINT32_C(0x11223344);
    enhancement_state = UINT32_C(0x12345678);
    vram = gpu_get_vram_ptr();
    for (size_t index = 0; index < 6u; ++index)
        vram[vram_index(index)] = (uint16_t)(UINT16_C(0x7000) + index);

    serial_before = gpu_render_vram_mutation_serial();
    gpu_write_gp0(UINT32_C(0xa0000000));
    gpu_write_gp0(xy(1023u, 511u));
    gpu_write_gp0(xy(3u, 2u));
    gpu_write_gp0(UINT32_C(0x22221111));
    if (gpu_gp0_parser_is_idle() || upload_commit_calls != 0u ||
        restore_event_calls != 0u || upload_event_calls != 0u ||
        gpu_render_vram_mutation_serial() != serial_before)
        return 1;

    if (!boot_state_save_buffer_raw(&cpu, bios_checksum, entry_pc,
                                    &state, &state_size))
        return 1;
    if (upload_commit_calls != 0u || checkpoint_write_calls != 1u ||
        gpu_render_vram_mutation_serial() != serial_before) {
        free(state);
        return 1;
    }
    checkpoint_native_state = UINT32_C(0xa1b2c3d4);
    enhancement_state = UINT32_C(0x87654321);

    dma_advance(14u);
    newer_source.receipt = ram_provenance_publish_event();
    ram_provenance_note_source_word(newer_base, &newer_source);
    ram[0] = UINT8_C(0x5a);
    cpu.pc = UINT32_C(0x80077770);
    malformed = (uint8_t *)malloc(state_size);
    if (malformed == NULL) {
        free(state);
        return 1;
    }
    memcpy(malformed, state, state_size);
    {
        uint32_t provenance_size = 0u;
        uint8_t *provenance = find_raw_section(
            malformed, state_size, BS_SEC_RAM_PROVENANCE, &provenance_size);
        if (provenance == NULL || provenance_size < 72u) {
            free(malformed);
            free(state);
            return 1;
        }
        provenance[32u + 38u] = UINT8_C(0xff);
    }
    gpu_init();
    if (!gpu_gp0_parser_is_idle()) {
        free(malformed);
        free(state);
        return 1;
    }
    for (size_t index = 0; index < 6u; ++index) {
        if (gpu_vram_peek((int)(vram_index(index) % 1024u),
                          (int)(vram_index(index) / 1024u)) != 0u) {
            free(malformed);
            free(state);
            return 1;
        }
    }
    {
        const uint32_t checkpoint_restores_before = checkpoint_restore_calls;
        const uint32_t restore_events_before = restore_event_calls;
        if (boot_state_load_buffer(malformed, state_size, bios_checksum,
                                   entry_pc, &cpu) ||
            ram[0] != UINT8_C(0x5a) ||
            cpu.pc != UINT32_C(0x80077770) ||
            checkpoint_restore_calls != checkpoint_restores_before ||
            restore_event_calls != restore_events_before ||
            !ram_provenance_source_word(newer_base, &restored_source) ||
            restored_source.receipt != newer_source.receipt) {
            free(malformed);
            free(state);
            return 1;
        }
    }
    free(malformed);

    if (!boot_state_save_buffer_raw(&cpu, bios_checksum, entry_pc,
                                    &before_reject, &before_reject_size)) {
        free(state);
        return 1;
    }
    {
        const uint32_t checkpoint_prepares_before = checkpoint_prepare_calls;
        const uint32_t checkpoint_restores_before = checkpoint_restore_calls;
        const uint32_t restore_events_before = restore_event_calls;

        checkpoint_reject_restore = 1;
        if (boot_state_load_buffer(state, state_size, bios_checksum,
                                   entry_pc, &cpu) ||
            checkpoint_prepare_calls != checkpoint_prepares_before + 1u ||
            checkpoint_restore_calls != checkpoint_restores_before ||
            restore_event_calls != restore_events_before) {
            free(before_reject);
            free(state);
            return 1;
        }
        checkpoint_reject_restore = 0;
    }
    if (!boot_state_save_buffer_raw(&cpu, bios_checksum, entry_pc,
                                    &after_reject, &after_reject_size) ||
        after_reject_size != before_reject_size ||
        memcmp(after_reject, before_reject, before_reject_size) != 0) {
        free(after_reject);
        free(before_reject);
        free(state);
        return 1;
    }
    free(after_reject);
    free(before_reject);

    if (!boot_state_save_buffer_raw(&cpu, bios_checksum, entry_pc,
                                    &before_fault, &before_fault_size)) {
        free(state);
        return 1;
    }
    cpu_before_fault = cpu;
    memcpy(ram_before_fault, ram, sizeof(ram_before_fault));
    {
        const uint32_t checkpoint_prepares_before = checkpoint_prepare_calls;
        const uint32_t checkpoint_restores_before = checkpoint_restore_calls;
        const uint32_t checkpoint_cancels_before = checkpoint_cancel_calls;
        const uint32_t restore_events_before = restore_event_calls;

        boot_state_test_fail_after_device_apply_once();
        if (boot_state_load_buffer(state, state_size, bios_checksum,
                                   entry_pc, &cpu) ||
            checkpoint_prepare_calls != checkpoint_prepares_before + 1u ||
            checkpoint_restore_calls != checkpoint_restores_before ||
            checkpoint_cancel_calls != checkpoint_cancels_before + 1u ||
            checkpoint_native_state != UINT32_C(0xa1b2c3d4) ||
            enhancement_state != UINT32_C(0x87654321) ||
            restore_event_calls != restore_events_before ||
            memcmp(&cpu, &cpu_before_fault, sizeof(cpu)) != 0 ||
            memcmp(ram, ram_before_fault, sizeof(ram_before_fault)) != 0 ||
            !gpu_gp0_parser_is_idle() ||
            !ram_provenance_source_word(newer_base, &restored_source) ||
            restored_source.kind != newer_source.kind ||
            restored_source.format != newer_source.format ||
            restored_source.receipt != newer_source.receipt) {
            free(before_fault);
            free(state);
            return 1;
        }
    }
    if (!boot_state_save_buffer_raw(&cpu, bios_checksum, entry_pc,
                                    &after_fault, &after_fault_size) ||
        read_u32(before_fault + 4u) != BOOT_STATE_VERSION ||
        after_fault_size != before_fault_size ||
        memcmp(after_fault, before_fault, before_fault_size) != 0) {
        free(after_fault);
        free(before_fault);
        free(state);
        return 1;
    }
    free(after_fault);
    free(before_fault);

    const uint32_t expected_restore_events = restore_event_calls + 1u;
    if (!boot_state_load_buffer(state, state_size, bios_checksum, entry_pc,
                                &cpu)) {
        free(state);
        return 1;
    }
    free(state);
    if (enhancement_state != UINT32_C(0x12345678)) return 1;
    if (ram[0] == UINT8_C(0x5a) ||
        !ram_provenance_source_word(completed_base, &restored_source) ||
        restored_source.receipt != completed_source.receipt ||
        restored_source.format != completed_source.format ||
        !ram_provenance_source_word(dma_base, &restored_source) ||
        restored_source.receipt != active_source.receipt ||
        ram_provenance_source_word(newer_base, &restored_source))
        return 1;
    dma_advance(1u);
    if (!ram_provenance_source_word(dma_base, &active_source)) return 1;
    dma_advance(13u);
    if (!ram_provenance_source_word(dma_base + 4u, &restored_source) ||
        restored_source.kind != active_source.kind ||
        restored_source.format != active_source.format ||
        restored_source.receipt != active_source.receipt)
        return 1;
    if (gpu_gp0_parser_is_idle() || upload_commit_calls != 0u ||
        restore_event_calls != expected_restore_events || upload_event_calls != 0u ||
        checkpoint_restore_calls != 1u ||
        gpu_render_vram_mutation_serial() != serial_before)
        return 1;
    for (size_t index = 0; index < 6u; ++index) {
        if (vram[vram_index(index)] != saved_pixels[index]) return 1;
    }

    gpu_write_gp0(UINT32_C(0x44443333));
    if (gpu_gp0_parser_is_idle() || upload_commit_calls != 0u) return 1;
    gpu_write_gp0(UINT32_C(0x66665555));
    if (!gpu_gp0_parser_is_idle() || upload_commit_calls != 1u ||
        restore_event_calls != expected_restore_events || upload_event_calls != 1u ||
        gpu_render_vram_mutation_serial() != serial_before + 1u)
        return 1;
    for (size_t index = 0; index < 6u; ++index) {
        if (vram[vram_index(index)] != final_pixels[index] ||
            upload_commit_pixels[index] != final_pixels[index])
            return 1;
    }

    gpu_set_vram_upload_commit_hook(NULL);
    gpu_set_vram_event_hook(NULL);
    boot_state_set_native_checkpoint_hooks(NULL);
    ram_provenance_set_cpu_tracking(false);
    return 0;
}
