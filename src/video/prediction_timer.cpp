#include "prediction_timer.hpp"
#include "frame_queue.hpp"
#include "image_interop.hpp"

#include <chrono>

namespace ounce
{

PredictionTimer::PredictionTimer() : worker(&PredictionTimer::run, this) {}

PredictionTimer::~PredictionTimer()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_one();
    worker.join();
}

void PredictionTimer::arm(uint64_t timeUs)
{
    {
        std::lock_guard lock(mutex);
        deadline = timeUs;
    }
    changed.notify_one();
}

void PredictionTimer::run()
{
    std::unique_lock lock(mutex);
    while (!stopping)
    {
        const auto now = nowUs();
        if (!deadline)
            changed.wait(lock);
        else if (now < deadline)
            changed.wait_for(lock, std::chrono::microseconds(deadline - now));
        else
        {
            deadline = 0;
            switch2WakeVideoFrame();
        }
    }
}

}
