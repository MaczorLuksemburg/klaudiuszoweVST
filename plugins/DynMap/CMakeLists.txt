juce_add_plugin(DynMap
    COMPANY_NAME             "${KLAUD_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE Klau
    PLUGIN_CODE              Dmap
    FORMATS                  ${KLAUD_PLUGIN_FORMATS}
    PRODUCT_NAME             "DynMap"
    DESCRIPTION              "Multiband dynamic mapping"
    VERSION                  1.0.0
    BUNDLE_ID                com.makiplugins.dynmap
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    VST3_CATEGORIES          Fx Dynamics
    AU_MAIN_TYPE             kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD  TRUE
    ${KLAUD_PLUGIN_COPY_ARGS})

target_sources(DynMap PRIVATE
    Source/Parameters.cpp
    Source/Curve.cpp
    Source/Crossover.cpp
    Source/Stage.cpp
    Source/Engine.cpp
    Source/Presets.cpp
    Source/PluginProcessor.cpp
    Source/Components.cpp
    Source/CurveEditor.cpp
    Source/BandDisplay.cpp
    Source/HistoryView.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(DynMap PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(DynMap
    PRIVATE
        klaud_shared
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline tests: DSP checks, a CPU bench and a rendered screenshot of the editor.
if(KLAUD_BUILD_TESTS)
    add_executable(DynMap_Tests Tests/DynMapTests.cpp)
    target_include_directories(DynMap_Tests PRIVATE $<TARGET_PROPERTY:DynMap,INCLUDE_DIRECTORIES>)
    target_compile_definitions(DynMap_Tests PRIVATE $<TARGET_PROPERTY:DynMap,COMPILE_DEFINITIONS>)
    target_link_libraries(DynMap_Tests PRIVATE
        DynMap
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    # `ctest` runs the DSP checks only; run DynMap_Tests without --no-gui locally for screenshots.
    add_test(NAME DynMap_Tests COMMAND DynMap_Tests --no-gui)
endif()

# PluginMeasure: hosts another VST3 and runs the measurement kit through it (see Tools/PluginMeasure.cpp),
# used to match DynMap's presets to other plugins. A local calibration tool, so Windows only.
if(KLAUD_BUILD_TESTS AND WIN32)
    juce_add_console_app(PluginMeasure PRODUCT_NAME "PluginMeasure")
    target_sources(PluginMeasure PRIVATE Tools/PluginMeasure.cpp)
    target_compile_definitions(PluginMeasure PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(PluginMeasure PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
