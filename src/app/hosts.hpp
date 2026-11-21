#pragma once

#include <array>
#include <chiaki/common.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ounce
{

// A console the user added or paired, kept in hosts.txt.
struct Host
{
    std::string name;
    std::string address;
    ChiakiTarget target = CHIAKI_TARGET_PS5_1;
    std::optional<std::array<uint8_t, 6>> mac; // from pairing or discovery

    // Pairing keys (from chiaki's registration); empty until paired.
    bool registered = false;
    std::array<char, 16> registKey {};   // ChiakiConnectInfo.regist_key
    std::array<uint8_t, 16> rpKey {};     // ChiakiConnectInfo.morning

    bool ps5() const { return chiaki_target_is_ps5(target); }
    // Lower-case hex MAC, as discovery's host_id compares; empty if unknown.
    std::string macHex() const;
    // The wake credential: regist_key as a hex number.
    std::optional<uint64_t> wakeCredential() const;
};

// hosts.txt in the data folder. save() writes a temporary file and renames it
// over the old one, keeping the previous version as hosts.txt.bak, so an
// interrupted SD write never loses the pairings.
class HostStore
{
  public:
    void load();
    bool save() const;

    std::vector<Host>& hosts() { return list; }
    Host* find(const std::string& address, const std::string& macHex);
    void put(const Host& host); // replaces a host with the same MAC or address
    void remove(const std::string& address);

  private:
    std::vector<Host> list;
};

std::string toHex(const uint8_t* data, size_t size);
bool fromHex(const std::string& text, uint8_t* out, size_t size);

}
