#include "hosts_activity.hpp"

#include "settings.hpp"
#include "settings_activity.hpp"
#include "stream_activity.hpp"

#include "version.h"

#include <arpa/inet.h>
#include <chiaki/time.h>
#include <fmt/format.h>

namespace ounce
{

namespace
{

constexpr uint64_t SearchEveryMs = 3000;
// Waking: search more often, resend the wake, and give up after a while.
constexpr uint64_t WakeSearchMs = 1000, WakeResendMs = 5000, WakeTimeoutMs = 45000;
// Once it answers as ready, Remote Play may need a moment more.
constexpr uint64_t WakeSettleMs = 1500;
// A console that hasn't answered for this long is shown as not found.
constexpr uint64_t OfflineAfterMs = 7000;

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

    newHeader = new brls::Header();
    newHeader->setTitle("Add a console");
    newHeader->setMarginTop(20);
    content->addView(newHeader);
    newList = new brls::Box(brls::Axis::COLUMN);
    content->addView(newList);

    auto* add = new brls::DetailCell();
    add->setText("Add console by address");
    add->setDetailText("For a console discovery doesn't find");
    add->registerClickAction([this](brls::View*) {
        addByAddress();
        return true;
    });
    content->addView(add);

    settingsCell = new brls::DetailCell();
    settingsCell->setText("Settings");
    settingsCell->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });
    content->addView(settingsCell);


    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(content);
    auto* frame = new brls::AppletFrame(scroll);
    frame->setTitle("OunceChiaki " OUNCE_VERSION);
    // Plus exits only here: in the stream it is the Options button.
    frame->registerAction("Exit", brls::BUTTON_START, [](brls::View*) {
        brls::Application::quit();
        return true;
    });
    frame->registerAction("Settings", brls::BUTTON_BACK, [](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });
    return frame;
}

void HostsActivity::onContentAvailable()
{
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
    const Settings& s = settings();
    settingsCell->setDetailText(fmt::format("{}p  ·  {} fps  ·  {}{}", s.height, s.fps,
        s.bitrate ? "" : "Auto (", bitrateLabel(s.effectiveBitrate()) + (s.bitrate ? "" : ")")));
    const uint64_t now = nowMs();
    if (discovery.isOpen() && now >= nextSearchMs)
    {
        std::vector<std::string> addresses;
        for (const auto& host : store.hosts())
            addresses.push_back(host.address);
        discovery.search(addresses);
        nextSearchMs = now + (waking ? WakeSearchMs : SearchEveryMs);
    }
    learn(discovery.hosts());

    const auto list = entries();
    checkWake(list);
    std::vector<std::string> addresses; // which section each is in counts too
    for (const auto& entry : list)
        addresses.push_back((entry.host.registered ? "+" : "-") + entry.host.address);
    if (addresses != shown)
        rebuild(list);
    else
        for (const auto& entry : list)
            updateCell(cells[entry.host.address], entry);

    const bool anyPaired = std::any_of(list.begin(), list.end(), [](const Entry& e) { return e.host.registered; });
    if (pairing.running())
        statusLabel->setText("Pairing... keep the console's pairing screen open.");
    else if (!anyPaired)
        statusLabel->setText("No paired consoles yet: pair one under \"Add a console\".");
    else
        statusLabel->setText("A: stream (wakes the console from rest mode)   Y: remove");
}

void HostsActivity::rebuild(const std::vector<Entry>& entries)
{
    // Keep focus on the same console across the rebuild.
    std::string focused;
    for (const auto& [address, cell] : cells)
        if (brls::Application::getCurrentFocus() == cell)
            focused = address;

    list->clearViews();
    newList->clearViews();
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
        if (entry.saved)
            cell->registerAction("Remove", brls::BUTTON_Y, [this, address](brls::View*) {
                removeHost(address);
                return true;
            });
        updateCell(cell, entry);
        (entry.host.registered ? list : newList)->addView(cell);
        cells[address] = cell;
        shown.push_back((entry.host.registered ? "+" : "-") + address);
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
    if (!entry.host.registered)
        state += entry.online ? "  ·  A: pair" : "";
    cell->setDetailText(fmt::format("{}  ·  {}  ·  {}", entry.host.ps5() ? "PS5" : "PS4", entry.host.address, state));
}

