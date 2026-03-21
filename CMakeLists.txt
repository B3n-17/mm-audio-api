cmake_minimum_required(VERSION 3.22)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(mm_audio_api)
set(TARGET_NAME magemods_audio_api)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# deps
add_subdirectory(thirdparty/plog)
add_subdirectory(thirdparty/miniz)
add_subdirectory(thirdparty/ogg)

# vorbis
set(OGG_LIBRARY ogg)
set(OGG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/ogg/include)
add_subdirectory(thirdparty/vorbis)

# opus
set(OP_DISABLE_HTTP ON)
set(OP_DISABLE_EXAMPLES ON)
set(OP_DISABLE_DOCS ON)
add_subdirectory(thirdparty/opus)
add_subdirectory(thirdparty/opusfile)

# Embed audio-debug.html into a generated header at configure time
file(READ "${CMAKE_SOURCE_DIR}/audio-debug.html" AUDIO_DEBUG_HTML_CONTENT)
string(REPLACE "\\" "\\\\" AUDIO_DEBUG_HTML_CONTENT "${AUDIO_DEBUG_HTML_CONTENT}")
string(REPLACE "\"" "\\\"" AUDIO_DEBUG_HTML_CONTENT "${AUDIO_DEBUG_HTML_CONTENT}")
string(REPLACE "\n" "\\n\"\n\"" AUDIO_DEBUG_HTML_CONTENT "${AUDIO_DEBUG_HTML_CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/generated/audio_debug_html.h"
    "// Auto-generated from audio-debug.html — do not edit\n"
    "#pragma once\n"
    "namespace Debug {\n"
    "inline constexpr const char* kAudioDebugHtml =\n"
    "\"${AUDIO_DEBUG_HTML_CONTENT}\";\n"
    "} // namespace Debug\n"
)

add_library(${TARGET_NAME} SHARED)

target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

target_link_libraries(${TARGET_NAME}
    PRIVATE
        miniz
        ogg
        vorbis
        vorbisfile
        opus
        opusfile
)

if (WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE ws2_32)
endif()

target_include_directories(${TARGET_NAME}
    PRIVATE
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/offline_build
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/thirdparty/utfcpp/source
        ${CMAKE_SOURCE_DIR}/thirdparty/plog/include
        ${CMAKE_SOURCE_DIR}/thirdparty/dr_libs
        ${CMAKE_SOURCE_DIR}/thirdparty/ogg/include
        ${CMAKE_SOURCE_DIR}/thirdparty/vorbis/include
        ${CMAKE_SOURCE_DIR}/thirdparty/opus/include
        ${CMAKE_SOURCE_DIR}/thirdparty/opusfile/include
)

add_subdirectory(src/extlib)

set_target_properties(${TARGET_NAME}
    PROPERTIES
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS ${TARGET_NAME} COMPONENT extlib)
