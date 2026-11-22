#pragma once

#include "controller.hpp"
#include "decoder.hpp"
#include "frame_queue.hpp"
#include "renderer.hpp"
#include "rumble.hpp"
#include "prediction_timer.hpp"
#include "stream.hpp"
#include "stream_overlay.hpp"

#include <optional>

#include <borealis.hpp>
#include <memory>

namespace ounce
{

// The stream: video full screen, the controller sent to the console
// (controller.hpp has the mapping). Home opens the stream menu.
class StreamActivity : public brls::Activity
{
  public:
    StreamActivity(Host host, StreamSettings settings);
    ~StreamActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

    // Called by the stream view on every frame, inside the Vulkan render pass.
    void frame(NVGcontext* vg, float x, float y);

  private:
    void startStream();
    void pollInput();
    void openMenu();
    void close(const std::string& message = "");
    void report(uint64_t now);

    Host host;
    StreamSettings settings;
    FrameQueue queue;
    Decoder decoder { queue };
    const GenerationTiming timing = GenerationTiming::forFps(settings.fps);
    uint64_t pairRejectGap = 0, pairRejectSpacing = 0, pairRejectStale = 0, pairRejectOther = 0;
    Renderer renderer { settings.frameGeneration };
    AudioOutput audio;
    // Declared last: destroyed first, joining chiaki's threads before the
    // audio output, renderer and decoder go away.
    Stream stream { decoder, audio };
    std::optional<StreamOverlay> overlay;
    FramePtr current, predictionHistory;
    std::unique_ptr<PredictionTimer> predictionTimer;
    uint64_t predictionDue = 0, realDrawUs = 0, lastPredictionUs = 0;
    uint64_t generated = 0, skippedPredictions = 0, supersededReal = 0;
    uint64_t realAgeUs = 0, realAgeMaxUs = 0, toPredictionUs = 0, fromPredictionUs = 0, followingReal = 0;

    brls::Label* status = nullptr;
    bool connected = false, menuOpen = false, closing = false, inputBlocked = false;
    bool homeHeld = false;
    int busyRetries = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true); // for delayed calls
    ChiakiControllerState lastSent {};
    bool sentOnce = false;
    std::optional<ControllerInput> controls;
    std::optional<Rumble> rumble; // only when rumble is on

    uint64_t reportUs = 0, framesShown = 0, framesRepeated = 0;
};

}
