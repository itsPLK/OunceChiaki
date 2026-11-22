#include "stream_menu.hpp"

#include <borealis/views/widgets/battery.hpp>
#include <borealis/views/widgets/wireless.hpp>
#include <ctime>
#include <fmt/format.h>
#include <switch.h>

namespace ounce
{

namespace
{

constexpr float PanelWidth = 420;

brls::Label* label(float size, NVGcolor color)
{
    auto* l = new brls::Label();
    l->setFontSize(size);
    l->setTextColor(color);
    return l;
}

brls::Header* header(const std::string& title)
{
    auto* h = new brls::Header();
    h->setTitle(title);
    h->setMarginTop(18);
    return h;
}

// Logged once per run: what Horizon allows of the screen's settings.
void logBrightness()
{
    static bool logged = false;
    if (logged)
        return;
    logged          = true;
    float level     = 0;
    bool autoOn     = false, sensor = false;
    const Result lrc = lblGetCurrentBrightnessSetting(&level);
    const Result arc = lblIsAutoBrightnessControlEnabled(&autoOn);
    const Result src = lblIsAmbientLightSensorAvailable(&sensor);
    brls::Logger::info("Menu: brightness {:.2f} (rc=0x{:x}), auto {} (rc=0x{:x}), light sensor {} (rc=0x{:x})", level, lrc,
        autoOn, arc, sensor, src);
}

}

StreamMenu::StreamMenu(Actions given) : brls::Box(brls::Axis::ROW), actions(std::move(given))
{
    setWidth(brls::View::AUTO);
    setHeight(brls::View::AUTO);
    setWidthPercentage(100);
    setHeightPercentage(100);

    // Left: the stream, slightly dimmed. Touching it goes back to the game.
    auto* shade = new brls::Box();
    shade->setGrow(1);
    shade->setBackgroundColor(nvgRGBA(0, 0, 0, 70));
    shade->addGestureRecognizer(new brls::TapGestureRecognizer(shade, [this] {
        if (actions.back)
            actions.back();
    }));
    addView(shade);

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(PanelWidth);
    panel->setHeightPercentage(100);
    panel->setPadding(30, 30, 24, 30);
    panel->setBackgroundColor(nvgRGBA(30, 30, 34, 245));
    addView(panel);

    const NVGcolor text = nvgRGB(240, 240, 240), dim = nvgRGB(160, 160, 168), danger = nvgRGB(255, 96, 96);

    // The time, big, with the Wi-Fi and battery beside it.
    auto* top = new brls::Box(brls::Axis::ROW);
    top->setAlignItems(brls::AlignItems::CENTER);
    clock = label(40, text);
    clock->setGrow(1);
    top->addView(clock);
    top->addView(new brls::WirelessWidget());
    battery = label(16, dim);
    battery->setMarginLeft(14);
    top->addView(battery);
    auto* batteryIcon = new brls::BatteryWidget();
    batteryIcon->setMarginLeft(6);
    top->addView(batteryIcon);
    panel->addView(top);

    // The stream. The frame reaches into the panel's right padding so its
    // scrollbar (drawn 14 px in from the frame's edge) clears the switches.
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setMarginRight(-24);
    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setPaddingRight(24);
    scroll->setContentView(list);
    panel->addView(scroll);

    list->addView(header(fmt::format("Streaming {}", actions.console)));
    auto* stats = new brls::BooleanCell();
    stats->init("Stream stats", actions.statsOn, [this](bool on) {
        if (actions.stats)
            actions.stats(on);
    });
    list->addView(stats);
    auto* mic = new brls::BooleanCell();
    mic->init("Microphone", actions.micOn, [this, mic](bool on) {
        if (actions.microphone && !actions.microphone(on) && on)
        {
            mic->setOn(false);
            brls::Application::notify("The microphone could not be started.");
        }
    });
    list->addView(mic);

    // The screen.
    logBrightness();
    // The slider on its own row, the full width of the panel (a SliderCell
    // shares its row with the title and a value slot).
    list->addView(header("Brightness"));
    float level = 0.5f;
    lblGetCurrentBrightnessSetting(&level);
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(70);
    row->setPadding(0, 16, 0, 16);
    auto* brightness = new brls::Slider();
    brightness->setGrow(1);
    brightness->setProgress(level);
    brightness->getProgressEvent()->subscribe([](float value) {
        lblSetCurrentBrightnessSetting(std::clamp(value, 0.0f, 1.0f));
        lblApplyCurrentBrightnessSettingToBacklight();
    });
    row->addView(brightness);
    list->addView(row);
    bool autoOn = false;
    lblIsAutoBrightnessControlEnabled(&autoOn);
    auto* autoBrightness = new brls::BooleanCell();
    autoBrightness->init("Auto-brightness", autoOn, [](bool on) {
        const Result rc = on ? lblEnableAutoBrightnessControl() : lblDisableAutoBrightnessControl();
        bool now        = false;
        lblIsAutoBrightnessControlEnabled(&now);
        brls::Logger::info("Menu: auto-brightness {} rc=0x{:x}; now {}", on ? "on" : "off", rc, now);
    });
    list->addView(autoBrightness);

    // Leaving.
    list->addView(header("Disconnect"));
    auto* leave = new brls::DetailCell();
    leave->setText("Disconnect");
    leave->setTextColor(danger);
    leave->registerClickAction([this](brls::View*) {
        if (actions.disconnect)
            actions.disconnect(false);
        return true;
    });
    list->addView(leave);
    auto* sleep = new brls::DetailCell();
    sleep->setText("Disconnect and sleep");
    sleep->setTextColor(danger);
    sleep->registerClickAction([this](brls::View*) {
        if (actions.disconnect)
            actions.disconnect(true);
        return true;
    });
    list->addView(sleep);

    auto* hint = label(15, dim);
    hint->setText("B or Home: back to the game");
    hint->setMarginTop(12);
    panel->addView(hint);

    registerAction("Back to the game", brls::BUTTON_B, [this](brls::View*) {
        if (actions.back)
            actions.back();
        return true;
    });

    update();
    timer.setCallback([this] { update(); });
    timer.start(1000);
}

StreamMenu::~StreamMenu() { timer.stop(); }

void StreamMenu::update()
{
    const std::time_t now = std::time(nullptr);
    const std::tm local   = *std::localtime(&now);
    clock->setText(fmt::format("{:02d}:{:02d}", local.tm_hour, local.tm_min));

    auto* platform = brls::Application::getPlatform();
    battery->setText(fmt::format("{}%{}", platform->getBatteryLevel(), platform->isBatteryCharging() ? ", charging" : ""));
}

}
