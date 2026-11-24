#include "stream_activity.hpp"

#include "log.hpp"
#include "stream_menu.hpp"

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
    audio.setAdaptive(settings.adaptiveAudio);
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
    const auto mode = settings.frameGeneration;
    // Interpolation: while the deferred real picture waits for its present,
    // leave the next one queued. Taken at once, a picture that arrives early (a
    // Wi-Fi burst) superseded the deferred one and lost its own midpoint too.
    // It is taken on the pass after the deferred present instead, no later than
    // its normal cadence.
    // Extrapolation: a picture arriving before the pending prediction skipped
    // it. Hold the picture only when the prediction is due within holdMaxUs
    // (6 ms at 60 fps), so a held real picture waits at most that.
    bool holdForDeferred = mode == FrameGeneration::Interpolate && predictionDue && !menuOpen;
    if (mode == FrameGeneration::Extrapolate && predictionDue && !menuOpen && queue.waiting())
    {
        const int64_t leftUs = int64_t(predictionDue) - int64_t(nowUs());
        if (!overtakeSeen)
        {
            overtakeSeen = true;
            overtakeEarly[leftUs <= 2000 ? 0 : leftUs <= 4000 ? 1 : leftUs <= 6000 ? 2 : 3]++;
        }
        holdForDeferred = leftUs <= int64_t(timing.holdMaxUs);
    }
    FramePtr fresh;
    if (holdForDeferred)
        holding |= queue.waiting();
    else
        fresh = queue.take();
    if (fresh)
        overtakeSeen = false;
    const bool newPicture = bool(fresh);
    const uint64_t now = nowUs();
    const bool heldFresh = fresh && holding;
    if (heldFresh)
    {
        const uint64_t wait = now - fresh->queuedUs;
        heldPictures++;
        heldWaitUs += wait;
        heldWaitMaxUs = std::max(heldWaitMaxUs, wait);
    }
    if (!holdForDeferred)
        holding = false;
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
        else if (fresh->decodedUs < current->decodedUs || spacing > timing.pairMaxUs)
            pairRejectSpacing++;
        // A held picture's age counts from the deferred present that released it.
        else if (now - std::max(fresh->decodedUs, heldFresh ? holdReleasedUs : 0) >= timing.freshMaxUs)
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
            {
                skippedPredictions++;
                skippedOvertaken++;
            }
        }
        predictionDue = 0;
        holdExtrapolated = false;
        predictionHistory = pairReady ? std::move(current) : nullptr;
        current = std::move(fresh);
        if (status && status->getVisibility() == brls::Visibility::VISIBLE)
            status->setVisibility(brls::Visibility::GONE);
    }

    // Record a draw on every Borealis pass. Between the two scheduled
    // presents its frame-driven loop discards unchanged command buffers.
    // After an extrapolated draw, repeats keep using its motion field so a
    // 34 ms keepalive doesn't snap the image backwards to the real frame.
    const bool wantGenerate = draw == VideoDraw::Generate;
    const bool usedPredict  = current && renderer.draw(current, wantGenerate ? predictionHistory : nullptr,
        !wantGenerate && holdExtrapolated && !menuOpen);
    const bool synthetic    = wantGenerate && usedPredict;
    const bool real = current && (draw == VideoDraw::Real || (wantGenerate && !synthetic && newPicture));
    if (real)
    {
        holdExtrapolated = false;
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
        if (mode == FrameGeneration::Extrapolate)
            holdExtrapolated = true;
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
        {
            skippedPredictions++;
            skippedLate++;
        }
        predictionDue = 0;
        predictionHistory.reset();
    }
    // The deferred present (or a prediction skipped as late) ended a hold:
    // take the held picture on the next pass.
    if (holding && !predictionDue)
    {
        holdReleasedUs = now;
        if (context && queue.waiting())
            context->wakeFrame();
    }
    if (context)
        context->videoFrameChanged(real || synthetic);
    // Drawn with the menu open too: it sits top left, clear of the panel.
    if (overlay && current)
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
    if (home && !homeHeld)
    {
        homeHeld = true;
        if (!menuOpen)
            openMenu();
        else
            closeMenu();
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
    holdExtrapolated = false;
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

    StreamMenu::Actions actions;
    actions.console = host.name;
    actions.statsOn = overlay.has_value();
    actions.micOn   = micOn;
    actions.back    = [this] { closeMenu(); };
    actions.stats   = [this](bool on) {
        if (on)
            overlay.emplace();
        else
            overlay.reset();
    };
    actions.microphone = [this](bool on) {
        if (!stream.setMicrophone(on) && on)
            return false;
        micOn = on;
        return true;
    };
    actions.disconnect = [this](bool sleep) {
        if (!sleep)
        {
            close();
            return;
        }
        stream.sleepConsole();
        // The request goes out on chiaki's control connection: give it a
        // moment before the session closes (chiaki-ng's client stops right after).
        std::weak_ptr<bool> token = alive;
        brls::delay(500, [this, token] {
            if (!token.expired())
                close();
        });
    };
    menuShown = true;
    brls::Application::pushActivity(new brls::Activity(new StreamMenu(std::move(actions))), brls::TransitionAnimation::NONE);
}

