#pragma once

#include "input_math.hpp"

#include <switch.h>

namespace ounce
{

// Player 1's HD rumble: the handheld Joy-Cons, or a Pro Controller / pair of
// Joy-Cons. Each side has a low band (160 Hz) and a high band (320 Hz).
// Borealis's sendRumble fixes the strength and varies the frequency, which
// doesn't fit game rumble, so this drives libnx directly. UI thread only.
class Rumble
{
  public:
    Rumble();
    ~Rumble();
    void set(const MotorLevels& levels);
    void stop() { set({}); }

  private:
    // Each controller style has its own vibration handles.
    enum Target { Handheld, ProController, JoyConPair, Targets, None = Targets };
    Target target() const;

    HidVibrationDeviceHandle handles[Targets][2] {};
    bool ready[Targets] {};
    MotorLevels sent;
    Target sentTo = None;

    void send(Target to, const MotorLevels& levels);
    bool permitSession = false;
    unsigned sends = 0, failures = 0;
    uint32_t lastRc = 0;
};

}
