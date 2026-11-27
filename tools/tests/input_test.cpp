// Host test for src/app/input_math.hpp.
#include "input_math.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ounce;
using Point = std::pair<uint16_t, uint16_t>;

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

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main()
{
    // Touchscreen corners land on the touchpad corners; out of range clamps.
    CHECK(touchpadPoint(0, 0) == Point(0, 0));
    CHECK(touchpadPoint(1279, 719) == Point(1919, 1078));
    CHECK(touchpadPoint(5000, 5000) == Point(1919, 1078));
    CHECK(touchpadPoint(640, 360).first == 960);

    // Taps: short and still.
    CHECK(isTap(1000, 1000 + 120000, 100, 100, 110, 95));
    CHECK(!isTap(1000, 1000 + 400000, 100, 100, 100, 100));  // held too long
    CHECK(!isTap(1000, 1000 + 100000, 100, 100, 200, 100));  // a swipe
    CHECK(!isTap(5000, 1000, 100, 100, 100, 100));           // clock went backwards

    // Haptics strength: silence and noise are zero, full scale is 1.
    std::vector<int16_t> quiet(60, 20);
    CHECK(hapticStrength(quiet.data(), 30) == std::make_pair(0.0f, 0.0f));
    CHECK(hapticStrength(nullptr, 30) == std::make_pair(0.0f, 0.0f));
    std::vector<int16_t> loud(60);
    for (size_t i = 0; i < 30; i++)
    {
        loud[i * 2]     = int16_t(i % 2 ? 16384 : -16384); // left: half scale
        loud[i * 2 + 1] = 0;                                // right: silent
    }
    const auto [l, r] = hapticStrength(loud.data(), 30);
    CHECK(near(l, 32768.0f / 65535.0f));
    CHECK(r == 0.0f);
    // A sparse click: one strong sample in a packet counts by half its peak.
    std::vector<int16_t> click(60, 0);
    click[20] = 30000;
    CHECK(near(hapticStrength(click.data(), 30).first, 30000.0f / 65536.0f));
    CHECK(hapticStrength(click.data(), 30).second == 0.0f);
    std::vector<int16_t> max(4, -32768);
    CHECK(hapticStrength(max.data(), 2).first == 1.0f);

    // Strength setting.
    CHECK(rumbleScale(0) == 0.0f && rumbleScale(2) == 1.0f && rumbleScale(-5) == 0.0f && rumbleScale(9) == 1.6f);

    // Mixing: classic rumble drives both sides by band, haptics per side; the
    // stronger wins and nothing exceeds 1.
    MotorLevels m = mixRumble(255, 0, 0, 0, 1.0f);
    CHECK(m.leftLow == 1.0f && m.rightLow == 1.0f && m.leftHigh == 0.0f && m.rightHigh == 0.0f);
    m = mixRumble(0, 0, 0.5f, 0.0f, 1.0f);
    CHECK(near(m.leftLow, 0.5f) && near(m.leftHigh, 0.5f) && m.rightLow == 0.0f);
    m = mixRumble(51, 0, 0.5f, 0.0f, 1.0f);
    CHECK(near(m.leftLow, 0.5f) && near(m.rightLow, 0.2f));
    m = mixRumble(255, 255, 1.0f, 1.0f, 1.6f);
    CHECK(m.leftLow == 1.0f && m.rightHigh == 1.0f);
    m = mixRumble(255, 255, 1.0f, 1.0f, 0.0f);
    CHECK(m == MotorLevels {});

    // Handheld motion: upright at rest reads as a flat DualSense; turning
    // left (about the console's top, SDL -z) is a yaw (+y), a steering-wheel
    // turn right (about the screen normal, SDL -y) is a roll right (-z).
    const float rest[3] = { 0.0f, 0.2f, -0.98f }, left[3] = { 0, 0, -1 }, wheel[3] = { 0, -1, 0 };
    CHECK((uprightToFlat(rest) == std::array<float, 3> { 0.0f, 0.98f, 0.2f }));
    CHECK((uprightToFlat(left) == std::array<float, 3> { 0, 1, 0 }));
    CHECK((uprightToFlat(wheel) == std::array<float, 3> { 0, 0, -1 }));

    // Levelling: the measured gravity goes to straight up, lengths kept, and
    // the frame is a rotation (a vector at right angles stays at right angles).
    auto nearVec = [&](std::array<float, 3> a, std::array<float, 3> b) {
        return near(a[0], b[0]) && near(a[1], b[1]) && near(a[2], b[2]);
    };
    for (const std::array<float, 3> g : { std::array<float, 3> { 0, 1, 0 }, { 0, 0, 1 }, { 0.3f, -0.4f, 0.87f },
             { -1, 0, 0 }, { 0, -1, 0 }, { 0.01f, -0.999f, 0.02f } })
    {
        const Frame r     = levelFrame(g);
        const float len   = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        CHECK(nearVec(applyFrame(r, g), { 0, len, 0 }));
        const auto x = applyFrame(r, { 1, 0, 0 }), y = applyFrame(r, { 0, 1, 0 }), z = applyFrame(r, { 0, 0, 1 });
        CHECK(near(x[0] * y[0] + x[1] * y[1] + x[2] * y[2], 0) && near(x[0] * z[0] + x[1] * z[1] + x[2] * z[2], 0));
        // Right-handed: x cross y is z.
        CHECK(nearVec({ x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] }, z));
    }
    CHECK(levelFrame({ 0, 0, 0 }) == IdentityFrame);

    if (failures)
        return 1;
    std::printf("input_test: ok\n");
    return 0;
}
