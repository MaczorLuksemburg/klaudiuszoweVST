# LavAlign: standalone app that phase-aligns lavalier mics to the boom (offline, file in / file out).
set(LAVALIGN_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Align.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/WavFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Platform.cpp)

set(LAVALIGN_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_VERSION_STRING="${KLAUD_VERSION}")

juce_add_gui_app(LavAlign
    COMPANY_NAME  "${KLAUD_COMPANY_NAME}"
    PRODUCT_NAME  "LavAlign"
    VERSION       ${KLAUD_VERSION}
    BUNDLE_ID     com.makiplugins.lavalign
    DOCUMENT_EXTENSIONS wav bwf aif aiff flac)

target_sources(LavAlign PRIVATE
    ${LAVALIGN_CORE_SOURCES}
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/Preview.cpp)

target_compile_definitions(LavAlign PRIVATE ${LAVALIGN_DEFINITIONS})

# One self-contained .exe: the C++ runtime is linked in, so it runs on PCs without the
# Visual C++ redistributable (it is handed around as a single file).
set_property(TARGET LavAlign PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(LavAlign
    PRIVATE
        klaud_shared
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# On Windows the app goes next to the plugins in C:\gen plugins, like every other build.
if(WIN32)
    add_custom_command(TARGET LavAlign POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${KLAUD_PLUGIN_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:LavAlign>" "${KLAUD_PLUGIN_OUTPUT_DIR}/LavAlign.exe"
        VERBATIM)
endif()

# Offline tests and command-line tools (see Tests/LavAlignTests.cpp).
if(KLAUD_BUILD_TESTS)
    juce_add_console_app(LavAlign_Tests PRODUCT_NAME "LavAlign_Tests")
    target_sources(LavAlign_Tests PRIVATE
        ${LAVALIGN_CORE_SOURCES}
        Source/MainComponent.cpp
        Source/Preview.cpp
        Tests/LavAlignTests.cpp)
    target_compile_definitions(LavAlign_Tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        LAVALIGN_TESTS_WITH_GUI=1
        JUCE_APPLICATION_VERSION_STRING="${KLAUD_VERSION}")
    target_link_libraries(LavAlign_Tests PRIVATE
        klaud_shared
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    add_test(NAME LavAlign_Tests COMMAND LavAlign_Tests)
endif()
