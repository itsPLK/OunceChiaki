#pragma once

#include <borealis.hpp>
#include <chiaki/controller.h>
#include <chiaki/orientation.h>
#include <switch.h>

namespace ounce
{

// The Switch controls as a PlayStation controller, by position: B/A/Y/X are
// Cross/Circle/Square/Triangle, ZL/ZR are L2/R2, Plus is Options and Minus the
// touchpad click. Of the taken-over system buttons, Chat is the PS button,
// Capture is Create/Share and Home opens the quick menu.
struct SystemButtons
{
    bool chat = false, capture = false;
};

ChiakiControllerState mapController(const brls::ControllerState& pad, const SystemButtons& system);

// Everything the console gets from player 1 in a stream: buttons and sticks,
// plus (when enabled) motion as a DualSense/DualShock gyro and the
// touchscreen as the touchpad, where a tap is a touchpad click. UI thread only.
class ControllerInput
{
  public:
    ControllerInput(bool motion, bool touchpad);
    ~ControllerInput();

    ChiakiControllerState read(const brls::ControllerState& pad, const SystemButtons& system);
    // The game asked for the motion origin to be reset (CHIAKI_EVENT_MOTION_RESET).
    void resetMotion();
    // Lifts every finger, e.g. when the quick menu opens; the touchscreen is
    // ignored until no finger is on it, so a tap on the menu isn't a click.
    void releaseTouches();

  private:
    void onSensor(const brls::SensorEvent& event);
    void readTouches(uint64_t now);
    void probeMotion(uint64_t now);

    bool motion, touchpad;
    brls::Event<brls::SensorEvent>::Subscription sensorSubscription;
    bool subscribed = false;
    ChiakiOrientationTracker tracker {};
    ChiakiAccelNewZero accelZero {}, realAccel {};
    bool motionSeen = false;
    // Motion comes from the console in handheld (held upright): see uprightToFlat.
    bool handheld = false, handheldKnown = false;

    // Touches: chiaki's touches live in touchState between frames.
    ChiakiControllerState touchState {};
    struct Finger
    {
        uint32_t id;
        int8_t touchId; // chiaki's id for this touch
        uint64_t downUs;
        uint32_t x0, y0, x, y;
    };
    Finger fingers[CHIAKI_CONTROLLER_TOUCHES_MAX] {};
    unsigned fingerCount = 0;
    uint64_t clickUntilUs = 0;
    bool waitForLift = false;
    uint64_t motionSamples = 0, touchesStarted = 0, taps = 0;

    // Motion probe (Debug log only): once a second, what each six-axis
    // source (handheld, Pro Controller, left and right Joy-Con) turned
    // through in its own axes, and what reached chiaki from Borealis.
    struct ProbeSource
    {
        HidSixAxisSensorHandle handle {};
        bool started = false;
        uint64_t sampling = 0;
        HidVector angle {};
    };
    ProbeSource probeSources[4] {};
    uint64_t probeStartUs = 0, probeLastGyroUs = 0;
    float probeTurn[3] {}, probeLastGyro[3] {};
    unsigned probeEvents = 0, probeDistinct = 0;
};

}
