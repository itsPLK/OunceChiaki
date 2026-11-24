#include "settings.hpp"

#include "files.hpp"
#include "log.hpp"

#include <algorithm>
#include <borealis.hpp>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>

namespace ounce
{

namespace
{

std::string path() { return std::string(DataDir) + "/settings.txt"; }

bool contains(const std::vector<int>& list, int value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

}

const std::vector<int> Settings::Heights  = { 360, 540, 720, 1080 };
const std::vector<int> Settings::Bitrates = { 2000, 3000, 5000, 8000, 10000, 12000, 15000, 20000, 25000, 30000, 40000, 50000 };

Settings& settings()
{
    static Settings instance;
    return instance;
}

// CivChiaki's Auto values: what the stream needs at each size and rate. Above
// what the Switch receives in time, the congestion signal lowers it anyway.
int autoBitrate(int height, int fps)
{
    const bool high = fps >= 60;
    switch (height)
    {
        case 360: return high ? 3000 : 2000;
        case 540: return high ? 8000 : 6000;
        case 720: return high ? 15000 : 10000;
        default: return high ? 20000 : 15000;
    }
}

std::string bitrateLabel(int kbps)
{
    return kbps % 1000 ? fmt::format("{:.1f} Mbps", kbps / 1000.0) : fmt::format("{} Mbps", kbps / 1000);
}

void Settings::load()
{
    *this = Settings {};
    std::ifstream file(path());
    if (!file)
        return;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const int value       = std::atoi(line.c_str() + eq + 1);
        if (key == "height" && contains(Heights, value))
            height = value;
        else if (key == "fps" && (value == 30 || value == 60))
            fps = value;
        else if (key == "frame_generation")
            frameGeneration = value == 1 ? FrameGeneration::Extrapolate
                : value == 2 ? FrameGeneration::Interpolate : FrameGeneration::Off;
        else if (key == "bitrate" && (value == 0 || contains(Bitrates, value)))
            bitrate = value;
        else if (key == "upscaling")
            upscaling = value != 0;
        else if (key == "sharpening" && value >= 0 && value <= 3)
            sharpening = value;
        else if (key == "low_latency_audio")
            lowLatencyAudio = value != 0;
        else if (key == "motion")
            motion = value != 0;
        else if (key == "touchpad")
            touchpad = value != 0;
        else if (key == "rumble" && value >= 0 && value <= 3)
            rumble = value;
    }
    brls::Logger::info("Settings: {}p{} at {}{}, upscaling {} sharpening {}, {} audio; motion {}, touchpad {}, rumble {}", height, fps,
        bitrateLabel(effectiveBitrate()), bitrate ? "" : " (Auto)", upscaling ? "FSR" : "off", sharpening,
        lowLatencyAudio ? "low-latency" : "adaptive",
        motion ? "on" : "off", touchpad ? "on" : "off", rumble);
}

bool Settings::save() const
{
    const std::string text = fmt::format("# OunceChiaki settings\nheight={}\nfps={}\n# 0 is Auto\nbitrate={}\n"
                                         "frame_generation={}\n# 1 FSR 1 upscaling\nupscaling={}\n# FSR sharpening: 0 off, 1 low, 2 medium, 3 high\nsharpening={}\nlow_latency_audio={}\nmotion={}\ntouchpad={}\n# 0 off, 1 weak, 2 normal, 3 strong\nrumble={}\n",
        height, fps, bitrate, int(frameGeneration), int(upscaling), sharpening, int(lowLatencyAudio), int(motion), int(touchpad), rumble);
    return writeFileSafely(path(), text);
}

int Settings::effectiveBitrate() const { return bitrate ? bitrate : autoBitrate(height, fps); }

StreamSettings Settings::stream() const
{
    StreamSettings out;
    out.height  = height;
    out.fps     = fps;
    out.frameGeneration = frameGeneration; // 30 to 60 or 60 to 120 fps
    out.upscaling = upscaling;
    out.sharpening = sharpening;
    out.bitrate = effectiveBitrate();
    out.adaptiveAudio = !lowLatencyAudio;
    out.motion   = motion;
    out.touchpad = touchpad;
    out.rumble   = rumble;
    return out;
}

}
