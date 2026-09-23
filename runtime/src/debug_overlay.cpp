/* In-game developer debug overlay (Dear ImGui, toggled with Ctrl+F3).
 *
 * This TU is the build-gate anchor for PSX_DEBUG_OVERLAY: when the gate is
 * OFF (Release), the whole file is empty, no symbols are emitted, and
 * callers get the static-inline no-ops from debug_overlay.h. When the gate is
 * ON (Debug/RelWithDebInfo with PSX_RECOMP_UI=ON), every public function
 * defined here is linked and exported.
 *
 * Lifecycle:
 *   psx_debug_overlay_init  — main.cpp calls this once at startup with the
 *                             SDL window. The GL context is owned by
 *                             gpu_gl_renderer.c and is not reachable here,
 *                             so init only stores the window. ImGui
 *                             context creation is lazy.
 *   psx_debug_overlay_pre_swap_target — called by gpu_gl_renderer.c on the
 *                             main thread at the bottom of each present path,
 *                             BEFORE SDL_GL_SwapWindow. The normal wrapper
 *                             supplies FBO 0; transactional staging supplies
 *                             its private drawable-sized FBO. On the first call
 *                             it does the lazy ImGui init (captures the
 *                             current GL context via SDL_GL_GetCurrentContext,
 *                             which is the main thread's). Subsequent calls
 *                             either render an ImGui frame (when visible)
 *                             and/or flush a one-shot window_shot (when
 *                             armed). Hidden and unarmed = zero GL work
 *                             (zero state leakage).
 *   psx_debug_overlay_shutdown — ImGui teardown, called at runtime exit
 *                             while the GL context is still current.
 *
 * Threading: the main thread owns the GL context; the interpolation thread
 * (interp_present) has its OWN SDL_GL_SHARE_WITH_CURRENT_CONTEXT context and
 * never calls into this file. The debug server thread may call
 * psx_debug_overlay_window_shot_arm concurrently with pre_swap; the armed
 * flag is a single byte store, harmless under racy reads.
 */
#ifdef PSX_DEBUG_OVERLAY

#include "debug_overlay.h"
#include "debug_overlay_data.h"
#include "memory.h"
#include "overlay_capture.h"

/* The vendored Dear ImGui lives at recomp-ui/src/third_party/imgui. Its
 * include dirs are applied target-wide by recomp-ui/recomp_ui.cmake. */
#include "psx_sdl.h"
#include <imgui.h>
#if defined(PSX_SDL3)
#include <imgui_impl_sdl3.h>
#define PSX_IMGUI_SDL_INIT_OPENGL ImGui_ImplSDL3_InitForOpenGL
#define PSX_IMGUI_SDL_NEW_FRAME ImGui_ImplSDL3_NewFrame
#define PSX_IMGUI_SDL_PROCESS_EVENT ImGui_ImplSDL3_ProcessEvent
#define PSX_IMGUI_SDL_SHUTDOWN ImGui_ImplSDL3_Shutdown
#else
#include <imgui_impl_sdl2.h>
#define PSX_IMGUI_SDL_INIT_OPENGL ImGui_ImplSDL2_InitForOpenGL
#define PSX_IMGUI_SDL_NEW_FRAME ImGui_ImplSDL2_NewFrame
#define PSX_IMGUI_SDL_PROCESS_EVENT ImGui_ImplSDL2_ProcessEvent
#define PSX_IMGUI_SDL_SHUTDOWN ImGui_ImplSDL2_Shutdown
#endif
#include <imgui_impl_opengl3.h>

/* png_write_rgb is the same dependency-free header the debug server uses to
 * write its screenshot_file / wide_shot PNGs — reuse the exact mechanism for
 * byte-format consistency (RGB 8-bit, top-down, "stored" DEFLATE). */
#include "png_write.h"

/* SDL_opengl.h provides the core-1.x GL tokens and the GL_BACK /
 * GL_COLOR_BUFFER_BIT / GL_BACK / GL_RGBA / GL_RGB / GL_UNSIGNED_BYTE
 * constants the readback path needs. The few 3.0+ function prototypes
 * the readback + FBO-rebind path needs (glBindFramebuffer, glReadBuffer,
 * glDrawBuffer, glPixelStorei, glGetError, glReadPixels) live in
 * <GL/glext.h> on Linux / MinGW and <OpenGL/glext.h> on macOS — but only
 * inside #ifdef GL_GLEXT_PROTOTYPES ... #endif guards that ALSO require
 * GL_VERSION_3_0 to be defined. Rather than rely on the system's
 * version-guard maze (which differs between distros and breaks as soon
 * as the driver exposes a newer core version), we declare the six
 * functions we need ourselves. OpenGL 3 entry points are resolved through
 * SDL: Windows' opengl32.dll exports only OpenGL 1.1. ImGui's GL3 backend has
 * its own private loader and is unaffected. */
#include <SDL_opengl.h>
#if defined(__APPLE__)
#  include <OpenGL/gl.h>
#else
#  include <GL/gl.h>
#endif
#if !defined(_WIN32)
extern "C" {
    void glReadBuffer(GLenum mode);
    void glDrawBuffer(GLenum mode);
    void glPixelStorei(GLenum pname, GLint param);
    void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                      GLenum format, GLenum type, void *pixels);
    GLenum glGetError(void);
}
#endif

typedef void (APIENTRYP PfnDebugBindFramebuffer)(GLenum, GLuint);
static PfnDebugBindFramebuffer s_bind_framebuffer = nullptr;

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* The widget sections need the real runtime getters/setters — same extern
 * block pattern debug_server.c uses, so the overlay never forks the logic
 * (it calls the SAME functions the TCP handlers do). Headers are pulled
 * under extern "C" to make the dependency visible (no transitive-include
 * trust). */
extern "C" {
#include "gpu.h"                /* gpu_get_display_info, gpu_get_draw_area,
                                   gpu_get_crtc_debug, gpu_get_gp0_count */
#include "gpu_render.h"         /* gr_set_texture_filter, gr_texture_filter,
                                   gr_scale, gr_backend */
#include "gpu_gl_renderer.h"    /* gl_renderer_set_interpolation,
                                   gl_renderer_interpolation_diag,
                                   gl_renderer_perf_aggregate */
#include "event_ring.h"         /* event_ring_dump_file, event_ring_dump_json */
#include "latency_ring.h"       /* latency_ring_summary_json */
#include "starvation_ring.h"    /* starvation_ring_total, starvation_ring_get,
                                   starvation_ring_dump */
#include "cpu_state.h"          /* gte_set_display_aspect */
#include "spu.h"
#include "xg_render_presentation_host.h" /* host snapshot: true tick/present rates */

extern void psx_frame_interpolation_set(int enabled);
extern int psx_frame_interpolation_enabled(void);
extern int psx_native_semantic_fps_set(int fps);
extern "C" bool psx_native_render_presentation_host_snapshot(
    XgRenderPresentationHostSnapshot *out_snapshot);

/* Memory read accessor — used by the RAM Inspector section. */
extern uint8_t psx_read_byte(uint32_t addr);
extern void     psx_write_byte(uint32_t addr, uint8_t val);

/* Debug-server frame counter (set by record_frame in main.cpp; read by
 * every frame, and we sample it once per ImGui frame for the FPS widget). */
extern uint64_t s_frame_count;

/* Native-wide setter/getter. Both live in gpu.c with no public header;
 * extern block follows the same pattern debug_server.c uses. */
extern void psx_ws_set_native_wide(int on);
extern int  psx_ws_get_native_wide(void);

/* Backdrop-stretch on/pct — file-scope statics in gpu_gl_renderer.c. */
extern int g_ws_bd_stretch_on;
extern int g_ws_bd_stretch_pct;

/* True guest-vblank raise counter (interrupts.c, cycle-paced). */
extern uint64_t g_vblank_raise_count;

/* Launcher-equivalent video/audio setters defined in main.cpp (extern "C"
 * there). turbo loads is a direct extern "C" global, same as debug_server. */
extern "C" int  psx_video_get_supersampling(void);
extern "C" void psx_video_set_supersampling(int s);
extern "C" int  psx_video_get_antialiasing(void);
extern "C" void psx_video_set_antialiasing(int on);
extern "C" int  psx_video_get_screen_model(void);
extern "C" void psx_video_set_screen_model(int k);
extern "C" void psx_video_get_aspect(int *num, int *den);
extern "C" int  psx_debug_display_aspect(int num, int den, int adaptive);
extern "C" int  psx_video_set_aspect_runtime(int num, int den, int native_wide);
extern "C" int  psx_video_set_display_stretch(int num, int den);
extern "C" int  psx_video_get_display_stretch(int *num, int *den);
extern "C" int  psx_audio_get_spu_hq(void);
extern "C" void psx_audio_set_spu_hq(int on);
extern "C" int  psx_video_get_native_depth_test(void);
extern "C" void psx_video_set_native_depth_test(int on);
extern "C" int  gl_renderer_native_depth_view(void);
extern "C" void gl_renderer_set_native_depth_view(int mode);
extern "C" int  gl_renderer_native_wireframe(void);
extern "C" void gl_renderer_set_native_wireframe(int on);
extern "C" int  psx_video_get_window_width(void);
extern "C" void psx_video_set_window_width(int w);
extern "C" int  psx_video_get_vsync(void);
extern "C" void psx_video_set_vsync(int mode);
extern "C" int  psx_input_controller_ports_swapped(void);
extern "C" int  psx_input_controller_port_swap_available(void);
extern "C" int  psx_input_swap_controller_ports(void);
extern "C" int  g_turbo_loads_enabled;
/* Multi-disc set (main.cpp): roster size (0 = single-disc), the disc in the
 * drive, each disc's located image, and the hot swap itself. */
extern "C" int         psx_disc_count(void);
extern "C" int         psx_disc_mounted(void);
extern "C" const char* psx_disc_path(int disc_number);
extern "C" int         psx_disc_swap(int disc_number, char* error, size_t error_cap);
static void draw_disc_section(void);
}

/* Visibility flag. Flipped by psx_debug_overlay_toggle (Ctrl+F3) and by
 * the debug_server overlay_toggle command. Read by pre_swap, swallow_keyboard,
 * and (future) input routing. The flag's address is process-local; no
 * atomicity required — only the main thread and the debug server thread
 * touch it, and the worst-case race is a one-frame stale read. */
static bool s_visible = false;

/* Window passed to init. Stored as a void* internally because we use the
 * forward-declared struct from the header in the public API, and SDL.h here
 * pulls the full definition. */
static SDL_Window *s_win = nullptr;

/* Lazy-init latch. Set to true the first time pre_swap runs with a non-null
 * GL context (the main thread's context is current at that point — init
 * is called with NULL by design). */
static bool s_imgui_ready = false;

/* window_shot arm. The debug server thread sets s_window_shot_armed and
 * copies the path; the next pre_swap consumes the arm and clears it. The
 * consumed-shot is a single frame late (next vblank), by design — the back
 * buffer is only composed at the swap site. */
static bool s_window_shot_armed = false;
static char s_window_shot_path[260] = {0};

/* Sticky SDL text-input latch: drives SDL_StartTextInput/StopTextInput from
 * io.WantTextInput each visible frame, but only calls SDL when the state
 * actually flips — avoids per-frame Start spam and survives ImGui teardown
 * (the next NewFrame reads WantTextInput=0 so the input stays correctly
 * off when the overlay is hidden). */
static bool s_text_input_started = false;

/* Debug-only "force text capture" flag (armed only via the TCP
 * overlay_force_capture command — no visible widget). When set, the window
 * keeps a permanent InputText active, so ImGui reports
 * io.WantCaptureKeyboard=io.WantTextInput=true deterministically — the test
 * harness asserts the pad-mask path without injecting SDL events (impossible
 * over TCP) and without depending on the focused-window state. */
static bool s_force_text_capture = false;
static char s_force_text_buf[64] = {0};

/* Interpolation guard: on hidden→visible, if the runtime reports effective
 * frame interpolation, tear it down via gl_renderer_set_interpolation(0,
 * host_hz, target_hz) — host_hz and target_hz come from the diag helper so
 * we use the SAME argument convention as the runtime's own startup call at
 * main.cpp:6002. On visible→hidden, re-read the live host_hz/target_hz and
 * call set_interpolation(1, ...) to restore: re-reading honors any hz
 * change between open/close (display unplug, etc.) and matches the runtime's
 * recompute-on-call behavior. s_interp_guard_active is the "we did the work"
 * latch — drives the restore. */
static bool s_interp_guard_active = false;

/* XML-data load latch. dbg_data_load_all() is called once on the first
 * visible frame after lazy ImGui init — the data dir lives next to the
 * exe at <base path>/debug_overlay/data and is staged by the CMake POST_BUILD
 * step. Calling it before lazy init is unsafe: SDL_GetBasePath is stable
 * for the process lifetime but the GL context / ImGui aren't ready yet, so
 * we defer to the first pre_swap with a non-null s_imgui_ready. */
static bool s_dbg_data_loaded = false;

/* RAM Inspector widget state. The address is the top-left of the hex dump;
 * the type selector controls how the "current value" line is decoded. The
 * auto-refresh toggle (default on) re-reads psx_read_byte on every visible
 * frame; the manual refresh button forces one. The watch-list click updates
 * s_inspect_addr. */
static uint32_t s_inspect_addr    = 0x8006F94E;
static int      s_inspect_type     = 0;          /* 0=u8, 1=u16, 2=u32 (LE) */
static bool     s_inspect_autoref  = true;
static int      s_inspect_rows     = 16;         /* 16 rows × 16 bytes/row = 256 */
static int      s_inspect_refresh_tick = 0;

/* Cache of the last aspect ratio (num/den) the widget set, so the
 * aspect_num / aspect_den actions (which set one half at a time) feed the
 * OTHER half from the cache. aspect_set uses value/2 directly. The
 * initial values match the runtime's "off" state (4/3 = no squash). */
static int s_aspect_num = 4;
static int s_aspect_den = 3;

/* Last-known backend name as a small static string, refreshed at lazy init
 * (gr_backend() never changes after init). */
static const char *s_backend_name = "?";

/* Presented-frame + vblank rate tracking for the GPU State readouts.
 * pre_swap runs once per present on the main thread, so counting its calls
 * measures exactly what reaches the screen (game frames + interpolated
 * frames alike — never vblank pacing). The guest vblank raise counter
 * gives the real VSYNC rate beside it. 500 ms sample windows. */
static uint64_t s_present_count      = 0;
static uint64_t s_present_last       = 0;
static uint64_t s_present_tick       = 0;
static double   s_present_fps        = 0.0;
static uint64_t s_vblank_last        = 0;
static uint64_t s_vblank_tick        = 0;
static double   s_vblank_hz          = 0.0;
/* True presenter cadence (host ticks that ran present_next vs ticks that
 * actually composed+swapped). pre_swap counts every swap attempt including
 * blocked ones, so FPS (presented) reads high when the driver/compositor
 * eats frames; this pair is the ground truth for whether the FPS selector's
 * period retarget stuck. Sampled in the same window as s_present_fps. */
static uint64_t s_host_att_last      = 0;
static uint64_t s_host_pre_last      = 0;
static double   s_host_att_rate      = 0.0;
static double   s_host_pre_rate      = 0.0;
static int      s_host_snap_ok       = 0;
/* Per-tick outcome breakdown: empty (no batch ready), fence (GPU work not
 * done), held (re-presented retained frame = duplicate). High hold/empty at
 * 120/240 means the worker can't feed phases that fast. */
static uint64_t s_host_empty_last    = 0;
static uint64_t s_host_fence_last    = 0;
static uint64_t s_host_held_last     = 0;
static double   s_host_empty_rate    = 0.0;
static double   s_host_fence_rate    = 0.0;
static double   s_host_held_rate     = 0.0;
/* Phase-generation outcomes: READY (with last authored count), EXPIRED
 * (worker too slow for the tick deadline), WHOLE_ONLY. Tells whether 240
 * starves from slow generation or from authoring too few phases. */
static uint64_t s_ph_ready_last      = 0;
static uint64_t s_ph_exp_last        = 0;
static uint64_t s_ph_whole_last      = 0;
static double   s_ph_ready_rate      = 0.0;
static double   s_ph_exp_rate        = 0.0;
static double   s_ph_whole_rate      = 0.0;
static uint32_t s_ph_last_count      = 0;
static uint32_t s_ph_last_status     = 0;
/* Fresh endpoint presents/s, hold re-presents/s, and phase (non-whole)
/s. At 240 with healthy consumption ends ~= ticks; ends << ticks with
 * holds ~= ticks means batches die after one present. */
static uint64_t s_ph_ends_last       = 0;
static uint64_t s_ph_holds_last      = 0;
static uint64_t s_ph_vis_last        = 0;
static double   s_ph_ends_rate       = 0.0;
static double   s_ph_holds_rate      = 0.0;
static double   s_ph_vis_rate        = 0.0;

static void gpu_state_sample_rates(void)
{
    uint64_t now = SDL_GetTicks64();
    s_present_count++;
    if (s_present_tick == 0u) {
        s_present_tick = now;
        s_vblank_tick = now;
        s_present_last = s_present_count;
        s_vblank_last = g_vblank_raise_count;
        return;
    }
    if (now - s_present_tick < 500u) return;
    double dt = (double)(now - s_present_tick) / 1000.0;
    if (dt > 0.0) {
        s_present_fps = (double)(s_present_count - s_present_last) / dt;
        uint64_t vb = g_vblank_raise_count;
        if (vb >= s_vblank_last) {
            s_vblank_hz = (double)(vb - s_vblank_last) / dt;
        }
        s_vblank_last = vb;
        XgRenderPresentationHostSnapshot snap;
        if (psx_native_render_presentation_host_snapshot(&snap)) {
            s_host_snap_ok = 1;
            if (snap.presenter_attempts >= s_host_att_last)
                s_host_att_rate = (double)(snap.presenter_attempts -
                                           s_host_att_last) / dt;
            if (snap.presenter_presented >= s_host_pre_last)
                s_host_pre_rate = (double)(snap.presenter_presented -
                                           s_host_pre_last) / dt;
            s_host_att_last = snap.presenter_attempts;
            s_host_pre_last = snap.presenter_presented;
            if (snap.presenter_empty >= s_host_empty_last)
                s_host_empty_rate = (double)(snap.presenter_empty -
                                             s_host_empty_last) / dt;
            if (snap.presenter_fence_pending >= s_host_fence_last)
                s_host_fence_rate = (double)(snap.presenter_fence_pending -
                                             s_host_fence_last) / dt;
            if (snap.presenter_held >= s_host_held_last)
                s_host_held_rate = (double)(snap.presenter_held -
                                            s_host_held_last) / dt;
            s_host_empty_last = snap.presenter_empty;
            s_host_fence_last = snap.presenter_fence_pending;
            s_host_held_last = snap.presenter_held;
            GlRendererNativeCompilerDiagnostics cd;
            gl_renderer_native_compiler_diagnostics(&cd);
            uint64_t cr = cd.temporal_status_counts[GL_RENDERER_NATIVE_TEMPORAL_READY];
            uint64_t ce = cd.temporal_status_counts[GL_RENDERER_NATIVE_TEMPORAL_DEADLINE_EXPIRED];
            uint64_t cw = cd.temporal_status_counts[GL_RENDERER_NATIVE_TEMPORAL_WHOLE_ONLY];
            if (cr >= s_ph_ready_last)
                s_ph_ready_rate = (double)(cr - s_ph_ready_last) / dt;
            if (ce >= s_ph_exp_last)
                s_ph_exp_rate = (double)(ce - s_ph_exp_last) / dt;
            if (cw >= s_ph_whole_last)
                s_ph_whole_rate = (double)(cw - s_ph_whole_last) / dt;
            s_ph_ready_last = cr;
            s_ph_exp_last = ce;
            s_ph_whole_last = cw;
            s_ph_last_count = cd.last_temporal_phase_count;
            s_ph_last_status = cd.last_temporal_status;
            uint64_t pe = snap.presentation.presented_endpoints;
            uint64_t ph = snap.presentation.presented_holds;
            uint64_t pv = snap.presentation.visual_only_updates;
            if (pe >= s_ph_ends_last)
                s_ph_ends_rate = (double)(pe - s_ph_ends_last) / dt;
            if (ph >= s_ph_holds_last)
                s_ph_holds_rate = (double)(ph - s_ph_holds_last) / dt;
            if (pv >= s_ph_vis_last)
                s_ph_vis_rate = (double)(pv - s_ph_vis_last) / dt;
            s_ph_ends_last = pe;
            s_ph_holds_last = ph;
            s_ph_vis_last = pv;
        } else {
            s_host_snap_ok = 0;
        }
    }
    s_present_last = s_present_count;
    s_present_tick = now;
    s_vblank_tick = now;
}

/* Independent tools window. The overlay used to render inside the game GL
 * window (covering the game); it now owns a separate SDL window + GL
 * context, so the debugger lives beside the game instead of on top of it.
 * s_tools_win/s_tools_ctx are created lazily on the first pre_swap (the
 * main thread owns the game GL context there). Entry GL state is captured
 * per pre_swap call (present paths do not all share one context) and
 * restored after tools-window work. s_legacy_inline selects the
 * old in-game render path when the tools window cannot be created
 * (headless/constrained GL) — same widgets, zero behavior delta. */
static SDL_Window   *s_tools_win     = nullptr;
static SDL_GLContext s_tools_ctx     = nullptr;
static bool          s_separate_ready = false;
static bool          s_legacy_inline  = false;

/* Forward: input-path helpers defined beside process_event (need SDL event
 * types), used early by the free-camera hold. */
static bool tools_input_active(void);
static SDL_Window *overlay_active_window(void);

/* State for the three new panels (teleport / party / gold+vars). */
static int      s_teleport_field_id   = 1;
static int      s_teleport_entry      = 0;
static int      s_teleport_filter_sel = -1;
static char     s_teleport_filter[64] = {0};
static char     s_teleport_status[96] = {0};
static int      s_teleport_status_frames = 0;
static int      s_last_teleport_id   = -1;
/* In-flight teleport tracking. The old code keyed completion off the
 * 0x800B0078 pointer, but that is the scheduler's per-dispatch current-
 * actor pointer (rewritten for every actor every frame by 0x800A2030),
 * so it was useless both as an "is field active" signal and as a
 * completion signal. Track the requested target + arm time instead and
 * confirm via the fieldID mirror in pre_swap. -1 = idle. */
static int      s_teleport_target_id = -1;
static int      s_teleport_source_id = -1;
static uint64_t s_teleport_arm_ms = 0u;
static uint64_t s_teleport_deadline_ms = 0u;
static uint64_t s_teleport_ready_ms = 0u;
static constexpr uint64_t kTeleportSettleMs = 10000u;
static constexpr uint64_t kTeleportTimeoutMs = 12000u;
static constexpr uint64_t kTeleportBootTimeoutMs = 60000u;
static constexpr uint64_t kTeleportSameMapConfirmMs = 2000u;
/* Boot-to-field request (teleport issued outside the field module).
 * Staged here, consumed by debug_server_apply_pending_guest_transition
 * at the next safe vblank edge, which writes the persistent target and
 * yanks to MainLoop(1) — the same mechanism the Kernel Menu transition
 * uses. -1 = none pending. */
static int      s_field_boot_target = -1;
static int      s_field_boot_entry = 0;

static int      s_party_slot[3]      = {0, 1, 2};
static int      s_party_bitfield     = 0x07FF;
static bool     s_party_unlock[11]   = { true, true, true, true, true,
                                         true, true, true, true, true, true };
static int      s_party_status_frames = 0;
static char     s_party_status[128]  = {0};
static int      s_party_level_p[3]   = {1, 1, 1};
static int      s_party_level_e[3]   = {1, 1, 1};
/* Stat editor state (roster-direct). u16: HP/maxHP/MP/maxMP. u8: five
 * attributes + hit/evade. EXP remaining (u32 x2) primes authentic
 * level-ups: the result loop fires on crossed thresholds. */
static int      s_party_hp[3]    = {0, 0, 0};
static int      s_party_mhp[3]   = {0, 0, 0};
static int      s_party_mp[3]    = {0, 0, 0};
static int      s_party_mmp[3]   = {0, 0, 0};
static int      s_party_atk[3]   = {0, 0, 0};
static int      s_party_def[3]   = {0, 0, 0};
static int      s_party_agi[3]   = {0, 0, 0};
static int      s_party_eth[3]   = {0, 0, 0};
static int      s_party_efd[3]   = {0, 0, 0};
static int      s_party_hit[3]   = {0, 0, 0};
static int      s_party_eva[3]   = {0, 0, 0};
static int      s_party_expr[3]  = {0, 0, 0};
static int      s_party_expe[3]  = {0, 0, 0};

