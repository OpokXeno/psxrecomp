/* Player settings in Debug and Release. Owns an ImGui context, independent
 * of both the launcher and developer tools. Called only by the main thread. */
#include "runtime_menu.h"
#include "runtime_menu_visibility.h"
#include "runtime_menu_actions.h"
#include "free_camera.h"

#ifdef PSX_RUNTIME_MENU
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#if defined(PSX_SDL3)
#include <imgui_impl_sdl3.h>
#define MENU_SDL_INIT_GL ImGui_ImplSDL3_InitForOpenGL
#define MENU_SDL_NEW_FRAME ImGui_ImplSDL3_NewFrame
#define MENU_SDL_EVENT ImGui_ImplSDL3_ProcessEvent
#define MENU_SDL_SHUTDOWN ImGui_ImplSDL3_Shutdown
#else
#include <imgui_impl_sdl2.h>
#define MENU_SDL_INIT_GL ImGui_ImplSDL2_InitForOpenGL
#define MENU_SDL_NEW_FRAME ImGui_ImplSDL2_NewFrame
#define MENU_SDL_EVENT ImGui_ImplSDL2_ProcessEvent
#define MENU_SDL_SHUTDOWN ImGui_ImplSDL2_Shutdown
#endif
#include <SDL_opengl.h>
#include "gpu.h"
#include "gpu_render.h"
#include "gpu_gl_renderer.h"
#include "cdrom.h"
#include "host_osd.h"
#include "hd_texture_runtime.h"
#include "mod_runtime.h"
#include "psx_netplay.h"
#include "psx_keybinds.h"
#include "savestate.h"
#include <cstdio>
#include <ctime>

extern "C" {
int psx_video_get_supersampling(void);
void psx_video_set_supersampling(int);
int psx_video_get_antialiasing(void);
void psx_video_set_antialiasing(int);
int psx_video_get_antialiasing_factor(void);
void psx_video_set_antialiasing_factor(int);
int psx_video_get_screen_model(void);
void psx_video_set_screen_model(int);
int psx_video_get_native_depth_test(void);
void psx_video_set_native_depth_test(int);
void psx_video_get_aspect(int*, int*);
int psx_video_get_display_stretch(int*, int*);
int psx_video_set_aspect_runtime(int, int, int);
int psx_video_set_display_stretch(int, int);
int psx_video_get_window_width(void);
void psx_video_set_window_width(int);
int psx_video_get_vsync(void);
void psx_video_set_vsync(int);
int psx_native_semantic_fps_set(int);
int psx_audio_get_spu_hq(void);
void psx_audio_set_spu_hq(int);
int psx_fast_map_load_set(int);
int psx_frame_interpolation_enabled(void);
void psx_frame_interpolation_set(int);
extern int g_ws_bd_stretch_on;
extern int g_ws_bd_stretch_pct;
}

