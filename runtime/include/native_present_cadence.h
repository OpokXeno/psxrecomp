#ifndef PSX_NATIVE_PRESENT_CADENCE_H
#define PSX_NATIVE_PRESENT_CADENCE_H

#include <math.h>
#include <stdint.h>

typedef struct NativePresentCadence {
    int swap_interval;
    uint64_t period_ns;
    uint64_t display_period_ns;
} NativePresentCadence;

/* Original keeps a 60 Hz presenter so scenes authored at 60 still run at 60.
 * A blocking swap may own cadence only when the panel can represent the
 * requested tick with an integer divisor. Otherwise it silently caps targets
 * above the panel rate (75/120/240 on a 60 Hz display all become 60). */
static inline NativePresentCadence native_present_cadence_resolve(
        int fps, double panel_hz, int vsync, int threaded) {
    const int tick_hz = fps > 30 ? fps : 60;
    NativePresentCadence cadence = {
        0, (UINT64_C(1000000000) + (uint64_t)tick_hz / 2u) / (uint64_t)tick_hz, 0u};
    if (vsync != 0 && threaded && isfinite(panel_hz) && panel_hz > 0.0) {
        const double ratio = panel_hz / (double)tick_hz;
        /* Bound before conversion, including bogus display refresh reports. */
        if (ratio >= 0.5 && ratio < 17.0) {
            const int divisor = (int)(ratio + 0.5);
            if (fabs(panel_hz - (double)tick_hz * divisor) <= panel_hz * 0.01) {
                cadence.swap_interval = vsync < 0 && divisor == 1 ? -1 : divisor;
                cadence.display_period_ns = (uint64_t)(
                    1000000000.0 * (double)divisor / panel_hz + 0.5);
                /* Wake before the next vblank. An exact wall-clock period
                 * plus a blocking swap can miss every other refresh after a
                 * mode change, halving a 60 Hz output to 30. */
                cadence.period_ns -= cadence.period_ns / 32u;
            }
        }
    }
    return cadence;
}

#endif
