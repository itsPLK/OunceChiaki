cmake_minimum_required(VERSION 3.16)

# Build with scripts/build.sh (it runs inside the build image and passes
# -DPLATFORM_SWITCH2=ON). Borealis's toolchain picks libounce's CMake toolchain.
set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/extern/borealis/library)
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)
if (NOT PLATFORM_SWITCH2)
    message(FATAL_ERROR "OunceChiaki builds only for Switch 2: use scripts/build.sh")
endif ()

project(OunceChiaki C CXX)

# Version: the VERSION file, plus the commit for logs and the about line.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION OUNCE_VERSION LIMIT_COUNT 1)
string(STRIP "${OUNCE_VERSION}" OUNCE_VERSION)
if (NOT OUNCE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, found '${OUNCE_VERSION}'")
endif ()
execute_process(COMMAND git describe --always --dirty --exclude "*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE OUNCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT OUNCE_COMMIT)
    set(OUNCE_COMMIT "unknown")
endif ()
configure_file(src/app/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)
message(STATUS "OunceChiaki ${OUNCE_VERSION} (${OUNCE_COMMIT})")

find_library(MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)
find_library(OPUS_LIBRARY opus REQUIRED)
find_path(DEPS_INCLUDE_DIR mbedtls/aes.h REQUIRED)
find_package(Threads REQUIRED)

include(cmake/chiaki_core.cmake)

add_subdirectory(${BOREALIS_LIBRARY})

# Video shaders, compiled to a SPIR-V header.
set(VIDEO_SPIRV ${CMAKE_CURRENT_BINARY_DIR}/generated/video_spirv.hpp)
add_custom_command(OUTPUT ${VIDEO_SPIRV}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_header.py ${CMAKE_CURRENT_SOURCE_DIR}/src/video ${VIDEO_SPIRV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_header.py ${CMAKE_CURRENT_SOURCE_DIR}/src/video/video.vert ${CMAKE_CURRENT_SOURCE_DIR}/src/video/video.frag
    COMMENT "Compiling video shaders")

file(GLOB APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp src/video/*.cpp)
add_executable(${PROJECT_NAME} ${APP_SOURCES} ${VIDEO_SPIRV} ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(${PROJECT_NAME} PRIVATE src/app src/video ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(${PROJECT_NAME} PRIVATE SDL_ENABLE_OLD_NAMES VK_USE_PLATFORM_VI_NN)
target_compile_options(${PROJECT_NAME} PRIVATE -fno-omit-frame-pointer)
# src/app/crash.cpp logs abort() calls; src/video/decoder_wraps.cpp shares the
# hardware decoder's images with Vulkan and logs its setup.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map" "-Wl,--wrap=abort")
foreach (symbol viddecInitialize gsvRegisterFrame gsvGpuRegisterMemory gsvCreateNativeMemory
                viddecImagePoolCreate viddecImagePoolClose viddecOpenDecoder
                viddecDecoderInitialize viddecDecoderRegisterImage viddecDecoderStart)
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=${symbol}")
endforeach ()
target_link_libraries(${PROJECT_NAME} PRIVATE borealis chiaki-core ${OPUS_LIBRARY} Threads::Threads)

# NRO: romfs holds the Borealis resources the library needs plus ours. Fonts
# come from the system's shared font.
set(ROMFS ${CMAKE_CURRENT_BINARY_DIR}/romfs)
set(BOREALIS_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/extern/borealis/resources)
set(NRO_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg)
if (NOT EXISTS ${NRO_ICON})
    set(NRO_ICON ${DEVKITPRO}/libounce/default_icon.jpg)
endif ()
add_custom_target(${PROJECT_NAME}.nro ALL DEPENDS ${PROJECT_NAME}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ROMFS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS}/i18n/en-US ${ROMFS}/img
    COMMAND ${CMAKE_COMMAND} -E copy ${BOREALIS_RESOURCES}/i18n/en-US/hints.json ${BOREALIS_RESOURCES}/i18n/en-US/scroll.json ${ROMFS}/i18n/en-US/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BOREALIS_RESOURCES}/material ${ROMFS}/material
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BOREALIS_RESOURCES}/img/sys ${ROMFS}/img/sys
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources/romfs ${ROMFS}
    COMMAND ${NX_NACPTOOL_EXE} --create "OunceChiaki" "OunceChiaki contributors" "${OUNCE_VERSION}" ${PROJECT_NAME}.nacp
    COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${NRO_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${ROMFS}
    COMMAND ${CMAKE_COMMAND} -E echo "OunceChiaki.nro: version ${OUNCE_VERSION} (${OUNCE_COMMIT})"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    VERBATIM)
