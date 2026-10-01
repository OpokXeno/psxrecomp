/* Shared Xenogears free camera, used by the player menu and debug overlay.
 * Camera ownership, pose formats and restoration are identical in both builds. */
#include "free_camera.h"
#ifdef PSX_FREE_CAMERA
#include "memory.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>

extern "C" int psx_netplay_active(void);
extern "C" int psx_input_runtime_bindings_available(void);
extern "C" uint8_t psx_read_byte(uint32_t address);
extern "C" void psx_write_byte(uint32_t address, uint8_t value);
static SDL_Window* s_game_window = nullptr;
static SDL_Window* s_input_window = nullptr;
static bool s_input_blocked = false;
static constexpr uint32_t kAddr_overlaySignature = 0x8006FAF0u;
static constexpr uint32_t kSigFieldOverlay = 4u;
static constexpr uint32_t kSigWorldOverlay = 5u;
static constexpr uint32_t kSigBattleOverlay = 6u;
static constexpr uint32_t kSigBattlingOverlay = 7u;
/* State for the Free Camera panel (W6) — rewritten with game knowledge.
 *
 * FIELD camera truth (docs/xenogears/field/09-camera-control.md +
 * field-overlay FUN_80073230 / FUN_80072D74, verified live in Ghidra):
 * - 0x800AF934 mode: 0 = follow, 1 = script-controlled, 2 = reacquire.
 *   Mode 0 recomputes the desired pose from the tracked actor every frame,
 *   so eye/at writes are clobbered — the old panel's failure mode.
 * - Current eye 0x800AF880/884/888 + current target ("at") 0x800AF890/894/898
 *   + desired eye 0x800AF8B0/B4/B8 + desired target 0x800AF8C0/C4/C8 are
 *   32-bit 16.16 fixed (integer part = PSX world units, >>0x10). The old
 *   panel wrote 3 x s16 at +0/+2/+4, straddling the fixed words (Xlo/Xhi/Ylo)
 *   — pure corruption. Writes must be 6 x u32 LE (current + desired).
 * - Smoothing divisors 0x800AF984 (target) / 0x800AF988 (eye): current
 *   interpolates toward desired by (desired-current)/divisor each frame.
 *   divisor=1 snaps next frame — what free camera wants.
 * - Lock flag 0x8000 at 0x800AF9D8 suppresses manual L1/R1 orbit.
 * - Shake state at 0x800AFA28 adds an XYZ offset to eye+target every frame.
 * Freeze: save {mode, divT, divE, lock, shake} -> {mode=1, divT=1, divE=1,
 * lock|=0x8000, shake=0} -> pose in s_cam_eye_f/s_cam_at_f written to
 * current+desired every pre_swap; restore on release.
 *
 * WORLD camera truth (world overlay FUN_80091c18/80097440/80096f18 +
 * native-renderer CLOUD_* authenticated addresses, verified in Ghidra):
 * - The camera is an ORBIT camera by construction: absolute eye is never
 *   stored; per frame ComputeCameraVector derives it from origin (followed
 *   target, 3 x u32 12.12 fixed at 0x8009BE28/2C/30) + yaw/pitch (12-bit
 *   turn, u16/s16 at 0x8009BD3A/38) + distance (u32 12.12 at 0x8009D3F0).
 *   The view matrix at 0x8009C808 is then built from those.
 * - Slot 9 (WorldCameraTaskUpdate 0x800914D0) follows the travel target and
 *   feeds streaming — it never writes the eye, so it keeps running (flying
 *   stays inside streamed terrain around the party).
 * - Slot 9 (0x800914D0) follows the travel target and feeds streaming; its
 *   sub-modes 0/1/2/0x10 also chase yaw toward the travel heading (mode 3 =
 *   translational follow only, no yaw chase — that chase is what felt like
 *   "inertia" when driving yaw externally).
 * - Slot 10 (0x80091C18) eases dist/pitch toward its chase targets
 *   (+0x54/+0x58/+0x60) and always derives the eye from the live inputs.
 * Freeze: verify slots 9/10 hold the ordinary updaters in steady update,
 * save slot9 fn + slot10 chase targets, suspend slot 9 (its follow chase
 * would fight the free origin; slot 10 keeps running and derives the eye
 * from our inputs). Every frame: fly the origin in 3D along the engine's
 * own view basis, drag streaming with its XZ deltas, write yaw/pitch/dist
 * + chase targets so slot 10 sees "settled". Release writes slot9 mode 0
 * (evaluate) so yaw/dist/pitch glide home — never the stale saved mode
 * (3 = idle never chases yaw). If slot 9 is reassigned, stop and report.
 *
 * BATTLE camera truth (battle-overlay FUN_800bbab8/800bc2f0 + annotations):
 * - Current eye/target are 3 x s16 at 0x800D3354/56/58 and 0x800D335C/5E/60;
 *   desired packs halves into 0x800D30A0 (eye) / 0x800D30A8 (target):
 *   word.lo<->c0, word.hi<->c1, next.lo<->c2 (high halves preserved).
 * - Mode at 0x800C3CC0 (1 = auto-frame via ComputeCameraParams, 2/3 =
 *   control/event tracks); gate at 0x800C3CBC (1 = interpolate).
 * - BattleSetCameraMode(4) is the engine's own freeze: no desired recompute
 *   and gate=5 skips interpolation. Control-sprite tracks only feed the
 *   mode-2 input state (0x8006F99C/AC), inert in mode 4.
 * Freeze: save {mode, gate} -> {mode=4, gate=5} -> write current+desired
 * every pre_swap; restore on release. Attack/event cameras are suppressed
 * while enabled.
 *
 * BATTLING camera truth (battling FUN_8007099c/80070808 + docs 08 §8):
 * - Per-frame BattlingUpdateCameraMode(preset @ 0x80092904, u32) drives
 *   eye (3 x u32 at 0x8009871C/20/24) and target (3 x u32 at
 *   0x8009867C/80/84) from presets 0-5; an invalid preset falls through
 *   the switch and writes nothing.
 * Freeze: save preset -> write 6 (invalid) -> write eye+target every
 * pre_swap; restore on release. Bout-mode scope: victory/replay orbits
 * write the eye directly and override the freeze.
 *
 * Native renderer note: interp and native paths both consume the guest
 * vectors/matrix built from the above state, so freezing + overriding
 * drives both. */
static bool     s_camera_enabled     = false;
static int      s_cam_frozen_mod     = -1;     /* overlay sig 4/5/6/7, -1 = none */
static bool     s_cam_saved_valid    = false;
static uint32_t s_cam_saved_mode     = 0;
static uint32_t s_cam_saved_div_t    = 0;
static uint32_t s_cam_saved_div_e    = 0;
static uint32_t s_cam_saved_lock     = 0;
static uint32_t s_cam_saved_shake    = 0;
static float    s_cam_eye_f[3]       = {0.0f, 0.0f, 0.0f};   /* PSX units (field/battle/battling) */
static float    s_cam_at_f[3]        = {0.0f, 0.0f, 500.0f};
/* World orbit editor: 12-bit turn yaw (0-4095), 12-bit pitch, dist in units,
 * plus the free origin (look target) in units. */
static int      s_w_yaw              = 0;
static int      s_w_pitch            = 0;
static float    s_w_dist_f           = 1024.0f;
static float    s_w_org_f[3]         = {0.0f, 0.0f, 0.0f};
/* World saved sub-state (slot10 chase targets + slot9 fn). The slot9
 * sub-mode is NOT restored: on release we write mode 0 (evaluate) so the
 * engine rechases yaw/dist/pitch back to the follow solution — restoring
 * a stale mode (typically 3 = idle, which never chases yaw) would leave
 * the camera staring the wrong way forever. */
static uint32_t s_w_saved_tdist      = 0;
static uint32_t s_w_saved_tpitch     = 0;
static uint32_t s_w_saved_pacc       = 0;
static uint32_t s_w_saved_fn         = 0;
static bool     s_camera_keys_enable = true;
static bool     s_cam_mouse_look     = true;    /* right-drag looks */
static bool     s_cam_capture_input  = true;    /* mask game pad/keys while flying */
static bool     s_cam_wheel_dolly    = true;    /* wheel dollies (consumes the event) */
static bool     s_cam_invert_y       = false;
static float    s_camera_fly_speed   = 32.0f;   /* PSX units per frame (keys) */
static float    s_camera_rot_speed   = 0.05f;   /* rad per arrow-key frame */
static float    s_cam_mouse_sens     = 0.005f;  /* rad per pixel */
static float    s_cam_wheel_step     = 64.0f;   /* PSX units per wheel notch */
static bool     s_cam_pan_enable     = true;    /* left-drag pans (grab the map) */
static bool     s_cam_panning        = false;   /* left-drag pan latched */
static float    s_cam_pan_factor     = 0.002f;  /* pan = dist * factor per pixel */
static int      s_cam_last_mx        = 0;
static int      s_cam_last_my        = 0;
static bool     s_cam_mouse_init     = false;
static float    s_cam_wheel_accum    = 0.0f;    /* notches, accumulated in process_event */
static float    s_cam_dt_scale       = 1.0f;    /* frame dt / 16.67ms: key motion is
                                                * framerate-normalized (mouse deltas
                                                * and wheel notches already are) */
