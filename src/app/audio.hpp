// Plays the stream's Opus audio through audren.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): AudrenAudioRenderer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <switch.h>

struct OpusDecoder;

namespace ounce
{

// One wave buffer per Opus packet (10 ms from the console), played as soon as
// it is decoded. More than MaxQueuedMs waiting is dropped rather than queued,
// so a stall can't leave a delay behind it (as CivChiaki's two 20 ms buffers).
class AudioOutput
{
  public:
    ~AudioOutput();

    // From chiaki's audio header; called on chiaki's thread.
    bool open(unsigned channels, unsigned rate, unsigned frameSamples);
    void close();
    // One Opus packet, or null for a lost one (Opus conceals it).
    void play(const uint8_t* packet, size_t size);

  private:
    int queuedMs();
    int freeBuffer();

    static constexpr int BufferCount = 12;
    static constexpr int MaxQueuedMs = 40;

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
