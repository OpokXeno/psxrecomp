#ifndef PSX_RUNTIME_MENU_VISIBILITY_H
#define PSX_RUNTIME_MENU_VISIBILITY_H

/* Fullscreen transitions remember the player's windowed preference. F10
 * may reveal the bar in fullscreen without changing that preference. */
struct RuntimeMenuVisibility {
    bool visible = true;
    bool fullscreen = false;
    bool windowed_visible = true;
    bool clear_pending = false;

    void initialize(bool is_fullscreen) {
        fullscreen = is_fullscreen;
        windowed_visible = true;
        visible = !is_fullscreen;
        clear_pending = false;
    }
    bool observe_fullscreen(bool is_fullscreen) {
        if (fullscreen == is_fullscreen) return false;
        fullscreen = is_fullscreen;
        if (fullscreen) { windowed_visible = visible; visible = false; }
        else visible = windowed_visible;
        clear_pending = true;
        return true;
    }
    void toggle() { visible = !visible; clear_pending = true; }
    void hide() { visible = false; clear_pending = true; }
};
#endif
