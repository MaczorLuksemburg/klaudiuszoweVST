juce_add_plugin(StereoScale
    COMPANY_NAME             klaudiuszowe
    PLUGIN_MANUFACTURER_CODE Klau
    PLUGIN_CODE              KsSc
    FORMATS                  VST3
    PRODUCT_NAME             "StereoScale"
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    VST3_CATEGORIES          Fx Spatial
    COPY_PLUGIN_AFTER_BUILD  TRUE
    VST3_COPY_DIR            "${KLAUD_PLUGIN_OUTPUT_DIR}")

target_sources(StereoScale PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(StereoScale PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(StereoScale
    PRIVATE
        klaud_shared
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
