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
 *   psx_debug_overlay_pre_swap — called by gpu_gl_renderer.c on the main
 *                             thread at the bottom of each present path,
 *                             BEFORE SDL_GL_SwapWindow. On the first call
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

/* The vendored Dear ImGui lives at recomp-ui/src/third_party/imgui. Its
 * include dirs are applied target-wide by recomp-ui/recomp_ui.cmake, so
 * the three headers below resolve from this TU. SDL2 headers come from the
 * runtime target (PSXRECOMP_RUNTIME_INCLUDE_DIRS + SDL2_INCLUDE_DIRS). */
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
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
 * functions we need ourselves. The symbols come from libGL.so on Linux,
 * opengl32.dll on Windows, OpenGL.framework on macOS — all already
 * linked by the runtime target. ImGui's GL3 backend has its own
 * (private) loader and is unaffected. */
#include <SDL_opengl.h>
#if defined(__APPLE__)
#  include <OpenGL/gl.h>
#else
#  include <GL/gl.h>
#endif
extern "C" {
    void glBindFramebuffer(GLenum target, GLuint framebuffer);
    void glReadBuffer(GLenum mode);
    void glDrawBuffer(GLenum mode);
    void glPixelStorei(GLenum pname, GLint param);
    void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                      GLenum format, GLenum type, void *pixels);
    GLenum glGetError(void);
}

#include <cstdint>
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

/* Launcher-equivalent video/audio setters defined in main.cpp (extern "C"
 * there). turbo loads is a direct extern "C" global, same as debug_server. */
extern "C" int  psx_video_get_supersampling(void);
extern "C" void psx_video_set_supersampling(int s);
extern "C" int  psx_video_get_antialiasing(void);
extern "C" void psx_video_set_antialiasing(int on);
extern "C" int  psx_video_get_screen_model(void);
extern "C" void psx_video_set_screen_model(int k);
extern "C" int  psx_audio_get_spu_hq(void);
extern "C" void psx_audio_set_spu_hq(int on);
extern "C" int  psx_video_get_window_width(void);
extern "C" void psx_video_set_window_width(int w);
extern "C" int  g_turbo_loads_enabled;
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

/* State for the three new panels (teleport / party / gold+vars). */
static int      s_teleport_field_id   = 1;
static int      s_teleport_entry      = 0;
static int      s_teleport_filter_sel = -1;
static char     s_teleport_filter[64] = {0};
static char     s_teleport_status[96] = {0};
static int      s_teleport_status_frames = 0;
static int      s_last_teleport_id   = -1;
static int      s_last_teleport_tick = 0;

static int      s_party_slot[3]      = {0, 1, 2};
static int      s_party_bitfield     = 0x07FF;
static bool     s_party_roster_show  = true;
static bool     s_party_unlock[11]   = { true, true, true, true, true,
                                         true, true, true, true, true, true };

static int      s_gold_value         = 0;
static int      s_gold_dirty         = 0;
static int      s_vars_filter_sel    = -1;
static char     s_vars_filter[64]    = {0};
static int      s_var_edit[512]      = {0};

/* State for the Force Battle panel (W6). The encounter-trigger address
 * 0x800B2298 is verified-by-reference (the reference's validation hook
 * writes it to 0 to disable encounters for deterministic replay). What
 * writing a non-zero does is HYPOTHESIZED from the reference's logic
 * (`playMusicAuthorized != 0` is one of the gates for the encounter
 * countdown to fire), but the live offset of the other encounter
 * countdown vars (g_encounterTimer / g_encounterDataCountdown /
 * g_encounterTriggerTime[32]) is not in the reference's address book,
 * so the panel ships the gate write + extensive readouts + a clear
 * "best-effort: depends on field encounter data" status — see
 * draw_battle_section for the exact contract. */
static int      s_battle_scene_id    = 0;
static int      s_battle_arena_id    = 0;
static int      s_battle_trigger_val = 1;     /* value written to 0x800B2298 */
static char     s_battle_status[96]  = {0};
static int      s_battle_status_frames = 0;

/* State for the Free Camera panel (W6). When `s_camera_enabled` is true,
 * pre_swap() writes s_camera_eye / s_camera_at as 3 x s16 LE triplets to
 * the verified guest addresses every frame. The write is guarded by the
 * same field-module-active check teleport uses (only meaningful while
 * the field module owns the camera; on the title/transition screens the
 * writes are still issued but the field poll overwrites them next frame
 * — visible, predictable, no crash). */
static bool     s_camera_enabled     = false;
static bool     s_camera_keys_enable = true;
static int      s_camera_eye[3]      = {0, 0, 0};   /* PSX s16 (x,y,z) */
static int      s_camera_at[3]       = {0, 0, 100}; /* PSX s16, at.z+100 keeps it forward */
static float    s_camera_fly_speed   = 32.0f;       /* PSX units per frame */
static float    s_camera_rot_speed   = 0.05f;       /* rad per arrow-key frame */
static int      s_camera_status_frames = 0;
static char     s_camera_status[64]  = {0};

/* State for the Event Jump panel (W6). The event list is loaded by
 * dbg_data_events() from events.xml. Verified events have a non-greyed
 * Jump button; unverified events are shown but disabled. */
static int      s_event_filter_sel   = -1;
static char     s_event_filter[64]   = {0};
static int      s_event_jump_status_frames = 0;
static char     s_event_jump_status[128] = {0};

/* ---- small helpers ------------------------------------------------------ */

/* Read the back buffer of the current default framebuffer to a top-down
 * RGB uint8_t* buffer (caller frees with std::free). On any GL error, the
 * returned buffer is empty (size 0) and the caller is expected to skip the
 * PNG write. The path was deliberately implemented as a freestanding helper
 * so the hidden+armed path can use it without dragging in ImGui. */
