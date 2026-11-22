#include "log.hpp"
#include "microphone.hpp"
#include "network.hpp"
#include "settings.hpp"
#include "hosts_activity.hpp"
#include "ounce_core.h"
#include "version.h"

#include <SDL3/SDL_main.h>
#include <borealis.hpp>
#include <chiaki/common.h>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

int main(int, char*[])
{
    ounce::openLog();
    brls::Logger::info("OunceChiaki {} ({}) starting", OUNCE_VERSION, OUNCE_COMMIT);

    if (!brls::Application::init())
    {
        brls::Logger::error("Borealis init failed");
        return EXIT_FAILURE;
    }
    brls::Application::createWindow("OunceChiaki");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    // + exits from the console list only; in the stream it is Options.
    brls::Application::setGlobalQuit(false);

    const ChiakiErrorCode core = chiaki_lib_init();
    if (core != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("chiaki_lib_init failed: {}", chiaki_error_string(core));
        return EXIT_FAILURE;
    }
    brls::Logger::info("chiaki_lib_init OK");

    // Log the UDP receive buffer the system grants (Takion asks for 1 MiB).
    if (const int sock = socket(AF_INET, SOCK_DGRAM, 0); sock >= 0)
    {
        ounce_set_receive_buffer(sock, 0xA500, ounce::chiakiLog());
        close(sock);
    }

    // Microphone probe (microphone branch): 4 s of capture, levels every 0.5 s.
    {
        ounce::Microphone mic;
        if (mic.start(nullptr))
            for (int i = 0; i < 8; i++)
            {
                svcSleepThread(500000000ull);
                brls::Logger::info("Microphone probe: {:.1f} s, {} frames, peak {}", (i + 1) * 0.5, mic.frames(), mic.takePeak());
            }
        else
            brls::Logger::info("Microphone probe: no capture");
    }
    ounce::settings().load();
    ounce::network().start();
    brls::Application::pushActivity(new ounce::HostsActivity());
    while (brls::Application::mainLoop())
        ;
    ounce::network().stop();
    brls::Logger::info("Exiting");
    ounce::closeLog();
    return EXIT_SUCCESS;
}
