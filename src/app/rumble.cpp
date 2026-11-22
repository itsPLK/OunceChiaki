#include "rumble.hpp"

#include <algorithm>
#include <borealis.hpp>
#include <cmath>

namespace ounce
{

namespace
{

constexpr float LowHz  = 160.0f;
constexpr float HighHz = 320.0f;

HidVibrationValue value(float low, float high)
{
    HidVibrationValue v {};
    v.amp_low   = std::clamp(low, 0.0f, 1.0f);
    v.freq_low  = LowHz;
    v.amp_high  = std::clamp(high, 0.0f, 1.0f);
    v.freq_high = HighHz;
    return v;
}

// Strengths a few percent apart feel the same; don't send an IPC for them.
bool close(const MotorLevels& a, const MotorLevels& b)
{
    auto near = [](float x, float y) { return std::fabs(x - y) < 0.02f; };
    return near(a.leftLow, b.leftLow) && near(a.leftHigh, b.leftHigh) && near(a.rightLow, b.rightLow)
        && near(a.rightHigh, b.rightHigh);
}

}

Rumble::Rumble()
{
    ready[Handheld]      = R_SUCCEEDED(hidInitializeVibrationDevices(handles[Handheld], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
    ready[ProController] = R_SUCCEEDED(hidInitializeVibrationDevices(handles[ProController], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey));
    ready[JoyConPair]    = R_SUCCEEDED(hidInitializeVibrationDevices(handles[JoyConPair], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual));
    brls::Logger::info("Rumble: handheld {}, Pro Controller {}, Joy-Con pair {}", ready[Handheld] ? "ready" : "unavailable",
        ready[ProController] ? "ready" : "unavailable", ready[JoyConPair] ? "ready" : "unavailable");
    bool permitted     = false;
    const Result prc   = hidIsVibrationPermitted(&permitted);
    brls::Logger::info("Rumble: vibration permitted {} (rc=0x{:x})", permitted, prc);
    if (R_SUCCEEDED(prc) && !permitted)
    {
        // Horizon drops every vibration while it isn't permitted (the actual
        // value read back stays 0). A permit session lasts until it's ended or
        // the app exits, and leaves the system setting alone.
        const Result src = hidBeginPermitVibrationSession();
        permitSession    = R_SUCCEEDED(src);
        bool now         = false;
        hidIsVibrationPermitted(&now);
        brls::Logger::info("Rumble: permit vibration session rc=0x{:x}; permitted now {}", src, now);
    }
    static const char* names[] = { "handheld", "Pro Controller", "Joy-Con pair" };
    for (int t = 0; t < Targets; t++)
    {
        if (!ready[t])
            continue;
        for (int i = 0; i < 2; i++)
        {
            HidVibrationDeviceInfo info {};
            const Result irc = hidGetVibrationDeviceInfo(handles[t][i], &info);
            bool mounted     = false;
            const Result mrc = hidIsVibrationDeviceMounted(handles[t][i], &mounted);
            brls::Logger::info("Rumble: {} device {}: handle 0x{:x}, type {} position {} (rc=0x{:x}), mounted {} (rc=0x{:x})",
                names[t], i, handles[t][i].type_value, info.type, info.position, irc, mounted, mrc);
        }
    }
}

Rumble::~Rumble()
{
    stop();
    brls::Logger::info("Rumble: {} vibration updates sent, {} failed", sends, failures);
    if (permitSession)
        hidEndPermitVibrationSession();
}

// A controller connected as player 1 wins; otherwise the attached Joy-Cons.
Rumble::Target Rumble::target() const
{
    const u32 style = hidGetNpadStyleSet(HidNpadIdType_No1);
    Target to       = None;
    if (style & HidNpadStyleTag_NpadFullKey)
        to = ProController;
    else if (style & HidNpadStyleTag_NpadJoyDual)
        to = JoyConPair;
    else if (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld)
        to = Handheld;
    return to != None && ready[to] ? to : None;
}

void Rumble::send(Target to, const MotorLevels& levels)
{
    const HidVibrationValue values[2] = { value(levels.leftLow, levels.leftHigh), value(levels.rightLow, levels.rightHigh) };
    const Result rc = hidSendVibrationValues(handles[to], values, 2);
    sends++;
    if (R_FAILED(rc) && failures++ == 0)
        brls::Logger::warning("Rumble: sending failed rc=0x{:x}", rc);
    lastRc = rc;
}

void Rumble::set(const MotorLevels& levels)
{
    const Target to = target();
    if (to == sentTo && (levels == sent || (close(levels, sent) && levels != MotorLevels {})))
        return;
    if (to != sentTo && sentTo != None)
        send(sentTo, {}); // the controller changed: silence the previous one
    if (to != sentTo)
        brls::Logger::info("Rumble: using the {}", to == Handheld ? "attached Joy-Cons" : to == ProController ? "Pro Controller"
            : to == JoyConPair ? "Joy-Con pair" : "no controller");
    sentTo = to;
    sent   = levels;
    if (to != None)
        send(to, levels);
}

}
