// Plays the stream's Opus audio through audren.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): AudrenAudioRenderer.
#pragma once

#include "audio_policy.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <switch.h>

struct OpusDecoder;

namespace ounce
{

// One wave buffer per Opus packet (10 ms from the console), played as soon as
// it is decoded. More than 40 ms waiting is dropped rather than queued, so a
// stall can't leave a delay behind it (as CivChiaki's two 20 ms buffers). In
// adaptive mode the limit follows the network; in both, packets are resampled
// by a few samples to hold the queue against clock drift (audio_policy.hpp).
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
    uint64_t cushions = 0, shrunk = 0, stretched = 0, wShrunk = 0, wStretched = 0;
    int16_t* resized = nullptr; // a packet after the drift resample
    int slotSamples  = 0;       // a wave buffer: one packet plus the largest stretch
    void queue(const int16_t* samples, int count);

    // An audren voice holds 4 wave buffers (40 ms); the rest wait in the
    // driver until audrvUpdate() hands them over. Called only when a packet
    // arrived, a network stall over 40 ms ran the voice dry with audio still
    // waiting (crackles at any limit, 2026-11-22). This thread updates every 5 ms.
    void pump();
    std::mutex lock; // the driver and the wave buffers
    std::thread pumpThread;
    std::atomic<bool> pumping { false };
    // Times the voice ran out of audio (seen within 5 ms), and for how long.
    uint64_t drySpells = 0, wDrySpells = 0, dryUs = 0, wDryUs = 0;
    uint64_t dryStartUs = 0;
    bool wasPlaying     = false;

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
