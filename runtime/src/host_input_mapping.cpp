#include "host_input_mapping.h"

#include "psx_keybinds.h"

#include <cmath>

namespace host_input {
namespace {

constexpr int kModeHybrid = 0;
constexpr int kModeAnalog = 1;
constexpr int kModeDigital = 2;

bool source_pressed(const ControllerSnapshot* controller, const ControllerSource& source,
                    int deadzone) {
    if (!controller) return false;
    switch (source.kind) {
    case ControllerSource::Kind::Button:
        return source.id >= 0 && source.id < SDL_CONTROLLER_BUTTON_MAX &&
               controller->buttons[source.id] != 0;
    case ControllerSource::Kind::AxisPositive:
        return source.id >= 0 && source.id < SDL_CONTROLLER_AXIS_MAX &&
               controller->axes[source.id] > deadzone;
    case ControllerSource::Kind::AxisNegative:
        return source.id >= 0 && source.id < SDL_CONTROLLER_AXIS_MAX &&
               controller->axes[source.id] < -deadzone;
    case ControllerSource::Kind::None:
        return false;
    }
    return false;
}

bool stick_axis(const ControllerSource& source) {
    return (source.kind == ControllerSource::Kind::AxisPositive ||
            source.kind == ControllerSource::Kind::AxisNegative) &&
           (source.id == SDL_CONTROLLER_AXIS_LEFTX || source.id == SDL_CONTROLLER_AXIS_LEFTY ||
            source.id == SDL_CONTROLLER_AXIS_RIGHTX || source.id == SDL_CONTROLLER_AXIS_RIGHTY);
}

uint16_t controller_buttons(const ControllerSnapshot* controller, const ControllerMap& map,
                            int deadzone, bool suppress_sticks) {
    uint16_t buttons = 0xFFFFu;
    for (const ControllerMapEntry& entry : map) {
        for (const ControllerSource& source : entry.sources) {
            if ((!suppress_sticks || !stick_axis(source)) &&
                source_pressed(controller, source, deadzone)) {
                buttons &= static_cast<uint16_t>(~entry.bit);
                break;
            }
        }
    }
    return buttons;
}

void axes_to_pad_pair(int16_t x_axis, int16_t y_axis, int deadzone, uint8_t* x_out,
                      uint8_t* y_out) {
    const double x = x_axis;
    const double y = y_axis;
    const double magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone || magnitude <= 0.0) {
        *x_out = 0x80u;
        *y_out = 0x80u;
        return;
    }
    const double capped = magnitude > 32767.0 ? 32767.0 : magnitude;
    const double range = std::max(1.0, 32767.0 - deadzone);
    const double scale = ((capped - deadzone) * 32767.0 / range) / magnitude;
    const int x_scaled = std::max(-32768, std::min(32767, static_cast<int>(std::lround(x * scale))));
    const int y_scaled = std::max(-32768, std::min(32767, static_cast<int>(std::lround(y * scale))));
    *x_out = static_cast<uint8_t>(std::max(0, std::min(255, (x_scaled + 32768) >> 8)));
    *y_out = static_cast<uint8_t>(std::max(0, std::min(255, (y_scaled + 32768) >> 8)));
}

void controller_sticks(const ControllerSnapshot* controller, int deadzone, bool fold_dpad,
                       uint8_t out[4]) {
    if (!controller) return;
    axes_to_pad_pair(controller->axes[SDL_CONTROLLER_AXIS_LEFTX],
                     controller->axes[SDL_CONTROLLER_AXIS_LEFTY], deadzone, &out[0], &out[1]);
    axes_to_pad_pair(controller->axes[SDL_CONTROLLER_AXIS_RIGHTX],
                     controller->axes[SDL_CONTROLLER_AXIS_RIGHTY], deadzone, &out[2], &out[3]);
    if (!fold_dpad) return;
    if (controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]) out[0] = 0x00u;
    if (controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]) out[0] = 0xFFu;
    if (controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]) out[1] = 0x00u;
    if (controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]) out[1] = 0xFFu;
}

