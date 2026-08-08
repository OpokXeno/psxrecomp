#ifndef PSXRECOMP_GPU_RENDER_ORACLE_INTERNAL_H
#define PSXRECOMP_GPU_RENDER_ORACLE_INTERNAL_H

#include "gpu_render_oracle.h"

static inline void gpu_render_oracle_hook_gp0_begin(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source) {
    if (device != NULL && gpu_render_oracle_capture_enabled(device)) {
        (void)gpu_render_oracle_gp0_begin(device, command, source);
    }
}

static inline void gpu_render_oracle_hook_gp0_begin_parsed(
    GpuRenderOracleDevice *device, GpuRenderOracleCommandKind command,
    GpuRenderOracleSourceKind source, const GpuRenderOraclePacket *packet) {
    if (device != NULL && gpu_render_oracle_capture_enabled(device)) {
        (void)gpu_render_oracle_gp0_begin_parsed(device, command, source, packet);
    }
}

static inline void gpu_render_oracle_hook_gp0_source_word(
    GpuRenderOracleDevice *device, uint64_t word_ordinal,
    uint64_t container_ordinal) {
    if (device != NULL && gpu_render_oracle_capture_enabled(device)) {
        (void)gpu_render_oracle_gp0_source_word(device, word_ordinal,
                                                container_ordinal);
    }
}

static inline void gpu_render_oracle_hook_gp0_complete(
    GpuRenderOracleDevice *device, GpuRenderOracleMutationKind mutation,
    const GpuRenderOracleDrawState *draw,
    const GpuRenderOracleTransfer *transfer) {
    if (device != NULL) {
        (void)gpu_render_oracle_gp0_complete(device, mutation, draw, transfer);
    }
}

static inline void gpu_render_oracle_hook_polyline_segment(
    GpuRenderOracleDevice *device, const GpuRenderOracleDrawState *draw) {
    if (device != NULL) {
        (void)gpu_render_oracle_polyline_segment(device, draw);
    }
}

static inline void gpu_render_oracle_hook_gp1_complete(
    GpuRenderOracleDevice *device, const GpuRenderOracleDisplayState *display) {
    if (device != NULL && gpu_render_oracle_capture_enabled(device)) {
        (void)gpu_render_oracle_gp1_complete(device, display);
    }
}

static inline void gpu_render_oracle_hook_upload_word(
    GpuRenderOracleDevice *device) {
    if (device != NULL && gpu_render_oracle_capture_enabled(device)) {
        (void)gpu_render_oracle_upload_word(device);
    }
}

static inline void gpu_render_oracle_hook_gpuread_word(
    GpuRenderOracleDevice *device) {
    if (device != NULL && gpu_render_oracle_capture_enabled(device)) {
        (void)gpu_render_oracle_gpuread_word(device);
    }
}

#if defined(GPU_RENDER_ORACLE_TESTING)
void gpu_render_oracle_test_seed_next_sequence(GpuRenderOracleDevice *device,
                                                uint64_t next_sequence);
void gpu_render_oracle_test_seed_vram_serial(GpuRenderOracleDevice *device,
                                              uint64_t vram_serial);
void gpu_render_oracle_test_seed_event_count(GpuRenderOracleDevice *device,
                                              uint64_t event_count);
#endif

#endif
