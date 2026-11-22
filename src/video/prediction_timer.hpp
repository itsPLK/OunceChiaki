#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ounce
{

// The frame-driven render loop sleeps between incoming frames. Wake it once
// for the predicted picture. Only the render thread decides what to show.
class PredictionTimer
{
  public:
    PredictionTimer();
    ~PredictionTimer();
    void arm(uint64_t timeUs);

  private:
    void run();
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t deadline = 0;
    bool stopping = false;
    std::thread worker;
};

}