static float    s_cam_hz             = 0.0f;    /* pre_swap EMA rate (diagnostics) */
static uint64_t s_cam_last_ticks     = 0u;
static int      s_camera_status_frames = 0;
static char     s_camera_status[128]  = {0};

/* Field camera vectors (3 x s32 16.16 fixed-point). */
static constexpr uint32_t kAddr_cameraEye           = 0x800AF880u;
static constexpr uint32_t kAddr_cameraEyeY          = 0x800AF884u;
static constexpr uint32_t kAddr_cameraEyeZ          = 0x800AF888u;
static constexpr uint32_t kAddr_cameraAt            = 0x800AF890u;
static constexpr uint32_t kAddr_cameraAtY           = 0x800AF894u;
static constexpr uint32_t kAddr_cameraAtZ           = 0x800AF898u;
/* Desired pose + freeze state (field-overlay FUN_80073230/FUN_80072D74).
 * Current eye/target interpolate toward desired by (d-c)/divisor; the
 * integer part (>>0x10) is PSX world units. Mode 0 rebuilds desired from
 * the tracked actor every frame; mode 1 (script) leaves desired alone. */
static constexpr uint32_t kAddr_cameraDesEye        = 0x800AF8B0u;
static constexpr uint32_t kAddr_cameraDesAt         = 0x800AF8C0u;
static constexpr uint32_t kAddr_cameraMode          = 0x800AF934u;
static constexpr uint32_t kAddr_cameraDivT          = 0x800AF984u;
static constexpr uint32_t kAddr_cameraDivE          = 0x800AF988u;
static constexpr uint32_t kAddr_cameraLock          = 0x800AF9D8u;
static constexpr uint32_t kAddr_cameraShake         = 0x800AFA28u;
static constexpr uint32_t kCameraLockBit            = 0x8000u;
/* Battle camera (battle-overlay FUN_800bbab8 update / FUN_800bc2f0 mode).
 * Current eye/target: 3 x s16. Desired packs halves: word.lo<->c0,
 * word.hi<->c1, next.lo<->c2 (next.hi preserved). Mode 4 + gate 5 is the
 * engine's own freeze (BattleSetCameraMode(4)). */
static constexpr uint32_t kAddr_battleCamMode       = 0x800C3CC0u;
static constexpr uint32_t kAddr_battleCamGate       = 0x800C3CBCu;
static constexpr uint32_t kAddr_battleCamEye       = 0x800D3354u;
static constexpr uint32_t kAddr_battleCamAt        = 0x800D335Cu;
static constexpr uint32_t kAddr_battleCamDesEye    = 0x800D30A0u;
static constexpr uint32_t kAddr_battleCamDesAt     = 0x800D30A8u;
/* Battling camera (FUN_8007099c preset switch / FUN_80070808 interp).
 * Eye/target: 3 x u32 (plain s32 units). Preset u32 at 0x80092904;
 * any value >5 falls through the switch and writes nothing. */
static constexpr uint32_t kAddr_battlingCamEye     = 0x8009871Cu;
static constexpr uint32_t kAddr_battlingCamAt      = 0x8009867Cu;
static constexpr uint32_t kAddr_battlingCamPreset  = 0x80092904u;
static constexpr uint32_t kBattlingCamFrozenPreset = 6u;
/* World orbit camera (FUN_80091c18 task / FUN_80097440 Euler builder /
 * FUN_80096f18 vector derivation). Origin: 3 x u32 12.12 fixed (X,Y,Z).
 * Yaw/pitch: 12-bit turn (u16/s16). Distance: u32 12.12 fixed.
 * Task array: pointer at 0x8009BE24, slots 0x80 bytes; slot 9 = follow,
 * slot 10 = eye writer (ordinary fn 0x80091C18, active flag/fn at +0x1C,
 * state short at +0x00, steady update = state 1). */
static constexpr uint32_t kAddr_worldCamOrigin     = 0x8009BE28u;
static constexpr uint32_t kAddr_worldCamPitch      = 0x8009BD38u;
static constexpr uint32_t kAddr_worldCamYaw        = 0x8009BD3Au;
static constexpr uint32_t kAddr_worldCamDist       = 0x8009D3F0u;
static constexpr uint32_t kAddr_worldTaskArrayPtr  = 0x8009BE24u;
static constexpr uint32_t kWorldCamSlotStride      = 0x80u;
static constexpr uint32_t kWorldCamSlotStateOff    = 0x00u;
static constexpr uint32_t kWorldCamSlotFnOff       = 0x1Cu;
static constexpr uint32_t kWorldCamSlot9           = 9u;
static constexpr uint32_t kWorldCamSlot10          = 10u;
static constexpr uint32_t kWorldCamSlot9ModeOff    = 0x20u;
static constexpr uint32_t kWorldCamSlot10ModeOff   = 0x20u;
static constexpr uint32_t kWorldCamSlot10TDistOff  = 0x54u;
static constexpr uint32_t kWorldCamSlot10TPitchOff = 0x58u;
static constexpr uint32_t kWorldCamSlot10PAccOff   = 0x60u;
static constexpr uint32_t kWorldCamSlot9Fn         = 0x800914D0u;
static constexpr uint32_t kWorldCamSlot10Fn        = 0x80091C18u;
/* View matrix (0x20 bytes: 3x3 s16 rotation row-major + 3x s32 translation)
 * and streaming accumulators (XZ, same units as origin deltas). The matrix
 * is GTE +Z-forward: row 2 = view forward, row 0 = view right. */
static constexpr uint32_t kAddr_worldCamMatrix      = 0x8009C808u;
static constexpr uint32_t kAddr_worldStreamX        = 0x8009BBB4u;
static constexpr uint32_t kAddr_worldStreamZ        = 0x8009BBBCu;
/* Write a u32 LE to guest RAM. 4 byte writes via psx_write_byte (same
 * accessor the debug_server's write_ram uses, so the byte path is
 * identical and bit-identical). */
static void write_u32_le(uint32_t addr, uint32_t val)
{
    psx_write_byte(addr + 0, (uint8_t)(val & 0xFFu));
    psx_write_byte(addr + 1, (uint8_t)((val >> 8)  & 0xFFu));
    psx_write_byte(addr + 2, (uint8_t)((val >> 16) & 0xFFu));
    psx_write_byte(addr + 3, (uint8_t)((val >> 24) & 0xFFu));
}

/* Write a u16 LE to guest RAM. */
static void write_u16_le(uint32_t addr, uint16_t val)
{
    psx_write_byte(addr + 0, (uint8_t)(val & 0xFFu));
    psx_write_byte(addr + 1, (uint8_t)((val >> 8) & 0xFFu));
}

static uint16_t read_u16_le(uint32_t addr)
{
    return (uint16_t)((uint32_t)psx_read_byte(addr) |
                      ((uint32_t)psx_read_byte(addr + 1) << 8));
}

static uint32_t read_u32_le(uint32_t addr)
{
    return  (uint32_t)psx_read_byte(addr)
         | ((uint32_t)psx_read_byte(addr + 1) << 8)
         | ((uint32_t)psx_read_byte(addr + 2) << 16)
         | ((uint32_t)psx_read_byte(addr + 3) << 24);
}

/* Resident module for panel readouts, derived from the same signature:
 * 1=Field 2=Battle 3=Worldmap 4=Battling (semantic ids), -1 = other/loading. */
static int resident_loaded_module(void)
{
    uint32_t sig = read_u32_le(kAddr_overlaySignature);
    if (sig == kSigFieldOverlay)    return 1;
    if (sig == kSigBattleOverlay)   return 2;
    if (sig == kSigWorldOverlay)    return 3;
    if (sig == kSigBattlingOverlay) return 4;
    return -1;
}

static const char *resident_module_name(int id)
{
    switch (id) {
        case 1:  return "Field";
        case 2:  return "Battle";
        case 3:  return "Worldmap";
        case 4:  return "Battling";
        default: return "other/loading";
    }
}

/* Raw overlay signature (4/5/6/7) or 0 when no game overlay is resident.
 * Free camera supports all four; anything else releases the freeze. */
static uint32_t resident_overlay_sig(void)
{
    uint32_t sig = read_u32_le(kAddr_overlaySignature);
    if (sig == kSigFieldOverlay || sig == kSigWorldOverlay ||
        sig == kSigBattleOverlay || sig == kSigBattlingOverlay) return sig;
    return 0u;
}

/* Free-camera write. Args are PSX world units (integers), clamped to s16
 * for field/battle and to +-16M for battling (plain s32 there).
 * Field: 6 x u32 fixed16 current+desired (eye 0x800AF880, at 0x800AF890,
 * desEye 0x800AF8B0, desAt 0x800AF8C0).
 * Battle: 3 x s16 current (eye 0x800D3354, at 0x800D335C) + desired halves
 * packed into 0x800D30A0/A8 (next-word high halves preserved).
 * Battling: 3 x u32 eye (0x8009871C) + target (0x8009867C).
 * Mirrors the pose into the panel's float editor. One-shot unless frozen
 * (the owner logic reclaims the pose next frame); held every frame while
 * enabled. World orbit mode has no positional pose — returns -3 there.
 * Returns 0 on success, negative otherwise. */
