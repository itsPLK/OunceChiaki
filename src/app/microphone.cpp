#include "microphone.hpp"

#include <algorithm>
#include <borealis.hpp>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <switch.h>

namespace ounce
{

namespace
{
AudioInBuffer audioBuffers[4];
}

bool Microphone::start(FrameCallback onFrame)
{
    stop();
    callback = std::move(onFrame);
    brls::Logger::info("Microphone: starting audin");
    Result rc = audinInitialize();
    if (R_FAILED(rc))
    {
        brls::Logger::warning("Microphone: audinInitialize failed rc=0x{:x}", rc);
        return false;
    }
    opened = true;
    char names[8][0x100] {};
    u32 count = 0;
    rc        = audinListAudioIns(&names[0][0], 8, &count);
    brls::Logger::info("Microphone: {} input device(s) (rc=0x{:x})", count, rc);
    for (u32 i = 0; i < std::min<u32>(count, 8); i++)
        brls::Logger::info("Microphone: device {}: \"{}\"", i, names[i]);
    deviceChannels = int(audinGetChannelCount());
    brls::Logger::info("Microphone: default input {} Hz, {} channel(s), PCM format {}, state {}", audinGetSampleRate(),
        deviceChannels, int(audinGetPcmFormat()), int(audinGetDeviceState()));
    if (audinGetSampleRate() != Rate || audinGetPcmFormat() != PcmFormat_Int16 || deviceChannels < 1 || deviceChannels > 2)
    {
        brls::Logger::warning("Microphone: unsupported input format");
        stop();
        return false;
    }

    // One 10 ms frame per buffer, in page-aligned memory.
    const size_t data = size_t(FrameSamples) * deviceChannels * sizeof(int16_t);
    bufferBytes       = (data + 0xfff) & ~size_t(0xfff);
    memory            = memalign(0x1000, bufferBytes * BufferCount);
    if (!memory)
    {
        stop();
        return false;
    }
    std::memset(memory, 0, bufferBytes * BufferCount);
    for (int i = 0; i < BufferCount; i++)
    {
        audioBuffers[i]             = {};
        audioBuffers[i].buffer      = static_cast<char*>(memory) + i * bufferBytes;
        audioBuffers[i].buffer_size = bufferBytes;
        audioBuffers[i].data_size   = data;
        rc                          = audinAppendAudioInBuffer(&audioBuffers[i]);
        if (R_FAILED(rc))
        {
            brls::Logger::warning("Microphone: append buffer failed rc=0x{:x}", rc);
            stop();
            return false;
        }
    }
    rc = audinStartAudioIn();
    brls::Logger::info("Microphone: start rc=0x{:x}", rc);
    if (R_FAILED(rc))
    {
        stop();
        return false;
    }
    pending.clear();
    frameCount = 0;
    running    = true;
    thread     = std::thread([this] { run(); });
    return true;
}

void Microphone::stop()
{
    running = false;
    if (thread.joinable())
        thread.join();
    if (opened)
    {
        audinStopAudioIn();
        audinExit();
        opened = false;
        brls::Logger::info("Microphone: stopped after {} frames", frameCount.load());
    }
    std::free(memory);
    memory = nullptr;
}

void Microphone::run()
{
    bool first = true;
    while (running)
    {
        AudioInBuffer* released = nullptr;
        u32 count               = 0;
        const Result rc         = audinWaitCaptureFinish(&released, &count, 100000000ull);
        if (R_FAILED(rc) || !released)
            continue;
        while (released)
        {
            if (first)
            {
                brls::Logger::info("Microphone: first buffer, {} bytes", released->data_size);
                first = false;
            }
            deliver(static_cast<const int16_t*>(released->buffer), released->data_size / sizeof(int16_t));
            audinAppendAudioInBuffer(released);
            released = nullptr;
            count    = 0;
            if (R_FAILED(audinGetReleasedAudioInBuffer(&released, &count)) || !count)
                break;
        }
    }
}

// Collects whole 10 ms stereo frames (a mono device is doubled to stereo).
void Microphone::deliver(const int16_t* samples, size_t count)
{
    int loudest = 0;
    for (size_t i = 0; i < count; i++)
    {
        loudest = std::max(loudest, std::abs(int(samples[i])));
        pending.push_back(samples[i]);
        if (deviceChannels == 1)
            pending.push_back(samples[i]);
    }
    if (loudest > peak.load())
        peak = loudest;
    const size_t frame = size_t(FrameSamples) * Channels;
    while (pending.size() >= frame)
    {
        if (callback)
            callback(pending.data());
        frameCount++;
        pending.erase(pending.begin(), pending.begin() + frame);
    }
}

}
