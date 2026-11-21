#include "main_activity.hpp"

#include "version.h"

#include "log.hpp"
#include "ounce_core.h"

#include <borealis/platforms/switch/switch_input.hpp>
#include <chiaki/common.h>
#include <fmt/format.h>
#include <functional>
#include <sys/socket.h>
#include <unistd.h>

namespace ounce
{

namespace
{

constexpr brls::Time DiscoveryMs = 2500;

brls::SwitchInputManager* switchInput()
{
    return static_cast<brls::SwitchInputManager*>(brls::Application::getPlatform()->getInputManager());
}

// The largest UDP receive buffer the system gives a socket (Takion asks for
// OUNCE_RECEIVE_BUFFER).
std::string receiveBufferCheck()
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return "UDP receive buffer: no socket";
    const int size = ounce_set_receive_buffer(sock, 0xA500, chiakiLog());
    close(sock);
    if (size <= 0)
        return "UDP receive buffer: every size refused";
    return fmt::format("UDP receive buffer: {} KiB (asked {} KiB)", size / 1024, OUNCE_RECEIVE_BUFFER / 1024);
}

brls::Button* addButton(brls::Box* box, const std::string& text, std::function<void()> action)
{
    auto* button = new brls::Button();
    button->setText(text);
    button->setWidth(420);
    button->setMarginBottom(12);
    button->registerClickAction([action](brls::View*) {
        action();
        return true;
    });
    box->addView(button);
    return button;
}

brls::Label* addLabel(brls::Box* box, const std::string& text, float size)
{
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setMarginBottom(14);
    box->addView(label);
    return label;
}

}

MainActivity::MainActivity(int coreInit) : coreInit(coreInit) {}

MainActivity::~MainActivity()
{
    refreshTimer.stop();
    auto* input = switchInput();
    input->setScreenshotButtonOverrideMode(brls::ButtonOverrideMode::NONE);
#ifdef __SWITCH2__
    input->setChatButtonOverrideMode(brls::ButtonOverrideMode::NONE);
#endif
}

brls::View* MainActivity::createContentView()
{
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(60, 80, 60, 80);

    addLabel(box, "OunceChiaki " OUNCE_VERSION, 36);
    addLabel(box, fmt::format("Build {}  ·  chiaki-ng {}", OUNCE_COMMIT, CHIAKI_VERSION), 18);
    addLabel(box, coreInit == CHIAKI_ERR_SUCCESS
            ? std::string("Chiaki core: ready")
            : fmt::format("Chiaki core: init failed ({})", chiaki_error_string(static_cast<ChiakiErrorCode>(coreInit))),
        22);
    addLabel(box, receiveBufferCheck(), 22);

    findButton = new brls::Button();
    findButton->setText("Find consoles on network");
    findButton->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    findButton->setWidth(420);
    findButton->setMarginTop(10);
    findButton->setMarginBottom(20);
    findButton->registerClickAction([this](brls::View*) {
        if (discovery.start())
            discoveryEnds = brls::getCPUTimeUsec() / 1000 + DiscoveryMs;
        refresh();
        return true;
    });
    box->addView(findButton);

    addButton(box, "Search one address...", [this] {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                lastAddress = text;
                if (discovery.start(text))
                    discoveryEnds = brls::getCPUTimeUsec() / 1000 + DiscoveryMs;
                else
                    hostsLabel->setText(fmt::format("'{}' is not an IPv4 address.", text));
                refresh();
            },
            "Console address", "The PlayStation's IPv4 address, e.g. 192.168.1.20", 15, lastAddress);
    });

    hostsLabel   = addLabel(box, "Search the network, or one address.", 20);
    buttonsLabel = addLabel(box, "", 20);

    addLabel(box, "Press + to exit. The log is in /switch/OunceChiaki/logs/ounce.log", 16);
    return box;
}

void MainActivity::onContentAvailable()
{
    // Take over Capture and Chat so they reach the app; Home stays the system's.
    auto* input = switchInput();
    input->setScreenshotButtonOverrideMode(brls::ButtonOverrideMode::CUSTOM_EVENT);
#ifdef __SWITCH2__
    input->setChatButtonOverrideMode(brls::ButtonOverrideMode::CUSTOM_EVENT);
#endif
    refreshTimer.setCallback([this] { refresh(); });
    refreshTimer.start(100);
    refresh();
}

void MainActivity::refresh()
{
    auto* input = switchInput();
    const bool capture = input->isScreenshotButtonPressed();
    if (capture && !captureHeld)
    {
        capturePresses++;
        brls::Logger::info("Capture button pressed ({})", capturePresses);
    }
    captureHeld = capture;
#ifdef __SWITCH2__
    const bool chat = input->isChatButtonPressed();
    if (chat && !chatHeld)
    {
        chatPresses++;
        brls::Logger::info("Chat button pressed ({})", chatPresses);
    }
    chatHeld = chat;
#endif
    buttonsLabel->setText(fmt::format("System buttons: Chat {} ({} presses)  ·  Capture {} ({} presses)",
        chatHeld ? "HELD" : "up", chatPresses, captureHeld ? "HELD" : "up", capturePresses));

    if (discovery.running() && brls::getCPUTimeUsec() / 1000 >= discoveryEnds)
        discovery.stop();
    if (!discoveryEnds)
        return;
    const auto hosts = discovery.hosts();
    std::string text = discovery.running() ? "Searching..." : fmt::format("Found {} console(s).", hosts.size());
    for (const auto& host : hosts)
        text += fmt::format("\n{}  {}  {}  ({})", host.ps5 ? "PS5" : "PS4", host.name, host.address,
            host.standby ? "rest mode" : "ready");
    hostsLabel->setText(text);
}

}
