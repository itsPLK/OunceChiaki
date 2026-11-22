#include "settings_activity.hpp"

#include "settings.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace ounce
{

namespace
{

template <typename T>
int indexOf(const std::vector<T>& list, const T& value)
{
    const auto it = std::find(list.begin(), list.end(), value);
    return it == list.end() ? 0 : int(it - list.begin());
}

}

// "Auto (20 Mbps)" first, then the fixed choices.
std::vector<std::string> SettingsActivity::bitrateChoices() const
{
    const Settings& s = settings();
    std::vector<std::string> out { fmt::format("Auto ({})", bitrateLabel(autoBitrate(s.height, s.fps))) };
    for (int kbps : Settings::Bitrates)
        out.push_back(bitrateLabel(kbps));
    return out;
}

void SettingsActivity::refreshBitrate()
{
    if (!bitrate)
        return;
    const Settings& s = settings();
    bitrate->setData(bitrateChoices());
    bitrate->setSelection(s.bitrate ? indexOf(Settings::Bitrates, s.bitrate) + 1 : 0, true);
}

brls::View* SettingsActivity::createContentView()
{
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(20, 60, 20, 60);

    auto* header = new brls::Header();
    header->setTitle("Stream");
    box->addView(header);

    std::vector<std::string> heights;
    for (int h : Settings::Heights)
        heights.push_back(fmt::format("{}p", h));
    auto* resolution = new brls::SelectorCell();
    resolution->init("Resolution", heights, indexOf(Settings::Heights, settings().height), [this](int index) {
        settings().height = Settings::Heights[index];
        settings().save();
        refreshBitrate();
    });
    box->addView(resolution);

    auto* fps = new brls::SelectorCell();
    // Stream rate and display mode: index / 3 picks 30 or 60 fps, index % 3 the
    // frame generation (off, extrapolation, interpolation) that doubles it.
    const int rate = (settings().fps == 60 ? 3 : 0) + int(settings().frameGeneration);
    fps->init("Frame rate",
        { "30 fps", "30 to 60 fps (extrapolation)", "30 to 60 fps (interpolation)", "60 fps",
            "60 to 120 fps (extrapolation)", "60 to 120 fps (interpolation)" },
        rate, [this](int index) {
        settings().fps             = index >= 3 ? 60 : 30;
        settings().frameGeneration = FrameGeneration(index % 3);
        settings().save();
        refreshBitrate();
    });
    box->addView(fps);

    bitrate = new brls::SelectorCell();
    const Settings& s = settings();
    bitrate->init("Bitrate", bitrateChoices(), s.bitrate ? indexOf(Settings::Bitrates, s.bitrate) + 1 : 0, [](int index) {
        settings().bitrate = index ? Settings::Bitrates[index - 1] : 0;
        settings().save();
    });
    box->addView(bitrate);

    auto* lowLatency = new brls::BooleanCell();
    lowLatency->init("Force low-latency audio", settings().lowLatencyAudio, [](bool on) {
        settings().lowLatencyAudio = on;
        settings().save();
    });
    box->addView(lowLatency);

    auto* note = new brls::Label();
    note->setFontSize(16);
    note->setMarginTop(20);
    note->setText("Force low-latency audio: audio never waits for a weak Wi-Fi to catch up, but may crackle on it.");
    box->addView(note);

    auto* controllerHeader = new brls::Header();
    controllerHeader->setTitle("Controller");
    controllerHeader->setMarginTop(30);
    box->addView(controllerHeader);

    auto* motion = new brls::BooleanCell();
    motion->init("Motion controls", settings().motion, [](bool on) {
        settings().motion = on;
        settings().save();
    });
    box->addView(motion);

    auto* touchpad = new brls::BooleanCell();
    touchpad->init("Touchscreen as touchpad", settings().touchpad, [](bool on) {
        settings().touchpad = on;
        settings().save();
    });
    box->addView(touchpad);

    auto* rumble = new brls::SelectorCell();
    rumble->init("Rumble", { "Off", "Weak", "Normal", "Strong" }, settings().rumble, [](int index) {
        settings().rumble = index;
        settings().save();
    });
    box->addView(rumble);

    auto* controllerNote = new brls::Label();
    controllerNote->setFontSize(16);
    controllerNote->setMarginTop(20);
    controllerNote->setText("Motion uses the Joy-Cons or Pro Controller as the PlayStation controller's gyro. "
                            "Touching the screen moves fingers on the touchpad; a quick tap clicks it (Minus also clicks). "
                            "Rumble follows the game's rumble and, on PS5, its haptics.");
    box->addView(controllerNote);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(box);
    auto* frame = new brls::AppletFrame(scroll);
    frame->setTitle("Settings");
    return frame;
}

}
