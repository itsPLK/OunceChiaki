#include "controller.hpp"

#include "input_math.hpp"

#include <algorithm>
#include <chiaki/time.h>
#include <cstring>

namespace ounce
{

namespace
{

int16_t axis(float value)
{
    return static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
}

}

ChiakiControllerState mapController(const brls::ControllerState& pad, const SystemButtons& system)
{
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    struct Mapping
    {
        brls::ControllerButton from;
        uint32_t to;
    };
    static constexpr Mapping buttons[] = {
        { brls::BUTTON_B, CHIAKI_CONTROLLER_BUTTON_CROSS },
        { brls::BUTTON_A, CHIAKI_CONTROLLER_BUTTON_MOON },
        { brls::BUTTON_Y, CHIAKI_CONTROLLER_BUTTON_BOX },
        { brls::BUTTON_X, CHIAKI_CONTROLLER_BUTTON_PYRAMID },
        { brls::BUTTON_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_UP },
        { brls::BUTTON_DOWN, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
        { brls::BUTTON_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT },
        { brls::BUTTON_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
        { brls::BUTTON_LB, CHIAKI_CONTROLLER_BUTTON_L1 },
        { brls::BUTTON_RB, CHIAKI_CONTROLLER_BUTTON_R1 },
        { brls::BUTTON_LSB, CHIAKI_CONTROLLER_BUTTON_L3 },
        { brls::BUTTON_RSB, CHIAKI_CONTROLLER_BUTTON_R3 },
        { brls::BUTTON_START, CHIAKI_CONTROLLER_BUTTON_OPTIONS },
        { brls::BUTTON_BACK, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD },
    };
    for (const auto& mapping : buttons)
        if (pad.buttons[mapping.from])
            state.buttons |= mapping.to;
    if (system.chat)
        state.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
    if (system.capture)
        state.buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
    // ZL/ZR are digital on Switch controllers.
    state.l2_state = pad.buttons[brls::BUTTON_LT] ? 255 : 0;
    state.r2_state = pad.buttons[brls::BUTTON_RT] ? 255 : 0;
    // Borealis's axes are -1..1 with down positive, as the PlayStation's.
    state.left_x  = axis(pad.axes[brls::LEFT_X]);
    state.left_y  = axis(pad.axes[brls::LEFT_Y]);
    state.right_x = axis(pad.axes[brls::RIGHT_X]);
    state.right_y = axis(pad.axes[brls::RIGHT_Y]);
    return state;
}

ControllerInput::ControllerInput(bool motion, bool touchpad) : motion(motion), touchpad(touchpad)
{
    chiaki_controller_state_set_idle(&touchState);
    chiaki_orientation_tracker_init(&tracker);
    chiaki_accel_new_zero_set_inactive(&accelZero, false);
    chiaki_accel_new_zero_set_inactive(&realAccel, true);
    if (motion)
    {
        // Borealis reads player 1's six-axis sensor every loop and reports it
        // in SDL's units and axes: acceleration in g, rotation in rad/s.
        auto* event        = brls::Application::getPlatform()->getInputManager()->getControllerSensorStateChanged();
        sensorSubscription = event->subscribe([this](brls::SensorEvent e) { onSensor(e); });
        subscribed         = true;
        // The handheld's handle, which Borealis started: reading it says
        // whether the console is in handheld.
        hidGetSixAxisSensorHandles(&handheldSensor, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    }
    if (touchpad)
        hidInitializeTouchScreen();
}

ControllerInput::~ControllerInput()
{
    brls::Logger::info("Controller: {} motion samples, {} touches, {} taps", motionSamples, touchesStarted, taps);
    if (subscribed)
        brls::Application::getPlatform()->getInputManager()->getControllerSensorStateChanged()->unsubscribe(
            sensorSubscription);
}

// As chiaki-ng's controller manager: accelerometer samples set the measured
// gravity, gyro samples advance the orientation.
void ControllerInput::onSensor(const brls::SensorEvent& event)
{
    if (event.controllerIndex != 0)
        return;
    if (motionSamples++ < 2)
        brls::Logger::info("Controller: first {} sample {:.3f} {:.3f} {:.3f}",
            event.type == brls::SensorEventType::ACCEL ? "accelerometer (g)" : "gyro (rad/s)", event.data[0],
            event.data[1], event.data[2]);
    const uint32_t now = uint32_t(chiaki_time_now_monotonic_us());
    std::array<float, 3> v { event.data[0], event.data[1], event.data[2] };
    if (handheld)
        v = uprightToFlat(event.data);
    if (event.type == brls::SensorEventType::ACCEL)
    {
        chiaki_accel_new_zero_set_active(&realAccel, v[0], v[1], v[2], true);
        chiaki_orientation_tracker_update(&tracker, tracker.gyro_x, tracker.gyro_y, tracker.gyro_z, v[0], v[1], v[2],
            &accelZero, false, now);
    }
    else
        chiaki_orientation_tracker_update(&tracker, v[0], v[1], v[2], tracker.accel_x, tracker.accel_y,
            tracker.accel_z, &accelZero, true, now);
    motionSeen = true;
}

void ControllerInput::resetMotion()
{
    chiaki_accel_new_zero_set_active(&accelZero, realAccel.accel_x, realAccel.accel_y, realAccel.accel_z, false);
    chiaki_orientation_tracker_init(&tracker);
    chiaki_orientation_tracker_update(&tracker, 0, 0, 0, realAccel.accel_x, realAccel.accel_y, realAccel.accel_z,
        &accelZero, false, uint32_t(chiaki_time_now_monotonic_us()));
    brls::Logger::info("Controller: motion origin reset");
}

void ControllerInput::releaseTouches()
{
    for (unsigned i = 0; i < fingerCount; i++)
        chiaki_controller_state_stop_touch(&touchState, uint8_t(fingers[i].touchId));
    fingerCount  = 0;
    clickUntilUs = 0;
    waitForLift  = true;
}

void ControllerInput::readTouches(uint64_t now)
{
    HidTouchScreenState screen {};
    if (!hidGetTouchScreenStates(&screen, 1))
        screen.count = 0;
    if (waitForLift)
    {
        if (screen.count > 0)
            return;
        waitForLift = false;
    }
    // Lift fingers that are no longer on the screen; a short, still touch is a tap.
    for (unsigned i = 0; i < fingerCount;)
    {
        bool present = false;
        for (int t = 0; t < screen.count; t++)
            present |= screen.touches[t].finger_id == fingers[i].id;
        if (present)
        {
            i++;
            continue;
        }
        chiaki_controller_state_stop_touch(&touchState, uint8_t(fingers[i].touchId));
        if (isTap(fingers[i].downUs, now, fingers[i].x0, fingers[i].y0, fingers[i].x, fingers[i].y))
        {
            clickUntilUs = now + ClickPulseUs;
            taps++;
        }
        fingers[i] = fingers[--fingerCount];
    }
    // Move known fingers, start new ones while chiaki has room.
    for (int t = 0; t < screen.count; t++)
    {
        const HidTouchState& touch = screen.touches[t];
        const auto [px, py]        = touchpadPoint(touch.x, touch.y);
        Finger* finger             = nullptr;
        for (unsigned i = 0; i < fingerCount; i++)
            if (fingers[i].id == touch.finger_id)
                finger = &fingers[i];
        if (finger)
        {
            finger->x = touch.x, finger->y = touch.y;
            chiaki_controller_state_set_touch_pos(&touchState, uint8_t(finger->touchId), px, py);
        }
        else if (fingerCount < CHIAKI_CONTROLLER_TOUCHES_MAX)
        {
            const int8_t touchId = chiaki_controller_state_start_touch(&touchState, px, py);
            if (touchId >= 0)
            {
                fingers[fingerCount++] = { touch.finger_id, touchId, now, touch.x, touch.y, touch.x, touch.y };
                touchesStarted++;
            }
        }
    }
}

ChiakiControllerState ControllerInput::read(const brls::ControllerState& pad, const SystemButtons& system)
{
    ChiakiControllerState state = mapController(pad, system);
    const uint64_t now          = chiaki_time_now_monotonic_us();
    if (touchpad)
    {
        readTouches(now);
        std::memcpy(state.touches, touchState.touches, sizeof(state.touches));
        state.touch_id_next = touchState.touch_id_next;
        if (now < clickUntilUs)
            state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    }
    if (motion)
    {
        // Borealis reads the handheld's sensor whenever the console is in
        // handheld, so that is when its motion is upright.
        HidSixAxisSensorState sensor {};
        const bool nowHandheld = hidGetSixAxisSensorStates(handheldSensor, &sensor, 1) &&
                                 (sensor.attributes & HidSixAxisSensorAttribute_IsConnected);
        if (!handheldKnown || nowHandheld != handheld)
            brls::Logger::info("Controller: motion from {}",
                nowHandheld ? "the console in handheld (upright, turned to a flat DualSense)" : "a controller");
        handheld      = nowHandheld;
        handheldKnown = true;
    }
    if (motion && motionSeen)
        chiaki_orientation_tracker_apply_to_controller_state(&tracker, &state);
    return state;
}

}
