// Captures the Switch's microphone (audin) in 10 ms stereo frames for the
// console's voice chat.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace ounce
{

class Microphone
{
  public:
    static constexpr int Rate = 48000, Channels = 2, FrameSamples = 480; // what chiaki sends: 10 ms

    // Called on the capture thread with FrameSamples interleaved stereo frames.
    using FrameCallback = std::function<void(const int16_t* pcm)>;

    ~Microphone() { stop(); }
    // Logs the input devices, opens the default one and starts capturing.
    bool start(FrameCallback onFrame);
    void stop();

    // Loudest sample since the last call (0..32767), for a level meter or a log.
    int takePeak() { return peak.exchange(0); }
    uint64_t frames() const { return frameCount.load(); }

  private:
    void run();
    void deliver(const int16_t* samples, size_t count);

    static constexpr int BufferCount = 4;
    FrameCallback callback;
    std::thread thread;
    std::atomic<bool> running { false };
    std::atomic<int> peak { 0 };
    std::atomic<uint64_t> frameCount { 0 };
    void* memory = nullptr; // BufferCount page-aligned capture buffers
    size_t bufferBytes = 0;
    int deviceChannels = Channels;
    std::vector<int16_t> pending; // samples waiting for a full frame
    bool opened = false;
};

}
