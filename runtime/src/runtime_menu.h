#ifndef PSX_RUNTIME_MENU_H
#define PSX_RUNTIME_MENU_H

#include <stdbool.h>
#include "psx_sdl.h"

#if defined(RECOMP_LAUNCHER) && !defined(PSX_NO_RUNTIME_MENU)
#define PSX_RUNTIME_MENU 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PSX_RUNTIME_MENU
void psx_runtime_menu_init(SDL_Window* window);
void psx_runtime_menu_shutdown(void);
bool psx_runtime_menu_process_event(const SDL_Event* event);
bool psx_runtime_menu_capture_input(void);
bool psx_runtime_menu_needs_present(void);
void psx_runtime_menu_pre_swap_target(unsigned int framebuffer);
void psx_runtime_menu_note_savestates_changed(void);
void psx_runtime_menu_savestate_status(const char* message, bool failed);
#else
static inline void psx_runtime_menu_init(SDL_Window* w) { (void)w; }
static inline void psx_runtime_menu_shutdown(void) {}
static inline bool psx_runtime_menu_process_event(const SDL_Event* e) { (void)e; return false; }
static inline bool psx_runtime_menu_capture_input(void) { return false; }
static inline bool psx_runtime_menu_needs_present(void) { return false; }
static inline void psx_runtime_menu_pre_swap_target(unsigned int f) { (void)f; }
static inline void psx_runtime_menu_note_savestates_changed(void) {}
static inline void psx_runtime_menu_savestate_status(const char* m, bool f) { (void)m; (void)f; }
#endif

#ifdef __cplusplus
}
#endif
#endif
