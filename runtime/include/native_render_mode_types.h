#ifndef PSX_NATIVE_RENDER_MODE_TYPES_H
#define PSX_NATIVE_RENDER_MODE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum NativeRenderPresentationGateReason {
    NATIVE_RENDER_PRESENTATION_GATE_NONE = 0,
    NATIVE_RENDER_PRESENTATION_GATE_REQUESTED_ORIGINAL,
    NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED,
    NATIVE_RENDER_PRESENTATION_GATE_HISTORY_NOT_EMPTY,
} NativeRenderPresentationGateReason;

typedef struct NativeRenderPresentationSnapshot {
    bool interpolation_requested;
    bool interpolation_effective;
    bool smooth_requested;
    bool smooth_effective;
    bool quiesced;
    uint32_t history_count;
    NativeRenderPresentationGateReason reason;
} NativeRenderPresentationSnapshot;

#endif
