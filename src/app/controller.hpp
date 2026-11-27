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
    void readMotion(uint64_t now);
    void readTouches(uint64_t now);
    void probeMotion(uint64_t now);

    bool motion, touchpad;
    ChiakiOrientationTracker tracker {};
    ChiakiAccelNewZero accelZero {}, realAccel {};
    bool motionSeen = false;
    // Six-axis sources, as Borealis picks them: handheld, Pro Controller,
    // left and right Joy-Con of a pair. The handheld is held upright (see
    // uprightToFlat).
    struct Source
    {
        HidSixAxisSensorHandle handle {};
        uint64_t sampling = 0; // the last sample taken
        bool probed = false; // the probe's last look
        uint64_t probedSampling = 0;
        HidVector angle {};
    };
    Source sources[4] {};
    int source        = -1;
    uint64_t lastRead = 0, lastSampleUs = 0;

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
    // source turned through in the system's axes, and what reached chiaki.
    uint64_t probeStartUs = 0, probeSamples = 0, probeReads = 0, probeGapUs = 0;
    float probeTurn[3] {};
};

}