static int      s_gold_value         = 0;
static int      s_gold_dirty         = 0;
static int      s_vars_filter_sel    = -1;
static char     s_vars_filter[64]    = {0};
static int      s_var_edit[512]      = {0};

/* Global enemy picker: (name, set, def). Parsed from
 * docs/xenogears-disc1-filesystem.md dir 0x0D visual-file lists.
 * Lanes of one battle must share a set (single enemy pair loads),
 * so picking an enemy from another set switches the battle set. */
struct DbgBattleEnemy { const char *name; int set; int def; };
static const DbgBattleEnemy kBattleEnemies[] = {
    { "Jackal", 0, 0 },
    { "Hobgob", 0, 1 },
    { "Armor Grub", 0, 2 },
    { "Armor Wasp", 0, 3 },
    { "Jackal", 1, 0 },
    { "Hobgob", 1, 1 },
    { "Lucre Bug", 1, 2 },
    { "Nolucre Bug", 1, 3 },
    { "Hobgob", 1, 4 },
    { "Armor Grub", 1, 5 },
    { "Hobgob", 1, 6 },
    { "Jackal dupe", 1, 7 },
    { "Gigafoot", 2, 0 },
    { "Gigafoot", 2, 1 },
    { "Trooper", 2, 2 },
    { "Trooper", 2, 3 },
    { "Forest Elf", 3, 0 },
    { "Dive Bomber", 3, 1 },
    { "Gonzalez", 3, 2 },
    { "Leonardo", 3, 3 },
    { "Heinrich", 3, 4 },
    { "Vargas", 3, 5 },
    { "Dwarf", 3, 6 },
    { "Forest Elf", 3, 7 },
    { "Aveh Corporal", 4, 0 },
    { "Aveh Soldier", 4, 1 },
    { "Aveh Corporal", 4, 2 },
    { "Aveh Corporal", 4, 3 },
    { "Aveh Soldier", 4, 4 },
    { "Aveh Guard", 4, 5 },
    { "Aveh Guard", 4, 6 },
    { "ShakhanGuard", 4, 7 },
    { "Sand Man", 5, 0 },
    { "Tin Robo", 5, 1 },
    { "Dune Man", 5, 2 },
    { "Neo Tin Robo", 5, 3 },
    { "Supa Tin Robo", 5, 4 },
    { "Shellbelle", 6, 0 },
    { "Hammerhead", 6, 1 },
    { "Shellbell F1", 6, 2 },
    { "Hammerhead F1", 6, 3 },
    { "Carrier", 7, 0 },
    { "Shadey", 7, 1 },
    { "Suzarn", 7, 2 },
    { "Carrier F1", 7, 3 },
    { "Dan", 8, 0 },
    { "Wiseman", 8, 1 },
    { "Wiseman", 8, 2 },
    { "Big Joe", 8, 3 },
    { "Alpha Weltall", 9, 0 },
    { "Musha Mk100", 9, 1 },
    { "NeoMushaMk100", 9, 2 },
    { "Assassin", 10, 0 },
    { "Assassin", 10, 1 },
    { "Rankar Dragon", 11, 0 },
    { "Elly", 11, 1 },
    { "Rankar R", 11, 2 },
    { "WM Rankar 1", 11, 3 },
    { "WM Rankar 2 [Rankar Dragon", 11, 4 },
    { "Land Crab", 11, 5 },
    { "Weltall]", 11, 6 },
    { "Armor Grub", 12, 0 },
    { "Armor Wasp", 12, 1 },
    { "Acid Frog", 12, 2 },
    { "Sand Shark", 12, 3 },
    { "Mullet", 12, 4 },
    { "Rain Frog", 12, 5 },
    { "Sand Shark", 12, 6 },
    { "Vierge", 13, 0 },
    { "Alpha Weltall", 13, 1 },
    { "True Weltall", 13, 2 },
    { "Clawknight", 14, 0 },
    { "Swordknight", 14, 1 },
    { "Aegisknight", 14, 2 },
    { "Wandknight", 14, 3 },
    { "Clawknight R", 14, 4 },
    { "Swordknight R", 14, 5 },
    { "Aegisknight R", 14, 6 },
    { "Wandknight R", 14, 7 },
    { "Alkanshel", 15, 0 },
    { "Schpariel", 15, 1 },
    { "Alkanshel", 15, 2 },
    { "HarquebusMk10", 16, 0 },
    { "Hatamoto Mk3", 16, 1 },
    { "Mechanic", 16, 2 },
    { "HarquebusMk10 duplicate", 16, 3 },
    { "Neo Wels", 17, 0 },
    { "Wels", 17, 1 },
    { "Wyvern", 18, 0 },
    { "Miang's Gear", 18, 1 },
    { "Haishao", 18, 2 },
    { "Haishao", 18, 3 },
    { "Calamity", 19, 0 },
    { "Dora", 19, 1 },
    { "Amphysvena", 20, 0 },
    { "Opiomorph", 20, 1 },
    { "Nomad Fix Bot", 21, 0 },
    { "Salvager", 21, 1 },
    { "Dora dupe", 21, 2 },
    { "Twin Burner", 22, 0 },
    { "Spear Trooper", 22, 1 },
    { "Quadrafoot dupe", 22, 2 },
    { "Rotten Sod", 23, 0 },
    { "Slugger", 23, 1 },
    { "Abandon", 23, 2 },
    { "Orphan", 23, 3 },
    { "Dorothy dupe", 23, 4 },
    { "Croaker Tribe", 24, 0 },
    { "Forbidden", 24, 1 },
    { "Forbidden", 24, 2 },
    { "Forbidden", 24, 3 },
    { "Forbidden", 24, 4 },
    { "Croaker Tribe", 24, 5 },
    { "Margie", 25, 0 },
    { "Shakhan Guard", 25, 1 },
    { "GuardMachine", 25, 2 },
    { "Shakhan Monk", 25, 3 },
    { "Freelancer", 25, 4 },
    { "Defencer", 25, 5 },
    { "Traffic Jam", 26, 0 },
    { "Traffic Jam", 26, 1 },
    { "Vierge dupe", 26, 2 },
    { "Medusoid", 27, 0 },
    { "May Fly", 27, 1 },
    { "Medusoid", 27, 2 },
    { "Edelweiss dupe", 27, 3 },
    { "Rico", 28, 0 },
    { "Death Scythe dupe", 28, 1 },
    { "Medusoid dupe", 28, 2 },
    { "Hecht", 29, 0 },
    { "Super Aerod", 29, 1 },
    { "Medusoid dupe", 29, 2 },
    { "Ripper", 30, 0 },
    { "Phobia", 30, 1 },
    { "Dorothy", 30, 2 },
    { "Death Eater", 30, 3 },
    { "Brigandier", 31, 0 },
    { "Crescens", 31, 1 },
    { "Medusoid dupe", 31, 2 },
    { "Edelweiss", 32, 0 },
    { "Planter", 32, 1 },
    { "Littlefoot", 33, 0 },
    { "Solaris Guard", 33, 1 },
    { "Security Cube", 33, 2 },
    { "Eagle Gunner", 34, 0 },
    { "Eagle Armor", 34, 1 },
    { "Eagle Blade", 34, 2 },
    { "White Knight", 35, 0 },
    { "Citadel", 35, 1 },
    { "Avalanche", 35, 2 },
    { "Shinobi Mk0", 36, 0 },
    { "Mammoth", 36, 1 },
    { "Eagle Blade dupe", 36, 2 },
    { "Conjurer", 37, 0 },
    { "Edin", 37, 1 },
    { "Gun Drone", 37, 2 },
    { "Fis-6", 38, 0 },
    { "Fis-6Mechanic", 38, 1 },
    { "Wyrm", 39, 0 },
    { "Death Scythe", 39, 1 },
    { "Wyrm", 39, 2 },
    { "Death Scythe", 39, 3 },
    { "Hopper", 40, 0 },
    { "Lil' Kobold", 40, 1 },
    { "Eagle Blade dupe", 40, 2 },
    { "Tears", 41, 0 },
    { "Gimmick", 41, 1 },
    { "Neo Tears", 41, 2 },
    { "Neo Gimmick", 41, 3 },
    { "Siebzehn", 42, 0 },
    { "Achtzehn", 42, 1 },
    { "Achtzehn", 42, 2 },
    { "Sand Tripper", 43, 0 },
    { "Quadrafoot", 43, 1 },
    { "Achtzehn dupe", 43, 2 },
    { "Sufal", 44, 0 },
    { "Sufal Gear", 44, 1 },
    { "Sufal Gear", 44, 2 },
    { "Sufal", 44, 3 },
    { "Sufal Mass", 44, 4 },
    { "Margie", 45, 0 },
    { "Ramsus", 45, 1 },
    { "Miang", 45, 2 },
    { "Fei", 45, 3 },
    { "Id", 45, 4 },
    { "Main Gun", 46, 0 },
    { "Small Gun", 46, 1 },
    { "Big Joe", 47, 0 },
    { "Scud", 47, 1 },
    { "Dwarf", 47, 2 },
    { "Leonardo", 47, 3 },
    { "Heinrich", 47, 4 },
    { "Vargas", 47, 5 },
    { "Gonzalez", 47, 6 },
    { "Forest Elf v2 duplicates", 47, 7 },
    { "Giant Wels", 48, 0 },
    { "Haishao", 48, 1 },
    { "Rhino", 49, 0 },
    { "Batrat", 49, 1 },
    { "Gebler Guard", 50, 0 },
    { "Swordsman", 50, 1 },
    { "Swordsman", 50, 2 },
    { "Id", 51, 0 },
    { "Weltall-Id dupe", 51, 1 },
    { "Id Weltall-Id", 52, 0 },
    { "Weltall-Id dupe", 52, 1 },
    { "[Rattan", 52, 2 },
    { "Mugwort", 52, 3 },
    { "Regulus]", 52, 4 },
    { "Shakhan", 53, 0 },
    { "Vendetta", 53, 1 },
    { "Shakhan dupe", 53, 2 },
    { "Bladegash", 54, 0 },
    { "Skyghene", 54, 1 },
    { "Marinebasher", 54, 2 },
    { "Grandgrowl", 54, 3 },
    { "Bladegash", 54, 4 },
    { "G Elements", 55, 0 },
    { "Hammer", 55, 1 },
    { "Grahf", 56, 0 },
    { "Grahf", 56, 1 },
    { "Executioner", 56, 2 },
    { "Original Weltall", 56, 3 },
    { "Dominia", 57, 0 },
    { "Tolone", 57, 1 },
    { "Seraphita", 57, 2 },
    { "Kelvena Kelvina", 57, 3 },
    { "Tolone", 57, 4 },
    { "Seraphita", 57, 5 },
    { "Snow Bot", 58, 0 },
    { "Bot", 58, 1 },
    { "Seraphita dupe", 58, 2 },
    { "Kelvena dupe", 58, 3 },
    { "Rapid Fire", 59, 0 },
    { "Breaker", 59, 1 },
    { "Fuel Tank", 59, 2 },
    { "Kelvena dupe", 59, 3 },
    { "Aragonite", 60, 0 },
    { "Merman", 60, 1 },
    { "Etone", 61, 0 },
    { "Etone", 61, 1 },
    { "Twin Burner", 61, 2 },
    { "Neo Etone", 61, 3 },
    { "Neo Etone", 61, 4 },
    { "Metatron", 62, 0 },
    { "Sundel", 62, 1 },
    { "Harlute", 62, 2 },
    { "Marlute", 62, 3 },
    { "Unused 5th placeholder", 62, 4 },
    { "stats present in Marlute battle", 62, 5 },
    { "Deus", 63, 0 },
    { "Deus", 63, 1 },
    { "Deus (Final Form variations: 70K", 63, 2 },
    { "52K", 63, 3 },
    { "40K)", 63, 4 },
    { "Deus Deus 1st Form", 64, 0 },
    { "Ft. Hurricane", 64, 1 },
    { "Pedestal", 65, 0 },
    { "Airwalk", 65, 1 },
    { "Eagle Wing", 65, 2 },
    { "Golem", 66, 0 },
    { "Griffon", 66, 1 },
    { "Griffon", 66, 2 },
    { "Redrum", 67, 0 },
    { "Bloody", 67, 1 },
    { "Bloody Bros", 67, 2 },
    { "(Weltall-Id)", 68, 0 },
    { "Eagle Gunner", 68, 1 },
    { "Wyvern", 68, 2 },
    { "Ramsus", 68, 3 },
    { "Pecking Duck", 69, 0 },
    { "Lil'Allemange", 69, 1 },
    { "Id Xenogears-Id", 70, 0 },
    { "Original Weltall (Wiseman)", 70, 1 },
    { "Tusk-Tusk", 71, 0 },
    { "Dragon", 71, 1 },
    { "Dragon", 71, 2 },
    { "Dragon", 71, 3 },
    { "Tusk-Tusk", 71, 4 },
    { "Dragon", 71, 5 },
    { "Dragon", 71, 6 },
    { "Dragon", 71, 7 },
    { "Wind Seraph", 72, 0 },
    { "Earth Seraph", 72, 1 },
    { "Power Seraph", 72, 2 },
    { "Sword Seraph", 73, 0 },
    { "Heal Seraph", 73, 1 },
    { "Fire Seraph", 73, 2 },
    { "Water Seraph", 73, 3 },
    { "Sword Seraph", 74, 0 },
    { "Heal Seraph", 74, 1 },
    { "Fire Seraph", 74, 2 },
    { "Water Seraph [Weltall-Id", 74, 3 },
    { "Wyvern", 74, 4 },
    { "Airwalk", 74, 5 },
    { "Yggdrasil II]", 74, 6 },
    { "Urobolus", 75, 0 },
};
static constexpr int kBattleEnemyCount = 297;
static constexpr int kBattleEnemySetMax = 75;

/* Battle arenas (dir 0x0F pairs): arena n <-> env file 6+2n, init 7+2n.
 * Parsed from docs/xenogears-disc1-filesystem.md (75 stages). */
struct DbgBattleArena { int id; const char *name; };
static const DbgBattleArena kBattleArenas[] = {
    { 0, "stage0 - Aveh Transport Ship" },
    { 1, "stage1 - Blackmoon Forest" },
    { 2, "stage2 - Nortune (Kislev) - D Block Alleyway" },
    { 3, "stage3 - Mountain Path" },
    { 4, "stage4 - Mountain Cave" },
    { 5, "stage5 - Anima Dungeon/Zeboim Ruins?" },
    { 6, "stage6 - Forest" },
    { 7, "stage7 - Desert" },
    { 8, "stage8 - Bledavik Tournament Stage" },
    { 9, "stage9 - Nortune - Sewers" },
    { 10, "stage10 - Nortune - Gear Paddocks" },
    { 11, "stage11 - Goliath Factory" },
    { 12, "stage12 - Desert - Wyrm Fight" },
    { 13, "stage13 - Fatima Castle Stairway" },
    { 14, "stage14 - Nisan" },
    { 15, "stage15 - Gear Hangar" },
    { 16, "stage16 - Yggdrasil II - Ocean (Starboard)" },
    { 17, "stage17 - Stalactite Cave - Calamity Fight" },
    { 18, "stage18 - Lahan Under Attack" },
    { 19, "stage19 - Metal Corridor" },
    { 20, "stage20 - Goliath" },
    { 21, "stage21 - Reaper Ship - Storeroom" },
    { 22, "stage22 - Reaper Ship - Meat Locker" },
    { 23, "stage23 - Facility - Hallways" },
    { 24, "stage24 - Stalactite Cave Depths" },
    { 25, "stage25 - Mountain Pass" },
    { 26, "stage26 - Giant Experiment Chamber" },
    { 27, "stage27 - Kefeinzel" },
    { 28, "stage28 - Sargasso" },
    { 29, "stage29 - Goliath Factory - Fis-6 Fight" },
    { 30, "stage30 - Zeboim City" },
    { 31, "stage31 - Fatima Castle Courtyard" },
    { 32, "stage32 - Nisan Underground Facility" },
    { 33, "stage33 - Anima Relic Puzzle Room? Unused?" },
    { 34, "stage34 - Shevat Hallway" },
    { 35, "stage35 - Babel Tower" },
    { 36, "stage36 - Babel Tower Platform" },
    { 37, "stage37 - Ship Hallway" },
    { 38, "stage38 - Solaris Gateway Bridge" },
    { 39, "stage39 - Zeboim Bridge" },
    { 40, "stage40 - Hecht" },
    { 41, "stage41 - Waters Surface?" },
    { 42, "stage42 - Platforms" },
    { 43, "stage43 - Overworld (Snow)" },
    { 44, "stage44 - Overworld (Plains)" },
    { 45, "stage45 - Shevat? Floating Platforms" },
    { 46, "stage46 - Destroyed Platform" },
    { 47, "stage47 - Overworld (Barrens)" },
    { 48, "stage48 - Solaris Waterway" },
    { 49, "stage49 - Raziel Tree" },
    { 50, "stage50 - Fatima Castle Hallway" },
    { 51, "stage51 - 4 Platforms Stage" },
    { 52, "stage52 - Soylent System - Sufal Mass Fight" },
    { 53, "stage53 - Overworld (Ocean)" },
    { 54, "stage54 - Solaris Room?" },
    { 55, "stage55 - Energy Core Bridge" },
    { 56, "stage56 - Merkava Room" },
    { 57, "stage57 - Merkava Corridor" },
    { 58, "stage58 - Merkava Platform" },
    { 59, "stage59 - Merkava Hallway" },
    { 60, "stage60 - Deus Rebirth Chamber" },
    { 61, "stage61 - Golgoda" },
    { 62, "stage62 - Babel Tower - Hanger" },
    { 63, "stage63 - Zohar" },
    { 64, "stage64 - Space Void?" },
    { 65, "stage65 - Deus Inner Core" },
    { 66, "stage66 - Path of Sephirot" },
    { 67, "stage67 - Babel Tower Platforms" },
    { 68, "stage68 - Hecht Starboard" },
    { 69, "stage69 - Yggdrasil II - Ocean (Port)" },
    { 70, "stage70 - Clouds - Achtzehn Fight" },
    { 71, "stage71 - Capsized Ship" },
    { 72, "stage72 - Ft. Jasper" },
    { 73, "stage73 - Facility Room" },
    { 74, "stage74 - Ft. Jasper - Central Chamber?" },
};
static constexpr int kBattleArenaCount = 75;

/* State for the Battle Selector panel (W6, rewritten with game knowledge).
 * The old panel only armed the encounter gate (0x800B2298) and hoped the
 * random countdown would fire — best-effort, often nothing. The new panel
 * mirrors the engine's own explicit-battle path (field opcode 0x71
 * StartBattle at 0x80093568 / FE 84 at 0x800933F8): stage a 32-byte
 * formation record into the resident section-6 table (0x800658DC, static,
 * installed per map by InstallFieldScene), forward the staged config
 * (0x800B2356 -> 0x8005954C), write selected index (0x80059508) + request
 * flag (0x800594F8 = 0), then commit the handoff (0x800ADBDC = 0,
 * 0x800ADBE0 = 0, 0x800ADB88 = 1, LAST). The field coordinator consumes it
 * exactly like a scripted explicit battle (ordinary snapshot + return).
 * Party/gears/levels stage through the persistent records the battle
 * loader copies at startup (kernel slots via the safe formation API,
 * gear byte roster+0xA0, level bytes roster+0x62/63). */