// Back to the game.
void StreamActivity::closeMenu()
{
    if (!menuShown)
        return;
    menuShown = false;
    brls::Application::popActivity(brls::TransitionAnimation::NONE, [this] {
        menuOpen = false;
        brls::Application::blockInputs(true);
        inputBlocked = true;
    });
}

void StreamActivity::close(const std::string& message)
{
    if (closing)
        return;
    closing = true;
    if (rumble)
        rumble->stop();
    stream.stop();
    const bool withMenu = menuShown;
    menuShown           = false;
    brls::sync([message, withMenu] {
        if (withMenu)
            brls::Application::popActivity(brls::TransitionAnimation::NONE); // the menu, then the stream
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
    // Without the debug log, only a window with congestion signals is logged.
    const bool debug = debugLog();
    if (debug || core.congestion_signals)
        brls::Logger::info("Stream: shown {} new, {} repeated in 5 s; queue wait {:.1f} ms avg; backlog {} ms, "
                           "congestion signals {}",
            framesShown, framesRepeated, queueStats.shown ? queueStats.waitUs / 1000.0 / queueStats.shown : 0.0,
            core.backlog_ms, core.congestion_signals);
    if (debug)
    {
        brls::Logger::info("Video timing: mode={} generated={} skipped={} supersededReal={} real decode-to-draw "
                           "avg/max={:.1f}/{:.1f} ms; real-to-generated/generated-to-real draw intervals={:.1f}/{:.1f} ms",
            settings.frameGeneration == FrameGeneration::Interpolate ? "interpolation" :
                settings.frameGeneration == FrameGeneration::Extrapolate ? "extrapolation" : "off",
            generated, skippedPredictions, supersededReal, framesShown ? realAgeUs / 1000.0 / framesShown : 0.0,
            realAgeMaxUs / 1000.0, generated ? toPredictionUs / 1000.0 / generated : 0.0,
            followingReal ? fromPredictionUs / 1000.0 / followingReal : 0.0);
        if (settings.frameGeneration != FrameGeneration::Off)
            brls::Logger::info("Video timing: pairs rejected: gap {} spacing {} stale {} other {}; held {} (wait avg/max "
                               "{:.1f}/{:.1f} ms); skipped predictions overtaken {} late {}",
                pairRejectGap, pairRejectSpacing, pairRejectStale, pairRejectOther, heldPictures,
                heldPictures ? heldWaitUs / 1000.0 / heldPictures : 0.0, heldWaitMaxUs / 1000.0, skippedOvertaken,
                skippedLate);
        if (settings.frameGeneration == FrameGeneration::Extrapolate)
            brls::Logger::info("Video timing: pictures before a pending prediction, by time left: <=2 ms {}, "
                               "2-4 ms {}, 4-6 ms {}, >6 ms {}",
                overtakeEarly[0], overtakeEarly[1], overtakeEarly[2], overtakeEarly[3]);
    }
    pairRejectGap = pairRejectSpacing = pairRejectStale = pairRejectOther = 0;
    overtakeEarly = {};
    heldPictures = heldWaitUs = heldWaitMaxUs = skippedOvertaken = skippedLate = 0;
    generated = skippedPredictions = supersededReal = realAgeUs = realAgeMaxUs = 0;
    toPredictionUs = fromPredictionUs = followingReal = 0;
    framesShown = framesRepeated = 0;
    reportUs                     = now;
}

}
