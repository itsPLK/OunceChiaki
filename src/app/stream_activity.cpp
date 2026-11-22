#include "stream_activity.hpp"

#include "ounce_core.h"

#include <borealis/platforms/switch/switch2_vulkan.hpp>
#include <borealis/platforms/switch/switch_input.hpp>
#include <fmt/format.h>

namespace ounce
{

namespace
{

constexpr int MaxBusyRetries = 8;

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
        owner->frame(vg, x, y);
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    StreamActivity* owner;
};

}

StreamActivity::StreamActivity(Host host, StreamSettings settings) : host(std::move(host)), settings(settings) {}

StreamActivity::~StreamActivity()
{
    predictionTimer.reset();
    alive.reset();
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
    controls.emplace(settings.motion, settings.touchpad);
    if (settings.rumble > 0)
        rumble.emplace();
    startStream();
    reportUs = nowUs();
}

void StreamActivity::startStream()
{
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
    events.motionReset = [this] {
        if (controls)
            controls->resetMotion();
    };
    events.ended = [this](const std::string& reason, bool error, ChiakiQuitReason code) {
        // Right after a disconnect the console still holds the last session for
        // a few seconds: wait and ask again instead of failing.
        if (code == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE && !connected && !closing && busyRetries < MaxBusyRetries)
        {
            busyRetries++;
            status->setText(fmt::format("Waiting for {} to finish the last session...", host.name));
            brls::Logger::info("Stream: console busy, retrying ({}/{})", busyRetries, MaxBusyRetries);
            std::weak_ptr<bool> token = alive;
            brls::delay(1000, [this, token] {
                if (token.expired() || closing)
                    return;
                stream.reset();
                startStream();
            });
            return;
        }
        close(error ? fmt::format("The stream ended: {}", reason) : "");
    };
    if (!stream.start(host, settings, std::move(events)))
        close("The stream could not start (see the log).");
}

