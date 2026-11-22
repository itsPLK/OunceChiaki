// Host test for src/app/audio_policy.hpp.
#include "audio_policy.hpp"

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
    CHECK(fixed.decide(40, false, 1 * s) == AudioAction::Play);
    CHECK(fixed.decide(50, false, 1 * s) == AudioAction::Drop);
    CHECK(fixed.decide(50, true, 1 * s) == AudioAction::Drop);
    CHECK(fixed.limit() == 40);

    // Adaptive: a quiet packet over the limit is trimmed, a loud one raises it.
    AudioPolicy a(true);
    CHECK(a.decide(30, false, 1 * s) == AudioAction::Play);
    CHECK(a.decide(50, true, 1 * s) == AudioAction::Trim);
    CHECK(a.limit() == 40);
    CHECK(a.decide(50, false, 1 * s) == AudioAction::Play);
    CHECK(a.limit() == 60);
    // Far over the limit: dropped, and the limit rises.
    CHECK(a.decide(110, false, 2 * s) == AudioAction::Drop);
    CHECK(a.limit() == 80);
    // Never above 120 ms.
    for (int i = 0; i < 10; i++)
        a.decide(500, false, 3 * s);
    CHECK(a.limit() == 120);
    // Ten quiet seconds lower it by 10 ms; it comes back to 40 in steps.
    a.decide(10, false, 12 * s);
    CHECK(a.limit() == 120);
    a.decide(10, false, 13 * s);
    CHECK(a.limit() == 110);
    for (uint64_t t = 14; t < 200; t++)
        a.decide(10, false, t * s);
    CHECK(a.limit() == 40);
    // An underrun counts as trouble: it delays the next step down.
    AudioPolicy b(true);
    b.decide(50, false, 1 * s); // 60
    b.underrun(9 * s);
    b.decide(10, false, 12 * s);
    CHECK(b.limit() == 60);
    b.decide(10, false, 19 * s);
    CHECK(b.limit() == 50);

    // Quiet detection.
    std::vector<int16_t> hush(960, 100), loud(960, 8000);
    CHECK(quietPcm(hush.data(), hush.size()));
    CHECK(!quietPcm(loud.data(), loud.size()));

    if (failures)
        return 1;
    std::printf("audio_test: ok\n");
    return 0;
}
