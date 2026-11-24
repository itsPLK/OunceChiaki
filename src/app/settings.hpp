#pragma once

#include "stream.hpp"

#include <string>
#include <vector>

namespace ounce
{

// Stream settings, kept in settings.txt. Bitrate 0 is Auto: CivChiaki's
// per-resolution and frame rate values (autoBitrate).
struct Settings
{
    int height  = 1080; // one of Heights
    int fps     = 60;   // 30 or 60
    FrameGeneration frameGeneration = FrameGeneration::Off;
    int bitrate = 0;    // kbps, 0 = Auto
    bool upscaling = false; // FSR 1 EASU on luma when the stream is smaller than the screen
    int sharpening = 0;     // FSR 1 RCAS after EASU: 0 off, 1 low, 2 medium, 3 high
    bool lowLatencyAudio = false; // force the 40 ms audio limit; otherwise it adapts to the network (audio_policy.hpp)
    // Controller
    bool motion   = true; // gyro and accelerometer as the controller's motion
    bool touchpad = true; // the touchscreen as the touchpad
    int rumble    = 1;    // 0 Off, 1 Weak, 2 Normal, 3 Strong (rumbleScale)
    bool debugLog = false; // periodic stats in the log (log.hpp)

    static const std::vector<int> Heights;
    static const std::vector<int> Bitrates; // fixed choices, kbps

    void load();
    bool save() const;

    int effectiveBitrate() const; // Auto resolved
    StreamSettings stream() const;
};

int autoBitrate(int height, int fps);
std::string bitrateLabel(int kbps); // "20 Mbps", "2.5 Mbps"

Settings& settings();

}
