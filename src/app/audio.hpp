// Plays the stream's Opus audio through audren.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): AudrenAudioRenderer.
#pragma once

#include "audio_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <switch.h>

struct OpusDecoder;

namespace ounce
{

// One wave buffer per Opus packet (10 ms from the console), played as soon as
// it is decoded. More than 40 ms waiting is dropped rather than queued, so a
// stall can't leave a delay behind it (as CivChiaki's two 20 ms buffers). In
// adaptive mode the limit follows the network (audio_policy.hpp).
class AudioOutput
{
  public:
    ~AudioOutput();

    // Before open(): low latency (false) or adaptive to an unstable network.
    void setAdaptive(bool on) { adaptive = on; }
    // From chiaki's audio header; called on chiaki's thread.
    bool open(unsigned channels, unsigned rate, unsigned frameSamples);
    void close();
    // One Opus packet, or null for a lost one (Opus conceals it).
    void play(const uint8_t* packet, size_t size);

  private:
    int queuedMs();
    int freeBuffer();

    // Enough 10 ms buffers for the adaptive limit plus its margin, and one more.
    static constexpr int BufferCount = (AudioPolicy::MaxMs + AudioPolicy::HardMarginMs) / 10 + 2;

    bool adaptive = false;
    AudioPolicy policy;
    int lastLimit = AudioPolicy::BaseMs;
    uint64_t trimmed = 0, cushions = 0, wTrimmed = 0;
    void queue(const int16_t* samples, int count);

    OpusDecoder* decoder = nullptr;
    int16_t* pcm         = nullptr;
    void* pool           = nullptr;
    AudioDriver driver {};
    AudioDriverWaveBuf buffers[BufferCount] {};
    bool driverOpen  = false;
    int channels     = 0;
    int rate         = 0;
    int frameSamples = 0;
    int bufferBytes  = 0;
    uint64_t played = 0, dropped = 0, concealed = 0, underruns = 0;
    // Diagnostics (audible cracks, 2026-11-22): every 5 s, what happened and
    // how much audio was queued when packets arrived.
    void report();
    uint64_t windowNs = 0, wPlayed = 0, wDropped = 0, wConcealed = 0, wUnderruns = 0;
    int wQueueMin = 1 << 30, wQueueMax = 0;
};

}
