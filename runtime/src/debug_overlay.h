#ifndef PSXRECOMP_DEBUG_OVERLAY_H
#define PSXRECOMP_DEBUG_OVERLAY_H

/* In-game developer debug overlay (Dear ImGui, toggled with Ctrl+F3).
 *
 * The whole API is guarded by PSX_DEBUG_OVERLAY: when undefined (Release
 * builds), every call is a static-inline no-op so callers compile to zero
 * code and zero symbols. The flag is set per-target by runtime.cmake only
 * when the build gate PSX_DEBUG_OVERLAY is ON. */
#include <stdbool.h>
#include "psx_sdl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PSX_DEBUG_OVERLAY

/* Wire the overlay into the runtime. Called once at startup with the SDL
 * window / GL context the runtime already owns; the overlay attaches its
 * ImGui context and framebuffers to those. Safe to call with a null window
 * in tests (no-op). */
void psx_debug_overlay_init(struct SDL_Window *win, SDL_GLContext ctx);

/* Tear down the overlay and free its ImGui resources. Called once at
 * shutdown, after the GL context is still current. */
void psx_debug_overlay_shutdown(void);

/* Flip the visibility flag. Bound to Ctrl+F3 by the input layer. */
void psx_debug_overlay_toggle(void);

/* Current visibility. Used by the input layer to decide whether to forward
 * keys to the game or let the overlay consume them. */
bool psx_debug_overlay_is_visible(void);

/* Hand an SDL event to the overlay before the game sees it. Returns true
 * if the overlay consumed the event (game must not see it). */
bool psx_debug_overlay_process_event(const SDL_Event *ev);

/* Render/capture against a caller-selected framebuffer. A nonzero target is a
 * drawable-sized color FBO; zero selects the default back buffer. The hook
 * never binds framebuffer 0 while a nonzero target is supplied. */
void psx_debug_overlay_pre_swap_target(unsigned int framebuffer);

/* Default-framebuffer wrapper retained for the normal present paths. */
void psx_debug_overlay_pre_swap(void);

/* True when the overlay is visible AND wants exclusive keyboard focus
 * (e.g. a text field is being edited). The game must skip its own key
 * dispatch for as long as this is true. */
bool psx_debug_overlay_swallow_keyboard(void);

/* Arm a one-shot capture of the next pre-swap composited target (game +
 * overlay) to a PNG. The default wrapper captures the GL back buffer; a
 * target-aware transactional call captures its staging FBO. The PNG is RGB,
 * top-down, drawable-window-sized. If `path` is NULL or empty, the runtime
 * writes "window_shot.png" next to the exe. Safe to call from the debug server
 * thread (flag is racy in the harmless "one-shot delayed by a frame" sense). */
void psx_debug_overlay_window_shot_arm(const char *path);

/* Read-only snapshot of the keyboard-capture state — exposed for the
 * debug-server's `overlay_capture_state` command (so the test harness can
 * assert the pad-mask path without injecting SDL events). All
 * out-params may be NULL. The three flags are 0/1 ints. `visible` is the
 * raw visibility; `want_capture` is visible && io.WantCaptureKeyboard
 * (false when ImGui is not yet initialized); `swallow` is what
 * psx_debug_overlay_swallow_keyboard() returns (visible && (WantCapture
 * || !imgui_ready), plus the free-camera input capture, which can hold
 * true while hidden when flying with the overlay closed). */
void psx_debug_overlay_capture_state(int *visible, int *want_capture,
                                     int *swallow);

/* Debug-only: flip the in-window "force text capture" checkbox from the
 * debug server. When set, the next pre_swap draws a permanent InputText
 * so ImGui reports WantCaptureKeyboard=WantTextInput=true. Used by the
 * TCP test to assert the active-mask path without SDL event injection.
 * `on` < 0 means "no change" (report current). `on` >= 0 sets the bool.
 * Returns the resulting state (0 or 1). */
int psx_debug_overlay_set_force_capture(int on);

/* Debug-only: invoke one of the overlay widget's action functions, the
 * same code path the in-window button/checkbox/slider would call. The
 * TCP test uses this to verify the TOGGLES section is wired to the REAL
 * runtime setters (asserting that flipping a widget value actually
 * changes the matching TCP getter) without needing a real mouse click
 * (which is impossible from a remote client). Accepted `name`s are
 * enumerated in TCP_COMMANDS.md; the canonical set is: texfilter,
 * native_wide, aspect_set, bd_stretch_on, bd_stretch_pct, interp,
 * dump_event_ring, dump_latency_ring, dump_starv_ring, teleport,
 * party_slot, party_bitfield, gold, write_var, force_battle,
 * camera_write, event_jump, read_field_id. `value` is interpreted
 * per the name; `value2` is only consulted by `aspect_set`,
 * `party_slot` (bitfield to OR in), `write_var` (set value),
 * `camera_write` (high 16 bits = ey/ay, low 16 bits = ex/ax), and
 * `teleport` (entry point). The dump_* names ignore both value args.
 * Returns 0 on success, negative on unknown name or failed write. */
