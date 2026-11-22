// The Switch 2 hardware H.264 decoder.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): Switch2VideoDecoder, fed by
// chiaki's video callback instead of Moonlight's decode units, with no FFmpeg.
#pragma once

#include "decoded_frame.hpp"
#include "frame_queue.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ounce
{

class Decoder
{
  public:
    explicit Decoder(FrameQueue& queue) : output(queue) {}
    ~Decoder();

    bool setup(int width, int height, int fps);
    void start();
    void stop();

    // One Annex B access unit from chiaki (or a header-only unit with SPS/PPS).
    // Returns false when the frame was not decoded; needsIdr() then says
    // whether a keyframe is required to continue.
    bool submit(const uint8_t* data, size_t size);
    uint64_t receivedPictures() const { return received.load(std::memory_order_relaxed); }
    bool needsIdr() const { return needIdr; }

    // Called (on the opener thread) when a decoder is ready for a keyframe.
    void setKeyframeNeeded(std::function<void()> callback);

  private:
    void startOpen();
    void rebuildDecoderSps();
    void closeHardware(); // with mutex held
    bool drain(uint64_t timeoutNs);
    void readFrames();
    void fail(const char* operation, Result rc);
    void report(uint64_t now);

    FrameQueue& output;
    std::shared_ptr<DecoderPool> pool;
    bool open = false, needIdr = true, firstFrame = false, loggedKeyframe = false;
    unsigned outputErrors = 0, decodedSinceOpen = 0;
    std::string lastDecoderSps;
    uint64_t openedUs = 0;
    Result lastOutputError = 0;
    int width = 0, height = 0, fps = 0;
    uint64_t retryAfterUs = 0;
    std::vector<uint8_t> sps, pps, sample;
    std::vector<uint8_t> decoderSps; // sps with the reference count the decoder can hold
    std::deque<std::pair<int64_t, uint64_t>> pending; // timestamp -> submit time
    std::atomic<bool> stopped { true };
    std::thread reader, opener;
    bool opening = false;
    std::function<void()> keyframeNeeded;
    unsigned generation = 0;
    std::mutex mutex;

    std::atomic<uint64_t> received { 0 };

    // Per-second report.
    uint64_t windowUs = 0;
    unsigned submitted = 0, decoded = 0, dropped = 0;
    uint64_t latencyUs = 0, latencyMaxUs = 0, submitUs = 0, submitMaxUs = 0;
};

}
