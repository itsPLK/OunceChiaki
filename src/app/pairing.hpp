#pragma once

#include "hosts.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ounce
{

// The 8-byte PSN account ID from its 12-character Base64 form.
std::optional<std::array<uint8_t, 8>> parseAccountId(const std::string& base64);

// Pairs ("registers") with a console using the PIN from its Remote Play
// settings. The result arrives on the UI thread.
class Pairing
{
  public:
    using Done = std::function<void(std::optional<Host> paired, const std::string& error)>;

    ~Pairing();
    bool start(const Host& host, const std::array<uint8_t, 8>& account, uint32_t pin, Done done);
    bool running() const;

    struct State;

  private:
    std::shared_ptr<State> state;
};

// Sends the wake-up packet to a paired console in rest mode.
bool wake(const Host& host, std::string& error);

}
