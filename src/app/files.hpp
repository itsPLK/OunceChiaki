#pragma once

#include <string>

namespace ounce
{

// Writes text to path through path.new, keeping the previous file as path.bak:
// an interrupted SD write never leaves a half file. One fwrite, since the
// console's SD card is slow with many small writes.
bool writeFileSafely(const std::string& path, const std::string& text);

}
