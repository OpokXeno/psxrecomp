#pragma once

#include <stddef.h>
#include "psx_sdl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host actions shared with the existing input profiles and savestate path.
 * Players are 1-based; button indices use PsxKeybindButton. */
int psx_input_runtime_bindings_available(void);
int psx_input_binding_player_count(void);
int psx_input_binding_gamepad(int player, SDL_JoystickID* instance,
                              char* name, size_t capacity);
void psx_input_gamepad_binding_label(int player, int button,
                                    char* out, size_t capacity);
int psx_input_set_gamepad_binding(int player, int button, int kind,
                                 int code, int axis_direction);
int psx_input_reset_gamepad_bindings(int player);
int psx_runtime_savestate_submit(int slot, int save);

enum RuntimeMenuSetting {
    MENU_ASPECT = 1u << 0, MENU_WINDOW_SIZE = 1u << 1,
    MENU_FULLSCREEN = 1u << 2, MENU_VSYNC = 1u << 3,
    MENU_SCALE = 1u << 4, MENU_TEXTURE_FILTER = 1u << 5,
    MENU_SPRITE_FILTER = 1u << 6, MENU_ANISOTROPY = 1u << 7,
    MENU_ANTIALIASING = 1u << 8, MENU_AA_FACTOR = 1u << 9,
    MENU_FPS = 1u << 10, MENU_DITHERING = 1u << 11,
    MENU_SCREEN_MODEL = 1u << 12, MENU_Z_BUFFER = 1u << 13,
    MENU_MIPMAPS = 1u << 14, MENU_WIREFRAME = 1u << 15,
    MENU_DEPTH_VIEW = 1u << 16, MENU_BACKDROP_STRETCH = 1u << 17,
    MENU_BACKDROP_PERCENT = 1u << 18, MENU_VOLUME = 1u << 19,
    MENU_SPU_HQ = 1u << 20, MENU_HD_TEXTURES = 1u << 21,
    MENU_VISIBILITY = 1u << 22, MENU_CAMERA = 1u << 23
};
int psx_runtime_menu_save_settings(unsigned int changed, int windowed_visible);
/* Restore advanced preferences after renderer initialization; returns the
 * remembered windowed menu visibility. */
int psx_runtime_menu_restore_settings(void);

#ifdef __cplusplus
}
#endif
