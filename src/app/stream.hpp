#pragma once

#include "audio.hpp"
#include "decoder.hpp"
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
    int bitrate = 20000; // kbps
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
    };

    Stream(Decoder& decoder, AudioOutput& audio) : decoder(decoder), audio(audio) {}
    ~Stream();

    bool start(const Host& host, const StreamSettings& settings, Events events);
    // Asks the session to end; `ended` follows on the UI thread.
    void stop();
    // Ends and releases the session (joins chiaki's threads) so start() can run again.
    void reset();
    void setController(const ChiakiControllerState& state);
    void loginPin(const std::string& pin);

    struct State;

  private:
    Decoder& decoder;
    AudioOutput& audio;
    std::shared_ptr<State> state;
};

}
