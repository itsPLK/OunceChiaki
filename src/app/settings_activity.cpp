#include "settings_activity.hpp"

#include "log.hpp"
#include "settings.hpp"

#include <algorithm>
#include <borealis/platforms/switch/switch2_vulkan.hpp>
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

// FSR only runs when the stream is smaller than the screen (renderer.cpp):
// grey it out otherwise, keeping the saved choice for lower resolutions.
void SettingsActivity::refreshUpscaling()
{
    if (!upscaling)
        return;
    const auto* ctx      = brls::Switch2VulkanContext::current();
    const uint32_t screen = ctx && ctx->extent.height ? ctx->extent.height : 1080;
    upscaling->setEnabled(uint32_t(settings().height) < screen);
    if (sharpening)
        sharpening->setVisibility(upscaling->isEnabled() && settings().upscaling ? brls::Visibility::VISIBLE
                                                                                 : brls::Visibility::GONE);
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
        refreshUpscaling();
    });
    box->addView(resolution);

    auto* fps = new brls::SelectorCell();
    fps->init("Frame rate", { "30 fps", "60 fps" }, settings().fps == 60 ? 1 : 0, [this](int index) {
        settings().fps = index ? 60 : 30;
        settings().save();
        refreshBitrate();
    });
    box->addView(fps);

    auto* frameGen = new brls::BooleanCell();
    frameGen->init("Frame generation", settings().frameGeneration != FrameGeneration::Off, [this](bool on) {
        settings().frameGeneration = !on ? FrameGeneration::Off
            : frameGenMode->getSelection() == 1 ? FrameGeneration::Interpolate : FrameGeneration::Extrapolate;
        frameGenMode->setVisibility(on ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        settings().save();
    });
    box->addView(frameGen);

    frameGenMode = new brls::SelectorCell();
    frameGenMode->init("Frame generation mode", { "Extrapolate", "Interpolate" },
        settings().frameGeneration == FrameGeneration::Interpolate ? 1 : 0, [](int index) {
            settings().frameGeneration = index ? FrameGeneration::Interpolate : FrameGeneration::Extrapolate;
            settings().save();
        });
    box->addView(frameGenMode);
    if (settings().frameGeneration == FrameGeneration::Off)
        frameGenMode->setVisibility(brls::Visibility::GONE);

    upscaling = new brls::BooleanCell();
    upscaling->init("FSR upscaling", settings().upscaling, [this](bool on) {
        settings().upscaling = on;
        settings().save();
        refreshUpscaling();
    });
    box->addView(upscaling);

    sharpening = new brls::SelectorCell();
    sharpening->init("FSR sharpening", { "Off", "Low", "Medium", "High" }, settings().sharpening, [](int index) {
        settings().sharpening = index;
        settings().save();
    });
    box->addView(sharpening);
    refreshUpscaling();

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
    note->setText("FSR upscaling: sharper edges when the resolution is below the screen's (no extra latency; "
                  "unavailable at the screen's resolution). "
                  "Force low-latency audio: audio never waits for a weak Wi-Fi to catch up, but may crackle on it.");
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

    auto* debugHeader = new brls::Header();
    debugHeader->setTitle("Troubleshooting");
    debugHeader->setMarginTop(30);
    box->addView(debugHeader);

    auto* debug = new brls::BooleanCell();
    debug->init("Debug log", settings().debugLog, [](bool on) {
        settings().debugLog = on;
        settings().save();
        setDebugLog(on);
    });
    box->addView(debug);

    auto* debugNote = new brls::Label();
    debugNote->setFontSize(16);
    debugNote->setMarginTop(20);
    debugNote->setText("Debug log: stream timing in logs/ounce.log every few seconds, for testing. "
                       "Off, the log keeps events and problems only.");
    box->addView(debugNote);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(box);
    auto* frame = new brls::AppletFrame(scroll);
    frame->setTitle("Settings");
    return frame;
}

}