static uint8_t *capture_window_rgb(int *out_w, int *out_h)
{
    *out_w = 0;
    *out_h = 0;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return nullptr;

    /* Defensive rebind: every present path leaves some FBO bound, so the
     * readback would otherwise target the last-bound FBO (e.g. the wide
     * FBO in native-wide mode). Bind both DRAW and READ to 0 so
     * glReadPixels reads the default framebuffer's back buffer, and so
     * the ImGui frame path that follows (if visible) draws into the same. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    /* The back buffer is the natural read target. On drivers that have
     * already presented (shouldn't happen at pre_swap but defensive), the
     * front buffer is also readable. */
    glReadBuffer(GL_BACK);

    const size_t row_bytes = (size_t)ww * 3;
    uint8_t *rgb = (uint8_t *)std::malloc(row_bytes * (size_t)wh);
    if (!rgb) return nullptr;
    /* GL_PACK_ALIGNMENT=1 prevents row padding for our tight RGB rows. */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::free(rgb);
        return nullptr;
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

    int internal_scale = gr_scale();
    ImGui::Text("Internal scale  : %dx",
                internal_scale > 0 ? internal_scale : 1);

    ImGui::Text("Texture filter  : %s",
                texfilter_label(gr_texture_filter()));

    /* Same accessors the TCP gpu_state handler uses (gpu.h). */
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    GpuDrawArea da;
    gpu_get_draw_area(&da);
    uint32_t hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0, hr1 = 0, hr2 = 0;
    gpu_get_crtc_debug(&hx1, &hx2, &hy1, &hy2, &hr1, &hr2);
    ImGui::Text("Display         : %ux%u  (depth=%d, %s)",
                (unsigned)di.width, (unsigned)di.height,
                di.depth24 ? 24 : 15,
                di.disabled ? "disabled" : "enabled");
    ImGui::Text("Display range   : x=[%u..%u] y=[%u..%u]",
                hx1, hx2, hy1, hy2);
    ImGui::Text("Draw area       : (%u,%u)..(%u,%u)  off=(%d,%d)",
                da.left, da.top, da.right, da.bottom, da.offset_x, da.offset_y);

    /* ws.xnum/xden mirror the TCP ws_aspect handler's response field. */
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    if (ws.xden > 0) {
        ImGui::Text("Aspect          : %d/%d  (squash mode %d)",
                    ws.xnum, ws.xden, ws.mode);
    } else {
        ImGui::Text("Aspect          : (unset)");
    }

    ImGui::Text("Native wide     : %s  (extra=%d)",
                psx_ws_get_native_wide() ? "on" : "off", ws.nw_extra);

    ImGui::Text("BD stretch      : on=%d pct=%d",
                g_ws_bd_stretch_on, g_ws_bd_stretch_pct);

    {
        /* Same diag helper the TCP gl_interp handler uses. */
        int en = 0, sus = 0, hist = 0;
        double hh = 0.0, th = 0.0;
        uint64_t swaps = 0;
        gl_renderer_interpolation_diag(&en, &sus, &hist, &hh, &th, &swaps);
        ImGui::Text("Interp          : %s  host=%.2fHz target=%.2fHz swaps=%llu",
                    en ? "on" : "off", hh, th,
                    (unsigned long long)swaps);
    }

    {
        /* all[1] is average total_ms per frame; same array the TCP
         * frame_perf handler reads. */
        double all[18] = {0};
        int n = gl_renderer_perf_aggregate(-1, all);
        if (n > 0 && all[1] > 0.0) {
            double fps = 1000.0 / all[1];
            ImGui::Text("FPS (avg)       : %.2f  (n=%d)", fps, n);
        } else {
            ImGui::Text("FPS (avg)       : (no GL samples yet)");
        }
    }

    ImGui::Text("Frame           : %llu",
                (unsigned long long)s_frame_count);
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

static void draw_toggles_section(void)
{
    bool tf = gr_texture_filter() != 0;
    if (ImGui::Checkbox("Texture filter (bilinear)", &tf)) {
        gr_set_texture_filter(tf ? 1 : 0);
    }

    /* Read live xnum/xden from gpu_ws_get_debug so the widget always
     * shows the runtime's actual state. */
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    int anum = (ws.xden > 0) ? ws.xnum : s_aspect_num;
    int aden = (ws.xden > 0) ? ws.xden : s_aspect_den;
    /* Read the live GTE aspect so the widget matches what the TCP
     * gte_get_display_aspect would return (single source of truth). */
    gte_get_display_aspect(&anum, &aden);
    if (ImGui::SliderInt("Aspect num", &anum, 1, 32)) {
        s_aspect_num = anum;
        gte_set_display_aspect_ex(anum, aden > 0 ? aden : 3);
    }
    if (ImGui::SliderInt("Aspect den", &aden, 1, 32)) {
        s_aspect_den = aden;
        gte_set_display_aspect_ex(anum > 0 ? anum : 4, aden);
    }
    if (ImGui::Button("Aspect 4:3 (squash off)")) {
        s_aspect_num = 4; s_aspect_den = 3;
        gte_set_display_aspect_ex(4, 3);
    }
    ImGui::SameLine();
    if (ImGui::Button("Aspect 16:9")) {
        s_aspect_num = 16; s_aspect_den = 9;
        gte_set_display_aspect_ex(16, 9);
    }

    /* Native-wide on/off — read from psx_ws_get_native_wide. */
    int nw = psx_ws_get_native_wide();
    bool nw_on = nw > 0;
    if (ImGui::Checkbox("Native wide (on/off/squash 0/1/2)", &nw_on)) {
        psx_ws_set_native_wide(nw_on ? 1 : 0);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("off"))   psx_ws_set_native_wide(0);
    ImGui::SameLine();
    if (ImGui::SmallButton("squash")) psx_ws_set_native_wide(1);
    ImGui::SameLine();
    if (ImGui::SmallButton("native")) psx_ws_set_native_wide(2);

    /* Backdrop stretch — on/pct are file-scope statics in gpu_gl_renderer.c
     * (externed above). The widget writes them directly; the next present
     * path picks them up. */
    bool bds = g_ws_bd_stretch_on != 0;
    if (ImGui::Checkbox("BD stretch on", &bds)) {
        g_ws_bd_stretch_on = bds ? 1 : 0;
    }
    int bdp = g_ws_bd_stretch_pct;
    if (ImGui::SliderInt("BD stretch pct (0=auto)", &bdp, 0, 200)) {
        g_ws_bd_stretch_pct = bdp;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Launcher settings:");

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

    bool tl = g_turbo_loads_enabled != 0;
    if (ImGui::Checkbox("Turbo loads (unpaced during CD loads)", &tl)) {
        g_turbo_loads_enabled = tl ? 1 : 0;
    }

    bool hq = psx_audio_get_spu_hq() != 0;
    if (ImGui::Checkbox("High-quality SPU (float shadow)", &hq)) {
        psx_audio_set_spu_hq(hq ? 1 : 0);
    }
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
 * 4/2 sequential byte writes (little-endian). The engine's field poll
 * picks up the teleport recipe from fieldMapNumber (0x8004F34C) +
 * fieldVars[1] (0x8006EF66) on the next frame; we do NOT touch
 * fieldID (0x8006F94E) — it's a cosmetic mirror and writing it
 * corrupts texture streaming. We do NOT call loadNewField (not
 * reentrant — the poll is the only correct caller). */
static constexpr uint32_t kAddr_fieldContextPtr     = 0x800B0078u;
static constexpr uint32_t kAddr_fieldID             = 0x8006F94Eu;
static constexpr uint32_t kAddr_fieldMapNumber      = 0x8004F34Cu;
static constexpr uint32_t kAddr_fieldChangePrevented= 0x800ADB64u;
static constexpr uint32_t kAddr_teleportGate1       = 0x800ADBECu;
static constexpr uint32_t kAddr_teleportArm         = 0x800ADBC4u;
static constexpr uint32_t kAddr_teleportGateMusic   = 0x8004F308u;
static constexpr uint32_t kAddr_teleportGateAnim    = 0x800ADB90u;
static constexpr uint32_t kAddr_fieldEntryPointU16  = 0x8006EF66u;
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
static constexpr uint32_t kAddr_gold                = 0x8006EF58u;
static constexpr uint32_t kAddr_fieldVarsBase       = 0x8006EF64u;
static constexpr uint32_t kAddr_partyRosterBase     = 0x8006D8A0u;
/* Camera SVECTORs (3 x s16 LE) — verified-static by addrs.xml +
 * reference validation (validateFieldEntities.cpp lines 228-233). */
static constexpr uint32_t kAddr_cameraEye           = 0x800AF880u;
static constexpr uint32_t kAddr_cameraAt            = 0x800AF890u;
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

/* True when the field module is the active module. The strongest
 * signal: the field context pointer is non-zero (set by field-module
 * init, cleared when the field module is left). The field poll
 * (0x800784A0) only runs while the field context is live, so when
 * this is zero the recipe writes will be ignored and a "teleport"
 * would silently no-op. Title screen (field 490) lives in the field
 * module — verified live. */
static bool field_module_active(void)
{
    return read_u32_le(kAddr_fieldContextPtr) != 0u;
}

/* Public read accessor used by the panel + tests. */
int psx_debug_overlay_read_field_id(void)
{
    if (read_u32_le(kAddr_fieldContextPtr) == 0u) return -1;
    return (int)read_u16_le(kAddr_fieldID);
}

int psx_debug_overlay_teleport(int fieldId, int entryPoint)
{
    if (!field_module_active()) {
        return 1;
    }
    write_u32_le(kAddr_teleportGate1,        0u);
    write_u32_le(kAddr_fieldChangePrevented, 0u);
    write_u32_le(kAddr_fieldMapNumber,      (uint32_t)fieldId);
    write_u16_le(kAddr_fieldEntryPointU16,  (uint16_t)((unsigned)entryPoint & 0xFFFFu));
    write_u32_le(kAddr_teleportGateMusic,    0u);
    write_u32_le(kAddr_teleportGateAnim,     0u);
    write_u32_le(kAddr_teleportArm,          0xFFu);
    s_last_teleport_id = fieldId;
    s_last_teleport_tick = 0;
    return 0;
}

int psx_debug_overlay_write_party_slot(int slot, int charId, int bitfieldBit)
{
    if (slot < 0 || slot > 2) return -1;
    if (charId < 0 || charId > 0xFF) return -2;
    /* The camp menu builds its member list from the unlock bitfield, and a
     * party member whose bit is clear crashes field loading (user-verified:
     * char followed in field, absent from menu, died on next screen change;
     * also user-verified that writing the slot BEFORE the bitfield crashes
     * while bitfield-first works — the game validates party state against
     * the bitfield on frames in between). So: compute the final bitfield
     * (OR every member including the about-to-be-written one, never clear
     * bits) and write it FIRST, then write the slot. */
    uint16_t bf = read_u16_le(kAddr_partyBitfield);
    if (charId < 11) bf |= (uint16_t)(1u << charId);
    for (int s = 0; s < 3; s++) {
        if (s == slot) continue;
        uint32_t id = read_u32_le(kAddr_kernelPartySlots + (uint32_t)s * 4u) & 0xFFu;
        if (id < 11u) bf |= (uint16_t)(1u << id);
    }
    if (bitfieldBit >= 0 && bitfieldBit < 16) {
        bf |= (uint16_t)(1u << bitfieldBit);
    }
    write_u16_le(kAddr_partyBitfield, bf);
    write_u32_le(kAddr_kernelPartySlots + (uint32_t)slot * 4u, (uint32_t)charId);
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
    write_u32_le(kAddr_encounterTrigger, (uint32_t)value);
    return 0;
}

/* Free-camera write. Writes 3 x s16 LE to cameraEye (0x800AF880) and
 * cameraAt (0x800AF890). Both addresses are verified-static (addrs.xml
 * + reference validation at 0x800af880/0x800af890). The write is
 * issued unconditionally — callers (the per-frame pre_swap loop, the
 * in-window "Apply" button, the TCP `camera_write` action) decide
 * whether the camera should be overridden. Coords are clamped to the
 * PS1 s16 range to avoid a silent wrap on a fat-finger. Returns 0 on
 * success, negative on bad value. */
int psx_debug_overlay_camera_write(int ex, int ey, int ez, int ax, int ay, int az)
{
    auto clamp_s16 = [](int v) -> int16_t {
        if (v < -32768) return (int16_t)-32768;
        if (v >  32767) return (int16_t) 32767;
        return (int16_t)v;
    };
    int16_t e[3] = { clamp_s16(ex), clamp_s16(ey), clamp_s16(ez) };
    int16_t a[3] = { clamp_s16(ax), clamp_s16(ay), clamp_s16(az) };
    for (int i = 0; i < 3; i++) write_u16_le(kAddr_cameraEye + (uint32_t)i * 2u, (uint16_t)e[i]);
    for (int i = 0; i < 3; i++) write_u16_le(kAddr_cameraAt  + (uint32_t)i * 2u, (uint16_t)a[i]);
    return 0;
}

/* Event Jump: apply the event's varWrites (via psx_write_byte to
 * fieldVars, same path as the W5 vars editor) then fire the verified
 * 7-write teleport recipe. `eventId` is the index into the events
 * table loaded from events.xml. Returns 0 on success, positive on
 * teleport-refused (field module not active), negative on bad id. */
int psx_debug_overlay_event_jump(int eventId)
{
    int ne = 0;
    const DbgEvent *evs = dbg_data_events(&ne);
    if (evs == nullptr || eventId < 0 || eventId >= ne) return -1;
    const DbgEvent &e = evs[eventId];
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
    int cur = psx_debug_overlay_read_field_id();
    const char *cur_name = nullptr;
    bool cur_known = (cur >= 0) && field_name_known(cur, &cur_name);
    ImGui::Text("Current field: %s%d%s%s",
                cur >= 0 ? "" : "?",
                cur >= 0 ? cur : 0,
                cur_known ? "  (" : "",
                cur_known ? cur_name : (cur >= 0 ? "  (unknown)" : ""));
    if (!field_module_active()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "Field module NOT active - teleport will be refused.");
    } else {
        ImGui::TextDisabled("Field module active - teleport is armed by recipe only.");
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
    if (s_teleport_field_id > 2047) s_teleport_field_id = 2047;
    ImGui::InputInt("Entry point", &s_teleport_entry, 1, 10);

    {
        const char *name = nullptr;
        if (field_name_known(s_teleport_field_id, &name)) {
            ImGui::Text("Target name: %s", name);
        } else {
            ImGui::TextDisabled("Target name: (unknown id)");
        }
    }

    bool can_tp = field_module_active();
    if (!can_tp) ImGui::BeginDisabled();
    if (ImGui::Button("Teleport")) {
        int rc = psx_debug_overlay_teleport(s_teleport_field_id, s_teleport_entry);
        if (rc == 0) {
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport armed -> field %d, entry %d (poll engages next frame)",
                s_teleport_field_id, s_teleport_entry);
            s_teleport_status_frames = 90; /* ~1.5s at 60fps */
        } else {
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport refused (rc=%d) - field module not active", rc);
            s_teleport_status_frames = 90;
        }
    }
    if (!can_tp) ImGui::EndDisabled();
    if (!can_tp) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Disabled: field module not active (fieldContextPtr 0x800B0078 == 0).");
        }
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Title (490)")) {
        s_teleport_field_id = 490;
        s_teleport_entry    = 0;
        if (field_module_active()) {
            int rc = psx_debug_overlay_teleport(490, 0);
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport armed -> field 490 title (rc=%d)", rc);
            s_teleport_status_frames = 90;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Debug room (0)")) {
        s_teleport_field_id = 0;
        s_teleport_entry    = 0;
        if (field_module_active()) {
            int rc = psx_debug_overlay_teleport(0, 0);
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport armed -> field 0 debug (rc=%d)", rc);
            s_teleport_status_frames = 90;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Lahan (1)")) {
        s_teleport_field_id = 1;
        s_teleport_entry    = 0;
        if (field_module_active()) {
            int rc = psx_debug_overlay_teleport(1, 0);
            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                "Teleport armed -> field 1 Lahan (rc=%d)", rc);
            s_teleport_status_frames = 90;
        }
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
                        if (field_module_active()) {
                            int rc = psx_debug_overlay_teleport(fields[i].id, s_teleport_entry);
                            std::snprintf(s_teleport_status, sizeof(s_teleport_status),
                                "Teleport armed -> %d %s (rc=%d)",
                                fields[i].id, fields[i].name ? fields[i].name : "", rc);
                            s_teleport_status_frames = 90;
                        }
                    }
                }
            }
        }
        ImGui::EndChild();
    }

    if (s_last_teleport_tick >= 0) s_last_teleport_tick++;
}

