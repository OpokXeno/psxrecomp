#ifndef PSX_WAYLAND_PRESENTATION_H
#define PSX_WAYLAND_PRESENTATION_H

#include <stdint.h>

struct SDL_Window;

typedef struct PsxWaylandPresentationEvent {
    uint64_t swap_sequence;
    uint64_t presentation_time_ns;
    uint64_t refresh_sequence;
    uint32_t refresh_ns;
    uint32_t flags;
    uint8_t presented;
} PsxWaylandPresentationEvent;

typedef struct PsxWaylandPresentationDiagnostics {
    uint64_t requested;
    uint64_t presented;
    uint64_t discarded;
    uint64_t pending;
    uint32_t clock_id;
    int wayland_window;
    int protocol_available;
} PsxWaylandPresentationDiagnostics;

typedef void (*PsxWaylandPresentationCallback)(
    const PsxWaylandPresentationEvent *event, void *opaque);

#ifdef PSX_WAYLAND_PRESENTATION
int psx_wayland_presentation_init(
    struct SDL_Window *window,
    PsxWaylandPresentationCallback callback, void *opaque);
void psx_wayland_presentation_shutdown(void);
int psx_wayland_presentation_request(uint64_t swap_sequence);
void psx_wayland_presentation_diagnostics(
    PsxWaylandPresentationDiagnostics *out_diagnostics);
#else
static inline int psx_wayland_presentation_init(
        struct SDL_Window *window,
        PsxWaylandPresentationCallback callback, void *opaque) {
    (void)window;
    (void)callback;
    (void)opaque;
    return 0;
}
static inline void psx_wayland_presentation_shutdown(void) {}
static inline int psx_wayland_presentation_request(uint64_t swap_sequence) {
    (void)swap_sequence;
    return 0;
}
static inline void psx_wayland_presentation_diagnostics(
        PsxWaylandPresentationDiagnostics *out_diagnostics) {
    if (out_diagnostics) *out_diagnostics =
        (PsxWaylandPresentationDiagnostics){0};
}
#endif

#endif
