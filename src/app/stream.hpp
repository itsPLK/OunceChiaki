#pragma once

#include "audio.hpp"
#include "decoder.hpp"
#include "frame_generation.hpp"
#include "hosts.hpp"

#include <chiaki/session.h>
#include <functional>
#include <memory>
#include <string>

namespace ounce
{

struct StreamSettings
{
    int height  = 1080; // 360, 540, 720 or 1080
    int fps     = 60;   // 30 or 60
    FrameGeneration frameGeneration = FrameGeneration::Off;
    int bitrate = 20000; // kbps
    bool adaptiveNetwork = false;
    bool motion = true, touchpad = true;
    int rumble  = 2; // rumbleScale: 0 off .. 3 strong
};

// One Remote Play session: chiaki's session with CivChiaki's settings
// (docs/STREAMING.md). Video and audio go from chiaki's receive thread straight
// into the decoder and the audio output; events arrive on the UI thread.
class Stream
{
  public:
    struct Events
    {
        std::function<void()> connected;
        std::function<void(bool incorrect)> loginPin;
        std::function<void(const std::string& reason, bool error, ChiakiQuitReason code)> ended;
        // The game reset the motion origin (e.g. a recenter in a gyro aiming menu).
        std::function<void()> motionReset;
    };

    // What the console wants the controller to feel, read once per frame:
    // classic rumble (two motors, 0..255) and, on PS5, the haptics stream's
    // strength per side (0..1; zero once no haptics arrived for HapticHoldUs).
    struct Feedback
    {
        uint8_t heavy = 0, light = 0;
        float hapticLeft = 0, hapticRight = 0;
    };
    static constexpr uint64_t HapticHoldUs = 50000;

    Stream(Decoder& decoder, AudioOutput& audio) : decoder(decoder), audio(audio) {}
    ~Stream();

    bool start(const Host& host, const StreamSettings& settings, Events events);
    // Asks the session to end; `ended` follows on the UI thread.
    void stop();
    // Ends and releases the session (joins chiaki's threads) so start() can run again.
    void reset();
    void setController(const ChiakiControllerState& state);
    Feedback feedback(); // UI thread
    void loginPin(const std::string& pin);

    struct State;

  private:
    Decoder& decoder;
    AudioOutput& audio;
    std::shared_ptr<State> state;
    unsigned lastHapticPackets = 0;
    uint32_t lastHaptics       = 0;
};

}