/* ---- Party editor UI --------------------------------------------------- */

static void draw_party_section(void)
{
    int nc = 0;
    const DbgCharacter *chars = dbg_data_characters(&nc);

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

    if (ImGui::Button("Write party to RAM")) {
        for (int s = 0; s < 3; s++) {
            psx_debug_overlay_write_party_slot(s, s_party_slot[s], -1);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Writes 3 u32 slots to 0x80062590 (kernel master;"
                          " gameState 0x8006F368 follows next frame).");
    }

    ImGui::Separator();
    ImGui::Text("Unlock bitfield (0x8006F364, 11 bits):");
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
    if (ImGui::CollapsingHeader("Roster (partyRoster, 0x8006D8A0, 0xA4 stride)",
                                &s_party_roster_show)) {
        if (ImGui::BeginTable("roster", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("unlocked");
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
                char hex[64] = {0};
                int pos = 0;
                uint32_t base = kAddr_partyRosterBase + (uint32_t)i * (uint32_t)kPartyRosterStride;
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
}

/* ---- Force Battle panel (W6) ------------------------------------------- */

static void draw_battle_section(void)
{
    /* Live encounter-gate state — same accessor the TCP `read_ram` +
     * Force Battle action uses, so the widget and the TCP view agree
     * on what's in RAM right now. */
    uint32_t gate_now = read_u32_le(kAddr_encounterTrigger);
    ImGui::Text("Encounter gate (0x800B2298): 0x%08X  (%u)",
                (unsigned)gate_now, (unsigned)gate_now);
    ImGui::TextDisabled(
        "Reference-verified: 0=disabled, non-zero=armed (per playMusicAuthorized gate).");
    ImGui::TextDisabled(
        "Actual battle firing still requires field encounter data + countdown = 0.");

    /* Party preset (read-only) — currentParty[0..2] + bitfield. Same
     * accessors the W5 Party panel uses, so the user can cross-check
     * the battle's party against the in-window Party editor. */
    {
        uint8_t p0 = psx_read_byte(kAddr_currentParty + 0);
        uint8_t p1 = psx_read_byte(kAddr_currentParty + 1);
        uint8_t p2 = psx_read_byte(kAddr_currentParty + 2);
        uint16_t bf = read_u16_le(kAddr_partyBitfield);
        ImGui::Text("Party (live): [%02X, %02X, %02X]  bitfield=0x%04X",
                    (unsigned)p0, (unsigned)p1, (unsigned)p2, (unsigned)bf);
    }

    ImGui::Separator();
    ImGui::InputInt("Battle scene id (display only)", &s_battle_scene_id);
    ImGui::InputInt("Battle arena id (display only)", &s_battle_arena_id);
    ImGui::TextDisabled(
        "Live offset of the chosen battleConfig is not in the reference address book; "
        "these inputs are captured for future live discovery and the W6 manual script.");

    /* Trigger value: what to write to 0x800B2298. 0 = disable (sanity
     * test), 1 = arm (default). */
    ImGui::InputInt("Trigger value (write to 0x800B2298)",
                    &s_battle_trigger_val);
    if (s_battle_trigger_val < 0)     s_battle_trigger_val = 0;
    if (s_battle_trigger_val > 0xFFFF) s_battle_trigger_val = 0xFFFF;

    bool can_arm = field_module_active();
    if (!can_arm) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "Field module NOT active — battle sequence will not engage. "
            "You can still arm the gate (writes to 0x800B2298) for inspection.");
    }
    if (!can_arm) ImGui::BeginDisabled();
    if (ImGui::Button("Start Battle (arm encounter gate)")) {
        int rc = psx_debug_overlay_force_battle(s_battle_trigger_val);
        uint32_t post = read_u32_le(kAddr_encounterTrigger);
        if (rc == 0) {
            std::snprintf(s_battle_status, sizeof(s_battle_status),
                "Encounter gate armed -> 0x800B2298=0x%08X (post=%u). Battle will fire when field + countdown align.",
                (unsigned)s_battle_trigger_val, (unsigned)post);
        } else {
            std::snprintf(s_battle_status, sizeof(s_battle_status),
                "force_battle refused (rc=%d).", rc);
        }
        s_battle_status_frames = 120;
    }
    if (!can_arm) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Disable (write 0)")) {
        int rc = psx_debug_overlay_force_battle(0);
        if (rc == 0) {
            std::snprintf(s_battle_status, sizeof(s_battle_status),
                "Encounter gate disabled -> 0x800B2298=0.");
        }
        s_battle_status_frames = 120;
    }

    if (s_battle_status_frames > 0 && s_battle_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_battle_status);
        s_battle_status_frames--;
    }

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

/* Apply fly-control updates to s_camera_eye / s_camera_at based on the
 * current SDL keyboard state. Only called when s_visible is true (so
 * the user's pad input is already masked to the overlay per the W5
 * swallow_keyboard guard) AND s_camera_keys_enable is on (lets the
 * user disable fly keys while keeping the per-frame override on).
 * The keys do NOT conflict with the game's input because the pad
 * mask is active whenever the overlay is visible (per T13 only widget
 * captures fight the mask; the WASD/E/Q/arrow set never reaches
 * ImGui's keyboard state — the game's pad sampler reads SDL directly
 * and is masked by psx_debug_overlay_swallow_keyboard). */
static void apply_camera_fly_keys(void)
{
    if (!s_camera_keys_enable) return;
    const Uint8 *ks = SDL_GetKeyboardState(nullptr);
    if (!ks) return;

    /* Read current eye/at as floats (PSX s16 are short — values up to
     * ~32k, well within float precision). The slider inputs already
     * use int; we round-trip through int to keep state in sync. */
    float ex = (float)s_camera_eye[0];
    float ey = (float)s_camera_eye[1];
    float ez = (float)s_camera_eye[2];
    float ax = (float)s_camera_at[0];
    float ay = (float)s_camera_at[1];
    float az = (float)s_camera_at[2];

    /* Forward = normalize(at - eye). Yaw-only — pitch is intentionally
     * not exposed (the field camera matrix is yaw-dominant and pitch
     * can clip the geometry). The view direction defaults to +Z when
     * at == eye + (0,0,1) (e.g. after the first enable). */
    float dx = ax - ex, dy = ay - ey, dz = az - ez;
    float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (dl < 0.001f) { dx = 0.0f; dy = 0.0f; dz = 1.0f; dl = 1.0f; }
    float fx = dx / dl, fy = dy / dl, fz = dz / dl;
    /* Right = forward x up. With up = (0,1,0), right is (fz, 0, -fx). */
    float rx = fz, rz = -fx;
    float rlen = std::sqrt(rx*rx + rz*rz);
    if (rlen < 0.001f) { rx = 1.0f; rz = 0.0f; rlen = 1.0f; }
    rx /= rlen; rz /= rlen;

    float spd = s_camera_fly_speed;
    if (ks[SDL_SCANCODE_W]) { ex += fx*spd; ey += fy*spd; ez += fz*spd; ax += fx*spd; ay += fy*spd; az += fz*spd; }
    if (ks[SDL_SCANCODE_S]) { ex -= fx*spd; ey -= fy*spd; ez -= fz*spd; ax -= fx*spd; ay -= fy*spd; az -= fz*spd; }
    if (ks[SDL_SCANCODE_D]) { ex += rx*spd; ez += rz*spd; ax += rx*spd; az += rz*spd; }
    if (ks[SDL_SCANCODE_A]) { ex -= rx*spd; ez -= rz*spd; ax -= rx*spd; az -= rz*spd; }
    if (ks[SDL_SCANCODE_E]) { ey += spd; ay += spd; }
    if (ks[SDL_SCANCODE_Q]) { ey -= spd; ay -= spd; }
    /* Arrows rotate `at` around eye in the XZ plane (yaw only). */
    float rot = s_camera_rot_speed;
    if (ks[SDL_SCANCODE_LEFT])  {
        float c = std::cos(rot), s = std::sin(rot);
        float ndx = dx*c - dz*s;
        float ndz = dx*s + dz*c;
        ax = ex + ndx; az = ez + ndz;
        dx = ndx; dz = ndz;
    }
    if (ks[SDL_SCANCODE_RIGHT]) {
        float c = std::cos(-rot), s = std::sin(-rot);
        float ndx = dx*c - dz*s;
        float ndz = dx*s + dz*c;
        ax = ex + ndx; az = ez + ndz;
        dx = ndx; dz = ndz;
    }
    /* Up/Down arrows tilt the at vector vertically (pitch around the
     * right axis). */
    if (ks[SDL_SCANCODE_UP])    { ay += spd; }
    if (ks[SDL_SCANCODE_DOWN])  { ay -= spd; }

    /* Round-trip back to int (PSX s16). Clamp happens inside the write
     * helper. */
    s_camera_eye[0] = (int)ex; s_camera_eye[1] = (int)ey; s_camera_eye[2] = (int)ez;
    s_camera_at[0]  = (int)ax; s_camera_at[1]  = (int)ay; s_camera_at[2]  = (int)az;
}

static void draw_camera_section(void)
{
    bool en = s_camera_enabled;
    if (ImGui::Checkbox("Enable free camera (writes eye+at every frame)",
                        &en)) {
        s_camera_enabled = en;
        std::snprintf(s_camera_status, sizeof(s_camera_status),
            s_camera_enabled ? "Free camera ENABLED."
                              : "Free camera DISABLED — game camera resumes.");
        s_camera_status_frames = 60;
    }
    if (s_camera_status_frames > 0 && s_camera_status[0]) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "%s", s_camera_status);
        s_camera_status_frames--;
    }
    if (!field_module_active()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "Field module NOT active — writes are still issued but the field "
            "poll will overwrite them. Use the field context to see the effect.");
    }

    ImGui::Checkbox("Enable fly keys (W/A/S/D move, Q/E up/down, arrows look)",
                    &s_camera_keys_enable);

    /* Read live eye/at from guest RAM (same accessors used in the
     * RAM Inspector). The sliders are 3-way (x/y/z). */
    int live_eye[3] = {
        (int)(int16_t)read_u16_le(kAddr_cameraEye + 0),
        (int)(int16_t)read_u16_le(kAddr_cameraEye + 2),
        (int)(int16_t)read_u16_le(kAddr_cameraEye + 4)
    };
    int live_at[3] = {
        (int)(int16_t)read_u16_le(kAddr_cameraAt + 0),
        (int)(int16_t)read_u16_le(kAddr_cameraAt + 2),
        (int)(int16_t)read_u16_le(kAddr_cameraAt + 4)
    };
    ImGui::Text("Live eye (0x800AF880): (%d, %d, %d)",
                live_eye[0], live_eye[1], live_eye[2]);
    ImGui::Text("Live at  (0x800AF890): (%d, %d, %d)",
                live_at[0], live_at[1], live_at[2]);

    if (ImGui::Button("Pull live -> editor")) {
        for (int i = 0; i < 3; i++) s_camera_eye[i] = live_eye[i];
        for (int i = 0; i < 3; i++) s_camera_at[i]  = live_at[i];
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply (write once)")) {
        psx_debug_overlay_camera_write(
            s_camera_eye[0], s_camera_eye[1], s_camera_eye[2],
            s_camera_at[0],  s_camera_at[1],  s_camera_at[2]);
        std::snprintf(s_camera_status, sizeof(s_camera_status),
            "Camera written (one-shot).");
        s_camera_status_frames = 60;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset at +Z")) {
        s_camera_at[0] = s_camera_eye[0];
        s_camera_at[1] = s_camera_eye[1];
        s_camera_at[2] = s_camera_eye[2] + 100;
    }

    ImGui::SliderInt3("eye (x,y,z)", s_camera_eye, -32768, 32767);
    ImGui::SliderInt3("at  (x,y,z)", s_camera_at,  -32768, 32767);
    ImGui::SliderFloat("Fly speed (PSX units/frame)",
                       &s_camera_fly_speed, 1.0f, 256.0f);
    ImGui::SliderFloat("Yaw speed (rad/frame)",
                       &s_camera_rot_speed, 0.0f, 0.5f);
    ImGui::TextDisabled(
        "When enabled, the overlay writes s16 LE triplets to 0x800AF880 "
        "and 0x800AF890 every pre_swap. Disable to let the game camera "
        "regain control on the next frame.");
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
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "Field module NOT active — Jump buttons will be refused.");
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
            if (!e.verified) {
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
            bool can_jump = e.verified && field_module_active();
            if (!can_jump) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Jump")) {
                int rc = psx_debug_overlay_event_jump(e.id);
                if (rc == 0) {
                    std::snprintf(s_event_jump_status,
                        sizeof(s_event_jump_status),
                        "Event %d armed: map=%d entry=%d, %d varWrite(s) applied.",
                        e.id, e.mapId, e.entryPoint, e.numVarWrites);
                } else {
                    std::snprintf(s_event_jump_status,
                        sizeof(s_event_jump_status),
                        "Event %d refused (rc=%d) - field module not active?",
                        e.id, rc);
                }
                s_event_jump_status_frames = 120;
            }
            if (!can_jump) ImGui::EndDisabled();
            if (!e.verified && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Unverified: status != \"verified\" in events.xml. "
                                  "Greyed out until Ghidra validation.");
            } else if (!field_module_active() &&
                       ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Field module not active (fieldContextPtr 0x800B0078 == 0).");
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



void psx_debug_overlay_init(struct SDL_Window *win, SDL_GLContext ctx)
{
    s_win = win;
    s_imgui_ready = false;
    s_window_shot_armed = false;
    s_window_shot_path[0] = '\0';
    (void)ctx; /* ctx is NULL by design — see file header */
}

void psx_debug_overlay_shutdown(void)
{
    if (s_imgui_ready) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        s_imgui_ready = false;
    }
    s_win = nullptr;
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
            gl_renderer_set_interpolation(0, host_hz, target_hz);
        }
    } else if (!s_visible && was_visible && s_interp_guard_active) {
        s_interp_guard_active = false;
        int enabled = 0, suspended = 0, history = 0;
        double host_hz = 0.0, target_hz = 0.0;
        uint64_t swaps = 0;
        gl_renderer_interpolation_diag(&enabled, &suspended, &history,
                                       &host_hz, &target_hz, &swaps);
        gl_renderer_set_interpolation(1, host_hz, target_hz);
    }
}

