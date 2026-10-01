#pragma once
#include <stdbool.h>
#include "psx_sdl.h"

#if defined(RECOMP_LAUNCHER) && (defined(PSX_DEBUG_OVERLAY) || !defined(PSX_NO_RUNTIME_MENU))
#define PSX_FREE_CAMERA 1
#endif

/* Settings only: live pose and saved engine state remain session-local. */
typedef struct PsxFreeCameraSettings {
    bool enabled;
    bool fly_keys;
    bool capture_input;
    bool mouse_look;
    bool invert_y;
    bool wheel_dolly;
    bool pan;
    float fly_speed;
    float rotation_speed;
    float look_sensitivity;
    float wheel_step;
    float pan_factor;
} PsxFreeCameraSettings;

#ifdef __cplusplus
extern "C" {
#endif
#ifdef PSX_FREE_CAMERA
void psx_free_camera_init(SDL_Window* window);
void psx_free_camera_shutdown(void);
void psx_free_camera_update(SDL_Window* input_window, bool input_blocked);
bool psx_free_camera_process_event(const SDL_Event* event, bool input_blocked);
bool psx_free_camera_capture_input(void);
bool psx_free_camera_draw_controls(bool compact);
void psx_free_camera_get_settings(PsxFreeCameraSettings* settings);
void psx_free_camera_set_settings(const PsxFreeCameraSettings* settings);
void psx_free_camera_get_pose(float eye[3], float at[3]);
int psx_free_camera_write(int ex, int ey, int ez, int ax, int ay, int az);
/* Release guest camera ownership before saving/loading guest RAM. The next
 * frame reacquires a fresh pose when free camera is still enabled. */
void psx_free_camera_suspend(void);
#else
static inline void psx_free_camera_init(SDL_Window* w) { (void)w; }
static inline void psx_free_camera_shutdown(void) {}
static inline void psx_free_camera_update(SDL_Window* w, bool b) { (void)w; (void)b; }
static inline bool psx_free_camera_process_event(const SDL_Event* e, bool b) { (void)e; (void)b; return false; }
static inline bool psx_free_camera_capture_input(void) { return false; }
static inline void psx_free_camera_suspend(void) {}
static inline void psx_free_camera_get_settings(PsxFreeCameraSettings* s) { (void)s; }
static inline void psx_free_camera_set_settings(const PsxFreeCameraSettings* s) { (void)s; }
#endif
#ifdef __cplusplus
}
#endif
