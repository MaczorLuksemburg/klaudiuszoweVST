juce_add_plugin(MSC
    COMPANY_NAME             "${KLAUD_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE Klau
    PLUGIN_CODE              Msc1
    FORMATS                  ${KLAUD_PLUGIN_FORMATS}
    PRODUCT_NAME             "MSC"
    DESCRIPTION              "Multistage stereo control"
    VERSION                  1.0.0
    BUNDLE_ID                com.makiplugins.msc
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    VST3_CATEGORIES          Fx Spatial
    AU_MAIN_TYPE             kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD  TRUE
    ${KLAUD_PLUGIN_COPY_ARGS})

target_sources(MSC PRIVATE
    Source/Parameters.cpp
    Source/Presets.cpp
    Source/PluginProcessor.cpp
    Source/Components.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(MSC PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(MSC
    PRIVATE
        klaud_shared
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline tests: DSP checks plus a rendered screenshot of the editor.
if(KLAUD_BUILD_TESTS)
    add_executable(MSC_Tests Tests/MscTests.cpp)
    target_include_directories(MSC_Tests PRIVATE $<TARGET_PROPERTY:MSC,INCLUDE_DIRECTORIES>)
    target_compile_definitions(MSC_Tests PRIVATE $<TARGET_PROPERTY:MSC,COMPILE_DEFINITIONS>)
    target_link_libraries(MSC_Tests PRIVATE
        MSC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    # `ctest` runs the DSP checks only; run MSC_Tests without --no-gui locally for screenshots.
    add_test(NAME MSC_Tests COMMAND MSC_Tests --no-gui)
endif()
