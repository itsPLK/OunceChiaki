#include "frame_queue.hpp"

#include <chiaki/time.h>

namespace ounce
{

uint64_t nowUs() { return chiaki_time_now_monotonic_us(); }

void FrameQueue::configure(int fps)
{
    std::lock_guard lock(mutex);
    frames.clear();
    periodUs = fps > 0 ? 1000000 / fps : 16667;
    smooth   = fps >= 60;
    primed   = false;
    firstUs  = 0;
    counters = {};
}

void FrameQueue::clear()
{
    std::lock_guard lock(mutex);
    frames.clear();
    primed  = false;
    firstUs = 0;
}

void FrameQueue::push(FramePtr frame)
{
    frame->queuedUs = nowUs();
    std::lock_guard lock(mutex);
    frames.push_back(std::move(frame));
    counters.pushed++;
    while (frames.size() > 2)
    {
        frames.pop_front();
        counters.droppedBacklog++;
    }
}

FramePtr FrameQueue::take()
{
    const uint64_t now = nowUs();
    std::lock_guard lock(mutex);
    // Decode latency is already paid; only time spent waiting here counts.
    while (!frames.empty() && now - frames.front()->queuedUs > periodUs * 2)
    {
        frames.pop_front();
        counters.droppedAge++;
    }
    if (frames.empty())
        return nullptr;
    FramePtr shown;
    if (!smooth)
    {
        counters.droppedNewer += frames.size() - 1;
        shown = std::move(frames.back());
        frames.clear();
    }
    else
    {
        if (!firstUs)
            firstUs = now;
        if (!primed && frames.size() < 2 && now - firstUs < periodUs)
            return nullptr;
        primed = true;
        shown  = std::move(frames.front());
        frames.pop_front();
    }
    counters.shown++;
    counters.waitUs += now - shown->queuedUs;
    return shown;
}

bool FrameQueue::waiting()
{
    std::lock_guard lock(mutex);
    return !frames.empty();
}

FrameQueueStats FrameQueue::stats()
{
    std::lock_guard lock(mutex);
    return counters;
}

}
