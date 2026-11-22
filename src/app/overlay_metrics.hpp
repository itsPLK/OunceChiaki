#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>

namespace ounce
{

struct OverlayWindow
{
    double receivedFps = 0, realFps = 0, generatedFps = 0;
    double decodeMs = 0, drawAgeMs = 0;
    bool hasDecode = false, hasReal = false;
};

// Updated by the render thread. 'received' counts complete H.264 pictures at
// decoder input, including pictures the decoder subsequently drops.
struct OverlayMetrics
{
    OverlayWindow window;
    std::array<float, 120> intervalsMs {};
    std::size_t next = 0, count = 0;

    // True when a new one-second text snapshot is ready. The graph gets a
    // point only for a real or generated draw; renderer keepalives are ignored.
    bool sample(uint64_t now, uint64_t received, bool real, bool generated, uint64_t submitted, uint64_t decoded)
    {
        if (!start)
        {
            start = now;
            firstReceived = received;
            lastDraw = real || generated ? now : 0;
            return false;
        }
        if (real || generated)
        {
            if (lastDraw)
            {
                intervalsMs[next] = (now - lastDraw) / 1000.0f;
                next = (next + 1) % intervalsMs.size();
                count = std::min(count + 1, intervalsMs.size());
            }
            lastDraw = now;
        }
        if (real)
        {
            realCount++;
            ageUs += now - decoded;
            if (submitted && submitted <= decoded)
            {
                decodeCount++;
                decodeUs += decoded - submitted;
            }
        }
        generatedCount += generated;
        const auto elapsed = now - start;
        if (elapsed < 1000000)
            return false;
        const double hz = 1000000.0 / elapsed;
        window.receivedFps = (received - firstReceived) * hz;
        window.realFps = realCount * hz;
        window.generatedFps = generatedCount * hz;
        window.hasDecode = decodeCount != 0;
        window.hasReal = realCount != 0;
        window.decodeMs = decodeCount ? decodeUs / (1000.0 * decodeCount) : 0;
        window.drawAgeMs = realCount ? ageUs / (1000.0 * realCount) : 0;
        firstReceived = received;
        start = now;
        realCount = generatedCount = decodeCount = ageUs = decodeUs = 0;
        return true;
    }

  private:
    uint64_t start = 0, firstReceived = 0, lastDraw = 0;
    uint64_t realCount = 0, generatedCount = 0, decodeCount = 0, ageUs = 0, decodeUs = 0;
};

}