int psx_free_camera_write(int ex, int ey, int ez, int ax, int ay, int az)
{
    uint32_t sig = resident_overlay_sig();
    if (sig == kSigWorldOverlay) return -3;
    if (sig != kSigFieldOverlay && sig != kSigBattleOverlay &&
        sig != kSigBattlingOverlay) {
        /* Headless/one-shot use outside the camera modules still updates
         * the editor (field layout) so a later field entry starts there. */
        sig = kSigFieldOverlay;
    }
    auto clamp_s16 = [](int v) -> int {
        if (v < -32768) return -32768;
        if (v >  32767) return  32767;
        return v;
    };
    auto clamp_s32m = [](int v) -> int32_t {
        if (v < -16777216) return -16777216;
        if (v >  16777216) return  16777216;
        return (int32_t)v;
    };
    if (sig == kSigBattlingOverlay) {
        int32_t e[3] = { clamp_s32m(ex), clamp_s32m(ey), clamp_s32m(ez) };
        int32_t a[3] = { clamp_s32m(ax), clamp_s32m(ay), clamp_s32m(az) };
        s_cam_eye_f[0] = (float)e[0]; s_cam_eye_f[1] = (float)e[1]; s_cam_eye_f[2] = (float)e[2];
        s_cam_at_f[0]  = (float)a[0]; s_cam_at_f[1]  = (float)a[1]; s_cam_at_f[2]  = (float)a[2];
        for (int i = 0; i < 3; i++) {
            write_u32_le(kAddr_battlingCamEye + (uint32_t)i * 4u, (uint32_t)e[i]);
            write_u32_le(kAddr_battlingCamAt + (uint32_t)i * 4u, (uint32_t)a[i]);
        }
        return 0;
    }
    int e[3] = { clamp_s16(ex), clamp_s16(ey), clamp_s16(ez) };
    int a[3] = { clamp_s16(ax), clamp_s16(ay), clamp_s16(az) };
    s_cam_eye_f[0] = (float)e[0]; s_cam_eye_f[1] = (float)e[1]; s_cam_eye_f[2] = (float)e[2];
    s_cam_at_f[0]  = (float)a[0]; s_cam_at_f[1]  = (float)a[1]; s_cam_at_f[2]  = (float)a[2];
    if (sig == kSigBattleOverlay) {
        for (int i = 0; i < 3; i++) {
            write_u16_le(kAddr_battleCamEye + (uint32_t)i * 2u, (uint16_t)e[i]);
            write_u16_le(kAddr_battleCamAt + (uint32_t)i * 2u, (uint16_t)a[i]);
        }
        uint32_t d0 = read_u32_le(kAddr_battleCamDesEye);
        uint32_t d1 = read_u32_le(kAddr_battleCamDesEye + 4u);
        uint32_t d2 = read_u32_le(kAddr_battleCamDesAt);
        uint32_t d3 = read_u32_le(kAddr_battleCamDesAt + 4u);
        d0 = (d0 & 0xFFFF0000u) | ((uint32_t)(uint16_t)e[0]);
        d0 = ((uint32_t)(uint16_t)e[1] << 16) | (d0 & 0x0000FFFFu);
        d1 = (d1 & 0xFFFF0000u) | ((uint32_t)(uint16_t)e[2]);
        d2 = (d2 & 0xFFFF0000u) | ((uint32_t)(uint16_t)a[0]);
        d2 = ((uint32_t)(uint16_t)a[1] << 16) | (d2 & 0x0000FFFFu);
        d3 = (d3 & 0xFFFF0000u) | ((uint32_t)(uint16_t)a[2]);
        write_u32_le(kAddr_battleCamDesEye, d0);
        write_u32_le(kAddr_battleCamDesEye + 4u, d1);
        write_u32_le(kAddr_battleCamDesAt, d2);
        write_u32_le(kAddr_battleCamDesAt + 4u, d3);
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        uint32_t eye_fixed = (uint32_t)((int32_t)e[i] << 16);
        uint32_t at_fixed  = (uint32_t)((int32_t)a[i] << 16);
        write_u32_le(kAddr_cameraEye + (uint32_t)i * 4u, eye_fixed);
        write_u32_le(kAddr_cameraDesEye + (uint32_t)i * 4u, eye_fixed);
        write_u32_le(kAddr_cameraAt + (uint32_t)i * 4u, at_fixed);
        write_u32_le(kAddr_cameraDesAt + (uint32_t)i * 4u, at_fixed);
    }
    return 0;
}

/* Snapshot the live pose into the editor. Positional modules fill
 * s_cam_eye_f/at_f; world fills s_w_yaw/pitch/dist. */
static void camera_pull_live(uint32_t sig)
{
    if (sig == kSigWorldOverlay) {
        s_w_yaw = (int)(read_u16_le(kAddr_worldCamYaw) & 0x0FFFu);
        s_w_pitch = (int)(int16_t)read_u16_le(kAddr_worldCamPitch);
        s_w_dist_f = (float)(int32_t)read_u32_le(kAddr_worldCamDist) / 4096.0f;
        s_w_org_f[0] = (float)(int32_t)read_u32_le(kAddr_worldCamOrigin + 0) / 4096.0f;
        s_w_org_f[1] = (float)(int32_t)read_u32_le(kAddr_worldCamOrigin + 4) / 4096.0f;
        s_w_org_f[2] = (float)(int32_t)read_u32_le(kAddr_worldCamOrigin + 8) / 4096.0f;
        return;
    }
    if (sig == kSigBattleOverlay) {
        s_cam_eye_f[0] = (float)(int16_t)read_u16_le(kAddr_battleCamEye + 0);
        s_cam_eye_f[1] = (float)(int16_t)read_u16_le(kAddr_battleCamEye + 2);
        s_cam_eye_f[2] = (float)(int16_t)read_u16_le(kAddr_battleCamEye + 4);
        s_cam_at_f[0]  = (float)(int16_t)read_u16_le(kAddr_battleCamAt + 0);
        s_cam_at_f[1]  = (float)(int16_t)read_u16_le(kAddr_battleCamAt + 2);
        s_cam_at_f[2]  = (float)(int16_t)read_u16_le(kAddr_battleCamAt + 4);
        return;
    }
    if (sig == kSigBattlingOverlay) {
        s_cam_eye_f[0] = (float)(int32_t)read_u32_le(kAddr_battlingCamEye + 0);
        s_cam_eye_f[1] = (float)(int32_t)read_u32_le(kAddr_battlingCamEye + 4);
        s_cam_eye_f[2] = (float)(int32_t)read_u32_le(kAddr_battlingCamEye + 8);
        s_cam_at_f[0]  = (float)(int32_t)read_u32_le(kAddr_battlingCamAt + 0);
        s_cam_at_f[1]  = (float)(int32_t)read_u32_le(kAddr_battlingCamAt + 4);
        s_cam_at_f[2]  = (float)(int32_t)read_u32_le(kAddr_battlingCamAt + 8);
        return;
    }
    s_cam_eye_f[0] = (float)(int32_t)read_u32_le(kAddr_cameraEye) / 65536.0f;
    s_cam_eye_f[1] = (float)(int32_t)read_u32_le(kAddr_cameraEyeY) / 65536.0f;
    s_cam_eye_f[2] = (float)(int32_t)read_u32_le(kAddr_cameraEyeZ) / 65536.0f;
    s_cam_at_f[0]  = (float)(int32_t)read_u32_le(kAddr_cameraAt) / 65536.0f;
    s_cam_at_f[1]  = (float)(int32_t)read_u32_le(kAddr_cameraAtY) / 65536.0f;
    s_cam_at_f[2]  = (float)(int32_t)read_u32_le(kAddr_cameraAtZ) / 65536.0f;
}

/* World task slot base address, or 0 when the task array pointer is
 * insane (wrong module BSS — never touch it). */
static uint32_t world_cam_slot_base(uint32_t slot)
{
    uint32_t arr = read_u32_le(kAddr_worldTaskArrayPtr);
    if (arr < 0x80000000u || arr > 0x80200000u - (slot + 1u) * kWorldCamSlotStride)
        return 0u;
    return arr + slot * kWorldCamSlotStride;
}

/* True when slots 9/10 hold the ordinary travel-camera updaters in steady
 * update (dispatch state 1). Anything else = cinematic/transition scope:
 * refuse the freeze instead of fighting (or corrupting) another owner. */
static bool world_cam_ordinary(uint32_t *out_s9, uint32_t *out_s10)
{
    uint32_t s9 = world_cam_slot_base(kWorldCamSlot9);
    uint32_t s10 = world_cam_slot_base(kWorldCamSlot10);
    if (s9 == 0u || s10 == 0u) return false;
    if (read_u16_le(s9 + kWorldCamSlotStateOff) != 1u ||
        read_u16_le(s10 + kWorldCamSlotStateOff) != 1u) return false;
    if (read_u32_le(s9 + kWorldCamSlotFnOff) != kWorldCamSlot9Fn ||
        read_u32_le(s10 + kWorldCamSlotFnOff) != kWorldCamSlot10Fn) return false;
    if (out_s9) *out_s9 = s9;
    if (out_s10) *out_s10 = s10;
    return true;
}

/* Engage the freeze for the resident module. Pulls the live pose first so
 * enabling never jumps the camera. Safe on the idle edge only (callers
 * check s_cam_frozen_mod). Returns true when frozen. */
