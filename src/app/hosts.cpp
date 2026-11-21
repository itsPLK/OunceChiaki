#include "hosts.hpp"

#include "log.hpp"

#include <borealis.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ounce
{

namespace
{

std::string path() { return std::string(DataDir) + "/hosts.txt"; }

std::string lower(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

}

std::string toHex(const uint8_t* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; i++)
    {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

bool fromHex(const std::string& text, uint8_t* out, size_t size)
{
    if (text.size() != size * 2)
        return false;
    for (size_t i = 0; i < size; i++)
    {
        char pair[3] = { text[i * 2], text[i * 2 + 1], 0 };
        char* end    = nullptr;
        long value   = std::strtol(pair, &end, 16);
        if (*end)
            return false;
        out[i] = static_cast<uint8_t>(value);
    }
    return true;
}

std::string Host::macHex() const { return mac ? toHex(mac->data(), mac->size()) : std::string(); }

std::optional<uint64_t> Host::wakeCredential() const
{
    if (!registered)
        return std::nullopt;
    std::string text;
    for (char c : registKey)
    {
        if (!c)
            break;
        text += c;
    }
    char* end = nullptr;
    const uint64_t value = std::strtoull(text.c_str(), &end, 16);
    if (text.empty() || *end)
        return std::nullopt;
    return value;
}

void HostStore::load()
{
    list.clear();
    std::ifstream file(path());
    if (!file)
    {
        brls::Logger::info("Hosts: no {} yet", path());
        return;
    }
    std::optional<Host> current;
    auto finish = [&] {
        if (current && !current->address.empty())
            list.push_back(*current);
        current.reset();
    };
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "[host]")
        {
            finish();
            current.emplace();
            continue;
        }
        const auto eq = line.find('=');
        if (!current || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "name")
            current->name = value;
        else if (key == "address")
            current->address = value;
        else if (key == "target")
            current->target = static_cast<ChiakiTarget>(std::atoi(value.c_str()));
        else if (key == "mac")
        {
            std::array<uint8_t, 6> mac {};
            if (fromHex(value, mac.data(), mac.size()))
                current->mac = mac;
        }
        else if (key == "regist_key")
            current->registered = fromHex(value, reinterpret_cast<uint8_t*>(current->registKey.data()), 16);
        else if (key == "rp_key")
            current->registered = current->registered && fromHex(value, current->rpKey.data(), 16);
    }
    finish();
    brls::Logger::info("Hosts: loaded {} console(s)", list.size());
}

bool HostStore::save() const
{
    std::ostringstream out;
    out << "# OunceChiaki consoles. Pairing keys are secret: don't share this file.\n";
    for (const auto& host : list)
    {
        out << "[host]\nname=" << host.name << "\naddress=" << host.address << "\ntarget=" << static_cast<int>(host.target)
            << "\n";
        if (host.mac)
            out << "mac=" << host.macHex() << "\n";
        if (host.registered)
            out << "regist_key=" << toHex(reinterpret_cast<const uint8_t*>(host.registKey.data()), 16)
                << "\nrp_key=" << toHex(host.rpKey.data(), 16) << "\n";
    }
    const std::string text = out.str(), target = path(), temporary = target + ".new", backup = target + ".bak";
    // One fwrite: the console's SD card is slow with many small writes.
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
    {
        brls::Logger::error("Hosts: can't write {}", temporary);
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed  = std::fclose(file) == 0;
    if (!written || !closed)
    {
        brls::Logger::error("Hosts: writing {} failed", temporary);
        std::remove(temporary.c_str());
        return false;
    }
    std::remove(backup.c_str());
    std::rename(target.c_str(), backup.c_str());
    if (std::rename(temporary.c_str(), target.c_str()) != 0)
    {
        brls::Logger::error("Hosts: renaming {} failed", temporary);
        std::rename(backup.c_str(), target.c_str());
        return false;
    }
    brls::Logger::info("Hosts: saved {} console(s)", list.size());
    return true;
}

Host* HostStore::find(const std::string& address, const std::string& macHex)
{
    const std::string mac = lower(macHex);
    for (auto& host : list)
        if (!mac.empty() && host.macHex() == mac)
            return &host;
    for (auto& host : list)
        if (host.address == address)
            return &host;
    return nullptr;
}

void HostStore::put(const Host& host)
{
    if (Host* existing = find(host.address, host.macHex()))
        *existing = host;
    else
        list.push_back(host);
}

void HostStore::remove(const std::string& address)
{
    std::erase_if(list, [&](const Host& host) { return host.address == address; });
}

}
