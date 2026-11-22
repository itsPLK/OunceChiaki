#include "overlay_metrics.hpp"

#include <cassert>
#include <cmath>

int main()
{
    ounce::OverlayMetrics metrics;
    uint64_t now = 1000000, received = 42;
    metrics.sample(now, received, false, false, 0, 0);
    // Thirty received pictures, one dropped before display, plus 28 midpoints.
    for (unsigned i = 1; i <= 60; ++i)
    {
        now = 1000000 + i * 1000000 / 60;
        if (i % 2 == 0)
            ++received;
        bool real = i % 2 == 0 && i != 60;
        bool midpoint = i % 2 == 1 && i < 57;
        bool updated = metrics.sample(now, received, real, midpoint, now - 20000, now - 16000);
        assert(updated == (i == 60));
    }
    const auto& w = metrics.window;
    assert(w.receivedFps == 30 && w.realFps == 29 && w.generatedFps == 28);
    assert(w.hasDecode && w.hasReal && w.decodeMs == 4 && w.drawAgeMs == 16);
    assert(metrics.count == 56); // 57 new draws, no intervals attributed to repeats.
    assert(metrics.sample(now + 1000000, received, false, false, 0, 0));
    assert(w.realFps == 0 && w.receivedFps == 0 && !w.hasReal && !w.hasDecode);

    for (unsigned i = 1; i <= 150; ++i)
        metrics.sample(now + 1000000 + i * 20000, ++received, true, false, 0, now + 1000000 + i * 20000);
    assert(metrics.count == metrics.intervalsMs.size());
    for (float ms : metrics.intervalsMs)
        assert(std::abs(ms - 20) < 0.01);
    assert(w.hasReal && !w.hasDecode);
}