static bool camera_freeze(uint32_t sig)
{
    if (s_cam_frozen_mod >= 0 || sig == 0u) return false;
    if (sig == kSigWorldOverlay) {
        uint32_t s9 = 0u, s10 = 0u;
        if (!world_cam_ordinary(&s9, &s10)) return false;
        camera_pull_live(sig);
        s_w_saved_tdist = read_u32_le(s10 + kWorldCamSlot10TDistOff);
        s_w_saved_tpitch = read_u32_le(s10 + kWorldCamSlot10TPitchOff);
        s_w_saved_pacc = read_u32_le(s10 + kWorldCamSlot10PAccOff);
        /* Suspend slot 9 (its follow chase would fight the free origin);
         * slot 10 keeps running and derives the eye from our inputs. */
        s_w_saved_fn = read_u32_le(s9 + kWorldCamSlotFnOff);
        s_cam_saved_valid = true;
        write_u32_le(s9 + kWorldCamSlotFnOff, 0u);
        s_cam_frozen_mod = (int)sig;
        s_cam_mouse_init = false;
        return true;
    }
    camera_pull_live(sig);
    if (sig == kSigBattleOverlay) {
        s_cam_saved_mode = read_u32_le(kAddr_battleCamMode);
        s_cam_saved_div_t = read_u32_le(kAddr_battleCamGate);
        s_cam_saved_valid = true;
        write_u32_le(kAddr_battleCamMode, 4u);
        write_u32_le(kAddr_battleCamGate, 5u);
    } else if (sig == kSigBattlingOverlay) {
        s_cam_saved_mode = read_u32_le(kAddr_battlingCamPreset);
        s_cam_saved_valid = true;
        write_u32_le(kAddr_battlingCamPreset, kBattlingCamFrozenPreset);
    } else {
        s_cam_saved_mode  = read_u32_le(kAddr_cameraMode);
        s_cam_saved_div_t = read_u32_le(kAddr_cameraDivT);
        s_cam_saved_div_e = read_u32_le(kAddr_cameraDivE);
        s_cam_saved_lock  = read_u32_le(kAddr_cameraLock);
        s_cam_saved_shake = read_u32_le(kAddr_cameraShake);
        s_cam_saved_valid = true;
        write_u32_le(kAddr_cameraMode, 1u);
        write_u32_le(kAddr_cameraDivT, 1u);
        write_u32_le(kAddr_cameraDivE, 1u);
        write_u32_le(kAddr_cameraLock, s_cam_saved_lock | kCameraLockBit);
        write_u32_le(kAddr_cameraShake, 0u);
    }
    s_cam_frozen_mod = (int)sig;
    s_cam_mouse_init = false;
    return true;
}

/* Release the freeze, restoring engine state. World restores the slot
 * sub-state only while the slots still hold the ordinary updaters (a
 * cinematic reassignment means the engine owns them again). */
static void camera_unfreeze(void)
{
    if (s_cam_frozen_mod < 0) return;
    uint32_t sig = (uint32_t)s_cam_frozen_mod;
    if (s_cam_saved_valid && resident_overlay_sig() == sig) {
        if (sig == kSigWorldOverlay) {
            uint32_t s9 = world_cam_slot_base(kWorldCamSlot9);
            uint32_t s10 = 0u, dummy = 0u;
            /* Restore slot 9 only if it still holds our 0 (a cinematic
             * reassignment means the engine owns it again). */
            if (s9 != 0u && read_u32_le(s9 + kWorldCamSlotFnOff) == 0u)
                write_u32_le(s9 + kWorldCamSlotFnOff, s_w_saved_fn);
            if (world_cam_ordinary(&dummy, &s10)) {
                /* Mode 0 (evaluate), not the stale saved mode: rechases
                 * yaw to the follow solution so the camera glides home
                 * instead of staring off forever. Dist/pitch snap to the
                 * saved chase targets instead: the engine's dist ease is
                 * capped (0x8000/frame) and would take ~30 s from far away.
                 * Yaw + origin glide back smoothly on their own. */
                int16_t home_pitch = (int16_t)s_w_saved_tpitch;
                write_u16_le(kAddr_worldCamPitch, (uint16_t)home_pitch);
                write_u32_le(kAddr_worldCamDist, s_w_saved_tdist);
                write_u16_le(s9 + kWorldCamSlot9ModeOff, 0u);
                write_u32_le(s10 + kWorldCamSlot10TDistOff, s_w_saved_tdist);
                write_u32_le(s10 + kWorldCamSlot10TPitchOff, s_w_saved_tpitch);
                write_u32_le(s10 + kWorldCamSlot10PAccOff, s_w_saved_pacc);
            }
        } else if (sig == kSigBattleOverlay) {
            write_u32_le(kAddr_battleCamMode, s_cam_saved_mode);
            write_u32_le(kAddr_battleCamGate, s_cam_saved_div_t);
        } else if (sig == kSigBattlingOverlay) {
            write_u32_le(kAddr_battlingCamPreset, s_cam_saved_mode);
        } else {
            write_u32_le(kAddr_cameraMode, s_cam_saved_mode);
            write_u32_le(kAddr_cameraDivT, s_cam_saved_div_t ? s_cam_saved_div_t : 1u);
            write_u32_le(kAddr_cameraDivE, s_cam_saved_div_e ? s_cam_saved_div_e : 1u);
            write_u32_le(kAddr_cameraLock, s_cam_saved_lock);
            write_u32_le(kAddr_cameraShake, s_cam_saved_shake);
        }
        s_cam_saved_valid = false;
    }
    s_cam_saved_valid = false;
    s_cam_frozen_mod = -1;
}

/* ---- Free Camera panel (W6) ------------------------------------------- */

/* True while the mouse is over (or interacting with) an ImGui window.
 * Left-drag pans only when this is false at press time (or the overlay is
 * hidden), so sliders/buttons keep working while the overlay is open. */
static bool cam_ui_eats_mouse(void)
{
    return s_input_blocked;
}

/* Left-drag pan latch: starts only on empty ground, ends on release. */
static bool cam_pan_gate(Uint32 btn)
{
    bool held = (btn & SDL_BUTTON_LMASK) != 0;
    if (held && !s_cam_panning) {
        if (!cam_ui_eats_mouse()) s_cam_panning = true;
    } else if (!held) {
        s_cam_panning = false;
    }
    return s_cam_pan_enable && s_cam_panning;
}

/* Guest camera coordinates use +Y down and +Z forward. Keep WASD on the
 * XZ plane and translate eye and target together so pitch, height and the
 * eye-to-target distance stay fixed. Zoom/dolly remains a wheel action. */
static void camera_ground_basis(float dx, float dz,
                                float& fx, float& fz, float& rx, float& rz)
{
    const float length = std::sqrt(dx*dx + dz*dz);
    fx = length > 0.001f ? dx / length : 0.0f;
    fz = length > 0.001f ? dz / length : 1.0f;
    rx = fz;
    rz = -fx;
}

/* Positive yaw looks right; positive pitch looks down in guest coordinates.
 * Rotate around the eye without changing the eye-to-target distance. */
static void camera_rotate_view(float& dx, float& dy, float& dz,
                               float yaw_delta, float pitch_delta)
{
    float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (distance < 0.001f) { dx = dy = 0; dz = 1; distance = 1; }
    float yaw = std::atan2(dx, dz) + yaw_delta;
    float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, dy / distance))) + pitch_delta;
    constexpr float limit = 1.5533f; // ~89 degrees
    pitch = std::fmax(-limit, std::fmin(limit, pitch));
    const float horizontal = std::cos(pitch) * distance;
    dx = std::sin(yaw) * horizontal;
    dy = std::sin(pitch) * distance;
    dz = std::cos(yaw) * horizontal;
}

/* Positional pose step (field/battle/battling): keys fly eye+at, mouse
 * right/middle-drag orbits `at` around `eye`, wheel dollies along view.
 * Updates s_cam_eye_f/at_f; the caller holds the pose per module. */
