#pragma once

#include <chiaki/log.h>

namespace ounce
{

// The app's data folder on the SD card.
inline constexpr const char* DataDir = "sdmc:/switch/OunceChiaki";

// Sends Borealis's log to logs/ounce.log (the previous run is kept as
// ounce.1.log) and returns a ChiakiLog that writes into the same log.
void openLog();
ChiakiLog* chiakiLog();

}
