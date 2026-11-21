# chiaki-core: chiaki-ng's protocol library (third_party/chiaki-ng/lib) built
# for the LAN with mbedTLS. Upstream stays untouched; tools/patch_chiaki.py
# writes patched copies of the streaming files into the build tree.
set(CHIAKI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chiaki-ng)
set(CHIAKI_LIB ${CHIAKI_DIR}/lib)
set(CHIAKI_GEN ${CMAKE_CURRENT_BINARY_DIR}/chiaki-gen)
set(CHIAKI_PATCHED common.c thread.c takion.c congestioncontrol.c streamconnection.c frameprocessor.c videoreceiver.c)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(PROTOC protoc REQUIRED)

# Patched copies.
set(PATCHED_SOURCES)
set(PATCH_INPUTS)
foreach (name ${CHIAKI_PATCHED})
    list(APPEND PATCHED_SOURCES ${CHIAKI_GEN}/${name})
    list(APPEND PATCH_INPUTS ${CHIAKI_LIB}/src/${name})
endforeach ()
add_custom_command(OUTPUT ${PATCHED_SOURCES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_chiaki.py ${CHIAKI_LIB} ${CHIAKI_GEN}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_chiaki.py ${PATCH_INPUTS}
    COMMENT "Patching chiaki-ng streaming sources")

# Takion protobuf (nanopb).
add_custom_command(OUTPUT ${CHIAKI_GEN}/takion.pb.c ${CHIAKI_GEN}/takion.pb.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CHIAKI_GEN}
    COMMAND ${PROTOC} -o${CHIAKI_GEN}/takion.pb -I${CHIAKI_LIB}/protobuf ${CHIAKI_LIB}/protobuf/takion.proto
    COMMAND ${Python3_EXECUTABLE} ${CHIAKI_DIR}/third-party/nanopb/generator/nanopb_generator.py -D ${CHIAKI_GEN} ${CHIAKI_GEN}/takion.pb
    DEPENDS ${CHIAKI_LIB}/protobuf/takion.proto
    COMMENT "Generating Takion protobuf code")

file(WRITE ${CHIAKI_GEN}/include/chiaki/config.h.tmp
    "#define CHIAKI_LIB_ENABLE_OPUS 0\n#define CHIAKI_LIB_ENABLE_PI_DECODER 0\n#define CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0\n")
configure_file(${CHIAKI_GEN}/include/chiaki/config.h.tmp ${CHIAKI_GEN}/include/chiaki/config.h COPYONLY)

# Upstream sources, minus the patched files and what a LAN build doesn't use:
# the internet path (aia, holepunch: curl/json-c), OpenSSL, and decoders the
# app replaces (Opus is decoded by the app; video by the console's decoder).
file(GLOB CHIAKI_SOURCES ${CHIAKI_LIB}/src/*.c)
list(FILTER CHIAKI_SOURCES EXCLUDE REGEX
    "/(aia|aia_openssl|opusdecoder|opusencoder|ffmpegdecoder|pidecoder|common|thread|takion|congestioncontrol|streamconnection|frameprocessor|videoreceiver)\\.c$")

set(GF_COMPLETE ${CHIAKI_DIR}/third-party/gf-complete)
set(JERASURE ${CHIAKI_DIR}/third-party/jerasure)
set(NANOPB ${CHIAKI_DIR}/third-party/nanopb)

add_library(chiaki-core STATIC
    ${CHIAKI_SOURCES}
    ${PATCHED_SOURCES}
    ${CHIAKI_LIB}/src/remote/rudp.c
    ${CHIAKI_LIB}/src/remote/rudpsendbuffer.c
    ${CHIAKI_GEN}/takion.pb.c
    ${NANOPB}/pb_common.c ${NANOPB}/pb_encode.c ${NANOPB}/pb_decode.c
    ${GF_COMPLETE}/src/gf.c ${GF_COMPLETE}/src/gf_wgen.c ${GF_COMPLETE}/src/gf_w4.c
    ${GF_COMPLETE}/src/gf_w8.c ${GF_COMPLETE}/src/gf_w16.c ${GF_COMPLETE}/src/gf_w32.c
    ${GF_COMPLETE}/src/gf_w64.c ${GF_COMPLETE}/src/gf_w128.c ${GF_COMPLETE}/src/gf_rand.c
    ${GF_COMPLETE}/src/gf_general.c ${GF_COMPLETE}/src/gf_cpu.c
    ${JERASURE}/src/galois.c ${JERASURE}/src/jerasure.c ${JERASURE}/src/reed_sol.c
    ${JERASURE}/src/cauchy.c ${JERASURE}/src/liberation.c
    src/core/ounce_core.c
    src/core/lan_only.c)
set_target_properties(chiaki-core PROPERTIES C_STANDARD 11)
target_include_directories(chiaki-core
    PUBLIC ${CHIAKI_LIB}/include ${CHIAKI_GEN}/include src/core ${DEPS_INCLUDE_DIR}
    PRIVATE ${CHIAKI_LIB}/src ${CHIAKI_GEN} ${NANOPB} ${GF_COMPLETE}/include ${JERASURE}/include)
target_compile_definitions(chiaki-core
    PUBLIC CHIAKI_LIB_ENABLE_MBEDTLS CHIAKI_VERSION="1.10.1"
        CHIAKI_VERSION_MAJOR=1 CHIAKI_VERSION_MINOR=10 CHIAKI_VERSION_PATCH=1
    PRIVATE PB_C99_STATIC_ASSERT)
target_compile_options(chiaki-core PRIVATE -O2 -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable)
target_link_libraries(chiaki-core PUBLIC ${MBEDCRYPTO_LIBRARY} Threads::Threads)