static void camera_pose_step(bool allow_motion)
{
    float ex = s_cam_eye_f[0], ey = s_cam_eye_f[1], ez = s_cam_eye_f[2];
    float ax = s_cam_at_f[0],  ay = s_cam_at_f[1],  az = s_cam_at_f[2];

    if (s_camera_keys_enable && allow_motion) {
        const Uint8 *ks = SDL_GetKeyboardState(nullptr);
        if (ks) {
            float dx = ax - ex, dy = ay - ey, dz = az - ez;
            float fx, fz, rx, rz;
            camera_ground_basis(dx, dz, fx, fz, rx, rz);

            bool boost = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
            float spd = s_camera_fly_speed * s_cam_dt_scale * (boost ? 8.0f : 1.0f);
            if (ks[SDL_SCANCODE_W]) { ex += fx*spd; ez += fz*spd; ax += fx*spd; az += fz*spd; }
            if (ks[SDL_SCANCODE_S]) { ex -= fx*spd; ez -= fz*spd; ax -= fx*spd; az -= fz*spd; }
            if (ks[SDL_SCANCODE_D]) { ex += rx*spd; ez += rz*spd; ax += rx*spd; az += rz*spd; }
            if (ks[SDL_SCANCODE_A]) { ex -= rx*spd; ez -= rz*spd; ax -= rx*spd; az -= rz*spd; }
            if (ks[SDL_SCANCODE_E]) { ey -= spd; ay -= spd; }
            if (ks[SDL_SCANCODE_Q]) { ey += spd; ay += spd; }
            const float rot = s_camera_rot_speed * s_cam_dt_scale;
            const float yaw = ((ks[SDL_SCANCODE_RIGHT] ? 1.0f : 0.0f) -
                               (ks[SDL_SCANCODE_LEFT] ? 1.0f : 0.0f)) * rot;
            const float pitch = ((ks[SDL_SCANCODE_DOWN] ? 1.0f : 0.0f) -
                                 (ks[SDL_SCANCODE_UP] ? 1.0f : 0.0f)) * rot *
                                (s_cam_invert_y ? -1.0f : 1.0f);
            if (yaw != 0 || pitch != 0) {
                camera_rotate_view(dx, dy, dz, yaw, pitch);
                ax = ex + dx; ay = ey + dy; az = ez + dz;
            }
        }
    }

    /* Mouse look + wheel dolly. SDL_GetMouseState works on every pre_swap
     * (visible or hidden); the drag buttons are right/middle so left-click
     * keeps driving ImGui widgets while the overlay is open. */
    {
#if defined(PSX_SDL3)
        float fmx = 0.0f, fmy = 0.0f;
        Uint32 btn = SDL_GetMouseState(&fmx, &fmy);
        int mx = (int)fmx, my = (int)fmy;
#else
        int mx = 0, my = 0;
        Uint32 btn = SDL_GetMouseState(&mx, &my);
#endif
        if (!s_cam_mouse_init) {
            s_cam_last_mx = mx; s_cam_last_my = my; s_cam_mouse_init = true;
        }
        int mdx = mx - s_cam_last_mx, mdy = my - s_cam_last_my;
        s_cam_last_mx = mx; s_cam_last_my = my;
        const bool dragging =
            (btn & SDL_BUTTON_RMASK) || (btn & SDL_BUTTON_MMASK);
        if (s_cam_mouse_look && dragging && (mdx != 0 || mdy != 0) && allow_motion) {
            float dx = ax - ex, dy = ay - ey, dz = az - ez;
            camera_rotate_view(dx, dy, dz, (float)mdx * s_cam_mouse_sens,
                (float)mdy * s_cam_mouse_sens * (s_cam_invert_y ? -1.0f : 1.0f));
            ax = ex + dx; ay = ey + dy; az = ez + dz;
        }
        if (s_cam_wheel_accum != 0.0f) {
            float dx = ax - ex, dy = ay - ey, dz = az - ez;
            float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dl < 0.001f) { dx = 0.0f; dy = 0.0f; dz = 1.0f; dl = 1.0f; }
            float step = s_cam_wheel_accum * s_cam_wheel_step;
            ex += dx / dl * step; ey += dy / dl * step; ez += dz / dl * step;
            ax += dx / dl * step; ay += dy / dl * step; az += dz / dl * step;
            s_cam_wheel_accum = 0.0f;
        }
        /* Left-drag glide: horizontal drag strafes in X, vertical drag
         * moves forward/back on the ground plane — mouse-driven WASD
         * (no height change; Q/E and wheel cover that). */
        if (cam_pan_gate(btn) && (mdx != 0 || mdy != 0) && allow_motion) {
            float dx = ax - ex, dz = az - ez;
            float fx, fz, rx, rz;
            camera_ground_basis(dx, dz, fx, fz, rx, rz);
            float ddx = ax - ex, ddy = ay - ey, ddz = az - ez;
            float dist = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
            if (dist < 1.0f) dist = 1.0f;
            float k = dist * s_cam_pan_factor;
            float px = (rx * (float)mdx - fx * (float)mdy) * k;
            float pz = (rz * (float)mdx - fz * (float)mdy) * k;
            ex += px; ez += pz;
            ax += px; az += pz;
        }
    }

    s_cam_eye_f[0] = ex; s_cam_eye_f[1] = ey; s_cam_eye_f[2] = ez;
    s_cam_at_f[0]  = ax; s_cam_at_f[1]  = ay; s_cam_at_f[2]  = az;
}

/* World orbit rig: translate its origin on the ground using the rendered
 * view's forward/right axes. Mouse/arrows turn the rig; the wheel changes
 * distance. Travel-heading conventions need not match camera conventions. */
static void camera_orbit_step(bool allow_motion)
{
    constexpr float kTurn = 4096.0f / (2.0f * 3.141592653589793f);
    float yawrad = (float)s_w_yaw * (6.283185307179586f / 4096.0f);
    float fx = std::sin(yawrad), fz = -std::cos(yawrad);
    float rx = std::cos(yawrad), rz = std::sin(yawrad);
    // A PSX MATRIX stores nine row-major s16 Q12 components. Row 0 is
    // screen-right and row 2 is view-forward; discard Y for ground motion.
    const float view_fx = (float)(int16_t)read_u16_le(kAddr_worldCamMatrix + 12u);
    const float view_fz = (float)(int16_t)read_u16_le(kAddr_worldCamMatrix + 16u);
    const float view_rx = (float)(int16_t)read_u16_le(kAddr_worldCamMatrix);
    const float view_rz = (float)(int16_t)read_u16_le(kAddr_worldCamMatrix + 4u);
    const float forward_length = std::sqrt(view_fx*view_fx + view_fz*view_fz);
    const float right_length = std::sqrt(view_rx*view_rx + view_rz*view_rz);
    if (forward_length > 1.0f && right_length > 1.0f) {
        fx = view_fx / forward_length; fz = view_fz / forward_length;
        rx = view_rx / right_length; rz = view_rz / right_length;
    }
    if (s_camera_keys_enable && allow_motion) {
        const Uint8 *ks = SDL_GetKeyboardState(nullptr);
        if (ks) {
            bool boost = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
            float spd = s_camera_fly_speed * s_cam_dt_scale * (boost ? 8.0f : 1.0f);
            float rot = s_camera_rot_speed * kTurn * s_cam_dt_scale;
            float ox = s_w_org_f[0], oy = s_w_org_f[1], oz = s_w_org_f[2];
            if (ks[SDL_SCANCODE_W]) { ox += fx*spd; oz += fz*spd; }
            if (ks[SDL_SCANCODE_S]) { ox -= fx*spd; oz -= fz*spd; }
            if (ks[SDL_SCANCODE_D]) { ox += rx*spd; oz += rz*spd; }
            if (ks[SDL_SCANCODE_A]) { ox -= rx*spd; oz -= rz*spd; }
            if (ks[SDL_SCANCODE_E]) { oy -= spd; }
            if (ks[SDL_SCANCODE_Q]) { oy += spd; }
            if (ks[SDL_SCANCODE_LEFT])  { s_w_yaw -= (int)std::lround(rot); }
            if (ks[SDL_SCANCODE_RIGHT]) { s_w_yaw += (int)std::lround(rot); }
            const int pitch_step = (int)std::lround(rot * 0.5f * (s_cam_invert_y ? -1.0f : 1.0f));
            if (ks[SDL_SCANCODE_UP])    { s_w_pitch += pitch_step; }
            if (ks[SDL_SCANCODE_DOWN])  { s_w_pitch -= pitch_step; }
            s_w_org_f[0] = ox; s_w_org_f[1] = oy; s_w_org_f[2] = oz;
        }
    }
    {
#if defined(PSX_SDL3)
        float fmx = 0.0f, fmy = 0.0f;
        Uint32 btn = SDL_GetMouseState(&fmx, &fmy);
        int mx = (int)fmx, my = (int)fmy;
#else
        int mx = 0, my = 0;
        Uint32 btn = SDL_GetMouseState(&mx, &my);
#endif
        if (!s_cam_mouse_init) {
            s_cam_last_mx = mx; s_cam_last_my = my; s_cam_mouse_init = true;
        }
        int mdx = mx - s_cam_last_mx, mdy = my - s_cam_last_my;
        s_cam_last_mx = mx; s_cam_last_my = my;
        const bool dragging =
            (btn & SDL_BUTTON_RMASK) || (btn & SDL_BUTTON_MMASK);
        if (s_cam_mouse_look && dragging && (mdx != 0 || mdy != 0) && allow_motion) {
            /* yaw+ turns right: drag right orbits right. */
            s_w_yaw += (int)std::lround((float)mdx * s_cam_mouse_sens * kTurn);
            float dy = (float)mdy * s_cam_mouse_sens * kTurn *
                       (s_cam_invert_y ? 1.0f : -1.0f);
            s_w_pitch += (int)std::lround(dy);
        }
        if (s_cam_wheel_accum != 0.0f) {
            s_w_dist_f += s_cam_wheel_accum * s_cam_wheel_step;
            s_cam_wheel_accum = 0.0f;
        }
        /* Left-drag glide: same WASD-style mapping on the ground plane —
         * horizontal strafes in X, vertical moves forward/back. */
        if (cam_pan_gate(btn) && (mdx != 0 || mdy != 0) && allow_motion) {
            float rxl = std::sqrt(rx*rx + rz*rz);
            float fxl = std::sqrt(fx*fx + fz*fz);
            float k = s_w_dist_f * s_cam_pan_factor;
            float px = 0.0f, pz = 0.0f;
            if (rxl > 0.05f) { px += (rx / rxl) * (float)mdx * k; pz += (rz / rxl) * (float)mdx * k; }
            if (fxl > 0.05f) { px += -(fx / fxl) * (float)mdy * k; pz += -(fz / fxl) * (float)mdy * k; }
            s_w_org_f[0] += px; s_w_org_f[2] += pz;
        }
    }
    s_w_yaw &= 0x0FFF;
    if (s_w_pitch > 256) s_w_pitch = 256;
    if (s_w_pitch < -1024) s_w_pitch = -1024;
    if (s_w_dist_f < 32.0f) s_w_dist_f = 32.0f;
    if (s_w_dist_f > 8192.0f) s_w_dist_f = 8192.0f;
    for (int i = 0; i < 3; i++) {
        if (s_w_org_f[i] < -32768.0f) s_w_org_f[i] = -32768.0f;
        if (s_w_org_f[i] >  32768.0f) s_w_org_f[i] =  32768.0f;
    }
}

