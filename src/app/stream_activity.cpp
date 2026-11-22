#include "stream_activity.hpp"

#include "ounce_core.h"

#include <borealis/platforms/switch/switch2_vulkan.hpp>
#include <borealis/platforms/switch/switch_input.hpp>
#include <fmt/format.h>

namespace ounce
{

namespace
{

brls::SwitchInputManager* switchInput()
{
    return static_cast<brls::SwitchInputManager*>(brls::Application::getPlatform()->getInputManager());
}

void systemButtons(brls::ButtonOverrideMode mode)
{
    auto* input = switchInput();
    input->setHomeButtonOverrideMode(mode);
    input->setScreenshotButtonOverrideMode(mode);
#ifdef __SWITCH2__
    input->setChatButtonOverrideMode(mode);
#endif
}

// Draws the stream inside Borealis's frame; the status label sits on top.
class StreamView : public brls::Box
{
  public:
    explicit StreamView(StreamActivity* owner) : owner(owner)
    {
        setFocusable(true);
        setHideHighlight(true);
    }
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override
    {
        owner->frame();
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    StreamActivity* owner;
};

}

StreamActivity::StreamActivity(Host host, StreamSettings settings) : host(std::move(host)), settings(settings) {}

StreamActivity::~StreamActivity()
{
    if (auto* context = brls::Switch2VulkanContext::current())
        context->setFrameDriven(false);
    if (inputBlocked)
        brls::Application::unblockInputs();
    systemButtons(brls::ButtonOverrideMode::NONE);
    // Members end in reverse order: the session joins first, then the audio
    // output closes, the renderer waits for the GPU and the decoder stops.
    current.reset();
}

brls::View* StreamActivity::createContentView()
{
    auto* view = new StreamView(this);
    view->setAxis(brls::Axis::COLUMN);
    view->setJustifyContent(brls::JustifyContent::CENTER);
    view->setAlignItems(brls::AlignItems::CENTER);
    status = new brls::Label();
    status->setFontSize(24);
    status->setText(fmt::format("Connecting to {}...", host.name));
    view->addView(status);
    return view;
}

void StreamActivity::onContentAvailable()
{
    systemButtons(brls::ButtonOverrideMode::CUSTOM_EVENT);
    brls::Application::blockInputs(true);
    inputBlocked = true;

    const int width = settings.height * 16 / 9;
    if (!decoder.setup(width, settings.height, settings.fps))
    {
        close("The video decoder could not be set up.");
        return;
    }
    decoder.start();
    Stream::Events events;
    events.connected = [this] {
        connected = true;
        status->setText(fmt::format("Connected to {}. Waiting for video...", host.name));
    };
    events.loginPin = [this](bool incorrect) {
        brls::Application::unblockInputs();
        inputBlocked = false;
        brls::Application::getImeManager()->openForNumber(
            [this](long pin) {
                stream.loginPin(fmt::format("{:04d}", pin));
                brls::Application::blockInputs(true);
                inputBlocked = true;
            },
            incorrect ? "Wrong PIN. Console login PIN" : "Console login PIN", "The 4-digit PIN of your PlayStation login",
            4);
    };
    events.ended = [this](const std::string& reason, bool error) {
        close(error ? fmt::format("The stream ended: {}", reason) : "");
    };
    if (!stream.start(host, settings, std::move(events)))
        close("The stream could not start (see the log).");
    reportUs = nowUs();
}

void StreamActivity::frame()
{
    if (closing)
        return;
    pollInput();
    auto* context = brls::Switch2VulkanContext::current();
    if (context && !menuOpen)
        context->setFrameDriven(true);
    FramePtr fresh = queue.take();
    if (fresh)
    {
        current = std::move(fresh);
        framesShown++;
        if (status && status->getVisibility() == brls::Visibility::VISIBLE)
            status->setVisibility(brls::Visibility::GONE);
    }
    else if (current)
        framesRepeated++;
    if (current)
        renderer.draw(current);
    if (context)
        context->videoFrameChanged(fresh != nullptr);
    report(nowUs());
}

void StreamActivity::pollInput()
{
    auto* input = switchInput();
    brls::ControllerState pad {};
    input->updateUnifiedControllerState(&pad);
    const bool home = input->isHomeButtonPressed();
    if (!menuOpen && home && !homeHeld)
    {
        homeHeld = true;
        openMenu();
        return;
    }
    homeHeld = home;
    if (menuOpen || !connected)
        return;
    SystemButtons system;
#ifdef __SWITCH2__
    system.chat = input->isChatButtonPressed();
#endif
    system.capture = input->isScreenshotButtonPressed();
    ChiakiControllerState state = mapController(pad, system);
    if (!sentOnce || !chiaki_controller_state_equals(&state, &lastSent))
    {
        stream.setController(state);
        lastSent = state;
        sentOnce = true;
    }
}

void StreamActivity::openMenu()
{
    menuOpen = true;
    // The console sees all buttons released while the menu is open.
    ChiakiControllerState idle;
    chiaki_controller_state_set_idle(&idle);
    stream.setController(idle);
    lastSent = idle;
    if (auto* context = brls::Switch2VulkanContext::current())
        context->setFrameDriven(false);
    brls::Application::unblockInputs();
    inputBlocked = false;
    auto* dialog = new brls::Dialog(fmt::format("Streaming {}", host.name));
    dialog->addButton("Back to the game", [this] {
        menuOpen = false;
        brls::Application::blockInputs(true);
        inputBlocked = true;
    });
    dialog->addButton("Disconnect", [this] { close(); });
    dialog->setCancelable(false);
    dialog->open();
}

void StreamActivity::close(const std::string& message)
{
    if (closing)
        return;
    closing = true;
    stream.stop();
    brls::sync([message] {
        brls::Application::popActivity(brls::TransitionAnimation::FADE, [message] {
            if (message.empty())
                return;
            auto* dialog = new brls::Dialog(message);
            dialog->addButton("OK", [] {});
            dialog->open();
        });
    });
}

void StreamActivity::report(uint64_t now)
{
    if (now - reportUs < 5000000)
        return;
    const OunceCoreStats core = ounce_core_stats();
    const auto queueStats     = queue.stats();
    brls::Logger::info("Stream: shown {} new, {} repeated in 5 s; queue wait {:.1f} ms avg; backlog {} ms, "
                       "congestion signals {}",
        framesShown, framesRepeated, queueStats.shown ? queueStats.waitUs / 1000.0 / queueStats.shown : 0.0,
        core.backlog_ms, core.congestion_signals);
    framesShown = framesRepeated = 0;
    reportUs                     = now;
}

}
