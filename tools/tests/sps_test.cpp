// Host test for src/video/h264_sps.cpp: run tools/tests/run.sh
#include "h264_sps.hpp"

#include <cstdio>
#include <string>

using namespace ounce;

static std::vector<uint8_t> unhex(const std::string& hex)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
    // PS5 1080p60 H.264 SPS, from the console (2026-11-22).
    const auto ps5 = unhex("674d4032918a01e0089f97016a020202800003e90001d4c044d0f1f150");
    const auto info = parseSps(ps5);
    CHECK(info);
    if (!info)
        return 1;
    std::printf("original:  %s\n", describe(*info).c_str());
    CHECK(info->profile == 77 && info->level == 50 && info->refFrames == 9 && info->maxDecFrameBuffering == 9);

    const auto same = limitReferences(ps5, 16);
    CHECK(same && *same == ps5); // nothing to limit: bit-identical

    const auto limited = limitReferences(ps5, 4);
    CHECK(limited);
    if (limited)
    {
        const auto again = parseSps(*limited);
        CHECK(again);
        if (again)
        {
            std::printf("limited:   %s\n", describe(*again).c_str());
            CHECK(again->refFrames == 4 && again->maxDecFrameBuffering == 4 && again->maxReorderFrames == 0);
            CHECK(again->profile == info->profile && again->level == info->level);
            CHECK(again->widthMbs == info->widthMbs && again->heightMapUnits == info->heightMapUnits);
            CHECK(again->pocType == info->pocType && again->frameMbsOnly == info->frameMbsOnly);
        }
        std::string hex;
        for (auto b : *limited)
        {
            char t[3];
            std::snprintf(t, sizeof t, "%02x", b);
            hex += t;
        }
        std::printf("rewritten: %s\n", hex.c_str());
    }
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures != 0;
}
