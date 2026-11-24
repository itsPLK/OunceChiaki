// CivChiaki's presentation policy (docs/STREAMING.md), between the decoder
// thread and the render loop:
// - at most 2 decoded frames wait; a third drops the oldest ("backlog");
// - a frame that waited more than 2 frame periods is dropped ("age");
// - 30 fps: the newest frame is shown, older ones dropped;
// - 60 fps: the oldest waiting frame is shown, so one frame can absorb
//   arrival jitter; the first frame waits for a second one for at most one period.
#pragma once

#include "decoded_frame.hpp"

#include <deque>
#include <mutex>

namespace ounce
{

struct FrameQueueStats
{
    uint64_t pushed = 0, shown = 0, droppedBacklog = 0, droppedAge = 0, droppedNewer = 0;
    uint64_t waitUs = 0; // total time shown frames waited in the queue
};

class FrameQueue
{
  public:
    void configure(int fps);
    void clear();

    void push(FramePtr frame); // decoder thread
    FramePtr take();           // render loop; null when nothing new
    bool waiting();            // a frame is queued

    FrameQueueStats stats();

  private:
    std::mutex mutex;
    std::deque<FramePtr> frames;
    uint64_t periodUs   = 16667;
    bool smooth         = true; // 60 fps behavior
    bool primed         = false;
    uint64_t firstUs    = 0;
    FrameQueueStats counters;
};

uint64_t nowUs();

}
