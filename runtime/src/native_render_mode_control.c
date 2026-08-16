#include "native_render_mode_control.h"

#include <string.h>

GuestRenderRenderMode native_render_mode_parse(const char *value) {
    if (value != NULL && strcmp(value, "shadow") == 0)
        return GUEST_RENDER_RENDER_SHADOW;
    if (value != NULL && strcmp(value, "native") == 0)
        return GUEST_RENDER_RENDER_NATIVE;
    return GUEST_RENDER_RENDER_ORIGINAL;
}

GuestRenderRenderMode native_render_mode_resolve(const char *config_value,
                                                 const char *environment_value,
                                                 const char *cli_value) {
    if (cli_value != NULL) return native_render_mode_parse(cli_value);
    if (environment_value != NULL)
        return native_render_mode_parse(environment_value);
    return native_render_mode_parse(config_value);
}

const char *native_render_mode_name(GuestRenderRenderMode mode) {
    switch (mode) {
    case GUEST_RENDER_RENDER_SHADOW: return "shadow";
    case GUEST_RENDER_RENDER_NATIVE: return "native";
    case GUEST_RENDER_RENDER_ORIGINAL:
    default: return "original";
    }
}

static bool ops_are_complete(const NativeRenderPresentationOps *ops) {
    return ops != NULL && ops->opengl_effective != NULL &&
           ops->set_interpolation_effective != NULL &&
           ops->set_interpolation_suspended != NULL &&
           ops->set_smooth_effective != NULL &&
           ops->clear_histories != NULL && ops->history_count != NULL;
}

static void publish_effective(NativeRenderModeControl *control,
                              bool interpolation, bool smooth) {
    control->ops.set_interpolation_effective(interpolation,
                                             control->user_data);
    control->ops.set_smooth_effective(smooth, control->user_data);
    control->snapshot.interpolation_effective = interpolation;
    control->snapshot.smooth_effective = smooth;
}

bool native_render_mode_control_init(
    NativeRenderModeControl *control,
    const NativeRenderPresentationOps *ops,
    void *user_data,
    bool interpolation_requested,
    bool smooth_requested) {
    if (control == NULL || !ops_are_complete(ops)) return false;
    memset(control, 0, sizeof(*control));
    control->ops = *ops;
    control->user_data = user_data;
    control->snapshot.interpolation_requested = interpolation_requested;
    control->snapshot.smooth_requested = smooth_requested;
    control->initialized = true;
    control->ops.set_interpolation_suspended(false, control->user_data);
    publish_effective(control, interpolation_requested, smooth_requested);
    control->snapshot.history_count = control->ops.history_count(user_data);
    return true;
}

void native_render_mode_control_set_interpolation(
    NativeRenderModeControl *control, bool enabled) {
    if (control == NULL) return;
    control->snapshot.interpolation_requested = enabled;
    if (!control->initialized || control->snapshot.quiesced) return;
    control->ops.set_interpolation_effective(enabled, control->user_data);
    control->snapshot.interpolation_effective = enabled;
}

void native_render_mode_control_set_smooth(
    NativeRenderModeControl *control, bool enabled) {
    if (control == NULL) return;
    control->snapshot.smooth_requested = enabled;
    if (!control->initialized) return;
    if (control->snapshot.quiesced) {
        control->ops.set_smooth_effective(false, control->user_data);
        control->snapshot.smooth_effective = false;
        return;
    }
    control->ops.set_smooth_effective(enabled, control->user_data);
    control->snapshot.smooth_effective = enabled;
}

bool native_render_mode_control_boundary(
    NativeRenderModeControl *control,
    GuestRenderRenderMode requested_mode,
    NativeRenderPresentationSnapshot *out_snapshot) {
    bool success = true;

    if (control == NULL || !control->initialized || out_snapshot == NULL)
        return false;
    if (requested_mode == GUEST_RENDER_RENDER_ORIGINAL) {
        control->snapshot.quiesced = false;
        control->snapshot.reason =
            NATIVE_RENDER_PRESENTATION_GATE_REQUESTED_ORIGINAL;
        control->ops.set_interpolation_suspended(false, control->user_data);
        publish_effective(control,
                          control->snapshot.interpolation_requested,
                          control->snapshot.smooth_requested);
    } else if (requested_mode != GUEST_RENDER_RENDER_SHADOW &&
               requested_mode != GUEST_RENDER_RENDER_NATIVE) {
        control->snapshot.reason =
            NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED;
        success = false;
    } else if (!control->ops.opengl_effective(control->user_data)) {
        control->snapshot.reason =
            NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED;
        success = false;
    } else {
        /* Latch dominance before touching either presentation path. */
        control->snapshot.quiesced = true;
        control->ops.set_interpolation_suspended(true, control->user_data);
        publish_effective(control, false, false);
        control->ops.clear_histories(control->user_data);
        control->snapshot.history_count =
            control->ops.history_count(control->user_data);
        if (control->snapshot.history_count != 0u) {
            control->snapshot.reason =
                NATIVE_RENDER_PRESENTATION_GATE_HISTORY_NOT_EMPTY;
            success = false;
        } else {
            control->snapshot.reason = NATIVE_RENDER_PRESENTATION_GATE_NONE;
        }
    }
    control->snapshot.history_count =
        control->ops.history_count(control->user_data);
    *out_snapshot = control->snapshot;
    return success;
}

bool native_render_mode_control_snapshot(
    const NativeRenderModeControl *control,
    NativeRenderPresentationSnapshot *out_snapshot) {
    if (control == NULL || !control->initialized || out_snapshot == NULL)
        return false;
    *out_snapshot = control->snapshot;
    return true;
}

const char *native_render_presentation_gate_reason_name(uint32_t reason) {
    switch (reason) {
    case NATIVE_RENDER_PRESENTATION_GATE_NONE: return "none";
    case NATIVE_RENDER_PRESENTATION_GATE_REQUESTED_ORIGINAL:
        return "requested_original";
    case NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED:
        return "opengl_required";
    case NATIVE_RENDER_PRESENTATION_GATE_HISTORY_NOT_EMPTY:
        return "history_not_empty";
    default: return "unknown";
    }
}
