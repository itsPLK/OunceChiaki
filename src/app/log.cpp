// fopencookie is a GNU extension in newlib.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "log.hpp"

#include <borealis.hpp>
#include <chiaki/time.h>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace ounce
{

namespace
{

// Borealis flushes its log after every line; to the SD card that costs a few
// ms per line, and on chiaki's receive thread a burst of warnings then made the
// stream fall behind for good (2026-11-22). Lines go to memory instead, and a
// writer thread saves them every FlushMs with one write. A crash loses at most
// the last FlushMs of log.
constexpr int FlushMs           = 250;
constexpr size_t MaxPending     = 1024 * 1024;
constexpr unsigned ChiakiPerSec = 50; // chiaki lines kept per second

struct Sink
{
    std::mutex mutex;
    std::condition_variable wake;
    std::string pending;
    size_t droppedBytes = 0;
    bool stopping       = false;
    FILE* file          = nullptr; // the SD file, written only by the writer
    FILE* cookie        = nullptr; // what Borealis writes to
    std::thread writer;
};

Sink sink;
ChiakiLog chiaki_log;

ssize_t sinkWrite(void*, const char* data, size_t size)
{
    std::lock_guard lock(sink.mutex);
    if (sink.pending.size() + size <= MaxPending)
        sink.pending.append(data, size);
    else
        sink.droppedBytes += size;
    return ssize_t(size);
}

void writerLoop()
{
    std::string batch;
    for (;;)
    {
        size_t dropped = 0;
        bool stopping  = false;
        {
            std::unique_lock lock(sink.mutex);
            sink.wake.wait_for(lock, std::chrono::milliseconds(FlushMs), [] { return sink.stopping; });
            batch.swap(sink.pending);
            dropped           = sink.droppedBytes;
            sink.droppedBytes = 0;
            stopping          = sink.stopping;
        }
        if (dropped)
            batch += "[log] " + std::to_string(dropped) + " bytes of log dropped (buffer full)\n";
        if (!batch.empty())
        {
            std::fwrite(batch.data(), 1, batch.size(), sink.file);
            std::fflush(sink.file);
            batch.clear();
        }
        if (stopping)
            return;
    }
}

// chiaki logs one warning per missing packet; during loss that is hundreds per
// second. Keep the first ChiakiPerSec lines of each second and count the rest.
void chiakiLogLine(ChiakiLogLevel level, const char* text, void*)
{
    static std::mutex mutex;
    static uint64_t windowMs = 0;
    static unsigned lines = 0, skipped = 0;
    unsigned report = 0;
    {
        std::lock_guard lock(mutex);
        const uint64_t now = chiaki_time_now_monotonic_ms();
        if (now - windowMs >= 1000)
        {
            report   = skipped;
            windowMs = now;
            lines = skipped = 0;
        }
        if (++lines > ChiakiPerSec)
        {
            skipped++;
            return;
        }
    }
    if (report)
        brls::Logger::warning("[chiaki] {} lines skipped in the last second", report);
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
    sink.file = std::fopen(current.c_str(), "w");
    if (sink.file)
    {
        cookie_io_functions_t functions {};
        functions.write = sinkWrite;
        sink.cookie     = fopencookie(nullptr, "w", functions);
        if (sink.cookie)
        {
            sink.writer = std::thread(writerLoop);
            brls::Logger::setLogOutput(sink.cookie);
        }
        else
            brls::Logger::setLogOutput(sink.file); // unbuffered fallback
    }
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
    chiaki_log_init(&chiaki_log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, chiakiLogLine, nullptr);
}

void closeLog()
{
    if (!sink.writer.joinable())
        return;
    std::fflush(sink.cookie);
    {
        std::lock_guard lock(sink.mutex);
        sink.stopping = true;
    }
    sink.wake.notify_all();
    sink.writer.join();
    brls::Logger::setLogOutput(stdout);
    std::fclose(sink.cookie);
    std::fclose(sink.file);
    sink.cookie = sink.file = nullptr;
}

ChiakiLog* chiakiLog() { return &chiaki_log; }

}
