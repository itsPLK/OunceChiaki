// The stream's quick menu (Home): a panel down the right of the screen over
// the stream, in the style of the Switch's own overlay. The clock, battery,
// Wi-Fi and the screen's brightness are read here; the stream's actions come
// from StreamActivity.
#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>

namespace ounce
{

class StreamMenu : public brls::Box
{
  public:
    struct Actions
    {
        std::string console; // the console's name
        bool statsOn = false, micOn = false;
        std::function<void()> back;               // close the menu, back to the game
        std::function<void(bool)> stats;          // show or hide the stream stats
        std::function<bool(bool)> microphone;     // false when it couldn't start
        std::function<void(bool sleep)> disconnect;
    };

    explicit StreamMenu(Actions actions);
    ~StreamMenu() override;

    bool isTranslucent() override { return true; }

  private:
    void update(); // clock, battery, Wi-Fi: every second

    Actions actions;
    brls::Label* clock    = nullptr;
    brls::Label* battery  = nullptr;
    brls::Label* network  = nullptr;
    brls::RepeatingTimer timer;
};

// The network's name (SSID), or empty; never the rest of the profile.
std::string wifiName();

}