int psx_debug_overlay_widget_action(const char *name, int value, int value2);

/* Consume separate one-shot requests for Xenogears' 8 MiB developer profile,
 * its native Kernel Menu, and a boot-to-field teleport. Enabling the profile
 * does not change game state, and opening the Kernel Menu does not require
 * the developer profile. */
int psx_debug_overlay_take_developer_mode_request(void);
int psx_debug_overlay_take_kernel_menu_request(void);
/* Consume a pending boot-to-field teleport staged by psx_debug_overlay_teleport
 * when the field module was not resident. Returns 1 with *fieldId/*entryPoint
 * filled (0..729 / 0..255) when a request is pending, 0 when idle. The
 * consumer (debug_server_apply_pending_guest_transition) stages the
 * persistent target and yanks to MainLoop(1) at the next safe vblank edge
 * — the same mechanism as the Kernel Menu transition. */
int psx_debug_overlay_take_field_boot_request(int *fieldId, int *entryPoint);

/* Debug-only: teleport to a field map, from anywhere.
 *
 * Inside the field module this mirrors what the game's own scripted
 * transitions do (opcode 0x98 at 0x800932D0): stage the target in
 * fieldMapNumber (0x8004F34C) + the entry point in the ACTIVE VM mirror
 * (0x800C3A68+2, plus the persistent 0x8006EF66 copy), then clear the
 * poll gate (0x800ADBEC) as the commit. The engine poll at 0x800784A0
 * picks it up the next frame and runs the full completion sequence.
 *
 * Outside the field module (battle / worldmap / menu / movie) there is
 * no poll to consume a recipe, so the call instead stages a boot-to-field
 * request: the debug server yanks to MainLoop(1) at the next safe vblank
 * edge (same mechanism as the Kernel Menu transition) and Field boots
 * straight into the target map+entry. This abandons the current module
 * without its teardown — same trade-off as the Kernel Menu yank; do not
 * use it mid memory-card write.
 *
 * Field residency is detected via the overlay signature at 0x8006FAF0
 * (4 = Field; same ground truth as the renderer's semantic-module
 * detection). 0x800B0078 and 0x800592C0 are NOT usable signals here.
 * 0 = armed in-place (engine will load target within a few frames).
 * 4 = boot-to-field requested (watch Current field; ~60 s timeout).
 * 1 = refused: game has no game state yet (too early in boot).
 * 2 = refused: a teleport is already in flight (no write performed).
 * 3 = refused: engine busy — party streaming, menu request, or
 *     fade/transition running (no write performed).
 * -1 = bad field id (valid: 0..729 per fields.xml).
 * -2 = bad entry point (valid: 0..255; each map only implements a few
 *     arrival records — out-of-range reads bytecode garbage and usually
 *     lands on a black screen). */
int psx_debug_overlay_teleport(int fieldId, int entryPoint);

/* Debug-only: write party slot [0..2] to the kernel master slots at
 * 0x80062590+slot*4 (u32, low byte = char id, 0xFF = empty). gameState
 * currentParty (0x8006F368) is a per-frame copy of these — writing it
 * directly is reverted by the kernel sync (0x800A3200), so the master
 * is written instead and gameState follows on the next frame.
 * Automatically ORs the unlock-bitfield (0x8006F364) bits of ALL
 * non-empty party members — the menu lists members from the bitfield
 * and a member without their bit crashes field loading. Additionally
 * ORs `bitfieldBit` (0..10) when >= 0. 0 = ok, negative on bad
 * slot/char. */
int psx_debug_overlay_write_party_slot(int slot, int charId, int bitfieldBit);

/* Debug-only: write the 2-byte party unlock bitfield at 0x8006F364
 * (LE). 0 = ok, negative on bad value. */
int psx_debug_overlay_write_party_bitfield(int bitfield);

/* Debug-only: write gold u32 LE at 0x8006EF58. 0 = ok. */
int psx_debug_overlay_write_gold(unsigned int gold);

/* Debug-only: write fieldVars[var] u16 LE at 0x8006EF64+var*2 (+ the
 * active VM mirror at 0x800C3A68+var*2 while field is loaded, since
 * script handlers read the active copy and persist would otherwise
 * clobber a persistent-only write). 0 = ok, negative on bad var. */
