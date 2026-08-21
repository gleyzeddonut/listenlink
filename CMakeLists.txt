cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(ListenLink VERSION 0.4.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# libopus, statically linked. Intrinsics are disabled so a single compile pass
# works for the arm64+x86_64 universal build; the encoder is still far faster
# than real time without them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)

juce_add_plugin(ListenLink
    COMPANY_NAME "DIY"
    BUNDLE_ID com.diy.listenlink
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Diyz
    PLUGIN_CODE Lsnl
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "ListenLink"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "ListenLink standalone needs audio input access.")

juce_generate_juce_header(ListenLink)

target_sources(ListenLink PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/StreamServer.cpp)

target_compile_definitions(ListenLink PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ListenLink
    PRIVATE
        juce::juce_audio_utils
        opus
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Developer ID signing (hardened runtime + timestamp, as notarization requires).
# Signs both the build artefact and the copy installed to ~/Library/Audio/Plug-Ins.
# Set -DLL_CODESIGN_ID="" to build unsigned.
set(LL_CODESIGN_ID "Developer ID Application: Daniel Gleyzer (K7VM2MP885)"
    CACHE STRING "codesign identity")

if(NOT LL_CODESIGN_ID STREQUAL "")
    add_custom_command(TARGET ListenLink_AU POST_BUILD
        COMMAND codesign --force --options runtime --timestamp
                --sign "${LL_CODESIGN_ID}" "$<TARGET_BUNDLE_DIR:ListenLink_AU>"
        COMMAND codesign --force --options runtime --timestamp
                --sign "${LL_CODESIGN_ID}"
                "$ENV{HOME}/Library/Audio/Plug-Ins/Components/ListenLink.component"
        VERBATIM)
    add_custom_command(TARGET ListenLink_VST3 POST_BUILD
        COMMAND codesign --force --options runtime --timestamp
                --sign "${LL_CODESIGN_ID}" "$<TARGET_BUNDLE_DIR:ListenLink_VST3>"
        COMMAND codesign --force --options runtime --timestamp
                --sign "${LL_CODESIGN_ID}"
                "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/ListenLink.vst3"
        VERBATIM)
    add_custom_command(TARGET ListenLink_Standalone POST_BUILD
        COMMAND codesign --force --options runtime --timestamp
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/standalone.entitlements"
                --sign "${LL_CODESIGN_ID}" "$<TARGET_BUNDLE_DIR:ListenLink_Standalone>"
        VERBATIM)
endif()
