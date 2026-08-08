#pragma once

#include <SDL.h>

#include <array>
#include <cstdint>
#include <vector>

namespace host_input {

struct ControllerSnapshot {
    SDL_JoystickID instance = -1;
    std::array<uint8_t, SDL_CONTROLLER_BUTTON_MAX> buttons{};
    std::array<int16_t, SDL_CONTROLLER_AXIS_MAX> axes{};
};

class HostInputSnapshot {
public:
    HostInputSnapshot() = default;
    HostInputSnapshot(std::array<uint8_t, SDL_NUM_SCANCODES> keyboard,
                      std::vector<ControllerSnapshot> controllers);

    static HostInputSnapshot capture();

    const std::array<uint8_t, SDL_NUM_SCANCODES>& keyboard() const;
    const std::vector<ControllerSnapshot>& controllers() const;
    const ControllerSnapshot* controller(SDL_JoystickID instance) const;

private:
    std::array<uint8_t, SDL_NUM_SCANCODES> keyboard_{};
    std::vector<ControllerSnapshot> controllers_{};
};

}
