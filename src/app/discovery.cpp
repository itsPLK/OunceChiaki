#include "discovery.hpp"

#include "log.hpp"

#include <arpa/inet.h>
#include <borealis.hpp>
#include <netinet/in.h>
#include <switch.h>

namespace ounce
{

Discovery::~Discovery() { stop(); }

namespace
{

std::string ipv4(uint32_t address) // network byte order
{
    char text[INET_ADDRSTRLEN] = "";
    in_addr in {};
    in.s_addr = address;
    inet_ntop(AF_INET, &in, text, sizeof(text));
    return text;
}

}

// Sends PS4 and PS5 search packets to one address (network byte order).
void Discovery::sendSearch(uint32_t address)
{
    sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = address;
    for (bool ps5 : { false, true })
    {
        addr.sin_port = htons(ps5 ? CHIAKI_DISCOVERY_PORT_PS5 : CHIAKI_DISCOVERY_PORT_PS4);
        ChiakiDiscoveryPacket packet {};
        packet.cmd              = CHIAKI_DISCOVERY_CMD_SRCH;
        packet.protocol_version = const_cast<char*>(ps5 ? CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5 : CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4);
        const ChiakiErrorCode err = chiaki_discovery_send(&discovery, &packet, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        brls::Logger::info("Discovery: {} search to {}: {}", ps5 ? "PS5" : "PS4", ipv4(address), chiaki_error_string(err));
    }
}

bool Discovery::start(const std::string& address)
{
    in_addr unicast {};
    if (!address.empty() && inet_pton(AF_INET, address.c_str(), &unicast) != 1)
    {
        brls::Logger::error("Discovery: '{}' is not an IPv4 address", address);
        return false;
    }
    stop();
    {
        std::lock_guard lock(mutex);
        found.clear();
    }
    brls::Logger::info("Discovery: init");
    ChiakiErrorCode err = chiaki_discovery_init(&discovery, chiakiLog(), AF_INET);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Discovery init failed: {}", chiaki_error_string(err));
        return false;
    }
    brls::Logger::info("Discovery: starting thread");
    err = chiaki_discovery_thread_start(&thread, &discovery, onHost, this);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Discovery thread failed: {}", chiaki_error_string(err));
        chiaki_discovery_fini(&discovery);
        return false;
    }
    active = true;

    if (!address.empty())
        sendSearch(unicast.s_addr);
    else
    {
        // The limited broadcast, and the subnet's directed broadcast in case the
        // network stack doesn't send the limited one onto the LAN.
        sendSearch(htonl(INADDR_BROADCAST));
        u32 ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
        const Result rc = nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &dns1, &dns2);
        if (R_SUCCEEDED(rc) && ip && mask)
        {
            brls::Logger::info("Discovery: this Switch is {} mask {}", ipv4(ip), ipv4(mask));
            sendSearch(ip | ~mask);
        }
        else
            brls::Logger::warning("Discovery: no IP configuration (0x{:x}), only the limited broadcast was sent", rc);
    }
    brls::Logger::info("Discovery started");
    return true;
}

void Discovery::stop()
{
    if (!active)
        return;
    chiaki_discovery_thread_stop(&thread);
    chiaki_discovery_fini(&discovery);
    active = false;
    brls::Logger::info("Discovery stopped");
}

std::vector<DiscoveredHost> Discovery::hosts()
{
    std::lock_guard lock(mutex);
    return found;
}

void Discovery::onHost(ChiakiDiscoveryHost* host, void* user)
{
    auto* self = static_cast<Discovery*>(user);
    if (!host->host_addr)
        return;
    DiscoveredHost entry;
    entry.address = host->host_addr;
    entry.name    = host->host_name ? host->host_name : "PlayStation";
    entry.ps5     = chiaki_discovery_host_is_ps5(host);
    entry.standby = host->state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY;
    std::lock_guard lock(self->mutex);
    for (auto& known : self->found)
        if (known.address == entry.address)
        {
            known = entry;
            return;
        }
    brls::Logger::info("Discovered {} {} at {} ({})", entry.ps5 ? "PS5" : "PS4", entry.name, entry.address,
        entry.standby ? "standby" : "ready");
    self->found.push_back(entry);
}

}