void StreamActivity::frame(NVGcontext* vg, float x, float y)
{
    if (closing)
        return;
    pollInput();
    auto* context = brls::Switch2VulkanContext::current();
    if (context && !menuOpen)
        context->setFrameDriven(true);
    FramePtr fresh = queue.take();
    const bool newPicture = bool(fresh);
    const uint64_t now = nowUs();
    const auto mode = settings.frameGeneration;
    // A pair is two consecutive pictures (no loss or drop between them) of the
    // same decoder and size. The timing window only guards against stalls:
    // arrival jitter on Wi-Fi is a large part of a 60 fps frame period.
    bool pairReady = false;
    if (fresh && current && mode != FrameGeneration::Off && !menuOpen)
    {
        const uint64_t spacing = fresh->decodedUs - current->decodedUs;
        if (!renderer.canPredict() || fresh->pool != current->pool || fresh->width != current->width
            || fresh->height != current->height)
            pairRejectOther++;
        else if (fresh->sequence != current->sequence + 1)
            pairRejectGap++;
        else if (fresh->decodedUs < current->decodedUs || spacing < timing.pairMinUs || spacing > timing.pairMaxUs)
            pairRejectSpacing++;
        else if (now - fresh->decodedUs >= timing.freshMaxUs)
            pairRejectStale++;
        else
            pairReady = true;
    }
    const auto draw = chooseVideoDraw(mode, newPicture, pairReady, now, predictionDue, menuOpen, timing.lateMaxUs);
    if (fresh)
    {
        if (predictionDue)
        {
            if (mode == FrameGeneration::Interpolate)
                supersededReal++;
            else
                skippedPredictions++;
        }
        predictionDue = 0;
        predictionHistory = pairReady ? std::move(current) : nullptr;
        current = std::move(fresh);
        if (status && status->getVisibility() == brls::Visibility::VISIBLE)
            status->setVisibility(brls::Visibility::GONE);
    }

    // Record a draw on every Borealis pass. Between the two scheduled
    // presents its frame-driven loop discards unchanged command buffers.
    const bool synthetic = current && renderer.draw(current, draw == VideoDraw::Generate ? predictionHistory : nullptr);
    const bool real = current && (draw == VideoDraw::Real || (draw == VideoDraw::Generate && !synthetic && newPicture));
    if (real)
    {
        if (lastPredictionUs)
        {
            fromPredictionUs += now - lastPredictionUs;
            followingReal++;
            lastPredictionUs = 0;
        }
        realDrawUs = now;
        const auto age = now - current->decodedUs;
        realAgeUs += age;
        realAgeMaxUs = std::max(realAgeMaxUs, age);
        framesShown++;
    }
    else if (synthetic)
    {
        generated++;
        toPredictionUs += now - realDrawUs;
        lastPredictionUs = now;
    }
    else if (current)
        framesRepeated++;

    if (newPicture && pairReady && (mode == FrameGeneration::Extrapolate || synthetic))
    {
        if (!predictionTimer)
            predictionTimer = std::make_unique<PredictionTimer>();
        predictionDue = now + timing.offsetUs;
        predictionTimer->arm(predictionDue);
        // Interpolation only needs B for the second present. The graphics
        // fence keeps A alive for the GPU after this reference is released.
        if (mode == FrameGeneration::Interpolate)
            predictionHistory.reset();
    }
    else if (newPicture || (predictionDue && (now >= predictionDue || menuOpen)))
    {
        if ((draw == VideoDraw::Generate && !synthetic) ||
            (predictionDue && mode == FrameGeneration::Extrapolate && !synthetic))
            skippedPredictions++;
        predictionDue = 0;
        predictionHistory.reset();
    }
    if (context)
        context->videoFrameChanged(real || synthetic);
    if (overlay && current && !menuOpen)
    {
        overlay->update(now, decoder.receivedPictures(), real, synthetic, current->submittedUs, current->decodedUs);
        overlay->draw(vg, x + 14, y + 14, status->getFont());
    }
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
    if (menuOpen || !connected || closing || !controls)
        return;
    SystemButtons system;
#ifdef __SWITCH2__
    system.chat = input->isChatButtonPressed();
#endif
    system.capture = input->isScreenshotButtonPressed();
    ChiakiControllerState state = controls->read(pad, system);
    if (rumble)
    {
        const Stream::Feedback feel = stream.feedback();
        rumble->set(mixRumble(feel.heavy, feel.light, feel.hapticLeft, feel.hapticRight, rumbleScale(settings.rumble)));
    }
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
    predictionHistory.reset();
    lastPredictionUs = 0;
    // The console sees all buttons released while the menu is open.
    ChiakiControllerState idle;
    chiaki_controller_state_set_idle(&idle);
    stream.setController(idle);
    lastSent = idle;
    if (controls)
        controls->releaseTouches();
    if (rumble)
        rumble->stop();
    if (auto* context = brls::Switch2VulkanContext::current())
        context->setFrameDriven(false);
    brls::Application::unblockInputs();
    inputBlocked = false;
    auto* dialog = new brls::Dialog(fmt::format("Streaming {}", host.name));
    auto resume = [this] {
        menuOpen = false;
        brls::Application::blockInputs(true);
        inputBlocked = true;
    };
    dialog->addButton("Back to the game", [this, resume] {
        if (overlay)
            overlay.emplace(); // Exclude menu time from the rate and interval samples.
        resume();
    });
    dialog->addButton(overlay ? "Hide stream stats" : "Show stream stats", [this, resume] {
        if (overlay)
            overlay.reset();
        else
            overlay.emplace();
        resume();
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
    if (rumble)
        rumble->stop();
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
    brls::Logger::info("Video timing: mode={} generated={} skipped={} supersededReal={} real decode-to-draw avg/max={:.1f}/{:.1f} ms; "
                       "real-to-generated/generated-to-real draw intervals={:.1f}/{:.1f} ms",
        settings.frameGeneration == FrameGeneration::Interpolate ? "interpolation" :
            settings.frameGeneration == FrameGeneration::Extrapolate ? "extrapolation" : "off",
        generated, skippedPredictions, supersededReal, framesShown ? realAgeUs / 1000.0 / framesShown : 0.0,
        realAgeMaxUs / 1000.0, generated ? toPredictionUs / 1000.0 / generated : 0.0,
        followingReal ? fromPredictionUs / 1000.0 / followingReal : 0.0);
    if (settings.frameGeneration != FrameGeneration::Off)
        brls::Logger::info("Video timing: pairs rejected: gap {} spacing {} stale {} other {}", pairRejectGap,
            pairRejectSpacing, pairRejectStale, pairRejectOther);
    pairRejectGap = pairRejectSpacing = pairRejectStale = pairRejectOther = 0;
    generated = skippedPredictions = supersededReal = realAgeUs = realAgeMaxUs = 0;
    toPredictionUs = fromPredictionUs = followingReal = 0;
    framesShown = framesRepeated = 0;
    reportUs                     = now;
}

}
