#ifndef PSX_NATIVE_RENDER_MODE_CONTROL_H
#define PSX_NATIVE_RENDER_MODE_CONTROL_H

#include "guest_render_types.h"
#include "native_render_mode_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NativeRenderPresentationOps {
    bool (*opengl_effective)(void *user_data);
    void (*set_interpolation_effective)(bool enabled, void *user_data);
    void (*set_interpolation_suspended)(bool suspended, void *user_data);
    void (*set_smooth_effective)(bool enabled, void *user_data);
    void (*clear_histories)(void *user_data);
    uint32_t (*history_count)(void *user_data);
} NativeRenderPresentationOps;

typedef struct NativeRenderModeControl {
    NativeRenderPresentationOps ops;
    void *user_data;
    NativeRenderPresentationSnapshot snapshot;
    bool initialized;
} NativeRenderModeControl;

GuestRenderRenderMode native_render_mode_parse(const char *value);
GuestRenderRenderMode native_render_mode_resolve(const char *config_value,
                                                 const char *environment_value,
                                                 const char *cli_value);
const char *native_render_mode_name(GuestRenderRenderMode mode);

bool native_render_mode_control_init(
    NativeRenderModeControl *control,
    const NativeRenderPresentationOps *ops,
    void *user_data,
    bool interpolation_requested,
    bool smooth_requested);
void native_render_mode_control_set_interpolation(
    NativeRenderModeControl *control, bool enabled);
void native_render_mode_control_set_smooth(
    NativeRenderModeControl *control, bool enabled);
bool native_render_mode_control_boundary(
    NativeRenderModeControl *control,
    GuestRenderRenderMode requested_mode,
    NativeRenderPresentationSnapshot *out_snapshot);
bool native_render_mode_control_snapshot(
    const NativeRenderModeControl *control,
    NativeRenderPresentationSnapshot *out_snapshot);
const char *native_render_presentation_gate_reason_name(uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif
