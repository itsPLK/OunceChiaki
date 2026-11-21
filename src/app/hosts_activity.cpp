#include "hosts_activity.hpp"

#include "version.h"

#include <arpa/inet.h>
#include <borealis/platforms/switch/switch_input.hpp>
#include <chiaki/time.h>
#include <fmt/format.h>

namespace ounce
{

namespace
{

constexpr uint64_t SearchEveryMs = 3000;
// A console that hasn't answered for this long is shown as not found.
constexpr uint64_t OfflineAfterMs = 7000;

brls::SwitchInputManager* switchInput()
{
    return static_cast<brls::SwitchInputManager*>(brls::Application::getPlatform()->getInputManager());
}

uint64_t nowMs() { return chiaki_time_now_monotonic_ms(); }

bool validIpv4(const std::string& text)
{
    in_addr addr {};
    return inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

void message(const std::string& text)
{
    auto* dialog = new brls::Dialog(text);
    dialog->addButton("OK", [] {});
    dialog->open();
}

}

HostsActivity::HostsActivity() { store.load(); }

HostsActivity::~HostsActivity()
{
    timer.stop();
    discovery.close();
    auto* input = switchInput();
    input->setScreenshotButtonOverrideMode(brls::ButtonOverrideMode::NONE);
#ifdef __SWITCH2__
    input->setChatButtonOverrideMode(brls::ButtonOverrideMode::NONE);
#endif
}

brls::View* HostsActivity::createContentView()
{
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(20, 60, 20, 60);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(18);
    statusLabel->setMarginBottom(16);
    statusLabel->setText("Searching the network...");
    content->addView(statusLabel);

    list = new brls::Box(brls::Axis::COLUMN);
    content->addView(list);

    auto* add = new brls::DetailCell();
    add->setText("Add console by address");
    add->setDetailText("For a console discovery doesn't find");
    add->registerClickAction([this](brls::View*) {
        addByAddress();
        return true;
    });
    content->addView(add);

    buttonsLabel = new brls::Label();
    buttonsLabel->setFontSize(15);
    buttonsLabel->setMarginTop(24);
    content->addView(buttonsLabel);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(content);
    auto* frame = new brls::AppletFrame(scroll);
    frame->setTitle("OunceChiaki " OUNCE_VERSION);
    return frame;
}

void HostsActivity::onContentAvailable()
{
    // Temporary (milestone 1 check): Capture and Chat reach the app here.
    auto* input = switchInput();
    input->setScreenshotButtonOverrideMode(brls::ButtonOverrideMode::CUSTOM_EVENT);
#ifdef __SWITCH2__
    input->setChatButtonOverrideMode(brls::ButtonOverrideMode::CUSTOM_EVENT);
#endif
    if (!discovery.open())
        statusLabel->setText("Network search failed to start (see the log). Saved consoles still work.");
    timer.setCallback([this] { refresh(); });
    timer.start(250);
    refresh();
}

std::vector<HostsActivity::Entry> HostsActivity::entries()
{
    const auto found = discovery.hosts();
    const uint64_t now = nowMs();
    std::vector<Entry> out;
    auto status = [&](const std::string& address, const std::string& mac, Entry& entry) {
        for (const auto& d : found)
            if ((!mac.empty() && d.macHex == mac) || d.address == address)
            {
                entry.online  = now - d.seenMs < OfflineAfterMs;
                entry.standby = d.standby;
            }
    };
    for (const auto& host : store.hosts())
    {
        Entry entry;
        entry.host  = host;
        entry.saved = true;
        status(host.address, host.macHex(), entry);
        out.push_back(entry);
    }
    for (const auto& d : found)
    {
        const bool known = std::any_of(out.begin(), out.end(), [&](const Entry& e) {
            return e.host.address == d.address || (!d.macHex.empty() && e.host.macHex() == d.macHex);
        });
        if (known)
            continue;
        Entry entry;
        entry.host.name    = d.name;
        entry.host.address = d.address;
        entry.host.target  = d.target;
        entry.online       = now - d.seenMs < OfflineAfterMs;
        entry.standby      = d.standby;
        out.push_back(entry);
    }
    return out;
}

// Saved consoles follow what discovery reports: a new DHCP address, the
// console's name and, until paired, its type.
void HostsActivity::learn(const std::vector<DiscoveredHost>& found)
{
    bool changed = false;
    for (const auto& d : found)
    {
        Host* host = store.find(d.address, d.macHex);
        if (!host)
            continue;
        if (host->address != d.address && !d.macHex.empty() && host->macHex() == d.macHex)
        {
            brls::Logger::info("Hosts: {} moved from {} to {}", host->name, host->address, d.address);
            host->address = d.address;
            changed       = true;
        }
        if (host->name != d.name && !d.name.empty())
        {
            host->name = d.name;
            changed    = true;
        }
        if (!host->registered && host->target != d.target)
        {
            host->target = d.target;
            changed      = true;
        }
        if (!host->mac && d.macHex.size() == 12)
        {
            std::array<uint8_t, 6> mac {};
            if (fromHex(d.macHex, mac.data(), mac.size()))
            {
                host->mac = mac;
                changed   = true;
            }
        }
    }
    if (changed)
        store.save();
}

void HostsActivity::refresh()
{
    testButtons();
    const uint64_t now = nowMs();
    if (discovery.isOpen() && now >= nextSearchMs)
    {
        std::vector<std::string> addresses;
        for (const auto& host : store.hosts())
            addresses.push_back(host.address);
        discovery.search(addresses);
        nextSearchMs = now + SearchEveryMs;
    }
    learn(discovery.hosts());

    const auto list = entries();
    std::vector<std::string> addresses;
    for (const auto& entry : list)
        addresses.push_back(entry.host.address);
    if (addresses != shown)
        rebuild(list);
    else
        for (const auto& entry : list)
            updateCell(cells[entry.host.address], entry);

    if (pairing.running())
        statusLabel->setText("Pairing... keep the console's pairing screen open.");
    else if (list.empty())
        statusLabel->setText("Searching the network... No consoles yet.");
    else
        statusLabel->setText("A: pair or connect   X: wake from rest mode   Y: remove");
}

void HostsActivity::rebuild(const std::vector<Entry>& entries)
{
    // Keep focus on the same console across the rebuild.
    std::string focused;
    for (const auto& [address, cell] : cells)
        if (brls::Application::getCurrentFocus() == cell)
            focused = address;

    list->clearViews();
    cells.clear();
    shown.clear();
    brls::View* focus = nullptr;
    for (const auto& entry : entries)
    {
        auto* cell          = new brls::DetailCell();
        const std::string address = entry.host.address;
        cell->registerClickAction([this, address](brls::View*) {
            activate(address);
            return true;
        });
        cell->registerAction("Wake", brls::BUTTON_X, [this, address](brls::View*) {
            wakeHost(address);
            return true;
        });
        cell->registerAction("Remove", brls::BUTTON_Y, [this, address](brls::View*) {
            removeHost(address);
            return true;
        });
        updateCell(cell, entry);
        list->addView(cell);
        cells[address] = cell;
        shown.push_back(address);
        if (address == focused)
            focus = cell;
    }
    if (focus)
        brls::Application::giveFocus(focus);
}

void HostsActivity::updateCell(brls::DetailCell* cell, const Entry& entry)
{
    if (!cell)
        return;
    cell->setText(entry.host.name.empty() ? entry.host.address : entry.host.name);
    std::string state = !entry.online ? "Not found" : entry.standby ? "Rest mode" : "Ready";
    cell->setDetailText(fmt::format("{}  ·  {}  ·  {}  ·  {}", entry.host.ps5() ? "PS5" : "PS4", entry.host.address,
        state, entry.host.registered ? "Paired" : "Not paired"));
}

void HostsActivity::activate(const std::string& address)
{
    for (const auto& entry : entries())
    {
        if (entry.host.address != address)
            continue;
        if (!entry.host.registered)
            pair(entry.host);
        else if (entry.standby)
            message(fmt::format("{} is in rest mode. Press X to wake it first.", entry.host.name));
        else
            brls::Application::notify("Streaming arrives in the next build.");
        return;
    }
}

void HostsActivity::pair(Host host)
{
    if (pairing.running())
        return;
    brls::Application::getImeManager()->openForText(
        [this, host](std::string text) {
            const auto account = parseAccountId(text);
            if (!account)
            {
                message("That isn't a PSN account ID: it is 12 characters of Base64 ending in '=' "
                        "(not your online ID).");
                return;
            }
            accountId = text;
            askPin(host, *account);
        },
        "PSN account ID", "12 characters of Base64, e.g. AbCdEfGhIjk=", 12, accountId);
}

void HostsActivity::askPin(Host host, std::array<uint8_t, 8> account)
{
    brls::Application::getImeManager()->openForNumber(
        [this, host, account](long pin) {
            if (pin < 0 || pin > 99999999)
            {
                message("The PIN is 8 digits.");
                return;
            }
            const bool started = pairing.start(host, account, static_cast<uint32_t>(pin),
                [this](std::optional<Host> paired, const std::string& error) {
                    if (!paired)
                    {
                        message(error);
                        return;
                    }
                    store.put(*paired);
                    store.save();
                    brls::Logger::info("Paired with {} at {}", paired->name, paired->address);
                    brls::Application::notify(fmt::format("Paired with {}", paired->name));
                    refresh();
                });
            if (!started)
                message("Pairing could not start (see the log).");
            refresh();
        },
        fmt::format("Pairing PIN for {}", host.name),
        host.ps5() ? "PS5: Settings > System > Remote Play > Pair Device"
                   : "PS4: Settings > Remote Play Connection Settings > Add Device",
        8);
}

void HostsActivity::wakeHost(const std::string& address)
{
    for (const auto& host : store.hosts())
        if (host.address == address)
        {
            std::string error;
            if (wake(host, error))
                brls::Application::notify(fmt::format("Waking {}...", host.name));
            else
                message(error);
            return;
        }
    message("Pair this console first: waking needs its pairing key.");
}

void HostsActivity::removeHost(const std::string& address)
{
    Host* host = store.find(address, "");
    if (!host)
    {
        brls::Application::notify("Only saved consoles can be removed.");
        return;
    }
    auto* dialog = new brls::Dialog(fmt::format("Remove {}{}?", host->name,
        host->registered ? " and its pairing" : ""));
    dialog->addButton("Cancel", [] {});
    dialog->addButton("Remove", [this, address] {
        store.remove(address);
        store.save();
        refresh();
    });
    dialog->open();
}

void HostsActivity::addByAddress()
{
    brls::Application::getImeManager()->openForText(
        [this](std::string text) {
            if (!validIpv4(text))
            {
                message(fmt::format("'{}' isn't an IPv4 address.", text));
                return;
            }
            lastAddress = text;
            if (!store.find(text, ""))
            {
                // Saved right away so every search asks it directly; discovery
                // fills in the name and type when it answers.
                Host host;
                host.name    = text;
                host.address = text;
                store.put(host);
                store.save();
            }
            nextSearchMs = 0;
            refresh();
        },
        "Console address", "The PlayStation's IPv4 address, e.g. 192.168.1.20", 15, lastAddress);
}

// Temporary (milestone 1 check): shows that Chat and Capture reach the app.
void HostsActivity::testButtons()
{
    auto* input = switchInput();
    const bool capture = input->isScreenshotButtonPressed();
    if (capture && !captureHeld)
        brls::Logger::info("Capture button pressed ({})", ++capturePresses);
    captureHeld = capture;
#ifdef __SWITCH2__
    const bool chat = input->isChatButtonPressed();
    if (chat && !chatHeld)
        brls::Logger::info("Chat button pressed ({})", ++chatPresses);
    chatHeld = chat;
#endif
    buttonsLabel->setText(fmt::format("Button test: Chat {} ({})  ·  Capture {} ({})  ·  Build {}",
        chatHeld ? "held" : "up", chatPresses, captureHeld ? "held" : "up", capturePresses, OUNCE_COMMIT));
}

}
