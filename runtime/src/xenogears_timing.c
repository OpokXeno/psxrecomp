#include "xenogears_timing.h"
#include <string.h>

extern uint8_t psx_read_byte(uint32_t address);

enum {
    XG_RAM_LAST_WORD = 0x001FFFFCu, XG_CACHED_BASE = 0x80000000u,
    XG_UNCACHED_BASE = 0xA0000000u, XG_FIELD_CONTEXT = 0x800B0078u,
    XG_FIELD_ID = 0x8006F94Eu, XG_MODULE_REQUESTED = 0x80018088u,
    XG_MODULE_ACTIVE = 0x800592C0u, XG_MODULE_POINTER = 0x800592BCu,
    XG_GAME_PROGRESS = 0x8006EF64u,
    XG_FIELD_ID_MASK = 0x07FFu, XG_FIELD_ID_LIMIT = 0x0400u,
};

typedef struct {
    NativeFpsStartupPolicy policy;
    XgTimingScene scene;
    NativeFpsMode effective_mode;
    XgTimingRoute route;
    XgTimingReason reason;
    uint64_t helper_activations, authored_updates;
    uint32_t last_counted_frame_token;
    uint32_t generation;
    uint8_t have_scene, fmv_active, active, have_counted_frame_token;
} XgTimingState;

static XgTimingState s_timing = {
    { NATIVE_FPS_MODE_ORIGINAL, NATIVE_FPS_STARTUP_ORIGINAL_DEFAULT, 0 },
    { 0 }, NATIVE_FPS_MODE_ORIGINAL, XG_TIMING_ROUTE_NONE,
    XG_TIMING_REASON_BOOT, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
};

static uint16_t read_u16(uint32_t a) { return (uint16_t)(psx_read_byte(a) | ((uint32_t)psx_read_byte(a + 1u) << 8)); }
static uint32_t read_u32(uint32_t a) { return read_u16(a) | ((uint32_t)read_u16(a + 2u) << 16); }

static int valid_pointer(uint32_t pointer)
{
    uint32_t physical;
    if (pointer <= XG_RAM_LAST_WORD) physical = pointer;
    else if (pointer >= XG_CACHED_BASE && pointer <= XG_CACHED_BASE + XG_RAM_LAST_WORD) physical = pointer - XG_CACHED_BASE;
    else if (pointer >= XG_UNCACHED_BASE && pointer <= XG_UNCACHED_BASE + XG_RAM_LAST_WORD) physical = pointer - XG_UNCACHED_BASE;
    else return 0;
    return physical != 0u && (physical & 3u) == 0u;
}

void psx_xenogears_timing_read_scene(XgTimingScene *out)
{
    XgTimingScene scene = { 0 };
    if (!out) return;
    scene.field_context = read_u32(XG_FIELD_CONTEXT);
    scene.requested_module = read_u32(XG_MODULE_REQUESTED);
    scene.active_module = read_u32(XG_MODULE_ACTIVE);
    scene.module_pointer = read_u32(XG_MODULE_POINTER);
    scene.raw_field_id = read_u16(XG_FIELD_ID);
    scene.masked_field_id = scene.raw_field_id & XG_FIELD_ID_MASK;
    if (valid_pointer(scene.field_context)) {
        scene.field_id = scene.masked_field_id;
        scene.game_progress = read_u16(XG_GAME_PROGRESS);
        scene.valid_field = (uint8_t)(scene.masked_field_id < XG_FIELD_ID_LIMIT);
    }
    *out = scene;
}

static int same_scene(const XgTimingScene *a, const XgTimingScene *b)
{
    if (a->valid_field != b->valid_field) return 0;
    return !a->valid_field || (a->field_context == b->field_context && a->field_id == b->field_id);
}

void psx_xenogears_timing_reset(XgTimingReason reason)
{
    s_timing.effective_mode = NATIVE_FPS_MODE_ORIGINAL;
    s_timing.route = XG_TIMING_ROUTE_NONE;
    s_timing.reason = reason;
    s_timing.have_scene = s_timing.fmv_active = s_timing.active = 0u;
    s_timing.have_counted_frame_token = 0u;
    memset(&s_timing.scene, 0, sizeof(s_timing.scene));
    if (reason == XG_TIMING_REASON_BOOT) s_timing.authored_updates = 0u;
}

int psx_xenogears_timing_set_startup_policy(const NativeFpsStartupPolicy *policy)
{
    NativeFpsStartupPolicy next = { NATIVE_FPS_MODE_ORIGINAL, NATIVE_FPS_STARTUP_ORIGINAL_DEFAULT, 0 };
    if (policy) next = *policy;
    if (next.requested_mode != NATIVE_FPS_MODE_NATIVE_59_94) next.requested_mode = NATIVE_FPS_MODE_ORIGINAL;
    if (s_timing.have_scene && s_timing.scene.valid_field) { s_timing.reason = XG_TIMING_REASON_REQUEST_FROZEN; return 0; }
    s_timing.policy = next;
    return 1;
}

