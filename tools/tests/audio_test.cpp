// Host test for src/app/audio_policy.hpp.
#include "audio_policy.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace ounce;

static int failures = 0;
#define CHECK(cond)                                                    \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

int main()
{
    const uint64_t s = 1000000;

    // Low latency: as before, over 40 ms is dropped, loud or quiet.
    AudioPolicy fixed(false);
    CHECK(fixed.decide(40, 1 * s) == AudioAction::Play);
    CHECK(fixed.decide(50, 1 * s) == AudioAction::Drop);
    CHECK(fixed.decide(50, 1 * s) == AudioAction::Drop);
    CHECK(fixed.limit() == 40);

    // Adaptive: a packet over the limit is played and raises it.
    AudioPolicy a(true);
    a.decide(0, 1); // connected at 1 us; the startup grace ends at 2 s
    CHECK(a.decide(30, 4 * s) == AudioAction::Play);
    CHECK(a.limit() == 40);
    CHECK(a.decide(50, 4 * s) == AudioAction::Play);
    CHECK(a.limit() == 60);
    // Far over the limit: dropped, and the limit rises.
    CHECK(a.decide(110, 5 * s) == AudioAction::Drop);
    CHECK(a.limit() == 80);
    // Never above 120 ms.
    for (int i = 0; i < 10; i++)
        a.decide(500, 6 * s);
    CHECK(a.limit() == 120);
    // Ten quiet seconds lower it by 10 ms; it comes back to 40 in steps.
    a.decide(10, 15 * s);
    CHECK(a.limit() == 120);
    a.decide(10, 16 * s);
    CHECK(a.limit() == 110);
    for (uint64_t t = 17; t < 203; t++)
        a.decide(10, t * s);
    CHECK(a.limit() == 40);
    // An underrun counts as trouble: it delays the next step down.
    AudioPolicy b(true);
    b.decide(0, 1);
    b.decide(50, 4 * s); // 60
    b.underrun(12 * s);
    b.decide(10, 15 * s);
    CHECK(b.limit() == 60);
    b.decide(10, 22 * s);
    CHECK(b.limit() == 50);

    // The burst at connect: dropped as in low latency, the limit stays.
    AudioPolicy c(true);
    CHECK(c.decide(110, 1 * s) == AudioAction::Drop);
    CHECK(c.decide(50, 2 * s) == AudioAction::Drop);
    CHECK(c.limit() == 40);
    CHECK(c.decide(50, 3 * s + 1) == AudioAction::Play);
    CHECK(c.limit() == 60);

    // Drift: a queue that creeps up is held near the target by shrinking
    // packets, one that runs low is stretched, and one near the target is left.
    {
        AudioPolicy d(false); // target 20 ms
        CHECK(d.target() == 20);
        CHECK(d.resize(20) == 0);
        CHECK(d.resize(30) == 0); // one reading barely moves the average
        for (int i = 0; i < 100; i++)
            d.resize(30);
        CHECK(d.resize(30) == -AudioPolicy::SmallStep);
        for (int i = 0; i < 100; i++)
            d.resize(40);
        CHECK(d.resize(40) == -AudioPolicy::LargeStep);
        for (int i = 0; i < 200; i++)
            d.resize(0);
        CHECK(d.resize(0) == AudioPolicy::SmallStep);
    }
    // Simulated: the console sends 0.25% fast; resizing keeps the queue bounded
    // with no drops (without it, the queue passes 40 ms within seconds).
    {
        AudioPolicy d(false);
        double queuedSamples = 960; // 20 ms
        int drops = 0, maxMs = 0;
        for (int packet = 0; packet < 100 * 600; packet++) // 10 minutes
        {
            queuedSamples -= 480 * 1.0025; // what audren played since the last packet, in console samples
            queuedSamples = std::max(0.0, queuedSamples);
            const int ms  = int(queuedSamples / 480) * 10;
            maxMs         = std::max(maxMs, ms);
            if (d.decide(ms, uint64_t(packet) * 10000) == AudioAction::Drop)
            {
                drops++;
                continue;
            }
            queuedSamples += 480 + d.resize(ms);
        }
        CHECK(drops == 0);
        CHECK(maxMs <= 40);
        // Played faster than sent (the opposite sign) must hold too.
        AudioPolicy e(false);
        queuedSamples = 960;
        drops = 0, maxMs = 0;
        for (int packet = 0; packet < 100 * 600; packet++)
        {
            queuedSamples -= 480 * 0.9975;
            queuedSamples = std::max(0.0, queuedSamples);
            const int ms  = int(queuedSamples / 480) * 10;
            maxMs         = std::max(maxMs, ms);
            if (e.decide(ms, uint64_t(packet) * 10000) == AudioAction::Drop)
            {
                drops++;
                continue;
            }
            queuedSamples += 480 + e.resize(ms);
        }
        CHECK(drops == 0);
        CHECK(maxMs <= 40);
    }

    // Resample: the ends are kept, a ramp stays a ramp.
    {
        std::vector<int16_t> in(480 * 2), out(484 * 2);
        for (int i = 0; i < 480; i++)
            in[i * 2] = in[i * 2 + 1] = int16_t(i * 10);
        resampleLinear(in.data(), 480, out.data(), 476, 2);
        CHECK(out[0] == 0 && out[475 * 2] == 4790 && out[475 * 2 + 1] == 4790);
        bool rising = true;
        for (int j = 1; j < 476; j++)
            rising = rising && out[j * 2] > out[(j - 1) * 2];
        CHECK(rising);
        resampleLinear(in.data(), 480, out.data(), 484, 2);
        CHECK(out[0] == 0 && out[483 * 2] == 4790);
    }

    if (failures)
        return 1;
    std::printf("audio_test: ok\n");
    return 0;
}
