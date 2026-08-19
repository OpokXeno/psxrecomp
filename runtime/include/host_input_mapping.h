#pragma once

#include "host_input_snapshot.h"
#include "psx_netplay.h"

#include <array>
#include <cstdint>
#include <vector>

namespace host_input {

struct ControllerSource {
    enum class Kind : uint8_t {
        None,
        Button,
        AxisPositive,
        AxisNegative,
    };

    Kind kind = Kind::None;
    int id = -1;
};

struct ControllerMapEntry {
    uint16_t bit = 0;
    const char* ini_name = nullptr;
    std::vector<ControllerSource> sources{};
};

using ControllerMap = std::array<ControllerMapEntry, 16>;

struct PlayerRoute {
    uint8_t kind = 0;
    int mode = 0;
    bool hybrid_analog = false;
    SDL_JoystickID instance = PSX_SDL_INVALID_JOYSTICK_ID;
};

struct MappingOptions {
    const ControllerMap& controller_map;
    int controller_deadzone = 0;
    bool keyboard_swallowed = false;
    bool dev_p1 = false;
};

int capture_pad_slot(const HostInputSnapshot& snapshot, int slot,
                     PlayerRoute* player, const MappingOptions& options,
                     PsxNetPad* out);

}
