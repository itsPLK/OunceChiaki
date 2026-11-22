// Decoded frames that lease a Switch 2 hardware decoder output slot.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): Switch2NativeFrame.hpp,
// without FFmpeg's AVFrame.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <switch2/video/decoder.h>
}

namespace ounce
{

// The SDK decoder session plus output slots released by frames. A frame (and so
// the pool) can outlive the decoder session: the renderer keeps the last frame
// until the GPU has finished reading it.
struct DecoderPool
{
    VideoDecoder decoder {};
    bool open = false;
    std::mutex releaseMutex;
    std::vector<VideoFrame> released;

    DecoderPool() { released.reserve(VIDDEC_MAX_IMAGES); }
    ~DecoderPool()
    {
        if (open)
            videoDecoderClose(&decoder);
    }
    // Stops the native session (it holds the decoder's pixel budget) while
    // output images stay alive for frames still held.
    void retireDecoder()
    {
        if (decoder.pending_armed)
        {
            gsvSyncDisarmEvent(&decoder.sync, decoder.event_slot);
            decoder.pending_armed = false;
        }
        if (decoder.started)
        {
            viddecDecoderStop(&decoder.decoder);
            decoder.started = false;
        }
        if (decoder.decoder_open)
        {
            viddecDecoderClose(&decoder.decoder);
            decoder.decoder_open = false;
        }
    }
    // Frames are destroyed on any thread, sometimes under other locks: they
    // only queue their slot, and the decoder returns it to the SDK.
    void deferRelease(const VideoFrame& frame)
    {
        std::lock_guard lock(releaseMutex);
        released.push_back(frame);
    }
    // Called only by the decoder, under its SDK mutex.
    void releaseFrames()
    {
        std::lock_guard lock(releaseMutex);
        for (auto& frame : released)
            videoDecoderReleaseFrame(&decoder, &frame);
        released.clear();
    }
};

struct DecodedFrame
{
    std::shared_ptr<DecoderPool> pool;
    VideoFrame frame {};
    uint32_t width = 0, height = 0;
    uint64_t submittedUs = 0; // when its access unit went to the decoder
    uint64_t decodedUs   = 0; // when the decoder returned it
    uint64_t queuedUs    = 0; // when it entered the frame queue

    ~DecodedFrame()
    {
        if (pool)
            pool->deferRelease(frame);
    }
};

using FramePtr = std::shared_ptr<DecodedFrame>;

}
