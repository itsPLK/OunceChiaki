#pragma once

#include "discovery.hpp"
#include "hosts.hpp"
#include "pairing.hpp"

#include <borealis.hpp>
#include <map>

namespace ounce
{

// The console list: saved consoles plus the ones discovery finds on the LAN.
// A pairs (or, once streaming lands, connects), X wakes, Y removes.
class HostsActivity : public brls::Activity
{
  public:
    HostsActivity();
    ~HostsActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    struct Entry
    {
        Host host;          // saved data, or what discovery reported
        bool saved   = false;
        bool online  = false;
        bool standby = false;
    };

    std::vector<Entry> entries();
    void refresh();
    void rebuild(const std::vector<Entry>& list);
    void updateCell(brls::DetailCell* cell, const Entry& entry);
    void learn(const std::vector<DiscoveredHost>& found);

    void activate(const std::string& address);
    void pair(Host host);
    void askPin(Host host, std::array<uint8_t, 8> account);
    void wakeHost(const std::string& address);
    void removeHost(const std::string& address);
    void addByAddress();

    HostStore store;
    Discovery discovery;
    Pairing pairing;
    brls::RepeatingTimer timer;
    uint64_t nextSearchMs = 0;
    std::string accountId;          // remembered for this run only
    std::string lastAddress = "192.168.1.";

    brls::Box* list           = nullptr;
    brls::Label* statusLabel  = nullptr;
    brls::DetailCell* settingsCell = nullptr;
    std::vector<std::string> shown; // addresses of the cells, in order
    std::map<std::string, brls::DetailCell*> cells;
};

}
