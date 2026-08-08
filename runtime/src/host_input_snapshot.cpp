#include "host_input_snapshot.h"

#include <algorithm>

namespace host_input {

HostInputSnapshot::HostInputSnapshot(
        std::array<uint8_t, SDL_NUM_SCANCODES> keyboard,
        std::vector<ControllerSnapshot> controllers)
    : keyboard_(std::move(keyboard)), controllers_(std::move(controllers)) {}

HostInputSnapshot HostInputSnapshot::capture() {
    std::array<uint8_t, SDL_NUM_SCANCODES> keyboard{};
    int count = 0;
    const Uint8* state = SDL_GetKeyboardState(&count);
    if (state && count > 0) {
        const size_t copied = std::min<size_t>(keyboard.size(), static_cast<size_t>(count));
        std::copy_n(state, copied, keyboard.begin());
    }

    std::vector<ControllerSnapshot> controllers;
    const int joysticks = SDL_NumJoysticks();
    for (int index = 0; index < joysticks; ++index) {
        if (!SDL_IsGameController(index)) continue;
        SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
        SDL_GameController* controller = SDL_GameControllerFromInstanceID(instance);
        if (!controller) controller = SDL_GameControllerOpen(index);
        if (!controller) continue;

        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
        if (joystick) instance = SDL_JoystickInstanceID(joystick);
        ControllerSnapshot snapshot{};
        snapshot.instance = instance;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
            snapshot.buttons[button] = static_cast<uint8_t>(SDL_GameControllerGetButton(
                controller, static_cast<SDL_GameControllerButton>(button)));
        for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis)
            snapshot.axes[axis] = SDL_GameControllerGetAxis(
                controller, static_cast<SDL_GameControllerAxis>(axis));
        controllers.push_back(snapshot);
    }
    return {keyboard, std::move(controllers)};
}

const std::array<uint8_t, SDL_NUM_SCANCODES>& HostInputSnapshot::keyboard() const {
    return keyboard_;
}

const std::vector<ControllerSnapshot>& HostInputSnapshot::controllers() const {
    return controllers_;
}

const ControllerSnapshot* HostInputSnapshot::controller(SDL_JoystickID instance) const {
    const auto found = std::find_if(controllers_.begin(), controllers_.end(),
                                    [instance](const ControllerSnapshot& controller) {
                                        return controller.instance == instance;
                                    });
    return found == controllers_.end() ? nullptr : &*found;
}

}
