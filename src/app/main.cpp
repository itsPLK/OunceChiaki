#include "log.hpp"
#include "main_activity.hpp"
#include "version.h"

#include <SDL3/SDL_main.h>
#include <borealis.hpp>
#include <chiaki/common.h>
#include <cstdlib>

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
    brls::Application::setGlobalQuit(true);

    const ChiakiErrorCode core = chiaki_lib_init();
    if (core != CHIAKI_ERR_SUCCESS)
        brls::Logger::error("chiaki_lib_init failed: {}", chiaki_error_string(core));
    else
        brls::Logger::info("chiaki_lib_init OK");

    brls::Application::pushActivity(new ounce::MainActivity(core));
    while (brls::Application::mainLoop())
        ;
    brls::Logger::info("Exiting");
    return EXIT_SUCCESS;
}
