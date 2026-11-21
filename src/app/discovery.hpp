#pragma once

#include <chiaki/discovery.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ounce
{

struct DiscoveredHost
{
    std::string address;
    std::string name;
    bool ps5     = false;
    bool standby = false;
};

// Broadcasts PS4 and PS5 search packets on the LAN and collects the answers.
class Discovery
{
  public:
    ~Discovery();

    // Starts a search: broadcast on the LAN, or ask the one address given.
    // Returns false (and logs why) if it couldn't start.
    bool start(const std::string& address = "");
    void stop();
    bool running() const { return active; }

    // Hosts that answered so far, deduplicated by address.
    std::vector<DiscoveredHost> hosts();

  private:
    static void onHost(ChiakiDiscoveryHost* host, void* user);
    void sendSearch(uint32_t address);

    ChiakiDiscovery discovery {};
    ChiakiDiscoveryThread thread {};
    bool active = false;
    std::mutex mutex;
    std::vector<DiscoveredHost> found;
};

}
