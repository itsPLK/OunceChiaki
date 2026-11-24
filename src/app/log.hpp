#pragma once

#include <chiaki/log.h>

namespace ounce
{

// The app's data folder on the SD card.
inline constexpr const char* DataDir = "sdmc:/switch/OunceChiaki";

// Sends Borealis's log to logs/ounce.log (the previous run is kept as
// ounce.1.log), buffered in memory and saved every 250 ms; chiakiLog() writes
// into the same log. closeLog() saves what is left.
void openLog();
void closeLog();
ChiakiLog* chiakiLog();

// Settings → Debug log (off by default): the periodic stats every 1 to 5 s
// (stream, video timing, GPU, decoder, Borealis's presents) and chiaki's
// heartbeats and hex dumps. Off, the log keeps events and problems only, so
// a stream doesn't write the SD card every second.
void setDebugLog(bool on);
bool debugLog();

}
