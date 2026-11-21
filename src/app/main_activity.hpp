#pragma once

#include "discovery.hpp"

#include <borealis.hpp>

namespace ounce
{

// First milestone screen: shows the build, the core's init result, LAN
// discovery and the system buttons the app can take over.
class MainActivity : public brls::Activity
{
  public:
    explicit MainActivity(int coreInit);
    ~MainActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    void refresh();

    int coreInit;
    Discovery discovery;
    brls::Time discoveryEnds = 0;
    brls::RepeatingTimer refreshTimer;
    brls::Label* hostsLabel   = nullptr;
    brls::Label* buttonsLabel = nullptr;
    brls::Button* findButton  = nullptr;
    std::string lastAddress = "192.168.1.";
    int chatPresses = 0, capturePresses = 0;
    bool chatHeld = false, captureHeld = false;
};

}
