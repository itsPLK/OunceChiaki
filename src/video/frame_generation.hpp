#pragma once

#include <cstdint>

namespace ounce
{

// Values are persisted in settings.txt. Older builds wrote 1 for extrapolation.
enum class FrameGeneration { Off = 0, Extrapolate = 1, Interpolate = 2 };

enum class VideoDraw { Repeat, Real, Generate };

// A single outstanding timer belongs to the last accepted input pair. If
// fresh video overtakes a deferred endpoint, resume with the newest real frame.
inline VideoDraw chooseVideoDraw(FrameGeneration mode, bool fresh, bool pairReady,
    uint64_t now, uint64_t deadline, bool menuOpen)
{
    if (fresh)
        return mode == FrameGeneration::Interpolate && pairReady && !deadline && !menuOpen
            ? VideoDraw::Generate : VideoDraw::Real;
    if (!deadline || (now < deadline && !menuOpen))
        return VideoDraw::Repeat;
    if (mode == FrameGeneration::Interpolate)
        return VideoDraw::Real;
    return !menuOpen && now - deadline < 8000 ? VideoDraw::Generate : VideoDraw::Repeat;
}

}
