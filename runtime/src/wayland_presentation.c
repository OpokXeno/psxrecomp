#include "wayland_presentation.h"

#ifdef PSX_WAYLAND_PRESENTATION

#include "psx_sdl.h"
#if !defined(PSX_SDL3)
#include <SDL_syswm.h>
#endif
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"

#include <stdlib.h>
#include <string.h>

typedef struct PendingFeedback {
    struct wp_presentation_feedback *feedback;
    uint64_t swap_sequence;
    struct PendingFeedback *next;
} PendingFeedback;

static struct wl_display *s_display;
static struct wl_surface *s_surface;
static struct wl_registry *s_registry;
static struct wp_presentation *s_presentation;
static PendingFeedback *s_pending;
static PsxWaylandPresentationCallback s_callback;
static void *s_callback_opaque;
static PsxWaylandPresentationDiagnostics s_diagnostics;

static void feedback_remove(PendingFeedback *pending) {
    PendingFeedback **link = &s_pending;

    while (*link && *link != pending) link = &(*link)->next;
    if (*link) *link = pending->next;
    if (s_diagnostics.pending != 0u) s_diagnostics.pending--;
}

static void feedback_sync_output(
        void *data, struct wp_presentation_feedback *feedback,
        struct wl_output *output) {
    (void)data;
    (void)feedback;
    (void)output;
}

static void feedback_presented(
        void *data, struct wp_presentation_feedback *feedback,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
        uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
        uint32_t flags) {
    PendingFeedback *pending = (PendingFeedback *)data;
    PsxWaylandPresentationEvent event = {0};

    event.swap_sequence = pending->swap_sequence;
    event.presentation_time_ns =
        ((((uint64_t)tv_sec_hi << 32u) | tv_sec_lo) * UINT64_C(1000000000)) +
        tv_nsec;
    event.refresh_sequence = ((uint64_t)seq_hi << 32u) | seq_lo;
    event.refresh_ns = refresh;
    event.flags = flags;
    event.presented = 1u;
    feedback_remove(pending);
    s_diagnostics.presented++;
    if (s_callback) s_callback(&event, s_callback_opaque);
    wp_presentation_feedback_destroy(feedback);
    free(pending);
}

static void feedback_discarded(
        void *data, struct wp_presentation_feedback *feedback) {
    PendingFeedback *pending = (PendingFeedback *)data;
    PsxWaylandPresentationEvent event = {0};

    event.swap_sequence = pending->swap_sequence;
    feedback_remove(pending);
    s_diagnostics.discarded++;
    if (s_callback) s_callback(&event, s_callback_opaque);
    wp_presentation_feedback_destroy(feedback);
    free(pending);
}

static const struct wp_presentation_feedback_listener s_feedback_listener = {
    feedback_sync_output,
    feedback_presented,
    feedback_discarded,
};

static void presentation_clock_id(
        void *data, struct wp_presentation *presentation, uint32_t clock_id) {
    (void)data;
    (void)presentation;
    s_diagnostics.clock_id = clock_id;
}

static const struct wp_presentation_listener s_presentation_listener = {
    presentation_clock_id,
};

static void registry_global(
        void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)data;
    if (!s_presentation &&
        strcmp(interface, wp_presentation_interface.name) == 0) {
        const uint32_t bind_version = version < 2u ? version : 2u;
        s_presentation = wl_registry_bind(
            registry, name, &wp_presentation_interface, bind_version);
        if (s_presentation) {
            wp_presentation_add_listener(
                s_presentation, &s_presentation_listener, NULL);
            s_diagnostics.protocol_available = 1;
        }
    }
}

static void registry_global_remove(
        void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener s_registry_listener = {
    registry_global,
    registry_global_remove,
};

int psx_wayland_presentation_init(
        struct SDL_Window *window,
        PsxWaylandPresentationCallback callback, void *opaque) {
#if defined(PSX_SDL3)
    SDL_PropertiesID properties;

    psx_wayland_presentation_shutdown();
    if (!window) return 0;
    properties = SDL_GetWindowProperties(window);
    if (!properties) return 0;
    s_display = (struct wl_display *)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
    s_surface = (struct wl_surface *)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    if (!s_display || !s_surface) return 0;
#else
    SDL_SysWMinfo info;

    psx_wayland_presentation_shutdown();
    memset(&info, 0, sizeof(info));
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) ||
        info.subsystem != SDL_SYSWM_WAYLAND || !info.info.wl.display ||
        !info.info.wl.surface)
        return 0;
    s_display = info.info.wl.display;
    s_surface = info.info.wl.surface;
#endif
    s_callback = callback;
    s_callback_opaque = opaque;
    s_diagnostics.wayland_window = 1;
    s_registry = wl_display_get_registry(s_display);
    if (!s_registry) return 0;
    wl_registry_add_listener(s_registry, &s_registry_listener, NULL);
    if (wl_display_roundtrip(s_display) < 0 ||
        wl_display_roundtrip(s_display) < 0 || !s_presentation) {
        psx_wayland_presentation_shutdown();
        return 0;
    }
    return 1;
}

void psx_wayland_presentation_shutdown(void) {
    PendingFeedback *pending = s_pending;

    while (pending) {
        PendingFeedback *next = pending->next;
        wp_presentation_feedback_destroy(pending->feedback);
        free(pending);
        pending = next;
    }
    if (s_presentation) wp_presentation_destroy(s_presentation);
    if (s_registry) wl_registry_destroy(s_registry);
    s_display = NULL;
    s_surface = NULL;
    s_registry = NULL;
    s_presentation = NULL;
    s_pending = NULL;
    s_callback = NULL;
    s_callback_opaque = NULL;
    memset(&s_diagnostics, 0, sizeof(s_diagnostics));
}

int psx_wayland_presentation_request(uint64_t swap_sequence) {
    PendingFeedback *pending;

    if (!s_presentation || !s_surface) return 0;
    pending = (PendingFeedback *)calloc(1u, sizeof(*pending));
    if (!pending) return 0;
    pending->feedback = wp_presentation_feedback(s_presentation, s_surface);
    if (!pending->feedback) {
        free(pending);
        return 0;
    }
    pending->swap_sequence = swap_sequence;
    pending->next = s_pending;
    s_pending = pending;
    wp_presentation_feedback_add_listener(
        pending->feedback, &s_feedback_listener, pending);
    s_diagnostics.requested++;
    s_diagnostics.pending++;
    return 1;
}

void psx_wayland_presentation_diagnostics(
        PsxWaylandPresentationDiagnostics *out_diagnostics) {
    if (out_diagnostics) *out_diagnostics = s_diagnostics;
}

#endif