/* Hold the integrated pose per module (re-assert freeze + write). */
static void camera_hold_field(void)
{
    write_u32_le(kAddr_cameraMode, 1u);
    write_u32_le(kAddr_cameraDivT, 1u);
    write_u32_le(kAddr_cameraDivE, 1u);
    write_u32_le(kAddr_cameraLock, read_u32_le(kAddr_cameraLock) | kCameraLockBit);
    auto to_fixed = [](float v) -> int32_t {
        float c = v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v);
        return (int32_t)std::lround(c * 65536.0f);
    };
    int32_t ef[3] = { to_fixed(s_cam_eye_f[0]), to_fixed(s_cam_eye_f[1]), to_fixed(s_cam_eye_f[2]) };
    int32_t af[3] = { to_fixed(s_cam_at_f[0]), to_fixed(s_cam_at_f[1]), to_fixed(s_cam_at_f[2]) };
    for (int i = 0; i < 3; i++) {
        write_u32_le(kAddr_cameraEye + (uint32_t)i * 4u, (uint32_t)ef[i]);
        write_u32_le(kAddr_cameraDesEye + (uint32_t)i * 4u, (uint32_t)ef[i]);
        write_u32_le(kAddr_cameraAt + (uint32_t)i * 4u, (uint32_t)af[i]);
        write_u32_le(kAddr_cameraDesAt + (uint32_t)i * 4u, (uint32_t)af[i]);
    }
}

static void camera_hold_battle(void)
{
    write_u32_le(kAddr_battleCamMode, 4u);
    write_u32_le(kAddr_battleCamGate, 5u);
    auto clamp_s16 = [](float v) -> int {
        int c = (int)std::lround(v);
        if (c < -32768) return -32768;
        if (c >  32767) return  32767;
        return c;
    };
    int e[3] = { clamp_s16(s_cam_eye_f[0]), clamp_s16(s_cam_eye_f[1]), clamp_s16(s_cam_eye_f[2]) };
    int a[3] = { clamp_s16(s_cam_at_f[0]), clamp_s16(s_cam_at_f[1]), clamp_s16(s_cam_at_f[2]) };
    for (int i = 0; i < 3; i++) {
        write_u16_le(kAddr_battleCamEye + (uint32_t)i * 2u, (uint16_t)e[i]);
        write_u16_le(kAddr_battleCamAt + (uint32_t)i * 2u, (uint16_t)a[i]);
    }
    uint32_t d0 = read_u32_le(kAddr_battleCamDesEye);
    uint32_t d1 = read_u32_le(kAddr_battleCamDesEye + 4u);
    uint32_t d2 = read_u32_le(kAddr_battleCamDesAt);
    uint32_t d3 = read_u32_le(kAddr_battleCamDesAt + 4u);
    d0 = ((uint32_t)(uint16_t)e[1] << 16) | ((uint32_t)(uint16_t)e[0]);
    d1 = (d1 & 0xFFFF0000u) | ((uint32_t)(uint16_t)e[2]);
    d2 = ((uint32_t)(uint16_t)a[1] << 16) | ((uint32_t)(uint16_t)a[0]);
    d3 = (d3 & 0xFFFF0000u) | ((uint32_t)(uint16_t)a[2]);
    write_u32_le(kAddr_battleCamDesEye, d0);
    write_u32_le(kAddr_battleCamDesEye + 4u, d1);
    write_u32_le(kAddr_battleCamDesAt, d2);
    write_u32_le(kAddr_battleCamDesAt + 4u, d3);
}

static void camera_hold_battling(void)
{
    write_u32_le(kAddr_battlingCamPreset, kBattlingCamFrozenPreset);
    auto clamp_m = [](float v) -> int32_t {
        float c = v < -16777216.0f ? -16777216.0f : (v > 16777216.0f ? 16777216.0f : v);
        return (int32_t)std::lround(c);
    };
    for (int i = 0; i < 3; i++) {
        write_u32_le(kAddr_battlingCamEye + (uint32_t)i * 4u,
                     (uint32_t)clamp_m(s_cam_eye_f[i]));
        write_u32_le(kAddr_battlingCamAt + (uint32_t)i * 4u,
                     (uint32_t)clamp_m(s_cam_at_f[i]));
    }
}

/* Per-frame free-camera integration, branched by resident overlay sig.
 * Runs with the overlay open OR closed (closed = fly around while playing).
 * Module switches release the old freeze before freezing the new one. */
static void apply_camera_frame(uint32_t sig)
{
    /* Frame-dt for framerate-normalized key motion. Without this, key
     * flight runs per-present: on a slow scene (long frames) translation
     * crawls while mouse deltas and wheel notches still feel instant. */
    {
        uint64_t now = SDL_GetTicks64();
        if (s_cam_last_ticks != 0u && now >= s_cam_last_ticks) {
            float dtms = (float)(now - s_cam_last_ticks);
            if (dtms > 250.0f) dtms = 250.0f;
            float sc = dtms / 16.6667f;
            if (sc < 0.1f) sc = 0.1f;
            if (sc > 20.0f) sc = 20.0f;
            s_cam_dt_scale = sc;
            if (dtms > 0.0f) {
                float inst = 1000.0f / dtms;
                s_cam_hz = (s_cam_hz == 0.0f) ? inst : s_cam_hz * 0.9f + inst * 0.1f;
            }
        }
        s_cam_last_ticks = now;
    }
    if (sig == 0u) {
        camera_unfreeze();
        return;
    }
    if ((int)sig != s_cam_frozen_mod) {
        camera_unfreeze();
        if (!camera_freeze(sig)) return; /* unsupported state; panel explains */
    }
    bool allow_motion = !s_input_blocked && s_input_window &&
        (SDL_GetWindowFlags(s_input_window) & SDL_WINDOW_INPUT_FOCUS);
    if (sig == kSigWorldOverlay) {
        /* While frozen, slot 9 must still hold our 0 and slot 10 the
         * ordinary updater. A cinematic reassignment owns the slots again:
         * stop driving and report instead of fighting the new owner. */
        uint32_t s9 = world_cam_slot_base(kWorldCamSlot9);
        uint32_t s10 = world_cam_slot_base(kWorldCamSlot10);
        bool ok = (s9 != 0u && s10 != 0u &&
                   read_u32_le(s9 + kWorldCamSlotFnOff) == 0u &&
                   read_u16_le(s10 + kWorldCamSlotStateOff) == 1u &&
                   read_u32_le(s10 + kWorldCamSlotFnOff) == kWorldCamSlot10Fn);
        if (!ok) {
            s_cam_saved_valid = false;
            s_cam_frozen_mod = -1;
            std::snprintf(s_camera_status, sizeof(s_camera_status),
                "World camera retaken by a cinematic — re-enable to freeze again.");
            s_camera_status_frames = 120;
            return;
        }
        camera_orbit_step(allow_motion);
        /* Drive the free origin (12.12 fixed) and drag streaming with its
         * XZ deltas — the same accumulators slot 9 feeds, so terrain keeps
         * loading around the camera instead of the party. */
        int32_t ox = (int32_t)std::lround(s_w_org_f[0] * 4096.0f);
        int32_t oy = (int32_t)std::lround(s_w_org_f[1] * 4096.0f);
        int32_t oz = (int32_t)std::lround(s_w_org_f[2] * 4096.0f);
        int32_t prevx = (int32_t)read_u32_le(kAddr_worldCamOrigin + 0);
        int32_t prevz = (int32_t)read_u32_le(kAddr_worldCamOrigin + 8);
        write_u32_le(kAddr_worldCamOrigin + 0, (uint32_t)ox);
        write_u32_le(kAddr_worldCamOrigin + 4, (uint32_t)oy);
        write_u32_le(kAddr_worldCamOrigin + 8, (uint32_t)oz);
        write_u32_le(kAddr_worldStreamX,
                     read_u32_le(kAddr_worldStreamX) + (uint32_t)(ox - prevx));
        write_u32_le(kAddr_worldStreamZ,
                     read_u32_le(kAddr_worldStreamZ) + (uint32_t)(oz - prevz));
        uint16_t yaw = (uint16_t)(s_w_yaw & 0x0FFF);
        int32_t pitch = (int32_t)(int16_t)s_w_pitch;
        uint32_t dist = (uint32_t)((int32_t)std::lround(s_w_dist_f * 4096.0f));
        write_u16_le(kAddr_worldCamYaw, yaw);
        write_u16_le(kAddr_worldCamPitch, (uint16_t)pitch);
        write_u32_le(kAddr_worldCamDist, dist);
        /* Chase targets see "settled": slot 10 derives the eye from our
         * exact inputs instead of easing away from them (this is what
         * makes the wheel zoom actually move the camera). Sub-modes 2/4+
         * skip the vector derivation entirely, so normalize them back to
         * 0 (evaluate): in ordinary mode the engine only uses 0/1/3. */
        write_u32_le(s10 + kWorldCamSlot10TDistOff, dist);
        write_u32_le(s10 + kWorldCamSlot10TPitchOff, (uint32_t)pitch);
        write_u32_le(s10 + kWorldCamSlot10PAccOff, (uint32_t)(pitch << 12));
        uint16_t wmode = read_u16_le(s10 + kWorldCamSlot10ModeOff);
        if (wmode == 2u || wmode > 3u)
            write_u16_le(s10 + kWorldCamSlot10ModeOff, 0u);
        return;
    }
    camera_pose_step(allow_motion);
    if (sig == kSigBattleOverlay)      camera_hold_battle();
    else if (sig == kSigBattlingOverlay) camera_hold_battling();
    else                               camera_hold_field();
}

