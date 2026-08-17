/*
 * frame_pacing.c — race-free wall-clock frame pacing.
 * See frame_pacing.h for the Bug B history this replaces.
 */
#include "frame_pacing.h"

/* FRAME_PACING_PURE_ONLY: tests compile only the SDL-free decision
 * function (tests/test_frame_pacing.c includes this file directly). */
#ifndef FRAME_PACING_PURE_ONLY
#include <SDL.h>
#endif

uint32_t frame_pacing_sleep_ms(uint64_t now, uint64_t deadline,
                               uint64_t freq, uint64_t period) {
    if (now >= deadline) return 0;            /* compare BEFORE subtract */
    uint64_t remaining = deadline - now;       /* cannot underflow */
    if (remaining > period) remaining = period;/* hard cap: one frame max */
    if (freq == 0) return 0;
    /* remaining <= period (~one frame of ticks), so *1000 cannot overflow. */
    uint64_t ms = (remaining * 1000u) / freq;
    if (ms < 2) return 0;                      /* sub-2ms: spin instead */
    return (uint32_t)(ms - 1);                 /* undershoot; spin covers rest */
}

#define FRAME_PACER_CATCHUP_MAX_PERIODS 12u

uint64_t frame_pacing_advance_deadline(uint64_t now, uint64_t deadline,
                                       uint64_t period, int recover_debt) {
    if (deadline == 0) return now + period;
    if (now < deadline) return deadline + period;
    if (now - deadline < period * (recover_debt
            ? FRAME_PACER_CATCHUP_MAX_PERIODS : 1u))
        return deadline + period;
    return now + period;
}

#ifndef FRAME_PACING_PURE_ONLY

/* Bounded catch-up window, in periods. A transient stall (heavy frame, CD
 * burst) leaves next_deadline in the past; KEEPING that debt and running
 * unpaced until it is repaid preserves the long-term rate at exactly one
 * period per frame — preserving the guest/video cadence after a transient
 * stall. Only debt beyond this window — sustained sub-realtime emulation, not
 * a hiccup — is forgiven, else the pacer would demand unbounded catch-up. */
/* Vigilante 8's streamed FMV transitions have measured host stalls near
 * 140 ms. Eight 60 Hz periods are only 133.3 ms, so the old bound classified
 * those finite transitions as sustained slowness and permanently forgave the
 * guest/video debt. Keep a bounded 12-period (200 ms at 60 Hz) window: enough
 * to repay the observed transition without turning a real hang, suspend, or
 * sub-realtime workload into an unbounded catch-up burst. */
static void frame_pacer_wait_internal(FramePacer *p, double period_ms,
                                      int recover_debt) {
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t period = (uint64_t)((double)freq * (period_ms / 1000.0));
    uint64_t now = SDL_GetPerformanceCounter();

    if (p->next_deadline == 0 ||
        (now >= p->next_deadline &&
         now - p->next_deadline >=
             period * FRAME_PACER_CATCHUP_MAX_PERIODS)) {
        /* First frame, or sustained slowness beyond the catch-up window:
         * re-anchor (forgive the debt). */
        p->next_deadline = frame_pacing_advance_deadline(
            now, p->next_deadline, period, recover_debt);
        return;
    }
    if (now >= p->next_deadline) {
        p->next_deadline = frame_pacing_advance_deadline(
            now, p->next_deadline, period, recover_debt);
        return;
    }

    for (;;) {
        now = SDL_GetPerformanceCounter();     /* ONE read per iteration */
        uint32_t ms = frame_pacing_sleep_ms(now, p->next_deadline, freq, period);
        if (ms == 0) break;
        SDL_Delay(ms);
    }
    while (SDL_GetPerformanceCounter() < p->next_deadline) {
        /* final sub-ms spin */
    }
    p->next_deadline += period;
}

void frame_pacer_wait(FramePacer *p, double period_ms) {
    frame_pacer_wait_internal(p, period_ms, 1);
}

void frame_pacer_wait_stable(FramePacer *p, double period_ms) {
    frame_pacer_wait_internal(p, period_ms, 0);
}
#endif /* FRAME_PACING_PURE_ONLY */
