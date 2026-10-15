#include "native_present_cadence.h"
#include "native_phase_cadence.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(native_phase_denominator(16666667u,75u,8u)==2u);
    assert(native_phase_denominator(33333333u,75u,8u)==3u);
    assert(native_phase_denominator(33333333u,165u,8u)==6u);
    assert(native_phase_denominator(16683350u,60u,8u)==1u);
    assert(native_phase_denominator(33366700u,60u,8u)==2u);
    assert(native_phase_denominator(UINT64_MAX,240u,8u)==8u);
    const int targets[] = {30, 45, 60, 75, 90, 100, 120, 144, 165, 240};
    const double panels[] = {0.0, 59.94, 60.0, 75.0, 120.0, 144.0, 165.0, 240.0};
    for (unsigned t = 0; t < sizeof(targets) / sizeof(*targets); ++t) {
        const int hz = targets[t] == 30 ? 60 : targets[t];
        const uint64_t period = (UINT64_C(1000000000) + hz / 2u) / hz;
        for (unsigned p = 0; p < sizeof(panels) / sizeof(*panels); ++p) {
            for (int vsync = -1; vsync <= 1; ++vsync) {
                NativePresentCadence cadence = native_present_cadence_resolve(
                    targets[t], panels[p], vsync, 1);
                assert(cadence.period_ns <= period);
                if (cadence.swap_interval) {
                    assert(vsync && cadence.display_period_ns);
                    const int divisor = cadence.swap_interval < 0 ? 1 : cadence.swap_interval;
                    const double actual_hz = panels[p] / divisor;
                    assert(fabs(actual_hz - hz) <= hz * 0.011);
                    assert(cadence.period_ns < cadence.display_period_ns);
                    /* A swap taking a full display period must leave the next
                     * tick due immediately, including after aspect/FPS changes. */
                    assert(cadence.display_period_ns >= cadence.period_ns);
                } else {
                    assert(!cadence.display_period_ns && cadence.period_ns == period);
                }
                cadence = native_present_cadence_resolve(targets[t], panels[p], vsync, 0);
                assert(!cadence.swap_interval && !cadence.display_period_ns);
                assert(cadence.period_ns == period);
            }
        }
    }
    assert(native_present_cadence_resolve(75, 60.0, 1, 1).period_ns == 13333333u);
    assert(native_present_cadence_resolve(120, 60.0, 1, 1).swap_interval == 0);
    assert(native_present_cadence_resolve(30, 60.0, 1, 1).swap_interval == 1);
    assert(native_present_cadence_resolve(45, 60.0, 1, 1).period_ns == 22222222u);
    assert(native_present_cadence_resolve(60, 120.0, -1, 1).swap_interval == 2);
    assert(native_present_cadence_resolve(75, NAN, 1, 1).swap_interval == 0);
    assert(native_present_cadence_resolve(75, INFINITY, 1, 1).swap_interval == 0);
    puts("Native cadence: 60/75/high targets, panel divisors and VSync transitions passed");
    return 0;
}
