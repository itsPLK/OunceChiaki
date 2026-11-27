#include "controller.hpp"

#include "input_math.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
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
        // The handles Borealis started (it keeps them running); reading them
        // needs no second start.
        hidGetSixAxisSensorHandles(&sources[0].handle, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidGetSixAxisSensorHandles(&sources[1].handle, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
        HidSixAxisSensorHandle dual[2] {};
        hidGetSixAxisSensorHandles(dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        sources[2].handle = dual[0];
        sources[3].handle = dual[1];
    }
    if (touchpad)
        hidInitializeTouchScreen();
}

ControllerInput::~ControllerInput()
{
    brls::Logger::info("Controller: {} motion samples, {} touches, {} taps", motionSamples, touchesStarted, taps);
}

// Every sample the sensor took since the last read (it keeps the last 17,
// 85 ms at 200 Hz), oldest first, each as one update of chiaki's tracker
// with both readings, as a DualSense report has. Borealis's own sensor events
// carry only the latest sample once per loop, so the tracker got some samples
// twice and missed others, timed by the loop: motion came out uneven.
void ControllerInput::readMotion(uint64_t now)
{
    // The source Borealis would pick: the console in handheld first, then a
    // Pro Controller, then a Joy-Con pair's left one (else its right one).
    int pick = -1;
    HidSixAxisSensorState states[17];
    int count = 0;
    for (int i = 0; i < 4 && pick < 0; i++)
    {
        count = int(hidGetSixAxisSensorStates(sources[i].handle, states, 17));
        if (count > 0 && (states[0].attributes & HidSixAxisSensorAttribute_IsConnected))
            pick = i;
    }
    if (pick < 0)
        return;
    if (pick != source)
    {
        static constexpr const char* names[] = { "the console in handheld (upright, turned to a flat DualSense)",
            "a Pro Controller", "the left Joy-Con", "the right Joy-Con" };
        brls::Logger::info("Controller: motion from {}", names[pick]);
        source   = pick;
        lastRead = 0; // start from the latest sample
    }
    // Samples are newest first; take the new ones.
    int fresh = 0;
    while (fresh < count && states[fresh].sampling_number > sources[pick].sampling)
        fresh++;
    if (fresh == 0)
        return;
    if (lastRead == 0)
        fresh = 1;
    sources[pick].sampling = states[0].sampling_number;
    // Spread them evenly over the time since the last read: exact over time,
    // whatever the sensor's rate, and no samples bunched at the read.
    const uint64_t span = lastRead ? std::min<uint64_t>(now - lastRead, 100000) : 0;
    const bool upright  = pick == 0;
    for (int k = fresh - 1; k >= 0; k--)
    {
        const HidSixAxisSensorState& sample = states[k];
        // System axes (x right, y the top, z the screen; acceleration in g
        // as gravity, rotation in turns/s) to SDL's, as Borealis converts.
        const float accelSdl[3] = { -sample.acceleration.x, -sample.acceleration.z, sample.acceleration.y };
        const float tau         = float(2.0 * M_PI);
        const float gyroSdl[3]  = { sample.angular_velocity.x * tau, sample.angular_velocity.z * tau,
             -sample.angular_velocity.y * tau };
        std::array<float, 3> accel { accelSdl[0], accelSdl[1], accelSdl[2] },
            gyro { gyroSdl[0], gyroSdl[1], gyroSdl[2] };
        if (upright)
            accel = uprightToFlat(accelSdl), gyro = uprightToFlat(gyroSdl);
        const uint64_t at = now - span * uint64_t(k) / uint64_t(fresh);
        if (motionSamples++ == 0)
            brls::Logger::info("Controller: first motion sample: accelerometer (g) {:.3f} {:.3f} {:.3f}, gyro "
                               "(rad/s) {:.3f} {:.3f} {:.3f}, delta_time {}",
                accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2], sample.delta_time);
        chiaki_accel_new_zero_set_active(&realAccel, accel[0], accel[1], accel[2], true);
        chiaki_orientation_tracker_update(&tracker, gyro[0], gyro[1], gyro[2], accel[0], accel[1], accel[2],
            &accelZero, false, uint32_t(at));
        if (debugLog() && lastSampleUs)
            for (int i = 0; i < 3; i++)
                probeTurn[i] += gyro[i] * float(at - lastSampleUs) * 1e-6f * 57.2958f;
        lastSampleUs = at;
    }
    if (debugLog())
    {
        probeSamples += uint64_t(fresh);
        probeReads++;
        if (lastRead)
            probeGapUs = std::max<uint64_t>(probeGapUs, now - lastRead);
    }
    lastRead   = now;
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
        readMotion(now);
    if (motion && motionSeen)
        chiaki_orientation_tracker_apply_to_controller_state(&tracker, &state);
    if (motion && debugLog())
        probeMotion(now);
    return state;
}

void ControllerInput::probeMotion(uint64_t now)
{
    if (probeStartUs && now - probeStartUs < 1000000)
        return;
    static constexpr const char* names[] = { "handheld", "pro", "joy-con L", "joy-con R" };
    std::string line;
    for (int i = 0; i < 4; i++)
    {
        Source& source = sources[i];
        HidSixAxisSensorState state {};
        if (!hidGetSixAxisSensorStates(source.handle, &state, 1) ||
            !(state.attributes & HidSixAxisSensorAttribute_IsConnected))
        {
            source.probed = false;
            continue;
        }
        if (source.probed)
        {
            // angle is in turns, summed by the system from every sample.
            line += fmt::format(" | {} {} samples, turned x {:+.0f} y {:+.0f} z {:+.0f} deg, accel {:+.2f} {:+.2f} {:+.2f}",
                names[i], state.sampling_number - source.probedSampling, (state.angle.x - source.angle.x) * 360.0f,
                (state.angle.y - source.angle.y) * 360.0f, (state.angle.z - source.angle.z) * 360.0f,
                state.acceleration.x, state.acceleration.y, state.acceleration.z);
        }
        source.probed         = true;
        source.probedSampling = state.sampling_number;
        source.angle          = state.angle;
    }
    if (probeStartUs)
        brls::Logger::info("Motion probe: to chiaki {} samples in {} reads (longest gap {:.1f} ms), turned x {:+.0f} y "
                           "{:+.0f} z {:+.0f} deg{}",
            probeSamples, probeReads, probeGapUs / 1000.0, probeTurn[0], probeTurn[1], probeTurn[2], line);
    probeStartUs = now;
    probeSamples = probeReads = probeGapUs = 0;
    probeTurn[0] = probeTurn[1] = probeTurn[2] = 0;
}

}
