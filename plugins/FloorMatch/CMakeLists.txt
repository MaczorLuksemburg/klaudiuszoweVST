juce_add_plugin(FloorMatch
    COMPANY_NAME             "${KLAUD_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE Klau
    PLUGIN_CODE              FlMt
    FORMATS                  ${KLAUD_PLUGIN_FORMATS}
    PRODUCT_NAME             "FloorMatch"
    DESCRIPTION              "Dialogue noise floor matcher"
    VERSION                  1.0.0
    BUNDLE_ID                com.makiplugins.floormatch
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    VST3_CATEGORIES          Fx Restoration
    AU_MAIN_TYPE             kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD  TRUE
    ${KLAUD_PLUGIN_COPY_ARGS})

target_sources(FloorMatch PRIVATE
    Source/Engine.cpp
    Source/Parameters.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(FloorMatch PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(FloorMatch
    PRIVATE
        klaud_shared
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline tests: DSP measurements on synthetic takes, a WAV render mode for real material,
# and a rendered screenshot of the editor.
if(KLAUD_BUILD_TESTS)
    add_executable(FloorMatch_Tests Tests/FloorMatchTests.cpp)
    target_include_directories(FloorMatch_Tests PRIVATE $<TARGET_PROPERTY:FloorMatch,INCLUDE_DIRECTORIES>)
    target_compile_definitions(FloorMatch_Tests PRIVATE $<TARGET_PROPERTY:FloorMatch,COMPILE_DEFINITIONS>)
    target_link_libraries(FloorMatch_Tests PRIVATE
        FloorMatch
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    add_test(NAME FloorMatch_Tests COMMAND FloorMatch_Tests --no-gui)
endif()
