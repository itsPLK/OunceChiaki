#pragma once

#include "controller.hpp"
#include "decoder.hpp"
#include "frame_queue.hpp"
#include "renderer.hpp"
#include "stream.hpp"

#include <borealis.hpp>

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
    void frame();

  private:
    void pollInput();
    void openMenu();
    void close(const std::string& message = "");
    void report(uint64_t now);

    Host host;
    StreamSettings settings;
    FrameQueue queue;
    Decoder decoder { queue };
    Renderer renderer;
    AudioOutput audio;
    // Declared last: destroyed first, joining chiaki's threads before the
    // audio output, renderer and decoder go away.
    Stream stream { decoder, audio };
    FramePtr current;

    brls::Label* status = nullptr;
    bool connected = false, menuOpen = false, closing = false, inputBlocked = false;
    bool homeHeld = false;
    ChiakiControllerState lastSent {};
    bool sentOnce = false;

    uint64_t reportUs = 0, framesShown = 0, framesRepeated = 0;
};

}
