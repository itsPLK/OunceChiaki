#include "controller.hpp"

#include <algorithm>

namespace ounce
{

namespace
{

int16_t axis(float value)
{
    return static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
}

}

ChiakiControllerState mapController(const brls::ControllerState& pad, const SystemButtons& system)
{
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    struct Mapping
    {
        brls::ControllerButton from;
        uint32_t to;
    };
    static constexpr Mapping buttons[] = {
        { brls::BUTTON_B, CHIAKI_CONTROLLER_BUTTON_CROSS },
        { brls::BUTTON_A, CHIAKI_CONTROLLER_BUTTON_MOON },
        { brls::BUTTON_Y, CHIAKI_CONTROLLER_BUTTON_BOX },
        { brls::BUTTON_X, CHIAKI_CONTROLLER_BUTTON_PYRAMID },
        { brls::BUTTON_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_UP },
        { brls::BUTTON_DOWN, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
        { brls::BUTTON_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT },
        { brls::BUTTON_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
        { brls::BUTTON_LB, CHIAKI_CONTROLLER_BUTTON_L1 },
        { brls::BUTTON_RB, CHIAKI_CONTROLLER_BUTTON_R1 },
        { brls::BUTTON_LSB, CHIAKI_CONTROLLER_BUTTON_L3 },
        { brls::BUTTON_RSB, CHIAKI_CONTROLLER_BUTTON_R3 },
        { brls::BUTTON_START, CHIAKI_CONTROLLER_BUTTON_OPTIONS },
        { brls::BUTTON_BACK, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD },
    };
    for (const auto& mapping : buttons)
        if (pad.buttons[mapping.from])
            state.buttons |= mapping.to;
    if (system.chat)
        state.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
    if (system.capture)
        state.buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
    // ZL/ZR are digital on Switch controllers.
    state.l2_state = pad.buttons[brls::BUTTON_LT] ? 255 : 0;
    state.r2_state = pad.buttons[brls::BUTTON_RT] ? 255 : 0;
    // Borealis's axes are -1..1 with down positive, as the PlayStation's.
    state.left_x  = axis(pad.axes[brls::LEFT_X]);
    state.left_y  = axis(pad.axes[brls::LEFT_Y]);
    state.right_x = axis(pad.axes[brls::RIGHT_X]);
    state.right_y = axis(pad.axes[brls::RIGHT_Y]);
    return state;
}

}
