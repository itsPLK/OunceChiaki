#include "files.hpp"

#include <borealis.hpp>
#include <cstdio>

namespace ounce
{

bool writeFileSafely(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".new", backup = path + ".bak";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
    {
        brls::Logger::error("Files: can't write {}", temporary);
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed  = std::fclose(file) == 0;
    if (!written || !closed)
    {
        brls::Logger::error("Files: writing {} failed", temporary);
        std::remove(temporary.c_str());
        return false;
    }
    std::remove(backup.c_str());
    std::rename(path.c_str(), backup.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        brls::Logger::error("Files: renaming {} failed", temporary);
        std::rename(backup.c_str(), path.c_str());
        return false;
    }
    return true;
}

}
