#include "native_render_baseline_runtime.h"

#include "audio_trace.h"
#include "gpu.h"
#include "spu.h"

#include <limits.h>
#include <string.h>

#define FNV_OFFSET UINT64_C(1469598103934665603)
#define FNV_PRIME UINT64_C(1099511628211)

extern uint64_t psx_get_cycle_count(void);
extern void gl_renderer_sync_cpu(void);
extern void vk_renderer_sync_cpu(void);

typedef struct {
    uint64_t cycles;
    uint64_t gp0;
    uint64_t gp1;
    uint64_t fill;
    uint64_t draw;
    uint64_t copy;
    uint64_t vram_serial;
    AudioTraceStats audio;
    SpuDebugInfo spu;
} NativeRenderBaselineRuntimeState;

static NativeRenderBaselineRuntimeState state;

static uint64_t mix(uint64_t hash, uint64_t value) {
    return (hash ^ value) * FNV_PRIME;
}

static int add_u64(uint64_t *value, uint64_t addition) {
    if (*value > UINT64_MAX - addition) return 0;
    *value += addition;
    return 1;
}

static int delta(uint64_t current, uint64_t previous, uint64_t *out) {
    if (current < previous) return 0;
    *out = current - previous;
    return 1;
}

static int audio_regressed(const AudioTraceStats *audio,
                           const SpuDebugInfo *spu) {
    int tap;
    for (tap = 0; tap < AUDIO_TAP_COUNT; ++tap) {
        if (audio->tap_frames[tap] < state.audio.tap_frames[tap] ||
            audio->tap_nonzero[tap] < state.audio.tap_nonzero[tap] ||
            audio->tap_audible[tap] < state.audio.tap_audible[tap]) return 1;
    }
    return audio->pump_calls < state.audio.pump_calls ||
           audio->pump_skips < state.audio.pump_skips ||
           audio->underruns < state.audio.underruns ||
           audio->mute_events < state.audio.mute_events ||
           audio->unmute_events < state.audio.unmute_events ||
           audio->events_total < state.audio.events_total ||
           spu->key_on_count < state.spu.key_on_count ||
           spu->render_frames < state.spu.render_frames ||
           spu->nonzero_frames < state.spu.nonzero_frames ||
           spu->cd_push_frames < state.spu.cd_push_frames ||
           spu->cd_overflow_frames < state.spu.cd_overflow_frames ||
           spu->cd_underflow_frames < state.spu.cd_underflow_frames;
}

