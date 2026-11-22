#include "frame_generation.hpp"

using namespace ounce;
using Mode = FrameGeneration;
using Draw = VideoDraw;

// Draw B at its deadline even when the timer wakes late or the menu opens.
int main()
{
    const auto interp = [](bool fresh, bool pair, uint64_t now, uint64_t due, bool menu = false) {
        return chooseVideoDraw(Mode::Interpolate, fresh, pair, now, due, menu);
    };
    if (interp(true, false, 0, 0) != Draw::Real ||
        interp(true, true, 33333, 0) != Draw::Generate ||
        interp(false, false, 49999, 50000) != Draw::Repeat ||
        interp(false, false, 50000, 50000) != Draw::Real ||
        interp(false, false, 61000, 50000) != Draw::Real ||
        interp(false, false, 48000, 50000, true) != Draw::Real ||
        interp(true, true, 48000, 50000) != Draw::Real ||
        interp(true, true, 33333, 0, true) != Draw::Real)
        return 1;
    if (chooseVideoDraw(Mode::Extrapolate, true, true, 33333, 0, false) != Draw::Real ||
        chooseVideoDraw(Mode::Extrapolate, false, false, 50000, 50000, false) != Draw::Generate ||
        chooseVideoDraw(Mode::Extrapolate, false, false, 60000, 50000, false) != Draw::Repeat ||
        chooseVideoDraw(Mode::Off, true, false, 0, 0, false) != Draw::Real)
        return 1;
}