static void draw_camera_section(bool compact)
{
    const uint32_t sig = resident_overlay_sig();
    const int mod = resident_loaded_module();
    const bool supported = (sig != 0u);

    if (s_cam_frozen_mod >= 0) {
        ImGui::Text("Module: %s  frozen (sig %u)",
                    resident_module_name(mod), (unsigned)s_cam_frozen_mod);
    } else {
        ImGui::Text("Module: %s", resident_module_name(mod));
    }

    bool en = s_camera_enabled;
    if (ImGui::Checkbox("Enable free camera (field/world/battle/battling)", &en)) {
        s_camera_enabled = en;
        s_cam_last_ticks = 0;
        if (!en) camera_unfreeze();
        std::snprintf(s_camera_status, sizeof(s_camera_status), "%s",
            en ? "Free camera armed; close menus to fly." : "Free camera off; game camera resumes.");
        s_camera_status_frames = 120;
    }
    /* camera_freeze() is the authority on s_cam_frozen_mod. */
    if (s_camera_status_frames > 0 && s_camera_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", s_camera_status);
        s_camera_status_frames--;
    }
    if (!supported) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "No game overlay resident — freeze engages on module entry.");
    } else if (s_camera_enabled && s_cam_frozen_mod < 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "Freeze pending/refused — see status above (cinematics override).");
    }
    if (sig == kSigBattleOverlay && s_cam_frozen_mod == (int)kSigBattleOverlay) {
        ImGui::TextDisabled("Neutral stance holds; attack anims write the pose directly "
                            "and retake briefly.");
    }
    if (sig == kSigBattlingOverlay && s_cam_frozen_mod == (int)kSigBattlingOverlay) {
        ImGui::TextDisabled("Bout scope: victory/replay orbits override the freeze.");
    }

    if (compact) {
        ImGui::Separator();
        ImGui::Checkbox("Enable fly keys", &s_camera_keys_enable);
        ImGui::Checkbox("Capture game input while flying", &s_cam_capture_input);
        ImGui::TextDisabled("WASD: ground movement; Q/E: down/up; arrows: look");
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("Fly speed", &s_camera_fly_speed, 1.0f, 2048.0f, "%.1f");
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("Arrow look speed", &s_camera_rot_speed, 0.0f, 0.5f, "%.3f");
        ImGui::Separator();
        ImGui::Checkbox("Right-drag looks", &s_cam_mouse_look);
        ImGui::Checkbox("Invert Y axis", &s_cam_invert_y);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Invert vertical look for mouse and arrow keys; Q/E keep moving down/up.");
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("Look sensitivity", &s_cam_mouse_sens, 0.0005f, 0.02f, "%.4f");
        ImGui::Checkbox("Wheel dollies forward/back", &s_cam_wheel_dolly);
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("Wheel step", &s_cam_wheel_step, 4.0f, 512.0f, "%.1f");
        ImGui::Checkbox("Left-drag glides", &s_cam_pan_enable);
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("Glide speed", &s_cam_pan_factor, 0.0002f, 0.01f, "%.4f");
        ImGui::TextDisabled("Close menus to move the camera.");
        return;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Mouse (works open or closed):");
    ImGui::Checkbox("Right-drag looks (yaw + pitch)", &s_cam_mouse_look);
    ImGui::SliderFloat("Look sensitivity (rad/px)", &s_cam_mouse_sens, 0.0005f, 0.02f, "%.4f");
    ImGui::Checkbox("Invert Y axis", &s_cam_invert_y);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Invert vertical look for mouse and arrow keys; Q/E keep moving down/up.");
    ImGui::Checkbox("Wheel dollies forward/back", &s_cam_wheel_dolly);
    ImGui::SliderFloat("Wheel step (units/notch)", &s_cam_wheel_step, 4.0f, 512.0f, "%.1f");
    ImGui::Checkbox("Left-drag glides (X strafe, Z forward)", &s_cam_pan_enable);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drag on empty view (not on a panel) to glide across "
                          "the map: sideways strafes, up/down goes forward/back. "
                          "Works with the overlay open or closed.");
    }
    ImGui::SliderFloat("Glide speed (dist-relative)", &s_cam_pan_factor, 0.0002f, 0.01f, "%.4f");

    ImGui::Separator();
    if (sig == kSigWorldOverlay) {
        ImGui::TextDisabled("Keys: WASD fly (view-relative), Q/E down/up, arrows look:");
    } else {
        ImGui::TextDisabled("Keys (WASD fly, Q/E down/up, arrows look/tilt):");
    }
    ImGui::Checkbox("Enable fly keys", &s_camera_keys_enable);
    ImGui::Checkbox("Capture game input while flying", &s_cam_capture_input);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Masks the game pad/keyboard while free-cam is on so "
                          "WASD flies the camera instead of the player. Turn off "
                          "to let game input through (keys will drive both).");
    }
    ImGui::SliderFloat("Fly speed (units/frame@60fps, Shift=x8)",
                       &s_camera_fly_speed, 1.0f, 2048.0f, "%.1f");
    {
        /* Live key indicators from the SAME SDL state the flight code
         * reads: if a key lights here but the camera doesn't move, the
         * hold (not the input) is the problem. */
        const Uint8 *ks = SDL_GetKeyboardState(nullptr);
        bool typing_now = s_input_blocked;
        char kl[96];
        std::snprintf(kl, sizeof(kl), "keys seen: %c %c %c %c %c %c  typing:%d keys:%d loop:%.0fHz",
            (ks && ks[SDL_SCANCODE_W]) ? 'W' : '.',
            (ks && ks[SDL_SCANCODE_A]) ? 'A' : '.',
            (ks && ks[SDL_SCANCODE_S]) ? 'S' : '.',
            (ks && ks[SDL_SCANCODE_D]) ? 'D' : '.',
            (ks && ks[SDL_SCANCODE_Q]) ? 'Q' : '.',
            (ks && ks[SDL_SCANCODE_E]) ? 'E' : '.',
            typing_now ? 1 : 0, s_camera_keys_enable ? 1 : 0, (double)s_cam_hz);
        ImGui::TextDisabled("%s", kl);
    }
    ImGui::SliderFloat("Arrow yaw speed (rad/frame)", &s_camera_rot_speed, 0.0f, 0.5f, "%.3f");

    ImGui::Separator();
    if (sig == kSigWorldOverlay) {
        /* Free-flight editor: origin is the look target AND the streaming
         * anchor (slot 9 suspended, deltas dragged to the grid masks). */
        float org[3] = {
            (float)(int32_t)read_u32_le(kAddr_worldCamOrigin + 0) / 4096.0f,
            (float)(int32_t)read_u32_le(kAddr_worldCamOrigin + 4) / 4096.0f,
            (float)(int32_t)read_u32_le(kAddr_worldCamOrigin + 8) / 4096.0f
        };
        int live_yaw = (int)(read_u16_le(kAddr_worldCamYaw) & 0x0FFFu);
        int live_pitch = (int)(int16_t)read_u16_le(kAddr_worldCamPitch);
        float live_dist = (float)(int32_t)read_u32_le(kAddr_worldCamDist) / 4096.0f;
        ImGui::Text("Live origin (look target): (%.1f, %.1f, %.1f)", org[0], org[1], org[2]);
        ImGui::Text("Live yaw/pitch/dist: %d / %d / %.1f", live_yaw, live_pitch, live_dist);
        if (ImGui::Button("Pull live -> editor")) {
            camera_pull_live(sig);
        }
        ImGui::DragFloat3("origin (units)", s_w_org_f, 8.0f, -32768.0f, 32767.0f, "%.1f");
        ImGui::SliderInt("yaw (12-bit turn)", &s_w_yaw, 0, 4095);
        ImGui::SliderInt("pitch (12-bit)", &s_w_pitch, -1024, 256);
        ImGui::SliderFloat("distance (units)", &s_w_dist_f, 32.0f, 8192.0f, "%.1f");
        ImGui::TextDisabled(
            "Full 3D flight: WASD flies view-relative, wheel zooms, drag looks. "
            "Streaming follows the origin. Cinematics retake the camera.");
        return;
    }

    uint32_t eye0, at0;
    const char *fmt_note;
    if (sig == kSigBattleOverlay) {
        eye0 = kAddr_battleCamEye; at0 = kAddr_battleCamAt;
        fmt_note = "s16 units";
    } else if (sig == kSigBattlingOverlay) {
        eye0 = kAddr_battlingCamEye; at0 = kAddr_battlingCamAt;
        fmt_note = "s32 units";
    } else {
        eye0 = kAddr_cameraEye; at0 = kAddr_cameraAt;
        fmt_note = "16.16 fixed";
    }
    float live_eye[3], live_at[3];
    if (sig == kSigBattleOverlay) {
        for (int i = 0; i < 3; i++) {
            live_eye[i] = (float)(int16_t)read_u16_le(eye0 + (uint32_t)i * 2u);
            live_at[i]  = (float)(int16_t)read_u16_le(at0 + (uint32_t)i * 2u);
        }
    } else if (sig == kSigBattlingOverlay) {
        for (int i = 0; i < 3; i++) {
            live_eye[i] = (float)(int32_t)read_u32_le(eye0 + (uint32_t)i * 4u);
            live_at[i]  = (float)(int32_t)read_u32_le(at0 + (uint32_t)i * 4u);
        }
    } else {
        for (int i = 0; i < 3; i++) {
            live_eye[i] = (float)(int32_t)read_u32_le(eye0 + (uint32_t)i * 4u) / 65536.0f;
            live_at[i]  = (float)(int32_t)read_u32_le(at0 + (uint32_t)i * 4u) / 65536.0f;
        }
    }
    {
        float dx = live_at[0]-live_eye[0], dy = live_at[1]-live_eye[1], dz = live_at[2]-live_eye[2];
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        ImGui::Text("Live eye: (%.1f, %.1f, %.1f) [%s]", live_eye[0], live_eye[1], live_eye[2], fmt_note);
        ImGui::Text("Live at : (%.1f, %.1f, %.1f)  dist=%.1f", live_at[0], live_at[1], live_at[2], dist);
    }

    if (ImGui::Button("Pull live -> editor")) {
        camera_pull_live(sig != 0u ? sig : kSigFieldOverlay);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply pose once")) {
        psx_free_camera_write(
            (int)std::lround(s_cam_eye_f[0]), (int)std::lround(s_cam_eye_f[1]),
            (int)std::lround(s_cam_eye_f[2]),
            (int)std::lround(s_cam_at_f[0]), (int)std::lround(s_cam_at_f[1]),
            (int)std::lround(s_cam_at_f[2]));
        std::snprintf(s_camera_status, sizeof(s_camera_status),
            "Pose written. Enable to hold it.");
        s_camera_status_frames = 90;
    }
    ImGui::SameLine();
    if (ImGui::Button("Target ahead")) {
        float dx = s_cam_at_f[0]-s_cam_eye_f[0];
        float dy = s_cam_at_f[1]-s_cam_eye_f[1];
        float dz = s_cam_at_f[2]-s_cam_eye_f[2];
        float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dl < 1.0f) { dx = 0.0f; dy = 0.0f; dz = 1.0f; dl = 1.0f; }
        float want = dl < 1.0f ? 500.0f : dl;
        s_cam_at_f[0] = s_cam_eye_f[0] + dx / dl * want;
        s_cam_at_f[1] = s_cam_eye_f[1] + dy / dl * want;
        s_cam_at_f[2] = s_cam_eye_f[2] + dz / dl * want;
    }

    ImGui::DragFloat3("eye (PSX units)", s_cam_eye_f, 4.0f, -32768.0f, 32767.0f, "%.1f");
    ImGui::DragFloat3("at (PSX units)", s_cam_at_f, 4.0f, -32768.0f, 32767.0f, "%.1f");
    ImGui::TextDisabled(
        "Close menus to fly with mouse and keys.");
}

