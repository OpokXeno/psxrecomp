#include "native_render_mode_control.h"

#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

typedef struct TestPresentation {
    bool opengl;
    bool interpolation;
    bool suspended;
    bool smooth;
    uint32_t history;
    uint32_t suspend_sequence;
    uint32_t first_effective_sequence;
    uint32_t sequence;
} TestPresentation;

static bool opengl_effective(void *user_data) {
    return ((TestPresentation *)user_data)->opengl;
}

static void set_interpolation(bool enabled, void *user_data) {
    TestPresentation *state = (TestPresentation *)user_data;
    state->interpolation = enabled;
    if (++state->sequence && state->first_effective_sequence == 0u)
        state->first_effective_sequence = state->sequence;
    if (!enabled) state->history = 0u;
}

static void set_suspended(bool suspended, void *user_data) {
    TestPresentation *state = (TestPresentation *)user_data;
    state->suspended = suspended;
    ++state->sequence;
    if (suspended) {
        state->suspend_sequence = state->sequence;
        state->history = 0u;
    }
}

static void set_smooth(bool enabled, void *user_data) {
    TestPresentation *state = (TestPresentation *)user_data;
    state->smooth = enabled;
    ++state->sequence;
    if (!enabled) state->history = 0u;
}

static void clear_histories(void *user_data) {
    ((TestPresentation *)user_data)->history = 0u;
}

static uint32_t history_count(void *user_data) {
    return ((TestPresentation *)user_data)->history;
}

static NativeRenderPresentationOps test_ops(void) {
    NativeRenderPresentationOps ops = {
        opengl_effective,
        set_interpolation,
        set_suspended,
        set_smooth,
        clear_histories,
        history_count,
    };
    return ops;
}

static int test_startup_precedence_and_invalid_fail_closed(void) {
    CHECK(native_render_mode_resolve("native", "shadow", "original") ==
          GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(native_render_mode_resolve("native", "shadow", NULL) ==
          GUEST_RENDER_RENDER_SHADOW);
    CHECK(native_render_mode_resolve("native", NULL, NULL) ==
          GUEST_RENDER_RENDER_NATIVE);
    CHECK(native_render_mode_resolve("native", "invalid", NULL) ==
          GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(native_render_mode_resolve("native", "shadow", "invalid") ==
          GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(strcmp(native_render_mode_name(GUEST_RENDER_RENDER_SHADOW),
                 "shadow") == 0);
    return 1;
}

static int test_quiesce_retains_requests_and_original_restores(void) {
    NativeRenderModeControl control = {0};
    NativeRenderPresentationSnapshot snapshot = {0};
    NativeRenderPresentationOps ops = test_ops();
    TestPresentation state = { .opengl = true, .history = 2u };

    CHECK(native_render_mode_control_init(&control, &ops, &state, true, true));
    state.history = 2u;
    state.sequence = 0u;
    state.first_effective_sequence = 0u;
    CHECK(native_render_mode_control_boundary(
        &control, GUEST_RENDER_RENDER_SHADOW, &snapshot));
    CHECK(snapshot.quiesced && snapshot.interpolation_requested &&
          snapshot.smooth_requested);
    CHECK(!snapshot.interpolation_effective && !snapshot.smooth_effective);
    CHECK(snapshot.history_count == 0u && state.suspended);
    CHECK(state.suspend_sequence != 0u &&
          state.suspend_sequence < state.first_effective_sequence);

    native_render_mode_control_set_interpolation(&control, false);
    native_render_mode_control_set_smooth(&control, false);
    native_render_mode_control_set_interpolation(&control, true);
    native_render_mode_control_set_smooth(&control, true);
    CHECK(state.suspended && !state.interpolation && !state.smooth);
    CHECK(state.history == 0u);

    CHECK(native_render_mode_control_boundary(
        &control, GUEST_RENDER_RENDER_ORIGINAL, &snapshot));
    CHECK(!snapshot.quiesced && snapshot.interpolation_requested &&
          snapshot.smooth_requested);
    CHECK(snapshot.interpolation_effective && snapshot.smooth_effective);
    CHECK(!state.suspended && state.interpolation && state.smooth);
    return 1;
}

static int test_non_gl_fails_gate(void) {
    NativeRenderModeControl control = {0};
    NativeRenderPresentationSnapshot snapshot = {0};
    NativeRenderPresentationOps ops = test_ops();
    TestPresentation state = {0};

    CHECK(native_render_mode_control_init(&control, &ops, &state, true, true));
    CHECK(!native_render_mode_control_boundary(
        &control, GUEST_RENDER_RENDER_NATIVE, &snapshot));
    CHECK(snapshot.reason == NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED);
    CHECK(!snapshot.quiesced);
    return 1;
}

int main(void) {
    return test_startup_precedence_and_invalid_fail_closed() &&
           test_quiesce_retains_requests_and_original_restores() &&
           test_non_gl_fails_gate()
        ? 0 : 1;
}
