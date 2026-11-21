#pragma once

#include <chiaki/discovery.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ounce
{

struct DiscoveredHost
{
    std::string address;
    std::string name;
    std::string macHex; // discovery's host-id, lower case
    ChiakiTarget target = CHIAKI_TARGET_PS5_1;
    bool standby        = false;
    uint64_t seenMs     = 0; // last answer, monotonic
};

// Listens for PS4/PS5 answers while open. search() asks the LAN (the limited
// and the subnet broadcast: the Switch doesn't send the limited one onto the
// LAN) and any addresses given.
class Discovery
{
  public:
    ~Discovery();

    bool open();
    void close();
    bool isOpen() const { return active; }

    void search(const std::vector<std::string>& addresses);

    // Hosts that answered, deduplicated by address.
    std::vector<DiscoveredHost> hosts();

  private:
    static void onHost(ChiakiDiscoveryHost* host, void* user);
    bool sendSearch(uint32_t address);

    ChiakiDiscovery discovery {};
    ChiakiDiscoveryThread thread {};
    bool active      = false;
    uint32_t subnet  = 0; // directed broadcast, network byte order; 0 if unknown
    std::mutex mutex;
    std::vector<DiscoveredHost> found;
};

}
