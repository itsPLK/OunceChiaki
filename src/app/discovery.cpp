#include "discovery.hpp"

#include "log.hpp"

#include <arpa/inet.h>
#include <borealis.hpp>
#include <chiaki/time.h>
#include <netinet/in.h>
#include <switch.h>

namespace ounce
{

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

std::string lower(const char* text)
{
    std::string out = text ? text : "";
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

}

Discovery::~Discovery() { close(); }

bool Discovery::open()
{
    if (active)
        return true;
    ChiakiErrorCode err = chiaki_discovery_init(&discovery, chiakiLog(), AF_INET);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Discovery init failed: {}", chiaki_error_string(err));
        return false;
    }
    err = chiaki_discovery_thread_start(&thread, &discovery, onHost, this);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Discovery thread failed: {}", chiaki_error_string(err));
        chiaki_discovery_fini(&discovery);
        return false;
    }
    active = true;

    u32 ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    const Result rc = nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &dns1, &dns2);
    if (R_SUCCEEDED(rc) && ip && mask)
    {
        subnet = ip | ~mask;
        brls::Logger::info("Discovery: this Switch is {} mask {}", ipv4(ip), ipv4(mask));
    }
    else
        brls::Logger::warning("Discovery: no IP configuration (0x{:x}); only the limited broadcast", rc);
    return true;
}

void Discovery::close()
{
    if (!active)
        return;
    chiaki_discovery_thread_stop(&thread);
    chiaki_discovery_fini(&discovery);
    active = false;
}

// Returns false when a send failed.
bool Discovery::sendSearch(uint32_t address)
{
    bool sent = true;
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
        if (err != CHIAKI_ERR_SUCCESS)
        {
            brls::Logger::warning("Discovery: {} search to {} failed: {}", ps5 ? "PS5" : "PS4", ipv4(address), chiaki_error_string(err));
            sent = false;
        }
    }
    return sent;
}

void Discovery::search(const std::vector<std::string>& addresses)
{
    if (!active)
        return;
    if (!sendSearch(htonl(INADDR_BROADCAST)))
    {
        // The system shut the socket down (the connection dropped): sends fail
        // with EPIPE until it is recreated. Reopen and search on the next round.
        brls::Logger::warning("Discovery: socket shut down, reopening");
        close();
        open();
        return;
    }
    if (subnet)
        sendSearch(subnet);
    for (const auto& address : addresses)
    {
        in_addr in {};
        if (inet_pton(AF_INET, address.c_str(), &in) == 1)
            sendSearch(in.s_addr);
    }
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
    entry.macHex  = lower(host->host_id);
    entry.standby = host->state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY;
    entry.seenMs  = chiaki_time_now_monotonic_ms();
    // PS5 firmware before 8.05 falls through upstream's PS4 version thresholds.
    entry.target = chiaki_discovery_host_is_ps5(host) ? CHIAKI_TARGET_PS5_1 : chiaki_discovery_host_system_version_target(host);
    std::lock_guard lock(self->mutex);
    for (auto& known : self->found)
        if (known.address == entry.address)
        {
            if (known.standby != entry.standby)
                brls::Logger::info("Discovery: {} is now {}", entry.name, entry.standby ? "in rest mode" : "ready");
            known = entry;
            return;
        }
    brls::Logger::info("Discovered {} {} at {} ({})", chiaki_target_is_ps5(entry.target) ? "PS5" : "PS4", entry.name,
        entry.address, entry.standby ? "rest mode" : "ready");
    self->found.push_back(entry);
}

}