static int      s_battle_trigger_val = 1;     /* encounter gate 0x800B2298 (randoms on/off) */
static int      s_battle_enemy_set   = 0;     /* derived pool: set of the picked lanes */
static int      s_battle_arena       = 0;
static int      s_battle_lane_def[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool     s_battle_lane_gear[8] = {false, false, false, false, false, false, false, false};
static char     s_battle_enemy_filter[64] = {0};
static int      s_battle_party[3]    = {0, 0xFF, 0xFF};
static bool     s_battle_mounted[3]  = {false, false, false};
static int      s_battle_gear[3]     = {0xFF, 0xFF, 0xFF};  /* per-slot gear id -> that slot char's roster+0xA0 */
static int      s_battle_level_p[3]  = {1, 1, 1};
static int      s_battle_level_e[3]  = {1, 1, 1};
static char     s_battle_status[160] = {0};
static int      s_battle_status_frames = 0;
static bool     s_battle_edit_init = false;

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

/* State for the Event Jump panel (W6). The event list is loaded by
 * dbg_data_events() from events.xml. Verified events have a non-greyed
 * Jump button; unverified events are shown but disabled. */
static int      s_event_filter_sel   = -1;
static char     s_event_filter[64]   = {0};
static bool     s_allow_unverified_events = false;
static int      s_event_jump_status_frames = 0;
static char     s_event_jump_status[128] = {0};

/* ---- small helpers ------------------------------------------------------ */

static bool bind_overlay_target(GLuint target_fbo)
{
    const GLenum buffer = target_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK;

    if (!s_bind_framebuffer) {
        s_bind_framebuffer = reinterpret_cast<PfnDebugBindFramebuffer>(
            SDL_GL_GetProcAddress("glBindFramebuffer"));
    }
    if (!s_bind_framebuffer) return false;
    s_bind_framebuffer(GL_READ_FRAMEBUFFER, target_fbo);
    s_bind_framebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
    glReadBuffer(buffer);
    glDrawBuffer(buffer);
    return true;
}

/* Read the selected drawable-sized framebuffer to a top-down RGB uint8_t*
 * buffer (caller frees with std::free). On any GL error, the returned buffer
 * is empty and the caller skips the PNG write. The hidden+armed path can use
 * this helper without creating an ImGui frame. */
static uint8_t *capture_window_rgb(GLuint target_fbo, int *out_w, int *out_h)
{
    *out_w = 0;
    *out_h = 0;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return nullptr;

    if (!bind_overlay_target(target_fbo)) return nullptr;

    const size_t row_bytes = (size_t)ww * 3;
    uint8_t *rgb = (uint8_t *)std::malloc(row_bytes * (size_t)wh);
    if (!rgb) {
        (void)bind_overlay_target(target_fbo);
        return nullptr;
    }
    /* GL_PACK_ALIGNMENT=1 prevents row padding for our tight RGB rows. */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    GLenum err = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    (void)bind_overlay_target(target_fbo);
    if (err != GL_NO_ERROR) {
        std::free(rgb);
        return nullptr;
    }
    for (int y = 0; y < wh / 2; y++) {
        uint8_t *top = rgb + (size_t)y * row_bytes;
        uint8_t *bottom = rgb + (size_t)(wh - 1 - y) * row_bytes;

        for (size_t x = 0; x < row_bytes; x++) {
            const uint8_t value = top[x];
            top[x] = bottom[x];
            bottom[x] = value;
        }
    }
    *out_w = ww;
    *out_h = wh;
    return rgb;
}

/* Write an already-top-down RGB buffer to a PNG using the same shared
 * png_write_rgb the debug server uses. */
static void write_rgb_png(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *fp = std::fopen(path, "wb");
    if (!fp) return;
    png_write_rgb(fp, rgb, (uint32_t)w, (uint32_t)h);
    std::fclose(fp);
}

/* ---- Widget sections (GPU State / RAM Inspector / Toggles / Rings) ----- */

static const char *texfilter_label(int v) { return v ? "bilinear" : "nearest"; }

static void draw_gpu_state_section(void)
{
    ImGui::Text("Backend         : %s", s_backend_name);

    /* Requested presentation scale (same source as the Toggles slider).
     * gr_scale() reports the GL raster, which stays 1 by design under the
     * native renderer — that phantom 1x is what this row used to show. */
    ImGui::Text("Internal scale  : %dx", psx_video_get_supersampling());

    ImGui::Text("Texture filter  : %s",
                texfilter_label(gr_texture_filter()));

    /* Same accessors the TCP gpu_state handler uses (gpu.h). Display comes
     * straight from the CRTC registers: 320x216 is genuine (worldmap modes
     * and field letterbox), not a stale read. */
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    uint32_t hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0, hr1 = 0, hr2 = 0;
    gpu_get_crtc_debug(&hx1, &hx2, &hy1, &hy2, &hr1, &hr2);
    ImGui::Text("Display         : %ux%u  (depth=%d, %s)",
                (unsigned)di.width, (unsigned)di.height,
                di.depth24 ? 24 : 15,
                di.disabled ? "disabled" : "enabled");
    ImGui::Text("Display range   : x=[%u..%u] y=[%u..%u]",
                hx1, hx2, hy1, hy2);

    /* Display-buffer draw state omitted: the live draw area alternates
     * every frame between the double-buffered contexts by design, so a
     * readout carries no information (verified against gpu_get_draw_area
     * + the field's paired render contexts). RAM Inspector covers
     * manual inspection. */

    /* Configured game aspect (4:3, or 16:9 when widescreen is selected).
     * Not the GTE/squash internals. */
    {
        int anum = 4, aden = 3;
        psx_video_get_aspect(&anum, &aden);
        int snum = 0, sden = 0;
        if (psx_video_get_display_stretch(&snum, &sden)) {
            ImGui::Text("Aspect          : %d:%d (stretched to %d:%d)",
                        anum, aden, snum, sden);
        } else if (aden > 0) {
            ImGui::Text("Aspect          : %d:%d", anum, aden);
        } else {
            ImGui::Text("Aspect          : (unset)");
        }
    }

    /* Native semantic target (managed in Toggles). Presented rate is the
     * FPS row below; the target alone never moves. */
    ImGui::Text("Native semantic : %d FPS target",
                gl_renderer_native_interpolation_target_fps());

    /* Presented frames per second: pre_swap counts every present (game +
     * interpolated alike), never vblank pacing. Replaces the GL perf-ring
     * average, whose sampling stays off unless something enables it. */
    if (s_present_fps > 0.0) {
        ImGui::Text("FPS (presented) : %.1f", s_present_fps);
    } else {
        ImGui::Text("FPS (presented) : (measuring...)");
    }
    /* Ground-truth presenter cadence: due ticks/s vs completed
     * compose+swap/s. Diverges from FPS (presented) when attempts don't
     * complete (blocked swap, empty tick). */
    if (s_host_snap_ok) {
        ImGui::Text("Presenter       : ticks %.1f/s, swapped %.1f/s",
                    s_host_att_rate, s_host_pre_rate);
        ImGui::Text("  outcomes      : empty %.1f/s, fence %.1f/s, held %.1f/s",
                    s_host_empty_rate, s_host_fence_rate, s_host_held_rate);
        ImGui::Text("  phases        : ready %.1f/s (last x%u %s), expired %.1f/s, whole %.1f/s",
                    s_ph_ready_rate, s_ph_last_count,
                    gl_renderer_native_temporal_status_name(s_ph_last_status),
                    s_ph_exp_rate, s_ph_whole_rate);
        ImGui::Text("  presents      : fresh %.1f/s, holds %.1f/s, phased %.1f/s",
                    s_ph_ends_rate, s_ph_holds_rate, s_ph_vis_rate);
    } else {
        ImGui::Text("Presenter       : (no host)");
    }

    /* True guest VSYNC rate (cycle-paced raise counter). */
    ImGui::Text("Vblanks         : %.2f Hz", s_vblank_hz);
}

/* ---- RAM Inspector ----------------------------------------------------- */

static char hex_nibble(uint8_t n) { return (char)(n < 10 ? '0' + n : 'a' + n - 10); }

static void draw_ram_inspector_section(void)
{
    int addr_i = (int)s_inspect_addr;
    if (ImGui::InputScalar("Addr", ImGuiDataType_U32, &addr_i, NULL, NULL,
                          "%08X", ImGuiInputTextFlags_CharsHexadecimal)) {
        s_inspect_addr = (uint32_t)addr_i & 0x1FFFFFFFu;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &s_inspect_autoref);
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        s_inspect_refresh_tick = 0;
    }

    ImGui::RadioButton("u8",  &s_inspect_type, 0); ImGui::SameLine();
    ImGui::RadioButton("u16", &s_inspect_type, 1); ImGui::SameLine();
    ImGui::RadioButton("u32", &s_inspect_type, 2);
    ImGui::SameLine();
    ImGui::TextDisabled("(LE)");

    if (s_inspect_autoref || s_inspect_refresh_tick == 0) {
        uint32_t v = 0;
        int n = 0;
        switch (s_inspect_type) {
            case 0: v = psx_read_byte(s_inspect_addr); n = 1; break;
            case 1:
                v = (uint32_t)psx_read_byte(s_inspect_addr)
                  | ((uint32_t)psx_read_byte(s_inspect_addr + 1) << 8);
                n = 2; break;
            case 2:
                v = (uint32_t)psx_read_byte(s_inspect_addr)
                  | ((uint32_t)psx_read_byte(s_inspect_addr + 1) << 8)
                  | ((uint32_t)psx_read_byte(s_inspect_addr + 2) << 16)
                  | ((uint32_t)psx_read_byte(s_inspect_addr + 3) << 24);
                n = 4; break;
            default: v = 0; n = 1; break;
        }
        ImGui::Text("0x%08X: 0x%0*X  (%u bytes)",
                    s_inspect_addr, n * 2, v, n);
    }
    if (s_inspect_autoref) s_inspect_refresh_tick++;

    /* Hex+ASCII dump — same psx_read_byte accessor the TCP read_ram
     * handler uses, so the widget is bit-for-bit consistent with what
     * the same address returns over TCP (asserted by the live test
     * for 256 bytes at 0x8006F94E). */
    ImGui::Separator();
    if (ImGui::BeginChild("ram_dump", ImVec2(0, 220), true)) {
        for (int row = 0; row < s_inspect_rows; row++) {
            uint32_t base = s_inspect_addr + (uint32_t)row * 16u;
            char line[80];
            int pos = std::snprintf(line, sizeof(line), "%08X  ", base);
            for (int col = 0; col < 16; col++) {
                uint8_t b = psx_read_byte(base + (uint32_t)col);
                line[pos++] = hex_nibble(b >> 4);
                line[pos++] = hex_nibble(b & 0xF);
                line[pos++] = (col == 7) ? '-' : ' ';
            }
            line[pos++] = ' ';
            for (int col = 0; col < 16; col++) {
                uint8_t b = psx_read_byte(base + (uint32_t)col);
                line[pos++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            }
            line[pos] = '\0';
            bool dummy = false;
            char lbl[96];
            std::snprintf(lbl, sizeof(lbl), "%s##row%d", line, row);
            if (ImGui::Selectable(lbl, &dummy,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                s_inspect_addr = base;
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Text("Named watches (from ram_map.xml):");
    if (dbg_data_ram_map_missing()) {
        ImGui::TextDisabled("(no ram_map.xml loaded)");
    } else {
        int nw = 0;
        const DbgWatch *w = dbg_data_watches(&nw);
        const char *current_region = nullptr;
        for (int i = 0; i < nw; i++) {
            if (!current_region ||
                std::strcmp(current_region, w[i].region) != 0) {
                current_region = w[i].region;
                ImGui::Text("%s", current_region);
            }
            ImGui::SameLine();
            char lbl[160];
            std::snprintf(lbl, sizeof(lbl), "0x%08X %s##j%d",
                          w[i].addr, w[i].name, i);
            if (ImGui::SmallButton(lbl)) {
                s_inspect_addr = w[i].addr;
            }
        }
    }
}

/* ---- Toggles ------------------------------------------------------------ */

static bool s_developer_mode_request = false;
static bool s_kernel_menu_request = false;

static void request_developer_mode(void)
{
    s_developer_mode_request = true;
}

static void request_kernel_menu(void)
{
    s_kernel_menu_request = true;
}

static void draw_toggles_section(void)
{
    /* Single aspect selector (4:3 / 3:2 / 16:9). 4:3 and 16:9 run the same
     * stack the launcher fixed-aspect applies, but live: display fit,
     * widescreen projection (GTE + ws configure) and the native view + cull
     * reconfigure. 3:2 keeps the 4:3 game view and only stretches the
     * present, so the guest's non-square pixels show square (round battle
     * reticle/palette, square portraits); FMVs stay 4:3. */
    static const char *kAspectRatio[] = { "4:3", "3:2 (stretched)", "16:9" };
    int vanum = 4, vaden = 3;
    psx_video_get_aspect(&vanum, &vaden);
    int aspect_index = (vaden > 0 && vanum * 3 != vaden * 4) ? 2
                     : psx_video_get_display_stretch(nullptr, nullptr) ? 1 : 0;
    if (ImGui::Combo("Aspect ratio", &aspect_index, kAspectRatio, 3)) {
        if (aspect_index == 2) {
            (void)psx_video_set_aspect_runtime(16, 9, 1);
            s_aspect_num = 16; s_aspect_den = 9;
            g_ws_bd_stretch_on = 1;
        } else if (aspect_index == 1) {
            (void)psx_video_set_display_stretch(3, 2);
            s_aspect_num = 4; s_aspect_den = 3;
            g_ws_bd_stretch_on = 0;
        } else {
            (void)psx_video_set_aspect_runtime(4, 3, 0);
            s_aspect_num = 4; s_aspect_den = 3;
            g_ws_bd_stretch_on = 0;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("3:2 stretches the 4:3 picture to Xenogears' pixel\n"
                          "aspect: circles round, portraits square. Same field\n"
                          "of view as 4:3; FMVs stay 4:3.");

    ImGui::Separator();
    ImGui::TextDisabled("Launcher settings:");

    bool dith = gpu_dithering_enabled() != 0;
    if (ImGui::Checkbox("Dithering", &dith)) {
        gpu_dithering_set(dith ? 1 : 0);
    }

    /* Native renderer only: per-pixel depth between classified 3D surfaces.
     * Off = pure OT order. */
    bool depth_test = psx_video_get_native_depth_test() != 0;
    if (ImGui::Checkbox("Native depth test", &depth_test)) {
        psx_video_set_native_depth_test(depth_test ? 1 : 0);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth-test producer-classified 3D draws (models,\n"
                          "terrain, sprites, shadows and effects all write;\n"
                          "blended texels never do). Applies next frame.");
    /* Debug presentation of the GPU planes (render scale > 1 only). */
    static const char *kDepthViews[] = { "Off", "Depth (colour)", "Policy", "Depth (grey)" };
    int depth_view = gl_renderer_native_depth_view();
    if (ImGui::Combo("Depth view", &depth_view, kDepthViews, 4))
        gl_renderer_set_native_depth_view(depth_view);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth (colour): red near to blue far; Depth (grey): near\n"
                          "bright, far dark (black = no certified surface).\n"
                          "Policy: grey NONE, yellow TEST, green TEST_WRITE,\n"
                          "red = classified but without a usable depth plane.");
    bool wireframe = gl_renderer_native_wireframe() != 0;
    if (ImGui::Checkbox("Wireframe", &wireframe))
        gl_renderer_set_native_wireframe(wireframe ? 1 : 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draw every triangle as lines over black\n"
                          "(presentation only; overrides the depth view).");

    /* VSync mode: 1 = on, 0 = off (immediate), -1 = adaptive. Live via
     * apply_present_cadence; native-semantic overrides still apply. */
    static const char *kVsyncModes[] = { "Off", "On", "Adaptive" };
    int vs = psx_video_get_vsync();
    int vs_index = vs >= 1 ? 1 : (vs == 0 ? 0 : 2);
    if (ImGui::Combo("VSync", &vs_index, kVsyncModes, 3)) {
        static const int modes[] = {0, 1, -1};
        psx_video_set_vsync(modes[vs_index]);
    }

    int ss = psx_video_get_supersampling();
    if (ImGui::SliderInt("Supersampling (internal scale)", &ss, 1, 8)) {
        psx_video_set_supersampling(ss);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Live: the GL raster rebuilds at the next present"
                          " (one-frame hitch).");
    }

    int ww = psx_video_get_window_width();
    if (ImGui::SliderInt("Window width (px)", &ww, 640, 3840)) {
        psx_video_set_window_width(ww);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Window resolution; height follows the configured"
                          " aspect ratio.");
    }

    bool aa = psx_video_get_antialiasing() != 0;
    if (ImGui::Checkbox("Antialiasing (present filter)", &aa)) {
        psx_video_set_antialiasing(aa ? 1 : 0);
    }

    static const char *kScreenModels[] = {
        "Raw", "CRT", "Composite", "Trinitron"
    };
    int sm = psx_video_get_screen_model();
    if (ImGui::Combo("Screen model", &sm, kScreenModels, 4)) {
        psx_video_set_screen_model(sm);
    }

    static const char *kNativeInterpolationTargets[] = {
        "30 FPS (Original)", "60 FPS", "75 FPS", "120 FPS", "144 FPS",
        "165 FPS", "240 FPS"
    };
    /* True target (denominator getter lies for 75/144/165). Unknown maps
     * to 60. */
    int native_fps = gl_renderer_native_interpolation_target_fps();
    int native_fps_index = 1;
    {
        static const int known[] = {30, 60, 75, 120, 144, 165, 240};
        for (int i = 0; i < 7; ++i) {
            if (known[i] == native_fps) { native_fps_index = i; break; }
        }
    }
    if (ImGui::Combo("Native semantic target", &native_fps_index,
                     kNativeInterpolationTargets, 7)) {
        static const int targets[] = {30, 60, 75, 120, 144, 165, 240};
        /* Full retarget (phase pool + presenter tick), not just the GL
         * denominator: the host period is what paces worker presents. */
        (void)psx_native_semantic_fps_set(targets[native_fps_index]);
    }

    bool tl = g_turbo_loads_enabled != 0;
    if (ImGui::Checkbox("Turbo loads (unpaced during CD loads)", &tl)) {
        g_turbo_loads_enabled = tl ? 1 : 0;
    }

    bool hq = psx_audio_get_spu_hq() != 0;
    if (ImGui::Checkbox("High-quality SPU (float shadow)", &hq)) {
        psx_audio_set_spu_hq(hq ? 1 : 0);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Game-native debug menu:");
    const bool ports_swapped = psx_input_controller_ports_swapped() != 0;
    const bool can_swap_ports = psx_input_controller_port_swap_available() != 0;
    if (!can_swap_ports) ImGui::BeginDisabled();
    if (ImGui::Button(ports_swapped
            ? "Use current controller as Controller 1"
            : "Use current controller as Controller 2")) {
        (void)psx_input_swap_controller_ports();
    }
    if (!can_swap_ports) ImGui::EndDisabled();
    ImGui::Text("Current controller route: Controller %d", ports_swapped ? 2 : 1);
    if (!can_swap_ports)
        ImGui::TextDisabled("Controller routing is fixed during replay or netplay.");

    const bool developer_ram = memory_developer_ram_enabled() != 0;
    if (!developer_ram) {
        if (ImGui::Button("Enable 8 MiB Developer Mode")) {
            request_developer_mode();
        }
    }
    if (ImGui::Button("Open native Kernel Menu")) {
        request_kernel_menu();
    }
    const uint32_t development_word =
        (uint32_t)psx_read_byte(0x80010000u) |
        ((uint32_t)psx_read_byte(0x80010001u) << 8) |
        ((uint32_t)psx_read_byte(0x80010002u) << 16) |
        ((uint32_t)psx_read_byte(0x80010003u) << 24);
    ImGui::Text("RAM profile: %s", developer_ram ? "Developer 8 MiB" : "Retail 2 MiB");
    ImGui::Text("Development word: 0x%08X", development_word);
    ImGui::TextWrapped(developer_ram
        ? "Developer mode is active. Open the Kernel Menu to select Field or "
          "Battle; their native debug overlays load at 0x80280000."
        : "Developer Mode enables the 8 MiB development path without leaving "
          "the current module. The Kernel Menu is available independently.");
}

/* ---- Rings -------------------------------------------------------------- */

static void draw_rings_section(void)
{
    ImGui::Text("Event ring");
    {
        /* Same accessor the TCP event_ring_tail handler uses. */
        static char evbuf[8 * 1024];
        int n = event_ring_dump_json(evbuf, (int)sizeof(evbuf), 3);
        int total = -1;
        const char *p = std::strstr(evbuf, "\"total\":");
        if (p) std::sscanf(p, "\"total\":%d", &total);
        ImGui::Text("  count: %s  last3: %d bytes",
                    total >= 0 ? "" : "?", n);
        if (total >= 0) ImGui::SameLine(), ImGui::Text("total=%d", total);
        if (ImGui::SmallButton("Dump JSON##event")) {
            (void)event_ring_dump_file("event_ring.json");
        }
    }

    ImGui::Text("Latency ring");
    {
        /* Same accessor the TCP latency handler uses. */
        static char sum[2048];
        int n = latency_ring_summary_json(sum, (int)sizeof(sum), 240);
        if (n > 0) {
            char compact[128];
            int copy = n < 120 ? n : 120;
            std::memcpy(compact, sum, (size_t)copy);
            compact[copy] = '\0';
            ImGui::TextWrapped("  %s", compact);
        } else {
            ImGui::TextDisabled("  (empty)");
        }
        if (ImGui::SmallButton("Dump JSON##latency")) {
            /* Mirror the TCP latency handler's response shape (summary
             * + frames array) so the JSON file is interchangeable. */
            FILE *f = std::fopen("latency_ring.json", "w");
            if (f) {
                std::fprintf(f, "{\"summary\":");
                int w = latency_ring_summary_json(sum, (int)sizeof(sum), 240);
                std::fwrite(sum, 1, (size_t)w, f);
                std::fprintf(f, ",\"frames\":");
                static char rawbuf[16 * 1024];
                int w2 = latency_ring_dump_json(rawbuf, (int)sizeof(rawbuf), 120);
                std::fwrite(rawbuf, 1, (size_t)w2, f);
                std::fprintf(f, "}\n");
                std::fclose(f);
            }
        }
    }

    ImGui::Text("Starvation ring");
    {
        /* Same accessors the TCP starv_ring handler uses. */
        uint64_t total = starvation_ring_total();
        ImGui::Text("  total events: %llu", (unsigned long long)total);
        StarvationEntry e;
        int shown = 0;
        uint64_t seq = total;
        while (shown < 3 && seq > 0) {
            seq--;
            if (!starvation_ring_get(seq, &e)) break;
            const char *kind_name =
                (e.kind == 0) ? "none" :
                (e.kind == 1) ? "tx_data_w" :
                (e.kind == 2) ? "rx_data_r" :
                (e.kind == 3) ? "stat_r" :
                (e.kind == 4) ? "ctrl_w" :
                (e.kind == 15) ? "pc_sample" : "other";
            ImGui::Text("  #%llu k=%-9s func=0x%08X",
                        (unsigned long long)e.seq, kind_name, e.current_func);
            shown++;
        }
        if (ImGui::SmallButton("Dump JSON##starv")) {
            /* Fresh write via per-entry accessors (starvation_ring_dump
             * is one-shot). */
            uint64_t total = starvation_ring_total();
            FILE *f = std::fopen("starvation_ring.json", "w");
            if (f) {
                std::fprintf(f, "{\"total\":%llu,\"entries\":[",
                             (unsigned long long)total);
                int emitted = 0;
                for (uint64_t s = 0; s < total; s++) {
                    StarvationEntry e;
                    if (!starvation_ring_get(s, &e)) continue;
                    std::fprintf(f,
                        "%s{\"seq\":%llu,\"kind\":%u,"
                        "\"cyc\":%llu,\"us\":%llu,"
                        "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\","
                        "\"in_exc\":%u}",
                        emitted ? "," : "",
                        (unsigned long long)e.seq, (unsigned)e.kind,
                        (unsigned long long)e.psx_cycle_count,
                        (unsigned long long)e.host_us,
                        e.current_func, e.last_store_pc, e.in_exception);
                    emitted++;
                }
                std::fprintf(f, "],\"emitted\":%d}\n", emitted);
                std::fclose(f);
            }
        }
    }
}

/* ---- WRITE actions: teleport / party / gold / vars --------------------- */

/* Verified address book (see debug_overlay/data/addrs.xml). Every write
 * goes through psx_write_byte to keep the path identical to the read-
 * accessor the existing inspector uses. u32/u16 values are emitted as
 * 4/2 sequential byte writes (little-endian).
 *
 * Map teleport mirrors what the game's own scripted transitions do
 * (opcode 0x98 FieldScriptChangeFieldWhenReady at 0x800932D0 and the
 * door-walk path through 0x80092894): stage the destination field in
 * fieldMapNumber (0x8004F34C), stage the entry point in the ACTIVE VM
 * mirror (0x800C3A68+2, not just the persistent gameState copy), then
 * clear the poll gate (0x800ADBEC) last as the commit. The field poll
 * inside RunFieldCoordinator (0x800784A0) picks the recipe up on the
 * next frame and runs the full completion sequence (bundle apply + var
 * init + actor reset + script restart + texture/stream re-init).
 *
 * Deliberately NOT written (the old 7-write recipe got these wrong):
 * - 0x800ADB64 is the MENU request slot (idle 0xFF; FE 58 writes 3,
 *   consumed by FieldLoadAndOpenMenu 0x800799D4). Writing 0 queued a
 *   bogus Normal-Menu request on every teleport.
 * - 0x8006EF82/84/86 are script vars 15/16/17, not a "position" the
 *   poll reads. Player placement comes from the destination map's
 *   arrival table (docs/xenogears/field/02 §5) via the entry var.
 *   Zeroing them just corrupted script state.
 * - 0x800ADBC4 is the party-skin load state (0xFF = idle). Writing
 *   0xFF during a party stream faked "idle"; instead refuse while busy.
 * - 0x8004F308/0x800ADB90 (music/anim gates) must be 0 for the poll to
 *   fire, but zeroing them while busy fakes readiness. The poll retries
 *   every frame on its own, so never write them — only the commit gate.
 * We do NOT touch fieldID (0x8006F94E) — the persist step copies the
 * target there itself — and we do NOT call loadNewField (not reentrant;
 * the poll is the only correct caller). */
static constexpr uint32_t kAddr_fieldID               = 0x8006F94Eu;
static constexpr uint32_t kAddr_fieldMapNumber        = 0x8004F34Cu;
static constexpr uint32_t kAddr_teleportGate1         = 0x800ADBECu;
static constexpr uint32_t kAddr_teleportGateMusic     = 0x8004F308u;
static constexpr uint32_t kAddr_teleportGateAnim      = 0x800ADB90u;
static constexpr uint32_t kAddr_fieldEntryPointU16    = 0x8006EF66u;
/* Resident loaded-module id (SLUS BSS, set by the overlay loader
 * 0x800199CC; -1 = loading/none). 1 = Field. NOTE: this belongs to the
 * developer debug menu's state machine and reads a constant 0xFFFFFFFF
 * during normal retail play in this runtime — it is NOT a usable
 * field-active signal (see xg_render_host_semantic_module in main.cpp).
 * Kept only so the panel can show why the old detection was wrong. */
static constexpr uint32_t kAddr_residentLoadedModule  = 0x800592C0u;
/* Ground truth for the resident module (same as main.cpp): the
 * field/world/battle/battling overlays all load at 0x8006FAF0, so the
 * first instruction word identifies whoever is resident. Confirmed live
 * across sustained field/world/battle sessions. */
static constexpr uint32_t kAddr_overlaySignature      = 0x8006FAF0u;
static constexpr uint32_t kSigFieldOverlay            = 0x00000004u;
static constexpr uint32_t kSigWorldOverlay            = 0x00000005u;
static constexpr uint32_t kSigBattleOverlay           = 0x00000006u;
static constexpr uint32_t kSigBattlingOverlay         = 0x00000007u;
/* gameState pointer (SLUS), set once the boot has progressed far enough
 * to own game state. Gates the boot-to-field path: yanking to MainLoop
 * before this exists has no game state to stage into. */
static constexpr uint32_t kAddr_gameStatePtr           = 0x8005A39Cu;
static constexpr uint32_t kAddr_activeFieldVarsBase   = 0x800C3A68u;
static constexpr uint32_t kAddr_activeEntryPointU16   = 0x800C3A68u + 2u;
/* Busy gates (read-only checks, never written): party-skin streaming in
 * flight, a menu request staged, or a fade/transition already running. */
static constexpr uint32_t kAddr_partyLoadState         = 0x800ADBC4u;
static constexpr uint32_t kAddr_menuRequest            = 0x800ADB64u;
static constexpr uint32_t kAddr_fadeGate               = 0x800B2118u;
static constexpr int      kMaxTeleportFieldId          = 729;
static constexpr int      kMaxTeleportEntryPoint       = 255;
/* currentParty in gameState (0x8006F368) is a per-frame COPY: the field
 * kernel's sync routine (0x800A3200, runs every frame) reloads it from the
 * kernel party slots at 0x80062590 (3 x u32, low byte = char id,
 * 0xFF = empty). External writes to 0x8006F368 are silently reverted within
 * a frame — verified live (wtrace + write/read-back). The kernel slots are
 * the MASTER representation: writing them sticks and propagates to both
 * gameState and the fieldVars mirrors (vars 0x3E/0x40/0x42 @0x8006EFA2). */
static constexpr uint32_t kAddr_currentParty        = 0x8006F368u;
static constexpr uint32_t kAddr_kernelPartySlots    = 0x80062590u;
static constexpr uint32_t kAddr_partyBitfield       = 0x8006F364u;
static constexpr uint32_t kAddr_partyFrameMask      = 0x8006F366u;
/* Party mirrors + mount bytes + exclusion mask. The engine maintains
 * mirror[i] == slot[i] on every path (boot init FUN_8001ad4c, stager
 * FUN_8008a7dc, rotate opcode FUN_8008c334). gameState+0x22B1+slot are
 * mount-state bytes (docs/xenogears/field/07 §10: 0 = on foot, 1 =
 * mounted); the battle loader takes the Gear placement path from them.
 * The party writer leaves them alone (worldmap reconcile + field mount
 * controller own them); only the battle selector drives them, explicitly
 * per user checkbox. The menu (docs/xenogears/menu/08 §9) refuses
 * exchanges for +0x2318 bits. */
static constexpr uint32_t kAddr_partyMirrorBase      = 0x8006FABCu;
static constexpr uint32_t kAddr_partyMountBase     = 0x8006F8E5u;
static constexpr uint32_t kAddr_partyExclusion       = 0x8006F94Cu;
static constexpr uint32_t kAddr_gold                = 0x8006EF58u;
static constexpr uint32_t kAddr_fieldVarsBase       = 0x8006EF64u;
static constexpr uint32_t kAddr_partyRosterBase     = 0x8006D8A0u;
/* Camera SVECTORs (3 x s16 LE) — verified-static by addrs.xml +
 * reference validation (validateFieldEntities.cpp lines 228-233). */
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
/* Battle region (11 x 0x170) — verified-static by addrs.xml +
 * reference validation (validateBattle.cpp line 25). */
static constexpr uint32_t kAddr_battleEntities      = 0x800CCCE8u;
static constexpr int      kBattleEntityStride       = 0x170;
static constexpr int      kBattleEntityCount        = 11;
/* Encounter trigger gate (reference-verified: validation writes 0 to
 * disable encounters for deterministic replay). Non-zero = encounters
 * armed (per reference's `playMusicAuthorized != 0` gate). Live
 * addresses of the other encounter vars (timer / countdown /
 * triggerTime[32]) are NOT in the reference address book. */
static constexpr uint32_t kAddr_encounterTrigger    = 0x800B2298u;
/* Explicit-battle handoff (field opcode 0x71 StartBattle 0x80093568 /
 * FE 84 0x800933f8 — same six writes, same order, same gates).
 * Formation table: 16 x 32B resident section-6 records, installed per map
 * at a static address by InstallFieldScene (FUN_8007008c -> 0x800658dc). */
static constexpr uint32_t kAddr_battleFormTable      = 0x800658DCu;
static constexpr int      kBattleFormCount           = 16;
static constexpr int      kBattleFormSize            = 32;
static constexpr uint32_t kAddr_battleSelIndex       = 0x80059508u;
static constexpr uint32_t kAddr_battleReqFlag        = 0x800594F8u;
static constexpr uint32_t kAddr_battleConfig         = 0x8005954Cu;
static constexpr uint32_t kAddr_battleConfigSrc      = 0x800B2356u;
static constexpr uint32_t kAddr_battleGateDC         = 0x800ADBDCu;
static constexpr uint32_t kAddr_battleGateE0         = 0x800ADBE0u;
static constexpr uint32_t kAddr_battleGate88         = 0x800ADB88u;
static constexpr uint32_t kAddr_battleGateE4         = 0x800ADBE4u;
static constexpr int      kPartyRosterStride        = 0xA4;
static constexpr int      kPartyRosterCount         = 11;
static constexpr int      kFieldVarsCount           = 512;

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

/* True when the field module is the active module. Reads the resident
 * overlay signature at 0x8006FAF0 (same ground truth as
 * xg_render_host_semantic_module in main.cpp): 4 = Field, 5 = Worldmap,
 * 6 = Battle. Neither 0x800B0078 (scheduler transient, rewritten every
 * dispatch) nor 0x800592C0 (dev-menu state machine, constant 0xFFFFFFFF
 * in retail play) is a usable signal in this runtime. */
static bool field_module_active(void)
{
    return read_u32_le(kAddr_overlaySignature) == kSigFieldOverlay;
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

/* Public read accessor used by the panel + tests. Returns -1 when the
 * field module is not the active module (the mirror is only meaningful
 * while field owns the game state). */
int psx_debug_overlay_read_field_id(void)
{
    if (!field_module_active()) return -1;
    return (int)read_u16_le(kAddr_fieldID);
}

/* Dry-run of the teleport preconditions: same return codes as
 * psx_debug_overlay_teleport but performs no write. Lets event_jump
 * validate BEFORE applying its varWrites, so a refused jump leaves RAM
 * untouched. Sets *out_boot when the request must go through the
 * boot-to-field path (field module not resident). */
static int psx_debug_overlay_teleport_guard(int fieldId, int entryPoint, bool *out_boot)
{
    if (s_teleport_target_id >= 0) {
        return 2;
    }
    if (fieldId < 0 || fieldId > kMaxTeleportFieldId) {
        return -1;
    }
    if (entryPoint < 0 || entryPoint > kMaxTeleportEntryPoint) {
        return -2;
    }
    if (!field_module_active()) {
        /* Boot-to-field path: needs game state to exist so there is a
         * persistent fieldID/entry to stage (too early in boot = refuse;
         * same code as before, new meaning documented in the header). */
        if (read_u32_le(kAddr_gameStatePtr) == 0u) {
            return 1;
        }
        if (out_boot) *out_boot = true;
        return 0;
    }
    if (out_boot) *out_boot = false;
    /* Refuse while the engine is mid-flight elsewhere: party-skin
     * streaming owns 0x800ADBC4 (0xFF = idle), a staged menu request
     * owns 0x800ADB64 (0xFF = idle), and a running fade/transition owns
     * 0x800B2118 (0 = idle). Arming on top of any of those corrupts the
     * in-progress operation (probable black screen). The music/anim/CD
     * gates are intentionally NOT checked here: the poll retries them
     * every frame on its own, so an early arm is harmless. */
    if (read_u32_le(kAddr_partyLoadState) != 0xFFu) {
        return 3;
    }
    if (read_u32_le(kAddr_menuRequest) != 0xFFu) {
        return 3;
    }
    if (read_u16_le(kAddr_fadeGate) != 0u) {
        return 3;
    }
    return 0;
}

int psx_debug_overlay_teleport(int fieldId, int entryPoint)
{
    bool boot = false;
    int rc = psx_debug_overlay_teleport_guard(fieldId, entryPoint, &boot);
    if (rc != 0) {
        return rc;
    }
    if (boot) {
        /* Not in field: stage a boot-to-field request for the safe-point
         * yank (debug_server_apply_pending_guest_transition). No RAM is
         * written here — staging happens on the emu thread right before
         * the transition, so the current module cannot clobber it. */
        spu_debug_music_quarantine_begin();
        s_field_boot_target = fieldId;
        s_field_boot_entry = entryPoint;
        s_teleport_source_id = -1;
        s_teleport_target_id = fieldId;
        s_last_teleport_id = fieldId;
        s_teleport_arm_ms = SDL_GetTicks64();
        s_teleport_deadline_ms = s_teleport_arm_ms + kTeleportBootTimeoutMs;
        return 4;
    }
    s_teleport_source_id = psx_debug_overlay_read_field_id();
    spu_debug_music_quarantine_begin();
    /* Stage values first, commit gate last: the poll consumes the trio
     * together, and the TCP path can race the game thread, so the gate
     * must never be visible before the values are.
     * Entry goes to BOTH mirrors: playable-actor init reads the active
     * VM mirror (FUN_800a3018(2) -> 0x800C3A68+2), while the persist
     * step (0x800A30FC) copies active -> persistent right before the
     * transition. Writing only the persistent copy (the old bug) meant
     * persist clobbered it with the stale active value and the
     * destination map spawned at the wrong arrival record — often
     * out-of-bounds garbage -> black screen. */
    write_u16_le(kAddr_activeEntryPointU16, (uint16_t)((unsigned)entryPoint & 0xFFFFu));
    write_u16_le(kAddr_fieldEntryPointU16,  (uint16_t)((unsigned)entryPoint & 0xFFFFu));
    write_u32_le(kAddr_fieldMapNumber,      (uint32_t)fieldId);
    write_u32_le(kAddr_teleportGate1,        0u);
    s_teleport_target_id = fieldId;
    s_last_teleport_id = fieldId;
    s_teleport_arm_ms = SDL_GetTicks64();
    s_teleport_deadline_ms = s_teleport_arm_ms + kTeleportTimeoutMs;
    return 0;
}

/* Party formation validation + atomic apply.
 *
 * Crash model (field-overlay FUN_8008a790/bc80/bdd8/c334, SLUS
 * FUN_8001ad4c/InitializeCharacterSkinSet, menu docs/xenogears/menu/08):
 * - The engine never validates IDs (annotation: "IDs are not validated
 *   against 0..10") — a garbage id sizes heap allocs wrong → refuse.
 * - Its own validator rejects duplicates and full parties; lookups
 *   (FUN_8009fa00) poison on any 0xFF hole → refuse duplicates, pack left.
 * - The menu contract keeps >=1 member → refuse all-empty.
 * - Per-member 0x5000 resource buffers + mirrors + flags travel WITH slot
 *   contents on every engine path; a direct slot write orphans them. The
 *   field-entry init rebuilds them from the current slots, so a validated
 *   formation takes full effect on the next field change (teleport/door);
 *   mid-field the new leader/followers desync until then (documented).
 * - Busy engine (skin streaming / staged menu / fade) + wrong module make
 *   any write unsafe → refuse with the teleport-style codes.
 * - Locked members (availability AND of 0x1D30&0x1D32 fails) are auto-
 *   unlocked by OR-ing both masks (explicit user intent, monotonic, never
 *   cleared) instead of refused — same philosophy as the old auto-OR.
 *
 * Codes: 0 = applied. 1 = field module not resident. 2 = engine busy.
 * -1 = bad id (not 0..10/0xFF). -2 = empty formation. -3 = duplicate. */
static int psx_debug_overlay_party_guard(int c0, int c1, int c2)
{
    int c[3] = { c0, c1, c2 };
    for (int i = 0; i < 3; i++) {
        if (c[i] != 0xFF && (c[i] < 0 || c[i] > 10)) return -1;
    }
    if (c[0] == 0xFF && c[1] == 0xFF && c[2] == 0xFF) return -2;
    for (int i = 0; i < 3; i++) {
        if (c[i] == 0xFF) continue;
        for (int j = i + 1; j < 3; j++) {
            if (c[i] == c[j]) return -3;
        }
    }
    if (!field_module_active()) return 1;
    if (read_u32_le(kAddr_partyLoadState) != 0xFFu) return 2;
    if (read_u32_le(kAddr_menuRequest) != 0xFFu) return 2;
    if (read_u16_le(kAddr_fadeGate) != 0u) return 2;
    return 0;
}

/* Branch uniformity (Lahan-escape crash finding, verified live with
 * T1/T2 bisects): a formation mixing story-unlocked and newly-unlocked
 * members crashes map transitions (field scripts branch on raw 0x1D30
 * bits via FieldScriptCheckAvailablePartyMember), while story-exact AND
 * full-unlock states are safe. So when any written member fails the
 * PRE-write availability AND, 0x1D30 goes to 0x07FF (all 11, uniform
 * set-paths everywhere). 0x1D32 keeps story semantics (menu/battle
 * availability AND unchanged apart from the member bits). Returns true
 * when the full unlock fired. */
static bool psx_debug_overlay_party_unlock(int *members, int n,
                                           uint16_t *out_bf, uint16_t *out_fm)
{
    uint16_t pre_bf = read_u16_le(kAddr_partyBitfield);
    uint16_t pre_fm = read_u16_le(kAddr_partyFrameMask);
    uint16_t bf = pre_bf;
    uint16_t fm = pre_fm;
    bool fresh = false;
    for (int i = 0; i < n; i++) {
        if (members[i] == 0xFF) continue;
        if (members[i] < 0 || members[i] > 10) continue;
        bf |= (uint16_t)(1u << members[i]);
        fm |= (uint16_t)(1u << members[i]);
        if (!((pre_bf & pre_fm) & (1u << members[i]))) fresh = true;
    }
    if (fresh) bf = 0x07FFu;
    write_u16_le(kAddr_partyBitfield, bf);
    write_u16_le(kAddr_partyFrameMask, fm);
    if (out_bf) *out_bf = bf;
    if (out_fm) *out_fm = fm;
    return fresh;
}

/* Apply a validated, packed formation: unlock bits (both masks, OR-only)
 * first, then mirrors and slots. Bitfield-first keeps every intermediate
 * frame in the safe (superset-bits, old-or-new-slots) shape — never the
 * crash combo (new slot, clear bit) the old code proved fatal.
 * Deliberately NOT touched: gameState+0x22B1 (the add-opcodes clear it,
 * but worldmap's reconcile (FUN_80075d4c) compares it against 0x8006EE70
 * and counts it — a blind 0 creates a mismatch plus a gear-ready under-
 * count that breaks worldmap entry; leaving it keeps reconcile quiet). */
static void psx_debug_overlay_party_commit(int c0, int c1, int c2)
{
    int c[3] = { c0, c1, c2 };
    psx_debug_overlay_party_unlock(c, 3, nullptr, nullptr);
    for (int i = 0; i < 3; i++) {
        uint32_t id = (c[i] == 0xFF) ? 0xFFu : (uint32_t)c[i];
        write_u32_le(kAddr_partyMirrorBase + (uint32_t)i * 4u, id);
        write_u32_le(kAddr_kernelPartySlots + (uint32_t)i * 4u, id);
    }
}

/* Full atomic formation write (panel + TCP party_set). Packs left like the
 * boot init and release compaction (holes poison engine lookups). */
int psx_debug_overlay_write_party_formation(int c0, int c1, int c2)
{
    int rc = psx_debug_overlay_party_guard(c0, c1, c2);
    if (rc != 0) return rc;
    int packed[3] = { 0xFF, 0xFF, 0xFF };
    int n = 0;
    int c[3] = { c0, c1, c2 };
    for (int i = 0; i < 3; i++) {
        if (c[i] != 0xFF) packed[n++] = c[i];
    }
    psx_debug_overlay_party_commit(packed[0], packed[1], packed[2]);
    return 0;
}

int psx_debug_overlay_write_party_slot(int slot, int charId, int bitfieldBit)
{
    if (slot < 0 || slot > 2) return -1;
    if (charId != 0xFF && (charId < 0 || charId > 10)) return -2;
    /* Single-slot granularity (TCP compat): guard module/busy/id only.
     * Cross-slot shape (duplicates/holes) is the atomic formation API's
     * job — the panel validates the whole formation before applying. */
    if (!field_module_active()) return 1;
    if (read_u32_le(kAddr_partyLoadState) != 0xFFu) return 2;
    if (read_u32_le(kAddr_menuRequest) != 0xFFu) return 2;
    if (read_u16_le(kAddr_fadeGate) != 0u) return 2;
    uint32_t id = (charId == 0xFF) ? 0xFFu : (uint32_t)charId;
    int form[3];
    for (int s = 0; s < 3; s++) {
        if (s == slot) form[s] = charId;
        else form[s] = (int)(read_u32_le(kAddr_kernelPartySlots + (uint32_t)s * 4u) & 0xFFu);
    }
    if (bitfieldBit >= 0 && bitfieldBit < 11) {
        uint16_t bf = read_u16_le(kAddr_partyBitfield);
        bf |= (uint16_t)(1u << bitfieldBit);
        write_u16_le(kAddr_partyBitfield, bf);
    }
    psx_debug_overlay_party_unlock(form, 3, nullptr, nullptr);
    write_u32_le(kAddr_partyMirrorBase + (uint32_t)slot * 4u, id);
    write_u32_le(kAddr_kernelPartySlots + (uint32_t)slot * 4u, id);
    return 0;
}

int psx_debug_overlay_write_party_bitfield(int bitfield)
{
    if (bitfield < 0 || bitfield > 0xFFFF) return -1;
    write_u16_le(kAddr_partyBitfield, (uint16_t)bitfield);
    return 0;
}

int psx_debug_overlay_write_gold(unsigned int gold)
{
    write_u32_le(kAddr_gold, (uint32_t)gold);
    return 0;
}

int psx_debug_overlay_write_var(int var, int value)
{
    if (var < 0 || var >= kFieldVarsCount) return -1;
    if (value < 0 || value > 0xFFFF) return -2;
    write_u16_le(kAddr_fieldVarsBase + (uint32_t)var * 2u, (uint16_t)value);
    /* Script handlers read the ACTIVE mirror (0x800C3A68), and the
     * persist step (0x800A30FC) copies active -> persistent, so a
     * persistent-only write is clobbered before a teleport's transition
     * consumes it (this silently dropped event_jump varWrites). Mirror
     * to active while field is loaded; skip it otherwise (that BSS
     * belongs to another module then). */
    if (field_module_active()) {
        write_u16_le(kAddr_activeFieldVarsBase + (uint32_t)var * 2u, (uint16_t)value);
    }
    return 0;
}

/* ---- W6: Force Battle / Free Camera / Event Jump helpers -------------- */

/* Force the next encounter-poll to fire a battle. Mechanism: write the
 * encounter-trigger gate (0x800B2298, reference-verified — the
 * reference's validation hook writes 0 here to disable encounters for
 * deterministic replay; non-zero enables the encounter countdown per
 * the reference's `playMusicAuthorized != 0` gate). The actual battle
 * firing still requires the field to have encounter data loaded AND
 * the countdown to reach zero; this function arms the gate but cannot
 * guarantee a battle on the next frame (the field-level encounter
 * timers are not in the reference address book).
 *
 * `value` is the gate value (0 = disable, non-zero = arm). Returns 0
 * on success, negative on bad `value`. The same write is exposed to
 * the TCP widget-action path. */
int psx_debug_overlay_force_battle(int value)
{
    if (value < 0 || value > 0xFFFF) return -1;
    overlay_capture_before_debug_write(
        kAddr_encounterTrigger, sizeof(uint32_t));
    write_u32_le(kAddr_encounterTrigger, (uint32_t)value);
    return 0;
}

/* Explicit battle start. Mirrors field opcode 0x71 StartBattle (0x80093568;
 * same six writes, same order, same readiness gates — FE 84 at 0x800933f8
 * writes the identical handoff): stage the 32-byte formation record into
 * the resident section-6 table slot, forward the staged config word,
 * write selected index + request flag, then commit the handoff triple
 * LAST (0x800ADBDC = 0, 0x800ADBE0 = 0, 0x800ADB88 = 1). The field
 * coordinator consumes it exactly like a scripted explicit battle
 * (ordinary transient snapshot + return to field; no post-battle
 * destination is staged — that needs FE 84's save-current call).
 *
 * `rec` layout (docs/xenogears/battle/02 §3): +0x00 enemy-set, +0x01
 * policy, +0x02 arena, +0x03 event idx, +0x04..06 party placement,
 * +0x07 pad, +0x08..0F enemy lanes (def 0-7 | 0x80 gear-scale, 0x7F
 * empty), +0x10..17 lane flags (bit 7 hidden), +0x18..1F positions.
 * The caller (panel/TCP) builds it by cloning the live template record
 * and overriding enemy-set/arena/policy/lanes — positions stay
 * map-tested (zeroed when the arena changes; the loader spreads stacked
 * occupants across successive coordinates).
 *
 * Returns 0 on arm (battle engages over the next frames), 1 = field
 * module not resident (section-6 table is field-resident), 2 = handoff
 * already armed (0x800ADB88 set — coordinator hasn't consumed yet),
 * 3 = engine busy or not ready (skin streaming / menu / fade / music /
 * anim / coordinator gates), -1 = bad index, -2 = bad record bytes
 * (enemy-set > 75, arena > 74, lane def > 7 and != 0x7F). */
int psx_debug_overlay_start_battle(int index, const uint8_t rec[32])
{
    if (index < 0 || index >= kBattleFormCount) return -1;
    if (!rec) return -2;
    if (rec[0x00] > 75 || rec[0x02] > 74) return -2;
    for (int i = 0; i < 8; i++) {
        uint8_t lane = rec[0x08 + i] & 0x7F;
        if (lane != 0x7F && lane > 7) return -2;
    }
    if (!field_module_active()) return 1;
    if (read_u32_le(kAddr_battleGate88) != 0u) return 2;
    if (read_u32_le(kAddr_partyLoadState) != 0xFFu) return 3;
    if (read_u32_le(kAddr_menuRequest) != 0xFFu) return 3;
    if (read_u16_le(kAddr_fadeGate) != 0u) return 3;
    if (read_u32_le(kAddr_battleGateDC) == 0u ||
        read_u32_le(kAddr_battleGateE4) == 0u ||
        read_u32_le(kAddr_teleportGate1) == 0u) return 3;
    if (read_u32_le(kAddr_teleportGateMusic) == 0xFFFFFFFFu) return 3;
    if (read_u32_le(kAddr_teleportGateAnim) != 0u) return 3;
    uint32_t base = kAddr_battleFormTable + (uint32_t)index * (uint32_t)kBattleFormSize;
    overlay_capture_before_debug_write(base, (size_t)kBattleFormSize);
    for (int i = 0; i < 32; i++) psx_write_byte(base + (uint32_t)i, rec[i]);
    /* Forward the staged config word unchanged (zero new information —
     * exactly what opcode 0x71 copies). */
    write_u32_le(kAddr_battleConfig, read_u32_le(kAddr_battleConfigSrc));
    psx_write_byte(kAddr_battleSelIndex, (uint8_t)index);
    psx_write_byte(kAddr_battleReqFlag, 0u);
    /* Commit LAST: the coordinator consumes the trio together. */
    write_u32_le(kAddr_battleGateDC, 0u);
    write_u32_le(kAddr_battleGateE0, 0u);
    write_u32_le(kAddr_battleGate88, 1u);
    return 0;
}

/* Roster byte/u16/u32 writers for battle staging (gear id, levels).
 * charId 0-10; the battle loader copies these records at startup. */
static int battle_roster_write_u8(int charId, uint32_t off, uint8_t val)
{
    if (charId < 0 || charId > 10) return -1;
    if (!field_module_active()) return 1;
    psx_write_byte(kAddr_partyRosterBase + (uint32_t)charId * (uint32_t)kPartyRosterStride + off, val);
    return 0;
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
int psx_debug_overlay_camera_write(int ex, int ey, int ez, int ax, int ay, int az)
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
    if (s_cam_saved_valid) {
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
    s_cam_frozen_mod = -1;
}

/* Event Jump: validate the teleport preconditions FIRST (dry-run, no
 * write), then apply the event's varWrites (via psx_write_byte to
 * fieldVars, same path as the W5 vars editor), then fire the teleport
 * recipe — or the boot-to-field request when outside field. `eventId`
 * is the index into the events table loaded from events.xml. Ordering
 * matters: a refused jump must leave RAM untouched, otherwise a failed
 * Jump silently corrupts GameProgress and poisons the NEXT jump.
 * Returns 0 on in-place arm, 4 on boot-to-field request, positive on
 * teleport-refused (1 = game not booted far enough, 2 = teleport
 * already in flight, 3 = engine busy), negative on bad id. */
int psx_debug_overlay_event_jump(int eventId)
{
    int ne = 0;
    const DbgEvent *evs = dbg_data_events(&ne);
    if (evs == nullptr || eventId < 0 || eventId >= ne) return -1;
    const DbgEvent &e = evs[eventId];
    bool boot = false;
    int rc = psx_debug_overlay_teleport_guard(e.mapId, e.entryPoint, &boot);
    if (rc != 0) {
        return rc;
    }
    /* Apply each varWrite via the W5 path (so the write goes through
     * the same clamp+error-report the manual vars editor uses). */
    for (int i = 0; i < e.numVarWrites; i++) {
        (void)psx_debug_overlay_write_var(e.varWrites[i].var,
                                          e.varWrites[i].value);
    }
    return psx_debug_overlay_teleport(e.mapId, e.entryPoint);
}

/* ---- Teleport panel UI ------------------------------------------------- */

static bool field_name_known(int id, const char **out_name)
{
    const char *n = dbg_data_field_name(id);
    if (n) { if (out_name) *out_name = n; return true; }
    if (out_name) *out_name = nullptr;
    return false;
}

static void draw_teleport_section(void)
{
    int mod = resident_loaded_module();
    int cur = psx_debug_overlay_read_field_id();
    const char *cur_name = nullptr;
    bool cur_known = (cur >= 0) && field_name_known(cur, &cur_name);
    ImGui::Text("Module: %s (overlay sig 0x%08X)",
                resident_module_name(mod), (unsigned)read_u32_le(kAddr_overlaySignature));
    ImGui::Text("Current field: %s%d%s%s",
                cur >= 0 ? "" : "?",
                cur >= 0 ? cur : 0,
                cur_known ? "  (" : "",
                cur_known ? cur_name : (cur >= 0 ? "  (unknown)" : ""));
    if (!field_module_active()) {
        ImGui::TextDisabled("Field NOT resident: Teleport will boot Field to the target");
        ImGui::TextDisabled("(yanks the current module, like the Kernel Menu transition).");
    } else {
        /* Live readiness gates so a refusal is explainable. */
        uint32_t party = read_u32_le(kAddr_partyLoadState);
        uint32_t menu  = read_u32_le(kAddr_menuRequest);
        uint16_t fade  = read_u16_le(kAddr_fadeGate);
        bool busy = (party != 0xFFu) || (menu != 0xFFu) || (fade != 0u);
        if (busy) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "Engine busy: party=0x%X menu=0x%X fade=%u - teleport will be refused.",
                (unsigned)party, (unsigned)menu, (unsigned)fade);
        } else {
            ImGui::TextDisabled("Field module active - engine idle, teleport ready.");
        }
        if (s_teleport_target_id >= 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                "Teleport in flight -> field %d ...", s_teleport_target_id);
        }
    }

    if (s_last_teleport_id >= 0 && s_teleport_status_frames > 0) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_teleport_status);
        s_teleport_status_frames--;
    } else if (s_teleport_status_frames == 0 && s_teleport_status[0]) {
        s_teleport_status[0] = '\0';
    }

    ImGui::Separator();

    ImGui::InputInt("Target field id", &s_teleport_field_id, 1, 10);
    if (s_teleport_field_id < 0)   s_teleport_field_id = 0;
    if (s_teleport_field_id > kMaxTeleportFieldId) s_teleport_field_id = kMaxTeleportFieldId;
    ImGui::InputInt("Entry point", &s_teleport_entry, 1, 10);
    if (s_teleport_entry < 0) s_teleport_entry = 0;
    if (s_teleport_entry > kMaxTeleportEntryPoint) s_teleport_entry = kMaxTeleportEntryPoint;
    ImGui::TextDisabled("Entry selects the map's arrival record (pos/layer/camera).");
    ImGui::TextDisabled("Out-of-range entries read bytecode garbage -> black screen. Use 0 unless sure.");

    {
        const char *name = nullptr;
        if (field_name_known(s_teleport_field_id, &name)) {
            ImGui::Text("Target name: %s", name);
        } else {
            ImGui::TextDisabled("Target name: (unknown id)");
        }
    }

    bool can_tp = (s_teleport_target_id < 0);
    if (!can_tp) ImGui::BeginDisabled();
    if (ImGui::Button(field_module_active() ? "Teleport" : "Boot to field")) {
        int rc = psx_debug_overlay_teleport(s_teleport_field_id, s_teleport_entry);
        if (rc == 0) {
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport armed -> field %d, entry %d (poll engages next frame)",
                s_teleport_field_id, s_teleport_entry);
            s_teleport_status_frames = 90; /* ~1.5s at 60fps */
        } else if (rc == 4) {
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Field boot requested -> field %d, entry %d (watch Current field)",
                s_teleport_field_id, s_teleport_entry);
            s_teleport_status_frames = 240;
        } else {
            const char *why = "unknown";
            if (rc == 1)       why = "game not booted far enough (no game state yet)";
            else if (rc == 2)  why = "teleport already in flight";
            else if (rc == 3)  why = "engine busy (party/menu/fade)";
            else if (rc == -1) why = "bad field id (0-729)";
            else if (rc == -2) why = "bad entry (0-255)";
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport refused (rc=%d): %s", rc, why);
            s_teleport_status_frames = 90;
        }
    }
    if (!can_tp) ImGui::EndDisabled();
    if (!can_tp) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Disabled: a teleport is already in flight.");
        }
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Title (490)")) {
        s_teleport_field_id = 490;
        s_teleport_entry    = 0;
        int rc = psx_debug_overlay_teleport(490, 0);
        std::snprintf(s_teleport_status, sizeof(s_teleport_status),
            "Teleport -> field 490 title (rc=%d)", rc);
        s_teleport_status_frames = 90;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Debug room (0)")) {
        s_teleport_field_id = 0;
        s_teleport_entry    = 0;
        int rc = psx_debug_overlay_teleport(0, 0);
        std::snprintf(s_teleport_status, sizeof(s_teleport_status),
            "Teleport -> field 0 debug (rc=%d)", rc);
        s_teleport_status_frames = 90;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Lahan (1)")) {
        s_teleport_field_id = 1;
        s_teleport_entry    = 0;
        int rc = psx_debug_overlay_teleport(1, 0);
        std::snprintf(s_teleport_status, sizeof(s_teleport_status),
            "Teleport -> field 1 Lahan (rc=%d)", rc);
        s_teleport_status_frames = 90;
    }

    ImGui::Separator();
    ImGui::Text("Fields (filter + double-click to set target):");
    ImGui::InputText("Filter##tp", s_teleport_filter, sizeof(s_teleport_filter));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Substring match on field name or id (hex ok).");

    int nf = 0;
    const DbgField *fields = dbg_data_fields(&nf);
    if (nf == 0) {
        ImGui::TextDisabled("(no fields.xml loaded)");
    } else {
        if (ImGui::BeginChild("tp_list", ImVec2(0, 220), true)) {
            for (int i = 0; i < nf; i++) {
                bool show = s_teleport_filter[0] == '\0';
                if (!show) {
                    char hay[128];
                    std::snprintf(hay, sizeof(hay), "%d %s",
                                  fields[i].id, fields[i].name ? fields[i].name : "");
                    int parsed = 0;
                    show = (std::strstr(hay, s_teleport_filter) != nullptr) ||
                           (std::sscanf(s_teleport_filter, "%d", &parsed) == 1 &&
                            parsed == fields[i].id);
                }
                if (!show) continue;
                char lbl[160];
                std::snprintf(lbl, sizeof(lbl), "%4d  %s##f%d",
                              fields[i].id,
                              fields[i].name ? fields[i].name : "",
                              i);
                if (ImGui::Selectable(lbl, s_teleport_field_id == fields[i].id,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    s_teleport_field_id = fields[i].id;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        int rc = psx_debug_overlay_teleport(fields[i].id, s_teleport_entry);
                        std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                            "Teleport -> %d %s (rc=%d)",
                            fields[i].id, fields[i].name ? fields[i].name : "", rc);
                        s_teleport_status_frames = 90;
                    }
                }
            }
        }
        ImGui::EndChild();
    }

}

