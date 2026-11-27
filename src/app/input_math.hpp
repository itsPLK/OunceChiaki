// Controller arithmetic with no console dependencies (tested by
// tools/tests/input_test.cpp).
#pragma once

#include <array>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace ounce
{

// The PlayStation touchpad's coordinates (as chiaki-ng uses them).
constexpr uint16_t TouchpadMaxX = 1920, TouchpadMaxY = 1079;

// The Switch touchscreen (1280x720 in HID coordinates) maps onto the whole touchpad.
inline std::pair<uint16_t, uint16_t> touchpadPoint(uint32_t x, uint32_t y)
{
    const uint32_t cx = std::min<uint32_t>(x, 1279), cy = std::min<uint32_t>(y, 719);
    return { uint16_t(cx * (TouchpadMaxX - 1) / 1279), uint16_t(cy * (TouchpadMaxY - 1) / 719) };
}

// A tap is a short touch that stays put: it presses the touchpad button.
constexpr uint64_t TapMaxUs     = 250000;
constexpr uint32_t TapMaxMove   = 24; // screen pixels
constexpr uint64_t ClickPulseUs = 100000;

inline bool isTap(uint64_t downUs, uint64_t upUs, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t dx = x0 > x1 ? x0 - x1 : x1 - x0, dy = y0 > y1 ? y0 - y1 : y1 - y0;
    return upUs >= downUs && upUs - downUs <= TapMaxUs && dx <= TapMaxMove && dy <= TapMaxMove;
}

// PS5 haptics arrive as 16-bit stereo audio (3 kHz, 10 ms per packet). A
// packet's strength per side, 0..1, is the larger of:
// - its mean absolute amplitude, doubled (chiaki-ng's measure, right for
//   steady vibration), with chiaki-ng's noise floor;
// - half its peak, so short clicks are felt: Astro's Playroom's right trigger
//   test sends sparse clicks peaking near full scale whose mean is only 0.03
//   (console log, 2026-11-22).
inline std::pair<float, float> hapticStrength(const int16_t* pcm, size_t frames)
{
    if (!pcm || !frames)
        return { 0.0f, 0.0f };
    uint64_t sum[2] = { 0, 0 };
    int peak[2]     = { 0, 0 };
    for (size_t i = 0; i < frames; i++)
        for (int side = 0; side < 2; side++)
        {
            const int a = std::abs(int(pcm[i * 2 + side]));
            sum[side] += uint64_t(a) * 2;
            peak[side] = std::max(peak[side], a);
        }
    auto level = [frames](uint64_t total, int top) {
        const uint64_t mean = total / frames;
        if (mean <= 100 && top < 1024)
            return 0.0f;
        return std::min(1.0f, std::max(float(mean) / 65535.0f, top / 65536.0f));
    };
    return { level(sum[0], peak[0]), level(sum[1], peak[1]) };
}

// Rumble strength setting: Off, Weak, Normal, Strong.
inline float rumbleScale(int setting)
{
    static const float scales[] = { 0.0f, 0.5f, 1.0f, 1.6f };
    return scales[std::clamp(setting, 0, 3)];
}

// HD rumble per side of the controller: low band (160 Hz), high band (320 Hz), 0..1.
struct MotorLevels
{
    float leftLow = 0, leftHigh = 0, rightLow = 0, rightHigh = 0;
    bool operator==(const MotorLevels&) const = default;
};

// Classic rumble (the console's two motors, 0..255: left the heavy
// low-frequency one, right the light high-frequency one) drives the low and
// high bands on both sides. Haptics drive each side on both bands. The
// stronger of the two wins.
inline MotorLevels mixRumble(uint8_t heavy, uint8_t light, float hapticLeft, float hapticRight, float scale)
{
    const float low = heavy / 255.0f * scale, high = light / 255.0f * scale;
    const float hl = std::min(1.0f, hapticLeft * scale), hr = std::min(1.0f, hapticRight * scale);
    MotorLevels m;
    m.leftLow   = std::min(1.0f, std::max(low, hl));
    m.leftHigh  = std::min(1.0f, std::max(high, hl));
    m.rightLow  = std::min(1.0f, std::max(low, hr));
    m.rightHigh = std::min(1.0f, std::max(high, hr));
    return m;
}

// Motion axes are SDL's (x right, y out of the face, z toward the player), a
// DualSense's when held flat. A console in handheld is held upright, screen
// to the player, so its turns came out on the wrong DualSense axes (turning
// left was a roll). This turns the upright pose into a flat controller: the
// console's top becomes the DualSense's far edge, the screen its face.
inline std::array<float, 3> uprightToFlat(const float v[3])
{
    return { v[0], -v[2], v[1] };
}

}