void HostsActivity::activate(const std::string& address)
{
    for (const auto& entry : entries())
    {
        if (entry.host.address != address)
            continue;
        if (!entry.host.registered && !entry.online && !entry.standby)
        {
            // Added by address and not answered yet: discovery hasn't said
            // whether it's a PS4 or a PS5, and pairing needs to know.
            auto* dialog = new brls::Dialog(fmt::format("{} hasn't answered a search yet. Which console is it?", entry.host.name));
            const Host host = entry.host;
            dialog->addButton("PS5", [this, host] {
                Host chosen   = host;
                chosen.target = CHIAKI_TARGET_PS5_1;
                pair(chosen);
            });
            dialog->addButton("PS4", [this, host] {
                Host chosen   = host;
                chosen.target = CHIAKI_TARGET_PS4_10; // system software 8 or later
                pair(chosen);
            });
            dialog->addButton("Cancel", [] {});
            dialog->open();
        }
        else if (!entry.host.registered)
            pair(entry.host);
        else
        {
            const Host* saved = store.find(entry.host.address, entry.host.macHex());
            connect(saved ? *saved : entry.host, entry.online && !entry.standby);
        }
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

// Streams a paired console. One in rest mode, or not answering (it may be
// asleep with the network off for a moment), is woken first: the stream starts
// once it answers as ready.
void HostsActivity::connect(const Host& host, bool ready)
{
    if (waking)
        return;
    if (ready)
    {
        brls::Application::pushActivity(new StreamActivity(host, settings().stream()));
        return;
    }
    std::string error;
    if (!wake(host, error))
    {
        message(error);
        return;
    }
    brls::Logger::info("Hosts: waking {} to stream", host.name);
    const uint64_t now = nowMs();
    waking.emplace(Waking { host, now, now, 0, nullptr });
    nextSearchMs = 0;
    auto* dialog = new brls::Dialog(fmt::format("Waking {}...", host.name));
    dialog->addButton("Cancel", [this] {
        brls::Logger::info("Hosts: waking cancelled");
        waking.reset(); // the dialog closes itself
    });
    dialog->setCancelable(false);
    waking->dialog = dialog;
    dialog->open();
}

void HostsActivity::checkWake(const std::vector<Entry>& list)
{
    if (!waking)
        return;
    const uint64_t now = nowMs();
    bool ready         = false;
    for (const auto& entry : list)
        if (entry.host.registered && (entry.host.address == waking->host.address
                || (!waking->host.macHex().empty() && entry.host.macHex() == waking->host.macHex())))
        {
            ready = entry.online && !entry.standby;
            if (ready)
                waking->host.address = entry.host.address; // it may have a new address
        }
    if (ready && !waking->readyMs)
    {
        brls::Logger::info("Hosts: {} is awake after {} ms", waking->host.name, now - waking->startMs);
        waking->readyMs = now;
    }
    if (waking->readyMs && now - waking->readyMs >= WakeSettleMs)
    {
        Host host = waking->host;
        if (const Host* saved = store.find(host.address, host.macHex()))
            host = *saved;
        auto* dialog = waking->dialog;
        waking.reset();
        auto start = [host] { brls::Application::pushActivity(new StreamActivity(host, settings().stream())); };
        if (dialog)
            dialog->close(start); // after the dialog is gone
        else
            start();
        return;
    }
    if (now - waking->startMs >= WakeTimeoutMs)
    {
        brls::Logger::info("Hosts: {} didn't wake", waking->host.name);
        const std::string text = fmt::format("{} didn't wake up. Is it on and connected to the network?", waking->host.name);
        auto* dialog = waking->dialog;
        waking.reset();
        if (dialog)
            dialog->close([text] { message(text); });
        else
            message(text);
        return;
    }
    if (!waking->readyMs && now - waking->sentMs >= WakeResendMs)
    {
        std::string error;
        wake(waking->host, error);
        waking->sentMs = now;
    }
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


}
