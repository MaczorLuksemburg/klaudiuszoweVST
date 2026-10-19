# Tempo matching uses the Rubber Band library (GPL v2 or later, so it can ship in an AGPLv3 plugin). Its own build
# system is Meson; the single-file build compiles the whole library from one source.
FetchContent_Declare(rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(rubberband)

add_library(waveref_rubberband STATIC ${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp)
target_include_directories(waveref_rubberband PUBLIC ${rubberband_SOURCE_DIR})
set_target_properties(waveref_rubberband PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    target_link_libraries(waveref_rubberband PUBLIC "-framework Accelerate")
endif()
if(MSVC)
    target_compile_definitions(waveref_rubberband PRIVATE NOMINMAX _USE_MATH_DEFINES)
else()
    target_compile_options(waveref_rubberband PRIVATE -w)
endif()

juce_add_plugin(WaveRef
    COMPANY_NAME             "${KLAUD_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE Klau
    PLUGIN_CODE              Wvrf
    FORMATS                  ${KLAUD_PLUGIN_FORMATS}
    PRODUCT_NAME             "WaveRef"
    DESCRIPTION              "Reference A/B switcher with a mid/side spectrum comparison"
    VERSION                  1.0.0
    BUNDLE_ID                com.makiplugins.waveref
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    VST3_CATEGORIES          Fx Analyzer
    AU_MAIN_TYPE             kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD  TRUE
    ${KLAUD_PLUGIN_COPY_ARGS})

target_sources(WaveRef PRIVATE
    Source/Parameters.cpp
    Source/TrackData.cpp
    Source/PluginProcessor.cpp
    Source/Components.cpp
    Source/PluginEditor.cpp)

# Reference files are decoded by JUCE: WAV, AIFF and FLAC always; MP3 and OGG through JUCE's own readers
# (on by default in juce_audio_formats), so they work the same on every system.
target_compile_definitions(WaveRef PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(WaveRef
    PRIVATE
        klaud_shared
        waveref_rubberband
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline tests: DSP checks plus a rendered screenshot of the editor.
if(KLAUD_BUILD_TESTS)
    add_executable(WaveRef_Tests Tests/WaveRefTests.cpp)
    target_include_directories(WaveRef_Tests PRIVATE $<TARGET_PROPERTY:WaveRef,INCLUDE_DIRECTORIES>)
    target_compile_definitions(WaveRef_Tests PRIVATE $<TARGET_PROPERTY:WaveRef,COMPILE_DEFINITIONS>)
    target_link_libraries(WaveRef_Tests PRIVATE
        WaveRef
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    # `ctest` runs the DSP checks only; run WaveRef_Tests without --no-gui locally for screenshots.
    add_test(NAME WaveRef_Tests COMMAND WaveRef_Tests --no-gui)
endif()