int psx_debug_overlay_write_var(int var, int value);

/* Debug-only: write the encounter-trigger gate u32 LE at 0x800B2298.
 * Reference-verified (the reference's validation hook writes 0 here
 * to disable encounters for deterministic replay). Non-zero arms the
 * gate; the actual battle still requires field encounter data +
 * countdown = 0 (those vars' live addresses are not in the reference
 * address book). 0 = ok, negative on bad value. */
int psx_debug_overlay_force_battle(int value);

/* Debug-only: write the camera pose of the resident module (PSX world
 * units, integers). Field: current+desired as 6 x u32 fixed16
 * (0x800AF880/884/888, 0x800AF890/894/898, 0x800AF8B0/B4/B8,
 * 0x800AF8C0/C4/C8). Battle: current 3 x s16 (0x800D3354/5C) + desired
 * halves packed into 0x800D30A0/A8. Battling: 3 x u32 eye (0x8009871C) +
 * target (0x8009867C). Mirrors the pose into the panel editor. One-shot
 * unless the panel's free camera is enabled (freeze holds it). World
 * orbit mode has no positional pose and returns -3.
 * 0 = ok, negative otherwise. */
int psx_debug_overlay_camera_write(int ex, int ey, int ez,
                                    int ax, int ay, int az);

/* Debug-only: apply the event's varWrites (via write_var) then
 * teleport (in-place recipe, or boot-to-field when outside field).
 * `eventId` is the index into the events table loaded from events.xml.
 * 0 = armed, 4 = boot-to-field requested, 1 = refused (game not booted
 * far enough), 2/3 = refused (in flight / engine busy), negative on
 * bad id. A refused jump leaves RAM untouched. */
int psx_debug_overlay_event_jump(int eventId);

/* Read the 2-byte current field ID from 0x8006F94E (LE) and return
 * -1 when the field module is not resident (the mirror is only
 * meaningful while field owns the game state). */
int psx_debug_overlay_read_field_id(void);

#else /* Release: every call is a static-inline no-op (zero code, zero symbols). */
static inline void psx_debug_overlay_init(struct SDL_Window *w, SDL_GLContext c) { (void)w; (void)c; }
static inline void psx_debug_overlay_shutdown(void) {}
static inline void psx_debug_overlay_toggle(void) {}
static inline bool psx_debug_overlay_is_visible(void) { return false; }
static inline bool psx_debug_overlay_process_event(const SDL_Event *e) { (void)e; return false; }
static inline void psx_debug_overlay_pre_swap_target(unsigned int f) { (void)f; }
static inline void psx_debug_overlay_pre_swap(void) { psx_debug_overlay_pre_swap_target(0u); }
static inline bool psx_debug_overlay_swallow_keyboard(void) { return false; }
static inline void psx_debug_overlay_window_shot_arm(const char *p) { (void)p; }
static inline void psx_debug_overlay_capture_state(int *v, int *wc, int *sw) { (void)v; (void)wc; (void)sw; }
static inline int psx_debug_overlay_set_force_capture(int on) { (void)on; return 0; }
static inline int psx_debug_overlay_widget_action(const char *n, int v, int v2) { (void)n; (void)v; (void)v2; return -1; }
static inline int psx_debug_overlay_take_developer_mode_request(void) { return 0; }
static inline int psx_debug_overlay_take_kernel_menu_request(void) { return 0; }
static inline int psx_debug_overlay_take_field_boot_request(int *f, int *e) { (void)f; (void)e; return 0; }
static inline int psx_debug_overlay_teleport(int f, int e) { (void)f; (void)e; return -1; }
static inline int psx_debug_overlay_write_party_slot(int s, int c, int b) { (void)s; (void)c; (void)b; return -1; }
static inline int psx_debug_overlay_write_party_bitfield(int b) { (void)b; return -1; }
static inline int psx_debug_overlay_write_gold(unsigned int g) { (void)g; return -1; }
static inline int psx_debug_overlay_write_var(int v, int x) { (void)v; (void)x; return -1; }
static inline int psx_debug_overlay_force_battle(int v) { (void)v; return -1; }
static inline int psx_debug_overlay_camera_write(int ex,int ey,int ez,int ax,int ay,int az) { (void)ex;(void)ey;(void)ez;(void)ax;(void)ay;(void)az; return -1; }
static inline int psx_debug_overlay_event_jump(int id) { (void)id; return -1; }
static inline int psx_debug_overlay_read_field_id(void) { return -1; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_DEBUG_OVERLAY_H */
