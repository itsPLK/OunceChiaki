#pragma once

#include <borealis.hpp>
#include <string>
#include <switch.h>

namespace ounce
{

// Keeps the network up and logs its state. Holds a nifm request for the app's
// life (as games do) and logs every change of connection type, status, signal
// and address. When the connection drops, Horizon shuts down every socket:
// sends then fail with EPIPE ("Broken pipe") until the sockets are recreated.
class Network
{
  public:
    void start();
    void stop();
    // Bumped whenever the connection or address changes; sockets opened before
    // a change may have been shut down.
    unsigned generation() const { return changes; }

  private:
    void poll();

    NifmRequest request {};
    bool requested = false;
    brls::RepeatingTimer timer;
    std::string last;
    unsigned changes = 0;
};

Network& network();

}