/* ---- Party editor UI --------------------------------------------------- */

static void draw_party_section(void)
{
    int nc = 0;
    const DbgCharacter *chars = dbg_data_characters(&nc);

    /* Live engine state: module + busy gates (same triple as teleport —
     * skin streaming, staged menu and fade all make slot writes unsafe). */
    {
        uint32_t party = read_u32_le(kAddr_partyLoadState);
        uint32_t menu  = read_u32_le(kAddr_menuRequest);
        uint16_t fade  = read_u16_le(kAddr_fadeGate);
        bool busy = (party != 0xFFu) || (menu != 0xFFu) || (fade != 0u);
        ImGui::Text("Module: %s", resident_module_name(resident_loaded_module()));
        if (!field_module_active()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "Field NOT resident — party writes refused outside field.");
        } else if (busy) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "Engine busy: party=0x%X menu=0x%X fade=%u — writes refused.",
                (unsigned)party, (unsigned)menu, (unsigned)fade);
        } else {
            ImGui::TextDisabled("Field idle — party writes accepted.");
        }
    }
    uint16_t live_bf = read_u16_le(kAddr_partyBitfield);
    uint16_t live_fm = read_u16_le(kAddr_partyFrameMask);
    uint16_t live_ex = read_u16_le(kAddr_partyExclusion);
    ImGui::Text("Masks live: unlock=0x%04X frame=0x%04X AND=0x%04X excl=0x%04X",
                (unsigned)live_bf, (unsigned)live_fm,
                (unsigned)(live_bf & live_fm), (unsigned)live_ex);

    if (s_party_status_frames > 0 && s_party_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_party_status);
        s_party_status_frames--;
    }

    ImGui::Separator();
    ImGui::Text("Party (3 slots, 0xFF = empty):");
    ImGui::SameLine();
    if (ImGui::SmallButton("Read current##party")) {
        for (int s = 0; s < 3; s++) {
            s_party_slot[s] = (int)(read_u32_le(kAddr_kernelPartySlots +
                                                (uint32_t)s * 4u) & 0xFFu);
        }
    }
    for (int s = 0; s < 3; s++) {
        ImGui::PushID(s);
        char lbl[64];
        char preview[64];
        if (s_party_slot[s] == 0xFF) {
            std::snprintf(preview, sizeof(preview), "0xFF (empty)");
        } else {
            const char *nm = "?";
            for (int i = 0; i < nc; i++) {
                if (chars[i].id == s_party_slot[s]) {
                    nm = chars[i].name ? chars[i].name : "?";
                    break;
                }
            }
            std::snprintf(preview, sizeof(preview), "%s", nm);
        }
        std::snprintf(lbl, sizeof(lbl), "Slot %d##party", s);
        if (ImGui::BeginCombo(lbl, preview)) {
            if (ImGui::Selectable("0xFF (empty)", s_party_slot[s] == 0xFF)) {
                s_party_slot[s] = 0xFF;
            }
            for (int i = 0; i < nc; i++) {
                bool sel = (s_party_slot[s] == chars[i].id);
                char item[64];
                std::snprintf(item, sizeof(item), "%d  %s",
                              chars[i].id,
                              chars[i].name ? chars[i].name : "");
                if (ImGui::Selectable(item, sel)) {
                    s_party_slot[s] = chars[i].id;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    /* Live validation of the pending formation against the engine's own
     * rules (validator FUN_8008a790 + menu docs/xenogears/menu/08):
     * no duplicates, no holes (packed left on write), >=1 member,
     * availability notes, exclusion + empty-record warnings. */
    {
        bool empty = true;
        for (int s = 0; s < 3; s++) {
            if (s_party_slot[s] == 0xFF) continue;
            empty = false;
            for (int t = s + 1; t < 3; t++) {
                if (s_party_slot[s] == s_party_slot[t]) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                        "Duplicate member %d (slots %d+%d) — engine assumes unique.",
                        s_party_slot[s], s, t);
                }
            }
            uint16_t avail = (uint16_t)(live_bf & live_fm);
            if (s_party_slot[s] < 11 && !(avail & (1u << s_party_slot[s]))) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                    "Slot %d: char %d locked — write auto-unlocks both masks.",
                    s, s_party_slot[s]);
            }
            if (s_party_slot[s] < 16 && (live_ex & (1u << s_party_slot[s]))) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                    "Slot %d: char %d is story-excluded (+0x2318) — risky.",
                    s, s_party_slot[s]);
            }
            if (s_party_slot[s] < kPartyRosterCount) {
                uint32_t base = kAddr_partyRosterBase +
                                (uint32_t)s_party_slot[s] * (uint32_t)kPartyRosterStride;
                bool zero = true;
                for (int b = 0; b < 16; b++) {
                    if (psx_read_byte(base + (uint32_t)b) != 0) { zero = false; break; }
                }
                if (zero) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                        "Slot %d: char %d roster record empty — battle risk.",
                        s, s_party_slot[s]);
                }
            }
        }
        if (empty) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Empty formation — the menu contract requires >= 1 member.");
        }
        bool holes = (s_party_slot[0] == 0xFF && (s_party_slot[1] != 0xFF || s_party_slot[2] != 0xFF)) ||
                     (s_party_slot[1] == 0xFF && s_party_slot[2] != 0xFF);
        if (holes && !empty) {
            ImGui::TextDisabled("Holes pack left on write (engine lookups poison on 0xFF).");
        }
    }

    if (ImGui::Button("Write party to RAM (atomic)")) {
        int rc = psx_debug_overlay_write_party_formation(
            s_party_slot[0], s_party_slot[1], s_party_slot[2]);
        if (rc == 0) {
            int packed[3] = { 0xFF, 0xFF, 0xFF };
            int n = 0;
            for (int s = 0; s < 3; s++) {
                if (s_party_slot[s] != 0xFF) packed[n++] = s_party_slot[s];
            }
            for (int s = 0; s < 3; s++) s_party_slot[s] = packed[s];
            bool full = read_u16_le(kAddr_partyBitfield) == 0x07FFu;
            std::snprintf(s_party_status, sizeof(s_party_status),
                "Party written [%d,%d,%d]%s. Full effect on field change.",
                packed[0], packed[1], packed[2],
                full ? " + full unlock (branch uniformity)" : "");
        } else {
            const char *why = "unknown";
            if (rc == 1)       why = "field module not resident";
            else if (rc == 2)  why = "engine busy (party/menu/fade)";
            else if (rc == -1) why = "bad id (0-10 or 0xFF)";
            else if (rc == -2) why = "empty formation";
            else if (rc == -3) why = "duplicate member";
            std::snprintf(s_party_status, sizeof(s_party_status),
                "Party refused (rc=%d): %s", rc, why);
        }
        s_party_status_frames = 120;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Validates the whole formation, then writes bitfields "
                          "(OR-only) + mirrors + 3 kernel slots at 0x80062590; "
                          "gameState 0x8006F368 follows next frame. New members "
                          "load fully on the next field change.");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Levels & Stats")) {
    ImGui::Text("Levels (roster +0x62/+0x63, 1-99; stats/EXP untouched):");
    for (int s = 0; s < 3; s++) {
        ImGui::PushID(500 + s);
        int ch = s_party_slot[s];
        if (ch < 0 || ch > 10) {
            ImGui::TextDisabled("Slot %d: --", s);
        } else {
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            uint8_t live_p = psx_read_byte(base + 0x62u);
            uint8_t live_e = psx_read_byte(base + 0x63u);
            ImGui::Text("Slot %d (live P%u/E%u):", s, (unsigned)live_p, (unsigned)live_e);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(46);
            ImGui::InputInt("Phys##plp", &s_party_level_p[s], 0, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(46);
            ImGui::InputInt("Ether##ple", &s_party_level_e[s], 0, 0);
            if (s_party_level_p[s] < 1) s_party_level_p[s] = 1;
            if (s_party_level_p[s] > 99) s_party_level_p[s] = 99;
            if (s_party_level_e[s] < 1) s_party_level_e[s] = 1;
            if (s_party_level_e[s] > 99) s_party_level_e[s] = 99;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Read live levels")) {
        for (int s = 0; s < 3; s++) {
            int ch = s_party_slot[s];
            if (ch < 0 || ch > 10) continue;
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            int lp = (int)psx_read_byte(base + 0x62u);
            int le = (int)psx_read_byte(base + 0x63u);
            s_party_level_p[s] = (lp >= 1 && lp <= 99) ? lp : 1;
            s_party_level_e[s] = (le >= 1 && le <= 99) ? le : 1;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Write levels")) {
        for (int s = 0; s < 3; s++) {
            int ch = s_party_slot[s];
            if (ch < 0 || ch > 10) continue;
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            int lp = s_party_level_p[s], le = s_party_level_e[s];
            if (lp < 1) { lp = 1; } if (lp > 99) { lp = 99; }
            if (le < 1) { le = 1; } if (le > 99) { le = 99; }
            psx_write_byte(base + 0x62u, (uint8_t)lp);
            psx_write_byte(base + 0x63u, (uint8_t)le);
        }
        std::snprintf(s_party_status, sizeof(s_party_status),
            "Levels written to roster records.");
        s_party_status_frames = 90;
    }
    ImGui::TextDisabled("Levels are the number only: stats/EXP below drive behavior.");

    ImGui::Separator();
    ImGui::Text("Stats (roster-direct, engine caps; battle copies them verbatim):");
    for (int s = 0; s < 3; s++) {
        ImGui::PushID(600 + s);
        int ch = s_party_slot[s];
        if (ch < 0 || ch > 10) {
            ImGui::TextDisabled("Slot %d: --", s);
        } else {
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            uint16_t lhp = read_u16_le(base + 0x4Cu);
            uint16_t lmhp = read_u16_le(base + 0x4Eu);
            ImGui::Text("Slot %d (HP %u/%u):", s, (unsigned)lhp, (unsigned)lmhp);
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("HP##shp", &s_party_hp[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60);
            ImGui::InputInt("max##smhp", &s_party_mhp[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(52);
            ImGui::InputInt("MP##smp", &s_party_mp[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(52);
            ImGui::InputInt("max##smmp", &s_party_mmp[s], 0, 0);
            ImGui::SetNextItemWidth(46);
            ImGui::InputInt("atk##sat", &s_party_atk[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(46);
            ImGui::InputInt("def##sdf", &s_party_def[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(46);
            ImGui::InputInt("agi##sag", &s_party_agi[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(46);
            ImGui::InputInt("eth##set", &s_party_eth[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(46);
            ImGui::InputInt("efd##sef", &s_party_efd[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(46);
            ImGui::InputInt("hit##shi", &s_party_hit[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(46);
            ImGui::InputInt("eva##sev", &s_party_eva[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(70);
            ImGui::InputInt("EXPp##sxp", &s_party_expr[s], 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(70);
            ImGui::InputInt("EXPe##sxe", &s_party_expe[s], 0, 0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("EXP remaining to next level (roster +0x44/+0x48). "
                                  "Set 0/1 to prime authentic level-ups (stats + "
                                  "unlock checks) on the next battle result.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Prime##sxp")) {
                s_party_expr[s] = 0;
                s_party_expe[s] = 0;
                int ch = s_party_slot[s];
                if (ch >= 0 && ch <= 10) {
                    uint32_t base = kAddr_partyRosterBase +
                                    (uint32_t)ch * (uint32_t)kPartyRosterStride;
                    write_u32_le(base + 0x44u, 0u);
                    write_u32_le(base + 0x48u, 0u);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Zero both tracks right now: the next battle result runs "
                                  "the real level-up loop (docs/battle/08).");
            }
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Read live stats")) {
        for (int s = 0; s < 3; s++) {
            int ch = s_party_slot[s];
            if (ch < 0 || ch > 10) continue;
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            s_party_hp[s]   = (int)read_u16_le(base + 0x4Cu);
            s_party_mhp[s]  = (int)read_u16_le(base + 0x4Eu);
            s_party_mp[s]   = (int)read_u16_le(base + 0x50u);
            s_party_mmp[s]  = (int)read_u16_le(base + 0x52u);
            s_party_atk[s]  = (int)psx_read_byte(base + 0x58u);
            s_party_def[s]  = (int)psx_read_byte(base + 0x59u);
            s_party_agi[s]  = (int)psx_read_byte(base + 0x5Au);
            s_party_eth[s]  = (int)psx_read_byte(base + 0x5Bu);
            s_party_efd[s]  = (int)psx_read_byte(base + 0x5Cu);
            s_party_hit[s]  = (int)psx_read_byte(base + 0x5Eu);
            s_party_eva[s]  = (int)psx_read_byte(base + 0x5Fu);
            s_party_expr[s] = (int)read_u32_le(base + 0x44u);
            s_party_expe[s] = (int)read_u32_le(base + 0x48u);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Max stats")) {
        for (int s = 0; s < 3; s++) {
            s_party_mhp[s] = 999; s_party_hp[s] = 999;
            s_party_mmp[s] = 99; s_party_mp[s] = 99;
            s_party_atk[s] = 200; s_party_def[s] = 200;
            s_party_agi[s] = 99;
            s_party_eth[s] = 200; s_party_efd[s] = 200;
            s_party_hit[s] = 99; s_party_eva[s] = 99;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Prime all")) {
        for (int s = 0; s < 3; s++) {
            int ch = s_party_slot[s];
            if (ch < 0 || ch > 10) continue;
            s_party_expr[s] = 0;
            s_party_expe[s] = 0;
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            write_u32_le(base + 0x44u, 0u);
            write_u32_le(base + 0x48u, 0u);
        }
        std::snprintf(s_party_status, sizeof(s_party_status),
            "EXP primed: next battle result levels up.");
        s_party_status_frames = 90;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Write stats")) {
        for (int s = 0; s < 3; s++) {
            int ch = s_party_slot[s];
            if (ch < 0 || ch > 10) continue;
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)ch * (uint32_t)kPartyRosterStride;
            int hp = s_party_hp[s], mhp = s_party_mhp[s];
            int mp = s_party_mp[s], mmp = s_party_mmp[s];
            if (hp < 0) { hp = 0; } if (hp > 999) { hp = 999; }
            if (mhp < 1) { mhp = 1; } if (mhp > 999) { mhp = 999; }
            if (mp < 0) { mp = 0; } if (mp > 99) { mp = 99; }
            if (mmp < 1) { mmp = 1; } if (mmp > 99) { mmp = 99; }
            if (hp > mhp) { hp = mhp; }
            if (mp > mmp) { mp = mmp; }
            write_u16_le(base + 0x4Cu, (uint16_t)hp);
            write_u16_le(base + 0x4Eu, (uint16_t)mhp);
            write_u16_le(base + 0x50u, (uint16_t)mp);
            write_u16_le(base + 0x52u, (uint16_t)mmp);
            int at[7] = { s_party_atk[s], s_party_def[s], s_party_agi[s],
                          s_party_eth[s], s_party_efd[s], s_party_hit[s],
                          s_party_eva[s] };
            int cap[7] = { 200, 200, 99, 200, 200, 99, 99 };
            /* +0x58..0x5C attributes, +0x5E/+0x5F hit/evade (+0x5D gap). */
            uint32_t off[7] = { 0x58u, 0x59u, 0x5Au, 0x5Bu, 0x5Cu, 0x5Eu, 0x5Fu };
            for (int i = 0; i < 7; i++) {
                if (at[i] < 0) at[i] = 0;
                if (at[i] > cap[i]) at[i] = cap[i];
                psx_write_byte(base + off[i], (uint8_t)at[i]);
            }
            uint32_t er = s_party_expr[s] < 0 ? 0u : (uint32_t)s_party_expr[s];
            uint32_t ee = s_party_expe[s] < 0 ? 0u : (uint32_t)s_party_expe[s];
            write_u32_le(base + 0x44u, er);
            write_u32_le(base + 0x48u, ee);
        }
        std::snprintf(s_party_status, sizeof(s_party_status),
            "Stats written to roster records.");
        s_party_status_frames = 90;
    }
    }

    ImGui::Separator();
    ImGui::Text("Unlock bitfield (0x8006F364, 11 bits; frame mask 0x8006F366 live=0x%04X):",
                (unsigned)read_u16_le(kAddr_partyFrameMask));
    ImGui::TextDisabled("Slot writes auto-OR both masks (never clear). This editor writes 0x8006F364 only.");
    if (ImGui::SmallButton("Set 0x07FF (all 11)")) s_party_bitfield = 0x07FF;
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) s_party_bitfield = 0;
    ImGui::SameLine();
    if (ImGui::SmallButton("Read current")) {
        s_party_bitfield = (int)read_u16_le(kAddr_partyBitfield);
    }
    if (ImGui::InputInt("Bitfield (hex/decimal)", &s_party_bitfield, 1, 16,
                       ImGuiInputTextFlags_CharsHexadecimal |
                       ImGuiInputTextFlags_CharsDecimal)) {
        if (s_party_bitfield < 0) s_party_bitfield = 0;
        if (s_party_bitfield > 0xFFFF) s_party_bitfield = 0xFFFF;
    }
    if (ImGui::Button("Write bitfield to RAM")) {
        psx_debug_overlay_write_party_bitfield(s_party_bitfield);
    }
    ImGui::SameLine();
    if (ImGui::Button("Write 11 checkboxes")) {
        uint16_t mask = 0;
        for (int b = 0; b < 11; b++)
            if (s_party_unlock[b]) mask |= (uint16_t)(1u << b);
        psx_debug_overlay_write_party_bitfield(mask);
    }
    /* 11 unlock checkboxes; named from characters.xml. */
    for (int b = 0; b < 11; b++) {
        const char *nm = "?";
        char synthetic[16];
        if (b < nc && chars[b].name) {
            nm = chars[b].name;
        } else {
            std::snprintf(synthetic, sizeof(synthetic), "char %d", b);
            nm = synthetic;
        }
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "bit %2d  %s##u%d", b, nm, b);
        ImGui::Checkbox(lbl, &s_party_unlock[b]);
        if ((b % 3) != 2 && b != 10) ImGui::SameLine();
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Character Records (0x8006D8A0, 0xA4 stride)")) {
        if (ImGui::BeginTable("roster", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("unlocked");
            ImGui::TableSetupColumn("gear");
            ImGui::TableSetupColumn("first 16 bytes (hex)");
            ImGui::TableHeadersRow();
            uint16_t bf = read_u16_le(kAddr_partyBitfield);
            for (int i = 0; i < kPartyRosterCount; i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i);
                ImGui::TableSetColumnIndex(1);
                const char *nm = "?";
                if (i < nc && chars[i].name) nm = chars[i].name;
                ImGui::Text("%s", nm);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", (bf & (1u << i)) ? "yes" : "no");
                ImGui::TableSetColumnIndex(3);
                /* Associated Gear id (roster+0xA0, 0xFF = no Gear): world
                 * entry allocates per-member Gear buffers from this byte. */
                uint8_t gear = psx_read_byte(kAddr_partyRosterBase +
                                             (uint32_t)i * (uint32_t)kPartyRosterStride + 0xA0u);
                if (gear == 0xFF) {
                    ImGui::TextDisabled("FF(none)");
                } else {
                    ImGui::Text("0x%02X", (unsigned)gear);
                }
                ImGui::TableSetColumnIndex(4);
                char hex[64] = {0};
                int pos = 0;
                bool zero = true;
                uint32_t base = kAddr_partyRosterBase + (uint32_t)i * (uint32_t)kPartyRosterStride;
                for (int b = 0; b < 16; b++) {
                    uint8_t v = psx_read_byte(base + (uint32_t)b);
                    if (v != 0) zero = false;
                    hex[pos++] = hex_nibble(v >> 4);
                    hex[pos++] = hex_nibble(v & 0xF);
                }
                hex[pos] = '\0';
                if (zero) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                        "%s (empty!)", hex);
                } else {
                    ImGui::Text("%s", hex);
                }
            }
            ImGui::EndTable();
        }
    }
}

/* ---- Battle selector panel (W6, rewritten) ------------------------------ */

/* Pull the whole editor from live RAM (current table[15] set/arena/lanes
 * as safe defaults + party + roster gear/levels). A blank record falls
 * back to lane 0 = def 0. */
static void battle_pull_all(void)
{
    uint32_t tbase = kAddr_battleFormTable + 15u * (uint32_t)kBattleFormSize;
    int set = (int)psx_read_byte(tbase + 0);
    int arena = (int)psx_read_byte(tbase + 2);
    s_battle_enemy_set = (set >= 0 && set <= kBattleEnemySetMax) ? set : 0;
    s_battle_arena = (arena >= 0 && arena < kBattleArenaCount) ? arena : 0;
    bool any = false;
    for (int i = 0; i < 8; i++) {
        uint8_t lane = psx_read_byte(tbase + 0x08u + (uint32_t)i);
        if (lane == 0x7Fu) {
            s_battle_lane_def[i] = 0xFF;
            s_battle_lane_gear[i] = false;
        } else {
            s_battle_lane_def[i] = lane & 0x07u;
            s_battle_lane_gear[i] = (lane & 0x80u) != 0u;
            any = true;
        }
    }
    if (!any) s_battle_lane_def[0] = 0;
    for (int s = 0; s < 3; s++) {
        int ch = (int)(read_u32_le(kAddr_kernelPartySlots + (uint32_t)s * 4u) & 0xFFu);
        s_battle_party[s] = (ch >= 0 && ch < 11) ? ch : 0xFF;
        s_battle_mounted[s] = psx_read_byte(kAddr_partyMountBase + (uint32_t)s) != 0u;
        if (s_battle_party[s] == 0xFF) {
            s_battle_gear[s] = 0xFF;
            s_battle_level_p[s] = 1;
            s_battle_level_e[s] = 1;
        } else {
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)s_battle_party[s] * (uint32_t)kPartyRosterStride;
            s_battle_gear[s] = (int)psx_read_byte(base + 0xA0u);
            int lp = (int)psx_read_byte(base + 0x62u);
            int le = (int)psx_read_byte(base + 0x63u);
            s_battle_level_p[s] = (lp >= 1 && lp <= 99) ? lp : 1;
            s_battle_level_e[s] = (le >= 1 && le <= 99) ? le : 1;
        }
    }
}

/* Enemy sets whose visuals are Gear-scale ([GEAR] in the disc-1 census,
 * MIX sets excluded — lane's call there). Picking from one pre-checks
 * Gear-scale on the filled lane. */
static bool battle_set_is_gear(int set)
{
    static const int kGearSets[] = { 2, 9, 13, 14, 15, 18, 19, 20, 21, 22,
        26, 27, 29, 31, 34, 35, 36, 37, 39, 42, 43, 46, 48, 52, 53, 54, 55,
        58, 59, 60, 61, 62, 63, 64, 65, 66, 68, 70, 71, 72, 73, 74, 75 };
    for (size_t i = 0; i < sizeof(kGearSets) / sizeof(kGearSets[0]); i++) {
        if (kGearSets[i] == set) return true;
    }
    return false;
}

/* Lane def name within a set (global list lookup). */
static const char *battle_lane_name(int set, int def)
{
    if (def < 0 || def > 7) return "0xFF (empty)";
    for (int i = 0; i < kBattleEnemyCount; i++) {
        if (kBattleEnemies[i].set == set && kBattleEnemies[i].def == def)
            return kBattleEnemies[i].name;
    }
    return "?";
}

/* Shared apply path (panel button + TCP start_battle): stage roster
 * gear/levels, party formation (safe API), formation record, then the
 * opcode-71 handoff. Reports into s_battle_status. Returns the
 * start_battle rc. */
static int battle_apply(void)
{
    /* 1. Roster gear + levels per battle-party slot. */
    for (int s = 0; s < 3; s++) {
        int ch = s_battle_party[s];
        if (ch < 0 || ch > 10) continue;
        uint32_t base = kAddr_partyRosterBase + (uint32_t)ch * (uint32_t)kPartyRosterStride;
        int gear = s_battle_gear[s];
        if (gear >= 0 && gear < 20) {
            psx_write_byte(base + 0xA0u, (uint8_t)gear);
        } else if (gear == 0xFF) {
            psx_write_byte(base + 0xA0u, 0xFFu);
        }
        int lp = s_battle_level_p[s], le = s_battle_level_e[s];
        if (lp < 1) { lp = 1; } if (lp > 99) { lp = 99; }
        if (le < 1) { le = 1; } if (le > 99) { le = 99; }
        psx_write_byte(base + 0x62u, (uint8_t)lp);
        psx_write_byte(base + 0x63u, (uint8_t)le);
        /* Mount state for the battle loader's Gear placement path
         * (docs/xenogears/field/07 §10). Field actors untouched. */
        psx_write_byte(kAddr_partyMountBase + (uint32_t)s,
                       s_battle_mounted[s] ? 1u : 0u);
    }
    /* 2. Party formation (validates: dups/empty/module/busy). */
    int rc = psx_debug_overlay_write_party_formation(
        s_battle_party[0], s_battle_party[1], s_battle_party[2]);
    if (rc != 0) {
        const char *why = "unknown";
        if (rc == 1)       why = "field module not resident";
        else if (rc == 2)  why = "engine busy (party/menu/fade)";
        else if (rc == -1) why = "bad party id";
        else if (rc == -2) why = "empty party";
        else if (rc == -3) why = "duplicate member";
        std::snprintf(s_battle_status, sizeof(s_battle_status),
            "Battle staging stopped at party (rc=%d): %s", rc, why);
        s_battle_status_frames = 150;
        return rc;
    }
    /* 3. Formation record, fixed layout (docs/xenogears/battle/02 §3 +
     * community Enemy Encounter Setups doc, byte-identical): policy 0x40
     * (map-standard wild battle), party placement 0/1/2, zeroed lane
     * flags/positions (the loader spreads stacked occupants across
     * successive coordinates). Always written to slot 15. */
    uint8_t rec[32] = {0};
    rec[0x00] = (uint8_t)s_battle_enemy_set;
    rec[0x01] = 0x40u;
    rec[0x02] = (uint8_t)s_battle_arena;
    rec[0x03] = 0u;
    rec[0x04] = 0u; rec[0x05] = 1u; rec[0x06] = 2u;
    rec[0x07] = 0u;
    int lanes = 0;
    for (int i = 0; i < 8; i++) {
        int def = s_battle_lane_def[i];
        if (def < 0 || def > 7) {
            rec[0x08 + i] = 0x7Fu;
        } else {
            /* Bit 7 = Gear-scale realization (lane byte, docs/battle/02
             * §3). Gear bosses (Deus & co.) hang without it — the loader
             * realizes them down the wrong pipeline. */
            rec[0x08 + i] = (uint8_t)(((uint8_t)def & 0x07u) |
                                     (s_battle_lane_gear[i] ? 0x80u : 0u));
            lanes++;
        }
        rec[0x10 + i] = 0u;
        /* Distinct positions per lane (not all zero): stacked Gear-scale
         * bosses never settle their placement, and the entrance gate
         * waits for actors at terrain height forever (red hang). Tables
         * hold 8 lanes by design. */
        rec[0x18 + i] = (uint8_t)i;
    }
    if (lanes == 0) {
        std::snprintf(s_battle_status, sizeof(s_battle_status),
            "Battle refused: no enemies picked.");
        s_battle_status_frames = 150;
        return -2;
    }
    /* 4. Handoff (guards inside). */
    rc = psx_debug_overlay_start_battle(15, rec);
    if (rc == 0) {
        std::snprintf(s_battle_status, sizeof(s_battle_status),
            "Battle armed: %d lane(s), arena %d (watch transition)", lanes, s_battle_arena);
    } else {
        const char *why = "unknown";
        if (rc == 1)       why = "field module not resident";
        else if (rc == 2)  why = "handoff already armed";
        else if (rc == 3)  why = "engine busy/not ready";
        else if (rc == -1) why = "bad table index";
        else if (rc == -2) why = "bad record bytes";
        std::snprintf(s_battle_status, sizeof(s_battle_status),
            "Battle refused (rc=%d): %s", rc, why);
    }
    s_battle_status_frames = 150;
    return rc;
}

static void draw_battle_section(void)
{
    int nc = 0;
    const DbgCharacter *chars = dbg_data_characters(&nc);
    int ng = 0;
    const DbgCharacter *gears = dbg_data_gears(&ng);

    auto char_name = [&](int id) -> const char * {
        if (id == 0xFF) return "0xFF (empty)";
        for (int i = 0; i < nc; i++) {
            if (chars[i].id == id) return chars[i].name ? chars[i].name : "?";
        }
        return "?";
    };
    auto gear_name = [&](int id) -> const char * {
        if (id == 0xFF) return "0xFF (none)";
        for (int i = 0; i < ng; i++) {
            if (gears[i].id == id) return gears[i].name ? gears[i].name : "?";
        }
        return "?";
    };

    if (!s_battle_edit_init && field_module_active()) {
        s_battle_edit_init = true;
        battle_pull_all();
    }

    /* Readiness: module + opcode-71 gates + busy triple + armed state. */
    {
        uint32_t dc = read_u32_le(kAddr_battleGateDC);
        uint32_t e4 = read_u32_le(kAddr_battleGateE4);
        uint32_t ec = read_u32_le(kAddr_teleportGate1);
        uint32_t m2 = read_u32_le(kAddr_menuRequest);
        uint32_t mu = read_u32_le(kAddr_teleportGateMusic);
        uint32_t an = read_u32_le(kAddr_teleportGateAnim);
        uint32_t party = read_u32_le(kAddr_partyLoadState);
        uint16_t fade = read_u16_le(kAddr_fadeGate);
        uint32_t armed = read_u32_le(kAddr_battleGate88);
        ImGui::Text("Module: %s%s", resident_module_name(resident_loaded_module()),
                    armed ? "  ARMED (handoff pending)" : "");
        if (!field_module_active()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "Field NOT resident — explicit battles need the section-6 table.");
        } else if (armed) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                "Handoff armed — coordinator consumes over the next frames.");
        } else {
            bool ready = (dc != 0u && e4 != 0u && ec != 0u && m2 == 0xFFu &&
                          mu != 0xFFFFFFFFu && an == 0u && party == 0xFFu && fade == 0u);
            if (ready) ImGui::TextDisabled("Readiness gates clear — Start will arm.");
            else ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "Not ready: dc=%u e4=%u ec=%u menu=0x%X music=0x%X anim=%u party=0x%X fade=%u",
                (unsigned)(dc != 0u), (unsigned)(e4 != 0u), (unsigned)(ec != 0u),
                (unsigned)m2, (unsigned)mu, (unsigned)an, (unsigned)party, (unsigned)fade);
        }
    }

    /* Random-encounter gate (kept): 0 = randoms off, non-zero = on. */
    {
        uint32_t gate_now = read_u32_le(kAddr_encounterTrigger);
        ImGui::Text("Encounter gate (0x800B2298): 0x%08X", (unsigned)gate_now);
        ImGui::SameLine();
        if (ImGui::SmallButton("Randoms on")) {
            (void)psx_debug_overlay_force_battle(1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Randoms off")) {
            (void)psx_debug_overlay_force_battle(0);
        }
    }

    if (s_battle_status_frames > 0 && s_battle_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_battle_status);
        s_battle_status_frames--;
    }

    ImGui::Separator();
    ImGui::Text("Enemies (8 lanes; all lanes share one set by engine design):");
    ImGui::InputText("Search##bef", s_battle_enemy_filter, sizeof(s_battle_enemy_filter));
    {
        /* Lane chips: picked enemies + clear buttons. */
        for (int i = 0; i < 8; i++) {
            ImGui::PushID(300 + i);
            if (s_battle_lane_def[i] < 0 || s_battle_lane_def[i] > 7) {
                ImGui::TextDisabled("lane %d: --", i);
            } else {
                ImGui::Text("lane %d: %s%s", i,
                            battle_lane_name(s_battle_enemy_set, s_battle_lane_def[i]),
                            s_battle_lane_gear[i] ? " [Gear]" : "");
                ImGui::SameLine();
                if (ImGui::SmallButton("x##blc")) {
                    s_battle_lane_def[i] = 0xFF;
                    s_battle_lane_gear[i] = false;
                }
                ImGui::SameLine();
                ImGui::Checkbox("Gear##blg", &s_battle_lane_gear[i]);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Gear-scale realization (lane bit 7). "
                                      "Required for Gear bosses (Deus & co.) — "
                                      "without it the loader hangs.");
                }
            }
            ImGui::PopID();
        }
    }
    if (ImGui::BeginChild("enemy_list", ImVec2(0, 180), true)) {
        for (int i = 0; i < kBattleEnemyCount; i++) {
            const DbgBattleEnemy &e = kBattleEnemies[i];
            if (s_battle_enemy_filter[0] != '\0') {
                char hay[96];
                std::snprintf(hay, sizeof(hay), "%s %d", e.name, e.set);
                if (std::strstr(hay, s_battle_enemy_filter) == nullptr) continue;
            }
            char lbl[128];
            std::snprintf(lbl, sizeof(lbl), "%s (set %d)##be%d", e.name, e.set, i);
            if (ImGui::Selectable(lbl, false)) {
                /* Lanes share one enemy pair: switching sets clears lanes.
                 * [GEAR] sets pre-check Gear-scale on the filled lane. */
                if (e.set != s_battle_enemy_set) {
                    bool occupied = false;
                    for (int l = 0; l < 8; l++) {
                        if (s_battle_lane_def[l] >= 0 && s_battle_lane_def[l] <= 7) {
                            occupied = true;
                            break;
                        }
                    }
                    if (occupied) {
                        for (int l = 0; l < 8; l++) {
                            s_battle_lane_def[l] = 0xFF;
                            s_battle_lane_gear[l] = false;
                        }
                        std::snprintf(s_battle_status, sizeof(s_battle_status),
                            "Enemy set switched to %d — lanes cleared.", e.set);
                        s_battle_status_frames = 120;
                    }
                    s_battle_enemy_set = e.set;
                }
                for (int l = 0; l < 8; l++) {
                    if (s_battle_lane_def[l] < 0 || s_battle_lane_def[l] > 7) {
                        s_battle_lane_def[l] = e.def;
                        s_battle_lane_gear[l] = battle_set_is_gear(e.set);
                        break;
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Record: policy 0x40 (wild standard), placement 0/1/2, positions 0.");
    {
        bool dup = false;
        for (int i = 0; i < 8 && !dup; i++) {
            if (s_battle_lane_def[i] < 0 || s_battle_lane_def[i] > 7) continue;
            for (int j = i + 1; j < 8; j++) {
                if (s_battle_lane_def[i] == s_battle_lane_def[j]) { dup = true; break; }
            }
        }
        if (dup) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "Duplicated boss defs load but hang once turns start (shared "
                "phase state; verified: 2x Deus red-freezes, 1 active + 1 "
                "reserve fights fine). Mobs duplicate safely.");
        }
    }
    ImGui::TextDisabled("Scripted bosses (Deus & co.) need Gear-scale + their event;"
                        " raw lanes may hang at the red fade (kill the game, RAM-only).");
    {
        char preview[96];
        const char *an = "?";
        if (s_battle_arena >= 0 && s_battle_arena < kBattleArenaCount)
            an = kBattleArenas[s_battle_arena].name;
        std::snprintf(preview, sizeof(preview), "%d: %s", s_battle_arena, an);
        if (ImGui::BeginCombo("Arena", preview)) {
            for (int i = 0; i < kBattleArenaCount; i++) {
                char item[128];
                std::snprintf(item, sizeof(item), "%d: %s",
                              kBattleArenas[i].id, kBattleArenas[i].name);
                if (ImGui::Selectable(item, s_battle_arena == i)) {
                    s_battle_arena = i;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();
    ImGui::Text("Battle party (chars + gears + levels):");
    for (int s = 0; s < 3; s++) {
        ImGui::PushID(200 + s);
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "Slot %d", s);
        ImGui::Text("%s", lbl); ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        if (ImGui::BeginCombo("##bpchar", char_name(s_battle_party[s]))) {
            if (ImGui::Selectable("0xFF (empty)", s_battle_party[s] == 0xFF)) {
                s_battle_party[s] = 0xFF;
            }
            for (int i = 0; i < nc; i++) {
                char item[64];
                std::snprintf(item, sizeof(item), "%d  %s",
                              chars[i].id, chars[i].name ? chars[i].name : "");
                if (ImGui::Selectable(item, s_battle_party[s] == chars[i].id)) {
                    s_battle_party[s] = chars[i].id;
                    /* Pull that char's live gear/levels into the editor. */
                    uint32_t base = kAddr_partyRosterBase +
                                    (uint32_t)chars[i].id * (uint32_t)kPartyRosterStride;
                    s_battle_gear[s] = (int)psx_read_byte(base + 0xA0u);
                    s_battle_level_p[s] = (int)psx_read_byte(base + 0x62u);
                    s_battle_level_e[s] = (int)psx_read_byte(base + 0x63u);
                    if (s_battle_level_p[s] < 1) s_battle_level_p[s] = 1;
                    if (s_battle_level_e[s] < 1) s_battle_level_e[s] = 1;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##bpgear", gear_name(s_battle_gear[s]))) {
            if (ImGui::Selectable("0xFF (none)", s_battle_gear[s] == 0xFF)) {
                s_battle_gear[s] = 0xFF;
                s_battle_mounted[s] = false;
            }
            for (int i = 0; i < ng; i++) {
                char item[64];
                std::snprintf(item, sizeof(item), "%d  %s",
                              gears[i].id, gears[i].name ? gears[i].name : "");
                if (ImGui::Selectable(item, s_battle_gear[s] == gears[i].id)) {
                    s_battle_gear[s] = gears[i].id;
                    /* A real gear implies fighting mounted. */
                    s_battle_mounted[s] = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(46);
        ImGui::InputInt("Phys##blp", &s_battle_level_p[s], 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(46);
        ImGui::InputInt("Ether##ble", &s_battle_level_e[s], 0, 0);
        if (s_battle_level_p[s] < 1) s_battle_level_p[s] = 1;
        if (s_battle_level_p[s] > 99) s_battle_level_p[s] = 99;
        if (s_battle_level_e[s] < 1) s_battle_level_e[s] = 1;
        if (s_battle_level_e[s] > 99) s_battle_level_e[s] = 99;
        /* Live HP readout for the chosen char (display only). */
        if (s_battle_party[s] >= 0 && s_battle_party[s] < 11) {
            uint32_t base = kAddr_partyRosterBase +
                            (uint32_t)s_battle_party[s] * (uint32_t)kPartyRosterStride;
            uint16_t hp = read_u16_le(base + 0x4Cu);
            uint16_t mhp = read_u16_le(base + 0x4Eu);
            uint8_t lv = psx_read_byte(base + 0x62u);
            ImGui::SameLine();
            ImGui::TextDisabled("HP %u/%u Lv%u", (unsigned)hp, (unsigned)mhp, (unsigned)lv);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Mounted##bm", &s_battle_mounted[s]);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fight IN the Gear (mount byte gameState+0x22B1). "
                              "Needs a gear assigned above; field actors stay as-is.");
        }
        if (s_battle_mounted[s] && s_battle_gear[s] == 0xFF) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "(no gear!)");
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Copy from Party panel")) {
        for (int s = 0; s < 3; s++) s_battle_party[s] = s_party_slot[s];
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Read live party")) {
        for (int s = 0; s < 3; s++) {
            s_battle_party[s] = (int)(read_u32_le(kAddr_kernelPartySlots +
                                                  (uint32_t)s * 4u) & 0xFFu);
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Start battle (explicit handoff)")) {
        (void)battle_apply();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stages roster gear/levels, party formation, the "
                          "formation record, then the opcode-71 handoff. The "
                          "coordinator consumes it like a scripted battle "
                          "(snapshot + return to field).");
    }
    if (s_battle_status_frames > 0 && s_battle_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_battle_status);
        s_battle_status_frames--;
    }
    ImGui::TextDisabled(
        "Positions follow the template record (zeroed on arena change; the "
        "loader spreads stacked occupants). Event bit (0x20) needs a valid "
        "event index. Field only; worldmap has its own encounter route.");

    ImGui::Separator();
    ImGui::Text("Battle entities (0x800CCCE8, 11 x 0x170 bytes):");
    if (ImGui::BeginTable("battle_entities", 4,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("slot");
        ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("status");
        ImGui::TableSetupColumn("first 16 bytes (hex)");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kBattleEntityCount; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            uint32_t base = kAddr_battleEntities +
                            (uint32_t)i * (uint32_t)kBattleEntityStride;
            uint8_t b_id = psx_read_byte(base + 0);
            ImGui::Text("0x%02X", (unsigned)b_id);
            ImGui::TableSetColumnIndex(2);
            /* "Status" is our name for the byte at offset +1 — a rough
             * proxy for "is this slot used" in the live battle. The
             * field is undocumented; the widget just exposes it so the
             * user can correlate with on-screen. */
            uint8_t b_stat = psx_read_byte(base + 1);
            ImGui::Text("0x%02X", (unsigned)b_stat);
            ImGui::TableSetColumnIndex(3);
            char hex[64] = {0};
            int pos = 0;
            for (int b = 0; b < 16; b++) {
                uint8_t v = psx_read_byte(base + (uint32_t)b);
                hex[pos++] = hex_nibble(v >> 4);
                hex[pos++] = hex_nibble(v & 0xF);
            }
            hex[pos] = '\0';
            ImGui::Text("%s", hex);
        }
        ImGui::EndTable();
    }
}

/* ---- Free Camera panel (W6) ------------------------------------------- */

/* True while the mouse is over (or interacting with) an ImGui window.
 * Left-drag pans only when this is false at press time (or the overlay is
 * hidden), so sliders/buttons keep working while the overlay is open. */
static bool cam_ui_eats_mouse(void)
{
    return s_visible && s_imgui_ready && ImGui::GetIO().WantCaptureMouse;
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
            float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dl < 0.001f) { dx = 0.0f; dy = 0.0f; dz = 1.0f; dl = 1.0f; }
            float fx = dx / dl, fy = dy / dl, fz = dz / dl;
            /* Screen-right = forward x up (y-up right-handed). Ground
             * note: up x forward is the LEFT side — a stale comment here
             * once claimed otherwise and mirrored strafe/glide. */
            float rx = -fz, rz = fx;
            float rlen = std::sqrt(rx*rx + rz*rz);
            if (rlen < 0.001f) { rx = 1.0f; rz = 0.0f; rlen = 1.0f; }
            rx /= rlen; rz /= rlen;

            bool boost = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
            float spd = s_camera_fly_speed * s_cam_dt_scale * (boost ? 8.0f : 1.0f);
            if (ks[SDL_SCANCODE_W]) { ex += fx*spd; ey += fy*spd; ez += fz*spd; ax += fx*spd; ay += fy*spd; az += fz*spd; }
            if (ks[SDL_SCANCODE_S]) { ex -= fx*spd; ey -= fy*spd; ez -= fz*spd; ax -= fx*spd; ay -= fy*spd; az -= fz*spd; }
            if (ks[SDL_SCANCODE_D]) { ex += rx*spd; ez += rz*spd; ax += rx*spd; az += rz*spd; }
            if (ks[SDL_SCANCODE_A]) { ex -= rx*spd; ez -= rz*spd; ax -= rx*spd; az -= rz*spd; }
            if (ks[SDL_SCANCODE_E]) { ey += spd; ay += spd; }
            if (ks[SDL_SCANCODE_Q]) { ey -= spd; ay -= spd; }
            float rot = s_camera_rot_speed * s_cam_dt_scale;
            /* Yaw+ turns right (engine heading convention: D-up displaces
             * (sin h, -cos h)), so LEFT rotates by -rot, RIGHT by +rot. */
            if (ks[SDL_SCANCODE_LEFT]) {
                float c = std::cos(rot), s = std::sin(rot);
                float ndx = dx*c + dz*s, ndz = -dx*s + dz*c;
                ax = ex + ndx; az = ez + ndz; dx = ndx; dz = ndz;
            }
            if (ks[SDL_SCANCODE_RIGHT]) {
                float c = std::cos(rot), s = std::sin(rot);
                float ndx = dx*c - dz*s, ndz = dx*s + dz*c;
                ax = ex + ndx; az = ez + ndz; dx = ndx; dz = ndz;
            }
            if (ks[SDL_SCANCODE_UP])    { ay += spd; }
            if (ks[SDL_SCANCODE_DOWN])  { ay -= spd; }
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
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist < 0.001f) { dx = 0.0f; dy = 0.0f; dz = 1.0f; dist = 1.0f; }
            float yaw = std::atan2(dx, dz);
            float pitch = std::asin(dy / dist);
            yaw   -= (float)mdx * s_cam_mouse_sens;
            float dyaw = (float)mdy * s_cam_mouse_sens * (s_cam_invert_y ? 1.0f : -1.0f);
            pitch += dyaw;
            const float kLim = 1.5533f; /* ~89 deg */
            if (pitch >  kLim) pitch =  kLim;
            if (pitch < -kLim) pitch = -kLim;
            float cp = std::cos(pitch);
            dx = std::sin(yaw) * cp * dist;
            dz = std::cos(yaw) * cp * dist;
            dy = std::sin(pitch) * dist;
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
            float dl = std::sqrt(dx*dx + dz*dz);
            float fx = 0.0f, fz = 1.0f;
            if (dl > 0.001f) { fx = dx / dl; fz = dz / dl; }
            float rx = -fz, rz = fx;
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

/* World orbit step: full 3D flight. WASD flies the origin (look target)
 * along the engine's own view basis (read from the view matrix: row 0 =
 * right, row 2 = forward — GTE +Z-forward, proven by the OT/SZ3 culling
 * rules requiring positive view-Z in front), Q/E move it vertically,
 * mouse/arrows drive yaw/pitch, wheel drives distance. The pipeline then
 * derives the eye from origin+yaw/pitch/dist, so the rig translates and
 * orbits with zero convention error. */
static void camera_orbit_step(bool allow_motion)
{
    constexpr float kTurn = 4096.0f / (2.0f * 3.141592653589793f);
    /* Ground basis from yaw alone (exact, no matrix-layout dependence):
     * D-up headings displace (sin h, -cos h) per FUN_80090c68, so forward
     * F(Y) = (sinY, -cosY), right R(Y) = (cosY, sinY); yaw+ turns right.
     * WASD glides on the plane (Q/E cover height). */
    float yawrad = (float)s_w_yaw * (6.283185307179586f / 4096.0f);
    float sy = std::sin(yawrad), cy = std::cos(yawrad);
    float fx = sy, fz = -cy;
    float rx = cy, rz = sy;
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
            if (ks[SDL_SCANCODE_E]) { oy += spd; }
            if (ks[SDL_SCANCODE_Q]) { oy -= spd; }
            if (ks[SDL_SCANCODE_LEFT])  { s_w_yaw -= (int)std::lround(rot); }
            if (ks[SDL_SCANCODE_RIGHT]) { s_w_yaw += (int)std::lround(rot); }
            if (ks[SDL_SCANCODE_UP])    { s_w_pitch += (int)std::lround(rot * 0.5f); }
            if (ks[SDL_SCANCODE_DOWN])  { s_w_pitch -= (int)std::lround(rot * 0.5f); }
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
    bool typing = s_visible && s_imgui_ready && ImGui::GetIO().WantCaptureKeyboard;
    /* Motion input needs a live path into the debugger: legacy single
     * window keeps the historic rule (works open or closed); the separate
     * tools window must own keyboard focus (playing in the game window
     * never also flies). Typing in a panel field never flies. */
    bool allow_motion = !typing && (s_legacy_inline || tools_input_active());
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

static void draw_camera_section(void)
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
        s_cam_last_ticks = 0u; /* rebase frame-dt: no jump on the first held frame */
        if (en) {
            uint32_t s2 = resident_overlay_sig();
            if (s2 == 0u) {
                std::snprintf(s_camera_status, sizeof(s_camera_status),
                    "Free camera armed: enter field/world/battle/battling to freeze.");
            } else if (camera_freeze(s2)) {
                std::snprintf(s_camera_status, sizeof(s_camera_status),
                    "Free camera ON (%s): drag RIGHT mouse to look, wheel dollies.",
                    resident_module_name(resident_loaded_module()));
            } else {
                std::snprintf(s_camera_status, sizeof(s_camera_status),
                    "Freeze refused (%s): not a freezable camera state (cinematic?).",
                    resident_module_name(resident_loaded_module()));
            }
        } else {
            camera_unfreeze();
            std::snprintf(s_camera_status, sizeof(s_camera_status),
                "Free camera OFF — engine state restored, game camera resumes.");
        }
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

    ImGui::Separator();
    ImGui::TextDisabled("Mouse (works open or closed):");
    ImGui::Checkbox("Right-drag looks (yaw + pitch)", &s_cam_mouse_look);
    ImGui::SliderFloat("Look sensitivity (rad/px)", &s_cam_mouse_sens, 0.0005f, 0.02f, "%.4f");
    ImGui::Checkbox("Invert mouse Y", &s_cam_invert_y);
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
        bool typing_now = s_visible && s_imgui_ready && ImGui::GetIO().WantCaptureKeyboard;
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
        psx_debug_overlay_camera_write(
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
        "Close the overlay (Ctrl+F3) to fly with mouse+keys and no UI.");
}

/* ---- Event Jump panel (W6) -------------------------------------------- */

static void draw_event_jump_section(void)
{
    int ne = 0;
    const DbgEvent *evs = dbg_data_events(&ne);
    if (evs == nullptr || ne == 0) {
        ImGui::TextDisabled("(no events.xml loaded)");
        return;
    }

    /* Filter input (substring match on name + integer match on id). */
    ImGui::InputText("Filter##ej", s_event_filter, sizeof(s_event_filter));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Substring match on event name or id (decimal).");
    }
    ImGui::Checkbox("Allow unverified events##ej", &s_allow_unverified_events);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable Jump for events whose status is not verified.");
    }

    int n_verified = 0;
    for (int i = 0; i < ne; i++) if (evs[i].verified) n_verified++;
    ImGui::Text("%d events  (%d verified, %d unverified)",
                ne, n_verified, ne - n_verified);

    if (s_event_jump_status_frames > 0 && s_event_jump_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_event_jump_status);
        s_event_jump_status_frames--;
    }

    if (!field_module_active()) {
        ImGui::TextDisabled("Field NOT resident: Jump boots Field to the event's map first.");
    }

    if (ImGui::BeginTable("events", 5,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 260))) {
        ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("map");
        ImGui::TableSetupColumn("varWrites");
        ImGui::TableSetupColumn("action");
        ImGui::TableHeadersRow();
        for (int i = 0; i < ne; i++) {
            const DbgEvent &e = evs[i];
            bool show = s_event_filter[0] == '\0';
            if (!show) {
                char hay[160];
                std::snprintf(hay, sizeof(hay), "%d %s", e.id,
                              e.name ? e.name : "");
                int parsed = 0;
                show = (std::strstr(hay, s_event_filter) != nullptr) ||
                       (std::sscanf(s_event_filter, "%d", &parsed) == 1 &&
                        parsed == e.id);
            }
            if (!show) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", e.id);
            ImGui::TableSetColumnIndex(1);
            if (!e.verified && !s_allow_unverified_events) {
                ImGui::TextDisabled("%s", e.name ? e.name : "");
            } else {
                ImGui::Text("%s", e.name ? e.name : "");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d (e=%d)", e.mapId, e.entryPoint);
            ImGui::TableSetColumnIndex(3);
            if (e.numVarWrites == 0) {
                ImGui::TextDisabled("(none)");
            } else {
                char vw[128] = {0};
                int pos = 0;
                for (int v = 0; v < e.numVarWrites; v++) {
                    int n = std::snprintf(vw + pos, sizeof(vw) - (size_t)pos,
                                          "%svar[%d]=%d",
                                          pos ? ", " : "",
                                          e.varWrites[v].var,
                                          e.varWrites[v].value);
                    if (n > 0) pos += n;
                    if (pos >= (int)sizeof(vw) - 1) break;
                }
                ImGui::Text("%s", vw);
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(i);
            bool can_jump = (e.verified || s_allow_unverified_events) &&
                            (s_teleport_target_id < 0);
            if (!can_jump) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Jump")) {
                int rc = psx_debug_overlay_event_jump(e.id);
                if (rc == 0) {
                    std::snprintf(s_event_jump_status,
                        sizeof(s_event_jump_status),
                        "Event %d armed: map=%d entry=%d, %d varWrite(s) applied.",
                        e.id, e.mapId, e.entryPoint, e.numVarWrites);
                } else if (rc == 4) {
                    std::snprintf(s_event_jump_status,
                        sizeof(s_event_jump_status),
                        "Event %d: field boot requested (map=%d), %d varWrite(s) applied.",
                        e.id, e.mapId, e.numVarWrites);
                } else {
                    std::snprintf(s_event_jump_status,
                        sizeof(s_event_jump_status),
                        "Event %d refused (rc=%d) - RAM untouched.",
                        e.id, rc);
                }
                s_event_jump_status_frames = 120;
            }
            if (!can_jump) ImGui::EndDisabled();
            if (!e.verified && !s_allow_unverified_events &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Unverified: status != \"verified\" in events.xml. "
                                  "Greyed out until Ghidra validation.");
            } else if (s_teleport_target_id >= 0 &&
                       ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("A teleport is already in flight.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

/* ---- Gold & fieldVars editor UI --------------------------------------- */

static void draw_gold_vars_section(void)
{
    {
        uint32_t cur = read_u32_le(kAddr_gold);
        if (ImGui::InputScalar("Gold (u32, 0x8006EF58)",
                               ImGuiDataType_U32, &s_gold_value, NULL, NULL,
                               "%u", ImGuiInputTextFlags_None)) {
            s_gold_dirty = 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply##gold")) {
            psx_debug_overlay_write_gold((unsigned)s_gold_value);
            s_gold_dirty = 0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("current in RAM: %u", (unsigned)cur);
        if (ImGui::SmallButton("Read current##gold")) {
            s_gold_value = (int)cur;
        }
    }

    ImGui::Separator();
    ImGui::Text("fieldVars (0x8006EF64, 512 x u16 LE):");
    ImGui::InputText("Filter##vars", s_vars_filter, sizeof(s_vars_filter));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Substring match on var name or var index (decimal).");
    }
    if (ImGui::SmallButton("Read all (live)")) {
        s_gold_value = (int)read_u32_le(kAddr_gold);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Filter applies to the table below.");

    int nv = 0;
    const DbgFlagVar *fvs = dbg_data_flag_vars(&nv);
    if (nv == 0) {
        ImGui::TextDisabled("(no flags.xml loaded)");
        return;
    }
    if (ImGui::BeginTable("vars", 4,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("var");
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("current");
        ImGui::TableSetupColumn("set + apply");
        ImGui::TableHeadersRow();
        for (int i = 0; i < nv; i++) {
            bool show = s_vars_filter[0] == '\0';
            if (!show) {
                char hay[128];
                std::snprintf(hay, sizeof(hay), "%d %s",
                              fvs[i].var,
                              fvs[i].name ? fvs[i].name : "");
                int parsed = 0;
                show = (std::strstr(hay, s_vars_filter) != nullptr) ||
                       (std::sscanf(s_vars_filter, "%d", &parsed) == 1 && parsed == fvs[i].var);
            }
            if (!show) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", fvs[i].var);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", fvs[i].name ? fvs[i].name : "");
            ImGui::TableSetColumnIndex(2);
            uint32_t addr = kAddr_fieldVarsBase + (uint32_t)fvs[i].var * 2u;
            uint16_t v = read_u16_le(addr);
            ImGui::Text("%u (0x%04X)", (unsigned)v, (unsigned)v);
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("##ve", &s_var_edit[fvs[i].var], 0, 0);
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply##va")) {
                psx_debug_overlay_write_var(fvs[i].var, s_var_edit[fvs[i].var]);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}



static void prepare_overlay();

void psx_debug_overlay_init(struct SDL_Window *win, SDL_GLContext ctx)
{
    s_win = win;
    s_imgui_ready = false;
    s_legacy_inline = false;
    s_separate_ready = false;
    s_tools_win = nullptr;
    s_tools_ctx = nullptr;
    s_window_shot_armed = false;
    s_window_shot_path[0] = '\0';
    s_bind_framebuffer = nullptr;
    if(ctx&&ctx==SDL_GL_GetCurrentContext())prepare_overlay();
}

void psx_debug_overlay_shutdown(void)
{
    if (s_imgui_ready) {
        if (s_separate_ready && s_tools_ctx) {
            SDL_GL_MakeCurrent(s_tools_win, s_tools_ctx);
        }
        ImGui_ImplOpenGL3_Shutdown();
        PSX_IMGUI_SDL_SHUTDOWN();
        ImGui::DestroyContext();
        s_imgui_ready = false;
    }
    if (s_tools_ctx) {
        SDL_GL_DeleteContext(s_tools_ctx);
        s_tools_ctx = nullptr;
    }
    if (s_tools_win) {
        SDL_DestroyWindow(s_tools_win);
        s_tools_win = nullptr;
    }
    s_separate_ready = false;
    s_legacy_inline = false;
    s_win = nullptr;
    s_bind_framebuffer = nullptr;
    s_window_shot_armed = false;
}

void psx_debug_overlay_toggle(void)
{
    const bool was_visible = s_visible;
    s_visible = !s_visible;

    /* Frame-interpolation guard: the interpolation present path runs on its
     * own GL share-context thread, so the ImGui frame on top of an interp
     * frame can flicker / be skipped. Force interp off while visible;
     * restore on close. host_hz/target_hz mirror the runtime's startup call
     * (main.cpp gl_renderer_set_interpolation) so the GPU side rebuilds
     * identically. */
    if (s_visible && !was_visible) {
        int enabled = 0, suspended = 0, history = 0;
        double host_hz = 0.0, target_hz = 0.0;
        uint64_t swaps = 0;
        gl_renderer_interpolation_diag(&enabled, &suspended, &history,
                                       &host_hz, &target_hz, &swaps);
        if (enabled && !s_interp_guard_active) {
            s_interp_guard_active = true;
            gl_renderer_set_interpolation(0, host_hz, target_hz, 0.0, 0);
        }
        if (s_separate_ready && s_tools_win) {
            SDL_ShowWindow(s_tools_win);
            SDL_RaiseWindow(s_tools_win);
        }
    } else if (!s_visible && was_visible) {
        if (s_interp_guard_active) {
            s_interp_guard_active = false;
            if (psx_frame_interpolation_enabled())
                psx_frame_interpolation_set(1);
        }
        if (s_separate_ready && s_tools_win) {
            SDL_HideWindow(s_tools_win);
        }
    }
}

bool psx_debug_overlay_is_visible(void)
{
    return s_visible;
}

/* The ImGui backend lives on this window; text input starts/stops there. */
static SDL_Window *overlay_active_window(void)
{
    if (s_separate_ready && s_tools_win) return s_tools_win;
    return s_win;
}

/* True when the given window currently owns keyboard focus. */
static bool window_has_input_focus(SDL_Window *win)
{
    return win && ((SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0);
}

/* True when free-camera motion input (keys/mouse/wheel) may drive the
 * camera this frame. Legacy in-game path: the single window (old rule —
 * works open or closed). Separate window: either debugger window focused
 * (tools for tweaking, game for flying over the fullscreen view); alt-tab
 * away disables motion. */
static bool tools_input_active(void)
{
    if (s_legacy_inline) return true;
    if (!s_visible || !s_imgui_ready || !s_tools_win) return false;
    return window_has_input_focus(s_tools_win) || window_has_input_focus(s_win);
}

bool psx_debug_overlay_process_event(const SDL_Event *ev)
{
    if (!ev) return false;

    /* Tools-window close button (X): hide the overlay instead of quitting.
     * Consumed before ImGui sees it; game-window close still quits via
     * main.cpp. SDL2/SDL3 spell window-close differently. */
    if (s_tools_win) {
#if defined(PSX_SDL3)
        if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            ev->window.windowID == SDL_GetWindowID(s_tools_win)) {
#else
        if (ev->type == SDL_WINDOWEVENT &&
            ev->window.event == SDL_WINDOWEVENT_CLOSE &&
            ev->window.windowID == SDL_GetWindowID(s_tools_win)) {
#endif
            if (s_visible) psx_debug_overlay_toggle();
            return true;
        }
    }

    /* Free-camera wheel dolly: accumulate notches and (when enabled)
     * consume the event before ImGui sees it, so the wheel flies the
     * camera instead of scrolling panels. Uncheck "Wheel dollies" in
     * the panel to scroll the UI with the wheel while flying. */
    if (ev->type == SDL_MOUSEWHEEL && s_camera_enabled && s_cam_wheel_dolly &&
        (s_legacy_inline || tools_input_active())) {
        s_cam_wheel_accum += (float)ev->wheel.y;
        return true;
    }

    /* Feed every SDL event into ImGui's SDL backend so its key state,
     * mouse motion, and text-input composition stay current. Gated on
     * s_imgui_ready to avoid touching a dead context; harmless when hidden
     * (ImGui just queues events internally — no render). The Ctrl+F3
     * consume check below is unchanged. */
    if (s_imgui_ready) {
        PSX_IMGUI_SDL_PROCESS_EVENT(ev);
    }

    if (ev->type != SDL_KEYDOWN) return false;
    /* Ctrl+F3 toggles the overlay. The keysym is F3 AND Ctrl is held;
     * plain F3 is the savestate load slot 2 hotkey in main.cpp, so
     * main.cpp consults this function before its savestate block and
     * skips the rest of the event when we return true. */
    if (psx_sdl_event_keycode(ev) == SDLK_F3 &&
        (psx_sdl_event_keymod(ev) & KMOD_CTRL)) {
        psx_debug_overlay_toggle();
        return true;
    }
    return false;
}

bool psx_debug_overlay_swallow_keyboard(void)
{
    /* The runtime's pad sampler polls SDL_GetKeyboardState every frame —
     * WantCaptureKeyboard alone does NOT stop that, so we MASK it.
     * Legacy in-game path (single window): historic rules — flying with
     * capture on masks (any camera module), visible-but-not-ready masks
     * (open edge), ImGui text input masks.
     * Separate window: the OS routes keys to the focused window, but the
     * game polls globally, so mask while the tools window owns keyboard
     * focus (typing values or flying must not also drive the player) or
     * while ImGui holds the keyboard. A hidden tools window never masks. */
    if (s_legacy_inline) {
        if (s_camera_enabled && s_cam_capture_input && resident_overlay_sig() != 0u) return true;
        if (!s_visible) return false;
        if (!s_imgui_ready) return true;
        return ImGui::GetIO().WantCaptureKeyboard;
    }
    if (!s_visible || !s_imgui_ready) return false;
    if (window_has_input_focus(s_tools_win)) return true;
    if (ImGui::GetIO().WantCaptureKeyboard) return true;
    if (window_has_input_focus(s_win) && s_camera_enabled &&
        s_camera_keys_enable && s_cam_capture_input &&
        resident_overlay_sig() != 0u)
        return true;
    return false;
}

/* Used by the debug_server's overlay_capture_state command — exposes the
 * three flags the pad-mask logic depends on, so the test can assert the
 * guard without injecting SDL events. Read-only snapshot. */
void psx_debug_overlay_capture_state(int *visible, int *want_capture,
                                     int *swallow)
{
    if (visible)         *visible = s_visible ? 1 : 0;
    if (want_capture)    *want_capture = (s_visible && s_imgui_ready &&
                                          ImGui::GetIO().WantCaptureKeyboard) ? 1 : 0;
    if (swallow)         *swallow = psx_debug_overlay_swallow_keyboard() ? 1 : 0;
}

void psx_debug_overlay_window_shot_arm(const char *path)
{
    if (!path || !*path) path = "window_shot.png";
    std::strncpy(s_window_shot_path, path, sizeof(s_window_shot_path) - 1);
    s_window_shot_path[sizeof(s_window_shot_path) - 1] = '\0';
    s_window_shot_armed = true;
}

int psx_debug_overlay_set_force_capture(int on)
{
    if (on >= 0) s_force_text_capture = (on != 0);
    return s_force_text_capture ? 1 : 0;
}

/* The pre-swap hook. Called on the main thread, before SDL_GL_SwapWindow,
 * from each of the four present paths in gpu_gl_renderer.c. Three things
 * happen here, in this order:
 *   1. Lazy ImGui init (first call only). Skipped if no GL context is
 *      current (next-frame retry; the ImGui GL3 backend init must run
 *      with the context current).
 *   2. If the overlay is visible: draw the minimal "Xenogears Debug" window.
 *      The ImGui GL3 backend handles its own program/VAO/texture/enable
 *      save+restore, but NOT the FBO binding, so this hook binds the supplied
 *      target immediately before and after RenderDrawData.
 *   3. If window_shot is armed: read the selected target (composited, with
 *      overlay if visible) and write the PNG. Done AFTER RenderDrawData
 *      so the shot includes the overlay's pixels.
 *
 * Hidden + unarmed performs no target binding or rendering; lazy init and the
 * existing per-attempt guest preparation retain their prior policy.
 */

/* One ImGui context bound to (win, ctx). The caller must hold that GL
 * context current. Rolls back cleanly on failure (retry next frame). */
static bool imgui_init_for(SDL_Window *win, SDL_GLContext ctx)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    /* DockingEnable / ViewportsEnable are not present in the vendored
     * ImGui 1.91.9b "master" build (docking branch only); leave
     * ConfigFlags at the default zero. */
    io.IniFilename = "debug_overlay.ini";

    ImGui::StyleColorsDark();
    /* Larger UI than the 13px default — the tools window sits beside the
     * game at monitor distance. Font must be added before the GL backend
     * init builds the atlas texture. */
    {
        ImFontConfig font_cfg;
        font_cfg.SizePixels = 19.0f;
        io.Fonts->AddFontDefault(&font_cfg);
        ImGui::GetStyle().ScaleAllSizes(1.25f);
    }

    if (!PSX_IMGUI_SDL_INIT_OPENGL(win, ctx)) {
        ImGui::DestroyContext();
        return false;
    }
    /* "#version 330 core" matches the GL context the runtime creates
     * (gpu_gl_renderer.c creates a core 3.3 context); the tools context
     * below requests the same profile. */
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        PSX_IMGUI_SDL_SHUTDOWN();
        ImGui::DestroyContext();
        return false;
    }
    return true;
}

/* Create the independent tools window + GL context. Same recipe the native
 * GPU thread uses (hidden window, shared context while the game context is
 * current). Starts hidden — toggle() shows it. Shows immediately when the
 * overlay is already visible (late init). Idempotent. */
static bool tools_window_ensure(SDL_Window *game_win, SDL_GLContext game_ctx)
{
    if (s_separate_ready) return true;
    if (!game_win || !game_ctx) return false;
    /* GL attributes are global inputs to the next context creation (same
     * core-3.3 recipe as main.cpp); set immediately before creating. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_Window *w = SDL_CreateWindow(
        "Xenogears Debug",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        560, 850,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!w) {
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        return false;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!ctx) {
        SDL_DestroyWindow(w);
        return false;
    }
    /* Immediate present on the tools context: its SwapWindow must never
     * block the game frame on vblank. Restore the entry context after. */
    SDL_GL_MakeCurrent(w, ctx);
    SDL_GL_SetSwapInterval(0);
    SDL_GL_MakeCurrent(game_win, game_ctx);
    s_tools_win = w;
    s_tools_ctx = ctx;
    s_separate_ready = true;
    if (s_visible) {
        SDL_ShowWindow(s_tools_win);
        SDL_RaiseWindow(s_tools_win);
    }
    return true;
}

static void prepare_overlay()
{
    /* Step 1: lazy init. */
    if (!s_imgui_ready && !s_legacy_inline) {
        if (!s_win) return; /* not initialized at all — try next frame */
        SDL_GLContext ctx = SDL_GL_GetCurrentContext();
        if (!ctx) return;   /* main context not current yet — try next frame */

        /* Prefer the independent tools window; fall back to the legacy
         * in-game render when it cannot be created (headless/GL limits).
         * Either way s_imgui_ready gates the UI below. */
        if (tools_window_ensure(s_win, ctx)) {
            /* Backend init builds GL objects: hold the tools context. */
            SDL_GL_MakeCurrent(s_tools_win, s_tools_ctx);
            if (!imgui_init_for(s_tools_win, s_tools_ctx)) {
                /* Tools GL broken after window creation — tear it down and
                 * try the legacy path next frame. */
                SDL_GL_MakeCurrent(s_win, ctx);
                SDL_GL_DeleteContext(s_tools_ctx);
                SDL_DestroyWindow(s_tools_win);
                s_tools_ctx = nullptr;
                s_tools_win = nullptr;
                s_separate_ready = false;
                return;
            }
            SDL_GL_MakeCurrent(s_win, ctx);
        } else if (!imgui_init_for(s_win, ctx)) {
            /* Init failed — roll back and try again next frame. */
            return;
        } else {
            s_legacy_inline = true;
        }
        s_imgui_ready = true;

        /* Cache the backend name once. gr_backend() returns the EFFECTIVE
         * backend (post-init), which is stable for the rest of the process
         * — querying it on every frame would be wasted work. */
        switch (gr_backend()) {
            case 0: s_backend_name = "software"; break;
            case 1: s_backend_name = "opengl";   break;
            case 2: s_backend_name = "vulkan";   break;
            default: s_backend_name = "?";       break;
        }
    }

    /* Step 1b: load XML data tables once. SDL_GetBasePath() is stable for
     * the process lifetime and the dir is staged by POST_BUILD. A failure
     * here is non-fatal — dbg_data_*_missing() lets the UI render an empty
     * placeholder so the user can still use the other widgets. */
    if (!s_dbg_data_loaded) {
        s_dbg_data_loaded = true;   /* one-shot latch; never re-try */
        const char *base = SDL_GetBasePath();
        char path[1024];
        if (base && *base) {
            std::snprintf(path, sizeof(path), "%sdebug_overlay/data", base);
        } else {
            std::snprintf(path, sizeof(path), "debug_overlay/data");
        }
        (void)dbg_data_load_all(path);
    }

}

/* Shared window content for both render paths. Legacy: floating window
 * over the game. Separate: fills the tools window (the OS chrome carries
 * the "Xenogears Debug" title). */
static void draw_debug_window(void)
{
if (s_legacy_inline) {
    ImGui::SetNextWindowSize(ImVec2(480, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("Xenogears Debug");
} else {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::Begin("Xenogears Debug", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
}
ImGui::Checkbox("Visible (Ctrl+F3)", &s_visible);
/* No visible widget for this: TCP tests set s_force_text_capture via
     * overlay_force_capture so ImGui reports WantCaptureKeyboard=true
     * deterministically without SDL injection. The field only appears
     * while that flag is armed. */
    if (s_force_text_capture) {
        /* WantCaptureKeyboard is only set when a widget is ACTIVELY
         * requesting keyboard input; a drawn-but-unfocused InputText
         * does not request it. Re-assert focus on the next widget every
         * frame so the field stays bound and the mask is stable. */
        ImGui::SetKeyboardFocusHere(0);
        ImGui::InputText("##force_text", s_force_text_buf,
                         sizeof(s_force_text_buf));
    }
    ImGui::Separator();

    /* ---- Section 1: GPU State (read-only) ---- */
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("GPU State")) {
        draw_gpu_state_section();
    }

    /* ---- Section 2: RAM Inspector ---- */
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("RAM Inspector")) {
        draw_ram_inspector_section();
    }

    /* ---- Section 3: Toggles ---- */
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Toggles")) {
        draw_toggles_section();
    }

    /* ---- Section: Disc (multi-disc hot swap) ---- */
    if (ImGui::CollapsingHeader("Disc")) {
        draw_disc_section();
    }

    /* ---- Section 4: Rings ---- */
    if (ImGui::CollapsingHeader("Rings")) {
        draw_rings_section();
    }

    /* ---- Section 5: Map Teleport (write action) ---- */
    if (ImGui::CollapsingHeader("Map Teleport")) {
        draw_teleport_section();
    }

    /* ---- Section 6: Party (write action) ---- */
    if (ImGui::CollapsingHeader("Party")) {
        draw_party_section();
    }

    /* ---- Section 7: Gold & Variables (write action) ---- */
    if (ImGui::CollapsingHeader("Gold & Variables")) {
        draw_gold_vars_section();
    }

    /* ---- Section 8: Force Battle (W6) ---- */
    if (ImGui::CollapsingHeader("Force Battle")) {
        draw_battle_section();
    }

    /* ---- Section 9: Free Camera (W6) ---- */
    if (ImGui::CollapsingHeader("Free Camera")) {
        draw_camera_section();
    }

    /* ---- Section 10: Event Jump (W6) ---- */
    if (ImGui::CollapsingHeader("Event Jump")) {
        draw_event_jump_section();
    }

    ImGui::End();
}

/* Drive SDL text input from this frame's WantTextInput. Latched so
 * we only call SDL_Start/Stop on transitions, not every frame;
 * pre_swap runs on the same thread as the SDL event pump, so the
 * state is correct for the NEXT PollEvent cycle. */
static void drive_text_input(SDL_Window *win)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput && !s_text_input_started) {
        psx_sdl_start_text_input(win);
        s_text_input_started = true;
    } else if (!io.WantTextInput && s_text_input_started) {
        psx_sdl_stop_text_input(win);
        s_text_input_started = false;
    }
}

/* One ImGui frame into the independent tools window. The entry GL
 * (window, context) is captured per call — pre_swap runs in several
 * present paths that do not all share one context — and restored
 * afterwards, so the game present and any window_shot readback proceed
 * untouched. */
static void render_tools_window(GLuint target_fbo)
{
    SDL_Window *entry_win = SDL_GL_GetCurrentWindow();
    SDL_GLContext entry_ctx = SDL_GL_GetCurrentContext();
    if (!entry_win || !entry_ctx) return;
    if (SDL_GL_MakeCurrent(s_tools_win, s_tools_ctx) != 0) return;
    int tw = 0, th = 0;
    SDL_GL_GetDrawableSize(s_tools_win, &tw, &th);
    if (tw <= 0 || th <= 0) {
        SDL_GL_MakeCurrent(entry_win, entry_ctx);
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    PSX_IMGUI_SDL_NEW_FRAME();
    ImGui::NewFrame();

    draw_debug_window();
    drive_text_input(s_tools_win);

    ImGui::Render();
    glViewport(0, 0, (GLsizei)tw, (GLsizei)th);
    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(s_tools_win);
    /* Hand the GL thread back exactly as found. bind_overlay_target keeps
     * the game-target FBO selected for the game present / window_shot. */
    SDL_GL_MakeCurrent(entry_win, entry_ctx);
    (void)bind_overlay_target(target_fbo);
}

void psx_debug_overlay_pre_swap_target(unsigned int framebuffer)
{
    const GLuint target_fbo=(GLuint)framebuffer;
    /* Presented-frame + vblank rates (GPU State readouts). Runs on every
     * present, visible or not — one pre_swap call is one image on screen. */
    gpu_state_sample_rates();
    prepare_overlay();
    if(!s_imgui_ready)return;
    if (s_teleport_target_id >= 0) {
        /* Confirm via the fieldID mirror: the persist step copies the
         * target there as the transition engages. Same-map teleports
         * keep the same id, so require a grace period before accepting
         * equality (the reload still takes ~seconds of fades). On
         * timeout, say where we are stuck instead of going silent —
         * the usual cause is an out-of-range entry point (arrival
         * table over-read -> garbage spawn -> black screen). */
        int cur_field = psx_debug_overlay_read_field_id();
        uint64_t now = SDL_GetTicks64();
        bool same_map = (s_teleport_target_id == s_teleport_source_id);
        bool confirmed = (cur_field == s_teleport_target_id) &&
                         (!same_map || (now - s_teleport_arm_ms) > kTeleportSameMapConfirmMs);
        if (confirmed) {
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport done: now on field %d.", cur_field);
            s_teleport_status_frames = 240;
            /* Keep s_last_teleport_id >= 0 so the panel shows the
             * message; the settle block below clears it. */
            s_teleport_target_id = -1;
            s_teleport_ready_ms = now + kTeleportSettleMs;
        } else if (now >= s_teleport_deadline_ms) {
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport timed out (still on field %d) - bad entry? engine stuck?",
                cur_field);
            s_teleport_status_frames = 240;
            s_teleport_target_id = -1;
            s_teleport_ready_ms = now + kTeleportSettleMs;
        }
    }
    if (read_u32_le(kAddr_teleportGateMusic) != 0u) {
        spu_debug_music_quarantine_end();
    }
    if (s_teleport_ready_ms != 0u && SDL_GetTicks64() >= s_teleport_ready_ms) {
        s_last_teleport_id = -1;
        s_teleport_ready_ms = 0u;
    }

    /* Step 1c: free-camera hold. Runs BEFORE the ImGui frame so the panel's
     * live readout reflects this frame's pose. Runs with the overlay open
     * OR closed (closed = fly around while playing). The resident overlay
     * sig (4=field, 5=world, 6=battle, 7=battling) selects the freeze +
     * hold path; anything else releases the freeze. While enabled the
     * owner logic stays frozen and the pose is driven every frame; on
     * disable the saved engine state is restored. */
    if (s_camera_enabled) {
        apply_camera_frame(resident_overlay_sig());
    } else if (s_cam_frozen_mod >= 0) {
        camera_unfreeze();
    }

    /* Step 2: render. Skipped when hidden (zero GL work). Legacy path
     * composites into the game target; the separate path draws into the
     * tools window and leaves the game framebuffer untouched. */
    if (s_visible) {
        if (s_legacy_inline) {
            ImGui_ImplOpenGL3_NewFrame();
            PSX_IMGUI_SDL_NEW_FRAME();
            ImGui::NewFrame();

            draw_debug_window();
            drive_text_input(overlay_active_window());

            ImGui::Render();
            if (bind_overlay_target(target_fbo)) {
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                (void)bind_overlay_target(target_fbo);
            }
        } else {
            /* The tools window is pure UI: redrawing it on every game
             * present (up to 440/s at 240 Hz targets) burns full ImGui
             * renders + window swaps that also perturb the very cadence
             * being measured. Cap at ~60 Hz; game sampling above is
             * unaffected (it runs per pre_swap regardless). */
            static uint64_t s_tools_last_ms = 0u;
            const uint64_t now_ms = SDL_GetTicks64();
            if (now_ms - s_tools_last_ms >= 16u) {
                s_tools_last_ms = now_ms;
                render_tools_window(target_fbo);
            }
        }
    } else if (s_text_input_started) {
        /* Hidden mid-text-input (Ctrl+F3 while typing): release the latch
         * to keep SDL text input off across the open/close cycle. */
        psx_sdl_stop_text_input(overlay_active_window());
        s_text_input_started = false;
    }

    /* Step 3: window_shot readback. Always runs when armed (works hidden
     * too — captures the game-only frame in that case). Must run AFTER
     * RenderDrawData when visible so the overlay's pixels are in the back
     * buffer. capture_window_rgb() binds and leaves only the caller-selected
     * target, even if the ImGui frame changed unrelated GL state. */
    if (s_window_shot_armed) {
        int w = 0, h = 0;
        uint8_t *rgb = capture_window_rgb(target_fbo, &w, &h);
        if (rgb) {
            write_rgb_png(s_window_shot_path, rgb, w, h);
            std::free(rgb);
        }
        s_window_shot_armed = false;
        s_window_shot_path[0] = '\0';
    }
}

void psx_debug_overlay_pre_swap(void)
{
    psx_debug_overlay_pre_swap_target(0u);
}

void psx_debug_overlay_post_swap(int completed)
{
    if (!completed && s_present_count > s_present_last)
        s_present_count--;
}

/* ---- widget action hook (debug-only, TCP-driven) ----------------------- */

/* ---- Disc: multi-disc hot swap ------------------------------------------ */
static char s_disc_swap_error[256];

static void draw_disc_section(void)
{
    const int count = psx_disc_count();
    if (count < 2) {
        ImGui::TextDisabled("Single-disc title");
        return;
    }
    ImGui::Text("In drive: Disc %d", psx_disc_mounted());
    for (int disc = 1; disc <= count; ++disc) {
        const char* path = psx_disc_path(disc);
        ImGui::PushID(disc);
        ImGui::BeginDisabled(disc == psx_disc_mounted());
        char label[32];
        std::snprintf(label, sizeof(label), "Insert Disc %d", disc);
        if (ImGui::Button(label))
            (void)psx_disc_swap(disc, s_disc_swap_error, sizeof(s_disc_swap_error));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", path && path[0] ? path : "(not located)");
        ImGui::PopID();
    }
    ImGui::TextWrapped("Opens the lid, inserts the disc and closes it after "
                       "two emulated seconds. Use it when the game asks for "
                       "another disc.");
    if (s_disc_swap_error[0])
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_disc_swap_error);
}

int psx_debug_overlay_widget_action(const char *name, int value, int value2)
{
    if (!name) return -1;

    /* Each branch calls the SAME setter the in-window widget uses
     * (draw_toggles_section). The TCP test drives this function to assert
     * the action path is wired to real runtime state — flipping a widget
     * value must change the matching TCP getter, which is the S4
     * contract. */
    if (std::strcmp(name, "texfilter") == 0) {
        gr_set_texture_filter(value ? 1 : 0);
        return 0;
    }
    if (std::strcmp(name, "native_wide") == 0) {
        if (value < 0) value = 0;
        if (value > 2) value = 2;
        psx_ws_set_native_wide(value);
        return 0;
    }
    if (std::strcmp(name, "developer_mode") == 0) {
        if (value == 0) return -2;
        request_developer_mode();
        return 0;
    }
    if (std::strcmp(name, "kernel_menu") == 0) {
        if (value == 0) return -2;
        request_kernel_menu();
        return 0;
    }
    if (std::strcmp(name, "disc_swap") == 0) {
        return psx_disc_swap(value, s_disc_swap_error,
                             sizeof(s_disc_swap_error)) ? 0 : -3;
    }
    if (std::strcmp(name, "swap_controller_ports") == 0) {
        if (value == 0) return -2;
        return psx_input_swap_controller_ports() ? 0 : -3;
    }
    if (std::strcmp(name, "aspect_set") == 0) {
        int num = value  > 0 ? value  : 4;
        int den = value2 > 0 ? value2 : 3;
        s_aspect_num = num;
        s_aspect_den = den;
        gte_set_display_aspect_ex(num, den);
        return 0;
    }
    if (std::strcmp(name, "bd_stretch_on") == 0) {
        g_ws_bd_stretch_on = value ? 1 : 0;
        return 0;
    }
    if (std::strcmp(name, "bd_stretch_pct") == 0) {
        if (value < 0)   value = 0;
        if (value > 200) value = 200;
        g_ws_bd_stretch_pct = value;
        return 0;
    }
    if (std::strcmp(name, "interp") == 0) {
        psx_frame_interpolation_set(value ? 1 : 0);
        return 0;
    }
    if (std::strcmp(name, "native_interp_fps") == 0) {
        return psx_native_semantic_fps_set(value) > 0 ? 0 : -2;
    }
    if (std::strcmp(name, "supersampling") == 0) {
        psx_video_set_supersampling(value);
        return 0;
    }
    if (std::strcmp(name, "antialiasing") == 0) {
        psx_video_set_antialiasing(value ? 1 : 0);
        return 0;
    }
    if (std::strcmp(name, "screen_model") == 0) {
        psx_video_set_screen_model(value);
        return 0;
    }
    if (std::strcmp(name, "turbo_loads") == 0) {
        g_turbo_loads_enabled = value ? 1 : 0;
        return 0;
    }
    if (std::strcmp(name, "spu_hq") == 0) {
        psx_audio_set_spu_hq(value ? 1 : 0);
        return 0;
    }
    if (std::strcmp(name, "native_depth_test") == 0) {
        psx_video_set_native_depth_test(value ? 1 : 0);
        return 0;
    }
    if (std::strcmp(name, "native_depth_view") == 0) {
        gl_renderer_set_native_depth_view(value);
        return 0;
    }
    if (std::strcmp(name, "native_wireframe") == 0) {
        gl_renderer_set_native_wireframe(value ? 1 : 0);
        return 0;
    }
    if (std::strcmp(name, "window_width") == 0) {
        psx_video_set_window_width(value);
        return 0;
    }
    if (std::strcmp(name, "dump_event_ring") == 0) {
        int n = event_ring_dump_file("event_ring.json");
        return n >= 0 ? 0 : -2;
    }
    if (std::strcmp(name, "dump_latency_ring") == 0) {
        /* Mirror the TCP latency handler's response shape. */
        static char sum[2048];
        static char rawbuf[16 * 1024];
        FILE *f = std::fopen("latency_ring.json", "w");
        if (!f) return -2;
        std::fprintf(f, "{\"summary\":");
        int w = latency_ring_summary_json(sum, (int)sizeof(sum), 240);
        std::fwrite(sum, 1, (size_t)w, f);
        std::fprintf(f, ",\"frames\":");
        int w2 = latency_ring_dump_json(rawbuf, (int)sizeof(rawbuf), 120);
        std::fwrite(rawbuf, 1, (size_t)w2, f);
        std::fprintf(f, "}\n");
        std::fclose(f);
        return 0;
    }
    if (std::strcmp(name, "dump_starv_ring") == 0) {
        /* starvation_ring_dump() is one-shot (the watchdog mechanism), so
         * build a fresh JSON file from the per-entry accessors the TCP
         * starv_ring handler uses (starvation_ring_total + _get). */
        uint64_t total = starvation_ring_total();
        FILE *f = std::fopen("starvation_ring.json", "w");
        if (!f) return -2;
        std::fprintf(f, "{\"total\":%llu,\"entries\":[",
                     (unsigned long long)total);
        int emitted = 0;
        for (uint64_t s = 0; s < total; s++) {
            StarvationEntry e;
            if (!starvation_ring_get(s, &e)) continue;
            std::fprintf(f,
                "%s{\"seq\":%llu,\"kind\":%u,"
                "\"cyc\":%llu,\"us\":%llu,"
                "\"func\":\"0x%08X\",\"store_pc\":\"0x%08X\","
                "\"in_exc\":%u}",
                emitted ? "," : "",
                (unsigned long long)e.seq, (unsigned)e.kind,
                (unsigned long long)e.psx_cycle_count,
                (unsigned long long)e.host_us,
                e.current_func, e.last_store_pc, e.in_exception);
            emitted++;
        }
        std::fprintf(f, "],\"emitted\":%d}\n", emitted);
        std::fclose(f);
        return 0;
    }
    if (std::strcmp(name, "teleport") == 0) {
        return psx_debug_overlay_teleport(value, value2);
    }
    if (std::strcmp(name, "party_slot") == 0) {
        /* value = slot*256 + charId (slot in low byte,
         * charId in next byte). value2 = bitfieldBit (or -1 to skip). */
        int slot = value & 0xFF;
        int charId = (value >> 8) & 0xFF;
        int bfBit = value2;
        return psx_debug_overlay_write_party_slot(slot, charId, bfBit);
    }
    if (std::strcmp(name, "party_set") == 0) {
        /* Atomic formation: value = c0 | c1<<8 | c2<<16 (0xFF = empty).
         * Full validation (duplicates/holes/empty/busy/module); packs
         * left before writing. value2 unused. */
        int c0 = value & 0xFF;
        int c1 = (value >> 8) & 0xFF;
        int c2 = (value >> 16) & 0xFF;
        return psx_debug_overlay_write_party_formation(c0, c1, c2);
    }
    if (std::strcmp(name, "party_bitfield") == 0) {
        return psx_debug_overlay_write_party_bitfield(value);
    }
    if (std::strcmp(name, "gold") == 0) {
        return psx_debug_overlay_write_gold((unsigned int)value);
    }
    if (std::strcmp(name, "write_var") == 0) {
        return psx_debug_overlay_write_var(value, value2);
    }
    if (std::strcmp(name, "read_field_id") == 0) {
        return psx_debug_overlay_read_field_id();
    }
    /* ---- W6 actions ---- */
    if (std::strcmp(name, "force_battle") == 0) {
        /* value = the gate value to write to 0x800B2298. 0 disables,
         * non-zero arms random encounters (best-effort: needs field
         * encounter data + countdown). */
        return psx_debug_overlay_force_battle(value);
    }
    if (std::strcmp(name, "start_battle") == 0) {
        /* Explicit battle via the panel editor (same path as the button):
         * value = arena override 0..74 (0xFF = keep panel), value2 =
         * global enemy index to append (0..kBattleEnemyCount-1, negative
         * = skip). Runs roster + party + record + handoff. */
        if (value >= 0 && value < kBattleArenaCount) s_battle_arena = value;
        if (value2 >= 0 && value2 < kBattleEnemyCount) {
            const DbgBattleEnemy &e = kBattleEnemies[value2];
            if (e.set != s_battle_enemy_set) {
                for (int l = 0; l < 8; l++) {
                    s_battle_lane_def[l] = 0xFF;
                    s_battle_lane_gear[l] = false;
                }
                s_battle_enemy_set = e.set;
            }
            for (int l = 0; l < 8; l++) {
                if (s_battle_lane_def[l] < 0 || s_battle_lane_def[l] > 7) {
                    s_battle_lane_def[l] = e.def;
                    s_battle_lane_gear[l] = battle_set_is_gear(e.set);
                    break;
                }
            }
        }
        return battle_apply();
    }
    if (std::strcmp(name, "battle_enemy") == 0) {
        /* Append a global enemy (by kBattleEnemies index) to the first
         * empty lane, switching sets (clearing lanes) as needed. Returns
         * 0 on append, -2 on bad index, -3 when lanes are full. */
        if (value < 0 || value >= kBattleEnemyCount) return -2;
        const DbgBattleEnemy &e = kBattleEnemies[value];
        if (e.set != s_battle_enemy_set) {
            for (int l = 0; l < 8; l++) {
                s_battle_lane_def[l] = 0xFF;
                s_battle_lane_gear[l] = false;
            }
            s_battle_enemy_set = e.set;
        }
        for (int l = 0; l < 8; l++) {
            if (s_battle_lane_def[l] < 0 || s_battle_lane_def[l] > 7) {
                s_battle_lane_def[l] = e.def;
                s_battle_lane_gear[l] = battle_set_is_gear(e.set);
                return 0;
            }
        }
        return -3;
    }
    if (std::strcmp(name, "camera_write") == 0) {
        /* Pack 6 s16 coords into 2 ints. value = (ey<<16)|ex,
         * value2 = (ay<<16)|ax; ez/az come from the panel's float editor
         * state. Writes the resident module's pose (field fixed16,
         * battle s16+packed desired, battling s32). One-shot unless the
         * panel's free camera is enabled, which then holds the pose.
         * World orbit mode has no positional pose: returns -3 there. */
        int ex = (int16_t)(value & 0xFFFF);
        int ey = (int16_t)((value >> 16) & 0xFFFF);
        int ax = (int16_t)(value2 & 0xFFFF);
        int ay = (int16_t)((value2 >> 16) & 0xFFFF);
        return psx_debug_overlay_camera_write(ex, ey, (int)std::lround(s_cam_eye_f[2]),
                                              ax, ay, (int)std::lround(s_cam_at_f[2]));
    }
    if (std::strcmp(name, "event_jump") == 0) {
        /* value = event id (index into events.xml table). Applies the
         * event's varWrites then teleports. The event's verified flag
         * is NOT enforced by the TCP action (TCP clients can see
         * disabled buttons via the panel; for headless testing the
         * guard is the field-module-active check inside the
         * teleport recipe). */
        return psx_debug_overlay_event_jump(value);
    }
    return -1;
}

int psx_debug_overlay_take_developer_mode_request(void)
{
    const bool requested = s_developer_mode_request;
    s_developer_mode_request = false;
    return requested ? 1 : 0;
}

int psx_debug_overlay_take_kernel_menu_request(void)
{
    const bool requested = s_kernel_menu_request;
    s_kernel_menu_request = false;
    return requested ? 1 : 0;
}

int psx_debug_overlay_take_field_boot_request(int *fieldId, int *entryPoint)
{
    if (s_field_boot_target < 0) {
        return 0;
    }
    if (fieldId)    *fieldId    = s_field_boot_target;
    if (entryPoint) *entryPoint = s_field_boot_entry;
    s_field_boot_target = -1;
    return 1;
}

#endif /* PSX_DEBUG_OVERLAY */
