// How much audio may wait, and what to do with a packet that arrives when too
// much already does. No console dependencies (tested by tools/tests/audio_test.cpp).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace ounce
{

enum class AudioAction
{
    Play,
    Drop, // audible audio skipped: a crack
    Trim, // a quiet packet skipped: inaudible
};

// Low latency (the default): more than BaseMs waiting is dropped.
//
// Adaptive (the "Unstable Wi-Fi" setting): after a Wi-Fi hiccup the delayed
// audio arrives at once. Instead of dropping what exceeds the limit, the limit
// rises by RaiseMs (up to MaxMs); above it only quiet packets are skipped, and
// audible ones only past the limit plus HardMarginMs. After StableUs without
// trouble the limit falls back by FallMs, towards BaseMs.
class AudioPolicy
{
  public:
    static constexpr int BaseMs = 40, MaxMs = 120, RaiseMs = 20, FallMs = 10, HardMarginMs = 40;
    static constexpr uint64_t StableUs = 10000000;

    explicit AudioPolicy(bool adaptive = false) : adaptive(adaptive) {}

    AudioAction decide(int queuedMs, bool quiet, uint64_t nowUs)
    {
        relax(nowUs);
        if (!adaptive)
            return queuedMs > BaseMs ? AudioAction::Drop : AudioAction::Play;
        if (queuedMs > limitMs + HardMarginMs)
        {
            raise(nowUs);
            return AudioAction::Drop;
        }
        if (queuedMs > limitMs)
        {
            if (quiet)
                return AudioAction::Trim;
            raise(nowUs); // the burst outgrew the limit: make room rather than crack
        }
        return AudioAction::Play;
    }

    // The output ran dry: in adaptive mode, worth a little more cushion.
    void underrun(uint64_t nowUs)
    {
        if (adaptive)
            troubleUs = nowUs;
    }

    int limit() const { return adaptive ? limitMs : BaseMs; }
    bool isAdaptive() const { return adaptive; }

  private:
    void raise(uint64_t nowUs)
    {
        limitMs   = std::min(MaxMs, limitMs + RaiseMs);
        troubleUs = nowUs;
    }

    void relax(uint64_t nowUs)
    {
        if (!troubleUs)
            troubleUs = nowUs;
        if (limitMs > BaseMs && nowUs - troubleUs >= StableUs)
        {
            limitMs   = std::max(BaseMs, limitMs - FallMs);
            troubleUs = nowUs;
        }
    }

    bool adaptive;
    int limitMs        = BaseMs;
    uint64_t troubleUs = 0;
};

// Quiet: mean absolute amplitude below about -36 dBFS.
inline bool quietPcm(const int16_t* pcm, size_t samples)
{
    if (!pcm || !samples)
        return true;
    uint64_t sum = 0;
    for (size_t i = 0; i < samples; i++)
        sum += uint64_t(std::abs(int(pcm[i])));
    return sum / samples < 500;
}

}