bool psx_debug_overlay_is_visible(void)
{
    return s_visible;
}

bool psx_debug_overlay_process_event(const SDL_Event *ev)
{
    if (!ev) return false;

    /* Feed every SDL event into ImGui's SDL2 backend so its key state,
     * mouse motion, and text-input composition stay current. Gated on
     * s_imgui_ready to avoid touching a dead context; harmless when hidden
     * (ImGui just queues events internally — no render). The Ctrl+F3
     * consume check below is unchanged. */
    if (s_imgui_ready) {
        ImGui_ImplSDL2_ProcessEvent(ev);
    }

    if (ev->type != SDL_KEYDOWN) return false;
    /* Ctrl+F3 toggles the overlay. The keysym is F3 AND Ctrl is held;
     * plain F3 is the savestate load slot 2 hotkey in main.cpp, so
     * main.cpp consults this function before its savestate block and
     * skips the rest of the event when we return true. */
    if (ev->key.keysym.sym == SDLK_F3 &&
        (ev->key.keysym.mod & KMOD_CTRL)) {
        psx_debug_overlay_toggle();
        return true;
    }
    return false;
}

bool psx_debug_overlay_swallow_keyboard(void)
{
    /* The runtime's pad sampler polls SDL_GetKeyboardState every frame —
     * WantCaptureKeyboard alone does NOT stop that, so we MASK it. The
     * mask is only armed when the overlay is visible AND ImGui reports it
     * wants the keyboard (a text field is being edited, or nav is active
     * via the keyboard). When ImGui is not ready yet (first frame the
     * overlay is opened before lazy init), fall back to visible-only: the
     * game hands us the keyboard as soon as Ctrl+F3 is pressed, which is
     * the same behavior the user expects. Cheap: a bool + a static fn
     * pointer; no allocation, no syscall. */
    if (!s_visible) return false;
    if (!s_imgui_ready) return true;
    return ImGui::GetIO().WantCaptureKeyboard;
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
 *      save+restore, but NOT the FBO binding — hence the defensive rebind
 *      in capture_window_rgb (which the visible path also calls for any
 *      pending window_shot — see below).
 *   3. If window_shot is armed: read the back buffer (composited, with
 *      overlay if visible) and write the PNG. Done AFTER RenderDrawData
 *      so the shot includes the overlay's pixels.
 *
 * Hidden + unarmed = pure no-op (zero GL work, zero state leak).
 */
void psx_debug_overlay_pre_swap(void)
{
    /* Step 1: lazy init. */
    if (!s_imgui_ready) {
        if (!s_win) return; /* not initialized at all — try next frame */
        SDL_GLContext ctx = SDL_GL_GetCurrentContext();
        if (!ctx) return;   /* main context not current yet — try next frame */

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        /* DockingEnable / ViewportsEnable are not present in the vendored
         * ImGui 1.91.9b "master" build (docking branch only); leave
         * ConfigFlags at the default zero and let the minimal window
         * stand alone. */
        io.IniFilename = "debug_overlay.ini";

        ImGui::StyleColorsDark();
        /* Larger UI than the 13px default — the window is read at couch
         * distance over the game. Font must be added before the GL backend
         * init builds the atlas texture. */
        {
            ImFontConfig font_cfg;
            font_cfg.SizePixels = 19.0f;
            io.Fonts->AddFontDefault(&font_cfg);
            ImGui::GetStyle().ScaleAllSizes(1.25f);
        }

        if (!ImGui_ImplSDL2_InitForOpenGL(s_win, ctx)) {
            /* Init failed — roll back and try again next frame. */
            ImGui::DestroyContext();
            return;
        }
        /* "#version 330 core" matches the GL context the runtime creates
         * (gpu_gl_renderer.c creates a core 3.3 context). */
        if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            return;
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

    /* Step 1c: free-camera per-frame write. Runs BEFORE the ImGui
     * frame so the live-readout in the panel reflects the just-written
     * values this frame (the user can see the value change right after
     * they tick the Enable box). Runs regardless of overlay visibility
     * — when enabled, the write continues even if the user closes the
     * window, which is the whole point: a "fly around while playing"
     * mode. When disabled: stop writing, the field poll reclaims the
     * camera the next frame. The guard (field module active) is the
     * same one teleport uses — when the field module is not the active
     * module, the writes are still issued but the field poll will
     * overwrite them on the next field frame. */
    if (s_camera_enabled) {
        if (s_visible) apply_camera_fly_keys();
        psx_debug_overlay_camera_write(
            s_camera_eye[0], s_camera_eye[1], s_camera_eye[2],
            s_camera_at[0],  s_camera_at[1],  s_camera_at[2]);
    }

    /* Step 2: render. Skipped when hidden (zero GL work). */
    if (s_visible) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(480, 640), ImGuiCond_FirstUseEver);
        ImGui::Begin("Xenogears Debug");
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
        if (ImGui::CollapsingHeader("GPU State",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_gpu_state_section();
        }

        /* ---- Section 2: RAM Inspector ---- */
        if (ImGui::CollapsingHeader("RAM Inspector",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_ram_inspector_section();
        }

        /* ---- Section 3: Toggles ---- */
        if (ImGui::CollapsingHeader("Toggles",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_toggles_section();
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
        if (ImGui::CollapsingHeader("Party (experimental)")) {
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

        /* Drive SDL text input from this frame's WantTextInput. Latched so
         * we only call SDL_Start/Stop on transitions, not every frame;
         * pre_swap runs on the same thread as the SDL event pump, so the
         * state is correct for the NEXT PollEvent cycle. */
        ImGuiIO &io = ImGui::GetIO();
        if (io.WantTextInput && !s_text_input_started) {
            SDL_StartTextInput();
            s_text_input_started = true;
        } else if (!io.WantTextInput && s_text_input_started) {
            SDL_StopTextInput();
            s_text_input_started = false;
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    } else if (s_text_input_started) {
        /* Hidden mid-text-input (Ctrl+F3 while typing): the Start call lives
         * inside the visible branch, so a toggle-while-typing leaves the
         * latch set — release it here to keep SDL text input off across the
         * open/close cycle. */
        SDL_StopTextInput();
        s_text_input_started = false;
    }

    /* Step 3: window_shot readback. Always runs when armed (works hidden
     * too — captures the game-only frame in that case). Must run AFTER
     * RenderDrawData when visible so the overlay's pixels are in the back
     * buffer. capture_window_rgb() defensively rebinds FBO 0 / sets
     * GL_READ_BUFFER, which restores the readback target even after the
     * ImGui frame may have left some state set. */
    if (s_window_shot_armed) {
        int w = 0, h = 0;
        uint8_t *rgb = capture_window_rgb(&w, &h);
        if (rgb) {
            write_rgb_png(s_window_shot_path, rgb, w, h);
            std::free(rgb);
        }
        s_window_shot_armed = false;
        s_window_shot_path[0] = '\0';
    }
}

/* ---- widget action hook (debug-only, TCP-driven) ----------------------- */

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
        int en = 0, sus = 0, hist = 0;
        double hh = 0.0, th = 0.0;
        uint64_t swaps = 0;
        gl_renderer_interpolation_diag(&en, &sus, &hist, &hh, &th, &swaps);
        gl_renderer_set_interpolation(value ? 1 : 0, hh, th);
        return 0;
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
        /* value = slot (lower 16 bits) + charId (next 8 bits) packed, or
         * use value=slot, value2=charId convention (value/100 = slot,
         * value%100 unused). To keep the API one-line, accept the
         * simple encoding: value = slot*256 + charId (slot in low byte,
         * charId in next byte). value2 = bitfieldBit (or -1 to skip). */
        int slot = value & 0xFF;
        int charId = (value >> 8) & 0xFF;
        int bfBit = value2;
        return psx_debug_overlay_write_party_slot(slot, charId, bfBit);
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
         * non-zero arms. The actual battle requires the field to have
         * encounter data loaded + the countdown to reach 0; this action
         * is best-effort (see draw_battle_section comment). */
        return psx_debug_overlay_force_battle(value);
    }
    if (std::strcmp(name, "camera_write") == 0) {
        /* Pack 6 s16 coords into 2 ints. value = (ey<<16)|ex,
         * value2 = (ay<<16)|ax, and ez/az are appended via the
         * overlay's editor state (s_camera_eye[2] / s_camera_at[2]).
         * This keeps the TCP API symmetric with the existing
         * two-int actions (teleport, write_var, etc.). */
        int ex = (int16_t)(value & 0xFFFF);
        int ey = (int16_t)((value >> 16) & 0xFFFF);
        int ax = (int16_t)(value2 & 0xFFFF);
        int ay = (int16_t)((value2 >> 16) & 0xFFFF);
        return psx_debug_overlay_camera_write(ex, ey, s_camera_eye[2],
                                              ax, ay, s_camera_at[2]);
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

#endif /* PSX_DEBUG_OVERLAY */
