// How much audio may wait, what to do with a packet that arrives when too much
// already does, and how to keep the queue level against clock drift. No console
// dependencies (tested by tools/tests/audio_test.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ounce
{

enum class AudioAction
{
    Play,
    Drop, // audible audio skipped: a crack
};

// Low latency ("Force low-latency audio"): more than BaseMs waiting is dropped.
//
// Adaptive (the default): after a Wi-Fi hiccup the delayed
// audio arrives at once. Instead of dropping what exceeds the limit, the limit
// rises by RaiseMs (up to MaxMs); audible audio is dropped only past the limit
// plus HardMarginMs. After StableUs without trouble the limit falls back by
// FallMs, towards BaseMs. For the first StartupUs it acts as low latency: the
// burst at connect isn't a bad network, and raising the limit for it left
// 120 ms of delay that took 80 s to fall back (2026-11-22).
//
// Both: the console's audio clock runs about 0.25% fast against audren's
// (2026-11-22: the queue grew 60 ms a minute on a clean network, one 10 ms
// drop every few seconds). resize() says how many samples to take from or add
// to a packet so the queue stays near target(): a resample of under 1%, which
// can't be heard, instead of a drop, which can.
class AudioPolicy
{
  public:
    static constexpr int BaseMs = 40, MaxMs = 120, RaiseMs = 20, FallMs = 10, HardMarginMs = 40;
    static constexpr int TargetBelowLimitMs = 20;
    static constexpr int SmallStep = 2, LargeStep = 4; // samples per 480-sample packet
    static constexpr uint64_t StableUs = 10000000, StartupUs = 2000000;

    explicit AudioPolicy(bool adaptive = false) : adaptive(adaptive) {}

    AudioAction decide(int queuedMs, uint64_t nowUs)
    {
        if (!startUs)
            startUs = nowUs;
        relax(nowUs);
        if (!adaptive || nowUs - startUs < StartupUs)
            return queuedMs > BaseMs ? AudioAction::Drop : AudioAction::Play;
        if (queuedMs > limitMs + HardMarginMs)
        {
            raise(nowUs);
            return AudioAction::Drop;
        }
        if (queuedMs > limitMs)
            raise(nowUs); // the burst outgrew the limit: make room rather than crack
        return AudioAction::Play;
    }

    // Samples to add (positive) or remove (negative) from the next packet.
    // queuedMs counts whole 10 ms buffers; the average smooths that out.
    int resize(int queuedMs)
    {
        if (!primed)
        {
            level  = queuedMs;
            primed = true;
        }
        level += (queuedMs - level) / 16.0;
        const double over = level - target();
        if (over > 15)
            return -LargeStep;
        if (over > 5)
            return -SmallStep;
        if (over < -10)
            return SmallStep;
        return 0;
    }

    // The output ran dry: in adaptive mode, worth a little more cushion.
    void underrun(uint64_t nowUs)
    {
        if (adaptive)
            troubleUs = nowUs;
    }

    int limit() const { return adaptive ? limitMs : BaseMs; }
    int target() const { return limit() - TargetBelowLimitMs; }
    double smoothedMs() const { return level; }
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
    uint64_t troubleUs = 0, startUs = 0;
    double level       = 0;
    bool primed        = false;
};

// Linear resample of interleaved frames, keeping the first and last frame so
// consecutive packets still join where they did.
inline void resampleLinear(const int16_t* in, int inFrames, int16_t* out, int outFrames, int channels)
{
    if (outFrames == inFrames || inFrames < 2 || outFrames < 2)
    {
        std::copy(in, in + size_t(std::min(inFrames, outFrames)) * channels, out);
        return;
    }
    const double step = double(inFrames - 1) / double(outFrames - 1);
    for (int j = 0; j < outFrames; j++)
    {
        const double pos = j * step;
        const int i      = std::min(int(pos), inFrames - 2);
        const double f   = pos - i;
        for (int c = 0; c < channels; c++)
        {
            const double a = in[size_t(i) * channels + c], b = in[size_t(i + 1) * channels + c];
            out[size_t(j) * channels + c] = int16_t(std::lround(a + (b - a) * f));
        }
    }
}

}