namespace {
SDL_Window* window;
ImGuiContext* context;
RuntimeMenuVisibility visibility;
bool capture;
bool text_input;
bool interpolation_suspended;
bool reset_focus;
float bar_height = 24.0f;
typedef void (APIENTRYP BindFramebuffer)(GLenum, GLuint);
BindFramebuffer bind_framebuffer;
bool save_slots_dirty = true;
bool file_menu_open;
bool save_slot_exists[SAVESTATE_SLOTS];
char save_slot_time[SAVESTATE_SLOTS][32];
char savestate_message[192];
bool savestate_message_failed;
enum class BindingDevice { Keyboard, Alternate, Gamepad };
struct {
    int player = 1;
    int button = -1;
    BindingDevice device = BindingDevice::Keyboard;
    SDL_JoystickID instance = PSX_SDL_INVALID_JOYSTICK_ID;
} rebind;
bool binding_menu_drawn;
unsigned int dirty_settings;
Uint64 settings_save_due;
/* The bar is presented on every host swap (240/s at 240 FPS) but its content
 * changes with input or slowly-varying settings. Rebuild the ImGui frame on
 * input, while interacting, or at ~60 Hz; otherwise redraw the retained draw
 * data, which stays valid until the next NewFrame. */
bool menu_input_pending = true;
bool menu_frame_valid;
Uint64 menu_frame_built_ns;
constexpr Uint64 kMenuRebuildIntervalNs = 16000000u;

struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ContextScope() { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

void release_capture() {
    capture = false;
    rebind.button = -1;
    if (text_input) { psx_sdl_stop_text_input(window); text_input = false; }
    if (interpolation_suspended) {
        interpolation_suspended = false;
        if (psx_frame_interpolation_enabled()) psx_frame_interpolation_set(1);
    }
}

void sync_fullscreen() {
    if (!window) return;
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!visibility.observe_fullscreen(fullscreen)) return;
    reset_focus = true;
    release_capture();
}

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

void remember(unsigned int setting) {
    dirty_settings |= setting;
    settings_save_due = SDL_GetTicks64() + 250;
}

void flush_settings(bool force = false) {
    if (!dirty_settings || (!force && SDL_GetTicks64() < settings_save_due)) return;
    if (!psx_runtime_menu_save_settings(dirty_settings,
            visibility.fullscreen ? visibility.windowed_visible : visibility.visible))
        host_osd_push("Settings changed; could not save preferences", 3000);
    dirty_settings = 0;
}

void toggle(const char* label, int value, void (*setter)(int), unsigned int setting) {
    bool enabled = value != 0;
    if (ImGui::MenuItem(label, nullptr, enabled)) {
        setter(enabled ? 0 : 1); remember(setting);
    }
}

void choice(const char* label, int value, const char* const* names,
            const int* values, int count, void (*setter)(int), unsigned int setting) {
    if (!ImGui::BeginMenu(label)) return;
    for (int i = 0; i < count; ++i)
        if (ImGui::MenuItem(names[i], nullptr, value == values[i])) { setter(values[i]); remember(setting); }
    ImGui::EndMenu();
}

void file_menu() {
    // Keep submenu positions stable when a result/error message changes.
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(280.0f, 1.0e6f));
    if (!ImGui::BeginMenu("File")) { file_menu_open = false; return; }
    if (!file_menu_open || save_slots_dirty) {
        for (int slot = 0; slot < SAVESTATE_SLOTS; ++slot) {
            save_slot_exists[slot] = savestate_slot_exists(slot) != 0;
            std::snprintf(save_slot_time[slot], sizeof(save_slot_time[slot]), "%s",
                          save_slot_exists[slot] ? "Saved" : "Empty");
            int64_t modified = 0;
            if (save_slot_exists[slot] && savestate_slot_mtime(slot, &modified)) {
                const std::time_t t = (std::time_t)modified;
                if (const auto* local = std::localtime(&t))
                    std::strftime(save_slot_time[slot], sizeof(save_slot_time[slot]),
                                  "%d/%m %H:%M", local);
            }
        }
        file_menu_open = true;
        save_slots_dirty = false;
    }
    const char* dir = savestate_dir();
    const bool host = !psx_netplay_active() || psx_netplay_is_host();
    const bool available = dir && *dir && host;
    const bool busy = savestate_pending() != 0;
    if (!host) ImGui::TextDisabled("Save states are host-only in netplay");
    for (int save = 1; save >= 0; --save) {
        if (!ImGui::BeginMenu(save ? "Save state" : "Load state", available)) continue;
        ImGui::PushID(save);
        for (int slot = 0; slot < SAVESTATE_SLOTS; ++slot) {
            char label[24];
            std::snprintf(label, sizeof(label), "Slot %d", slot + 1);
            if (ImGui::MenuItem(label, save_slot_time[slot], false,
                                !busy && (save || save_slot_exists[slot]))) {
                (void)psx_runtime_savestate_submit(slot, save);
            }
        }
        ImGui::PopID();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, savestate_message_failed
        ? ImVec4(1.0f, 0.5f, 0.3f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(savestate_message[0] ? savestate_message : "Select a slot to save or load.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndMenu();
}

void keyboard_binding_label(SDL_Scancode scancode, char* out, size_t capacity) {
    if ((int)scancode > 512 && (int)scancode <= 517)
        std::snprintf(out, capacity, "Mouse%d", (int)scancode - 512);
    else {
        const char* name = SDL_GetScancodeName(scancode);
        std::snprintf(out, capacity, "%s", name && *name ? name : "None");
    }
}

void binding_menu(int player, BindingDevice device, const char* label, bool enabled) {
    if (!ImGui::BeginMenu(label, enabled)) return;
    binding_menu_drawn = true;
    if (rebind.button >= 0) {
        ImGui::TextDisabled("Assign %s", psx_keybinds_button_label(rebind.button));
        ImGui::TextDisabled(device == BindingDevice::Gamepad ? "Press a button or move an axis"
                              : device == BindingDevice::Alternate ? "Press a key or mouse button" : "Press a key");
        ImGui::TextDisabled("Esc cancels; Backspace clears");
        ImGui::Separator();
    }
    for (int button = 0; button < psx_keybinds_button_count(); ++button) {
        char assigned[128];
        if (device == BindingDevice::Gamepad)
            psx_input_gamepad_binding_label(player, button, assigned, sizeof(assigned));
        else
            keyboard_binding_label(device == BindingDevice::Alternate
                ? psx_keybinds_get_button_alt(player, button) : psx_keybinds_get_button(player, button),
                assigned, sizeof(assigned));
        const bool listening = rebind.button == button && rebind.player == player && rebind.device == device;
        if (ImGui::MenuItem(psx_keybinds_button_label(button), listening ? "Listening..." : assigned, listening)) {
            rebind.player = player; rebind.button = button; rebind.device = device;
            if (device == BindingDevice::Gamepad)
                psx_input_binding_gamepad(player, &rebind.instance, nullptr, 0);
        }
    }
    ImGui::Separator();
    if (device != BindingDevice::Alternate && ImGui::MenuItem("Reset to defaults")) {
        rebind.button = -1;
        if (device == BindingDevice::Gamepad) psx_input_reset_gamepad_bindings(player);
        else { psx_keybinds_reset_player(player); psx_keybinds_save(); }
    }
    ImGui::EndMenu();
}

void controls_menu() {
    if (!ImGui::BeginMenu("Controls")) return;
    const bool editable = psx_input_runtime_bindings_available() != 0;
    ImGui::TextDisabled("Bindings apply immediately and are saved");
    for (int player = 1; player <= psx_input_binding_player_count(); ++player) {
        char title[16];
        std::snprintf(title, sizeof(title), "Player %d", player);
        if (!ImGui::BeginMenu(title, editable)) continue;
        binding_menu(player, BindingDevice::Keyboard, "Keyboard", true);
        binding_menu(player, BindingDevice::Alternate, "Alternate keyboard / mouse", true);
        char gamepad[128];
        const bool connected = psx_input_binding_gamepad(player, nullptr, gamepad, sizeof(gamepad)) != 0;
        if (connected) ImGui::TextDisabled("%s", gamepad);
        binding_menu(player, BindingDevice::Gamepad, "Gamepad", connected);
        if (!connected) tooltip("Connect the controller assigned to this player to edit its bindings.");
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

bool capture_binding_event(const SDL_Event* ev) {
    if (rebind.button < 0) return false;
    if (ev->type == SDL_KEYDOWN) {
        if (ev->key.repeat) return true;
        const auto key = psx_sdl_event_keycode(ev);
        if (key == SDLK_ESCAPE) { rebind.button = -1; return true; }
        if (rebind.device == BindingDevice::Gamepad) {
            if (key == SDLK_BACKSPACE) {
                psx_input_set_gamepad_binding(rebind.player, rebind.button, 0, 0, 0);
                rebind.button = -1;
            }
        } else {
#if defined(PSX_SDL3)
            const auto scancode = ev->key.scancode;
#else
            const auto scancode = ev->key.keysym.scancode;
#endif
            const auto assigned = key == SDLK_BACKSPACE ? SDL_SCANCODE_UNKNOWN : scancode;
            if (rebind.device == BindingDevice::Alternate)
                psx_keybinds_set_button_alt(rebind.player, rebind.button, assigned);
            else psx_keybinds_set_button(rebind.player, rebind.button, assigned);
            psx_keybinds_save(); rebind.button = -1;
        }
        return true;
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN && rebind.device == BindingDevice::Alternate &&
        ev->button.button >= 1 && ev->button.button <= 5) {
        psx_keybinds_set_button_alt(rebind.player, rebind.button, (SDL_Scancode)(512 + ev->button.button));
        psx_keybinds_save(); rebind.button = -1;
        return true;
    }
    if (rebind.device == BindingDevice::Gamepad) {
        SDL_JoystickID instance = PSX_SDL_INVALID_JOYSTICK_ID;
        if (!psx_input_binding_gamepad(rebind.player, &instance, nullptr, 0) || instance != rebind.instance) {
            rebind.button = -1; return false;
        }
#if defined(PSX_SDL3)
        const auto& button = ev->gbutton;
        const auto& axis = ev->gaxis;
#else
        const auto& button = ev->cbutton;
        const auto& axis = ev->caxis;
#endif
        if (ev->type == SDL_CONTROLLERBUTTONDOWN && button.which == rebind.instance) {
            psx_input_set_gamepad_binding(rebind.player, rebind.button, 1, button.button, 0);
            rebind.button = -1;
            return true;
        }
        if (ev->type == SDL_CONTROLLERAXISMOTION && axis.which == rebind.instance &&
            (axis.value > 16384 || axis.value < -16384)) {
            psx_input_set_gamepad_binding(rebind.player, rebind.button, 2, axis.axis, axis.value < 0 ? -1 : 1);
            rebind.button = -1;
            return true;
        }
    }
    return false;
}

void video_menu() {
    if (!ImGui::BeginMenu("Video")) return;
    if (ImGui::BeginMenu("Aspect ratio")) {
        int n = 4, d = 3;
        psx_video_get_aspect(&n, &d);
        const bool stretched = psx_video_get_display_stretch(nullptr, nullptr) != 0;
        if (ImGui::MenuItem("4:3", nullptr, n * 3 == d * 4 && !stretched)) {
            psx_video_set_aspect_runtime(4, 3, 0); g_ws_bd_stretch_on = 0;
            remember(MENU_ASPECT | MENU_BACKDROP_STRETCH);
        }
        if (ImGui::MenuItem("3:2 (stretched)", nullptr, stretched)) {
            psx_video_set_display_stretch(3, 2); g_ws_bd_stretch_on = 0;
            remember(MENU_ASPECT | MENU_BACKDROP_STRETCH);
        }
        tooltip("Stretches the original view to correct the pixel aspect. FMVs stay 4:3.");
        if (ImGui::MenuItem("16:9", nullptr, n * 9 == d * 16 && !stretched)) {
            psx_video_set_aspect_runtime(16, 9, 1); g_ws_bd_stretch_on = 1;
            remember(MENU_ASPECT | MENU_BACKDROP_STRETCH);
        }
        ImGui::EndMenu();
    }
    bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (ImGui::MenuItem("Fullscreen", "F11", fullscreen)) {
        SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        remember(MENU_FULLSCREEN);
    }
    if (ImGui::BeginMenu("Window size")) {
        const char* names[] = {"640 px", "960 px", "1280 px", "1920 px", "2560 px", "3840 px"};
        const int sizes[] = {640, 960, 1280, 1920, 2560, 3840};
        for (int i = 0; i < 6; ++i)
            if (ImGui::MenuItem(names[i], nullptr, psx_video_get_window_width() == sizes[i], !fullscreen)) {
                psx_video_set_window_width(sizes[i]);
                remember(MENU_WINDOW_SIZE);
            }
        ImGui::EndMenu();
    }
    const char* vs_names[] = {"Off", "On", "Adaptive"};
    const int vs_values[] = {0, 1, -1};
    choice("VSync", psx_video_get_vsync(), vs_names, vs_values, 3, psx_video_set_vsync, MENU_VSYNC);
    const char* fps_names[] = {"Original", "60 FPS", "75 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS"};
    const int fps_values[] = {30, 60, 75, 120, 144, 165, 240};
    if (ImGui::BeginMenu("Frame rate", gr_backend() == 1)) {
        for (int i = 0; i < 7; ++i)
            if (ImGui::MenuItem(fps_names[i], nullptr, gl_renderer_native_interpolation_target_fps() == fps_values[i])) {
                psx_native_semantic_fps_set(fps_values[i]);
                remember(MENU_FPS);
            }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Rendering", gr_backend() == 1)) {
        int scale = psx_video_get_supersampling();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Internal scale", &scale, 1, 8)) { psx_video_set_supersampling(scale); remember(MENU_SCALE); }
        const char* filters[] = {"Nearest", "Bilinear"};
        const int filter_values[] = {0, 1};
        choice("Texture filtering", gr_texture_filter(), filters, filter_values, 2, gr_set_texture_filter, MENU_TEXTURE_FILTER);
        choice("Sprite filtering", gl_renderer_sprite_filter(), filters, filter_values, 2, gl_renderer_set_sprite_filter, MENU_SPRITE_FILTER);
        const char* aniso_names[] = {"Off", "2x", "4x", "8x", "16x"};
        const int aniso_values[] = {0, 2, 4, 8, 16};
        choice("Anisotropic filtering", gl_renderer_anisotropy(), aniso_names, aniso_values, 5, gl_renderer_set_anisotropy, MENU_ANISOTROPY);
        toggle("Dithering", gpu_dithering_enabled(), gpu_dithering_set, MENU_DITHERING);
        const char* aa_names[] = {"Off", "FXAA", "SMAA", "TAA", "MSAA", "SSAA"};
        const int aa_values[] = {0, 1, 2, 3, 4, 5};
        choice("Antialiasing", psx_video_get_antialiasing(), aa_names, aa_values, 6, psx_video_set_antialiasing, MENU_ANTIALIASING);
        const char* factor_names[] = {"1x", "2x", "4x", "8x", "16x"};
        const int factors[] = {1, 2, 4, 8, 16};
        if (psx_video_get_antialiasing() == 0) ImGui::BeginDisabled();
        choice("AA multiplier", psx_video_get_antialiasing_factor(), factor_names, factors,
               psx_video_get_antialiasing() == 3 ? 3 : 5, psx_video_set_antialiasing_factor, MENU_AA_FACTOR);
        if (psx_video_get_antialiasing() == 0) ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    const char* screen_names[] = {"Raw", "CRT", "Composite", "Trinitron"};
    const int screen_values[] = {0, 1, 2, 3};
    choice("Screen model", psx_video_get_screen_model(), screen_names, screen_values, 4, psx_video_set_screen_model, MENU_SCREEN_MODEL);
    if (ImGui::BeginMenu("Advanced", gr_backend() == 1)) {
        toggle("Z-buffer", psx_video_get_native_depth_test(), psx_video_set_native_depth_test, MENU_Z_BUFFER);
        const char* names[] = {"Off", "Depth (colour)", "Policy", "Depth (grey)"};
        const int values[] = {0, 1, 2, 3};
        choice("Depth view", gl_renderer_native_depth_view(), names, values, 4, gl_renderer_set_native_depth_view, MENU_DEPTH_VIEW);
        toggle("Wireframe", gl_renderer_native_wireframe(), gl_renderer_set_native_wireframe, MENU_WIREFRAME);
        toggle("Generate texture mipmaps (experimental)", gl_renderer_debug_mipmaps(), gl_renderer_set_debug_mipmaps, MENU_MIPMAPS);
        bool stretch = g_ws_bd_stretch_on != 0;
        if (ImGui::MenuItem("Stretch widescreen backdrops", nullptr, stretch)) {
            g_ws_bd_stretch_on = !stretch; remember(MENU_BACKDROP_STRETCH);
        }
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Backdrop stretch (%)", &g_ws_bd_stretch_pct, 0, 100)) remember(MENU_BACKDROP_PERCENT);
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void draw_menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    // Preserve every open menu/submenu while choosing settings or actions.
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    bar_height = ImGui::GetWindowHeight();
    binding_menu_drawn = false;
    file_menu();
    video_menu();
    if (ImGui::BeginMenu("Audio")) {
        int volume = host_volume_get();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Volume (%)", &volume, 0, 100)) { host_volume_set(volume); remember(MENU_VOLUME); }
        toggle("High-quality SPU", psx_audio_get_spu_hq(), psx_audio_set_spu_hq, MENU_SPU_HQ);
        ImGui::EndMenu();
    }
    controls_menu();
    if (ImGui::BeginMenu("Camera")) {
        if (psx_free_camera_draw_controls(true)) remember(MENU_CAMERA);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mods")) {
        const bool offline = !psx_netplay_active();
        ImGui::TextDisabled("Changes apply immediately and are saved");
        if (!offline) ImGui::TextDisabled("Mods are locked during netplay");
        if (ImGui::MenuItem("Fast map load", nullptr, cdrom_data_read_policy_enabled() != 0,
                           offline && cdrom_data_read_policy_available())) {
            if (psx_fast_map_load_set(!cdrom_data_read_policy_enabled()) < 0)
                host_osd_push("Fast map load changed; could not save setting", 3000);
        }
        HdTextureRuntimeStats hd{};
        hd_texture_runtime_stats(&hd);
        if (ImGui::MenuItem("HD texture replacements", nullptr, hd.enabled != 0, offline && hd.active)) {
            hd_texture_runtime_set_enabled(!hd.enabled);
            remember(MENU_HD_TEXTURES);
        }
        tooltip("Switch the texture packs loaded at launch on or off immediately.");
        ImGui::Separator();
        for (const auto& feature : PSXRecompV4::mod_runtime_features()) {
            ImGui::PushID(feature.package_id.c_str());
            ImGui::PushID(feature.feature_id.c_str());
            if (ImGui::MenuItem(feature.name.c_str(), feature.runtime_toggle ? nullptr : "Restart required",
                                feature.enabled, offline && feature.runtime_toggle)) {
                std::string error;
                if (!PSXRecompV4::mod_runtime_set_feature_enabled(feature.package_id, feature.feature_id,
                                                                !feature.enabled, &error))
                    host_osd_push(error.c_str(), 3000);
            }
            tooltip(feature.runtime_toggle ? feature.description.c_str()
                    : "Configure this feature in the launcher, then restart the game.");
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Hide menu bar", "F10")) {
            visibility.hide(); reset_focus = true;
            remember(MENU_VISIBILITY);
        }
        ImGui::EndMenu();
    }
    if (!binding_menu_drawn) rebind.button = -1;
    ImGui::PopItemFlag();
    ImGui::EndMainMenuBar();
}

bool ensure_context() {
    if (context) return true;
    if (!window || !SDL_GL_GetCurrentContext()) return false;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    const bool platform = MENU_SDL_INIT_GL(window, SDL_GL_GetCurrentContext());
    const bool backend = platform && ImGui_ImplOpenGL3_Init("#version 330");
    if (!backend) {
        if (platform) MENU_SDL_SHUTDOWN();
        ImGui::DestroyContext(context); context = nullptr;
    }
    ImGui::SetCurrentContext(previous);
    return backend;
}

void render_frame(unsigned int framebuffer) {
    sync_fullscreen();
    if (reset_focus && context) {
        ContextScope scope;
        ImGui::SetWindowFocus(nullptr);
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        reset_focus = false;
    }
    if (!visibility.visible || !ensure_context()) { release_capture(); flush_settings(true); return; }
    ContextScope scope;
    const Uint64 now_ns = (Uint64)((double)SDL_GetPerformanceCounter() * 1e9 /
                                   (double)SDL_GetPerformanceFrequency());
    if (menu_frame_valid && !menu_input_pending && !capture && !text_input &&
        now_ns - menu_frame_built_ns < kMenuRebuildIntervalNs) {
        if (bind_framebuffer) {
            bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
        }
        return;
    }
    menu_input_pending = false;
    menu_frame_built_ns = now_ns;
    ImGui_ImplOpenGL3_NewFrame();
    MENU_SDL_NEW_FRAME();
    ImGui::NewFrame();
    draw_menu();
    capture = visibility.visible && (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
                          || ImGui::IsAnyItemActive());
    const bool want_text = visibility.visible && ImGui::GetIO().WantTextInput;
    if (want_text != text_input) {
        if (want_text) psx_sdl_start_text_input(window);
        else psx_sdl_stop_text_input(window);
        text_input = want_text;
    }
    ImGui::Render();
    menu_frame_valid = true;
    if (!ImGui::IsAnyItemActive()) flush_settings();
    if (bind_framebuffer) {
        bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
    }
    // Crossfade presentation has a worker context; suspend while navigating.
    if (capture && !interpolation_suspended) {
        int enabled = 0;
        double host = 0, target = 0;
        gl_renderer_interpolation_diag(&enabled, nullptr, nullptr, &host, &target, nullptr);
        if (enabled) {
            gl_renderer_set_interpolation(0, host, target, 0.0, 0);
            interpolation_suspended = true;
        }
    } else if (!capture && interpolation_suspended) {
        interpolation_suspended = false;
        if (psx_frame_interpolation_enabled()) psx_frame_interpolation_set(1);
    }
}
} // namespace

void psx_runtime_menu_init(SDL_Window* w) {
    window = w;
    visibility.initialize(w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0);
    visibility.windowed_visible = psx_runtime_menu_restore_settings() != 0;
    visibility.visible = !visibility.fullscreen && visibility.windowed_visible;
    dirty_settings = 0;
    capture = false; reset_focus = false;
    rebind.button = -1; file_menu_open = false; save_slots_dirty = true;
    savestate_message[0] = '\0'; savestate_message_failed = false;
    if (SDL_GL_GetCurrentContext())
        bind_framebuffer = reinterpret_cast<BindFramebuffer>(SDL_GL_GetProcAddress("glBindFramebuffer"));
    ensure_context();
}

void psx_runtime_menu_shutdown(void) {
    flush_settings(true);
    if (context) {
        ContextScope scope;
        if (text_input) psx_sdl_stop_text_input(window);
        ImGui_ImplOpenGL3_Shutdown();
        MENU_SDL_SHUTDOWN();
        ImGui::DestroyContext(context);
    }
    context = nullptr; window = nullptr;
    bind_framebuffer = nullptr; capture = false; text_input = false;
    rebind.button = -1;
    interpolation_suspended = false;
}

bool psx_runtime_menu_capture_input(void) {
    return visibility.visible && capture && window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
}

bool psx_runtime_menu_process_event(const SDL_Event* ev) {
    if (!ev || !window) return false;
    sync_fullscreen();
    const bool key = ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP || ev->type == SDL_TEXTINPUT;
    const bool mouse = ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONDOWN ||
                       ev->type == SDL_MOUSEBUTTONUP || ev->type == SDL_MOUSEWHEEL;
    Uint32 event_window = 0;
    if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) event_window = ev->key.windowID;
    else if (ev->type == SDL_TEXTINPUT) event_window = ev->text.windowID;
    else if (ev->type == SDL_MOUSEMOTION) event_window = ev->motion.windowID;
    else if (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP) event_window = ev->button.windowID;
    else if (ev->type == SDL_MOUSEWHEEL) event_window = ev->wheel.windowID;
    if ((key || mouse) && event_window != SDL_GetWindowID(window)) return false;
    if (ev->type == SDL_KEYDOWN && psx_sdl_event_keycode(ev) == SDLK_F10 &&
        !(psx_sdl_event_keymod(ev) & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT | KMOD_GUI))) {
        if (!ev->key.repeat) {
            visibility.toggle(); reset_focus = true;
            if (!visibility.fullscreen) remember(MENU_VISIBILITY);
            release_capture();
        }
        return true;
    }
    if (!visibility.visible) return false;
    if (capture_binding_event(ev)) return true;
    if (context) { ContextScope scope; MENU_SDL_EVENT(ev); menu_input_pending = true; }
    if (key) return psx_runtime_menu_capture_input();
    if (mouse) {
        if (psx_runtime_menu_capture_input()) return true;
        if (ev->type == SDL_MOUSEMOTION) return ev->motion.y < bar_height;
        if (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP) {
            if (ev->button.y < bar_height) {
                if (ev->type == SDL_MOUSEBUTTONDOWN) capture = true;
                return true;
            }
        }
    }
    return false;
}

bool psx_runtime_menu_needs_present(void) { sync_fullscreen(); return visibility.visible || visibility.clear_pending; }
void psx_runtime_menu_pre_swap_target(unsigned int framebuffer) {
    render_frame(framebuffer); visibility.clear_pending = false;
}
void psx_runtime_menu_service_layer(void) {
    if (!visibility.visible || !window) {
        gl_renderer_ui_layer_hide();
        render_frame(0u); /* releases capture and flushes settings */
        visibility.clear_pending = false;
        return;
    }
    const Uint64 now_ns = (Uint64)((double)SDL_GetPerformanceCounter() * 1e9 /
                                   (double)SDL_GetPerformanceFrequency());
    if (menu_frame_valid && !menu_input_pending && !capture && !text_input &&
        now_ns - menu_frame_built_ns < kMenuRebuildIntervalNs)
        return; /* The published layer is still current. */
    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    const unsigned int framebuffer = gl_renderer_ui_layer_begin(width, height);
    if (!framebuffer) return;
    render_frame(framebuffer);
    gl_renderer_ui_layer_end();
    visibility.clear_pending = false;
}
void psx_runtime_menu_note_savestates_changed(void) { save_slots_dirty = true; }
void psx_runtime_menu_savestate_status(const char* message, bool failed) {
    std::snprintf(savestate_message, sizeof(savestate_message), "%s", message ? message : "");
    savestate_message_failed = failed;
}
#endif