static NativeRenderBaselineReason capture_audio(
        NativeRenderBaselineSnapshot *snapshot) {
    AudioTraceStats audio;
    SpuDebugInfo spu;
    uint64_t hash = FNV_OFFSET;
    int tap;
    audio_trace_get_stats(&audio);
    spu_debug_info(&spu);
    if (audio_regressed(&audio, &spu)) return NATIVE_RENDER_BASELINE_OVERFLOW;
    for (tap = 0; tap < AUDIO_TAP_COUNT; ++tap) {
        hash = mix(hash, audio.tap_frames[tap] - state.audio.tap_frames[tap]);
        hash = mix(hash, audio.tap_nonzero[tap] - state.audio.tap_nonzero[tap]);
        hash = mix(hash, audio.tap_audible[tap] - state.audio.tap_audible[tap]);
    }
    hash = mix(hash, audio.pump_calls - state.audio.pump_calls);
    hash = mix(hash, audio.pump_skips - state.audio.pump_skips);
    hash = mix(hash, audio.underruns - state.audio.underruns);
    hash = mix(hash, audio.mute_events - state.audio.mute_events);
    hash = mix(hash, audio.unmute_events - state.audio.unmute_events);
    hash = mix(hash, audio.events_total - state.audio.events_total);
    hash = mix(hash, spu.key_on_count - state.spu.key_on_count);
    hash = mix(hash, spu.render_frames - state.spu.render_frames);
    hash = mix(hash, spu.nonzero_frames - state.spu.nonzero_frames);
    hash = mix(hash, spu.cd_push_frames - state.spu.cd_push_frames);
    hash = mix(hash, spu.cd_overflow_frames - state.spu.cd_overflow_frames);
    hash = mix(hash, spu.cd_underflow_frames - state.spu.cd_underflow_frames);
    hash = mix(hash, spu.ctrl);
    hash = mix(hash, spu.active_mask);
    hash = mix(hash, (uint16_t)spu.main_l);
    hash = mix(hash, (uint16_t)spu.main_r);
    hash = mix(hash, (uint16_t)spu.cd_l);
    hash = mix(hash, (uint16_t)spu.cd_r);
    if (!add_u64(&snapshot->audio_frames,
                 audio.tap_frames[AUDIO_TAP_SPU_OUT] -
                     state.audio.tap_frames[AUDIO_TAP_SPU_OUT]) ||
        !add_u64(&snapshot->audio_events,
                 audio.events_total - state.audio.events_total))
        return NATIVE_RENDER_BASELINE_OVERFLOW;
    snapshot->audio_digest = mix(snapshot->audio_digest, hash);
    state.audio = audio;
    state.spu = spu;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

static NativeRenderBaselineReason capture_gpu(
        NativeRenderBaselineSnapshot *snapshot) {
    GpuDisplayInfo display;
    const uint16_t *vram;
    uint64_t nop;
    uint64_t env;
    uint64_t gp0 = gpu_get_gp0_count();
    uint64_t gp1 = gpu_get_gp1_count();
    uint64_t fill;
    uint64_t draw;
    uint64_t copy;
    uint64_t gp0_delta;
    uint64_t gp1_delta;
    uint64_t fill_delta;
    uint64_t draw_delta;
    uint64_t copy_delta;
    uint64_t mutation_delta;
    uint64_t vram_serial = gpu_render_vram_mutation_serial();
    uint64_t vram_hash = FNV_OFFSET;
    uint64_t display_hash = FNV_OFFSET;
    uint32_t x;
    uint32_t y;

    snapshot->global_vram_mutation_serial = vram_serial;
    snapshot->global_vram_serial_overflowed =
        gpu_render_vram_mutation_overflowed() ? 1 : 0;
    if (snapshot->global_vram_serial_overflowed)
        return NATIVE_RENDER_BASELINE_VRAM_SERIAL_OVERFLOW;
    gpu_get_gp0_stats(&nop, &fill, &draw, &env, &copy);
    if (!delta(gp0, state.gp0, &gp0_delta) ||
        !delta(gp1, state.gp1, &gp1_delta) ||
        !delta(fill, state.fill, &fill_delta) ||
        !delta(draw, state.draw, &draw_delta) ||
        !delta(copy, state.copy, &copy_delta) ||
        !delta(vram_serial, state.vram_serial, &mutation_delta))
        return NATIVE_RENDER_BASELINE_OVERFLOW;
    if (!add_u64(&snapshot->gp0_writes, gp0_delta) ||
        !add_u64(&snapshot->gp1_writes, gp1_delta) ||
        !add_u64(&snapshot->vram_mutations, mutation_delta))
        return NATIVE_RENDER_BASELINE_OVERFLOW;
    snapshot->gpu_digest = mix(snapshot->gpu_digest, gp0_delta);
    snapshot->gpu_digest = mix(snapshot->gpu_digest, gp1_delta);
    snapshot->gpu_digest = mix(snapshot->gpu_digest, fill_delta);
    snapshot->gpu_digest = mix(snapshot->gpu_digest, draw_delta);
    snapshot->gpu_digest = mix(snapshot->gpu_digest, copy_delta);
    snapshot->gpu_digest = mix(snapshot->gpu_digest, mutation_delta);
    state.gp0 = gp0;
    state.gp1 = gp1;
    state.fill = fill;
    state.draw = draw;
    state.copy = copy;
    state.vram_serial = vram_serial;
    snapshot->field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_VRAM_SERIAL;

    gl_renderer_sync_cpu();
    vk_renderer_sync_cpu();
    vram = gpu_get_vram();
    vram_hash = mix(vram_hash, 1024u);
    vram_hash = mix(vram_hash, 512u);
    for (y = 0; y < 512u; ++y) {
        for (x = 0; x < 1024u; ++x)
            vram_hash = mix(vram_hash, vram[y * 1024u + x]);
    }
    snapshot->vram_digest = mix(snapshot->vram_digest, vram_hash);
    snapshot->field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_VRAM_DIGEST;

    gpu_get_display_info(&display);
    if (display.depth24 || display.display_x >= 1024u ||
        display.display_y >= 512u || display.width == 0u ||
        display.width > 640u || display.height == 0u || display.height > 512u)
        return NATIVE_RENDER_BASELINE_UNSUPPORTED_DISPLAY;

    display_hash = mix(display_hash, display.display_x);
    display_hash = mix(display_hash, display.display_y);
    display_hash = mix(display_hash, display.width);
    display_hash = mix(display_hash, display.height);
    display_hash = mix(display_hash, display.disabled != 0);
    if (!display.disabled) {
        for (y = 0; y < display.height; ++y) {
            const uint32_t vram_y = (display.display_y + y) & 511u;
            for (x = 0; x < display.width; ++x) {
                const uint32_t vram_x = (display.display_x + x) & 1023u;
                display_hash = mix(
                    display_hash, vram[vram_y * 1024u + vram_x] & 0x7fffu);
            }
        }
    }
    snapshot->display15_digest =
        mix(snapshot->display15_digest, display_hash);
    snapshot->display_digest = snapshot->display15_digest;
    if (!add_u64(&snapshot->display_samples, 1u))
        return NATIVE_RENDER_BASELINE_OVERFLOW;
    snapshot->field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_DISPLAY15_DIGEST;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

void native_render_baseline_runtime_reset(void) {
    memset(&state, 0, sizeof(state));
}

void native_render_baseline_runtime_arm(void) {
    uint64_t nop;
    uint64_t env;
    state.cycles = psx_get_cycle_count();
    state.gp0 = gpu_get_gp0_count();
    state.gp1 = gpu_get_gp1_count();
    gpu_get_gp0_stats(&nop, &state.fill, &state.draw, &env, &state.copy);
    state.vram_serial = gpu_render_vram_mutation_serial();
    audio_trace_get_stats(&state.audio);
    spu_debug_info(&state.spu);
}

NativeRenderBaselineReason native_render_baseline_runtime_observe(
        NativeRenderBaselineSnapshot *snapshot) {
    uint64_t now = psx_get_cycle_count();
    uint64_t cycle_delta;
    NativeRenderBaselineReason reason;
    if (!delta(now, state.cycles, &cycle_delta) ||
        !add_u64(&snapshot->guest_cycle_delta, cycle_delta) ||
        !add_u64(&snapshot->vblank_delta, 1u))
        return NATIVE_RENDER_BASELINE_OVERFLOW;
    snapshot->cycle_digest = mix(snapshot->cycle_digest, cycle_delta);
    state.cycles = now;
    reason = capture_gpu(snapshot);
    if (reason != NATIVE_RENDER_BASELINE_COMPLETE) return reason;
    return capture_audio(snapshot);
}
