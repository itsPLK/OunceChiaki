#include "log.hpp"

#include <borealis.hpp>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace ounce
{

namespace
{

ChiakiLog chiaki_log;

void chiakiLogLine(ChiakiLogLevel level, const char* text, void*)
{
    switch (level)
    {
        case CHIAKI_LOG_ERROR:
            brls::Logger::error("[chiaki] {}", text);
            break;
        case CHIAKI_LOG_WARNING:
            brls::Logger::warning("[chiaki] {}", text);
            break;
        case CHIAKI_LOG_INFO:
            brls::Logger::info("[chiaki] {}", text);
            break;
        default:
            brls::Logger::debug("[chiaki] {}", text);
            break;
    }
}

}

void openLog()
{
    const std::string logs = std::string(DataDir) + "/logs";
    mkdir(DataDir, 0777);
    mkdir(logs.c_str(), 0777);
    const std::string current  = logs + "/ounce.log";
    const std::string previous = logs + "/ounce.1.log";
    std::remove(previous.c_str());
    std::rename(current.c_str(), previous.c_str());
    if (FILE* file = std::fopen(current.c_str(), "w"))
    {
        // Line-buffered: the console's SD card is slow, but a crash must not
        // lose the last lines.
        std::setvbuf(file, nullptr, _IOLBF, 0);
        brls::Logger::setLogOutput(file);
    }
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
    chiaki_log_init(&chiaki_log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, chiakiLogLine, nullptr);
}

ChiakiLog* chiakiLog() { return &chiaki_log; }

}
