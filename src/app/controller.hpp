#pragma once

#include <borealis.hpp>
#include <chiaki/controller.h>

namespace ounce
{

// The Switch controls as a PlayStation controller, by position: B/A/Y/X are
// Cross/Circle/Square/Triangle, ZL/ZR are L2/R2, Plus is Options and Minus the
// touchpad click (until the touchscreen is the touchpad). Of the taken-over
// system buttons, Chat is the PS button, Capture is Create/Share and Home opens
// the stream menu. Gyro and touch come later (AGENTS.md).
struct SystemButtons
{
    bool chat = false, capture = false;
};

ChiakiControllerState mapController(const brls::ControllerState& pad, const SystemButtons& system);

}
