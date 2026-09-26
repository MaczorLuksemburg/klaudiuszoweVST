cmake_minimum_required(VERSION 3.22)

project(klaudiuszoweVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendor name shown in DAWs and in each plugin header.
set(KLAUD_COMPANY_NAME "Maki plugins")

# Formats every plugin is built in. JUCE skips the ones the current OS can't build
# (AU is macOS only), so Windows and Linux get VST3, macOS gets VST3 + AU for Logic.
set(KLAUD_PLUGIN_FORMATS VST3 AU)

# On Windows, finished plugins are copied here after every build.
# macOS and Linux use the system plugin folders (~/Library/Audio/Plug-Ins, ~/.vst3).
set(KLAUD_PLUGIN_OUTPUT_DIR "C:/gen plugins" CACHE PATH "Where built plugins are copied on Windows")

if(WIN32)
    set(KLAUD_PLUGIN_COPY_ARGS VST3_COPY_DIR "${KLAUD_PLUGIN_OUTPUT_DIR}")
else()
    set(KLAUD_PLUGIN_COPY_ARGS)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.2
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# Look and feel shared by all plugins.
add_library(klaud_shared INTERFACE)
target_include_directories(klaud_shared INTERFACE shared)

option(KLAUD_BUILD_TESTS "Build the offline plugin test programs" ON)

if(KLAUD_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(plugins/StereoScale)
add_subdirectory(plugins/MSC)
add_subdirectory(plugins/FloorMatch)
