#pragma once

#include <cstdint>

namespace ounce
{

// Values are persisted in settings.txt. Older builds wrote 1 for extrapolation.
enum class FrameGeneration { Off = 0, Extrapolate = 1, Interpolate = 2 };

enum class VideoDraw { Repeat, Real, Generate };

// Frame generation timing as fractions of the stream's frame period: the
// generated picture half a period after the real one, inputs under 0.36 of a
// period old, predictions at most 0.24 late (at 30 fps the values tuned for 30
// to 60 fps: 16.7, 12 and 8 ms). Extrapolation holds a picture that arrives
// at most 0.36 of a period before the pending prediction (6 ms at 60 fps).
// Pairs must be consecutive pictures; the 2.5
// period upper spacing limit catches stalls, while burst arrivals decoded
// back-to-back remain valid consecutive motion pairs.
struct GenerationTiming
{
    uint64_t periodUs, offsetUs, pairMaxUs, freshMaxUs, lateMaxUs, holdMaxUs;

    static GenerationTiming forFps(int fps)
    {
        const uint64_t p = (1000000 + fps / 2) / (fps > 0 ? fps : 30);
        auto part = [p](uint64_t percent) { return (p * percent + 50) / 100; };
        return { p, (p + 1) / 2, part(250), part(36), part(24), part(36) };
    }
};

// A single outstanding timer belongs to the last accepted input pair. If
// fresh video overtakes a deferred endpoint, resume with the newest real frame.
inline VideoDraw chooseVideoDraw(FrameGeneration mode, bool fresh, bool pairReady,
    uint64_t now, uint64_t deadline, bool menuOpen, uint64_t lateMaxUs = 8000)
{
    if (fresh)
        return mode == FrameGeneration::Interpolate && pairReady && !deadline && !menuOpen
            ? VideoDraw::Generate : VideoDraw::Real;
    if (!deadline || (now < deadline && !menuOpen))
        return VideoDraw::Repeat;
    if (mode == FrameGeneration::Interpolate)
        return VideoDraw::Real;
    return !menuOpen && now - deadline < lateMaxUs ? VideoDraw::Generate : VideoDraw::Repeat;
}

}