void psx_xenogears_timing_on_savestate_loaded(void) { psx_xenogears_timing_reset(XG_TIMING_REASON_SAVESTATE_LOAD); }

void psx_xenogears_timing_vblank_boundary(int fmv_active)
{
    XgTimingScene scene;
    const uint8_t fmv = (uint8_t)(fmv_active != 0);
    psx_xenogears_timing_read_scene(&scene);
    if (fmv != s_timing.fmv_active) { psx_xenogears_timing_reset(XG_TIMING_REASON_FMV_BOUNDARY); s_timing.fmv_active = fmv; }
    if (s_timing.fmv_active || (s_timing.have_scene && same_scene(&s_timing.scene, &scene))) return;
    psx_xenogears_timing_reset(XG_TIMING_REASON_SCENE_TRANSITION);
    s_timing.scene = scene; s_timing.have_scene = 1u; s_timing.generation++;
    if (!scene.valid_field) { s_timing.reason = XG_TIMING_REASON_WAITING_FIELD; return; }
    s_timing.route = XG_TIMING_ROUTE_FIELD_DEV;
    s_timing.reason = s_timing.policy.requested_mode == NATIVE_FPS_MODE_NATIVE_59_94
        ? XG_TIMING_REASON_FIELD_DEV : XG_TIMING_REASON_ORIGINAL_MODE;
}

int32_t psx_xenogears_timing_field_frame_step(const XgFieldFrameContext *c, int32_t step)
{
    if (!c || step != 2 || s_timing.route != XG_TIMING_ROUTE_FIELD_DEV ||
        c->load_base != 0x8006E800u || c->logical_identity != 0xBBB22575u ||
        c->site_pc != 0x800758E4u || c->instruction_word != 0x24630002u ||
        (c->tier != XG_FIELD_TIER_COLD_INTERPRETER && c->tier != XG_FIELD_TIER_WARM_NATIVE)) return step;
    const int first_poll = !s_timing.have_counted_frame_token ||
        c->frame_token != s_timing.last_counted_frame_token;
    if (first_poll) {
        s_timing.last_counted_frame_token = c->frame_token;
        s_timing.have_counted_frame_token = 1u;
        s_timing.authored_updates++;
    }
    if (s_timing.policy.requested_mode != NATIVE_FPS_MODE_NATIVE_59_94) return step;
    s_timing.active = 1u;
    s_timing.effective_mode = NATIVE_FPS_MODE_NATIVE_59_94;
    if (first_poll) s_timing.helper_activations++;
    return 1;
}

NativeFpsMode psx_xenogears_timing_effective_mode(void) { return s_timing.active ? s_timing.effective_mode : NATIVE_FPS_MODE_ORIGINAL; }
NativeFpsMode psx_xenogears_timing_requested_mode(void) { return s_timing.policy.requested_mode; }
XgTimingRoute psx_xenogears_timing_route(void) { return s_timing.route; }
XgTimingReason psx_xenogears_timing_reason(void) { return s_timing.reason; }
const char *psx_xenogears_timing_route_name(void) { return s_timing.route == XG_TIMING_ROUTE_FIELD_DEV ? "field_dev" : "none"; }
const char *psx_xenogears_timing_reason_name(void)
{
    static const char *names[] = { "boot", "original_mode", "waiting_field", "field_dev", "scene_transition", "fmv_boundary", "savestate_load", "request_frozen" };
    return (unsigned)s_timing.reason < sizeof(names) / sizeof(names[0]) ? names[s_timing.reason] : "waiting_field";
}
uint32_t psx_xenogears_timing_scene_generation(void) { return s_timing.generation; }
uint64_t psx_xenogears_timing_accumulator(void) { return 0u; }
int psx_xenogears_timing_field_route_active(void) { return s_timing.active != 0u; }
uint64_t psx_xenogears_timing_field_helper_activations(void) { return s_timing.helper_activations; }
uint64_t psx_xenogears_timing_field_authored_updates(void) { return s_timing.authored_updates; }
void psx_xenogears_timing_field_invalidate(void) { s_timing.active = 0u; s_timing.effective_mode = NATIVE_FPS_MODE_ORIGINAL; s_timing.have_counted_frame_token = 0u; }
uint32_t psx_xenogears_timing_field_delta_numerator(void) { return 1u; }
uint32_t psx_xenogears_timing_field_delta_denominator(void) { return 1u; }
uint32_t psx_xenogears_timing_field_accumulator_step(void) { return 1u; }
int psx_xenogears_timing_field_logical_tick(void) { return 1; }
XgFieldTickPhase psx_xenogears_timing_field_tick_phase(void) { return XG_FIELD_TICK_ORIGINAL; }
const char *psx_xenogears_timing_field_tick_phase_name(void) { return "original"; }
int32_t psx_xenogears_timing_field_integrate_velocity(uint32_t a, uint32_t b, int32_t p, int32_t v, int32_t x) { (void)a; (void)b; (void)x; return p - v; }
