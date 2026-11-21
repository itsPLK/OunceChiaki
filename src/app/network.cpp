#include "network.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <fmt/format.h>

namespace ounce
{

namespace
{

std::string ipv4(u32 address)
{
    char text[INET_ADDRSTRLEN] = "";
    in_addr in {};
    in.s_addr = address;
    inet_ntop(AF_INET, &in, text, sizeof(text));
    return text;
}

}

Network& network()
{
    static Network instance;
    return instance;
}

void Network::start()
{
    bool foreground = false;
    nifmIsAnyForegroundRequestAccepted(&foreground);
    Result rc = nifmCreateRequest(&request, true);
    if (R_SUCCEEDED(rc))
    {
        requested = true;
        rc        = nifmRequestSubmit(&request);
    }
    NifmRequestState state = NifmRequestState_Invalid;
    if (requested)
        nifmGetRequestState(&request, &state);
    brls::Logger::info("Network: request rc=0x{:x} state={} (a foreground request was {}accepted before)", rc,
        static_cast<int>(state), foreground ? "" : "not ");
    poll();
    timer.setCallback([this] { poll(); });
    timer.start(1000);
}

void Network::stop()
{
    timer.stop();
    if (requested)
    {
        nifmRequestCancel(&request);
        nifmRequestClose(&request);
        requested = false;
    }
}

void Network::poll()
{
    NifmInternetConnectionType type     = {};
    NifmInternetConnectionStatus status = {};
    u32 strength = 0, ip = 0;
    const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    nifmGetCurrentIpAddress(&ip);
    NifmRequestState state = NifmRequestState_Invalid;
    if (requested)
        nifmGetRequestState(&request, &state);
    // Signal strength alone changes often; log it only with another change.
    const std::string now = R_FAILED(rc)
        ? fmt::format("not connected (0x{:x}), request {}", rc, static_cast<int>(state))
        : fmt::format("{} status {} address {} request {}",
              type == NifmInternetConnectionType_WiFi ? "Wi-Fi" : type == NifmInternetConnectionType_Ethernet ? "Ethernet" : "?",
              static_cast<int>(status), ipv4(ip), static_cast<int>(state));
    if (now == last)
        return;
    if (!last.empty())
        changes++;
    brls::Logger::info("Network: {} (signal {}/3)", now, strength);
    last = now;
}

}
