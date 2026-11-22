#include "stream_overlay.hpp"

#include <fmt/format.h>
#include <nanovg.h>

namespace ounce
{

void StreamOverlay::update(uint64_t now, uint64_t received, bool real, bool generated, uint64_t submitted, uint64_t decoded)
{
    if (!metrics.sample(now, received, real, generated, submitted, decoded))
        return;
    measureText = true;
    const auto& w = metrics.window;
    rates = fmt::format("Received {:.1f}    Shown {:.1f} FPS", w.receivedFps, w.realFps + w.generatedFps);
    breakdown = fmt::format("Real {:.1f}    Generated {:.1f} FPS", w.realFps, w.generatedFps);
    const auto ms = [](bool valid, double value) { return valid ? fmt::format("{:.1f} ms", value) : "—"; };
    latency = fmt::format("Decode {}    Decoded to draw {}", ms(w.hasDecode, w.decodeMs), ms(w.hasReal, w.drawAgeMs));
}

void StreamOverlay::draw(NVGcontext* vg, float x, float y, int font)
{
    constexpr float Height = 150, Pad = 12, GraphHeight = 48;
    constexpr float RateSize = 19, DetailSize = 15, CaptionSize = 13;
    constexpr char caption[] = "Draw interval (ms)   ·   last 120 intervals";
    const float left = x + Pad, bottom = y + Height - 8;
    nvgSave(vg);
    nvgFontFaceId(vg, font);
    nvgFontQuality(vg, 1.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    if (measureText)
    {
        // Use the console font's actual advances. Keep the widest sample so
        // the panel and graph stay still as the number of digits changes.
        const auto fit = [&](float size, const char* line) {
            nvgFontSize(vg, size);
            const float width = nvgTextBounds(vg, 0, 0, line, nullptr, nullptr);
            panelWidth = std::max(panelWidth, width + 2 * Pad);
        };
        fit(RateSize, rates.c_str());
        fit(DetailSize, breakdown.c_str());
        fit(DetailSize, latency.c_str());
        fit(CaptionSize, caption);
        measureText = false;
    }
    const float graphWidth = panelWidth - 2 * Pad - 39;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, panelWidth, Height, 6);
    nvgFillColor(vg, nvgRGBA(10, 16, 22, 222));
    nvgFill(vg);
    nvgFillColor(vg, nvgRGB(246, 249, 252));
    nvgFontSize(vg, RateSize);
    nvgText(vg, left, y + 7, rates.c_str(), nullptr);
    nvgFontSize(vg, DetailSize);
    nvgText(vg, left, y + 32, breakdown.c_str(), nullptr);
    nvgText(vg, left, y + 54, latency.c_str(), nullptr);
    nvgFontSize(vg, CaptionSize);
    nvgFillColor(vg, nvgRGB(185, 201, 213));
    nvgText(vg, left, y + 79, caption, nullptr);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    // 8.3, 16.7 and 33.3 ms: one frame at 120, 60 and 30 fps.
    static const float guides[] = { 8.333f, 16.667f, 33.333f };
    static const char* labels[] = { "8.3", "16.7", "33.3" };
    for (int i = 0; i < 3; ++i)
    {
        const float guide = bottom - GraphHeight * guides[i] / 50.0f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, left, guide);
        nvgLineTo(vg, left + graphWidth, guide);
        nvgStrokeWidth(vg, 1);
        nvgStrokeColor(vg, nvgRGBA(190, 208, 219, 85));
        nvgStroke(vg);
        nvgText(vg, x + panelWidth - Pad, guide - 8, labels[i], nullptr);
    }
    const size_t capacity = metrics.intervalsMs.size();
    const size_t first = (metrics.next + capacity - metrics.count) % capacity;
    nvgBeginPath(vg);
    for (size_t i = 0; i < metrics.count; ++i)
    {
        const float px = left + graphWidth * (capacity - metrics.count + i) / (capacity - 1);
        const float ms = metrics.intervalsMs[(first + i) % capacity];
        const float py = bottom - GraphHeight * std::min(ms / 50.0f, 1.0f);
        if (i == 0)
            nvgMoveTo(vg, px, py);
        else
            nvgLineTo(vg, px, py);
    }
    nvgStrokeWidth(vg, 1.6f);
    nvgStrokeColor(vg, nvgRGB(106, 224, 195));
    nvgStroke(vg);
    nvgRestore(vg);
}

}
