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

}