void psx_free_camera_init(SDL_Window* window) {
    // A session restart may have replaced guest RAM; discard the old owner.
    s_cam_saved_valid = false;
    s_cam_frozen_mod = -1;
    s_cam_last_ticks = 0;
    s_cam_mouse_init = false;
    s_cam_panning = false;
    s_cam_wheel_accum = 0;
    s_game_window = window;
    s_input_window = window;
}

void psx_free_camera_suspend(void) {
    camera_unfreeze();
    s_cam_last_ticks = 0;
    s_cam_mouse_init = false;
    s_cam_panning = false;
    s_cam_wheel_accum = 0;
}

void psx_free_camera_shutdown(void) {
    psx_free_camera_suspend();
    s_game_window = s_input_window = nullptr;
}

void psx_free_camera_update(SDL_Window* input_window, bool input_blocked) {
    SDL_Window* next_window = input_window ? input_window : s_game_window;
    if (next_window != s_input_window) s_cam_mouse_init = false;
    s_input_window = next_window;
    s_input_blocked = input_blocked;
    if (!s_game_window || psx_netplay_active() || !psx_input_runtime_bindings_available()) {
        psx_free_camera_suspend();
        return;
    }
    if (s_input_blocked || !s_input_window ||
        !(SDL_GetWindowFlags(s_input_window) & SDL_WINDOW_INPUT_FOCUS)) {
        s_cam_wheel_accum = 0;
        s_cam_panning = false;
    }
    if (s_camera_enabled) apply_camera_frame(resident_overlay_sig());
    else camera_unfreeze();
}

bool psx_free_camera_process_event(const SDL_Event* event, bool input_blocked) {
    if (!event || input_blocked || !s_game_window || psx_netplay_active() ||
        !psx_input_runtime_bindings_available() ||
        !s_camera_enabled || !s_cam_wheel_dolly || s_cam_frozen_mod < 0 ||
        event->type != SDL_MOUSEWHEEL) return false;
    SDL_Window* focus = SDL_GetKeyboardFocus();
    if (!focus || (focus != s_game_window && focus != s_input_window) ||
        event->wheel.windowID != SDL_GetWindowID(focus)) return false;
    s_cam_wheel_accum += (float)event->wheel.y;
    return true;
}

bool psx_free_camera_capture_input(void) {
    return s_game_window && !psx_netplay_active() && psx_input_runtime_bindings_available() && s_camera_enabled &&
        s_cam_capture_input && s_cam_frozen_mod >= 0 &&
        resident_overlay_sig() == (uint32_t)s_cam_frozen_mod;
}

void psx_free_camera_get_pose(float eye[3], float at[3]) {
    for (int i = 0; i < 3; ++i) { eye[i] = s_cam_eye_f[i]; at[i] = s_cam_at_f[i]; }
}

void psx_free_camera_get_settings(PsxFreeCameraSettings* settings) {
    if (!settings) return;
    settings->enabled = s_camera_enabled;
    settings->fly_keys = s_camera_keys_enable;
    settings->capture_input = s_cam_capture_input;
    settings->mouse_look = s_cam_mouse_look;
    settings->invert_y = s_cam_invert_y;
    settings->wheel_dolly = s_cam_wheel_dolly;
    settings->pan = s_cam_pan_enable;
    settings->fly_speed = s_camera_fly_speed;
    settings->rotation_speed = s_camera_rot_speed;
    settings->look_sensitivity = s_cam_mouse_sens;
    settings->wheel_step = s_cam_wheel_step;
    settings->pan_factor = s_cam_pan_factor;
}

void psx_free_camera_set_settings(const PsxFreeCameraSettings* settings) {
    if (!settings) return;
    if (s_camera_enabled != settings->enabled) psx_free_camera_suspend();
    s_camera_enabled = settings->enabled;
    s_camera_keys_enable = settings->fly_keys;
    s_cam_capture_input = settings->capture_input;
    s_cam_mouse_look = settings->mouse_look;
    s_cam_invert_y = settings->invert_y;
    s_cam_wheel_dolly = settings->wheel_dolly;
    s_cam_pan_enable = settings->pan;
    if (std::isfinite(settings->fly_speed) && settings->fly_speed >= 1.0f && settings->fly_speed <= 2048.0f)
        s_camera_fly_speed = settings->fly_speed;
    if (std::isfinite(settings->rotation_speed) && settings->rotation_speed >= 0.0f && settings->rotation_speed <= 0.5f)
        s_camera_rot_speed = settings->rotation_speed;
    if (std::isfinite(settings->look_sensitivity) && settings->look_sensitivity >= 0.0005f && settings->look_sensitivity <= 0.02f)
        s_cam_mouse_sens = settings->look_sensitivity;
    if (std::isfinite(settings->wheel_step) && settings->wheel_step >= 4.0f && settings->wheel_step <= 512.0f)
        s_cam_wheel_step = settings->wheel_step;
    if (std::isfinite(settings->pan_factor) && settings->pan_factor >= 0.0002f && settings->pan_factor <= 0.01f)
        s_cam_pan_factor = settings->pan_factor;
}

bool psx_free_camera_draw_controls(bool compact) {
    PsxFreeCameraSettings before{}, after{};
    psx_free_camera_get_settings(&before);
    const bool online = psx_netplay_active() != 0;
    const bool locked = online || !psx_input_runtime_bindings_available();
    if (locked) ImGui::TextDisabled(online ? "Free camera is locked during netplay"
        : "Free camera is locked during input recording/replay");
    ImGui::BeginDisabled(locked);
    draw_camera_section(compact);
    ImGui::EndDisabled();
    psx_free_camera_get_settings(&after);
    return before.enabled != after.enabled ||
           before.fly_keys != after.fly_keys ||
           before.capture_input != after.capture_input ||
           before.mouse_look != after.mouse_look ||
           before.invert_y != after.invert_y ||
           before.wheel_dolly != after.wheel_dolly ||
           before.pan != after.pan ||
           before.fly_speed != after.fly_speed ||
           before.rotation_speed != after.rotation_speed ||
           before.look_sensitivity != after.look_sensitivity ||
           before.wheel_step != after.wheel_step ||
           before.pan_factor != after.pan_factor;
}
#endif
