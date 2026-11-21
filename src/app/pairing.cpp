#include "pairing.hpp"

#include "log.hpp"

#include <borealis.hpp>
#include <chiaki/base64.h>
#include <chiaki/discovery.h>
#include <chiaki/regist.h>
#include <cstring>

namespace ounce
{

std::optional<std::array<uint8_t, 8>> parseAccountId(const std::string& base64)
{
    if (base64.size() != 12 || base64.back() != '=')
        return std::nullopt;
    uint8_t out[16] = {};
    size_t size     = sizeof(out);
    if (chiaki_base64_decode(base64.c_str(), base64.size(), out, &size) != CHIAKI_ERR_SUCCESS || size != 8)
        return std::nullopt;
    std::array<uint8_t, 8> account {};
    std::memcpy(account.data(), out, 8);
    return account;
}

struct Pairing::State : std::enable_shared_from_this<Pairing::State>
{
    ChiakiRegist regist {};
    std::string address; // ChiakiRegistInfo.host points here while it runs
    Host host;
    Done done;
    bool running = false; // regist started and not yet finished
    bool alive   = true;  // the Pairing still exists
};

namespace
{

void finished(ChiakiRegistEvent* event, void* user)
{
    // Chiaki's registration thread: copy the result, finish on the UI thread.
    // The Pairing holds the state until it has joined this thread.
    std::shared_ptr<Pairing::State> state = static_cast<Pairing::State*>(user)->shared_from_this();
    std::optional<Host> paired;
    std::string error;
    if (event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
    {
        const ChiakiRegisteredHost& result = *event->registered_host;
        Host host = state->host;
        host.target = result.target;
        if (result.server_nickname[0])
            host.name.assign(result.server_nickname, strnlen(result.server_nickname, sizeof(result.server_nickname)));
        std::array<uint8_t, 6> mac {};
        std::memcpy(mac.data(), result.server_mac, mac.size());
        host.mac        = mac;
        host.registered = true;
        std::memcpy(host.registKey.data(), result.rp_regist_key, host.registKey.size());
        std::memcpy(host.rpKey.data(), result.rp_key, host.rpKey.size());
        paired = host;
    }
    else
        error = event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED
            ? "Pairing was canceled."
            : "The console refused the pairing. Check the PIN and the account ID, and that the console shows the pairing screen.";
    brls::sync([state, paired, error] {
        if (!state->running)
            return;
        chiaki_regist_fini(&state->regist);
        state->running = false;
        if (state->alive && state->done)
            state->done(paired, error);
    });
}

}

Pairing::~Pairing()
{
    if (!state)
        return;
    state->alive = false;
    if (state->running)
    {
        chiaki_regist_stop(&state->regist);
        chiaki_regist_fini(&state->regist);
        state->running = false;
    }
}

bool Pairing::running() const { return state && state->running; }

bool Pairing::start(const Host& host, const std::array<uint8_t, 8>& account, uint32_t pin, Done done)
{
    if (running())
        return false;
    state          = std::make_shared<State>();
    state->host    = host;
    state->address = host.address;
    state->done    = std::move(done);

    ChiakiRegistInfo info {};
    info.target    = host.target;
    info.host      = state->address.c_str();
    info.broadcast = false;
    std::memcpy(info.psn_account_id, account.data(), account.size());
    info.pin = pin;
    brls::Logger::info("Pairing with {} at {} ({})", host.name, host.address, host.ps5() ? "PS5" : "PS4");
    const ChiakiErrorCode err = chiaki_regist_start(&state->regist, chiakiLog(), &info, finished, state.get());
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Pairing could not start: {}", chiaki_error_string(err));
        state.reset();
        return false;
    }
    state->running = true;
    return true;
}

bool wake(const Host& host, std::string& error)
{
    const auto credential = host.wakeCredential();
    if (!credential)
    {
        error = "This console isn't paired.";
        return false;
    }
    const ChiakiErrorCode err = chiaki_discovery_wakeup(chiakiLog(), nullptr, host.address.c_str(), *credential, host.ps5());
    brls::Logger::info("Wake {} at {}: {}", host.name, host.address, chiaki_error_string(err));
    if (err != CHIAKI_ERR_SUCCESS)
    {
        error = std::string("Wake failed: ") + chiaki_error_string(err);
        return false;
    }
    return true;
}

}