bool hybrid_stick_active(const ControllerSnapshot* controller, int deadzone) {
    if (!controller) return false;
    const double x = controller->axes[SDL_CONTROLLER_AXIS_LEFTX];
    const double y = controller->axes[SDL_CONTROLLER_AXIS_LEFTY];
    return std::sqrt(x * x + y * y) > deadzone;
}

bool hybrid_dpad_active(const HostInputSnapshot& snapshot, const ControllerSnapshot* controller,
                        int player, bool keyboard_active, bool keyboard_swallowed) {
    if (controller && (controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ||
                       controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ||
                       controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] ||
                       controller->buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN])) return true;
    return keyboard_active && !keyboard_swallowed &&
           psx_keybinds_dpad_active(snapshot.keyboard().data(), player) != 0;
}

}

int capture_pad_slot(const HostInputSnapshot& snapshot, int slot, PlayerRoute* player,
                     const MappingOptions& options, PsxNetPad* out) {
    if (!out || !player || slot < 0 || slot > 1) return 0;
    *out = {0xFFFFu, 0x80u, 0x80u, 0x80u, 0x80u, 0u, 0u};
    const bool dev_here = options.dev_p1 && slot == 0;
    if (player->kind == 0 && !dev_here) return 0;

    const int player_number = slot + 1;
    const ControllerSnapshot* selected = snapshot.controller(player->instance);
    const int mode = player->kind != 0 ? player->mode : (dev_here ? kModeHybrid : kModeDigital);
    int analog = 0;
    if (mode == kModeAnalog) analog = 1;
    else if (mode == kModeHybrid) {
        if (hybrid_stick_active(selected, options.controller_deadzone)) player->hybrid_analog = true;
        else if (hybrid_dpad_active(snapshot, selected, player_number,
                                    player->kind == 1 || dev_here,
                                    options.keyboard_swallowed)) player->hybrid_analog = false;
        analog = player->hybrid_analog ? 1 : 0;
    }

    const bool suppress_sticks = analog != 0;
    uint16_t buttons = 0xFFFFu;
    if (player->kind == 1) {
        if (!options.keyboard_swallowed)
            buttons = psx_keybinds_pad_word(snapshot.keyboard().data(), player_number);
    } else if (player->kind == 2) {
        buttons = controller_buttons(selected, options.controller_map, options.controller_deadzone,
                                     suppress_sticks);
    }
    if (dev_here) {
        if (!options.keyboard_swallowed)
            buttons &= psx_keybinds_pad_word(snapshot.keyboard().data(), 1);
        for (const ControllerSnapshot& controller : snapshot.controllers())
            buttons &= controller_buttons(&controller, options.controller_map,
                                          options.controller_deadzone, suppress_sticks);
    }

    uint8_t sticks[4] = {0x80u, 0x80u, 0x80u, 0x80u};
    if (mode == kModeAnalog) {
        if (player->kind == 1) {
            if (!options.keyboard_swallowed)
                psx_keybinds_sticks(snapshot.keyboard().data(), player_number, sticks);
        } else {
            controller_sticks(selected, options.controller_deadzone, true, sticks);
        }
    } else if (analog) {
        if (player->kind == 1) {
            if (!options.keyboard_swallowed)
                psx_keybinds_sticks(snapshot.keyboard().data(), player_number, sticks);
        } else {
            controller_sticks(selected, options.controller_deadzone, false, sticks);
        }
    }
    if (dev_here && analog) {
        if (!options.keyboard_swallowed)
            psx_keybinds_sticks(snapshot.keyboard().data(), 1, sticks);
        for (const ControllerSnapshot& controller : snapshot.controllers()) {
            uint8_t extra[4] = {0x80u, 0x80u, 0x80u, 0x80u};
            controller_sticks(&controller, options.controller_deadzone, false, extra);
            if (extra[0] != 0x80u || extra[1] != 0x80u) { sticks[0] = extra[0]; sticks[1] = extra[1]; }
            if (extra[2] != 0x80u || extra[3] != 0x80u) { sticks[2] = extra[2]; sticks[3] = extra[3]; }
        }
    }

    out->buttons = buttons;
    out->lx = sticks[0]; out->ly = sticks[1]; out->rx = sticks[2]; out->ry = sticks[3];
    out->analog = analog ? 1u : 0u;
    out->connected = 1u;
    return 1;
}

}
