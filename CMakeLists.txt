cmake_minimum_required(VERSION 3.24)

# Réglages macOS : binaire universel (Apple Silicon + Intel)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Version minimale de macOS")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures macOS")

# Windows : runtime C++ intégré au plugin (rien à installer chez l'utilisateur)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Siample VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SIAMPLE_BUILD_TESTS "Compiler les tests de Siample" OFF)

# JUCE est téléchargé automatiquement à la configuration
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# Signalsmith Stretch (licence MIT) : étirement temporel et transposition
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        1.4.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(signalsmith-stretch)

set(SIAMPLE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SIAMPLE_FORMATS AU)
endif()

juce_add_plugin(Siample
    COMPANY_NAME "Siample"
    BUNDLE_ID "com.siample.Siample"
    PLUGIN_MANUFACTURER_CODE Simp
    PLUGIN_CODE Siam
    FORMATS ${SIAMPLE_FORMATS}
    PRODUCT_NAME "Siample"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

set(SIAMPLE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Stretcher.cpp)

target_sources(Siample PRIVATE ${SIAMPLE_SOURCES})
target_include_directories(Siample PRIVATE Source)

target_compile_definitions(Siample
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_MP3AUDIOFORMAT=1)

if(MSVC)
    target_compile_options(Siample PUBLIC /utf-8)
endif()

target_link_libraries(Siample
    PRIVATE
        juce::juce_audio_utils
        signalsmith-stretch
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(SIAMPLE_BUILD_TESTS)
    juce_add_console_app(SiampleTests PRODUCT_NAME "SiampleTests")
    target_sources(SiampleTests PRIVATE tests/SiampleTests.cpp ${SIAMPLE_SOURCES})
    target_include_directories(SiampleTests PRIVATE Source)
    target_compile_definitions(SiampleTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_STANDALONE_APPLICATION=1
        JucePlugin_Name="Siample"
        JucePlugin_Build_Standalone=1
        SIAMPLE_TESTS=1)
    target_link_libraries(SiampleTests PRIVATE
        juce::juce_audio_utils
        signalsmith-stretch
        juce::juce_recommended_config_flags)
endif()
